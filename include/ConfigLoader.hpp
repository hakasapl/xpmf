#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace XPMF {

/**
 * @brief Loads and serves the plugin configuration from Data/SKSE/Plugins/XPMF
 *
 * The configuration is a set of profiles. A profile names the material objects it is about -
 * wildcard patterns over their EditorIDs - and carries every setting there is for them: the
 * textures their projection shows, whether vertex colors may tint it, whether roofs keep it
 * out. Snow and ash are the two the plugin ships with; they fall from the sky alike and look
 * nothing alike, which is the whole reason a profile is the unit of configuration.
 *
 * One profile per file, every *.json in the folder XPMF next to the DLL
 * (snow.json, ash.json, and whatever anyone adds). A folder of files rather than one file with
 * a list in it so that a mod can bring a profile for its own materials without overwriting
 * anyone else's - in a mod manager the files of different mods simply end up side by side. A
 * material object that several profiles match belongs to the one that matches it most
 * specifically (MaterialClassifier: the pattern with the most literal characters, a pbr
 * profile over a general one), and among equals to the first file name in alphabetical order -
 * which is how an added profile takes a few materials out of a shipped one's hands: name them.
 *
 * JSON rather than the INI this started as: a profile holds lists, which [General] key=value
 * lines do not express. What JSON cannot hold is an explanation, so the settings are documented
 * in the README rather than in the files.
 *
 * Everything is read once at plugin load (loadConfig) into statics; the getters are plain
 * accessors and never touch the disk. Only "name" and "editorIds" have to be there; every other
 * setting has a default - the shipped snow profile's, except that a texture not named is not
 * replaced and a Snow flag or a falloff value not given is the record's - so a profile can be
 * three lines long. What is there is validated strictly: a field has to have its type (and its
 * range), and a file with any problem at all is rejected as a whole, with every reason in one
 * error in the log - half a profile is not something anyone asked for. Keys that are not settings
 * only earn a warning; "comment" (a note) and "$schema" (an editor's pointer to the JSON schema
 * in the repository, schema/profile.schema.json, which describes every setting) do not even
 * that. Without the folder the built-in profiles (ash, snow) apply; with it, exactly the valid
 * files in it do - deleting ash.json is how ash is left alone.
 */
class ConfigLoader {
public:
    ConfigLoader() = delete;

    /**
     * @brief One profile: which material objects, and what is done for them
     *
     * The settings fall into the plugin's three independent parts - patching the material (the
     * four textures, isSnow, the three falloff values and maxAngle, each applied when given),
     * neutralizeVertexColors, and the vertex alpha (neutralizeVertexAlpha, roofShelter with
     * shelterFade) - and any combination
     * of them works. Each of the three geometry settings comes with a skip list: wildcard patterns
     * over the EditorIDs of the statics (base records) it is not applied to. A fourth, specularMult,
     * scales the specular strength of the shapes the projection is on.
     */
    struct Profile {
        std::string name; /**< For the log */
        std::string file; /**< The file it came from, lower case, for the log; empty for a built-in profile */
        std::vector<std::string> editorIds; /**< Lower case wildcard patterns (* and ?); a material object whose
                                               EditorID matches one belongs to the profile... */
        std::vector<std::string> excludeEditorIds; /**< ...unless it matches one of these as well; none by default */
        bool pbr {}; /**< ...and, when set, only if Community Shaders' True PBR has a configuration for it. The
                        one kind of profile that patches such a material object's record: its textures are
                        taken to be made for PBR */

        std::string diffuseTexture; /**< Data relative, lower case, backslashed path of the texture the projection
                                       shows; empty = not replaced, the game's ProjectedDiffuse stays */
        std::string normalTexture; /**< Same for its normal map; empty = the game's ProjectedNormal stays */
        std::string noiseTexture; /**< Same for the coverage noise; empty = the game's ProjectedNoise stays */
        std::string detailNormalTexture; /**< Same for the detail normal; empty = the game's ProjectedNormalDetail
                                            stays */
        std::optional<bool> isSnow; /**< The material objects' Snow flag; std::nullopt (null in the file, or the
                                       key left out) = as the record has it */

        // The three of a material object's values the engine's single pass path reads besides the
        // color and the Snow flag (see MaterialMatcher); the projection covers a pixel where
        // dot(normal, up) * vertex alpha > cos(max angle) + (1 - cos) * (bias + scale * noise).
        // std::nullopt (null in the file, or the key left out) = as each record has it
        std::optional<float> falloffScale; /**< How far the coverage noise raises what a surface has to face up by:
                                              the patches the projection is missing from. 0.25 to 0.5 on the vanilla
                                              snow materials */
        std::optional<float> falloffBias; /**< What a surface has to face up by before the noise: how much of it is
                                             covered at all. 0.4 on the vanilla snow materials, up to 0.82 on the
                                             "Light" ones that are a dusting */
        std::optional<float> noiseUVScale; /**< World units per tile of the coverage noise - and, the shader tiling
                                              the projected textures at a fixed ratio to it, of the profile's
                                              textures. 20 to 1500 on the vanilla materials; above 0, the engine
                                              divides by it */
        std::optional<float> maxAngle; /**< The one value the path reads that is the static's rather than the
                                          material's: the DNAM max angle, in degrees, of every static carrying one
                                          of the profile's patched material objects; the projection needs
                                          dot(normal, up) above cos(angle) before the noise has its say. 30 to 120
                                          in vanilla (the Creation Kit's "30-90" is a hint: Nordic ruins and word
                                          walls sit at 120); std::nullopt = each static's own */

        bool neutralizeVertexColors {}; /**< Whether shapes that carry the projection get white vertex colors */
        std::vector<std::string> neutralizeVertexColorsSkip; /**< Lower case wildcard patterns (* and ?) over the
                                                                EditorIDs of the statics whose shapes keep the
                                                                mesh's colors all the same; none by default */

        // Vertex alpha scales the projection (see ProjectedVertexData): what the shapes start from,
        // and whether the roof shelter then multiplies it down
        bool neutralizeVertexAlpha {}; /**< Whether shapes that carry the projection start from a vertex alpha of 1
                                          - a mask the mesh's author painted against the game's own projection is
                                          discarded - rather than from the mesh's own. Never on a shape whose alpha
                                          is transparency */
        std::vector<std::string> neutralizeVertexAlphaSkip; /**< Same, for the statics whose shapes keep the
                                                               mesh's alpha */
        bool roofShelter {}; /**< Whether vertex alpha is multiplied down to keep the projection out from under
                                cover */
        std::vector<std::string> roofShelterSkip; /**< Same, for the statics whose shapes stay covered under a
                                                     roof */
        float shelterFade {}; /**< World units over which it fades out under cover */
        std::optional<float> specularMult; /**< What the specular strength of every shape the projection is on
                                              is multiplied by, on a copy of the shape's material: 0 takes the
                                              highlight off, 1 leaves it. Without the Snow flag the shader lights
                                              covered pixels with the mesh's own specular, which is how a glossy
                                              mesh makes glossy snow; with the flag it swaps in the snow rim
                                              light instead (bEnableSnowRimLighting). Not applied to Community
                                              Shaders' PBR materials, whose roughness scale lives in that field.
                                              std::nullopt = as the mesh has it */

        /**
         * @brief The name with the file it came from: two files may well share a name
         */
        [[nodiscard]] auto label() const -> std::string { return file.empty() ? name : name + " (" + file + ")"; }

        /**
         * @brief Whether the profile gives its material objects any falloff value of its own
         */
        [[nodiscard]] auto overridesFalloff() const -> bool
        {
            return falloffScale.has_value() || falloffBias.has_value() || noiseUVScale.has_value();
        }

        /**
         * @brief Whether the profile has anything for its material objects (or their statics) at
         * all: a texture named, or any of the material values given
         */
        [[nodiscard]] auto patchesMaterial() const -> bool
        {
            return !diffuseTexture.empty() || !normalTexture.empty() || !noiseTexture.empty()
                || !detailNormalTexture.empty() || isSnow.has_value() || overridesFalloff() || maxAngle.has_value();
        }
    };

    /**
     * @brief The engine's own projected textures: a profile that names one of them asks for no
     * substitution, the same as not naming a texture, and they are what every draw without a
     * profile's set samples
     */
    constexpr static const char* GAME_DIFFUSE = R"(textures\effects\projecteddiffuse.dds)";
    constexpr static const char* GAME_NORMAL = R"(textures\effects\projectednormal.dds)";
    constexpr static const char* GAME_NOISE = R"(textures\effects\projectednoise.dds)";
    constexpr static const char* GAME_DETAIL_NORMAL = R"(textures\effects\projectednormaldetail.dds)";

    /**
     * @brief Loads the profiles in the XPMF folder
     */
    static void loadConfig();

    /**
     * @brief Get the profiles, in file name order - the order that breaks ties between them
     *
     * @return const std::vector<Profile>& The folder's profiles, or the built-in ones (ash, snow)
     *         when there is no folder. Stable for the life of the process: other classes keep
     *         pointers into it
     */
    [[nodiscard]] static auto getProfiles() -> const std::vector<Profile>&;

    /**
     * @brief Whether any profile patches materials, i.e. whether the draw hook is needed
     */
    [[nodiscard]] static auto isAnyMaterialPatched() -> bool;

    /**
     * @brief Whether any profile changes vertex colors or alpha, i.e. whether the Clone3D hook is needed
     */
    [[nodiscard]] static auto isAnyGeometryChanged() -> bool;

    /**
     * @brief Whether any profile scales specular, i.e. whether the Clone3D hook is needed for that alone
     */
    [[nodiscard]] static auto isAnySpecularChanged() -> bool;

    /**
     * @brief Whether any profile keeps its projection out from under roofs, i.e. whether height maps are needed
     */
    [[nodiscard]] static auto isAnyRoofSheltered() -> bool;

    /**
     * @brief Turns whatever the user wrote for a texture into a resource system path
     *
     * @param raw UTF-8; any slashes, any case, with or without Data\ / textures\ / .dds
     * @return std::string Relative to Data, lower case, backslashes, under textures\, ending in
     *         .dds; empty for a blank value
     */
    [[nodiscard]] static auto normalizeTexturePath(std::string_view raw) -> std::string;

private:
    //
    // DEFAULT CFG VALUES
    //
    constexpr static const char* DEFAULT_SNOW_DIFFUSE = R"(textures\landscape\snow01.dds)"; /**< LSnow01, the vanilla
                                                                                            snow ground */
    constexpr static const char* DEFAULT_SNOW_NORMAL = R"(textures\landscape\snow01_n.dds)";
    constexpr static const char* DEFAULT_SNOW_PATTERN = "*snow*"; /**< Every vanilla and DLC snow material has it in
                                                                     its EditorID, and no other material does */
    constexpr static const char* DEFAULT_ASH_DIFFUSE
        = R"(textures\dlc02\landscape\volcanic_ash_01.dds)"; /**< LVolcanicAsh01, the ground of southern
                                                                Solstheim */
    constexpr static const char* DEFAULT_ASH_NORMAL = R"(textures\dlc02\landscape\volcanic_ash_01_n.dds)";
    constexpr static const char* DEFAULT_ASH_PATTERN_MATERIAL
        = "ashmaterial*"; /**< AshMaterialSolstheim1P, ...Light1P, ...Mtns1P. Anchored at the start: "*ash*" is
                             also in splash, trash and wash, and "*ashmaterial*" still in SplashMaterial */
    constexpr static const char* DEFAULT_ASH_PATTERN_DLC = "dlc2ashmaterial*"; /**< DLC2AshMaterialDusting1P */
    constexpr static const char* DEFAULT_ASH_PATTERN_LOD = "ashlodmaterial*"; /**< AshLODMaterialMtns1P */

    // What a profile that leaves a setting out gets: the shipped snow profile's values
    constexpr static bool DEFAULT_PBR = false;
    constexpr static bool DEFAULT_NEUTRALIZE_VERTEX_COLORS = true;
    constexpr static bool DEFAULT_NEUTRALIZE_VERTEX_ALPHA = true; /**< The roof shelter decides what lies under
                                                                     cover; a mask painted for the game's own
                                                                     projection is in its way */
    constexpr static bool DEFAULT_ROOF_SHELTER = true;
    constexpr static float DEFAULT_SHELTER_FADE = 64.0F; /**< About how far wind carries snow in under an eave */
    constexpr static double MAX_ANGLE_LIMIT = 180.0; /**< cos(angle) is what the shader compares with; at 180 every
                                                        face is covered and past it there is nothing to say */
    constexpr static double MAX_SPECULAR_MULT = 10.0; /**< Ten times a mesh's own specular is as far as it goes */
    constexpr static float MAX_SHELTER_FADE = 128.0F; /**< The shelter mask has been checked over 0 to 128 (0 a hard
                                                        edge, 32 and 96 in game); every covered vertex searches
                                                        this far for open sky, and past it the open vertices the
                                                        mask may thin lie farther from an eave than looks right */

    static inline std::vector<Profile> s_profiles; /**< In file name order */

    /**
     * @brief The profiles the plugin ships with, snow and ash, for an installation without the folder
     */
    [[nodiscard]] static auto builtInProfiles() -> std::vector<Profile>;
};

} // namespace XPMF
