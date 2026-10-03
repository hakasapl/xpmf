#include "MaterialMatcher.hpp"

#include "ConfigLoader.hpp"
#include "EditorIdLookup.hpp"
#include "MaterialClassifier.hpp"
#include "PbrMaterialObjects.hpp"
#include "ProjectedGeometry.hpp"
#include "ProjectedTextures.hpp"
#include "SeasonsOfSkyrim.hpp"
#include "TextureColor.hpp"

#include "PCH.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstddef>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace XPMF;

namespace {

/**
 * @brief "EditorID [FormID, last plugin to touch it]" for the log
 */
auto describeForm(const RE::TESForm& form,
                  std::string_view editorId) -> std::string
{
    const auto* const file = form.GetFile();
    return std::format("{} [{:08X} {}]",
                       editorId.empty() ? "(no EditorID)" : editorId,
                       form.GetFormID(),
                       file != nullptr ? file->GetFilename() : "?");
}

} // namespace

void MaterialMatcher::onDataLoaded()
{
    if (!ConfigLoader::isAnyMaterialPatched() && !ConfigLoader::isAnyGeometryChanged()
        && !ConfigLoader::isAnySpecularChanged()) {
        spdlog::info("No profile names a texture or a material value, or has neutralizeVertexColors, "
                     "neutralizeVertexAlpha, roofShelter or specularMult on: nothing to do");
        return;
    }
    if (!EditorIdLookup::isAvailable()) {
        spdlog::error("po3's Tweaks (po3_Tweaks.dll) is not loaded. It is required - its EditorID cache is how "
                      "material objects are told apart - so nothing was changed");
        return;
    }

    auto* const dataHandler = RE::TESDataHandler::GetSingleton();
    if (dataHandler == nullptr) {
        spdlog::error("No data handler at kDataLoaded; nothing was changed");
        return;
    }

    // Material objects Community Shaders' True PBR has a configuration for are not this plugin's
    PbrMaterialObjects::load();

    // How many statics carry each material: for the log only, so a report shows at a glance what
    // a verdict below is worth
    std::unordered_map<const RE::BGSMaterialObject*, std::size_t> usage;
    for (const auto* const stat : dataHandler->GetFormArray<RE::TESObjectSTAT>()) {
        if (stat != nullptr && stat->data.materialObj != nullptr) {
            ++usage[stat->data.materialObj];
        }
    }

    // Verdicts first: without a single material to work on there is no reason to load textures
    struct Candidate {
        RE::BGSMaterialObject* material {};
        std::string editorId;
        bool pbr {}; /**< True PBR has a configuration for it */
    };
    std::unordered_map<const ConfigLoader::Profile*, std::vector<Candidate>> byProfile;
    std::size_t named = 0;
    std::size_t total = 0;
    const RE::BGSMaterialObject* winterSnow = nullptr;
    for (auto* const material : dataHandler->GetFormArray<RE::BGSMaterialObject>()) {
        if (material == nullptr) {
            continue;
        }
        ++total;

        auto verdict = MaterialClassifier::classify(*material);
        if (!verdict.editorId.empty()) {
            ++named;
        }
        // Seasons of Skyrim projects this one's values onto clones by itself, which no static's
        // material says; ProjectedGeometry has to know which record that is
        if (SeasonsOfSkyrim::isSinglePassMaterial(verdict.editorId)) {
            winterSnow = material;
        }
        spdlog::info("{}: {}, {} statics - {} ({}{}){}",
                     describeForm(*material, verdict.editorId),
                     MaterialClassifier::isSinglePass(*material) ? "single pass" : "multipass",
                     usage[material],
                     verdict.profile != nullptr ? std::format("profile '{}'", verdict.profile->label()) : "no profile",
                     MaterialClassifier::describe(verdict.reason),
                     verdict.pattern.empty() ? "" : " " + verdict.pattern,
                     verdict.pbr ? " [True PBR configuration]" : "");
        if (verdict.profile != nullptr) {
            byProfile[verdict.profile].push_back(
                {.material = material, .editorId = std::move(verdict.editorId), .pbr = verdict.pbr});
        }
    }

    if (total > 0 && named == 0) {
        spdlog::error("None of the {} material objects has an EditorID on record, so \"Load EditorIDs\" "
                      "(bLoadEditorIDs under [Fixes] in po3_Tweaks.ini) must be off. Turn it back on; nothing was "
                      "changed",
                      total);
        return;
    }
    if (byProfile.empty()) {
        spdlog::warn("None of the {} material objects belongs to a profile; nothing was changed", total);
        return;
    }

    // The game's own coverage noise is what every material without a noise of its own is drawn with.
    // Only roof shelter cares what it averages
    const bool shelter = ConfigLoader::isAnyRoofSheltered();
    const float gameNoise = shelter ? meanNoise({}) : VANILLA_MEAN_NOISE;

    ProjectedGeometry::Materials materials;
    for (const auto& profile : ConfigLoader::getProfiles()) {
        const auto found = byProfile.find(&profile);
        if (found == byProfile.end()) {
            spdlog::info("Profile '{}': no material object matches it", profile.label());
            continue;
        }

        // Whose material is it? A multipass one is nobody's: nothing about it is projected. One
        // with a True PBR configuration stays exactly as its author left it unless the profile is
        // for PBR materials only (its textures are PBR ones then, and PBR's shader takes the color
        // and the projected textures like vanilla's does), as does every one of a profile that
        // patches nothing - but the shapes under either still carry a projection, which is what
        // the vertex color and roof shelter parts work on.
        // Whether the profile has anything for the records at all: a texture, a falloff value or
        // a max angle
        const bool patches = profile.patchesMaterial();
        std::vector<const Candidate*> ours;
        std::vector<const Candidate*> untouched;
        std::size_t leftMultipass = 0;
        std::size_t leftPbr = 0;
        for (const auto& candidate : found->second) {
            if (!MaterialClassifier::isSinglePass(*candidate.material)) {
                ++leftMultipass;
                continue;
            }
            const bool handsOff = candidate.pbr && !profile.pbr;
            if (patches && handsOff) {
                ++leftPbr;
                spdlog::info("{}: has a True PBR configuration and profile '{}' is not for PBR materials, so the "
                             "record is left untouched; its shapes still get the profile's vertex colors and roof "
                             "shelter",
                             describeForm(*candidate.material, candidate.editorId),
                             profile.label());
            }
            (patches && !handsOff ? ours : untouched).push_back(&candidate);
        }

        // Without a material to patch there is no texture to load either
        std::optional<Match> match;
        if (!ours.empty()) {
            match = matchTextures(profile);
            if (!match.has_value()) {
                spdlog::error("Profile '{}': its {} material objects stay as they are", profile.label(), ours.size());
                untouched.insert(untouched.end(), ours.begin(), ours.end());
                ours.clear();
            }
        }
        // Where a snow line lies under a roof depends on the average of the noise its draws sample
        const float noiseAverage
            = profile.roofShelter && match.has_value() && match->ownNoise ? meanNoise(profile.noiseTexture) : gameNoise;

        std::size_t statics = 0;
        for (const auto* const candidate : ours) {
            auto* const material = candidate->material;
            auto& data = material->directionalData;

            // The color: the profile's where its diffuse supplies the look, the record's own
            // where the game's diffuse stays - tagged either way when there is a set of textures
            // to name (see ProjectedTextures)
            const RE::NiColor before = data.singlePassColor;
            RE::NiColor color = match->color.value_or(before);
            if (match->set.has_value()) {
                color = ProjectedTextures::tag(*match->set, color);
            }
            spdlog::info("{}: ({:.4f}, {:.4f}, {:.4f}) -> {}",
                         describeForm(*material, candidate->editorId),
                         before.red,
                         before.green,
                         before.blue,
                         match->color.has_value() ? "the profile's color" : "its own color, tagged");
            data.singlePassColor = color;

            // Scale, bias and noise scale: the profile's where it gives one, the material's own
            // where not - which keeps its coverage what it was. The noise scale takes the
            // projected textures' tiling with it (see the class)
            if (profile.overridesFalloff()) {
                const auto take = [](float& field, const std::optional<float>& given) -> std::string {
                    const float before = field;
                    field = given.value_or(before);
                    return given.has_value() ? std::format("{:g} -> {:g}", before, field)
                                             : std::format("{:g} (its own)", before);
                };
                const std::string scale = take(data.falloffScale, profile.falloffScale);
                const std::string bias = take(data.falloffBias, profile.falloffBias);
                const std::string noiseScale = take(data.noiseUVScale, profile.noiseUVScale);
                spdlog::info("{}: falloff scale {}, falloff bias {}, noise UV scale {}",
                             describeForm(*material, candidate->editorId),
                             scale,
                             bias,
                             noiseScale);
            }

            materials.emplace(material, ProjectedGeometry::Treatment {.profile = &profile, .meanNoise = noiseAverage});
            statics += usage[material];
        }
        for (const auto* const candidate : untouched) {
            materials.emplace(
                candidate->material,
                ProjectedGeometry::Treatment {.profile = &profile, .meanNoise = gameNoise, .untouched = true});
        }

        spdlog::info("Profile '{}': patched {} material objects used by {} statics (diffuse {}); {} single pass left "
                     "as they are ({} of them for having a True PBR configuration), {} multipass ignored",
                     profile.label(),
                     ours.size(),
                     statics,
                     profile.diffuseTexture.empty() ? "not replaced" : profile.diffuseTexture.c_str(),
                     untouched.size(),
                     leftPbr,
                     leftMultipass);
    }

    // The max angle is the static's rather than the material's (DNAM), and Clone3D reads it from
    // the static: a profile that gives one has it written into every static that carries one of its
    // patched materials
    std::unordered_map<const RE::BGSMaterialObject*, const ConfigLoader::Profile*> angled;
    for (const auto& [material, treatment] : materials) {
        if (!treatment.untouched && treatment.profile->maxAngle.has_value()) {
            angled.emplace(material, treatment.profile);
        }
    }
    if (!angled.empty()) {
        struct Angled {
            std::size_t statics {};
            float lowest {}; /**< The angles the statics had */
            float highest {};
        };
        std::unordered_map<const ConfigLoader::Profile*, Angled> report;
        for (auto* const stat : dataHandler->GetFormArray<RE::TESObjectSTAT>()) {
            const auto found = stat != nullptr ? angled.find(stat->data.materialObj) : angled.end();
            if (found == angled.end()) {
                continue;
            }
            auto& tally = report[found->second];
            const float before = stat->data.materialThresholdAngle;
            tally.lowest = tally.statics == 0 ? before : std::min(tally.lowest, before);
            tally.highest = tally.statics == 0 ? before : std::max(tally.highest, before);
            ++tally.statics;
            stat->data.materialThresholdAngle = *found->second->maxAngle;
        }
        for (const auto& profile : ConfigLoader::getProfiles()) {
            if (!profile.maxAngle.has_value()) {
                continue;
            }
            if (const auto found = report.find(&profile); found != report.end()) {
                spdlog::info("Profile '{}': max angle {:g} on {} statics (their own ran from {:g} to {:g})",
                             profile.label(),
                             *profile.maxAngle,
                             found->second.statics,
                             found->second.lowest,
                             found->second.highest);
            } else {
                spdlog::info("Profile '{}': max angle {:g} reaches no static", profile.label(), *profile.maxAngle);
            }
        }
    }

    // Everything above changed what a projection looks like; this changes which vertices it looks
    // like that on (vertex colors and roof shelter, see ProjectedGeometry)
    ProjectedTextures::activate();
    ProjectedGeometry::onMaterialsReady(std::move(materials), winterSnow);
}

auto MaterialMatcher::matchTextures(const ConfigLoader::Profile& profile) -> std::optional<Match>
{
    const RE::NiColor white {1.0F, 1.0F, 1.0F};
    const bool replacesDiffuse = !profile.diffuseTexture.empty();
    const bool sampled = isProjectedDiffuseEnabled();
    const auto logged = [&](const Match& match) -> void {
        if (match.color.has_value()) {
            spdlog::info("Profile '{}': single pass color ({:.4f}, {:.4f}, {:.4f}){}",
                         profile.label(),
                         match.color->red,
                         match.color->green,
                         match.color->blue,
                         match.set.has_value() ? ", tagged" : "");
        } else {
            spdlog::info("Profile '{}': its materials keep their own single pass colors{}",
                         profile.label(),
                         match.set.has_value() ? ", tagged" : "");
        }
    };

    // The textures themselves, through a color that says whose draw it is: white where the
    // profile's diffuse supplies the look (its average, where the shader is set to sample none),
    // the record's own where the game's diffuse stays
    if (const auto added = ProjectedTextures::add({.diffuse = profile.diffuseTexture,
                                                   .normal = profile.normalTexture,
                                                   .noise = profile.noiseTexture,
                                                   .detailNormal = profile.detailNormalTexture,
                                                   .pbr = profile.pbr});
        added.has_value()) {
        Match match {.set = added->set, .ownNoise = added->ownNoise};
        if (replacesDiffuse && sampled) {
            match.color = white;
        } else if (replacesDiffuse) {
            // The shader samples no diffuse with the setting off, so the color has to carry the
            // texture: its average
            const auto mean = TextureColor::meanColor(profile.diffuseTexture);
            if (!mean.has_value()) {
                spdlog::error(
                    "Profile '{}': without {} there is nothing to match", profile.label(), profile.diffuseTexture);
                return std::nullopt;
            }
            spdlog::info("Profile '{}': {} is off, so the shader samples no projected diffuse and its materials get "
                         "the texture's average; its normal map cannot apply",
                         profile.label(),
                         PROJECTED_DIFFUSE_SETTING);
            match.color = mean;
        }
        logged(match);
        return match;
    }
    if (!replacesDiffuse) {
        return std::nullopt; // no room for another set: nothing can be done for the profile
    }

    // The diffuse did not load as a renderer texture (or no set could be added for it). From here
    // on the color has to carry it, which leaves nothing to tag: the profile's draws cannot be
    // told apart, and its other textures stay the game's
    const auto mean = TextureColor::meanColor(profile.diffuseTexture);
    if (!mean.has_value()) {
        spdlog::error("Profile '{}': without {} there is nothing to match", profile.label(), profile.diffuseTexture);
        return std::nullopt;
    }
    Match match {.color = *mean};
    if (sampled) {
        // The shader multiplies the color with the game's diffuse, so that one's average has to
        // come out again: per channel ratio of the two averages, a channel the divisor has next
        // to nothing in left alone. No upper clamp: a dark ProjectedDiffuse legitimately needs a
        // color above 1 to land on the texture, and the product is what ends up on screen
        if (const auto game = TextureColor::meanColor(ConfigLoader::GAME_DIFFUSE); game.has_value()) {
            const auto divide
                = [](float channel, float by) -> float { return by >= MIN_DIVISOR ? channel / by : channel; };
            match.color = RE::NiColor {
                divide(mean->red, game->red), divide(mean->green, game->green), divide(mean->blue, game->blue)};
            spdlog::warn("Profile '{}': falling back to a color matched through the game's {}",
                         profile.label(),
                         ConfigLoader::GAME_DIFFUSE);
        } else {
            spdlog::warn("Profile '{}': neither texture could be used; its materials get the texture's average "
                         "uncompensated",
                         profile.label());
        }
    }
    logged(match);
    return match;
}

auto MaterialMatcher::meanNoise(const std::string& dataPath) -> float
{
    const auto mean = TextureColor::meanColor(dataPath.empty() ? ConfigLoader::GAME_NOISE : dataPath);
    if (!mean.has_value()) {
        spdlog::warn("...so roof shelter assumes the vanilla noise's average, {}", VANILLA_MEAN_NOISE);
        return VANILLA_MEAN_NOISE;
    }
    spdlog::info(
        "Coverage noise {} averages {:.3f}", dataPath.empty() ? ConfigLoader::GAME_NOISE : dataPath.c_str(), mean->red);
    return mean->red;
}

auto MaterialMatcher::isProjectedDiffuseEnabled() -> bool
{
    const auto* const setting = RE::GetINISetting(PROJECTED_DIFFUSE_SETTING);
    return setting == nullptr || setting->GetBool();
}
