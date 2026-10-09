#include "ProjectedLod.hpp"

#include "Text.hpp"

#include "PCH.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace XPMF;

void ProjectedLod::install()
{
    REL::Relocation<std::uintptr_t> vtable {RE::VTABLE_BSSubIndexTriShape.front()};
    PostLinkObjectHook::s_func = vtable.write_vfunc(PostLinkObjectHook::SLOT, PostLinkObjectHook::thunk);
    spdlog::info("Object LOD hook installed (BSSubIndexTriShape::PostLinkObject)");
}

void ProjectedLod::onMaterialsReady(std::vector<Entry> entries)
{
    if (s_ready.load(std::memory_order_acquire)) {
        spdlog::error("Object LOD identifiers were handed in twice; the second set was ignored");
        return;
    }
    s_lodAngle = RE::GetINISetting(K_LOD_ANGLE);
    s_hdLodAngle = RE::GetINISetting(K_HD_LOD_ANGLE);
    if (s_lodAngle == nullptr || s_hdLodAngle == nullptr) {
        spdlog::warn("The LOD snow threshold angle settings were not found; object LOD is projected at {:g} degrees, "
                     "{:g} on HD shapes, as the engine's defaults have it",
                     DEFAULT_LOD_ANGLE,
                     DEFAULT_HD_LOD_ANGLE);
    }
    const auto describe = [](const RE::BGSMaterialObject& material) -> std::string {
        const auto* const file = material.GetFile();
        return std::format("[{:08X} {}]", material.GetFormID(), file != nullptr ? file->GetFilename() : "?");
    };
    const auto owner = [](const Candidate& candidate) -> std::string {
        if (candidate.registered == nullptr) {
            return "the game";
        }
        const auto* const profile = candidate.registered->entry.profile;
        return std::format("profile '{}'", profile != nullptr ? profile->label() : std::string {"?"});
    };
    // The game's own first, so that a name beginning with either is left to Prepare
    for (const char* const identifier : K_GAME_IDENTIFIERS) {
        s_candidates.push_back({.identifier = identifier, .registered = nullptr});
    }
    for (auto& entry : entries) {
        if (entry.identifier.empty() || entry.material == nullptr) {
            continue;
        }
        const std::string label = entry.profile != nullptr ? entry.profile->label() : std::string {"?"};
        if (std::ranges::find(K_GAME_IDENTIFIERS, entry.identifier) != K_GAME_IDENTIFIERS.end()) {
            spdlog::info("Object LOD: {} is the game's own identifier, so the game projects those shapes itself and "
                         "profile '{}' has nothing to do for them",
                         entry.identifier,
                         label);
            continue;
        }
        const auto claimed = std::ranges::find(s_candidates, entry.identifier, &Candidate::identifier);
        if (claimed != s_candidates.end()) {
            spdlog::warn("Object LOD: profile '{}' claims {} as well; the first claim, by {}, stands",
                         label,
                         entry.identifier,
                         owner(*claimed));
            continue;
        }
        if (entry.materialHD == nullptr) {
            entry.materialHD = entry.material;
        }
        Registered& registered = s_registered.emplace_back(std::move(entry));
        s_candidates.push_back({.identifier = registered.entry.identifier, .registered = &registered});
        spdlog::info("Object LOD: shapes named {} get material object {} and shapes named {} get {} (profile '{}'), "
                     "whatever the generator appends to the name, projected at {:g} degrees, {:g} for the HD name",
                     registered.entry.identifier,
                     describe(*registered.entry.material),
                     registered.entry.identifier + K_HD_SUFFIX,
                     describe(*registered.entry.materialHD),
                     label,
                     angleOf(false),
                     angleOf(true));
    }
    if (s_registered.empty()) {
        spdlog::info("Object LOD: no profile names a LOD material of its own, so only the game's own objsnow and "
                     "objash shapes are projected");
        return;
    }
    // Only the beginning of a shape's name is read, so of two identifiers a name begins with the
    // longer one has to be tried first
    std::ranges::sort(s_candidates, [](const Candidate& left, const Candidate& right) -> bool {
        if (left.identifier.size() != right.identifier.size()) {
            return left.identifier.size() > right.identifier.size();
        }
        return left.identifier < right.identifier;
    });
    for (const auto& longer : s_candidates) {
        for (const auto& shorter : s_candidates) {
            if (longer.identifier.size() > shorter.identifier.size()
                && longer.identifier.starts_with(shorter.identifier)) {
                spdlog::info("Object LOD: {} begins with {}; the longer identifier counts, so shapes named {}... go "
                             "to {}, not {}",
                             longer.identifier,
                             shorter.identifier,
                             longer.identifier,
                             owner(longer),
                             owner(shorter));
            }
        }
    }
    s_ready.store(true, std::memory_order_release);
}

void ProjectedLod::PostLinkObjectHook::thunk(RE::BSSubIndexTriShape* shape,
                                             RE::NiStream& stream)
{
    s_func(shape, stream);
    if (shape != nullptr && s_ready.load(std::memory_order_acquire)) {
        onShapeLinked(*shape);
    }
}

void ProjectedLod::onShapeLinked(RE::BSSubIndexTriShape& shape)
{
    if (shape.name.empty()) {
        return;
    }
    const std::string name = Text::toLower(shape.name.c_str());
    const auto found = std::ranges::find_if(
        s_candidates, [&name](const Candidate& candidate) -> bool { return name.starts_with(candidate.identifier); });
    if (found == s_candidates.end() || found->registered == nullptr) {
        return; // no profile's, or the game's own: Prepare's
    }
    Registered& registered = *found->registered;
    // objMoss, objMossHD, objMoss-LargeRef, objMossHD-LargeRef: HD right after the identifier
    // means the HD material and angle, and what follows either is not read
    const bool hd = std::string_view {name}.substr(found->identifier.size()).starts_with(K_HD_SUFFIX);

    // What Prepare does for the game's four: the record's values as they stand, the HD material
    // and angle for the HD name
    const RE::BGSMaterialObject& material = hd ? *registered.entry.materialHD : *registered.entry.material;
    const auto& data = material.directionalData;
    constexpr float DEGREES = 0.017453292F; /**< What the engine multiplies the angle by */
    const float noiseScale = data.noiseUVScale > 0.0F ? data.noiseUVScale : 1.0F;
    const RE::NiColorA params {data.falloffScale, data.falloffBias, 1.0F / noiseScale, std::cos(angleOf(hd) * DEGREES)};
    const bool snow = data.flags.any(RE::BSMaterialObject::DIRECTIONAL_DATA::Flag::kSnow);
    if (!shape.SetProjectedUVData(params, data.singlePassColor, snow)) {
        return;
    }
    std::atomic<bool>& logged = hd ? registered.loggedHD : registered.logged;
    if (!logged.exchange(true)) {
        spdlog::info("Object LOD: '{}' is the first shape to get {} of profile '{}'",
                     shape.name.c_str(),
                     hd ? registered.entry.identifier + K_HD_SUFFIX : registered.entry.identifier,
                     registered.entry.profile != nullptr ? registered.entry.profile->label() : "?");
    }
}

auto ProjectedLod::angleOf(bool hd) -> float
{
    const RE::Setting* const setting = hd ? s_hdLodAngle : s_lodAngle;
    if (setting != nullptr) {
        return setting->GetFloat();
    }
    return hd ? DEFAULT_HD_LOD_ANGLE : DEFAULT_LOD_ANGLE;
}
