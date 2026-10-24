#include "WaterMask.hpp"

#include "ShelterMap.hpp"

#include "PCH.h"

#include <algorithm>
#include <cstddef>
#include <span>

using namespace XPMF;

auto WaterMask::Water::surfaceAt(const RE::NiPoint3& point) const -> float
{
    // The level of the cell the point stands in - the nearest of the block's, for a point beyond
    // it - or the placed water over it, whichever lies higher
    const int slotX = std::clamp(ShelterMap::cellOf(point.x) - placed.centerX + 1, 0, ShelterMap::K_BLOCK - 1);
    const int slotY = std::clamp(ShelterMap::cellOf(point.y) - placed.centerY + 1, 0, ShelterMap::K_BLOCK - 1);
    const float level = levels.at(static_cast<std::size_t>((slotY * ShelterMap::K_BLOCK) + slotX));
    return std::max(level, placed.heightAt(point.x, point.y));
}

auto WaterMask::apply(const Water& water,
                      std::span<const RE::NiPoint3> positions,
                      float edgeOpenness,
                      float edgeThrow,
                      std::span<float> openness) -> bool
{
    if (positions.empty() || openness.size() != positions.size()) {
        return false;
    }
    // The fade bent to the material like the shelter's, and begun half its distance above the
    // surface, so that the projection is visibly half gone at the surface itself - or at the
    // height the wave lifts it to there: the surface, swung up by up to the throw, and eased onto
    // the surface from a quarter of the throw on where the wave would go under it
    const ShelterMap::Fade fade = ShelterMap::Fade::of(K_FADE, edgeOpenness);
    constexpr float HALF = 0.5F;
    constexpr float KNEE = 0.25F;
    bool wet = false;
    for (std::size_t index = 0; index < positions.size(); ++index) {
        const RE::NiPoint3& position = positions[index];
        const float lift
            = edgeThrow > 0.0F ? ShelterMap::swungEdge(position, 0.0F, edgeThrow, KNEE * edgeThrow, 0.0F) : 0.0F;
        const float left = fade.at(water.surfaceAt(position) + lift + (HALF * K_FADE) - position.z);
        wet = wet || left < 1.0F;
        openness[index] = std::min(openness[index], left);
    }
    return wet;
}
