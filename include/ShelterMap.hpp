#pragma once

#include "PCH.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <vector>

namespace XPMF {

/**
 * @brief Top-down height fields that answer "how far under cover is this point?"
 *
 * Snow falls straight down, and the single pass projection is straight down too, so whether
 * a point is sheltered is a question about the column above it: is any surface higher up? That
 * makes the whole scene reducible to one number per column - the height of its topmost
 * surface - which is what these maps hold, sampled on a lattice of K_SPACING units. A point is
 * under cover where the lattice node nearest to it tops out more than K_CLEARANCE above the
 * surface the point lies on, and that node sits in a block of four that all do (see
 * Field::isCovered); it is in the open where not, which includes a roof's own top side and
 * the foot of a wall (a taller neighbor column blocks nothing above).
 *
 * The maps are rasterized from the triangles the game renders (the CPU copies it keeps for its
 * decal builder), not from collision: what shelters is exactly what can be seen sheltering, a
 * roof without collision counts, an invisible collision box does not.
 *
 * Cells are the unit of bookkeeping. A Layer is what the triangles gathered from one source
 * cell put over one target cell (a building on a cell border covers both), a target's map is
 * the maximum over its layers, and a Field is the 3x3 block of maps a query may reach into.
 */
class ShelterMap {
public:
    ShelterMap() = delete;

    constexpr static float K_CELL_SIZE = 4096.0F; /**< World units per exterior cell side */
    constexpr static float K_SPACING = 32.0F; /**< Lattice spacing; a roof edge is located to about half of it */
    constexpr static int K_CELLS = static_cast<int>(K_CELL_SIZE / K_SPACING); /**< Lattice cells per cell side */
    constexpr static int K_NODES = K_CELLS + 1; /**< Lattice nodes per side; the last row is the neighbor's first */
    constexpr static int K_BLOCK = 3; /**< Cells per side of the block of cells a source cell's triangles reach and a
                                         query may read: the cell and its neighbors, since a building on a border
                                         overhangs the next cell and never the one beyond */
    constexpr static int K_BLOCK_CELLS = K_BLOCK * K_BLOCK; /**< Cells in a block, row major */
    constexpr static std::size_t K_BLOCK_CENTER = K_BLOCK_CELLS / 2; /**< The block's middle cell */
    constexpr static float K_CLEARANCE = 24.0F; /**< How far above a point a surface must be to shelter it: more
                                                   than a curved surface the point itself lies on can rise within
                                                   half a spacing of it, less than any roof a snowed-on thing
                                                   fits under */
    constexpr static float K_NOTHING = std::numeric_limits<float>::lowest(); /**< Node no triangle covers */
    constexpr static int K_EDGE_SAMPLES = 12; /**< Points tested along a triangle edge that crosses a drip line */

    using Heights = std::vector<float>; /**< K_NODES x K_NODES top surface heights, row major (y, then x) */

    /**
     * @brief Height field under construction for one target cell
     */
    struct Layer {
        int cellX {}; /**< Target cell X */
        int cellY {}; /**< Target cell Y */
        Heights top; /**< Empty until the first triangle lands */
    };

    /**
     * @brief Rasterizes one world space triangle into the layers of the 3x3 cells around a source cell
     *
     * A node takes the triangle's height where the node's column passes through it, keeping
     * the maximum. Near-vertical triangles cover no columns and cost a bounding box test.
     *
     * @param layers The block of layers around the source cell, row major, [K_BLOCK_CENTER] being
     *        the source itself
     */
    static void rasterize(std::array<Layer,
                                     K_BLOCK_CELLS>& layers,
                          const RE::NiPoint3& first,
                          const RE::NiPoint3& second,
                          const RE::NiPoint3& third);

    /**
     * @brief The tilt of the surface a point lies on: how much higher that surface runs per unit
     * east and per unit north of the point
     *
     * The lattice is read next to a point, never at it, and on the point's own surface a node
     * uphill reads higher than the point - on a steep roof by more than K_CLEARANCE, which would
     * let the roof shelter itself. With the tilt known, every node is compared with where the
     * point's surface passes that node instead of with the point's own height.
     */
    struct Slope {
        float dzdx {}; /**< Rise per unit of x; 0 on a flat surface */
        float dzdy {}; /**< Rise per unit of y */

        /**
         * @brief The slope of the surface a world space normal stands on
         *
         * Flat for a normal that is nearly horizontal: a wall holds no snow, and the plane of one
         * says nothing about the columns next to it.
         */
        [[nodiscard]] static auto of(const RE::NiPoint3& normal) -> Slope;

        /**
         * @brief The slope of a world space triangle; flat for a degenerate one
         */
        [[nodiscard]] static auto ofTriangle(const RE::NiPoint3& first,
                                             const RE::NiPoint3& second,
                                             const RE::NiPoint3& third) -> Slope;
    };

    /**
     * @brief The 3x3 block of finished maps around a cell, immutable and safe to read on the worker
     */
    struct Field {
        int centerX {}; /**< Cell X of the middle map */
        int centerY {}; /**< Cell Y of the middle map */
        std::array<std::shared_ptr<const Heights>, K_BLOCK_CELLS> maps; /**< Row major; nullptr where no map exists
                                                                          yet */

        /**
         * @brief Per vertex openness (1 in the open .. 0 deep under cover) of one mesh, the first
         * of four steps: every vertex gets the openness of its own spot, read along its own surface
         *
         * That alone is only right where the mesh is fine enough to follow the fade, and game
         * meshes are not - a stair flight is two rows of vertices, a porch plank has one at either
         * end, a covered walkway's floor is one polygon with every vertex on its rim - and a
         * vertex value is wrong in both directions on such a mesh. A covered vertex would drag
         * the interpolated value down along the whole triangle and strip snow that lies in the
         * open; and a triangle whose corners all sit a hand's width under an eave, but whose
         * middle lies deep under the roof, would keep its snow throughout, because nothing ever
         * looks at the middle. Two things put that right: ShelterRefinement, where the profile
         * allows it, adds the vertices such a mesh lacks, and the openness of this step then
         * stands as it is on every surface it tested; settleOpenness makes the best of the
         * vertices there are everywhere else.
         *
         * @param positions World space vertex positions
         * @param normals World space vertex normals, one per position, or empty for a mesh
         *        without them (every vertex is then read as lying on a flat surface)
         * @param fade World units under cover over which openness falls to 0
         * @param openness Out: one value per position
         * @return bool Whether any vertex is under cover
         */
        [[nodiscard]] auto initialOpenness(std::span<const RE::NiPoint3> positions,
                                           std::span<const RE::NiPoint3> normals,
                                           float fade,
                                           std::vector<float>& openness) const -> bool;

        /**
         * @brief The second to fourth steps: the openness of initialOpenness (plus whatever
         * ShelterRefinement appended) settled against the mesh's triangles
         *
         * The second step looks at the middle: at the centroid and the edge midpoints of every
         * triangle that has a covered corner, the openness the corners interpolate to is compared
         * with the openness of that spot, and where the interpolation comes out too open the
         * covered corners are lowered just enough to close the gap (the excess spread over them
         * in proportion to their weight there; a corner ends at the lowest value any probe asked
         * of it). Open corners are never touched, so a triangle that is mostly in the open keeps
         * its snow there.
         *
         * The third step walks every triangle edge that joins an open and a covered vertex,
         * finds where along it cover actually begins, and raises the covered vertex's openness
         * just enough that the interpolated value crosses edgeOpenness there (half a fade past
         * the drip line) rather than somewhere out in the open. It runs after the second, so
         * whatever that took from a vertex next to the open, the drip line on that edge stays put.
         *
         * Raising has a floor. A porch plank whose far end is deep under the roof and whose near
         * end pokes a hand's width past the eave interpolates from 1 to 0 over its whole length,
         * and the snow reaches two thirds of the way in - while the plank next to it, whose near
         * end sits a few units inside the eave and was lowered, is bare: a strip of snow on one
         * plank. So the fourth step lowers the open end of such an edge instead, just enough for
         * the crossing to land where the third step wanted it. Only an open vertex standing on
         * the drip line is touched: along every edge from it, the open surface stays within half
         * a fade (or a spacing) of cover, so what it thins is a strip that wide along the eave.
         * A vertex with an edge running away from cover - a courtyard corner, a plank end well
         * clear of the roof - is anchored in the open and keeps its snow. Edges running along an
         * eave do not anchor: their whole length is close to cover.
         *
         * All three trade a vertex's own openness for a better line across the triangles around
         * it, which is the right trade only where those triangles are all there is to carry the
         * fade. Where ShelterRefinement has been, they are not: the vertices it tested and the
         * ones it added sit where the fade bends, each with the openness of its own spot, and two
         * shapes that meet - one piece of a walkway and the next - agree along the seam because
         * both read the same field. Those vertices are measured, and no step moves them; what is
         * left to settle on such a shape is what was too narrow or too small to test, and the
         * vertices whose normals are bent away from the faces around them: read along a tilt
         * those faces do not have, their own openness is no measurement of what the faces show.
         *
         * @param indices The mesh's triangle list; empty does nothing
         * @param edgeOpenness Openness at which snow visibly ends on the mesh's material
         * @param measured Per vertex, whether its openness is to stand as it is (from
         *        ShelterRefinement::refine); empty for a mesh none of whose vertices are
         * @param openness In: one value per position; out: settled
         */
        void settleOpenness(std::span<const RE::NiPoint3> positions,
                            std::span<const std::uint16_t> indices,
                            float fade,
                            float edgeOpenness,
                            const std::vector<bool>& measured,
                            std::vector<float>& openness) const;

        /**
         * @brief Openness (1 in the open .. 0 deep under cover) of one world space point
         *
         * @param slope The tilt of the surface the point lies on
         * @param fade World units under cover over which openness falls to 0
         */
        [[nodiscard]] auto opennessAt(const RE::NiPoint3& point,
                                      const Slope& slope,
                                      float fade) const -> float;

        /**
         * @brief What the columns over a world space rectangle hold, at a glance
         *
         * A cheap look before an expensive one: a shape none of whose vertices is under cover may
         * still have a roof edge crossing the middle of a triangle, and one all of whose vertices
         * are may have a skylight over the middle of one; whether either is even possible is a
         * matter of scanning the lattice over the shape's footprint, which is far cheaper than
         * probing the inside of every triangle to find out.
         */
        struct Overhead {
            bool anyCovered {}; /**< Some column tops out above lowZ plus the clearance: something may shelter */
            bool anyOpen {}; /**< Some column tops out at or below highZ plus the clearance, or is empty */
        };

        /**
         * @param lowZ The lowest point of what stands in the rectangle
         * @param highZ Its highest point
         */
        [[nodiscard]] auto overhead(float minX,
                                    float minY,
                                    float maxX,
                                    float maxY,
                                    float lowZ,
                                    float highZ) const -> Overhead;

    private:
        /**
         * @brief Top surface height at a global lattice node; K_NOTHING where unknown
         */
        [[nodiscard]] auto topAt(int nodeX,
                                 int nodeY) const -> float;

        /**
         * @brief The height a column has to top out above to shelter a point: where the point's
         * own surface passes the column, plus K_CLEARANCE
         */
        [[nodiscard]] static auto ceilingAt(const RE::NiPoint3& point,
                                            const Slope& slope,
                                            int nodeX,
                                            int nodeY) -> float;

        /**
         * @brief Whether a world space point has something overhead
         *
         * Under cover when the lattice node nearest to the point tops out above the point's
         * ceiling and belongs to a block of four nodes that all do. The nearest node alone
         * puts a roof's edge where it is to within half a spacing, either way; the block is
         * what keeps a rope, a beam or a railing - anything a single node wide - from
         * sheltering what lies under it, and the foot of a wall in the open: the columns
         * inside the wall top out high, but a point next to it is nearest to one that does not.
         *
         * @param slope The tilt of the surface the point lies on, so that the surface itself
         *        is never read as cover
         */
        [[nodiscard]] auto isCovered(const RE::NiPoint3& point,
                                     const Slope& slope) const -> bool;

        /**
         * @brief How far inside cover a world space point is
         *
         * @param point The point
         * @param slope The tilt of the surface the point lies on
         * @param reach The farthest distance worth reporting (the fade distance plus a spacing)
         * @return float 0 in the open; otherwise the distance to the nearest lattice column that
         *         is open at the point's height, less half a spacing (the drip line runs
         *         somewhere between that column and the covered one before it), never above
         *         reach less that half spacing and never below 0
         */
        [[nodiscard]] auto depthUnderCover(const RE::NiPoint3& point,
                                           const Slope& slope,
                                           float reach) const -> float;

        /**
         * @brief How far a world space point in the open is from cover: the drip line seen from
         * outside, the counterpart of depthUnderCover
         *
         * @param point The point, taken to be in the open
         * @param slope The tilt of the surface the point lies on
         * @param reach The farthest distance worth telling apart
         * @return float The distance to the nearest lattice column that is covered at the
         *         point's height, less half a spacing, never below 0; more than reach when no such
         *         column lies within it
         */
        [[nodiscard]] auto distanceToCover(const RE::NiPoint3& point,
                                           const Slope& slope,
                                           float reach) const -> float;
    };

    /**
     * @brief The exterior cell a world coordinate falls in, on one axis
     */
    [[nodiscard]] static auto cellOf(float coordinate) -> int;

    /**
     * @brief What cover means for a whole shape
     */
    enum class Verdict : std::uint8_t {
        OPEN, /**< Snow stays as it is */
        PARTIAL, /**< Snow has to be masked vertex by vertex */
        SHELTERED /**< No snow belongs on this shape at all */
    };

    /**
     * @brief What cover does to the vertices of a shape that can hold snow
     */
    struct Tally {
        std::size_t holders {}; /**< Vertices facing up enough to hold snow, covered or not */
        std::size_t covered {}; /**< ...of those, with anything overhead */
        std::size_t buried {}; /**< ...entirely under cover */
        std::size_t bare {}; /**< ...far enough under cover for their snow to be gone */
    };

    /**
     * @brief Counts a shape's snow-holding vertices by what cover does to them
     *
     * @param openness Per vertex openness, from Field::initialOpenness and settleOpenness
     * @param facing Per vertex dot(normal, up)
     * @param holdsSnowFrom The facing from which a vertex can hold snow at all
     * @param edgeOpenness Openness at which snow visibly ends
     */
    [[nodiscard]] static auto tally(std::span<const float> openness,
                                    std::span<const float> facing,
                                    float holdsSnowFrom,
                                    float edgeOpenness) -> Tally;

    /**
     * @brief Judges a shape from the openness of the vertices that can hold snow
     *
     * Only those count. Every rock overhangs its own flanks and every house its own walls, but
     * a flank or a wall never carries snow, covered or not - judged on all vertices, half the
     * snowed-on shapes in a town came out "partly covered" and got vertex data of their own
     * for nothing.
     *
     * A shape whose vertex alpha can serve as a mask is OPEN when no such vertex is covered,
     * SHELTERED when all of them are entirely, PARTIAL otherwise. A shape whose vertex alpha is
     * taken (it blends or alpha tests with it) cannot be masked, so it is all or nothing:
     * SHELTERED once snow is gone from at least half of those vertices, OPEN below that.
     *
     * @param counts The shape's snow-holding vertices, from tally
     * @param maskable Whether the shape's vertex alpha is free to be used as a mask
     */
    [[nodiscard]] static auto judge(const Tally& counts,
                                    bool maskable) -> Verdict;

    /**
     * @brief Element-wise maximum of a cell's layers
     *
     * @return std::shared_ptr<const Heights> nullptr when no layer holds anything
     */
    [[nodiscard]] static auto combine(const std::vector<std::shared_ptr<const Heights>>& layers)
        -> std::shared_ptr<const Heights>;

private:
    /**
     * @brief The lattice node nearest to a world coordinate, on one axis
     */
    [[nodiscard]] static auto nearestNodeOf(float coordinate) -> int;
};

} // namespace XPMF
