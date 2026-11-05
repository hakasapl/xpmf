#pragma once

#include "PCH.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace XPMF {

/**
 * @brief Gives the patched material objects projected textures of their own, profile by profile
 *
 * A single pass material has no textures, the shader lends it four: coverage comes from "the"
 * projected noise, every covered pixel samples "the" projected diffuse, and "the" projected normal
 * and detail normal are blended in. Those textures look global - the engine loads them once, from
 * hardcoded paths under textures\effects, into BSGraphics::State - but they are bound per draw:
 * BSLightingShader::SetupGeometry reads the pointers and puts the textures into pixel shader
 * slots (noise 11, diffuse 3, normal 8, detail normal 10) for every geometry whose technique has
 * PROJECTED_UV, through the renderer's state cache, and nothing else at render time reads them
 * (the normal has one more reader, the sparkle snow material setup, which this leaves alone). So
 * a draw can be handed different textures by pointing State at them for the length of that one
 * call: the engine does the binding, the caching and the deciding (no diffuse or normals with
 * the Prefs setting off or in a cube map pass) exactly as it always did, just with other textures
 * in its hands, and the next projected draw finds the game's where they always were.
 *
 * Those four are all there is. A single pass material object has no texture set, the record's
 * model is never loaded at runtime, and nothing else in the lighting shader's projected UV block
 * samples anything. A set substitutes the ones a profile names and leaves the rest the game's.
 *
 * Which draws, and which textures? Those of shapes that got their projection from a material this
 * plugin patched, and the set of that material's profile - and a shape does not remember its
 * material, but it carries the material's color, copied bit for bit into
 * BSLightingShaderProperty::projectedUVColor by both code paths that apply a material
 * (TESBoundObject::Clone3D and the object LOD builder). So the color carries the set: the lowest
 * bits of its floats' mantissas are overwritten with a signature in red and green and the set's
 * number in blue (tag). Twelve bits of a 23 bit mantissa move a channel by less than one part in
 * two thousand - a fifth of an 8 bit step, invisible - and no arithmetic on colors produces the
 * signature (one chance in 2^24 for an untagged color). Whatever the color is: a material whose
 * profile replaces the diffuse gets white (the texture supplies the color; its average, when the
 * shader is set to sample none), and one whose profile leaves the diffuse alone keeps its own
 * color and just carries the tag. No bookkeeping, no lifetimes, LOD included.
 *
 * Everything else - moss, another mod's materials, the material objects Community Shaders' True
 * PBR has a configuration for and no profile is written for - keeps sampling the game's textures,
 * because nothing about them or about the engine's pointers was changed.
 *
 * One thing is different about a set for PBR materials: the diffuse. Community Shaders' PBR path
 * expects a base color texture in an sRGB format, which the GPU decodes to linear on sampling -
 * its Diffuse() does no decode of its own under TRUE_PBR - while its projected block, written for
 * the game's non-sRGB ProjectedDiffuse.dds, decodes the projected diffuse in the shader
 * (ColorToLinear). A PBR albedo handed to it as loaded is decoded twice: darker and more
 * saturated than the same texture on the landscape next to it, which on snow reads as blue. So
 * for such a set the diffuse is loaded a second time as its non-sRGB counterpart - same pixels,
 * no decode on sampling - and the shader's decode is the only one, as it is for the landscape.
 * With linear lighting off the same holds: the projected block then decodes nothing, and the PBR
 * base color is put back into sRGB space by Diffuse(). Nothing about the file changes.
 *
 * The same call decides how the textures are tiled. For every projected draw SetupGeometry copies
 * two Prefs settings into ProjectedUVParams3, as they are: fProjectedUVDiffuseNormalTilingScale,
 * diffuse and normal map tiles per tile of the coverage noise, and fProjectedUVNormalDetailTilingScale,
 * the detail normal's. Every single pass projected pixel shader there is - the 804 variants in the
 * game's shader package and Community Shaders' alike - samples the noise at its coordinates times
 * ProjectedUVParams.z (1 / the material's noise UV scale), the diffuse and normal map at those
 * times the first setting and the detail normal at those times the second; the multipass ones
 * read neither setting. So a set that tiles a texture its own way (materialUVScale,
 * detailUVScale) has the setting say, for the length of the call, whatever puts the texture at
 * the set's world units per tile on this draw's noise scale - and the noise keeps its own. The
 * engine reads the values in that one place only, and they are the game's again before the call
 * returns, like the textures.
 *
 * Community Shaders hooks the same virtual (vtable write, chained). This hook is installed at
 * plugin load, theirs at kPostPostLoad, so theirs wraps this one; neither writes what the other
 * reads. ENB sees ordinary D3D calls coming out of the engine's own state flush.
 */
class ProjectedTextures {
public:
    ProjectedTextures() = delete;

    /**
     * @brief The textures of one profile, relative to Data; an empty path leaves the game's in place
     */
    struct Paths {
        std::string diffuse;
        std::string normal;
        std::string noise;
        std::string detailNormal;
        bool pbr {}; /**< The diffuse is a PBR base color: an sRGB format is projected as its non-sRGB counterpart */
        auto operator==(const Paths&) const -> bool = default;

        /**
         * @brief Whether no texture is named at all
         */
        [[nodiscard]] auto namesNothing() const -> bool
        {
            return diffuse.empty() && normal.empty() && noise.empty() && detailNormal.empty();
        }
    };

    /**
     * @brief How one profile tiles the projected textures, in world units per tile; std::nullopt
     * leaves the game's ratio to the noise
     */
    struct Tiling {
        std::optional<float> material; /**< The diffuse and normal map (materialUVScale) */
        std::optional<float> detail; /**< The detail normal (detailUVScale) */
        auto operator==(const Tiling&) const -> bool = default;

        /**
         * @brief Whether no tiling is given at all
         */
        [[nodiscard]] auto namesNothing() const -> bool { return !material.has_value() && !detail.has_value(); }
    };

    /**
     * @brief What add() made of a set of paths
     */
    struct Added {
        std::optional<std::size_t> set; /**< The set the paths became, for tag(); std::nullopt when nothing at all
                                           is substituted (no path named or none of them loaded, and no tiling) */
        bool ownNoise {}; /**< Whether draws of the set sample a coverage noise other than the game's */
    };

    /**
     * @brief Installs the SetupGeometry hook; SKSE load callback. Does nothing until activate()
     */
    static void install();

    /**
     * @brief Loads a set of textures
     *
     * The way the engine loads its own (BSShaderManager::GetTexture, demand load), so loose files,
     * archives and MO2's VFS all work. Before activate() only.
     *
     * @param paths What to project. A diffuse that fails to load fails the set; any of the other
     *        three that does is logged and left to the game's texture
     * @param tiling How to tile it: a set of its own even where no texture is named
     * @return std::optional<Added> std::nullopt when the diffuse did not load as a renderer texture
     *         or there is no room for another set
     */
    [[nodiscard]] static auto add(const Paths& paths,
                                  const Tiling& tiling) -> std::optional<Added>;

    /**
     * @brief A color marked as belonging to a set: what a material of the set's profile is given
     *
     * @param set From add()
     * @param color The color to mark: white (or the texture's average) when the set replaces the
     *        diffuse, the material's own otherwise. Each channel moves by less than one part in
     *        two thousand
     */
    [[nodiscard]] static auto tag(std::size_t set,
                                  const RE::NiColor& color) -> RE::NiColor;

    /**
     * @brief Lets the hook loose on the sets added so far; no set can be added afterwards
     */
    static void activate();

private:
    using TexturePointer = RE::NiPointer<RE::NiSourceTexture>;

    /**
     * @brief Vtable hook on BSLightingShader::SetupGeometry: swaps the textures in around the call
     */
    struct SetupGeometryHook {
        static void thunk(RE::BSLightingShader* shader,
                          RE::BSRenderPass* pass,
                          std::uint32_t renderFlags);
        static inline REL::Relocation<decltype(thunk)> s_func; /**< Whatever occupied the slot before */
        constexpr static std::size_t SLOT = 0x6; /**< BSShader::SetupGeometry(BSRenderPass*, uint32) */
    };

    /**
     * @brief One profile's textures, as holders that are never destroyed; nullptr = the game's stays
     *
     * Holders rather than plain smart pointers, for two reasons. They are swapped with State's
     * pointers around the engine call - pointer for pointer, no reference counting on the render
     * path - so between the two swaps a holder owns the game's texture, not ours. And they live
     * on the heap for the life of the process, like the engine's own projected textures: a
     * static's destructor would release a texture at exit, when the renderer may already be gone.
     */
    struct Set {
        Paths paths; /**< What it was loaded from: two profiles with the same textures and tiling share a set */
        Tiling tiling;
        TexturePointer* diffuse {};
        TexturePointer* normal {};
        TexturePointer* noise {};
        TexturePointer* detailNormal {};
    };

    constexpr static const char* DIFFUSE_TILING_SETTING
        = "fProjectedUVDiffuseNormalTilingScale:Display"; /**< ProjectedUVParams3.x: diffuse and normal map tiles per
                                                             noise tile (0.5 by default) */
    constexpr static const char* DETAIL_TILING_SETTING
        = "fProjectedUVNormalDetailTilingScale:Display"; /**< ProjectedUVParams3.y: the detail normal's (1.2) */

    constexpr static std::uint32_t MARK_MASK = 0xFFFU; /**< The twelve lowest mantissa bits of red and green hold
                                                          the signature... */
    constexpr static std::uint32_t RED_MARK = 0xA5CU; /**< ...two patterns no arithmetic on colors lands on... */
    constexpr static std::uint32_t GREEN_MARK = 0x35AU;
    constexpr static std::uint32_t SET_MASK = 0xFFU; /**< ...and the eight lowest of blue the set's number + 1 */
    constexpr static std::size_t MAX_SETS = 255; /**< What fits into that byte */
    constexpr static std::size_t NOT_TAGGED = MAX_SETS; /**< setOf()'s answer for a color without a tag */

    constexpr static std::uint32_t LIGHTING_TECHNIQUE_START = 0x4800002DU; /**< BSRenderPass::passEnum of
                                                                              lighting technique 0 */
    constexpr static std::uint32_t LIGHTING_TYPE_SHIFT = 24; /**< The technique's top bits are its type */
    constexpr static std::uint32_t LIGHTING_TYPE_MASK = 0x3F; /**< Six bits of it */

    /**
     * @brief The set a shape's projection color names, or NOT_TAGGED
     */
    [[nodiscard]] static auto setOf(const RE::NiColorA& color) -> std::size_t;

    /**
     * @brief Whether a lighting shader type reads the projected diffuse and normal slots as such
     *
     * The slots are shared: face tint (FACEGEN) and the parallax height map (PARALLAX) live in the
     * diffuse's, the inner layer of MULTI_LAYER_PARALLAX in the normal's, and the pixel shader
     * samples a projected diffuse and normal only when it is none of those nor SPARKLE. The engine
     * binds its textures there regardless (only hair is exempt); this hook stays out of those
     * types so that whatever they sample in those slots is what they sampled before. The noise's
     * and the detail normal's slots are shared with nothing a projected draw could be.
     */
    [[nodiscard]] static auto samplesProjectedTextures(std::uint32_t lightingType) -> bool;

    /**
     * @brief Demand loads a texture and wraps it in a holder that is never destroyed
     *
     * @return TexturePointer* nullptr when it did not load as a renderer texture
     */
    [[nodiscard]] static auto hold(const std::string& dataPath) -> TexturePointer*;

    /**
     * @brief hold() for the diffuse of a PBR set: an sRGB format texture comes back as a source
     * texture of this plugin's own holding the same pixels in the non-sRGB counterpart format
     *
     * Anything else - not sRGB, unreadable, no device - comes back as hold() would have it, with a
     * warning where the projection may not match the landscape for it.
     */
    [[nodiscard]] static auto holdForPbr(const std::string& dataPath) -> TexturePointer*;

    /**
     * @brief The Prefs setting under a name, if it is there and holds a float; logged when not
     */
    [[nodiscard]] static auto floatSetting(const char* name) -> RE::Setting*;

    /**
     * @brief What a tiling setting has to say for a draw: texture tiles per tile of the noise
     *
     * @param worldUnits World units per tile of the texture, if the set gives any
     * @param noiseFrequency The draw's ProjectedUVParams.z: noise tiles per world unit
     * @return std::optional<float> std::nullopt where the game's setting stays: no tiling given, or
     *         no finite value above 0 to be had
     */
    [[nodiscard]] static auto tilesPerNoiseTile(const std::optional<float>& worldUnits,
                                                float noiseFrequency) -> std::optional<float>;

    static inline std::array<Set, MAX_SETS> s_sets; /**< Written before s_active, read only after */
    static inline std::size_t s_count = 0; /**< Ditto */
    static inline RE::Setting* s_diffuseTiling {}; /**< Ditto; nullptr unless a set tiles the diffuse its own way */
    static inline RE::Setting* s_detailTiling {}; /**< Ditto, for the detail normal */
    static inline std::atomic<bool> s_active {false}; /**< Gates the hook until the sets are final */
};

} // namespace XPMF
