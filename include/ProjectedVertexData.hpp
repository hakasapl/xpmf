#pragma once

#include "ShelterRefinement.hpp"

#include "PCH.h"

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <span>
#include <unordered_map>

namespace XPMF {

/**
 * @brief Builds and shares the replacement vertex data that carries this plugin's vertex colors
 *
 * The lighting shader hands a shape's vertex color to the pixel shader whole: rgb tints the
 * finished pixel - the projected snow or ash included, which is what keeps a matched material
 * from matching on the three quarters of vanilla snow shapes whose colors darken snow-facing
 * vertices - and alpha scales the projection weight (dot(normal, up) * alpha against the
 * material's threshold), which is the mask one in five of those shapes already uses and the
 * channel the roof shelter scales. Both live in the shape's one interleaved vertex buffer,
 * shared by every clone of the model, so changing either for projection-carrying clones only
 * means giving those clones a buffer of their own: a "variant" of the source renderer data with
 * identical positions and topology (the index buffer is shared outright) and different colors.
 *
 * Variants are built through the engine's own CreateTriShape and freed by its own release,
 * so they are ordinary renderer data to everything else - the decal builder included, which
 * is why each gets a copy of the source's CPU index list as well.
 *
 * Most shapes with a projection on them share one variant per model and profile setting: white
 * colors (a profile's neutralizeVertexColors), and the alpha either as the mesh has it - a mask
 * its author painted stays a mask - or reset to 1 (neutralizeVertexAlpha), the projection then
 * covering the whole shape; each setting except on the statics its skip list names (see
 * ProjectedGeometry). Only a shape partly under cover gets a private variant, because only
 * there does the alpha depend on where the instance stands: whichever of the two it starts
 * from, multiplied down towards nothing where the cover is deep; one entirely under cover gets
 * no variant at all - its projection is switched off and it goes back to the model's own data.
 *
 * A private variant may also be "refined": where the profile asks for it (roofShelterFixVertices)
 * and the mesh is too coarse for its cover, ShelterRefinement adds vertices along the drip line
 * and through the fade, and the variant then has vertices the model has not and a triangle list
 * of its own - an index buffer of its own too, an ordinary D3D buffer the renderer data takes
 * its reference on like the shared one. The added vertices are interpolated from the model's
 * (positions, uvs, normals, tangents, colors alike), so to the shader the surface is the same
 * surface with more corners to carry alpha. What differs for the shape is what it counts: a
 * BSTriShape draws by its own vertex and triangle counts, which install() therefore sets to a
 * refined variant's and puts back to the model's when anything else goes in.
 *
 * Whitening is all or nothing per shape. Which vertices end up under snow depends on the
 * instance's orientation, the static's angle, the material and the noise, and guessing at that
 * per vertex buys little: a shape with a projection on it has white colors, everywhere.
 *
 * The registry keeps one reference on every variant and one on its source. The second pins
 * the source's address, which is the cache key; the first means a variant whose count is back
 * to 1 has no users left, which is how collectGarbage finds what to free.
 */
class ProjectedVertexData {
public:
    ProjectedVertexData() = delete;

    using Data = RE::BSGraphics::TriShape; /**< The engine's renderer data of a shape: its vertex and index
                                              buffers, with the CPU copies it keeps of them */

    /**
     * @brief What is known about the shape the data belongs to
     */
    struct Shape {
        Data* source {}; /**< The model's original renderer data (see sourceOf) */
        std::uint32_t vertexCount {}; /**< Vertices in the model's shape */
        std::uint32_t triangleCount {}; /**< Triangles in the model's shape */
        bool colorsEnabled {}; /**< Whether the shader's Vertex_Colors flag is set. When it is not, the colors
                                  were never shown and may hold anything, so a variant (which only makes sense
                                  with the flag turned on) has all of them white */
        bool keepAlpha {}; /**< Whether the mesh's vertex alpha is transparency: an alpha property that blends
                              (snow drifts fading into the ground, ...) or tests an alpha the mesh paints. Such
                              a shape keeps its alpha whatever roofShelter or neutralizeVertexAlpha says: scaling
                              it would fade the shape itself, resetting it would make it solid. A shape that only
                              alpha tests an unpainted alpha is not one of these (its test threshold follows its
                              alpha instead, see ProjectedGeometry::scaledAlphaThreshold), and neither is one
                              with the Vertex_Alpha shader flag and no alpha property: the flag alone shows
                              nothing through */
        // The profile's geometry settings, each for a static its skip list does not name
        // (ProjectedGeometry::settingsOf)
        bool neutralize {}; /**< neutralizeVertexColors: colors the shader shows become white */
        bool shelter {}; /**< roofShelter: whether cover is measured for the shape at all */
        bool neutralizeAlpha {}; /**< neutralizeVertexAlpha: the alpha starts from 1 rather than from the mesh's
                                    own - on a shape that shows its colors and whose alpha is not transparency
                                    (keepAlpha) */
        bool fixVertices {}; /**< roofShelterFixVertices: a shape partly under cover may be given the vertices its
                                cover needs (ShelterRefinement); with shelter, and never on one that keeps its
                                alpha or draws a LOD prefix of its triangles */
    };

    /**
     * @brief What a shape draws: its vertex and triangle counts
     */
    struct Counts {
        std::uint16_t vertices {};
        std::uint16_t triangles {};
    };

    /**
     * @brief Returns the shared variant of a source, building it on first use
     *
     * Thread safe; called from the loader threads (Clone3D) and the worker.
     *
     * @return Data* Renderer data carrying one reference for the caller - the source itself when
     *         the variant would be identical to it - or nullptr when the layout is not
     *         supported or the engine failed to create the buffer (keep what the shape has)
     */
    [[nodiscard]] static auto shared(const Shape& shape) -> Data*;

    /**
     * @brief Builds a private variant with the shelter in its alpha (colors as for the shared one)
     *
     * @param values Per vertex, 0..255 for 0..1: what the alpha the shape starts from (the mesh's
     *        own, or 1 with neutralizeAlpha) is multiplied by
     * @return Data* As for shared(); also nullptr once K_BUDGET is spent
     */
    [[nodiscard]] static auto custom(const Shape& shape,
                                     std::span<const std::uint8_t> values) -> Data*;

    /**
     * @brief Builds a private variant with the shelter in its alpha and the vertices ShelterRefinement
     * added, on the refined triangle list
     *
     * @param refinement The added vertices and the triangle list, from ShelterRefinement::refine
     * @param values As for custom(), one per vertex of the refined shape: the model's first, the
     *        added ones after them
     * @return Data* New data carrying one reference for the caller, never the source; nullptr as
     *         for custom(), or when the refined shape would exceed what a BSTriShape counts
     */
    [[nodiscard]] static auto customRefined(const Shape& shape,
                                            const ShelterRefinement::Result& refinement,
                                            std::span<const std::uint8_t> values) -> Data*;

    /**
     * @brief Resolves renderer data to the model's original: itself, unless it is a variant
     */
    [[nodiscard]] static auto sourceOf(Data* data) -> Data*;

    /**
     * @brief Whether renderer data is a variant built for a shape whose Vertex_Colors flag was off
     *
     * Such a shape only shows colors because this plugin switched them on for the variant's sake,
     * and has to stop showing them when it goes back to the model's own data.
     */
    [[nodiscard]] static auto isForColorlessShape(const Data* data) -> bool;

    /**
     * @brief What a refined variant draws; std::nullopt for anything that is not one
     */
    [[nodiscard]] static auto countsOf(const Data* data) -> std::optional<Counts>;

    /**
     * @brief What the model of a refined variant draws, i.e. what a shape holding the variant has
     * to count again once it goes back to anything else; std::nullopt for anything that is not one
     */
    [[nodiscard]] static auto sourceCountsOf(const Data* data) -> std::optional<Counts>;

    /**
     * @brief Fingerprint of the per vertex values a private variant was built from; 0 for anything else
     */
    [[nodiscard]] static auto fingerprintOf(const Data* data) -> std::uint64_t;

    /**
     * @brief Fingerprint custom() would store for these values
     */
    [[nodiscard]] static auto fingerprint(std::span<const std::uint8_t> values) -> std::uint64_t;

    /**
     * @brief Fingerprint customRefined() would store for these values on this refinement
     */
    [[nodiscard]] static auto fingerprintRefined(std::span<const std::uint8_t> values,
                                                 const ShelterRefinement::Result& refinement) -> std::uint64_t;

    /**
     * @brief One more reference on renderer data (the engine's own count)
     */
    static void addRef(Data* data);

    /**
     * @brief One reference less; at zero the engine frees the data, D3D buffers and CPU copies alike
     */
    static void release(Data* data);

    /**
     * @brief Puts renderer data into a shape, releasing what it held; consumes the caller's reference
     *
     * A refined variant brings its own vertex and triangle counts into the shape, and a shape
     * that leaves one gets the model's back: the renderer draws a BSTriShape by its own counts.
     *
     * Only where nothing else touches the shape: on a clone not yet handed to the scene, or on
     * the main thread.
     */
    static void install(RE::BSTriShape& shape,
                        Data* data);

    /**
     * @brief Frees every variant nothing uses any more; main thread
     */
    static void collectGarbage();

private:
    constexpr static std::size_t K_BUDGET = std::size_t {256} * 1024 * 1024; /**< Bytes of private variant vertex
                                                                                and index data (held twice: once
                                                                                by the GPU, once as the engine's
                                                                                CPU copy) before custom() declines */

    /**
     * @brief How a variant's colors are derived from the source's
     */
    struct Recipe {
        bool whiten {}; /**< rgb = white; otherwise the mesh's */
        bool resetAlpha {}; /**< alpha = 1 to begin with: the mesh's was never shown (Vertex_Colors off) and may
                               hold anything, or the profile neutralizes it; otherwise the mesh's own... */
        std::span<const std::uint8_t> values; /**< ...multiplied by these, one per vertex, 0..255 for 0..1; empty
                                                 leaves the alpha as it starts */
        const ShelterRefinement::Result* refinement {}; /**< Vertices to add and the triangle list to draw them
                                                           with; nullptr keeps the model's topology */
    };

    struct Entry {
        Data* source {}; /**< Pinned by a reference of its own */
        std::size_t bytes {}; /**< Vertex (and, refined, index) data size */
        std::uint64_t fingerprint {}; /**< Private variants only */
        bool isShared {};
        bool colorless {}; /**< Built for a shape whose Vertex_Colors flag was off */
        bool refined {}; /**< Has vertices and triangles of its own (customRefined) */
        Counts counts; /**< What it draws; refined only */
        Counts sourceCounts; /**< What its model draws; refined only */
    };

    struct SharedKey {
        const Data* source {};
        bool whiten {}; /**< One model can stand under the materials of two profiles, one of each setting... */
        bool resetAlpha {}; /**< ...and under two statics of one profile, one of which its skip list names */
        auto operator==(const SharedKey&) const -> bool = default;
    };

    struct SharedKeyHash {
        auto operator()(const SharedKey& key) const noexcept -> std::size_t;
    };

    /**
     * @brief Builds renderer data for a recipe
     *
     * @return Data* New data with a reference count of 1, the source (count untouched) when
     *         nothing would differ, or nullptr on failure
     */
    [[nodiscard]] static auto build(const Shape& shape,
                                    const Recipe& recipe) -> Data*;

    static inline std::mutex s_lock; /**< Guards everything below */
    static inline std::unordered_map<const Data*, Entry> s_variants; /**< Every live variant */
    static inline std::unordered_map<SharedKey, Data*, SharedKeyHash> s_shared; /**< Shared ones by what they are of */
    static inline std::size_t s_privateBytes = 0; /**< Against K_BUDGET */
};

} // namespace XPMF
