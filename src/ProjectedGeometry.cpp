#include "ProjectedGeometry.hpp"

#include "ConfigLoader.hpp"
#include "EditorIdLookup.hpp"
#include "MaterialClassifier.hpp"
#include "ProjectedVertexData.hpp"
#include "SeasonsOfSkyrim.hpp"
#include "ShelterMap.hpp"
#include "ShelterRefinement.hpp"
#include "Text.hpp"
#include "VertexLayout.hpp"

#include "PCH.h"

#include <Windows.h>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

using namespace XPMF;

namespace {

using ShaderFlag = RE::BSShaderProperty::EShaderPropertyFlag;

/**
 * @brief Order independent 64 bit mix, for the field stamp
 */
auto hashMix(std::uint64_t hash,
             std::uint64_t value) -> std::uint64_t
{
    constexpr std::uint64_t GOLDEN = 0x9E3779B97F4A7C15ULL;
    return hash ^ (value + GOLDEN + (hash << 6U) + (hash >> 2U));
}

/**
 * @brief The cell a worldspace keeps its loaded large references in, or nullptr
 *
 * Read from the member rather than through TESWorldSpace::GetSkyCell, which is the engine's
 * get-or-create. A worldspace flagged to use its parent's sky cell has none of its own; the
 * engine walks up the same way.
 */
auto skyCellOf(const RE::TESWorldSpace* world) -> RE::TESObjectCELL*
{
    while (world != nullptr && world->skyCell == nullptr && world->parentWorld != nullptr
           && world->parentUseFlags.any(RE::TESWorldSpace::ParentUseFlag::kUseSkyCell)) {
        world = world->parentWorld;
    }
    return world != nullptr ? world->skyCell : nullptr;
}

/**
 * @brief Whether an object is of one of the game's own classes: its vtable lies in the game module
 *
 * Another plugin's subclass keeps its vtable in that plugin's DLL. Community Shaders' True PBR
 * gives a PBR shape a lighting material of a class of its own, in which the inherited fields
 * mean other things - specularColorScale is its roughness scale, specularPower its specular
 * level, rimLightPower its displacement scale - so such a material is not this plugin's to
 * scale: a specularMult of 0 made every snowed PBR rock a mirror.
 */
auto isGameClass(const void* object) -> bool
{
    const auto vtable = *static_cast<const std::uintptr_t*>(object);
    HMODULE module = nullptr;
    const bool found
        = ::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(vtable), // NOLINT: the API takes an address
                               &module)
        != 0;
    return found && reinterpret_cast<std::uintptr_t>(module) == REL::Module::get().base();
}

/**
 * @brief Calls visit on every leaf object under a root, depth first, looking at no more than
 * budget objects
 *
 * @param skipCulled Whether app-culled subtrees are left out (hidden states render nothing)
 */
template <typename Visit>
void forEachLeaf(RE::NiAVObject& root,
                 std::size_t budget,
                 bool skipCulled,
                 Visit&& visit)
{
    static thread_local std::vector<RE::NiAVObject*> stack;
    stack.clear();
    stack.push_back(&root);
    std::size_t visited = 0;
    while (!stack.empty() && visited < budget) {
        auto* const object = stack.back();
        stack.pop_back();
        ++visited;
        if (skipCulled && object->GetAppCulled()) {
            continue;
        }
        if (auto* const node = object->AsNode(); node != nullptr) {
            for (const auto& child : node->GetChildren()) {
                if (child != nullptr) {
                    stack.push_back(child.get());
                }
            }
            continue;
        }
        visit(*object);
    }
}

} // namespace

//
// Setup
//

auto ProjectedGeometry::isWanted() -> bool
{
    // Vertex colors, vertex alpha, roof shelter and specular are this class's own business. With
    // Seasons of Skyrim there is one more thing to do on a clone, for the material part: see
    // adoptWinterSnow
    return ConfigLoader::isAnyGeometryChanged() || ConfigLoader::isAnySpecularChanged()
        || (SeasonsOfSkyrim::isLoaded() && ConfigLoader::isAnyMaterialPatched());
}

void ProjectedGeometry::install()
{
    if (!isWanted()) {
        spdlog::info(
            "Nothing for the Clone3D hooks to do (no profile has neutralizeVertexColors, neutralizeVertexAlpha, "
            "roofShelter or specularMult on)");
        return;
    }

    // Statics do not override Clone3D, so this slot holds TESBoundObject's implementation; going
    // through the static vtable keeps every other form type out of the hook
    REL::Relocation<std::uintptr_t> vtable {RE::VTABLE_TESObjectSTAT[0]};
    Clone3DHook::s_func = vtable.write_vfunc(Clone3DHook::SLOT, Clone3DHook::thunk);
    spdlog::info("Static Clone3D hook installed");

    // The engine projects a material onto statics only, Seasons of Skyrim its winter snow onto
    // movable statics and containers as well - from hooks on these same slots, which are in by now
    // (kPostLoad), so that the ones written here wrap them and see what they did
    if (SeasonsOfSkyrim::isLoaded()) {
        REL::Relocation<std::uintptr_t> movable {RE::VTABLE_BGSMovableStatic[MOVABLE_STATIC_VTABLE]};
        WinterClone3DHook<0>::s_func = movable.write_vfunc(Clone3DHook::SLOT, WinterClone3DHook<0>::thunk);
        REL::Relocation<std::uintptr_t> container {RE::VTABLE_TESObjectCONT[0]};
        WinterClone3DHook<1>::s_func = container.write_vfunc(Clone3DHook::SLOT, WinterClone3DHook<1>::thunk);
        spdlog::info("Seasons of Skyrim is loaded: movable static and container Clone3D hooks installed");
    }
}

void ProjectedGeometry::onMaterialsReady(Materials materials,
                                         const RE::BGSMaterialObject* winterSnow)
{
    if (!isWanted()) {
        return;
    }
    s_materials = std::move(materials);
    if (const auto found = s_materials.find(winterSnow); winterSnow != nullptr && found != s_materials.end()) {
        s_winterSnow = &*found; // stays put: nothing is added to or taken from the map after this
        spdlog::info("Seasons of Skyrim's single pass winter snow is treated as profile '{}'{}",
                     found->second.profile->name,
                     found->second.untouched ? " (its record was left untouched, so its color is too)" : "");
    }
    findKept();
    s_ready.store(true, std::memory_order_release);
    if (!ConfigLoader::isAnyGeometryChanged()) {
        return; // no cell pass: only the clones' projection color is this class's to look after
    }

    auto* const events = RE::ScriptEventSourceHolder::GetSingleton();
    if (events == nullptr) {
        spdlog::error("No script event source; shapes keep their clone time vertex colors and roofs shelter nothing");
        return;
    }
    events->AddEventSink<RE::TESCellAttachDetachEvent>(&s_cellSink);
    s_cellPass.store(true, std::memory_order_release);
    {
        const std::scoped_lock lock(s_queueMutex);
        if (!s_workerStarted) {
            // Detached on purpose: joining at process exit would deadlock under the loader lock,
            // and the worker owns nothing that outlives the process
            std::thread(&ProjectedGeometry::workerLoop).detach();
            s_workerStarted = true;
        }
    }
    requestSlice();
    for (const auto& profile : ConfigLoader::getProfiles()) {
        spdlog::info(
            "Geometry pass: profile '{}' has {} material objects (vertex colors: {}, neutralize vertex alpha: {}, "
            "roof shelter: {}, roof shelter fix vertices: {})",
            profile.name,
            std::ranges::count_if(s_materials,
                                  [&](const auto& item) -> bool { return item.second.profile == &profile; }),
            profile.neutralizeVertexColors,
            profile.neutralizeVertexAlpha,
            profile.roofShelter,
            profile.roofShelter && profile.roofShelterFixVertices);
    }
}

auto ProjectedGeometry::keyOf(int cellX,
                              int cellY) -> CellKey
{
    return (static_cast<CellKey>(static_cast<std::uint32_t>(cellX)) << 32U) | static_cast<std::uint32_t>(cellY);
}

auto ProjectedGeometry::treatmentOf(const RE::BGSMaterialObject* material) -> const Treatment*
{
    if (material == nullptr) {
        return nullptr;
    }
    const auto found = s_materials.find(material);
    return found != s_materials.end() ? withGeometry(found->second) : nullptr;
}

auto ProjectedGeometry::withGeometry(const Treatment& treatment) -> const Treatment*
{
    // A profile with all of its clone-side settings off wants its statics left alone
    const auto& profile = *treatment.profile;
    const bool wanted = profile.neutralizeVertexColors || profile.neutralizeVertexAlpha || profile.roofShelter
        || profile.specularMult.has_value();
    return wanted ? &treatment : nullptr;
}

auto ProjectedGeometry::settingsOf(const Treatment& treatment,
                                   const RE::TESForm* base) -> Settings
{
    const auto& profile = *treatment.profile;
    const auto kept = s_kept.find(&profile);
    const auto named = [&](std::unordered_set<const RE::TESForm*> Kept::* list) -> bool {
        return kept != s_kept.end() && base != nullptr && (kept->second.*list).contains(base);
    };
    const bool shelter = profile.roofShelter && !named(&Kept::shelter);
    return {.neutralizeColors = profile.neutralizeVertexColors && !named(&Kept::colors),
            .neutralizeAlpha = profile.neutralizeVertexAlpha && !named(&Kept::alpha),
            .shelter = shelter,
            .fixVertices = shelter && profile.roofShelterFixVertices,
            .specularMult = profile.specularMult};
}

void ProjectedGeometry::findKept()
{
    constexpr std::size_t NAMED = 100; /**< Statics listed in the log per skip list; the rest are only counted */
    auto* const dataHandler = RE::TESDataHandler::GetSingleton();
    if (dataHandler == nullptr) {
        return;
    }
    for (const auto& profile : ConfigLoader::getProfiles()) {
        // The skip lists that mean anything: a setting that is off is applied to nobody anyway
        struct SkipList {
            const char* key {}; /**< The JSON key, for the log */
            const char* what {}; /**< What the named statics keep, for the log */
            const std::vector<std::string>* patterns {};
            std::unordered_set<const RE::TESForm*> Kept::* kept {};
        };
        std::vector<SkipList> lists;
        if (profile.neutralizeVertexColors && !profile.neutralizeVertexColorsSkip.empty()) {
            lists.push_back({.key = "neutralizeVertexColorsSkip",
                             .what = "vertex colors",
                             .patterns = &profile.neutralizeVertexColorsSkip,
                             .kept = &Kept::colors});
        }
        if (profile.neutralizeVertexAlpha && !profile.neutralizeVertexAlphaSkip.empty()) {
            lists.push_back({.key = "neutralizeVertexAlphaSkip",
                             .what = "vertex alpha",
                             .patterns = &profile.neutralizeVertexAlphaSkip,
                             .kept = &Kept::alpha});
        }
        if (profile.roofShelter && !profile.roofShelterSkip.empty()) {
            lists.push_back({.key = "roofShelterSkip",
                             .what = "snow under cover",
                             .patterns = &profile.roofShelterSkip,
                             .kept = &Kept::shelter});
        }
        if (lists.empty()) {
            continue;
        }

        // The statics that carry one of the profile's materials - and, where Seasons of Skyrim's
        // winter snow is treated as this profile, any static, movable static or container, since
        // that snow goes on whatever Seasons of Skyrim decides - with their EditorIDs, looked up once
        struct Candidate {
            const RE::TESForm* form {};
            std::string editorId; /**< As loaded, for the log */
            std::string lowerId; /**< What the patterns are matched against */
        };
        std::vector<Candidate> candidates;
        const auto consider = [&](const RE::TESForm* form) -> void {
            std::string editorId = EditorIdLookup::find(form);
            if (!editorId.empty()) {
                std::string lowerId = Text::toLower(editorId);
                candidates.push_back({.form = form, .editorId = std::move(editorId), .lowerId = std::move(lowerId)});
            }
        };
        const bool winter = s_winterSnow != nullptr && s_winterSnow->second.profile == &profile;
        for (const auto* const stat : dataHandler->GetFormArray<RE::TESObjectSTAT>()) {
            if (stat == nullptr) {
                continue;
            }
            const auto found = s_materials.find(stat->data.materialObj);
            if (winter || (found != s_materials.end() && found->second.profile == &profile)) {
                consider(stat);
            }
        }
        if (winter) {
            for (const auto* const movable : dataHandler->GetFormArray<RE::BGSMovableStatic>()) {
                consider(movable);
            }
            for (const auto* const container : dataHandler->GetFormArray<RE::TESObjectCONT>()) {
                consider(container);
            }
        }

        Kept& kept = s_kept[&profile];
        for (const auto& list : lists) {
            auto& named = kept.*list.kept;

            // How many statics each pattern names: one that names none is likely a typo
            struct PatternUse {
                const std::string* pattern {};
                std::size_t statics {};
            };
            std::vector<PatternUse> uses;
            uses.reserve(list.patterns->size());
            for (const auto& pattern : *list.patterns) {
                uses.push_back({.pattern = &pattern});
            }
            std::string names;
            for (const auto& candidate : candidates) {
                for (auto& use : uses) {
                    if (!MaterialClassifier::matches(*use.pattern, candidate.lowerId)) {
                        continue;
                    }
                    ++use.statics;
                    if (named.insert(candidate.form).second && named.size() <= NAMED) {
                        names += std::format("{}{} [{:08X}]",
                                             names.empty() ? "" : ", ",
                                             candidate.editorId,
                                             candidate.form->GetFormID());
                    }
                    break;
                }
            }
            for (const auto& use : uses) {
                if (use.statics == 0) {
                    spdlog::warn(
                        "Profile '{}': {} pattern '{}' names no static that carries one of its material objects",
                        profile.label(),
                        list.key,
                        *use.pattern);
                }
            }
            spdlog::info("Profile '{}': {} leaves {} statics their {}{}{}{}",
                         profile.label(),
                         list.key,
                         named.size(),
                         list.what,
                         named.empty() ? "" : ": ",
                         names,
                         named.size() > NAMED ? std::format(", and {} more", named.size() - NAMED) : "");
        }
    }
}

//
// Clone pass
//

auto ProjectedGeometry::Clone3DHook::thunk(RE::TESBoundObject* base,
                                           RE::TESObjectREFR* ref,
                                           bool arg3) -> RE::NiAVObject*
{
    auto* const root = s_func(base, ref, arg3);
    if (root == nullptr || ref == nullptr || !s_ready.load(std::memory_order_acquire)) {
        return root;
    }

    // The engine takes the material from the reference's base rather than from "this"; same here
    const auto* const object = ref->GetBaseObject();
    const auto* const stat = object != nullptr ? object->As<RE::TESObjectSTAT>() : nullptr;
    if (stat == nullptr) {
        return root;
    }
    // Seasons of Skyrim's winter snow first: it goes over whatever the base form's own material
    // had the engine project
    if (!dressWinterSnow(*root, stat)) {
        if (const auto* const treatment = treatmentOf(stat->data.materialObj); treatment != nullptr) {
            dressClone(*root, settingsOf(*treatment, stat));
        }
    }

    // Any static may be a roof: new 3D in a cell means its height layers are out of date. The
    // cell is the one the reference stands in, not the one that owns it - a large reference is
    // owned by the sky cell, which has no place on the grid (see startGather)
    if (s_cellPass.load(std::memory_order_acquire)) {
        const auto* const cell = ref->GetParentCell();
        if (cell == nullptr || !cell->IsInteriorCell()) {
            const auto position = ref->GetPosition();
            const CellKey key = keyOf(ShelterMap::cellOf(position.x), ShelterMap::cellOf(position.y));
            const std::scoped_lock lock(s_queueMutex);
            if (s_touched.empty() || s_touched.back() != key) {
                s_touched.push_back(key);
            }
        }
    }
    return root;
}

template <std::size_t N>
auto ProjectedGeometry::WinterClone3DHook<N>::thunk(RE::TESBoundObject* base,
                                                    RE::TESObjectREFR* ref,
                                                    bool arg3) -> RE::NiAVObject*
{
    auto* const root = s_func(base, ref, arg3);
    if (root != nullptr && s_ready.load(std::memory_order_acquire)) {
        dressWinterSnow(*root, ref != nullptr ? ref->GetBaseObject() : nullptr);
    }
    return root;
}

auto ProjectedGeometry::dressWinterSnow(RE::NiAVObject& root,
                                        const RE::TESForm* base) -> bool
{
    if (s_winterSnow == nullptr || !SeasonsOfSkyrim::hasWinterSnow(root)) {
        return false;
    }
    adoptWinterSnow(root, false); // a loader thread, and a fresh clone: none of it is on the list yet
    if (const auto* const treatment = withGeometry(s_winterSnow->second); treatment != nullptr) {
        dressClone(root, settingsOf(*treatment, base));
    }
    return true;
}

void ProjectedGeometry::adoptWinterSnow(RE::NiAVObject& root,
                                        bool switchedOffToo)
{
    const auto& [material, treatment] = *s_winterSnow;
    if (treatment.untouched) {
        return; // the record says what it always said, and Seasons of Skyrim's copy says the same
    }

    // Seasons of Skyrim projects a copy of the record's color and falloff values that it took
    // before MaterialMatcher gave the record the profile's (see SeasonsOfSkyrim). Both are read
    // from the shader property at every draw, so writing them is all it takes - to every shape the
    // snow is on and, on the main thread, every shape it was on until this plugin switched it off
    // (s_switchedOff), which wants the color for when it comes back. The falloff values only where
    // the profile overrides one: the property holds (scale, bias, 1 / noise UV scale, cos(max
    // angle)) the way Clone3D and Seasons of Skyrim write it, and the angle is the static's own
    const auto& data = material->directionalData;
    const RE::NiColor& color = data.singlePassColor;
    const bool falloff = treatment.profile->overridesFalloff();
    forEachLeaf(root, K_MAX_NODES_PER_REF, false, [&](RE::NiAVObject& object) -> void {
        auto* const geometry = object.AsGeometry();
        auto* const shader = geometry != nullptr
            ? netimmerse_cast<RE::BSLightingShaderProperty*>(geometry->GetGeometryRuntimeData().shaderProperty.get())
            : nullptr;
        if (shader == nullptr) {
            return;
        }
        const bool snowed = shader->flags.any(ShaderFlag::kProjectedUV)
            || (switchedOffToo && s_switchedOff.contains(geometry->AsTriShape()));
        if (!snowed) {
            return;
        }
        shader->projectedUVColor.red = color.red;
        shader->projectedUVColor.green = color.green;
        shader->projectedUVColor.blue = color.blue;
        if (falloff) {
            shader->projectedUVParams.red = data.falloffScale;
            shader->projectedUVParams.green = data.falloffBias;
            shader->projectedUVParams.blue = 1.0F / data.noiseUVScale;
        }
    });
}

void ProjectedGeometry::dressClone(RE::NiAVObject& root,
                                   const Settings& settings)
{
    forEachLeaf(root, K_MAX_NODES_PER_REF, false, [&](RE::NiAVObject& object) -> void {
        // Every lit shape the engine just put projected snow on gets the profile's specular...
        if (settings.specularMult.has_value()) {
            auto* const geometry = object.AsGeometry();
            auto* const shader = geometry != nullptr ? netimmerse_cast<RE::BSLightingShaderProperty*>(
                                                           geometry->GetGeometryRuntimeData().shaderProperty.get())
                                                     : nullptr;
            if (shader != nullptr && shader->flags.any(ShaderFlag::kProjectedUV)) {
                scaleSpecular(*shader, *settings.specularMult);
            }
        }
        // ...and only those that show their colors the shared variant: enabling colors on the
        // rest waits for the cell pass, which knows the alpha
        const auto shape = view(object);
        if (!shape.has_value() || !shape->shader->flags.all(ShaderFlag::kProjectedUV, ShaderFlag::kVertexColors)) {
            return;
        }
        const ProjectedVertexData::Shape description {.source = ProjectedVertexData::sourceOf(shape->data),
                                                      .vertexCount = shape->vertexCount,
                                                      .triangleCount = shape->triangleCount,
                                                      .colorsEnabled = true,
                                                      .keepAlpha = shape->keepAlpha,
                                                      .neutralize = settings.neutralizeColors,
                                                      .shelter = settings.shelter,
                                                      .neutralizeAlpha = settings.neutralizeAlpha,
                                                      .fixVertices = settings.fixVertices && shape->refinable};
        if (auto* const variant = ProjectedVertexData::shared(description); variant != nullptr) {
            ProjectedVertexData::install(*shape->shape, variant);
        }
    });
}

void ProjectedGeometry::scaleSpecular(RE::BSLightingShaderProperty& shader,
                                      float factor)
{
    // A lighting property only ever holds a lighting material, and the engine's RTTI cast would
    // not know Community Shaders' PBR subclass of it, so the casts are plain
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast)
    auto* const material = static_cast<RE::BSLightingShaderMaterialBase*>(shader.material);
    if (material == nullptr) {
        return;
    }
    if (!isGameClass(material)) {
        static std::atomic<bool> loggedForeign {false};
        if (!loggedForeign.exchange(true)) {
            spdlog::info("specularMult leaves materials of another plugin's class as they are (Community Shaders' True "
                         "PBR keeps its roughness scale in the specular field); the first such material was just met");
        }
        return;
    }
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast)
    auto* const copy = static_cast<RE::BSLightingShaderMaterialBase*>(material->Create());
    if (copy == nullptr) {
        return;
    }
    copy->CopyMembers(material);
    copy->specularColorScale *= factor;
    shader.SetMaterial(copy, true); // copies it once more, into one the property owns
    copy->~BSLightingShaderMaterialBase();
    RE::free(copy);
}

auto ProjectedGeometry::view(RE::NiAVObject& object) -> std::optional<ShapeView>
{
    // Plain tri shapes only: not dynamic, multi index or otherwise special ones
    auto* const shape = object.AsTriShape();
    if (shape == nullptr) {
        return std::nullopt;
    }
    const auto type = shape->GetType().get();
    if (type != RE::BSGeometry::Type::kTriShape && type != RE::BSGeometry::Type::kMeshLODTriShape) {
        return std::nullopt;
    }
    // ...that are rigid and have a CPU copy of their vertex data
    const auto& geometry = shape->GetGeometryRuntimeData();
    auto* const data = geometry.rendererData;
    if (geometry.skinInstance != nullptr || data == nullptr || data->rawVertexData == nullptr) {
        return std::nullopt;
    }

    // Solid, lit surfaces only: effect shaders are fog, glow and light shafts, and the
    // landscape / LOD flags mark terrain and LOD
    auto* const shader = netimmerse_cast<RE::BSLightingShaderProperty*>(geometry.shaderProperty.get());
    if (shader == nullptr
        || shader->flags.any(ShaderFlag::kSkinned,
                             ShaderFlag::kLODObjects,
                             ShaderFlag::kLODLandscape,
                             ShaderFlag::kMultiTextureLandscape)) {
        return std::nullopt;
    }

    // ...with a rigid static's vertex layout
    const auto layout = VertexLayout::from(data->vertexDesc);
    const auto& counts = shape->GetTrishapeRuntimeData();
    std::uint32_t vertexCount = counts.vertexCount;
    std::uint32_t triangleCount = counts.triangleCount;
    // A shape drawing a refined variant counts that variant's vertices and triangles; what
    // everything here reads and rebuilds from is the model's, which the registry remembers
    if (const auto model = ProjectedVertexData::sourceCountsOf(data); model.has_value()) {
        vertexCount = model->vertices;
        triangleCount = model->triangles;
    }
    if (!layout.has_value() || vertexCount == 0 || triangleCount == 0) {
        return std::nullopt;
    }
    // Alpha that is looked at for transparency is not this plugin's to rewrite - unless all it is
    // looked at for is a cut against a threshold, and the mesh paints none: such a shape can be
    // masked with its threshold scaled to match (scaledAlphaThreshold). Only an alpha property
    // looks: the Vertex_Alpha shader flag by itself blends and cuts nothing (Windhelm's
    // WHgrayquarter04 has it on the walkways under its roofs, painted 254 and 255, and they are
    // as snowed under vanilla as anything in the open), so it is not asked.
    bool keepAlpha = false;
    RE::NiAlphaProperty* alphaTest = nullptr;
    if (auto* const alpha = geometry.alphaProperty.get(); alpha != nullptr) {
        if (isPlainAlphaTest(*alpha) && !paintsAlpha(*ProjectedVertexData::sourceOf(data), vertexCount)) {
            alphaTest = alpha;
        } else {
            keepAlpha = true;
        }
    }
    return ShapeView {.shape = shape,
                      .shader = shader,
                      .data = data,
                      .vertexCount = vertexCount,
                      .triangleCount = triangleCount,
                      .keepAlpha = keepAlpha,
                      .refinable = type == RE::BSGeometry::Type::kTriShape,
                      .alphaTest = alphaTest};
}

auto ProjectedGeometry::isPlainAlphaTest(const RE::NiAlphaProperty& alpha) -> bool
{
    using TestFunction = RE::NiAlphaProperty::TestFunction;
    constexpr std::uint16_t FUNCTION_SHIFT = 10; // bits 10-12 of the flags, as GetAlphaTesting reads bit 9
    constexpr std::uint16_t FUNCTION_MASK = 0x7;
    const auto function = static_cast<TestFunction>((alpha.alphaFlags >> FUNCTION_SHIFT) & FUNCTION_MASK);
    // Only a "keep what is above" test scales with the alpha: anything else cuts the other way
    return alpha.GetAlphaTesting() && !alpha.GetAlphaBlending()
        && (function == TestFunction::kGreater || function == TestFunction::kGreaterEqual);
}

auto ProjectedGeometry::paintsAlpha(const Data& source,
                                    std::uint32_t vertexCount) -> bool
{
    const auto layout = VertexLayout::from(source.vertexDesc);
    if (!layout.has_value() || !layout->hasColors || source.rawVertexData == nullptr) {
        return false; // no color at all is alpha 1 everywhere to the shader
    }
    constexpr std::uint32_t ALPHA = 3; /**< Byte of the alpha within a color */
    const std::uint8_t* vertex = source.rawVertexData + layout->colorOffset + ALPHA;
    for (std::uint32_t index = 0; index < vertexCount; ++index, vertex += layout->stride) {
        if (*vertex != VertexLayout::COLOR_MAX) {
            return true;
        }
    }
    return false;
}

auto ProjectedGeometry::originalAlphaThreshold(const RE::NiAlphaProperty& alpha) -> std::uint8_t
{
    const auto found = s_alphaTests.find(&alpha);
    return found != s_alphaTests.end() ? found->second.original : alpha.alphaThreshold;
}

auto ProjectedGeometry::setAlphaThreshold(RE::BSTriShape& shape,
                                          std::uint8_t threshold) -> bool
{
    auto& geometry = shape.GetGeometryRuntimeData();
    auto* const alpha = geometry.alphaProperty.get();
    if (alpha == nullptr) {
        return true; // nothing tests the shape's alpha any more
    }

    // The shape's own copy takes any threshold. Asked for the mesh's it has done its work: the
    // model's property goes back on, and the copy goes with the last reference to it
    if (const auto found = s_alphaTests.find(alpha); found != s_alphaTests.end()) {
        if (threshold == found->second.original) {
            geometry.alphaProperty = found->second.model;
            s_alphaTests.erase(found);
        } else {
            alpha->alphaThreshold = threshold; // read at every draw; nothing to set up again
        }
        return true;
    }
    if (threshold == alpha->alphaThreshold) {
        return true; // the model's property at the mesh's threshold: as loaded
    }

    // The model's property is every instance's, and every shape's of the model that names the
    // same one: this shape's threshold needs a property of its own
    auto copy = copyOf(*alpha);
    auto* const own = copy.get();
    if (own == nullptr) {
        static std::atomic<bool> loggedRefusal {false};
        if (!loggedRefusal.exchange(true)) {
            spdlog::warn("The engine made no copy of an alpha property; alpha tested shapes keep their projection "
                         "under cover rather than be cut away by a lowered alpha");
        }
        return false;
    }
    const std::uint8_t original = alpha->alphaThreshold;
    own->alphaThreshold = threshold;
    ScaledAlphaTest scaled {
        .property = std::move(copy), .model = RE::NiPointer<RE::NiAlphaProperty> {alpha}, .original = original};
    geometry.alphaProperty = scaled.property;
    s_alphaTests.emplace(own, std::move(scaled));

    static std::atomic<bool> loggedCopy {false};
    if (!loggedCopy.exchange(true)) {
        spdlog::info("Alpha tested shapes partly under cover get an alpha property of their own for their scaled "
                     "test threshold, the model's being shared by all of its instances; the first was just made "
                     "(threshold {} -> {})",
                     static_cast<std::uint32_t>(original),
                     static_cast<std::uint32_t>(threshold));
    }
    return true;
}

auto ProjectedGeometry::copyOf(RE::NiAlphaProperty& model) -> RE::NiPointer<RE::NiAlphaProperty>
{
    // NiAlphaProperty::CreateClone answers with the property itself - which is how every clone of
    // a model comes to share one - unless the property is animated or one of the top two bits of
    // its flags is set. With the bit set for the length of the call the engine does the copying;
    // neither the model's property nor the copy keeps it.
    constexpr auto COPY_WHEN_CLONED = static_cast<std::uint16_t>(RE::NiAlphaProperty::AlphaFlags::kIsEditorModifiable);
    const std::uint16_t flags = model.alphaFlags;
    model.alphaFlags = static_cast<std::uint16_t>(flags | COPY_WHEN_CLONED);
    const RE::NiPointer<RE::NiObject> clone {model.Clone()};
    model.alphaFlags = flags;
    if (clone == nullptr || clone.get() == &model) {
        return nullptr;
    }
    auto* const copy = netimmerse_cast<RE::NiAlphaProperty*>(clone.get());
    if (copy == nullptr) {
        return nullptr;
    }
    copy->alphaFlags = flags;
    return RE::NiPointer<RE::NiAlphaProperty> {copy};
}

auto ProjectedGeometry::scaledAlphaThreshold(std::uint8_t original,
                                             std::uint8_t lowestAlpha) -> std::uint8_t
{
    // Rounded down: the lowest alpha then always clears it, as 1 always cleared the original
    return static_cast<std::uint8_t>((static_cast<std::uint32_t>(original) * lowestAlpha) / VertexLayout::COLOR_MAX);
}

void ProjectedGeometry::apply(const Swap& swap)
{
    using Flag8 = RE::BSShaderProperty::EShaderPropertyFlag8;
    RE::BSTriShape& shape = *swap.shape;
    Data* const data = swap.data;
    const bool projected = swap.projected;

    // An alpha tested shape's threshold goes with the vertex alpha it is about to get, and ahead
    // of it: the alpha alone would cut the shape away. A shape that cannot have its threshold
    // keeps what it has, vertex data and all
    if (swap.alphaThreshold.has_value() && !setAlphaThreshold(shape, *swap.alphaThreshold)) {
        ProjectedVertexData::release(data);
        return;
    }

    const bool hadColorlessVariant
        = ProjectedVertexData::isForColorlessShape(shape.GetGeometryRuntimeData().rendererData);
    ProjectedVertexData::install(shape, data);

    const auto& geometry = shape.GetGeometryRuntimeData();
    auto* const shader = geometry.shaderProperty.get();
    if (shader == nullptr) {
        return;
    }
    bool changed = false;

    // The engine set these in Clone3D for every lit shape of a static with such a material -
    // Projected_UV always, Snow where the material has the snow flag. A shape judged sheltered
    // loses them again, one that comes back into the open regains what it had: on a shape without
    // the projection the Snow flag alone would still switch the improved snow shading on, and ash
    // never had it.
    if (shader->flags.any(ShaderFlag::kProjectedUV) != projected) {
        shader->SetFlags(Flag8::kProjectedUV, projected);
        shader->SetFlags(Flag8::kSnow, projected && swap.isSnow);
        changed = true;
    }
    // Which shapes are switched off is remembered here and nowhere else: nothing on the property
    // says so (see collectReference)
    if (projected) {
        s_switchedOff.erase(&shape);
    } else {
        s_switchedOff.try_emplace(&shape, swap.shape);
    }

    // A variant always has colors worth showing (white wherever the mesh had none); a shape whose
    // shader ignored them has to be told to look, and told to stop once the variant is gone
    const bool hasColorlessVariant = ProjectedVertexData::isForColorlessShape(geometry.rendererData);
    if (hasColorlessVariant != hadColorlessVariant
        || (hasColorlessVariant && !shader->flags.any(ShaderFlag::kVertexColors))) {
        shader->SetFlags(Flag8::kVertexColors, hasColorlessVariant);
        changed = true;
    }
    if (changed) {
        shader->SetupGeometry(&shape); // as the engine does after changing the projected UV flags
    }
}

//
// Cell pass, main thread
//

auto ProjectedGeometry::CellSink::ProcessEvent(const RE::TESCellAttachDetachEvent* /*event*/,
                                               RE::BSTEventSource<RE::TESCellAttachDetachEvent>* /*source*/)
    -> RE::BSEventNotifyControl
{
    // Fired per reference, from wherever the cell is being attached; all it means here is "look
    // at the grid again"
    s_gridChanged.store(true, std::memory_order_release);
    requestSlice();
    return RE::BSEventNotifyControl::kContinue;
}

void ProjectedGeometry::slice()
{
    const auto now = Clock::now();

    // Behind a loading screen nothing is seen until it goes: the pass hurries while one is up, so
    // that what is under a roof is bare by the time the screen fades in
    auto* const ui = RE::UI::GetSingleton();
    const bool loading = ui != nullptr && ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME);
    s_loading.store(loading, std::memory_order_release);

    drainResults();
    for (std::size_t count = 0; count < K_APPLY_BATCH && !s_swaps.empty(); ++count) {
        Swap swap = std::move(s_swaps.front());
        s_swaps.pop_front();
        apply(swap);
    }
    retireSome();

    std::vector<CellKey> touched;
    {
        const std::scoped_lock lock(s_queueMutex);
        touched.swap(s_touched);
    }
    for (const auto key : touched) {
        if (const auto found = s_cells.find(key); found != s_cells.end()) {
            markDirty(found->second, now);
        }
    }

    if (s_gridChanged.exchange(false, std::memory_order_acq_rel) || now >= s_nextGridScan) {
        scanGrid(now);
        s_nextGridScan = now + K_GRID_RESCAN;
    }

    const auto deadline
        = now + (loading ? std::chrono::duration_cast<Clock::duration>(K_LOADING_SLICE_BUDGET) : K_SLICE_BUDGET);
    if (s_gather.has_value() || startGather(now)) {
        if (advanceGather(deadline)) {
            finishGather(Clock::now());
        }
    }
    scheduleReceivers(now);

    if (now >= s_nextGarbage) {
        ProjectedVertexData::collectGarbage();
        // A copied alpha property only this registry still holds was a shape's that is gone, and a
        // shape only the switched-off list still holds is gone itself
        std::erase_if(s_alphaTests, [](const auto& item) -> bool { return item.second.property->GetRefCount() <= 1; });
        std::erase_if(s_switchedOff, [](const auto& item) -> bool { return item.second->GetRefCount() <= 1; });
        s_nextGarbage = now + K_GARBAGE_INTERVAL;

        if (const auto builds = s_refinedBuilds.load(std::memory_order_relaxed); builds != s_loggedRefinedBuilds) {
            spdlog::info("Roof shelter vertex fix: {} refined vertex buffers built so far, adding {} vertices and {} "
                         "triangles in all over {} field probes",
                         builds,
                         s_refinedVertices.load(std::memory_order_relaxed),
                         s_refinedTriangles.load(std::memory_order_relaxed),
                         s_refinedProbes.load(std::memory_order_relaxed));
            s_loggedRefinedBuilds = builds;
        }
    }

    s_lastSliceEnd.store(Clock::now().time_since_epoch().count(), std::memory_order_relaxed);
    s_slicePending.store(hasWork(), std::memory_order_release);
    s_sliceQueued.store(false, std::memory_order_release);
    s_queueSignal.notify_one();
}

auto ProjectedGeometry::hasWork() -> bool
{
    if (s_gather.has_value() || !s_swaps.empty() || !s_retiredOccluders.empty() || !s_retiredReceivers.empty()) {
        return true;
    }
    // A pending settle recheck or receivers waiting for their neighbors are not work: both are
    // seconds away, and the idle tick is often enough to notice them
    return std::ranges::any_of(s_cells, [](const auto& item) -> bool { return item.second.dirty; });
}

void ProjectedGeometry::drainResults()
{
    std::deque<Result> results;
    {
        const std::scoped_lock lock(s_queueMutex);
        results.swap(s_results);
    }

    for (auto& result : results) {
        if (auto* const raster = std::get_if<RasterResult>(&result); raster != nullptr) {
            retire(raster->retired);
            const auto source = s_cells.find(raster->source);
            if (source == s_cells.end()) {
                continue;
            }
            source->second.rasterInFlight = false;
            if (raster->epoch != source->second.epoch) {
                continue; // gathered again since; that job's layers are the ones to keep
            }
            for (int slotY = 0; slotY < ShelterMap::K_BLOCK; ++slotY) {
                for (int slotX = 0; slotX < ShelterMap::K_BLOCK; ++slotX) {
                    const auto target = s_cells.find(keyOf(raster->cellX + slotX - 1, raster->cellY + slotY - 1));
                    if (target == s_cells.end()) {
                        continue;
                    }
                    auto& layer = raster->layers.at(static_cast<std::size_t>((slotY * ShelterMap::K_BLOCK) + slotX));
                    if (layer != nullptr) {
                        target->second.layers[raster->source] = std::move(layer);
                    } else if (target->second.layers.erase(raster->source) == 0) {
                        continue; // had nothing there before either
                    }
                    rebuildMap(target->second);
                }
            }
            continue;
        }

        auto& computed = std::get<ReceiverResult>(result);
        for (auto& swap : computed.swaps) {
            s_swaps.push_back(std::move(swap));
        }
        const auto cell = s_cells.find(computed.cell);
        if (cell == s_cells.end()) {
            retire(computed.receivers);
            continue;
        }
        cell->second.receiversInFlight = false;
        if (computed.epoch == cell->second.epoch) {
            cell->second.receivers = std::move(computed.receivers);
        } else {
            retire(computed.receivers); // a newer gather already replaced them
        }
    }
}

void ProjectedGeometry::retire(std::vector<Occluder>& occluders)
{
    std::ranges::move(occluders, std::back_inserter(s_retiredOccluders));
    occluders.clear();
}

void ProjectedGeometry::retire(std::vector<Receiver>& receivers)
{
    std::ranges::move(receivers, std::back_inserter(s_retiredReceivers));
    receivers.clear();
}

void ProjectedGeometry::retireSome()
{
    // In batches, and here rather than wherever a vector happens to die, so that a last
    // reference to a shape never goes anywhere surprising
    for (std::size_t count = 0; count < K_RETIRE_BATCH && !s_retiredOccluders.empty(); ++count) {
        ProjectedVertexData::release(s_retiredOccluders.back().pinned);
        s_retiredOccluders.pop_back();
    }
    for (std::size_t count = 0; count < K_RETIRE_BATCH && !s_retiredReceivers.empty(); ++count) {
        ProjectedVertexData::release(s_retiredReceivers.back().shape.source);
        s_retiredReceivers.pop_back();
    }
}

void ProjectedGeometry::markDirty(Cell& cell,
                                  Clock::time_point now)
{
    if (!cell.dirty) {
        cell.dirty = true;
        cell.firstDirtyAt = now;
    }
    cell.lastDirtyAt = now;
}

void ProjectedGeometry::scanGrid(Clock::time_point now)
{
    const auto* const tes = RE::TES::GetSingleton();
    const auto* const grid = tes != nullptr && tes->interiorCell == nullptr ? tes->gridCells : nullptr;

    std::unordered_set<CellKey> loaded;
    if (grid != nullptr) {
        for (std::uint32_t gridX = 0; gridX < grid->length; ++gridX) {
            for (std::uint32_t gridY = 0; gridY < grid->length; ++gridY) {
                auto* const cell = grid->GetCell(gridX, gridY);
                const auto* const coordinates
                    = cell != nullptr && cell->IsAttached() ? cell->GetCoordinates() : nullptr;
                if (coordinates == nullptr) {
                    continue;
                }
                const CellKey key = keyOf(coordinates->cellX, coordinates->cellY);
                loaded.insert(key);
                if (!s_cells.contains(key)) {
                    Cell fresh;
                    fresh.cellX = coordinates->cellX;
                    fresh.cellY = coordinates->cellY;
                    fresh.formId = cell->GetFormID();
                    fresh.firstDirtyAt = now;
                    fresh.lastDirtyAt = now;
                    s_cells.emplace(key, std::move(fresh));
                }
            }
        }
    }

    std::vector<CellKey> gone;
    for (const auto& [key, cell] : s_cells) {
        if (!loaded.contains(key)) {
            gone.push_back(key);
        }
    }
    for (const auto key : gone) {
        dropCell(key);
    }
}

void ProjectedGeometry::dropCell(CellKey key)
{
    const auto found = s_cells.find(key);
    if (found == s_cells.end()) {
        return;
    }
    if (s_gather.has_value() && s_gather->key == key) {
        retire(s_gather->occluders);
        retire(s_gather->receivers);
        s_gather.reset();
    }

    const int cellX = found->second.cellX;
    const int cellY = found->second.cellY;
    retire(found->second.receivers);
    s_cells.erase(found);

    // What its statics put over the cells around it goes with it
    for (int offsetY = -1; offsetY <= 1; ++offsetY) {
        for (int offsetX = -1; offsetX <= 1; ++offsetX) {
            const auto neighbor = s_cells.find(keyOf(cellX + offsetX, cellY + offsetY));
            if (neighbor != s_cells.end() && neighbor->second.layers.erase(key) > 0) {
                rebuildMap(neighbor->second);
            }
        }
    }
}

void ProjectedGeometry::rebuildMap(Cell& cell)
{
    std::vector<std::shared_ptr<const ShelterMap::Heights>> layers;
    layers.reserve(cell.layers.size());
    for (const auto& [source, layer] : cell.layers) {
        layers.push_back(layer);
    }
    cell.map = ShelterMap::combine(layers);
    ++cell.mapVersion;
}

auto ProjectedGeometry::startGather(Clock::time_point now) -> bool
{
    // Nearest cell first: what the player can see best is right soonest
    int playerX = 0;
    int playerY = 0;
    if (const auto* const player = RE::PlayerCharacter::GetSingleton(); player != nullptr) {
        const auto position = player->GetPosition();
        playerX = ShelterMap::cellOf(position.x);
        playerY = ShelterMap::cellOf(position.y);
    }

    // Behind a loading screen 3D streams in without pause, and nothing shows: a cell is gathered
    // sooner, and gathered again as more of it arrives
    const bool loading = s_loading.load(std::memory_order_acquire);
    const auto quietPeriod
        = std::chrono::duration_cast<Clock::duration>(loading ? K_LOADING_QUIET_PERIOD : K_QUIET_PERIOD);
    const auto maxDirtyWait = std::chrono::duration_cast<Clock::duration>(
        loading ? K_LOADING_DIRTY_WAIT : std::chrono::duration_cast<std::chrono::milliseconds>(K_MAX_DIRTY_WAIT));

    Cell* best = nullptr;
    CellKey bestKey = 0;
    int bestDistance = 0;
    for (auto& [key, cell] : s_cells) {
        if (cell.rasterInFlight) {
            continue; // one set of layers at a time per source
        }
        if (cell.recheckAt.has_value() && now >= *cell.recheckAt) {
            cell.recheckAt.reset();
            markDirty(cell, now - K_QUIET_PERIOD);
        }
        if (!cell.dirty || (now - cell.lastDirtyAt < quietPeriod && now - cell.firstDirtyAt < maxDirtyWait)) {
            continue;
        }
        const int distance = std::max(std::abs(cell.cellX - playerX), std::abs(cell.cellY - playerY));
        if (best == nullptr || distance < bestDistance) {
            best = &cell;
            bestKey = key;
            bestDistance = distance;
        }
    }
    if (best == nullptr) {
        return false;
    }

    auto* const form = RE::TESForm::LookupByID<RE::TESObjectCELL>(best->formId);
    if (form == nullptr || !form->IsAttached()) {
        best->dirty = false; // the grid scan will drop it
        return false;
    }

    Gather gather;
    gather.key = bestKey;
    gather.cellX = best->cellX;
    gather.cellY = best->cellY;
    {
        const auto& runtime = form->GetRuntimeData();
        const RE::BSSpinLockGuard locker(runtime.spinLock);
        gather.refs.reserve(runtime.references.size());
        for (const auto& ref : runtime.references) {
            if (ref != nullptr) {
                gather.refs.push_back(ref);
            }
        }
    }

    // Large references - which is to say every building - are not in that set: once loaded the
    // engine moves them into the worldspace's sky cell (QueuedPromoteLargeReferencesTask), where
    // they outlive their own cell. The ones standing in this cell are picked out by position.
    const std::size_t ownReferences = gather.refs.size();
    if (auto* const sky = skyCellOf(form->GetRuntimeData().worldSpace); sky != nullptr && sky != form) {
        const auto& runtime = sky->GetRuntimeData();
        const RE::BSSpinLockGuard locker(runtime.spinLock);
        for (const auto& ref : runtime.references) {
            if (ref == nullptr) {
                continue;
            }
            const auto position = ref->GetPosition();
            if (ShelterMap::cellOf(position.x) == gather.cellX && ShelterMap::cellOf(position.y) == gather.cellY) {
                gather.refs.push_back(ref);
            }
        }
    }

    // Nothing promises a reference is in one set only, and one gathered twice would be computed,
    // and have its vertex data swapped, twice
    if (gather.refs.size() > ownReferences) {
        std::ranges::sort(gather.refs, {}, [](const auto& ref) -> const RE::TESObjectREFR* { return ref.get(); });
        const auto duplicates = std::ranges::unique(
            gather.refs, {}, [](const auto& ref) -> const RE::TESObjectREFR* { return ref.get(); });
        gather.refs.erase(duplicates.begin(), duplicates.end());
    }
    best->dirty = false;
    s_gather.emplace(std::move(gather));
    return true;
}

auto ProjectedGeometry::advanceGather(Clock::time_point deadline) -> bool
{
    constexpr std::size_t CLOCK_STRIDE = 4; /**< References walked between looks at the clock */
    Gather& gather = *s_gather;
    while (gather.next < gather.refs.size()) {
        if (gather.next % CLOCK_STRIDE == 0 && Clock::now() >= deadline) {
            return false;
        }
        const auto& ref = gather.refs[gather.next];
        ++gather.next;
        collectReference(*ref); // never null: startGather skips those
    }
    return true;
}

void ProjectedGeometry::collectReference(RE::TESObjectREFR& ref)
{
    Gather& gather = *s_gather;

    if (ref.IsDisabled() || ref.IsDeleted()) {
        return;
    }
    const auto* const base = ref.GetBaseObject();
    if (base == nullptr) {
        return;
    }
    // Buildings, rocks and their kin shelter; trees shelter nothing worth the name. A container
    // matters only as something Seasons of Skyrim snows on
    const auto formType = base->GetFormType();
    const bool mayShelter = formType == RE::FormType::Static || formType == RE::FormType::MovableStatic
        || formType == RE::FormType::StaticCollection;
    if (!mayShelter && (s_winterSnow == nullptr || formType != RE::FormType::Container)) {
        return;
    }
    auto* const root = ref.Get3D();
    if (root == nullptr || root->GetAppCulled()) {
        return; // not loaded (yet), or hidden; its load marks the cell dirty again
    }

    // A reference's root sits at the reference's position once the engine has run its first
    // world update; fresh 3D still carries the transform of the model it was cloned from
    constexpr float READY_TOLERANCE_SQ = 1.0F;
    if (root->world.translate.GetSquaredDistance(ref.GetPosition()) > READY_TOLERANCE_SQ
        && root->world.translate.GetSquaredDistance(root->local.translate) > READY_TOLERANCE_SQ) {
        gather.unready = true;
        return;
    }

    // Anything of those kinds may be a roof as long as one profile asks for shelter. A receiver is
    // what stands under a material of a profile - which for the engine is a static's own, and for
    // Seasons of Skyrim's winter snow whatever 3D carries its tag, over anything the base form says
    const bool shelter = mayShelter && ConfigLoader::isAnyRoofSheltered();
    const auto* const stat = base->As<RE::TESObjectSTAT>();
    const bool winterSnow = s_winterSnow != nullptr && SeasonsOfSkyrim::hasWinterSnow(*root);
    if (winterSnow) {
        adoptWinterSnow(*root, true); // done at Clone3D already, unless its hooks ran after this plugin's
    }
    const auto* const treatment = winterSnow ? withGeometry(s_winterSnow->second)
        : stat != nullptr                    ? treatmentOf(stat->data.materialObj)
                                             : nullptr;
    const bool snowed = treatment != nullptr;
    if (!shelter && !snowed) {
        return;
    }
    const Settings settings = snowed ? settingsOf(*treatment, base) : Settings {};

    // Whether the Snow shader flag went on next to Projected_UV: the engine goes by the material's
    // snow flag, Seasons of Skyrim always sets it
    const bool isSnow = snowed
        && (winterSnow
            || stat->data.materialObj->directionalData.flags.any(RE::BSMaterialObject::DIRECTIONAL_DATA::Flag::kSnow));

    // Hidden subtrees render nothing (harvested states, editor markers), so they are skipped
    forEachLeaf(*root, K_MAX_NODES_PER_REF, true, [&](RE::NiAVObject& object) -> void {
        const auto shape = view(object);
        if (!shape.has_value()) {
            return;
        }

        // The model's own data is what is rasterized: the same surface as any variant of it, with
        // the counts the view reports (a refined variant has more of both) and with the model's
        // own layout (a variant may carry a color the model has not, and a stride to match)
        Data* const source = ProjectedVertexData::sourceOf(shape->data);
        const auto sourceLayout = VertexLayout::from(source->vertexDesc);
        if (shelter && sourceLayout.has_value() && source->rawIndexData != nullptr) {
            ProjectedVertexData::addRef(source);
            gather.occluders.push_back({.keepAlive = RE::NiPointer<RE::BSTriShape> {shape->shape},
                                        .pinned = source,
                                        .vertexCount = shape->vertexCount,
                                        .triangleCount = shape->triangleCount,
                                        .layout = *sourceLayout,
                                        .world = shape->shape->world});
        }

        // The engine projects snow onto every lit shape of such a static, so one without the flag
        // is one this plugin sheltered earlier - still a receiver, it may be in the open by now
        const bool colorsEnabled = shape->shader->flags.any(ShaderFlag::kVertexColors)
            && !ProjectedVertexData::isForColorlessShape(shape->data);
        // ...provided the engine really did project onto it, which with the flag off means this
        // plugin switched it off. Only the list says so. The projection color's alpha, which Clone3D
        // writes as 1, used to stand in for it and cannot: BSLightingShaderProperty's constructor
        // gives every property the color (0.6, 0.7, 0.8, 1), so the blood and dirt decals the engine
        // builds onto a static's shapes after the clone - fresh properties, under a BGSDecalNode in
        // its scene graph, which Clone3D's material pass (it takes every lighting property under the
        // root; the decals were not there yet) never saw - looked projected onto too, and were
        // switched on over the constructor's zero parameters: the whole decal under a flat patch of
        // the game's projected diffuse, on top of the blood
        const bool everProjected
            = shape->shader->flags.any(ShaderFlag::kProjectedUV) || s_switchedOff.contains(shape->shape);
        const bool receives = snowed && (colorsEnabled || settings.shelter) && everProjected;
        if (!receives) {
            return;
        }

        // What the shader will compare dot(normal, up) * alpha against on this shape, from the
        // values it was given to project with - (falloff scale, falloff bias, 1 / noise UV scale,
        // cos(max angle)), which is what Clone3D copies from the static and its material and what
        // Seasons of Skyrim writes itself - exactly as SetupGeometry derives it
        const auto& projection = shape->shader->projectedUVParams;
        const float cosAngle = projection.alpha;
        const float threshold = ((1.0F - cosAngle) * projection.green) + cosAngle;
        const float noiseAmplitude = (1.0F - cosAngle) * projection.red;
        ProjectedVertexData::addRef(source);
        gather.receivers.push_back(
            {.keepAlive = RE::NiPointer<RE::BSTriShape> {shape->shape},
             .shape = {.source = source,
                       .vertexCount = shape->vertexCount,
                       .triangleCount = shape->triangleCount,
                       .colorsEnabled = colorsEnabled,
                       .keepAlpha = shape->keepAlpha,
                       .neutralize = settings.neutralizeColors,
                       .shelter = settings.shelter,
                       .neutralizeAlpha = settings.neutralizeAlpha,
                       .fixVertices = settings.fixVertices && shape->refinable},
             .current = shape->data,
             .projected = shape->shader->flags.any(ShaderFlag::kProjectedUV),
             .currentFingerprint = ProjectedVertexData::fingerprintOf(shape->data),
             .world = shape->shape->world,
             .threshold = threshold,
             .noiseAmplitude = noiseAmplitude,
             .meanNoise = treatment->meanNoise,
             .fade = treatment->profile->shelterFade,
             .isSnow = isSnow,
             .alphaTest = shape->alphaTest != nullptr,
             .alphaThreshold
             = shape->alphaTest != nullptr ? originalAlphaThreshold(*shape->alphaTest) : std::uint8_t {0},
             .currentAlphaThreshold
             = shape->alphaTest != nullptr ? shape->alphaTest->alphaThreshold : std::uint8_t {0}});
    });
}

void ProjectedGeometry::finishGather(Clock::time_point now)
{
    Gather gather = std::move(*s_gather);
    s_gather.reset();

    const auto found = s_cells.find(gather.key);
    if (found == s_cells.end()) {
        retire(gather.occluders);
        retire(gather.receivers);
        return;
    }
    Cell& cell = found->second;
    const bool first = cell.epoch == 0;
    ++cell.epoch;

    // References that had 3D without a computed world transform were skipped; look again soon
    if (gather.unready && cell.unreadyRetries < K_MAX_UNREADY_RETRIES) {
        ++cell.unreadyRetries;
        markDirty(cell, now);
    }
    if (first) {
        cell.recheckAt = now + K_SETTLE_RECHECK;
    }

    retire(cell.receivers);
    cell.receivers = std::move(gather.receivers);
    cell.receiversSince = now;
    cell.computedAgainst = 0;

    if (!ConfigLoader::isAnyRoofSheltered()) {
        return; // nothing was gathered to rasterize
    }
    cell.rasterInFlight = true;
    submit(RasterJob {.source = gather.key,
                      .cellX = gather.cellX,
                      .cellY = gather.cellY,
                      .epoch = cell.epoch,
                      .occluders = std::move(gather.occluders)});
}

auto ProjectedGeometry::fieldStamp(const Cell& cell) -> std::uint64_t
{
    constexpr std::uint64_t STAMP_SEED = 0x5EED; /**< Any non-zero start */
    std::uint64_t stamp = hashMix(STAMP_SEED, cell.epoch);
    for (int offsetY = -1; offsetY <= 1; ++offsetY) {
        for (int offsetX = -1; offsetX <= 1; ++offsetX) {
            const auto neighbor = s_cells.find(keyOf(cell.cellX + offsetX, cell.cellY + offsetY));
            stamp = hashMix(stamp, neighbor != s_cells.end() ? neighbor->second.mapVersion + 1 : 0);
        }
    }
    return stamp != 0 ? stamp : 1; // 0 is "never computed"
}

auto ProjectedGeometry::neighborhoodBusy(const Cell& cell) -> bool
{
    for (int offsetY = -1; offsetY <= 1; ++offsetY) {
        for (int offsetX = -1; offsetX <= 1; ++offsetX) {
            const CellKey key = keyOf(cell.cellX + offsetX, cell.cellY + offsetY);
            const auto neighbor = s_cells.find(key);
            if (neighbor == s_cells.end()) {
                continue;
            }
            if (neighbor->second.dirty || neighbor->second.rasterInFlight
                || (s_gather.has_value() && s_gather->key == key)) {
                return true;
            }
        }
    }
    return false;
}

void ProjectedGeometry::scheduleReceivers(Clock::time_point now)
{
    for (auto& [key, cell] : s_cells) {
        if (cell.receivers.empty() || cell.receiversInFlight) {
            continue;
        }
        const std::uint64_t stamp = fieldStamp(cell);
        if (stamp == cell.computedAgainst) {
            continue;
        }
        // Colors computed against half a neighborhood are computed twice; only a neighborhood
        // that never settles is not waited for. The exception is a cell's very first judgement:
        // it waits for the cell's own roofs alone, so that a floor under its own building is
        // bare within a moment of loading, and the neighbors' roofs - which take seconds more to
        // arrive, one gather at a time - are caught by the second pass their maps set off
        if (cell.rasterInFlight) {
            continue;
        }
        if (cell.everJudged && neighborhoodBusy(cell) && now - cell.receiversSince < K_RECEIVER_TIMEOUT) {
            continue;
        }

        ReceiverJob job;
        job.cell = key;
        job.epoch = cell.epoch;
        job.field.centerX = cell.cellX;
        job.field.centerY = cell.cellY;
        for (int offsetY = -1; offsetY <= 1; ++offsetY) {
            for (int offsetX = -1; offsetX <= 1; ++offsetX) {
                const auto neighbor = s_cells.find(keyOf(cell.cellX + offsetX, cell.cellY + offsetY));
                if (neighbor != s_cells.end()) {
                    job.field.maps.at(static_cast<std::size_t>(((offsetY + 1) * ShelterMap::K_BLOCK) + offsetX + 1))
                        = neighbor->second.map;
                }
            }
        }
        job.receivers = std::move(cell.receivers);
        cell.receivers.clear();
        cell.receiversInFlight = true;
        cell.everJudged = true;
        cell.computedAgainst = stamp;
        submit(std::move(job));
    }
}

//
// Worker
//

void ProjectedGeometry::submit(Job job)
{
    {
        const std::scoped_lock lock(s_queueMutex);
        s_jobs.push_back(std::move(job));
    }
    s_queueSignal.notify_one();
}

void ProjectedGeometry::requestSlice()
{
    s_slicePending.store(true, std::memory_order_release);
    s_queueSignal.notify_one();
}

void ProjectedGeometry::pumpSlice()
{
    // SKSE drains its task queue once per frame, and a task queued from inside that drain runs
    // in the same frame - so slices are queued from here, one at a time, each only after the
    // previous one finished and a short pause has let that frame's drain end
    if (!s_cellPass.load(std::memory_order_acquire) || s_sliceQueued.load(std::memory_order_acquire)) {
        return;
    }
    const Clock::time_point lastEnd {Clock::duration {s_lastSliceEnd.load(std::memory_order_relaxed)}};
    const auto spacing = s_slicePending.load(std::memory_order_acquire)
        ? std::chrono::duration_cast<Clock::duration>(K_SLICE_SPACING)
        : std::chrono::duration_cast<Clock::duration>(K_IDLE_TICK);
    if (Clock::now() < lastEnd + spacing) {
        return;
    }
    const auto* const taskInterface = SKSE::GetTaskInterface();
    if (taskInterface == nullptr) {
        return;
    }
    s_sliceQueued.store(true, std::memory_order_release);
    taskInterface->AddTask([]() { slice(); });
}

void ProjectedGeometry::workerLoop()
{
    // Nothing here is latency critical - the clone pass already gave snow its color - while the
    // game's own threads are. Behind a loading screen it is the other way around: the work has
    // to be done by the time the screen fades in, and the game is waiting on its disk
    ::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    bool hurried = false;

    for (;;) {
        if (const bool loading = s_loading.load(std::memory_order_acquire); loading != hurried) {
            ::SetThreadPriority(::GetCurrentThread(), loading ? THREAD_PRIORITY_NORMAL : THREAD_PRIORITY_BELOW_NORMAL);
            hurried = loading;
        }
        std::optional<Job> job;
        {
            std::unique_lock<std::mutex> lock(s_queueMutex);
            if (s_jobs.empty()) {
                s_queueSignal.wait_for(lock,
                                       s_slicePending.load(std::memory_order_acquire) ? K_SLICE_SPACING : K_IDLE_TICK);
            }
            if (!s_jobs.empty()) {
                job = std::move(s_jobs.front());
                s_jobs.pop_front();
            }
        }

        pumpSlice();
        if (!job.has_value()) {
            continue;
        }

        Result result = std::visit([](auto& typed) -> Result { return run(typed); }, *job);
        {
            const std::scoped_lock lock(s_queueMutex);
            s_results.push_back(std::move(result));
        }
        requestSlice();
    }
}

auto ProjectedGeometry::run(RasterJob& job) -> RasterResult
{
    std::array<ShelterMap::Layer, ShelterMap::K_BLOCK_CELLS> layers;
    for (int slotY = 0; slotY < ShelterMap::K_BLOCK; ++slotY) {
        for (int slotX = 0; slotX < ShelterMap::K_BLOCK; ++slotX) {
            auto& layer = layers.at(static_cast<std::size_t>((slotY * ShelterMap::K_BLOCK) + slotX));
            layer.cellX = job.cellX + slotX - 1;
            layer.cellY = job.cellY + slotY - 1;
        }
    }

    std::vector<RE::NiPoint3> world;
    for (const auto& occluder : job.occluders) {
        const std::span<const std::uint8_t> vertices {
            occluder.pinned->rawVertexData, static_cast<std::size_t>(occluder.layout.stride) * occluder.vertexCount};
        const std::span<const std::uint16_t> indices {occluder.pinned->rawIndexData,
                                                      static_cast<std::size_t>(occluder.triangleCount) * 3};
        world.resize(occluder.vertexCount);
        for (std::uint32_t index = 0; index < occluder.vertexCount; ++index) {
            world[index] = occluder.world
                * VertexLayout::position(vertices.subspan(static_cast<std::size_t>(index) * occluder.layout.stride,
                                                          occluder.layout.stride));
        }
        for (std::size_t corner = 0; corner + 2 < indices.size(); corner += 3) {
            const std::uint16_t first = indices[corner];
            const std::uint16_t second = indices[corner + 1];
            const std::uint16_t third = indices[corner + 2];
            if (first < occluder.vertexCount && second < occluder.vertexCount && third < occluder.vertexCount) {
                ShelterMap::rasterize(layers, world[first], world[second], world[third]);
            }
        }
    }

    RasterResult result;
    result.source = job.source;
    result.cellX = job.cellX;
    result.cellY = job.cellY;
    result.epoch = job.epoch;
    for (std::size_t slot = 0; slot < layers.size(); ++slot) {
        if (!layers.at(slot).top.empty()) {
            result.layers.at(slot) = std::make_shared<const ShelterMap::Heights>(std::move(layers.at(slot).top));
        }
    }
    result.retired = std::move(job.occluders);
    return result;
}

void ProjectedGeometry::worldPositions(const Receiver& receiver,
                                       const VertexLayout& layout,
                                       std::vector<RE::NiPoint3>& positions)
{
    const Data& source = *receiver.shape.source;
    const std::uint32_t vertexCount = receiver.shape.vertexCount;
    const std::span<const std::uint8_t> vertices {source.rawVertexData,
                                                  static_cast<std::size_t>(layout.stride) * vertexCount};
    positions.resize(vertexCount);
    for (std::uint32_t index = 0; index < vertexCount; ++index) {
        positions[index] = receiver.world
            * VertexLayout::position(vertices.subspan(static_cast<std::size_t>(index) * layout.stride, layout.stride));
    }
}

auto ProjectedGeometry::refinementLimits(std::uint32_t vertexCount) -> ShelterRefinement::Limits
{
    constexpr std::size_t MAX_COUNT = std::numeric_limits<std::uint16_t>::max(); /**< What a BSTriShape counts to */
    constexpr std::size_t MIN_ALLOWANCE = 128; /**< Even a plank of four vertices may need a few loops */
    const std::size_t room = MAX_COUNT - std::min<std::size_t>(vertexCount, MAX_COUNT);
    return {.tolerance = ShelterRefinement::K_TOLERANCE,
            .minEdge = ShelterRefinement::K_MIN_EDGE,
            .minHeight = ShelterRefinement::K_MIN_HEIGHT,
            .maxRounds = ShelterRefinement::K_MAX_ROUNDS,
            .maxAddedVertices = std::min({ShelterRefinement::K_MAX_ADDED_VERTICES,
                                          std::max<std::size_t>(vertexCount, MIN_ALLOWANCE),
                                          room}),
            .maxTriangles = MAX_COUNT};
}

auto ProjectedGeometry::run(ReceiverJob& job) -> ReceiverResult
{
    constexpr float FULL = VertexLayout::COLOR_MAX; /**< As a float, for the alpha arithmetic */
    constexpr float MIN_UP = 0.05F; /**< Below this a surface carries no snow, and dividing by it is unwise */

    ReceiverResult result;
    result.cell = job.cell;
    result.epoch = job.epoch;

    std::vector<RE::NiPoint3> positions;
    std::vector<RE::NiPoint3> modelPositions;
    std::vector<RE::NiPoint3> normals;
    std::vector<float> openness;
    std::vector<bool> measured; // per vertex, whether the vertex fix has its openness in hand
    std::vector<float> facing;
    std::vector<std::uint8_t> values; // per vertex, what the mesh's alpha is scaled by, or becomes
    for (auto& receiver : job.receivers) {
        const Data& source = *receiver.shape.source;
        const auto layout = VertexLayout::from(source.vertexDesc);
        if (!layout.has_value() || source.rawVertexData == nullptr) {
            continue;
        }
        const bool shelter = receiver.shape.shelter; // its profile's roofShelter
        const std::uint32_t vertexCount = receiver.shape.vertexCount;
        const std::span<const std::uint8_t> vertices {source.rawVertexData,
                                                      static_cast<std::size_t>(layout->stride) * vertexCount};
        const auto vertexAt = [&](std::uint32_t index) -> std::span<const std::uint8_t> {
            return vertices.subspan(static_cast<std::size_t>(index) * layout->stride, layout->stride);
        };

        // World space normals: the rotation alone, a NiTransform scaling uniformly. Their z is how
        // far up a vertex faces; their tilt is what the shelter map reads its own surface by
        normals.clear();
        if (layout->hasNormals) {
            normals.resize(vertexCount);
            for (std::uint32_t index = 0; index < vertexCount; ++index) {
                normals[index] = receiver.world.rotate * layout->normal(vertexAt(index));
            }
        }

        // The shelter's alpha value per vertex: 1 in the open and less under cover, which the
        // alpha the shape starts from - the mesh's own, so that a mask its author painted only
        // ever loses more, or 1 where the profile neutralizes it - is multiplied by. Alpha only means
        // something relative to what the shader compares it with: a surface facing up by
        // `facing`, at an alpha of 1, has just lost its snow at (threshold + K_BLEND_FLOOR) /
        // facing, so openness 0..1 is mapped onto [that, 1] - the same openness then means the
        // same amount of snow on a 30 degree walkway (threshold 0.93) as on a 90 degree rock
        // (0.4), instead of the walkway going bare at the first hint of cover.
        const auto goneAlpha = [&](float facing) -> float {
            return std::clamp(
                K_NORMAL_MAP_SAFETY * (receiver.threshold + K_BLEND_FLOOR) / std::max(facing, MIN_UP), 0.0F, 1.0F);
        };
        // ...and where on that scale snow visibly ends on a flat surface (projection weight 0 at
        // average noise), which is what the edge localization aims for
        const float flatGone = goneAlpha(1.0F);
        constexpr float MIN_SPAN = 0.01F;
        constexpr float EDGE_MARGIN = 0.05F;
        const float edgeOpenness
            = std::clamp((receiver.threshold + (receiver.noiseAmplitude * receiver.meanNoise) - flatGone)
                             / std::max(1.0F - flatGone, MIN_SPAN),
                         EDGE_MARGIN,
                         1.0F - EDGE_MARGIN);
        // ...and what the fade under cover is bent to pass at a fixed share of its distance, so
        // that one shelterFade ends the snow as far in on this material as on any other
        const ShelterMap::Fade fade = ShelterMap::Fade::of(receiver.fade, edgeOpenness);

        // A vertex holds snow if it could show any with nothing overhead
        const float holdsSnowFrom = std::max(receiver.threshold + K_BLEND_FLOOR, MIN_UP);

        // A shape without a CPU index list still gets per vertex values, just no edge placement
        // and no refinement
        std::span<const std::uint16_t> indices;
        if (source.rawIndexData != nullptr) {
            indices = {source.rawIndexData, static_cast<std::size_t>(receiver.shape.triangleCount) * 3};
        }

        auto verdict = ShelterMap::Verdict::OPEN;
        std::optional<ShelterRefinement::Result> refinement;
        if (shelter) {
            worldPositions(receiver, *layout, positions);
            bool anyCover = job.field.initialOpenness(positions, normals, fade, openness);

            // The vertex fix, where the profile wants it and the shape can take it: with a vertex
            // under cover there is a fade to follow; without one there may still be a roof edge
            // crossing the middle of a triangle, which is worth a look only if any column over
            // the shape's footprint tops out above it at all
            measured.clear();
            if (receiver.shape.fixVertices && !receiver.shape.keepAlpha && !indices.empty()) {
                bool worthALook = anyCover;
                if (!worthALook) {
                    RE::NiPoint3 low = positions.front();
                    RE::NiPoint3 high = positions.front();
                    for (const auto& position : positions) {
                        low.x = std::min(low.x, position.x);
                        low.y = std::min(low.y, position.y);
                        low.z = std::min(low.z, position.z);
                        high.x = std::max(high.x, position.x);
                        high.y = std::max(high.y, position.y);
                        high.z = std::max(high.z, position.z);
                    }
                    worthALook = job.field.overhead(low.x, low.y, high.x, high.y, low.z, high.z).anyCovered;
                }
                if (worthALook) {
                    modelPositions.resize(vertexCount);
                    for (std::uint32_t index = 0; index < vertexCount; ++index) {
                        modelPositions[index] = VertexLayout::position(vertexAt(index));
                    }
                    refinement = ShelterRefinement::refine(job.field,
                                                           modelPositions,
                                                           positions,
                                                           normals,
                                                           openness,
                                                           measured,
                                                           indices,
                                                           holdsSnowFrom,
                                                           fade,
                                                           refinementLimits(vertexCount));
                    if (refinement.has_value()) {
                        indices = refinement->indices;
                        anyCover = anyCover
                            || std::ranges::any_of(openness, [](float value) -> bool { return value < 1.0F; });
                        s_refinedProbes.fetch_add(refinement->probes, std::memory_order_relaxed);
                    }
                }
            }

            if (anyCover) {
                // What the vertex fix tested stands as measured: left to the settling, a floor's
                // rim would be pulled down to mend the middle of triangles that are no longer
                // there, and a hard edge left along it. The settling is for the rest - what was
                // too narrow or too small to test, and every shape the fix is not for
                job.field.settleOpenness(positions, indices, fade, edgeOpenness, measured, openness);
                facing.resize(positions.size());
                for (std::size_t index = 0; index < positions.size(); ++index) {
                    facing[index] = layout->hasNormals ? normals[index].z : 1.0F;
                }
                verdict = ShelterMap::judge(ShelterMap::tally(openness, facing, holdsSnowFrom, edgeOpenness),
                                            !receiver.shape.keepAlpha);
            }
        }

        Data* wanted = nullptr;
        bool projected = true;
        std::uint8_t alphaThreshold = receiver.alphaThreshold; // the mesh's, unless a mask lowers the alpha
        if (verdict == ShelterMap::Verdict::OPEN) {
            wanted = ProjectedVertexData::shared(receiver.shape);
        } else if (verdict == ShelterMap::Verdict::SHELTERED) {
            // No snow, so nothing for vertex colors to tint and nothing for alpha to mask: the
            // model's own data, baked shading and all
            projected = false;
            ProjectedVertexData::addRef(receiver.shape.source);
            wanted = receiver.shape.source;
        } else {
            // One value per vertex of the shape as it will be drawn: the model's, then the added
            const std::size_t total = openness.size();
            values.resize(total);
            for (std::size_t index = 0; index < total; ++index) {
                const float gone = goneAlpha(facing[index]);
                values[index] = static_cast<std::uint8_t>(((gone + ((1.0F - gone) * openness[index])) * FULL) + 0.5F);
            }
            if (receiver.alphaTest) {
                // Such a shape paints no alpha (all 1), so the lowest value is the lowest alpha it gets
                alphaThreshold = scaledAlphaThreshold(receiver.alphaThreshold, *std::ranges::min_element(values));
            }
            const std::uint64_t fingerprint = refinement.has_value()
                ? ProjectedVertexData::fingerprintRefined(values, *refinement)
                : ProjectedVertexData::fingerprint(values);
            if (receiver.projected && fingerprint == receiver.currentFingerprint
                && alphaThreshold == receiver.currentAlphaThreshold) {
                continue; // what it already has
            }
            if (refinement.has_value()) {
                wanted = ProjectedVertexData::customRefined(receiver.shape, *refinement, values);
                if (wanted != nullptr) {
                    s_refinedBuilds.fetch_add(1, std::memory_order_relaxed);
                    s_refinedVertices.fetch_add(refinement->added.size(), std::memory_order_relaxed);
                    s_refinedTriangles.fetch_add((refinement->indices.size() / 3) - receiver.shape.triangleCount,
                                                 std::memory_order_relaxed);
                } else {
                    // The refined buffer could not be had (the budget, or a failed creation): the
                    // mask on the model's own vertices is still the better part of the fix
                    wanted = ProjectedVertexData::custom(receiver.shape, std::span {values}.first(vertexCount));
                }
            } else {
                wanted = ProjectedVertexData::custom(receiver.shape, values);
            }
            if (wanted == nullptr) {
                wanted = ProjectedVertexData::shared(receiver.shape); // over budget
                alphaThreshold = receiver.alphaThreshold;
            }
        }

        if (wanted == nullptr) {
            continue; // no vertex data could be built for it
        }
        if (wanted == receiver.current && projected == receiver.projected
            && alphaThreshold == receiver.currentAlphaThreshold) {
            ProjectedVertexData::release(wanted);
            continue; // keeps what it has
        }
        result.swaps.push_back({.shape = receiver.keepAlive,
                                .data = wanted,
                                .projected = projected,
                                .isSnow = receiver.isSnow,
                                .alphaThreshold = receiver.alphaTest ? std::optional {alphaThreshold} : std::nullopt});
        receiver.current = wanted;
        receiver.projected = projected;
        receiver.currentFingerprint = ProjectedVertexData::fingerprintOf(wanted);
        receiver.currentAlphaThreshold = alphaThreshold;
    }

    result.receivers = std::move(job.receivers);
    return result;
}
