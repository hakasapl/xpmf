#pragma once

#include "ConfigLoader.hpp"

#include "PCH.h"

#include <compare>
#include <cstdint>
#include <string>
#include <string_view>

namespace XPMF {

/**
 * @brief Decides which profile, if any, a material object (MATO) belongs to
 *
 * Nothing in the record says what a material is of, which is why this needs a class of its own.
 * What a MATO offers, checked against every record in Skyrim.esm, Update.esm and the three DLCs:
 *
 *  - The "Snow" flag (DATA offset 0x30, DIRECTIONAL_DATA::flags). The obvious candidate, and
 *    wrong in both directions. Update.esm re-saves every Skyrim.esm material with the flag
 *    set, including IceShader01 (839 statics), both Falmer glow shaders, the Markarth and
 *    tundra moss and the volcanic sulfur. Dragonborn's records meanwhile use the older 48
 *    byte DATA that ends before the flag, the reader (up to 0x34 bytes, whatever the record
 *    holds) leaves the constructor's 0 in place, and so DLC2SnowMaterialLakeSurface - real
 *    snow - loads without it, as does any material from a plugin converted from Skyrim LE.
 *    To the engine the flag is a shading hint and nothing more: Clone3D forwards it as
 *    "isSnow", which sets the Snow shader flag (SLSF2 bit 28) next to Projected_UV, and that
 *    flag only ever puts a shape through the improved snow technique (rim light, sparkle, the
 *    subsurface pass), which IniSettings holds off; with it off the flag changes nothing but
 *    the distance an env map fades at.
 *
 *  - The model (MODL). Only the Creation Kit opens it. At runtime a MATO's shader properties
 *    are deserialized from its DNAM blobs, and the single pass snow, ash and moss materials all
 *    name the same placeholder, ShaderTests\ShaderBox.nif.
 *
 *  - The single pass color. Snow is a bluish grey (0.42, 0.46, 0.49) and Solstheim ash a
 *    neutral one (0.28, 0.28, 0.28): close enough that any retexture-matched snow color
 *    overlaps, so no threshold separates them safely.
 *
 *  - The EditorID. Every vanilla and DLC snow material contains "Snow" (SnowMaterial*,
 *    SnowLODMaterial*, DLC1SnowMaterial*, DLC2SnowMaterial*) and every ash material starts with
 *    "AshMaterial", "DLC2AshMaterial" or "AshLODMaterial" (AshMaterialSolstheim*1P,
 *    DLC2AshMaterialDusting1P, AshLODMaterialMtns1P), no other material does, and mods that add
 *    such materials follow the same convention since that is how their authors and every xEdit
 *    script tell them apart too - as does po3's Tweaks, whose Dynamic Snow Material tweak applies
 *    this very test. The game discards the string; Tweaks keeps it (see EditorIdLookup). For a
 *    single pass material this is the only identity there is: the record is a color and three
 *    falloff numbers.
 *
 * Hence the rule. A profile matches a material when one of its "editorIds" patterns matches the
 * EditorID, none of its "excludeEditorIds" patterns does, and - for a "pbr" profile - Community
 * Shaders' True PBR has a configuration for the material (PbrMaterialObjects). Patterns are case
 * insensitive, * stands for any run of characters and ? for any one; a pattern without either is
 * an exact EditorID. They had better be anchored where a word is short: "*ash*" is also in
 * splash, trash and wash, and "*ashmaterial*" still in SplashMaterial. The same patterns name
 * the statics a profile's skip lists (neutralizeVertexColorsSkip, neutralizeVertexAlphaSkip,
 * roofShelterSkip) are about, by their own EditorIDs (see ProjectedGeometry).
 *
 * Of the profiles that match, the material belongs to the most specific one: the one whose
 * matching pattern has the most literal characters ("SnowMaterialFarm" over "SnowMaterial*" over
 * "*snow*"; with as many literals, the one with fewer wildcards), then a "pbr" profile over one
 * for any material (it matches a subset), then the first file name in alphabetical order (the
 * order ConfigLoader hands the profiles out in).
 */
class MaterialClassifier {
public:
    MaterialClassifier() = delete;

    /**
     * @brief How a verdict came about
     */
    enum class Reason : std::uint8_t {
        EDITOR_ID, /**< The profile's pattern matched */
        LOD_MATERIAL, /**< Named as a profile's LOD material (lodMaterial, lodMaterialHD), which beats any pattern */
        EXCLUDED, /**< A profile's patterns matched, but so did one of its exclusions, and no other profile took it */
        NOT_PBR, /**< A profile's patterns matched, but it is a pbr profile and True PBR has no configuration for the
                    material, and no other profile took it */
        NONE /**< No profile has a pattern for it */
    };

    /**
     * @brief The outcome for one material, with what it was based on for the log
     */
    struct Verdict {
        const ConfigLoader::Profile* profile {}; /**< The profile the material belongs to; nullptr for none */
        Reason reason {Reason::NONE}; /**< How that came about */
        std::string pattern; /**< The pattern that decided it (lower case), for the log */
        std::string editorId; /**< EditorID the material was loaded with; empty when Tweaks has none on record */
        bool pbr {}; /**< Whether True PBR has a configuration for it */
    };

    /**
     * @brief How specific a pattern is: how much of the EditorID it pins down, and how little it leaves open
     */
    struct Specificity {
        int literals {}; /**< Characters that are not wildcards; more is more specific */
        int wildcards {}; /**< * and ?; with as many literals, fewer is more specific */

        [[nodiscard]] auto operator<=>(const Specificity& other) const -> std::strong_ordering
        {
            if (literals != other.literals) {
                return literals <=> other.literals;
            }
            return other.wildcards <=> wildcards;
        }
        [[nodiscard]] auto operator==(const Specificity& other) const -> bool = default;
    };

    /**
     * @brief Classifies a material object
     *
     * @param material The material to look at
     * @return Verdict See the class description for the rule
     */
    [[nodiscard]] static auto classify(const RE::BGSMaterialObject& material) -> Verdict;

    /**
     * @brief Whether a material renders in a single pass
     *
     * A single pass material is blended into the object's own draw as a flat color times the
     * projected diffuse; a multipass one draws the object a second time with a shader property
     * and textures of its own. Only the former is this plugin's business: there is nothing
     * projected about the latter, and turning one into the other changes far more than a color.
     *
     * @param material The material to look at
     * @return bool True when the record's Single Pass flag is set
     */
    [[nodiscard]] static auto isSinglePass(const RE::BGSMaterialObject& material) -> bool;

    /**
     * @brief Short text for a reason, for the log
     */
    [[nodiscard]] static auto describe(Reason reason) -> std::string_view;

    /**
     * @brief Whether a lower case text matches a lower case wildcard pattern (* and ?) from end to end
     */
    [[nodiscard]] static auto matches(std::string_view pattern,
                                      std::string_view text) -> bool;

    /**
     * @brief How specific a pattern is
     */
    [[nodiscard]] static auto specificity(std::string_view pattern) -> Specificity;
};

} // namespace XPMF
