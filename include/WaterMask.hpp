#pragma once

#include "ShelterMap.hpp"

#include "PCH.h"

#include <array>
#include <span>

namespace XPMF {

/**
 * @brief Keeps a projection off what lies under water (a profile's waterFade)
 *
 * It reaches the shader the way the roof shelter does, through vertex alpha, and it is openness
 * in the roof shelter's sense - 1 where the projection stays, 0 where it is gone, the fade in
 * between bent to the material (ShelterMap::Fade) so that the projection is visibly half gone
 * half way through it - so that ProjectedGeometry takes, per vertex, the lesser of the two and
 * maps that onto alpha the way it always has. It is on or off by itself, whatever the roof
 * shelter and the other settings say. And it is a function of a vertex's world position alone:
 * the copies of a vertex on either side of a uv seam, or in two shapes that meet, get the same
 * value, so no seam shows where none was.
 *
 * The projection fades out across a water surface, over K_FADE centered on it: visibly half gone
 * at the surface itself - which is where the eye puts the edge - and gone altogether half the
 * fade below it, while a hand above the surface (the top of an ice floe) it is as it was. The
 * surfaces are the cell's own water level (the sea, a lake that fills a whole cell) and the
 * placed water planes - rivers, ponds, streams, the activators with a water type - which the cell
 * pass rasterizes from their triangles into height layers, the way it does roofs; where several
 * lie over a point, the highest counts.
 *
 * Like the roof shelter's edge, the water's waves along it rather than running dead level (the
 * profile's waterFadeRandomness): the same noise over the ground and the same easing
 * (ShelterMap::swungEdge), lifting the fade by up to the throw - and only lifting it. Where a wave
 * would take the edge under the surface it eases down onto it instead, so what lies under water
 * stays bare; splash and spray reach up unevenly, never down.
 */
class WaterMask {
public:
    WaterMask() = delete;

    constexpr static float K_FADE = 24.0F; /**< World units over which the projection fades out at a water surface,
                                              half above it and half below */

    /**
     * @brief The water over the 3x3 cells around a cell; immutable and safe to read on the worker
     */
    struct Water {
        using Levels = std::array<float, ShelterMap::K_BLOCK_CELLS>;

        ShelterMap::Field placed; /**< The tops of the placed water planes */
        Levels levels = noLevels(); /**< Each cell's own water level, row major like the maps;
                                       ShelterMap::K_NOTHING where it has none */

        /**
         * @brief The highest water surface over a world space point; ShelterMap::K_NOTHING where there is none
         */
        [[nodiscard]] auto surfaceAt(const RE::NiPoint3& point) const -> float;

        [[nodiscard]] constexpr static auto noLevels() -> Levels
        {
            Levels levels {};
            levels.fill(ShelterMap::K_NOTHING);
            return levels;
        }
    };

    /**
     * @brief Lowers a shape's openness to what the water leaves each vertex
     *
     * @param water The water around the shape's cell
     * @param positions World space positions of its vertices, any added ones included
     * @param edgeOpenness Openness at which the projection visibly ends on the shape's material
     * @param edgeThrow The profile's waterFadeRandomness: world units the fade is lifted by at most
     * @param openness In: one value per position (1 where nothing else covers it); out: each the
     *        lesser of what it was and what the water leaves
     * @return bool Whether any vertex reaches into the water, as far as the fade does
     */
    static auto apply(const Water& water,
                      std::span<const RE::NiPoint3> positions,
                      float edgeOpenness,
                      float edgeThrow,
                      std::span<float> openness) -> bool;
};

} // namespace XPMF
