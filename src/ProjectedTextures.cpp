#include "ProjectedTextures.hpp"

#include "TextureColor.hpp"

#include "PCH.h"

#include <d3d11.h>

#include <DirectXTex.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>

using namespace XPMF;

void ProjectedTextures::install()
{
    // The first of BSLightingShader's vtables is the BSShader one SetupGeometry lives in
    REL::Relocation<std::uintptr_t> vtable {RE::VTABLE_BSLightingShader[0]};
    SetupGeometryHook::s_func = vtable.write_vfunc(SetupGeometryHook::SLOT, SetupGeometryHook::thunk);
    spdlog::info("Lighting shader SetupGeometry hook installed");
}

auto ProjectedTextures::add(const Paths& paths,
                            const Tiling& tiling) -> std::optional<Added>
{
    if (s_active.load(std::memory_order_acquire)) {
        spdlog::error("Projected textures were asked for after the draw hook went live; nothing was loaded");
        return std::nullopt; // the sets must not change under a running hook
    }
    if (paths.namesNothing() && tiling.namesNothing()) {
        return Added {}; // the game's textures throughout: nothing to tell draws apart for
    }
    for (std::size_t set = 0; set < s_count; ++set) {
        if (s_sets.at(set).paths == paths && s_sets.at(set).tiling == tiling) {
            return Added {.set = set, .ownNoise = s_sets.at(set).noise != nullptr};
        }
    }
    const auto orNone = [](const std::string& path) -> const char* { return path.empty() ? "(none)" : path.c_str(); };
    const auto orGameRatio = [](const std::optional<float>& worldUnits) -> std::string {
        return worldUnits.has_value() ? std::format("{:g}", *worldUnits) : "(the game's ratio to the noise)";
    };
    if (s_count >= MAX_SETS) {
        spdlog::error("More than {} different sets of projected textures; the set of diffuse {}, normal {}, noise {}, "
                      "detail normal {}, material UV scale {}, detail UV scale {} gets none",
                      MAX_SETS,
                      orNone(paths.diffuse),
                      orNone(paths.normal),
                      orNone(paths.noise),
                      orNone(paths.detailNormal),
                      orGameRatio(tiling.material),
                      orGameRatio(tiling.detail));
        return std::nullopt;
    }

    Set fresh {.paths = paths, .tiling = tiling};
    if (!paths.diffuse.empty()) {
        fresh.diffuse = paths.pbr ? holdForPbr(paths.diffuse) : hold(paths.diffuse);
        if (fresh.diffuse == nullptr) {
            return std::nullopt;
        }
    }
    const auto holdOrKeep = [](const std::string& path, const char* what) -> TexturePointer* {
        if (path.empty()) {
            return nullptr;
        }
        auto* const holder = hold(path);
        if (holder == nullptr) {
            spdlog::warn("...so the game's projected {} stays", what);
        }
        return holder;
    };
    fresh.normal = holdOrKeep(paths.normal, "normal");
    fresh.noise = holdOrKeep(paths.noise, "noise");
    fresh.detailNormal = holdOrKeep(paths.detailNormal, "detail normal");
    if (fresh.diffuse == nullptr && fresh.normal == nullptr && fresh.noise == nullptr && fresh.detailNormal == nullptr
        && tiling.namesNothing()) {
        return Added {}; // nothing loaded that would be swapped in, and nothing to tile
    }

    const std::size_t set = s_count;
    s_sets.at(set) = std::move(fresh);
    ++s_count;
    const auto orGame = [](const std::string& path, const TexturePointer* holder) -> const char* {
        return holder != nullptr ? path.c_str() : "(the game's)";
    };
    const Set& added = s_sets.at(set);
    spdlog::info("Projected texture set {}: diffuse {}{}, normal {}, noise {}, detail normal {}; material UV scale {}, "
                 "detail UV scale {}",
                 set + 1,
                 orGame(paths.diffuse, added.diffuse),
                 paths.pbr ? " (for PBR)" : "",
                 orGame(paths.normal, added.normal),
                 orGame(paths.noise, added.noise),
                 orGame(paths.detailNormal, added.detailNormal),
                 orGameRatio(tiling.material),
                 orGameRatio(tiling.detail));
    return Added {.set = set, .ownNoise = added.noise != nullptr};
}

auto ProjectedTextures::tag(std::size_t set,
                            const RE::NiColor& color) -> RE::NiColor
{
    // Bit patterns in the lowest mantissa bits: the value hardly moves (see the class), the
    // pattern is exact, and both code paths that copy the color into a shape copy it bit for bit
    const auto mark = [](float channel, std::uint32_t mask, std::uint32_t bits) -> float {
        return std::bit_cast<float>((std::bit_cast<std::uint32_t>(channel) & ~mask) | bits);
    };
    return {mark(color.red, MARK_MASK, RED_MARK),
            mark(color.green, MARK_MASK, GREEN_MARK),
            mark(color.blue, SET_MASK, static_cast<std::uint32_t>(set) + 1U)};
}

void ProjectedTextures::activate()
{
    if (s_count == 0 || s_active.load(std::memory_order_acquire)) {
        return;
    }
    // The settings the shader's tiling comes from, where a set has a tiling of its own to put there
    const auto sets = std::span(s_sets).first(s_count);
    if (std::ranges::any_of(sets, [](const Set& set) -> bool { return set.tiling.material.has_value(); })) {
        s_diffuseTiling = floatSetting(DIFFUSE_TILING_SETTING);
    }
    if (std::ranges::any_of(sets, [](const Set& set) -> bool { return set.tiling.detail.has_value(); })) {
        s_detailTiling = floatSetting(DETAIL_TILING_SETTING);
    }
    s_active.store(true, std::memory_order_release);
    spdlog::info("{} projected texture sets are live; every material without one keeps the game's textures", s_count);
}

auto ProjectedTextures::hold(const std::string& dataPath) -> TexturePointer*
{
    // Exactly how the engine loads its own projected textures, at startup and from its
    // ReloadProjectedUVTextures console command: a demand load
    RE::NiPointer<RE::NiTexture> texture;
    RE::BSShaderManager::GetTexture(dataPath.c_str(), true, texture, false);
    auto* const source = texture != nullptr ? netimmerse_cast<RE::NiSourceTexture*>(texture.get()) : nullptr;
    if (source == nullptr || source->rendererTexture == nullptr) {
        spdlog::warn("{} did not load as a renderer texture", dataPath);
        return nullptr;
    }
    return new TexturePointer {source}; // NOLINT(cppcoreguidelines-owning-memory): never freed on purpose
}

auto ProjectedTextures::holdForPbr(const std::string& dataPath) -> TexturePointer*
{
    // The engine's texture first: it is the one the landscape samples, so the copy below is
    // measured against it, and it lends the copy everything but its pixels
    TexturePointer* const original = hold(dataPath);
    if (original == nullptr) {
        return nullptr;
    }

    const auto dds = TextureColor::loadDds(dataPath);
    if (!dds.has_value()) {
        spdlog::warn("...so it is projected as the engine loaded it");
        return original;
    }
    const DirectX::TexMetadata& metadata = dds->metadata;
    const DirectX::ScratchImage& image = dds->image;
    if (!DirectX::IsSRGB(metadata.format)) {
        spdlog::warn("{} is not in an sRGB format (DXGI format {}), which Community Shaders' PBR expects of a base "
                     "color; it is projected as it is, and may not match the landscape",
                     dataPath,
                     static_cast<std::uint32_t>(metadata.format));
        return original;
    }

    // The same pixels in the format's non-sRGB counterpart: sampled, they come out as stored,
    // and the shader's own decode is the only one they get
    auto* const device = reinterpret_cast<ID3D11Device*>(RE::BSGraphics::Renderer::GetDevice());
    ID3D11ShaderResourceView* view = nullptr;
    const HRESULT result = device != nullptr ? DirectX::CreateShaderResourceViewEx(device,
                                                                                   image.GetImages(),
                                                                                   image.GetImageCount(),
                                                                                   metadata,
                                                                                   D3D11_USAGE_IMMUTABLE,
                                                                                   D3D11_BIND_SHADER_RESOURCE,
                                                                                   0,
                                                                                   0,
                                                                                   DirectX::CREATETEX_IGNORE_SRGB,
                                                                                   &view)
                                             : E_FAIL;
    if (FAILED(result) || view == nullptr) {
        spdlog::warn("{} could not be created as a non-sRGB texture (HRESULT {:#010x}); it is projected as the engine "
                     "loaded it",
                     dataPath,
                     static_cast<std::uint32_t>(result));
        return original;
    }
    ID3D11Resource* resource = nullptr;
    view->GetResource(&resource); // a reference of its own, kept for good like the view's

    // The engine's side of a texture: what the render pass binds
    const DXGI_FORMAT format = DirectX::MakeLinear(metadata.format);
    auto* const rendererTexture = new RE::NiTexture::RendererData(static_cast<std::uint16_t>(metadata.width),
                                                                  static_cast<std::uint16_t>(metadata.height));
    rendererTexture->texture = reinterpret_cast<REX::W32::ID3D11Texture2D*>(resource);
    rendererTexture->resourceView = reinterpret_cast<REX::W32::ID3D11ShaderResourceView*>(view);
    rendererTexture->unk1C = static_cast<std::uint8_t>(metadata.mipLevels); // mips
    rendererTexture->unk1D = static_cast<std::uint8_t>(format); // format, in the engine's byte

    // ...under a source texture of our own. Nothing in the engine builds one for a plugin, so it
    // is the engine's own, copied: same class, same name, same format preferences, and pixels of
    // ours. It is on no list (a texture's neighbors are the engine's bookkeeping, and a texture
    // that is never destroyed never has to leave one), it has no stream (that is for loading),
    // and its name is a reference of its own on the string pool's entry. It is never destroyed.
    auto* const clone = static_cast<RE::NiSourceTexture*>(RE::malloc(sizeof(RE::NiSourceTexture)));
    if (clone == nullptr) {
        return original;
    }
    std::memcpy(static_cast<void*>(clone), static_cast<const void*>(original->get()), sizeof(RE::NiSourceTexture));
    std::construct_at(&clone->name, (*original)->name);
    clone->prev = nullptr;
    clone->next = nullptr;
    clone->resourceStream = nullptr;
    clone->rendererTexture = reinterpret_cast<RE::BSGraphics::Texture*>(rendererTexture);

    spdlog::info("{}: {}x{}, {} mips, sRGB DXGI format {} - projected for PBR as its non-sRGB counterpart, format {}",
                 dataPath,
                 metadata.width,
                 metadata.height,
                 metadata.mipLevels,
                 static_cast<std::uint32_t>(metadata.format),
                 static_cast<std::uint32_t>(format));
    return new TexturePointer {clone}; // NOLINT(cppcoreguidelines-owning-memory): never freed on purpose
}

auto ProjectedTextures::floatSetting(const char* name) -> RE::Setting*
{
    auto* const setting = RE::GetINISetting(name);
    if (setting == nullptr || setting->GetType() != RE::Setting::Type::kFloat) {
        spdlog::warn("{} is not a float setting of the game's, so the profiles' tiling that goes there cannot apply; "
                     "their textures keep the game's ratio to the noise",
                     name);
        return nullptr;
    }
    spdlog::info(
        "{} is {:g}; the profiles that give their own tiling replace it draw by draw", name, setting->GetFloat());
    return setting;
}

auto ProjectedTextures::tilesPerNoiseTile(const std::optional<float>& worldUnits,
                                          float noiseFrequency) -> std::optional<float>
{
    if (!worldUnits.has_value()) {
        return std::nullopt;
    }
    // The shader samples the texture at the noise's coordinates times the setting, and those are
    // the projection's world units times the noise's frequency: one tile of the texture every
    // worldUnits of them is 1 / (worldUnits * frequency) texture tiles per noise tile
    const float tiles = 1.0F / (*worldUnits * noiseFrequency);
    return std::isfinite(tiles) && tiles > 0.0F ? std::optional<float> {tiles} : std::nullopt;
}

auto ProjectedTextures::setOf(const RE::NiColorA& color) -> std::size_t
{
    // The signature first: two twelve bit patterns no arithmetic lands on. Then the set, whose
    // byte an untagged color that happens to carry the signature has 0 in - which wraps to a
    // number far beyond s_count
    if ((std::bit_cast<std::uint32_t>(color.red) & MARK_MASK) != RED_MARK
        || (std::bit_cast<std::uint32_t>(color.green) & MARK_MASK) != GREEN_MARK) {
        return NOT_TAGGED;
    }
    const std::uint32_t set = (std::bit_cast<std::uint32_t>(color.blue) & SET_MASK) - 1U;
    return set < s_count ? set : NOT_TAGGED;
}

auto ProjectedTextures::samplesProjectedTextures(std::uint32_t lightingType) -> bool
{
    constexpr std::uint32_t PARALLAX = 3;
    constexpr std::uint32_t FACEGEN = 4;
    constexpr std::uint32_t FACEGEN_RGB_TINT = 5;
    constexpr std::uint32_t HAIR = 6; // the engine binds nothing projected for it in the first place
    constexpr std::uint32_t MULTILAYER_PARALLAX = 11;
    constexpr std::uint32_t MULTI_INDEX_SPARKLE = 14;
    return lightingType != PARALLAX && lightingType != FACEGEN && lightingType != FACEGEN_RGB_TINT
        && lightingType != HAIR && lightingType != MULTILAYER_PARALLAX && lightingType != MULTI_INDEX_SPARKLE;
}

void ProjectedTextures::SetupGeometryHook::thunk(RE::BSLightingShader* shader,
                                                 RE::BSRenderPass* pass,
                                                 std::uint32_t renderFlags)
{
    // This runs for every lit draw of every frame; all but the last few lines are about leaving
    // as early and as cheaply as possible
    if (!s_active.load(std::memory_order_acquire) || pass == nullptr || pass->shaderProperty == nullptr
        || !pass->shaderProperty->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kProjectedUV)) {
        s_func(shader, pass, renderFlags);
        return;
    }
    // Only lighting properties are drawn by this shader, and only they have a projection color
    const auto* const property = static_cast<const RE::BSLightingShaderProperty*>(pass->shaderProperty);
    auto* const state = RE::BSGraphics::State::GetSingleton();
    const std::size_t tagged = state != nullptr ? setOf(property->projectedUVColor) : NOT_TAGGED;
    if (tagged == NOT_TAGGED) {
        s_func(shader, pass, renderFlags);
        return;
    }

    // For the length of this one call the engine's projected textures are the profile's. Whether
    // to bind them at all (the setting, cube map passes), into which slots, with which sampler
    // state, and whether the state cache already holds them all stay the engine's decisions - and
    // the next projected draw finds the game's textures where they always were. The swaps move
    // pointers only; between them the holders keep the game's textures alive.
    const Set& set = s_sets.at(tagged);
    const bool surface = samplesProjectedTextures(((pass->passEnum - LIGHTING_TECHNIQUE_START) >> LIGHTING_TYPE_SHIFT)
                                                  & LIGHTING_TYPE_MASK);
    // The tiling the same way: what the settings the engine copies into ProjectedUVParams3 have to
    // say for the set's world units per tile on this draw's noise scale - the property's, which
    // the engine passes on as ProjectedUVParams.z - swapped in and out like the textures. Only
    // where the shader samples a diffuse, normal and detail normal at all
    const float noiseFrequency = property->projectedUVParams.blue;
    std::optional<float> diffuseTiling
        = surface && s_diffuseTiling != nullptr ? tilesPerNoiseTile(set.tiling.material, noiseFrequency) : std::nullopt;
    std::optional<float> detailTiling
        = surface && s_detailTiling != nullptr ? tilesPerNoiseTile(set.tiling.detail, noiseFrequency) : std::nullopt;
    const auto swapTiling = [](RE::Setting* setting, std::optional<float>& value) -> void {
        if (value.has_value()) {
            const float held = setting->GetFloat();
            setting->SetFloat(*value);
            value = held;
        }
    };
    const auto swapAll = [&]() -> void {
        swapTiling(s_diffuseTiling, diffuseTiling);
        swapTiling(s_detailTiling, detailTiling);
        if (set.noise != nullptr) {
            std::swap(state->defaultTextureProjNoiseMap, *set.noise);
        }
        if (set.detailNormal != nullptr) {
            std::swap(state->defaultTextureProjNormalDetailMap, *set.detailNormal);
        }
        if (surface && set.diffuse != nullptr) {
            std::swap(state->defaultTextureProjDiffuseMap, *set.diffuse);
        }
        if (surface && set.normal != nullptr) {
            std::swap(state->defaultTextureProjNormalMap, *set.normal);
        }
    };
    swapAll();
    s_func(shader, pass, renderFlags);
    swapAll();
}
