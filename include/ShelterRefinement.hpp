#pragma once

#include "ShelterMap.hpp"

#include "PCH.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace XPMF {

/**
 * @brief Puts vertices where the cover changes on a mesh too coarse to follow it
 *
 * The shelter reaches the pixel shader through vertex alpha and nothing else, so between two
 * vertices it can only ever be a straight line. Game meshes are built for anything but: a
 * covered walkway's floor is a row of planks that run from one eave to the other with a vertex
 * at either end, so whatever alpha those ends get, the plank interpolates it across the whole
 * bay - a wedge of snow reaching in under the roof, or a floor stripped bare to the edge. Mesh
 * fixes that hand-place a few loops of vertices along the eaves (Simplicity of Snow's) put that
 * right; this class places them at runtime, for any shape, against the roof that is actually
 * there.
 *
 * It is a refinement, not a re-mesh: triangles are split, never moved, and only where a probe
 * shows the straight line to be wrong. Every edge of a triangle that can hold snow is sampled at
 * a few points, the openness found there compared with what its two ends interpolate to, and an
 * edge whose worst sample is off by more than the tolerance is split at that sample. A triangle
 * whose edges all pass but whose middle does not (a roof corner over the middle of a large
 * triangle) splits its longest edge. The children go through the same test, round by round,
 * until every sample agrees or an edge would get shorter than the lattice resolves. Deep under
 * cover and out in the open everything interpolates flat and nothing is touched: the vertices
 * land in the fade band and along the drip line, which is where the hand-made fixes put them.
 *
 * What is tested is every triangle wide enough for a wedge to show on it, and every narrower one
 * that is a piece of something wider. The game's own walkway floors are one polygon each, cut
 * into a fan of slivers a few units across that run from one rim of the floor to the other: none
 * of them is wide enough by itself, all of them together are the floor, and it is along their
 * long edges that the fade has to be carried. So narrowness is judged by the surface - the snow
 * holding triangles that continue one another across shared edges without a crease - and only
 * what is narrow as a whole is left alone: a rope, the top of a beam, the edge of a plank. A
 * fan is refined along those long edges and nowhere else, in rings across it like the loops of
 * a hand-made fix: what a split cuts across a sliver is never tested itself, a sliver splits
 * both of its long edges or neither, and a cut that has come to run the length of its sliver is
 * turned to run across it. Without the three, every corner a fan starts at grows hairlines of
 * snow, a sliver wide, pointing in under the roof.
 *
 * The corners of every triangle tested, and every vertex added, carry the openness measured at
 * their own spot, and are reported as such: that is what those surfaces are to be masked by.
 * ShelterMap's settling is for vertices that have to carry a fade no vertex was put in for - a
 * strip too narrow to test, a shape this class is not let at - and would move these to mend
 * triangles that are no longer there: a floor's rim pulled down for the sake of its middle.
 *
 * One kind of vertex is not reported, tested or not: the one whose spot reads otherwise along
 * the tilt of a triangle it was tested with than along its own. A vertex is read along the tilt
 * of its normal, and where a mesh's normals are bent away from its faces - smoothed over the rim
 * of a ledge, tipped up on a wall so that it catches the light - that is the tilt of a surface
 * that is not there: a wall under an overhang, read as a hillside open to the sky. Such a
 * vertex measures nothing its triangles show, and is left to the settling as it always was.
 *
 * Splits are decided per geometric edge - by the positions of its ends, not by their indices -
 * so that the two triangles sharing an edge, and the duplicated vertices of a seam between two
 * plank strips, split at the same point and stay watertight. Triangles that cannot hold snow (a
 * plank's side, a wall) are never split, not even to conform with a split neighbor: the seam
 * they leave is one the hand-made fixes leave too (their floor top is a shape of its own), and
 * splitting them would double the vertex count for a surface that shows no snow.
 *
 * Every added vertex is a convex combination of the model's own vertices, from which its
 * attributes are interpolated when the vertex buffer is built (ProjectedVertexData); its
 * position is the model space split point itself, computed in the same order on both sides of
 * a seam so that the two copies are bit for bit the same.
 */
class ShelterRefinement {
public:
    ShelterRefinement() = delete;

    /**
     * @brief A vertex added to a shape: a convex combination of up to three of the model's own
     */
    struct Vertex {
        std::array<std::uint16_t, 3> source {}; /**< Model vertex indices; an unused slot repeats the first */
        std::array<float, 3> weight {}; /**< Their weights, summing to 1; 0 in an unused slot */
        RE::NiPoint3 position; /**< Model space position */
    };

    struct Result {
        std::vector<Vertex> added; /**< Appended after the model's vertices, in index order */
        std::vector<std::uint16_t> indices; /**< The whole refined triangle list */
        std::size_t probes {}; /**< Field lookups spent, for the log */
    };

    /**
     * @brief How far the refinement may go
     */
    struct Limits {
        float tolerance {}; /**< Openness the interpolation may be off by before a split is worth a vertex */
        float minEdge {}; /**< The shortest edge a split may leave behind, in world units */
        float minHeight {}; /**< The narrowest triangle (its height over its longest edge) worth testing for
                               itself: a sliver - a rope, the side of a plank, the edge of a beam - has no room
                               across it to show a wedge, however far off its openness runs along it. A sliver
                               of a surface at least twice as wide is tested all the same */
        int maxRounds {}; /**< Rounds of splitting; each round halves an edge at most */
        std::size_t maxAddedVertices {}; /**< Vertices that may be added to the shape. A round that would go past
                                            it (or past maxTriangles) splits the edges that are furthest off, as
                                            many as there is room for, and is the last */
        std::size_t maxTriangles {}; /**< Triangles the refined shape may have in all */
    };

    constexpr static float K_TOLERANCE = 0.1F; /**< Of the way from bare to snowed. On the steepest part of the fade
                                                  this moves the snow's edge by a fifteenth of the fade. The
                                                  lattice locates a drip line no better than that, but what shows
                                                  is not where the edge is: it is two triangles, or two pieces of
                                                  a walkway, disagreeing about it, and at 0.15 the fan a floor is
                                                  cut into showed as notches along the edge */
    constexpr static float K_MIN_EDGE = 16.0F; /**< Half a lattice spacing: the field itself resolves nothing finer */
    constexpr static float K_MIN_HEIGHT = 12.0F; /**< Narrower than a plank, wider than a rope or a beam's edge */
    constexpr static int K_MAX_ROUNDS = 6; /**< Enough to bring a 512 unit edge down to the minimum */
    constexpr static std::size_t K_MAX_ADDED_VERTICES = 4096; /**< Per shape, whatever its size */

    /**
     * @brief Refines a shape's triangle list against a field
     *
     * The three per vertex arrays are extended in place with the added vertices, so that the
     * caller's settling and tallying see the refined mesh as they would any other.
     *
     * @param field The 3x3 block of height maps around the shape
     * @param modelPositions Model space positions of the shape's own vertices
     * @param positions World space positions of the shape's own vertices; the added ones are appended
     * @param normals World space normals, one per position, or empty for a mesh without them; appended alike
     * @param openness Per vertex, from Field::initialOpenness; the added vertices come with theirs
     * @param measured Out: one per position, the added ones included - whether the vertex is a
     *        corner of a triangle that was tested, or was added, and reads the same along the
     *        tilt of those triangles as along its own: its openness is to stand as it is
     *        (Field::settleOpenness). Filled in whether anything was split or not
     * @param indices The shape's triangle list
     * @param holdsSnowFrom The facing (dot(normal, up)) from which a vertex can hold snow: only
     *        triangles with such a corner are refined
     * @param fade The profile's shelterFade
     * @param limits How far to go
     * @return std::optional<Result> std::nullopt when nothing was split (the three arrays are then
     *         unchanged)
     */
    [[nodiscard]] static auto refine(const ShelterMap::Field& field,
                                     std::span<const RE::NiPoint3> modelPositions,
                                     std::vector<RE::NiPoint3>& positions,
                                     std::vector<RE::NiPoint3>& normals,
                                     std::vector<float>& openness,
                                     std::vector<bool>& measured,
                                     std::span<const std::uint16_t> indices,
                                     float holdsSnowFrom,
                                     float fade,
                                     const Limits& limits) -> std::optional<Result>;
};

} // namespace XPMF
