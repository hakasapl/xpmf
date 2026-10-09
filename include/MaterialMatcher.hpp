#pragma once

#include "ConfigLoader.hpp"

#include "PCH.h"

#include <optional>
#include <string>

namespace XPMF {

/**
 * @brief Makes every single pass material object (MATO) of a profile show the profile's textures
 * exactly - snow the landscape's snow, ash the landscape's ash
 *
 * How the engine uses a MATO (1.7.99 offsets; 1.5.97 and 1.6.1170 behave the same):
 *
 *  - TESBoundObject::Clone3D (0x277E20) applies one only when the base form is a STAT, from
 *    TESObjectSTAT::data.materialObj. For a single pass material it reads exactly six values:
 *    falloffScale, falloffBias, noiseUVScale, singlePassColor, the snow flag, and the static's
 *    own max angle. They go to every BSLightingShaderProperty of the clone (0x1517C60): shader
 *    flags Projected_UV and, with the snow flag, Snow are set, projectedUVParams becomes
 *    (scale, bias, 1 / noiseUVScale, cos(angle)) and projectedUVColor the color. materialUVScale,
 *    the projection direction, the normal dampener and the DNAM shader properties only matter
 *    to the multipass path, which builds a second piece of geometry instead.
 *
 *  - BSLightingShader::SetupGeometry (0x15492F0, constants written by 0x154C3A0) turns that into
 *        ProjectedUVParams  = ((1 - cos) * scale, -, 1 / noiseUVScale, (1 - cos) * bias + cos)
 *        ProjectedUVParams2 = the color, untouched
 *        ProjectedUVParams3 = (diffuse tiling, detail tiling, 0, bEnableProjecteUVDiffuseNormals)
 *    and binds four global textures from textures\effects: ProjectedNoise, ProjectedDiffuse,
 *    ProjectedNormal and ProjectedNormalDetail.
 *
 *  - The pixel shader covers a pixel when
 *        dot(normal, up) * vertexAlpha > cos + (1 - cos) * (bias + scale * noise)
 *    which is why scale, bias and noise scale decide how much of an object is covered - the
 *    vanilla "Light" materials (bias up to 0.82) are a dusting, the standard ones (0.4) a
 *    blanket. The covered pixel's albedo is
 *        ProjectedDiffuse * color     with bEnableProjecteUVDiffuseNormals (Prefs, default on)
 *        color                        without
 *    and from there on it is lit exactly like any other albedo, landscape included.
 *
 *  - Object LOD never looks at a STAT. The "objsnow" / "objsnowHD" LOD shapes take the same six
 *    values (0x505970) from SnowLODMaterial / SnowLODMaterialHD, which the engine fetches as
 *    default objects - at an index that moved between AE builds (331 in CommonLib's tables,
 *    320 in 1.7.99), so they are best found by name like every other material. A profile's
 *    lodMaterial and lodMaterialHD go the same way onto the LOD shapes named after them
 *    (ProjectedLod), which is this plugin's way of adding to the four.
 *
 * A single pass material has no texture of its own - but the shader gives it one: every covered
 * pixel samples ProjectedDiffuse, a texture the engine loads once from a hardcoded path and binds
 * for every projected UV draw, whatever was projected. One texture for snow, ash and moss alike
 * is why vanilla materials carry a color at all, and why none of them looks like the ground next
 * to it. So instead of matching a color to the landscape texture through the game's
 * ProjectedDiffuse, the landscape texture is made the projected diffuse of the patched materials
 * - and of those only: ProjectedTextures substitutes it (and whichever of the normal map, the
 * coverage noise and the detail normal the profile names) draw by draw for shapes whose
 * projection color carries the tag this class gives patched materials, a mark in the color's
 * lowest bits that names the profile's set of textures. A covered pixel is then the landscape
 * texture itself, its average and its detail, whatever retexture is installed; the game's
 * ProjectedDiffuse.dds plays no part in how a patched material looks, and every other material -
 * moss, material objects Community Shaders' True PBR has configurations for - is exactly what it
 * was, textures included. A profile that names no diffuse leaves the color as the record has it,
 * marked; its other textures still go with the draws.
 *
 * Two fallbacks. With bEnableProjecteUVDiffuseNormals off (which IniSettings rules out: it turns
 * the setting on right before this class runs) the shader samples no diffuse and uses the color
 * as is, so a material gets mean(its texture) - still tagged, since the coverage noise is sampled
 * regardless and the profile's may go with the draws. And should the texture not load as a
 * renderer texture, there is nothing to tag: the game's average is divided out of the color
 * instead, color = mean(texture) / mean(ProjectedDiffuse), and the game's textures stay.
 *
 * Tiling goes with the coverage: the shader derives the projected diffuse's coordinates from the
 * coverage noise's, scaled by fProjectedUVDiffuseNormalTilingScale, and the noise's scale is the
 * material's own noiseUVScale (vanilla ones run from 20 to 1500) - so a texture at another scale
 * takes another noise scale, which a profile can give its materials (noiseUVScale, below).
 *
 * The values are written into the existing materials rather than into new forms that statics get
 * pointed at. Rendering cannot tell the difference - Clone3D reads the same values off whichever
 * form it is handed - but everything else can: the forms keep their FormID and EditorID, so po3's
 * Tweaks still recognizes a snowy static for its Dynamic Snow Material tweak (it tests the
 * EditorID of the static's MATO), a material that another plugin assigns to a static after this
 * one ran still matches, and the LOD materials above are covered by the same loop. Each material
 * also keeps its own scale, bias and noise scale unless the profile gives one (falloffScale,
 * falloffBias, noiseUVScale: the three of the record's values the single pass path reads, so the
 * only three there is anything to override), so nothing gains or loses cover that the profile did
 * not ask for; only the color changes (the snow flag stays as the record has it). The fourth
 * value the path reads, the max angle, is the static's rather than the material's (its DNAM), so
 * a profile's maxAngle is written into the statics that carry its patched materials instead.
 *
 * Multipass materials are left alone altogether. They render a second piece of geometry with a
 * texture set of their own, nothing about them is projected, and none of the plugin's three parts
 * applies to the statics that carry them.
 */
class MaterialMatcher {
public:
    MaterialMatcher() = delete;

    /**
     * @brief Sorts the load order's material objects into profiles and patches each profile's
     *
     * Runs at kDataLoaded: every plugin's materials exist by then, and no cell has been
     * attached yet, so no static has been cloned with the old values.
     */
    static void onDataLoaded();

private:
    constexpr static const char* PROJECTED_DIFFUSE_SETTING
        = "bEnableProjecteUVDiffuseNormals:Display"; /**< Spelled as the engine spells it */
    constexpr static float MIN_DIVISOR = 1.0F / 255.0F; /**< One 8 bit step: a texture channel averaging less
                                                           than this renders black whatever the color is */
    constexpr static float VANILLA_MEAN_NOISE = 0.2F; /**< Average of the vanilla ProjectedNoise (0.208), for
                                                         when the one in the load order cannot be decoded */

    /**
     * @brief What matching a profile came to
     */
    struct Match {
        std::optional<std::size_t> set; /**< The profile's set of projected textures, when it has one */
        std::optional<RE::NiColor> color; /**< The color its materials get instead of their own: white when
                                             the diffuse is replaced by texture, its average when the shader
                                             cannot sample one; std::nullopt keeps each record's own */
        bool ownNoise {}; /**< Whether their draws sample the profile's coverage noise rather than the game's */
    };

    /**
     * @brief Makes a profile's textures what its materials show
     *
     * @return std::optional<Match> std::nullopt when nothing can be done for it - a diffuse it
     *         names could not be read at all, or no set of textures could be added (already
     *         logged) - in which case its materials are left as they are
     */
    [[nodiscard]] static auto matchTextures(const ConfigLoader::Profile& profile) -> std::optional<Match>;

    /**
     * @brief Average of the coverage noise as the shader reads it (its red channel)
     *
     * Where a partly sheltered surface visibly loses its projection depends on it (see
     * ProjectedGeometry), and the noise texture is whatever the load order or a profile says.
     *
     * @param dataPath The noise texture; empty for the game's
     */
    [[nodiscard]] static auto meanNoise(const std::string& dataPath) -> float;

    /**
     * @brief Whether the shader multiplies the single pass color with ProjectedDiffuse
     *
     * @return bool The game's bEnableProjecteUVDiffuseNormals; true (its default) when the
     *         setting cannot be found
     */
    [[nodiscard]] static auto isProjectedDiffuseEnabled() -> bool;
};

} // namespace XPMF
