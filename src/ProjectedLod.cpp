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
#include <optional>
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
    const auto label = [](const Entry& entry) -> std::string {
        return entry.profile != nullptr ? entry.profile->label() : std::string {"?"};
    };
    for (auto& entry : entries) {
        if (entry.identifier.empty() || entry.material == nullptr) {
            continue;
        }
        if (std::ranges::find(K_GAME_IDENTIFIERS, entry.identifier) != K_GAME_IDENTIFIERS.end()) {
            spdlog::info("Object LOD: {} is the game's own identifier, so the game projects those shapes itself and "
                         "profile '{}' has nothing to do for them",
                         entry.identifier,
                         label(entry));
            continue;
        }
        const auto game = std::ranges::find_if(K_GAME_IDENTIFIERS, [&entry](const char* const identifier) -> bool {
            return nameOf(identifier, entry.identifier).has_value();
        });
        if (game != K_GAME_IDENTIFIERS.end()) {
            spdlog::warn("Object LOD: profile '{}' has the identifier {}, which is one of the game's own shape names "
                         "({} with HD or a dash after it), and the game keeps those, so the profile projects no LOD "
                         "until it is given another lodIdentifier",
                         label(entry),
                         entry.identifier,
                         *game);
            continue;
        }
        if (entry.materialHD == nullptr) {
            entry.materialHD = entry.material;
        }
        const auto claimed = std::ranges::find_if(s_candidates, [&entry](const Registered* const registered) -> bool {
            return registered->entry.identifier == entry.identifier;
        });
        if (claimed != s_candidates.end()) {
            // A profile and its pbr twin name the same records under the same identifier
            const Entry& first = (*claimed)->entry;
            if (first.material == entry.material && first.materialHD == entry.materialHD) {
                spdlog::info("Object LOD: profile '{}' names {} as well, with the same material objects as profile "
                             "'{}', which stands for it",
                             label(entry),
                             entry.identifier,
                             label(first));
            } else {
                spdlog::warn("Object LOD: profile '{}' claims {} as well, with other material objects; the first "
                             "claim, by profile '{}', stands",
                             label(entry),
                             entry.identifier,
                             label(first));
            }
            continue;
        }
        Registered& registered = s_registered.emplace_back(std::move(entry));
        s_candidates.push_back(&registered);
        spdlog::info("Object LOD: shapes named {} get material object {} and shapes named {} get {} (profile '{}'), "
                     "either name with a dash and more after it or not, projected at {:g} degrees, {:g} for the HD "
                     "name",
                     registered.entry.identifier,
                     describe(*registered.entry.material),
                     registered.entry.identifier + K_HD_SUFFIX,
                     describe(*registered.entry.materialHD),
                     label(registered.entry),
                     angleOf(false),
                     angleOf(true));
    }
    if (s_candidates.empty()) {
        spdlog::info("Object LOD: no profile names a LOD material of its own, so only the game's own objsnow and "
                     "objash shapes are projected");
        return;
    }
    // A name that is two identifiers' (objmosshd-largeref: objmoss's HD name, objmosshd's plain
    // one) goes to the longer, so that one has to be tried first
    std::ranges::sort(s_candidates, [](const Registered* const left, const Registered* const right) -> bool {
        const std::string& one = left->entry.identifier;
        const std::string& other = right->entry.identifier;
        return one.size() != other.size() ? one.size() > other.size() : one < other;
    });
    for (const Registered* const longer : s_candidates) {
        for (const Registered* const shorter : s_candidates) {
            if (shorter == longer || !nameOf(shorter->entry.identifier, longer->entry.identifier).has_value()) {
                continue;
            }
            spdlog::info("Object LOD: {} is a name of {}'s as well, and the longer identifier counts, so shapes "
                         "named {} go to profile '{}', not profile '{}'",
                         longer->entry.identifier,
                         shorter->entry.identifier,
                         longer->entry.identifier,
                         label(longer->entry),
                         label(shorter->entry));
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
    // The game's own names are Prepare's, whatever a profile's identifier says
    for (const char* const game : K_GAME_IDENTIFIERS) {
        if (nameOf(game, name).has_value()) {
            return;
        }
    }
    Registered* registered = nullptr;
    bool hd = false;
    for (Registered* const candidate : s_candidates) { // longest identifier first
        if (const auto match = nameOf(candidate->entry.identifier, name); match.has_value()) {
            registered = candidate;
            hd = *match;
            break;
        }
    }
    if (registered == nullptr) {
        return;
    }

    // What Prepare does for the game's four: the record's values as they stand, the HD material
    // and angle for the HD name
    const RE::BGSMaterialObject& material = hd ? *registered->entry.materialHD : *registered->entry.material;
    const auto& data = material.directionalData;
    constexpr float DEGREES = 0.017453292F; /**< What the engine multiplies the angle by */
    const float noiseScale = data.noiseUVScale > 0.0F ? data.noiseUVScale : 1.0F;
    const RE::NiColorA params {data.falloffScale, data.falloffBias, 1.0F / noiseScale, std::cos(angleOf(hd) * DEGREES)};
    const bool snow = data.flags.any(RE::BSMaterialObject::DIRECTIONAL_DATA::Flag::kSnow);
    if (!shape.SetProjectedUVData(params, data.singlePassColor, snow)) {
        return;
    }
    std::atomic<bool>& logged = hd ? registered->loggedHD : registered->logged;
    if (!logged.exchange(true)) {
        spdlog::info("Object LOD: '{}' is the first shape to get {} of profile '{}'",
                     shape.name.c_str(),
                     hd ? registered->entry.identifier + K_HD_SUFFIX : registered->entry.identifier,
                     registered->entry.profile != nullptr ? registered->entry.profile->label() : "?");
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

auto ProjectedLod::nameOf(std::string_view identifier,
                          std::string_view name) -> std::optional<bool>
{
    if (!name.starts_with(identifier)) {
        return std::nullopt;
    }
    std::string_view rest = name.substr(identifier.size());
    const bool hd = rest.starts_with(K_HD_SUFFIX);
    if (hd) {
        rest.remove_prefix(std::string_view {K_HD_SUFFIX}.size());
    }
    if (rest.empty() || rest.front() == K_SEPARATOR) {
        return hd;
    }
    return std::nullopt;
}
