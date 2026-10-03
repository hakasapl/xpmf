#include "ProjectedVertexData.hpp"

#include "Offsets.hpp"
#include "ShelterRefinement.hpp"
#include "VertexLayout.hpp"

#include "PCH.h"

#include <intrin.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <mutex>
#include <optional>
#include <span>
#include <vector>

using namespace XPMF;

namespace {

constexpr std::size_t K_MAX_COUNT = std::numeric_limits<std::uint16_t>::max(); /**< Vertices or triangles a
                                                                                  BSTriShape counts to */
constexpr std::size_t K_RGB = 3; /**< A color's channels before the alpha */

/**
 * @brief FNV-1a over a byte range, continuing from a previous hash
 */
auto hashBytes(std::span<const std::uint8_t> bytes,
               std::uint64_t hash) -> std::uint64_t
{
    constexpr std::uint64_t PRIME = 0x100000001B3ULL;
    for (const auto byte : bytes) {
        hash = (hash ^ byte) * PRIME;
    }
    return hash;
}

template <typename T>
auto hashValue(const T& value,
               std::uint64_t hash) -> std::uint64_t
{
    std::array<std::uint8_t, sizeof(T)> bytes {};
    std::memcpy(bytes.data(), &value, sizeof(T));
    return hashBytes(bytes, hash);
}

/**
 * @brief Fills in one added vertex from the model's vertices it is a blend of
 *
 * Every attribute a static carries is interpolated by the blend's weights: uvs and colors
 * linearly, normal, tangent and bitangent as vectors that are normalized again. The position
 * is the split point the refinement computed. Whatever else the vertex holds (padding) is the
 * heaviest source's.
 *
 * @param in The model's layout
 * @param input The model's vertex data
 * @param out The variant's layout: the model's, or the model's with a color appended
 * @param vertex The variant's vertex to fill
 */
void interpolateVertex(const VertexLayout& in,
                       std::span<const std::uint8_t> input,
                       const VertexLayout& out,
                       std::span<std::uint8_t> vertex,
                       const ShelterRefinement::Vertex& blend)
{
    std::array<std::span<const std::uint8_t>, 3> from {};
    std::size_t heaviest = 0;
    for (std::size_t slot = 0; slot < from.size(); ++slot) {
        from.at(slot) = input.subspan(static_cast<std::size_t>(blend.source.at(slot)) * in.stride, in.stride);
        if (blend.weight.at(slot) > blend.weight.at(heaviest)) {
            heaviest = slot;
        }
    }
    std::memcpy(vertex.data(), from.at(heaviest).data(), in.stride);

    const auto weighted = [&](auto&& read) -> float {
        float total = 0.0F;
        for (std::size_t slot = 0; slot < from.size(); ++slot) {
            if (blend.weight.at(slot) > 0.0F) {
                total += blend.weight.at(slot) * read(from.at(slot));
            }
        }
        return total;
    };
    const auto floatAt = [](std::uint32_t offset) {
        return [offset](std::span<const std::uint8_t> source) -> float {
            float value = 0.0F;
            std::memcpy(&value, source.data() + offset, sizeof(value));
            return value;
        };
    };
    const auto halfAt = [](std::uint32_t offset) {
        return [offset](std::span<const std::uint8_t> source) -> float {
            std::uint16_t half = 0;
            std::memcpy(&half, source.data() + offset, sizeof(half));
            return VertexLayout::halfToFloat(half);
        };
    };
    const auto unitAt = [](std::uint32_t offset) {
        return [offset](std::span<const std::uint8_t> source) -> float { return VertexLayout::byteToUnit(source[offset]); };
    };
    const auto byteAt = [](std::uint32_t offset) {
        return [offset](std::span<const std::uint8_t> source) -> float { return static_cast<float>(source[offset]); };
    };
    const auto putFloat = [&](std::uint32_t offset, float value) -> void {
        std::memcpy(vertex.data() + offset, &value, sizeof(value));
    };
    const auto putHalf = [&](std::uint32_t offset, float value) -> void {
        const std::uint16_t half = VertexLayout::floatToHalf(value);
        std::memcpy(vertex.data() + offset, &half, sizeof(half));
    };
    const auto unitVector = [&](std::uint32_t offset) -> RE::NiPoint3 {
        return {weighted(unitAt(offset)), weighted(unitAt(offset + 1)), weighted(unitAt(offset + 2))};
    };
    const auto normalized = [](const RE::NiPoint3& vector) -> RE::NiPoint3 {
        const float length = vector.Length();
        constexpr float MIN_LENGTH = 1.0e-6F;
        return length > MIN_LENGTH ? vector * (1.0F / length) : vector;
    };
    const auto putUnitVector = [&](std::uint32_t offset, const RE::NiPoint3& vector) -> void {
        const RE::NiPoint3 unit = normalized(vector);
        vertex[offset] = VertexLayout::unitToByte(unit.x);
        vertex[offset + 1] = VertexLayout::unitToByte(unit.y);
        vertex[offset + 2] = VertexLayout::unitToByte(unit.z);
    };

    // The position is the split point itself
    putFloat(0, blend.position.x);
    putFloat(sizeof(float), blend.position.y);
    putFloat(2 * sizeof(float), blend.position.z);

    if (in.hasUV) {
        putHalf(out.uvOffset, weighted(halfAt(in.uvOffset)));
        putHalf(out.uvOffset + sizeof(std::uint16_t), weighted(halfAt(in.uvOffset + sizeof(std::uint16_t))));
    }
    if (in.hasUV2) {
        putHalf(out.uv2Offset, weighted(halfAt(in.uv2Offset)));
        putHalf(out.uv2Offset + sizeof(std::uint16_t), weighted(halfAt(in.uv2Offset + sizeof(std::uint16_t))));
    }
    if (in.hasNormals) {
        putUnitVector(out.normalOffset, unitVector(in.normalOffset));
    }
    if (in.hasTangents) {
        putUnitVector(out.tangentOffset, unitVector(in.tangentOffset));
    }

    // The bitangent lives in three places: x after the position, y behind the normal, z behind the tangent
    RE::NiPoint3 bitangent {weighted(floatAt(VertexLayout::BITANGENT_X_OFFSET)), 0.0F, 0.0F};
    if (in.hasNormals) {
        bitangent.y = weighted(unitAt(in.normalOffset + VertexLayout::NORMAL_COMPONENTS));
    }
    if (in.hasTangents) {
        bitangent.z = weighted(unitAt(in.tangentOffset + VertexLayout::NORMAL_COMPONENTS));
    }
    if (in.hasNormals && in.hasTangents) {
        bitangent = normalized(bitangent);
    }
    putFloat(VertexLayout::BITANGENT_X_OFFSET, bitangent.x);
    if (in.hasNormals) {
        vertex[out.normalOffset + VertexLayout::NORMAL_COMPONENTS] = VertexLayout::unitToByte(bitangent.y);
    }
    if (in.hasTangents) {
        vertex[out.tangentOffset + VertexLayout::NORMAL_COMPONENTS] = VertexLayout::unitToByte(bitangent.z);
    }

    // The color the sources blend to, or white with an alpha of 1 where the model has none; the
    // recipe has its say afterwards, as on every other vertex
    const auto color = vertex.subspan(out.colorOffset, VertexLayout::COLOR_SIZE);
    if (in.hasColors) {
        for (std::uint32_t channel = 0; channel < VertexLayout::COLOR_SIZE; ++channel) {
            const float value = weighted(byteAt(in.colorOffset + channel)) + 0.5F;
            color[channel] = static_cast<std::uint8_t>(std::clamp(value, 0.0F, static_cast<float>(VertexLayout::COLOR_MAX)));
        }
    } else {
        std::ranges::fill(color, VertexLayout::COLOR_MAX);
    }
}

/**
 * @brief An index buffer of its own for a refined triangle list, from the device the engine
 * renders with: a plain immutable buffer, which is all the engine's own are
 *
 * @return REX::W32::ID3D11Buffer* With one reference, the caller's; nullptr on failure
 */
auto createIndexBuffer(std::span<const std::uint16_t> indices) -> REX::W32::ID3D11Buffer*
{
    auto* const device = RE::BSGraphics::Renderer::GetDevice();
    if (device == nullptr || indices.empty()) {
        return nullptr;
    }
    REX::W32::D3D11_BUFFER_DESC desc {};
    desc.byteWidth = static_cast<std::uint32_t>(indices.size_bytes());
    desc.usage = REX::W32::D3D11_USAGE_DEFAULT;
    desc.bindFlags = REX::W32::D3D11_BIND_INDEX_BUFFER;
    const REX::W32::D3D11_SUBRESOURCE_DATA initial {indices.data(), 0, 0};
    REX::W32::ID3D11Buffer* buffer = nullptr;
    if (device->CreateBuffer(&desc, &initial, &buffer) < 0 || buffer == nullptr) {
        return nullptr;
    }
    return buffer;
}

} // namespace

auto ProjectedVertexData::shared(const Shape& shape) -> Data*
{
    if (shape.source == nullptr) {
        return nullptr;
    }
    // Colors the shader ignores tint nothing and mask nothing - to it the alpha is 1 already - so
    // such a shape needs no variant. One that shows them gets its alpha reset where the profile
    // neutralizes the alpha (unless it is transparency), and its colors whitened where it
    // neutralizes the colors
    if (!shape.colorsEnabled) {
        addRef(shape.source);
        return shape.source;
    }
    const bool resetAlpha = shape.neutralizeAlpha && !shape.keepAlpha;
    if (!resetAlpha && !shape.neutralize) {
        addRef(shape.source); // the mesh's colors and alpha stay what they are
        return shape.source;
    }

    const SharedKey key {.source = shape.source, .whiten = shape.neutralize, .resetAlpha = resetAlpha};
    const std::scoped_lock lock(s_lock);
    if (const auto found = s_shared.find(key); found != s_shared.end()) {
        addRef(found->second);
        return found->second;
    }

    // Built with the lock held: two loader threads cloning the same model at once would
    // otherwise both build it, and a buffer creation is quick
    Recipe recipe;
    recipe.whiten = shape.neutralize;
    recipe.resetAlpha = resetAlpha;

    Data* const variant = build(shape, recipe);
    if (variant == nullptr) {
        return nullptr;
    }
    if (variant != shape.source) {
        // Fresh from the engine with a count of 1: that one is the registry's. The registry also
        // pins the source, whose address is the key the variant is found by.
        addRef(shape.source);
        s_variants.emplace(variant, Entry {.source = shape.source, .isShared = true});
    } else {
        addRef(shape.source); // "nothing to change" is remembered too; this pin is the shared map's
    }
    s_shared.emplace(key, variant);
    addRef(variant); // the caller's
    return variant;
}

auto ProjectedVertexData::custom(const Shape& shape,
                                 std::span<const std::uint8_t> values) -> Data*
{
    if (shape.source == nullptr || values.size() != shape.vertexCount) {
        return nullptr;
    }
    {
        const std::scoped_lock lock(s_lock);
        if (s_privateBytes >= K_BUDGET) {
            return nullptr;
        }
    }

    // Only a shape whose alpha is free to be written gets this far (see ShelterMap::judge), so
    // it starts from the mesh's own unless the profile neutralizes it or the shader never showed it
    Recipe recipe;
    recipe.whiten = !shape.colorsEnabled || shape.neutralize;
    recipe.resetAlpha = !shape.colorsEnabled || shape.neutralizeAlpha;
    recipe.values = values;
    Data* const variant = build(shape, recipe);
    if (variant == nullptr) {
        return nullptr;
    }
    if (variant == shape.source) {
        addRef(variant);
        return variant;
    }

    const auto layout = VertexLayout::from(variant->vertexDesc);
    const std::size_t bytes = layout.has_value() ? static_cast<std::size_t>(layout->stride) * shape.vertexCount : 0;
    addRef(shape.source);
    addRef(variant); // the caller's, next to the registry's initial one
    const std::scoped_lock lock(s_lock);
    s_variants.emplace(variant,
                       Entry {.source = shape.source,
                              .bytes = bytes,
                              .fingerprint = fingerprint(values),
                              .colorless = !shape.colorsEnabled});
    s_privateBytes += bytes;
    return variant;
}

auto ProjectedVertexData::customRefined(const Shape& shape,
                                        const ShelterRefinement::Result& refinement,
                                        std::span<const std::uint8_t> values) -> Data*
{
    // The rest - the vertex count, the triangle list, what the added vertices blend - build() checks
    const std::size_t vertices = static_cast<std::size_t>(shape.vertexCount) + refinement.added.size();
    const std::size_t triangles = refinement.indices.size() / 3;
    if (shape.source == nullptr || refinement.added.empty() || values.size() != vertices || triangles > K_MAX_COUNT) {
        return nullptr;
    }
    {
        const std::scoped_lock lock(s_lock);
        if (s_privateBytes >= K_BUDGET) {
            return nullptr;
        }
    }

    Recipe recipe;
    recipe.whiten = !shape.colorsEnabled || shape.neutralize;
    recipe.resetAlpha = !shape.colorsEnabled || shape.neutralizeAlpha;
    recipe.values = values;
    recipe.refinement = &refinement;
    Data* const variant = build(shape, recipe);
    if (variant == nullptr || variant == shape.source) {
        return nullptr; // never the source: the topology differs
    }

    const auto layout = VertexLayout::from(variant->vertexDesc);
    const std::size_t bytes = (layout.has_value() ? static_cast<std::size_t>(layout->stride) * vertices : 0)
        + (refinement.indices.size() * sizeof(std::uint16_t));
    addRef(shape.source);
    addRef(variant); // the caller's, next to the registry's initial one
    const std::scoped_lock lock(s_lock);
    s_variants.emplace(variant,
                       Entry {.source = shape.source,
                              .bytes = bytes,
                              .fingerprint = fingerprintRefined(values, refinement),
                              .colorless = !shape.colorsEnabled,
                              .refined = true,
                              .counts = {.vertices = static_cast<std::uint16_t>(vertices),
                                         .triangles = static_cast<std::uint16_t>(triangles)},
                              .sourceCounts = {.vertices = static_cast<std::uint16_t>(shape.vertexCount),
                                               .triangles = static_cast<std::uint16_t>(shape.triangleCount)}});
    s_privateBytes += bytes;
    return variant;
}

auto ProjectedVertexData::sourceOf(Data* data) -> Data*
{
    const std::scoped_lock lock(s_lock);
    const auto found = s_variants.find(data);
    return found != s_variants.end() ? found->second.source : data;
}

auto ProjectedVertexData::isForColorlessShape(const Data* data) -> bool
{
    const std::scoped_lock lock(s_lock);
    const auto found = s_variants.find(data);
    return found != s_variants.end() && found->second.colorless;
}

auto ProjectedVertexData::countsOf(const Data* data) -> std::optional<Counts>
{
    const std::scoped_lock lock(s_lock);
    const auto found = s_variants.find(data);
    if (found == s_variants.end() || !found->second.refined) {
        return std::nullopt;
    }
    return found->second.counts;
}

auto ProjectedVertexData::sourceCountsOf(const Data* data) -> std::optional<Counts>
{
    const std::scoped_lock lock(s_lock);
    const auto found = s_variants.find(data);
    if (found == s_variants.end() || !found->second.refined) {
        return std::nullopt;
    }
    return found->second.sourceCounts;
}

auto ProjectedVertexData::fingerprintOf(const Data* data) -> std::uint64_t
{
    const std::scoped_lock lock(s_lock);
    const auto found = s_variants.find(data);
    return found != s_variants.end() ? found->second.fingerprint : 0;
}

auto ProjectedVertexData::fingerprint(std::span<const std::uint8_t> values) -> std::uint64_t
{
    constexpr std::uint64_t OFFSET_BASIS = 0xCBF29CE484222325ULL;
    const std::uint64_t hash = hashBytes(values, OFFSET_BASIS);
    return hash != 0 ? hash : 1; // 0 means "not a private variant"
}

auto ProjectedVertexData::fingerprintRefined(std::span<const std::uint8_t> values,
                                             const ShelterRefinement::Result& refinement) -> std::uint64_t
{
    constexpr std::uint64_t OFFSET_BASIS = 0xCBF29CE484222325ULL;
    constexpr std::uint64_t REFINED_SALT = 0x51F15ED0000ULL; /**< So that values on the model's topology and the
                                                                same values on a refined one never agree */
    std::uint64_t hash = hashBytes(values, OFFSET_BASIS ^ REFINED_SALT);
    const std::span<const std::uint8_t> indexBytes {
        reinterpret_cast<const std::uint8_t*>(refinement.indices.data()), // NOLINT: bytes of a trivial array
        refinement.indices.size() * sizeof(std::uint16_t)};
    hash = hashBytes(indexBytes, hash);
    for (const auto& added : refinement.added) {
        hash = hashValue(added.source, hash);
        hash = hashValue(added.weight, hash);
    }
    return hash != 0 ? hash : 1;
}

void ProjectedVertexData::addRef(Data* data)
{
    if (data != nullptr) {
        // The engine's own release is a lock xadd on this field
        _InterlockedIncrement(reinterpret_cast<volatile long*>(&data->refCount)); // NOLINT
    }
}

void ProjectedVertexData::release(Data* data)
{
    if (data == nullptr) {
        return;
    }
    // BSTriShape::~BSTriShape releases renderer data through vfunc 5 of the geometry buffer
    // manager; using the same path keeps this symmetric with the engine's allocator
    static const REL::Relocation<void**> managerAddress {Offsets::K_GEOMETRY_BUFFER_MANAGER};
    void* const manager = *managerAddress; // NOLINT
    if (manager == nullptr) {
        return; // leaking one buffer set beats calling into a dead manager
    }
    const auto* const vtbl = *reinterpret_cast<Offsets::ReleaseRendererData_t* const*>(manager);
    constexpr std::size_t RELEASE_VFUNC = 5;
    vtbl[RELEASE_VFUNC](manager, data); // NOLINT
}

void ProjectedVertexData::install(RE::BSTriShape& shape,
                                  Data* data)
{
    auto& geometry = shape.GetGeometryRuntimeData();
    Data* const previous = geometry.rendererData;
    if (data == nullptr || data == previous) {
        release(data);
        return;
    }

    // The renderer draws the shape by its own counts: a refined variant's come in with it, the
    // model's come back with anything else. A count is never left ahead of the buffers it is
    // read against: grown, the data goes in first; shrunk, the counts come down first
    const std::optional<Counts> counts = countsOf(data);
    const std::optional<Counts> wanted = counts.has_value() ? counts : sourceCountsOf(previous);
    auto& triShape = shape.GetTrishapeRuntimeData();
    const bool shrinking = wanted.has_value()
        && (wanted->vertices < triShape.vertexCount || wanted->triangles < triShape.triangleCount);
    if (shrinking) {
        triShape.vertexCount = wanted->vertices;
        triShape.triangleCount = wanted->triangles;
    }
    geometry.rendererData = data;
    geometry.vertexDesc = data->vertexDesc; // differs only where a color was appended
    if (wanted.has_value() && !shrinking) {
        triShape.vertexCount = wanted->vertices;
        triShape.triangleCount = wanted->triangles;
    }
    release(previous);
}

void ProjectedVertexData::collectGarbage()
{
    std::vector<Data*> dead;
    std::vector<const Data*> deadVariants;
    {
        // A count of 1 means only this registry is left holding on. Nothing can raise it in the
        // meantime: shared() needs the lock, and the engine only copies renderer data from a
        // shape that holds it, of which there are none.
        const std::scoped_lock lock(s_lock);
        std::erase_if(s_variants, [&](const auto& item) -> bool {
            auto* const variant = const_cast<Data*>(item.first); // NOLINT: the registry owns it
            if (variant->refCount > 1) {
                return false;
            }
            if (!item.second.isShared) {
                s_privateBytes -= std::min(s_privateBytes, item.second.bytes);
            }
            deadVariants.push_back(variant);
            dead.push_back(variant);
            dead.push_back(item.second.source);
            return true;
        });
        std::erase_if(s_shared, [&](const auto& item) -> bool {
            Data* const data = item.second;
            if (std::ranges::find(deadVariants, data) != deadVariants.end()) {
                return true; // a variant that just went; the registry's reference was its only one
            }
            if (data != item.first.source || data->refCount > 1) {
                return false;
            }
            dead.push_back(data); // an identical-to-source entry whose model has been unloaded
            return true;
        });
    }
    // Outside the lock: a release may free D3D buffers
    for (auto* const data : dead) {
        release(data);
    }
}

auto ProjectedVertexData::SharedKeyHash::operator()(const SharedKey& key) const noexcept -> std::size_t
{
    constexpr std::size_t WHITEN_SALT = 0x9E3779B97F4A7C15ULL; /**< So the keys of one source never collide */
    constexpr std::size_t RESET_SALT = 0x517CC1B727220A95ULL;
    return std::hash<const Data*> {}(key.source) ^ (key.whiten ? WHITEN_SALT : 0) ^ (key.resetAlpha ? RESET_SALT : 0);
}

auto ProjectedVertexData::build(const Shape& shape,
                                const Recipe& recipe) -> Data*
{
    const Data& source = *shape.source;
    const auto sourceLayout = VertexLayout::from(source.vertexDesc);
    if (!sourceLayout.has_value() || source.rawVertexData == nullptr || shape.vertexCount == 0) {
        return nullptr;
    }
    const ShelterRefinement::Result* const refinement = recipe.refinement;
    const std::size_t added = refinement != nullptr ? refinement->added.size() : 0;
    const std::size_t vertexCount = static_cast<std::size_t>(shape.vertexCount) + added;
    if (vertexCount > K_MAX_COUNT || (!recipe.values.empty() && recipe.values.size() != vertexCount)
        || (refinement != nullptr && (refinement->indices.empty() || refinement->indices.size() % 3 != 0))) {
        return nullptr;
    }
    if (refinement != nullptr
        && std::ranges::any_of(refinement->added, [&](const ShelterRefinement::Vertex& vertex) -> bool {
               return std::ranges::any_of(vertex.source,
                                          [&](std::uint16_t index) -> bool { return index >= shape.vertexCount; });
           })) {
        return nullptr; // an added vertex blends model vertices only
    }

    // A mesh without a color attribute gets one appended (white, until the recipe says otherwise)
    RE::BSGraphics::VertexDesc desc = source.vertexDesc;
    const auto layout = sourceLayout->hasColors ? sourceLayout : sourceLayout->withColors(desc);
    if (!layout.has_value()) {
        return nullptr;
    }

    const std::span<const std::uint8_t> input {source.rawVertexData,
                                               static_cast<std::size_t>(sourceLayout->stride) * shape.vertexCount};
    std::vector<std::uint8_t> output(static_cast<std::size_t>(layout->stride) * vertexCount);
    bool changed = !sourceLayout->hasColors || refinement != nullptr;
    for (std::size_t index = 0; index < vertexCount; ++index) {
        const auto to = std::span {output}.subspan(index * layout->stride, layout->stride);
        const auto color = to.subspan(layout->colorOffset, VertexLayout::COLOR_SIZE);
        if (index < shape.vertexCount) {
            const auto from = input.subspan(index * sourceLayout->stride, sourceLayout->stride);
            std::memcpy(to.data(), from.data(), from.size());
            if (!sourceLayout->hasColors) {
                std::ranges::fill(color, VertexLayout::COLOR_MAX);
            }
        } else {
            interpolateVertex(*sourceLayout, input, *layout, to, refinement->added[index - shape.vertexCount]);
        }
        const std::array<std::uint8_t, VertexLayout::COLOR_SIZE> before {color[0], color[1], color[2], color[3]};

        if (recipe.whiten) {
            std::ranges::fill(color.first(K_RGB), VertexLayout::COLOR_MAX);
        }
        if (recipe.resetAlpha) {
            color[K_RGB] = VertexLayout::COLOR_MAX;
        }
        if (!recipe.values.empty()) {
            // The alpha scaled, rounded to nearest: a mask the mesh's author painted is (where it
            // was kept) only ever lowered further, and a reset one becomes the shelter's alone
            const std::uint32_t scaled = static_cast<std::uint32_t>(color[K_RGB]) * recipe.values[index];
            color[K_RGB] = static_cast<std::uint8_t>((scaled + (VertexLayout::COLOR_MAX / 2)) / VertexLayout::COLOR_MAX);
        }
        changed = changed || !std::ranges::equal(before, color);
    }
    if (!changed) {
        return shape.source;
    }

    auto* const renderer = RE::BSGraphics::Renderer::GetSingleton();
    if (renderer == nullptr) {
        return nullptr;
    }

    // The triangles: the model's own index buffer, shared, unless the list changed - then a
    // buffer of the variant's own, which the engine's CreateTriShape takes a reference on and its
    // release lets go of exactly as it does the shared one
    REX::W32::ID3D11Buffer* own = nullptr;
    REX::W32::ID3D11Buffer** indexBuffer = &shape.source->indexBuffer;
    if (refinement != nullptr) {
        own = createIndexBuffer(refinement->indices);
        if (own == nullptr) {
            return nullptr;
        }
        indexBuffer = &own;
    }
    static const REL::Relocation<Offsets::CreateTriShapeData_t> createTriShapeData {Offsets::K_CREATE_TRISHAPE_DATA};
    Data* const variant = createTriShapeData(renderer,
                                             output.data(),
                                             static_cast<std::uint32_t>(output.size()),
                                             std::bit_cast<std::uint64_t>(desc),
                                             indexBuffer);
    if (own != nullptr) {
        own->Release(); // the variant holds its own now
    }
    if (variant == nullptr) {
        return nullptr;
    }

    // The engine leaves the CPU index list empty; the decal builder walks it, and so does this
    // plugin's own occluder gather. From RE::malloc because the engine's release frees it.
    const std::uint16_t* list = refinement != nullptr ? refinement->indices.data() : source.rawIndexData;
    const std::size_t count = refinement != nullptr ? refinement->indices.size()
                                                    : static_cast<std::size_t>(shape.triangleCount) * 3;
    if (list != nullptr && count > 0) {
        const std::size_t indexBytes = count * sizeof(std::uint16_t);
        if (auto* const indices = static_cast<std::uint16_t*>(RE::malloc(indexBytes)); indices != nullptr) {
            std::memcpy(indices, list, indexBytes);
            variant->rawIndexData = indices;
        }
    }
    return variant;
}
