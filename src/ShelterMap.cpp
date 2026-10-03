#include "ShelterMap.hpp"

#include "PCH.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <vector>

using namespace XPMF;

namespace {

/**
 * @brief Floor division; C++ division truncates toward zero, and half the world has negative coordinates
 */
auto floorDiv(int value,
              int divisor) -> int
{
    const int quotient = value / divisor;
    return (value % divisor != 0 && (value < 0) != (divisor < 0)) ? quotient - 1 : quotient;
}

auto nodeIndex(int localX,
               int localY) -> std::size_t
{
    return (static_cast<std::size_t>(localY) * static_cast<std::size_t>(ShelterMap::K_NODES))
        + static_cast<std::size_t>(localX);
}

} // namespace

void ShelterMap::rasterize(std::array<Layer,
                                      K_BLOCK_CELLS>& layers,
                           const RE::NiPoint3& first,
                           const RE::NiPoint3& second,
                           const RE::NiPoint3& third)
{
    // Twice the signed area of the triangle's footprint; a wall has none and covers no column
    const float denominator
        = ((second.y - third.y) * (first.x - third.x)) + ((third.x - second.x) * (first.y - third.y));
    constexpr float MIN_FOOTPRINT = 1.0F;
    if (std::abs(denominator) < MIN_FOOTPRINT) {
        return;
    }

    // Lattice nodes inside the footprint's bounding box, clipped to the block of layers
    const Layer& center = layers[K_BLOCK_CENTER];
    const int blockWest = (center.cellX - 1) * K_CELLS;
    const int blockSouth = (center.cellY - 1) * K_CELLS;
    const int nodeWest
        = std::max(static_cast<int>(std::ceil(std::min({first.x, second.x, third.x}) / K_SPACING)), blockWest);
    const int nodeEast = std::min(static_cast<int>(std::floor(std::max({first.x, second.x, third.x}) / K_SPACING)),
                                  blockWest + (K_BLOCK * K_CELLS));
    const int nodeSouth
        = std::max(static_cast<int>(std::ceil(std::min({first.y, second.y, third.y}) / K_SPACING)), blockSouth);
    const int nodeNorth = std::min(static_cast<int>(std::floor(std::max({first.y, second.y, third.y}) / K_SPACING)),
                                   blockSouth + (K_BLOCK * K_CELLS));

    const float inverse = 1.0F / denominator;
    constexpr float EDGE_TOLERANCE = -1.0e-4F; // nodes exactly on a shared edge belong to both triangles
    for (int nodeY = nodeSouth; nodeY <= nodeNorth; ++nodeY) {
        const float worldY = static_cast<float>(nodeY) * K_SPACING;
        for (int nodeX = nodeWest; nodeX <= nodeEast; ++nodeX) {
            const float worldX = static_cast<float>(nodeX) * K_SPACING;
            const float weightFirst
                = (((second.y - third.y) * (worldX - third.x)) + ((third.x - second.x) * (worldY - third.y))) * inverse;
            const float weightSecond
                = (((third.y - first.y) * (worldX - third.x)) + ((first.x - third.x) * (worldY - third.y))) * inverse;
            const float weightThird = 1.0F - weightFirst - weightSecond;
            if (weightFirst < EDGE_TOLERANCE || weightSecond < EDGE_TOLERANCE || weightThird < EDGE_TOLERANCE) {
                continue;
            }
            const float height = (weightFirst * first.z) + (weightSecond * second.z) + (weightThird * third.z);

            // A node on a cell's west or south edge is also the last node of the cell before it
            const int cellX = floorDiv(nodeX, K_CELLS);
            const int cellY = floorDiv(nodeY, K_CELLS);
            const int localX = nodeX - (cellX * K_CELLS);
            const int localY = nodeY - (cellY * K_CELLS);
            for (int shareY = 0; shareY <= (localY == 0 ? 1 : 0); ++shareY) {
                for (int shareX = 0; shareX <= (localX == 0 ? 1 : 0); ++shareX) {
                    const int slotX = cellX - shareX - center.cellX + 1;
                    const int slotY = cellY - shareY - center.cellY + 1;
                    if (slotX < 0 || slotX >= K_BLOCK || slotY < 0 || slotY >= K_BLOCK) {
                        continue;
                    }
                    Layer& layer = layers.at(static_cast<std::size_t>((slotY * K_BLOCK) + slotX));
                    if (layer.top.empty()) {
                        layer.top.assign(static_cast<std::size_t>(K_NODES) * K_NODES, K_NOTHING);
                    }
                    float& top = layer.top[nodeIndex(shareX == 1 ? K_CELLS : localX, shareY == 1 ? K_CELLS : localY)];
                    top = std::max(top, height);
                }
            }
        }
    }
}

auto ShelterMap::Slope::of(const RE::NiPoint3& normal) -> Slope
{
    // Tilted past this a surface is a wall as far as snow is concerned (it holds none), and the
    // plane of one says nothing about the columns next to it
    constexpr float MIN_UP = 0.2F;
    if (normal.z < MIN_UP) {
        return {};
    }
    return {.dzdx = -normal.x / normal.z, .dzdy = -normal.y / normal.z};
}

auto ShelterMap::Slope::ofTriangle(const RE::NiPoint3& first,
                                   const RE::NiPoint3& second,
                                   const RE::NiPoint3& third) -> Slope
{
    const RE::NiPoint3 normal = (second - first).Cross(third - first);
    const float length = normal.Length();
    constexpr float MIN_LENGTH = 1.0e-6F; /**< Twice the area of a triangle that is no triangle */
    if (length <= MIN_LENGTH) {
        return {};
    }
    // Whichever way the triangle is wound, its slope is that of the side that faces up
    const RE::NiPoint3 unit = normal * (1.0F / length);
    return of(unit.z < 0.0F ? -unit : unit);
}

auto ShelterMap::Fade::of(float distance,
                          float edgeOpenness) -> Fade
{
    // What the plain falloff is at the depth the edge is to lie at, raised to the power, is the
    // edge value. Kept off 0 and 1, where no power gets it there
    constexpr float MIN_EDGE = 0.01F;
    const float plain = 1.0F - (K_EDGE_DEPTH * K_EDGE_DEPTH * (3.0F - (2.0F * K_EDGE_DEPTH)));
    const float edge = std::clamp(edgeOpenness, MIN_EDGE, 1.0F - MIN_EDGE);
    return {.distance = distance, .power = std::log(edge) / std::log(plain)};
}

auto ShelterMap::Field::topAt(int nodeX,
                              int nodeY) const -> float
{
    const int cellX = floorDiv(nodeX, K_CELLS);
    const int cellY = floorDiv(nodeY, K_CELLS);
    const int slotX = cellX - centerX + 1;
    const int slotY = cellY - centerY + 1;
    if (slotX < 0 || slotX >= K_BLOCK || slotY < 0 || slotY >= K_BLOCK) {
        return K_NOTHING;
    }
    const auto& map = maps.at(static_cast<std::size_t>((slotY * K_BLOCK) + slotX));
    return map != nullptr ? (*map)[nodeIndex(nodeX - (cellX * K_CELLS), nodeY - (cellY * K_CELLS))] : K_NOTHING;
}

auto ShelterMap::cellOf(float coordinate) -> int { return static_cast<int>(std::floor(coordinate / K_CELL_SIZE)); }

auto ShelterMap::nearestNodeOf(float coordinate) -> int
{
    return static_cast<int>(std::lround(coordinate / K_SPACING));
}

auto ShelterMap::Field::ceilingAt(const RE::NiPoint3& point,
                                  const Slope& slope,
                                  int nodeX,
                                  int nodeY) -> float
{
    return point.z + K_CLEARANCE + (slope.dzdx * ((static_cast<float>(nodeX) * K_SPACING) - point.x))
        + (slope.dzdy * ((static_cast<float>(nodeY) * K_SPACING) - point.y));
}

auto ShelterMap::Field::isCovered(const RE::NiPoint3& point,
                                  const Slope& slope) const -> bool
{
    const auto covered
        = [&](int nodeX, int nodeY) -> bool { return topAt(nodeX, nodeY) > ceilingAt(point, slope, nodeX, nodeY); };
    const int nodeX = nearestNodeOf(point.x);
    const int nodeY = nearestNodeOf(point.y);
    if (!covered(nodeX, nodeY)) {
        return false;
    }
    // ...and in one of the four blocks of four nodes that node is a corner of
    for (int blockY = nodeY - 1; blockY <= nodeY; ++blockY) {
        for (int blockX = nodeX - 1; blockX <= nodeX; ++blockX) {
            if (covered(blockX, blockY) && covered(blockX + 1, blockY) && covered(blockX, blockY + 1)
                && covered(blockX + 1, blockY + 1)) {
                return true;
            }
        }
    }
    return false;
}

namespace {

/**
 * @brief Calls visit(nodeX, nodeY) for every node at Chebyshev distance `ring` from a node: the
 * whole perimeter of the square, each node once
 */
template <typename Visit>
void forEachOnRing(int centerX,
                   int centerY,
                   int ring,
                   const Visit& visit)
{
    if (ring == 0) {
        visit(centerX, centerY);
        return;
    }
    for (int offset = -ring; offset <= ring; ++offset) {
        visit(centerX + offset, centerY - ring);
        visit(centerX + offset, centerY + ring);
    }
    for (int offset = -ring + 1; offset <= ring - 1; ++offset) {
        visit(centerX - ring, centerY + offset);
        visit(centerX + ring, centerY + offset);
    }
}

/**
 * @brief The least distance a point within half a spacing of a node can have to any node of the
 * ring at Chebyshev distance `ring` around it
 */
auto leastRingDistance(int ring) -> float
{
    constexpr float HALF = 0.5F;
    return (static_cast<float>(ring) - HALF) * ShelterMap::K_SPACING;
}

} // namespace

auto ShelterMap::Field::depthUnderCover(const RE::NiPoint3& point,
                                        const Slope& slope,
                                        float reach) const -> float
{
    if (!isCovered(point, slope)) {
        return 0.0F;
    }
    const int nodeX = nearestNodeOf(point.x);
    const int nodeY = nearestNodeOf(point.y);

    // Distance to the nearest column that is open at this height - the drip line, from inside.
    // Ring by ring out from the nearest node, which is up to half a spacing from the point (hence
    // the half spacing more of radius): once no node of the next ring can be nearer than the best
    // column found, the rest cannot either, which next to a drip line - where most of the asking
    // happens - ends the search after a ring or two
    constexpr float HALF_SPACING = K_SPACING * 0.5F;
    const int radius = static_cast<int>(std::ceil((reach + HALF_SPACING) / K_SPACING));
    float nearestSq = reach * reach;
    for (int ring = 0; ring <= radius; ++ring) {
        const float least = leastRingDistance(ring);
        if (ring > 0 && least * least >= nearestSq) {
            break;
        }
        forEachOnRing(nodeX, nodeY, ring, [&](int columnX, int columnY) -> void {
            if (topAt(columnX, columnY) > ceilingAt(point, slope, columnX, columnY)) {
                return;
            }
            const float deltaX = (static_cast<float>(columnX) * K_SPACING) - point.x;
            const float deltaY = (static_cast<float>(columnY) * K_SPACING) - point.y;
            nearestSq = std::min(nearestSq, (deltaX * deltaX) + (deltaY * deltaY));
        });
    }
    // The drip line runs somewhere between that open column and the covered one before it; half a
    // spacing is the unbiased guess (measuring to the node itself reads 0 to K_SPACING too deep)
    return std::max(std::sqrt(nearestSq) - HALF_SPACING, 0.0F);
}

auto ShelterMap::Field::distanceToCover(const RE::NiPoint3& point,
                                        const Slope& slope,
                                        float reach) const -> float
{
    const int nodeX = nearestNodeOf(point.x);
    const int nodeY = nearestNodeOf(point.y);
    constexpr float HALF_SPACING = K_SPACING * 0.5F;
    const int radius = static_cast<int>(std::ceil((reach + HALF_SPACING) / K_SPACING));
    float nearestSq = std::numeric_limits<float>::max();
    for (int ring = 0; ring <= radius; ++ring) { // ring by ring, as depthUnderCover does
        const float least = leastRingDistance(ring);
        if (ring > 0 && least * least >= nearestSq) {
            break;
        }
        forEachOnRing(nodeX, nodeY, ring, [&](int columnX, int columnY) -> void {
            if (topAt(columnX, columnY) <= ceilingAt(point, slope, columnX, columnY)) {
                return;
            }
            const float deltaX = (static_cast<float>(columnX) * K_SPACING) - point.x;
            const float deltaY = (static_cast<float>(columnY) * K_SPACING) - point.y;
            nearestSq = std::min(nearestSq, (deltaX * deltaX) + (deltaY * deltaY));
        });
    }
    if (nearestSq == std::numeric_limits<float>::max()) {
        return reach + K_SPACING; // nothing covered within the window: well clear of any cover
    }
    return std::max(std::sqrt(nearestSq) - HALF_SPACING, 0.0F);
}

auto ShelterMap::Field::opennessAt(const RE::NiPoint3& point,
                                   const Slope& slope,
                                   const Fade& fade) const -> float
{
    // The fade, plus the spacing depthUnderCover's estimate gives up: a point deep under cover has
    // to be able to reach 0, or nothing ever counts as covered
    const float depth = depthUnderCover(point, slope, fade.distance + K_SPACING);
    if (depth <= 0.0F) {
        return 1.0F;
    }
    // A smooth step down over the distance, bent to the material (Fade)
    const float t = fade.distance > 0.0F ? std::clamp(depth / fade.distance, 0.0F, 1.0F) : 1.0F;
    return std::pow(1.0F - (t * t * (3.0F - (2.0F * t))), fade.power);
}

auto ShelterMap::Field::anyCoverOver(float minX,
                                     float minY,
                                     float maxX,
                                     float maxY,
                                     float lowZ) const -> bool
{
    // The nodes whose columns pass over the rectangle, clipped to what the field knows about
    const int fieldWest = (centerX - 1) * K_CELLS;
    const int fieldSouth = (centerY - 1) * K_CELLS;
    const int nodeWest = std::max(static_cast<int>(std::floor(minX / K_SPACING)), fieldWest);
    const int nodeEast = std::min(static_cast<int>(std::ceil(maxX / K_SPACING)), fieldWest + (K_BLOCK * K_CELLS));
    const int nodeSouth = std::max(static_cast<int>(std::floor(minY / K_SPACING)), fieldSouth);
    const int nodeNorth = std::min(static_cast<int>(std::ceil(maxY / K_SPACING)), fieldSouth + (K_BLOCK * K_CELLS));
    const float coveredAbove = lowZ + K_CLEARANCE;
    for (int nodeY = nodeSouth; nodeY <= nodeNorth; ++nodeY) {
        for (int nodeX = nodeWest; nodeX <= nodeEast; ++nodeX) {
            if (topAt(nodeX, nodeY) > coveredAbove) {
                return true;
            }
        }
    }
    return false;
}

auto ShelterMap::Field::initialOpenness(std::span<const RE::NiPoint3> positions,
                                        std::span<const RE::NiPoint3> normals,
                                        const Fade& fade,
                                        std::vector<float>& openness) const -> bool
{
    openness.assign(positions.size(), 1.0F);
    bool anyCover = false;
    for (std::size_t index = 0; index < positions.size(); ++index) {
        openness[index]
            = opennessAt(positions[index], index < normals.size() ? Slope::of(normals[index]) : Slope {}, fade);
        anyCover = anyCover || openness[index] < 1.0F;
    }
    return anyCover;
}

void ShelterMap::Field::settleOpenness(std::span<const RE::NiPoint3> positions,
                                       std::span<const std::uint16_t> indices,
                                       const Fade& fade,
                                       float edgeOpenness,
                                       const std::vector<bool>& measured,
                                       std::vector<float>& openness) const
{
    if (indices.empty() || openness.size() != positions.size()) {
        return;
    }
    const auto opennessAt
        = [&](const RE::NiPoint3& point, const Slope& slope) -> float { return this->opennessAt(point, slope, fade); };

    // Which vertices are in the open is decided now: those are never lowered, and they are the ends
    // the third step measures from
    std::vector<bool> isOpen(positions.size());
    for (std::size_t index = 0; index < positions.size(); ++index) {
        isOpen[index] = openness[index] >= 1.0F;
    }
    // ...and which stand as they are: the ones ShelterRefinement measured. Every step below reads
    // them and none writes them
    const auto stands = [&](std::size_t vertex) -> bool { return vertex < measured.size() && measured[vertex]; };
    const auto movable = [&](std::size_t vertex) -> bool { return !isOpen[vertex] && !stands(vertex); };
    const auto isTriangle = [&](const std::array<std::size_t, 3>& triangle) -> bool {
        return triangle[0] < positions.size() && triangle[1] < positions.size() && triangle[2] < positions.size();
    };

    // Second step: the inside of every triangle that has a covered corner. Where the corners
    // interpolate to more openness than a probe in the middle has, the covered corners are lowered
    // just enough to close the gap, the excess spread over them in proportion to their weight at
    // the probe; a corner ends at the lowest value any probe asks of it. Open corners are never
    // touched: a triangle that is mostly in the open keeps its snow there. Neither are measured
    // ones, and a triangle with no other kind is not looked at.
    constexpr std::array<std::array<float, 3>, 4> PROBES {
        {{1.0F / 3.0F, 1.0F / 3.0F, 1.0F / 3.0F}, {0.5F, 0.5F, 0.0F}, {0.0F, 0.5F, 0.5F}, {0.5F, 0.0F, 0.5F}}};
    std::vector<float> lowered(openness);
    for (std::size_t corner = 0; corner + 2 < indices.size(); corner += 3) {
        const std::array<std::size_t, 3> triangle {indices[corner], indices[corner + 1], indices[corner + 2]};
        if (!isTriangle(triangle) || std::ranges::none_of(triangle, movable)) {
            continue;
        }
        const RE::NiPoint3& first = positions[triangle[0]];
        const RE::NiPoint3& second = positions[triangle[1]];
        const RE::NiPoint3& third = positions[triangle[2]];
        const Slope slope = Slope::ofTriangle(first, second, third);
        for (const auto& weights : PROBES) {
            float interpolated = 0.0F;
            float coveredWeight = 0.0F;
            for (std::size_t k = 0; k < triangle.size(); ++k) {
                const std::size_t vertex = triangle.at(k);
                interpolated += weights.at(k) * openness[vertex];
                coveredWeight += movable(vertex) ? weights.at(k) : 0.0F;
            }
            if (interpolated <= 0.0F || coveredWeight <= 0.0F) {
                continue; // nothing left to lower, or nothing here that may be
            }
            const RE::NiPoint3 probe = (first * weights[0]) + (second * weights[1]) + (third * weights[2]);
            const float excess = interpolated - opennessAt(probe, slope);
            if (excess <= 0.0F) {
                continue;
            }
            const float cut = excess / coveredWeight;
            for (const std::size_t vertex : triangle) {
                if (movable(vertex)) {
                    lowered[vertex] = std::min(lowered[vertex], std::max(openness[vertex] - cut, 0.0F));
                }
            }
        }
    }
    openness.swap(lowered);

    // Third step: put the snow edge where cover begins on every edge that crosses a drip line
    constexpr float MIN_EDGE_LENGTH = 1.0F; /**< Shorter is a doubled vertex, not a direction */
    const auto edgeLength
        = [&](std::size_t from, std::size_t to) -> float { return (positions[to] - positions[from]).Length(); };
    // Fraction of an edge, measured from its first vertex, that lies in the open
    const auto inTheOpen = [&](std::size_t from, std::size_t to, const Slope& slope) -> float {
        const RE::NiPoint3& start = positions[from];
        const RE::NiPoint3 delta = positions[to] - start;
        for (int sample = 1; sample <= K_EDGE_SAMPLES; ++sample) {
            const float fraction = static_cast<float>(sample) / static_cast<float>(K_EDGE_SAMPLES);
            if (isCovered(start + (delta * fraction), slope)) {
                return (static_cast<float>(sample) - 0.5F) / static_cast<float>(K_EDGE_SAMPLES);
            }
        }
        return 1.0F;
    };

    /**
     * An edge whose covered end, even at 0, leaves the crossing beyond its target
     */
    struct Overrun {
        std::size_t open;
        std::size_t covered;
        float target;
    };
    std::vector<Overrun> overruns;
    const auto localize = [&](std::size_t covered, std::size_t open, const Slope& slope) -> void {
        const float length = edgeLength(open, covered);
        if (length <= MIN_EDGE_LENGTH) {
            return;
        }
        // Interpolated openness runs from 1 at the open end to the covered vertex's value; it has
        // to pass edgeOpenness at the target fraction, which fixes that value
        constexpr float MIN_TARGET = 0.05F;
        const float target
            = std::clamp(inTheOpen(open, covered, slope) + (K_EDGE_DEPTH * fade.distance / length), MIN_TARGET, 1.0F);
        const float needed = 1.0F - ((1.0F - edgeOpenness) / target);
        if (!stands(covered)) {
            openness[covered] = std::max(openness[covered], std::clamp(needed, 0.0F, 1.0F));
        }
        if (needed < 0.0F) {
            overruns.push_back({.open = open, .covered = covered, .target = target});
        }
    };

    for (std::size_t corner = 0; corner + 2 < indices.size(); corner += 3) {
        const std::array<std::size_t, 3> triangle {indices[corner], indices[corner + 1], indices[corner + 2]};
        if (!isTriangle(triangle)) {
            continue;
        }
        const Slope slope = Slope::ofTriangle(positions[triangle[0]], positions[triangle[1]], positions[triangle[2]]);
        for (const std::size_t covered : triangle) {
            for (const std::size_t open : triangle) {
                // ...unless both ends are measured: there is nothing on that edge to move
                if (!isOpen[covered] && isOpen[open] && !(stands(covered) && stands(open))) {
                    localize(covered, open, slope);
                }
            }
        }
    }
    if (overruns.empty()) {
        return;
    }

    // Fourth step: an edge the third could not settle has one value left to move, the open end's.
    // It is lowered so that the interpolation passes edgeOpenness at the target after all - but
    // only where the open vertex stands on the drip line: along every edge from it the open
    // surface stays within dripLineReach of cover, so what it thins is confined to a strip that
    // wide along the eave. A vertex with an edge running away from cover is anchored in the open
    // and keeps its snow, whatever a long edge under a roof asks of it; an edge running along an
    // eave anchors nothing, its whole length being close to cover. An edge whose covered end is
    // itself above edgeOpenness is snowy to that end and beyond the open end's reach.
    const float dripLineReach = std::max(0.5F * fade.distance, K_SPACING);
    std::vector<float> wanted(positions.size(), 1.0F); // the lowest value any overrun asks of an open vertex
    for (const auto& overrun : overruns) {
        const float coveredEnd = openness[overrun.covered];
        if (coveredEnd >= edgeOpenness || stands(overrun.open)) {
            continue;
        }
        const float value = (edgeOpenness - (coveredEnd * overrun.target)) / (1.0F - overrun.target);
        wanted[overrun.open] = std::min(wanted[overrun.open], std::clamp(value, 0.0F, 1.0F));
    }
    // Whether the open run of an edge, walked from its first vertex, gets farther than
    // dripLineReach from any cover
    const auto runsAwayFromCover = [&](std::size_t from, std::size_t to, const Slope& slope) -> bool {
        const RE::NiPoint3& start = positions[from];
        const RE::NiPoint3 delta = positions[to] - start;
        for (int sample = 0; sample <= K_EDGE_SAMPLES; ++sample) {
            const RE::NiPoint3 point
                = start + (delta * (static_cast<float>(sample) / static_cast<float>(K_EDGE_SAMPLES)));
            if (isCovered(point, slope)) {
                return false; // the rest of the edge lies under cover
            }
            if (distanceToCover(point, slope, dripLineReach) > dripLineReach) {
                return true;
            }
        }
        return false;
    };
    std::vector<bool> anchored(positions.size());
    for (std::size_t corner = 0; corner + 2 < indices.size(); corner += 3) {
        const std::array<std::size_t, 3> triangle {indices[corner], indices[corner + 1], indices[corner + 2]};
        if (!isTriangle(triangle)
            || (wanted[triangle[0]] >= 1.0F && wanted[triangle[1]] >= 1.0F && wanted[triangle[2]] >= 1.0F)) {
            continue;
        }
        const Slope slope = Slope::ofTriangle(positions[triangle[0]], positions[triangle[1]], positions[triangle[2]]);
        for (const std::size_t vertex : triangle) {
            if (wanted[vertex] >= 1.0F || anchored[vertex]) {
                continue;
            }
            for (const std::size_t other : triangle) {
                if (other != vertex && edgeLength(vertex, other) > MIN_EDGE_LENGTH
                    && runsAwayFromCover(vertex, other, slope)) {
                    anchored[vertex] = true;
                    break;
                }
            }
        }
    }
    for (std::size_t index = 0; index < positions.size(); ++index) {
        if (wanted[index] < 1.0F && !anchored[index]) {
            openness[index] = std::min(openness[index], wanted[index]);
        }
    }
}

auto ShelterMap::tally(std::span<const float> openness,
                       std::span<const float> facing,
                       float holdsSnowFrom,
                       float edgeOpenness) -> Tally
{
    Tally counts;
    for (std::size_t index = 0; index < openness.size() && index < facing.size(); ++index) {
        if (facing[index] < holdsSnowFrom) {
            continue;
        }
        ++counts.holders;
        counts.covered += openness[index] < 1.0F ? 1 : 0;
        counts.buried += openness[index] <= 0.0F ? 1 : 0;
        counts.bare += openness[index] < edgeOpenness ? 1 : 0;
    }
    return counts;
}

auto ShelterMap::judge(const Tally& counts,
                       bool maskable) -> Verdict
{
    if (counts.holders == 0) {
        return Verdict::OPEN; // nothing on it shows snow either way
    }
    if (!maskable) {
        return counts.bare * 2 >= counts.holders ? Verdict::SHELTERED : Verdict::OPEN;
    }
    if (counts.covered == 0) {
        return Verdict::OPEN;
    }
    return counts.buried == counts.holders ? Verdict::SHELTERED : Verdict::PARTIAL;
}

auto ShelterMap::combine(const std::vector<std::shared_ptr<const Heights>>& layers) -> std::shared_ptr<const Heights>
{
    std::shared_ptr<Heights> combined;
    for (const auto& layer : layers) {
        if (layer == nullptr || layer->empty()) {
            continue;
        }
        if (combined == nullptr) {
            combined = std::make_shared<Heights>(*layer);
            continue;
        }
        std::ranges::transform(
            *combined, *layer, combined->begin(), [](float lhs, float rhs) -> float { return std::max(lhs, rhs); });
    }
    return combined;
}
