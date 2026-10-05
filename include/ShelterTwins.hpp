#pragma once

#include "ShelterRefinement.hpp"

#include "PCH.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

namespace XPMF {

/**
 * @brief Shapes of one model that draw the same triangles, and how a refinement of one is carried
 * to the others
 *
 * The lighting shader gives a shape one material, so a game mesh carries dirt, moss, rubble or ice
 * on part of a surface as a second shape cut from the same faces: a copy with a texture of its own
 * and a painted vertex alpha for a mask (Castle Volkihar's stairs under their rubble, the hay of a
 * hay mound, the exposed wood of a fallen aspen). The copy works because both shapes rasterize to
 * the same depth, which they only do while their triangulation is the same: re-triangulate one
 * (ShelterRefinement) and the pair z-fights wherever the overlay shows. So where one of them is
 * refined, every shape of the model drawing its triangles is cut the same way - the same split
 * points, bit for bit, on the same triangles, with its own attributes interpolated across them
 * (ProjectedVertexData) - and copies of one surface stay copies. A decal flagged overlay is drawn
 * with a depth bias and needs none of this; the caller leaves such shapes out.
 *
 * Shapes are told apart by position alone, to an eighth of a unit, so that two copies of one face
 * are found whatever their vertex order, winding or duplicated vertices. Carrying a refinement over
 * presumes the two shapes share a model space (the same transform below the model's root), which
 * is what a copied face has; the caller checks that.
 */
class ShelterTwins {
public:
    ShelterTwins() = delete;

    constexpr static std::uint32_t K_NONE = std::numeric_limits<std::uint32_t>::max();

    /**
     * @brief A shape as the search sees it
     */
    struct Shape {
        std::span<const RE::NiPoint3> positions; /**< Its vertices, in a space every shape of the search shares */
        std::span<const std::uint16_t> indices; /**< Its triangle list */
    };

    /**
     * @brief Groups of shapes that draw a triangle another of the group draws too - the same three
     * corners - directly or through a third; a shape that shares nothing is in no group
     *
     * @return Indices into shapes, each group sorted, in order of their first members
     */
    [[nodiscard]] static auto groups(std::span<const Shape> shapes) -> std::vector<std::vector<std::size_t>>;

    enum class Outcome {
        kNothingToCarry, /**< No split triangle of the refined shape is drawn by the twin: it keeps its triangles */
        kCarried,
        kFailed /**< The refinement does not fit the twin (a corner off the triangle it came from, too many
                   vertices): the refined shape had better not be refined at all */
    };

    /**
     * @brief What a twin is to draw in place of the refined shape's split triangles
     */
    struct Replayed {
        Outcome outcome {Outcome::kFailed};
        ShelterRefinement::Result result; /**< The twin's added vertices, blended from its own, and its triangle list;
                                             only with kCarried */
        std::vector<std::uint32_t> addedFrom; /**< Per added vertex, the refined shape's added vertex it copies */
        std::vector<std::uint32_t> vertexFrom; /**< Per vertex of the twin's model, a vertex of the refined shape at
                                                  the same spot, or K_NONE */
    };

    /**
     * @brief Cuts a twin the way the refined shape was cut
     *
     * Every twin triangle with the corners of a split triangle of the refined shape is replaced by
     * that triangle's pieces, corner for corner by position, in the twin's own winding. The pieces'
     * added vertices become the twin's, at the very same model space position and with the same
     * blend weights over the twin's corners at the same spots, one per distinct blend; a twin
     * triangle no split triangle matches stays as it is.
     *
     * @param refinement The refined shape's, with origins
     * @param leaderPositions The refined shape's model space positions, its model's vertices only
     * @param leaderIndices The refined shape's model triangle list
     * @param twinPositions The twin's model space positions
     * @param twinIndices The twin's triangle list
     */
    [[nodiscard]] static auto replay(const ShelterRefinement::Result& refinement,
                                     std::span<const RE::NiPoint3> leaderPositions,
                                     std::span<const std::uint16_t> leaderIndices,
                                     std::span<const RE::NiPoint3> twinPositions,
                                     std::span<const std::uint16_t> twinIndices) -> Replayed;
};

} // namespace XPMF
