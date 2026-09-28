#include "ShelterRefinement.hpp"

#include "ShelterMap.hpp"

#include "PCH.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <optional>
#include <span>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

using namespace XPMF;

namespace {

using Field = ShelterMap::Field;
using Slope = ShelterMap::Slope;

constexpr float K_KEY_SCALE = 8.0F; /**< Points are told apart to an eighth of a unit: the duplicated vertices of
                                       one mesh agree to far better than that, and nothing that close is ever
                                       meant to be apart */
constexpr std::uint32_t K_NONE = std::numeric_limits<std::uint32_t>::max();
constexpr int K_ROUNDS_CAP = 8; /**< Rounds of splitting, whatever the limits say */
constexpr std::size_t K_MAX_VERTICES = std::numeric_limits<std::uint16_t>::max(); /**< What a BSTriShape counts to */

/**
 * @brief The identity of a point across duplicated vertices: its position, quantized
 */
struct PointKey {
    std::int64_t x {};
    std::int64_t y {};
    std::int64_t z {};

    auto operator==(const PointKey&) const -> bool = default;

    [[nodiscard]] auto operator<(const PointKey& other) const -> bool
    {
        return std::tie(x, y, z) < std::tie(other.x, other.y, other.z);
    }

    [[nodiscard]] static auto of(const RE::NiPoint3& point) -> PointKey
    {
        return {.x = std::llround(point.x * K_KEY_SCALE),
                .y = std::llround(point.y * K_KEY_SCALE),
                .z = std::llround(point.z * K_KEY_SCALE)};
    }
};

struct PointKeyHash {
    auto operator()(const PointKey& key) const noexcept -> std::size_t
    {
        constexpr std::uint64_t GOLDEN = 0x9E3779B97F4A7C15ULL;
        std::uint64_t hash = static_cast<std::uint64_t>(key.x) * GOLDEN;
        hash ^= (static_cast<std::uint64_t>(key.y) + GOLDEN + (hash << 6U) + (hash >> 2U));
        hash ^= (static_cast<std::uint64_t>(key.z) + GOLDEN + (hash << 6U) + (hash >> 2U));
        return static_cast<std::size_t>(hash);
    }
};

/**
 * @brief The identity of an edge across the triangles that share it: its two points, in order
 */
struct EdgeKey {
    PointKey first;
    PointKey second;
    auto operator==(const EdgeKey&) const -> bool = default;
};

struct EdgeKeyHash {
    auto operator()(const EdgeKey& key) const noexcept -> std::size_t
    {
        const PointKeyHash hash;
        return hash(key.first) ^ (hash(key.second) * 0x100000001B3ULL);
    }
};

struct Triangle {
    std::array<std::uint32_t, 3> corner {};
    int round {}; /**< The round it was made in; a triangle is tested in the round after */
    bool alive {}; /**< Not yet split */
    bool holds {}; /**< Has a corner that can hold snow, so its interpolation shows */
};

/**
 * @brief A vertex as a convex combination of the model's own
 */
struct Blend {
    std::array<std::uint16_t, 3> source {};
    std::array<float, 3> weight {};
};

auto normalized(const RE::NiPoint3& vector) -> RE::NiPoint3
{
    const float length = vector.Length();
    constexpr float MIN_LENGTH = 1.0e-6F;
    return length > MIN_LENGTH ? vector * (1.0F / length) : vector;
}

/**
 * @brief Does the splitting for one shape; one instance per call to refine
 */
class Refiner {
public:
    Refiner(const Field& field,
            std::span<const RE::NiPoint3> model,
            std::vector<RE::NiPoint3>& positions,
            std::vector<RE::NiPoint3>& normals,
            std::vector<float>& openness,
            float holdsSnowFrom,
            float fade,
            const ShelterRefinement::Limits& limits)
        : m_field(field)
        , m_model(model)
        , m_positions(positions)
        , m_normals(normals)
        , m_openness(openness)
        , m_holdsSnowFrom(holdsSnowFrom)
        , m_fade(fade)
        , m_limits(limits)
        , m_sourceCount(positions.size())
    {
        m_keys.reserve(positions.size());
        for (const auto& position : positions) {
            m_keys.push_back(PointKey::of(position));
        }
    }

    [[nodiscard]] auto run(std::span<const std::uint16_t> indices) -> std::optional<ShelterRefinement::Result>
    {
        m_triangles.reserve(indices.size() / 3 * 2);
        bool anyHolder = false;
        for (std::size_t at = 0; at + 2 < indices.size(); at += 3) {
            Triangle triangle {.corner = {indices[at], indices[at + 1], indices[at + 2]},
                               .round = 0,
                               .alive = true,
                               .holds = false};
            const bool valid = std::ranges::all_of(triangle.corner,
                                                   [&](std::uint32_t index) -> bool { return index < m_sourceCount; });
            triangle.holds = valid && holdsSnow(triangle.corner);
            anyHolder = anyHolder || triangle.holds;
            m_triangles.push_back(triangle);
        }
        if (!anyHolder) {
            return std::nullopt;
        }

        const int rounds = std::min(m_limits.maxRounds, K_ROUNDS_CAP);
        std::size_t added = 0;
        std::size_t alive = m_triangles.size();
        bool anySplit = false;
        for (int round = 0; round < rounds; ++round) {
            m_marks.clear();
            m_seen.clear();
            for (std::size_t index = 0; index < m_triangles.size(); ++index) {
                const Triangle& triangle = m_triangles[index];
                if (triangle.alive && triangle.holds && triangle.round == round) {
                    evaluate(triangle);
                }
            }
            if (m_marks.empty()) {
                break;
            }

            // Every snow holding triangle that shares a marked edge splits it - the ones tested
            // this round and any older neighbor alike - which keeps the refined surface watertight.
            // What that comes to is counted first, and a round that would go over the budget is
            // not begun at all: half a round would leave seams
            std::vector<std::size_t> toSplit;
            std::unordered_set<std::uint64_t> records;
            std::size_t newTriangles = 0;
            for (std::size_t index = 0; index < m_triangles.size(); ++index) {
                const Triangle& triangle = m_triangles[index];
                if (!triangle.alive || !triangle.holds) {
                    continue;
                }
                std::size_t marked = 0;
                for (std::size_t edge = 0; edge < 3; ++edge) {
                    const std::uint32_t a = triangle.corner[edge];
                    const std::uint32_t b = triangle.corner[(edge + 1) % 3];
                    if (m_marks.contains(edgeKeyOf(a, b))) {
                        ++marked;
                        records.insert(pairKeyOf(a, b));
                    }
                }
                if (marked > 0) {
                    toSplit.push_back(index);
                    newTriangles += marked;
                }
            }
            if (added + records.size() > m_limits.maxAddedVertices || alive + newTriangles > m_limits.maxTriangles
                || m_positions.size() + records.size() > K_MAX_VERTICES) {
                break;
            }

            m_records.clear();
            for (const std::size_t index : toSplit) {
                split(index, round + 1);
            }
            added += records.size();
            alive += newTriangles;
            anySplit = true;
        }
        if (!anySplit) {
            return std::nullopt;
        }

        ShelterRefinement::Result result;
        result.added = std::move(m_added);
        result.indices.reserve(alive * 3);
        for (const Triangle& triangle : m_triangles) {
            if (triangle.alive) {
                for (const std::uint32_t index : triangle.corner) {
                    result.indices.push_back(static_cast<std::uint16_t>(index));
                }
            }
        }
        result.probes = m_probes;
        return result;
    }

private:
    [[nodiscard]] auto holdsSnow(const std::array<std::uint32_t, 3>& corners) const -> bool
    {
        if (m_normals.empty()) {
            return true; // without normals every vertex is read as facing up
        }
        return std::ranges::any_of(corners,
                                   [&](std::uint32_t index) -> bool { return m_normals[index].z >= m_holdsSnowFrom; });
    }

    /**
     * @brief The two ends of an edge in a fixed order - by position, so that both triangles sharing
     * the edge, and both copies of a duplicated one, compute the same points along it
     */
    [[nodiscard]] auto oriented(std::uint32_t a,
                                std::uint32_t b) const -> std::pair<std::uint32_t, std::uint32_t>
    {
        return m_keys[a] < m_keys[b] ? std::pair {a, b} : std::pair {b, a};
    }

    [[nodiscard]] auto edgeKeyOf(std::uint32_t a,
                                 std::uint32_t b) const -> EdgeKey
    {
        const auto [first, second] = oriented(a, b);
        return {.first = m_keys[first], .second = m_keys[second]};
    }

    [[nodiscard]] static auto pairKeyOf(std::uint32_t a,
                                        std::uint32_t b) -> std::uint64_t
    {
        return (static_cast<std::uint64_t>(std::min(a, b)) << 32U) | std::max(a, b);
    }

    [[nodiscard]] auto probe(const RE::NiPoint3& point,
                             const Slope& slope) -> float
    {
        // Cached by position: the midpoint of a shared edge is asked for by both of its triangles,
        // and the two copies of a seam have to get one answer
        const PointKey key = PointKey::of(point);
        if (const auto found = m_probed.find(key); found != m_probed.end()) {
            return found->second;
        }
        ++m_probes;
        const float value = m_field.opennessAt(point, slope, m_fade);
        m_probed.emplace(key, value);
        return value;
    }

    [[nodiscard]] auto pointAlong(std::uint32_t first,
                                  std::uint32_t second,
                                  float t) const -> RE::NiPoint3
    {
        return m_positions[first] + ((m_positions[second] - m_positions[first]) * t);
    }

    [[nodiscard]] auto normalAlong(std::uint32_t first,
                                   std::uint32_t second,
                                   float t) const -> RE::NiPoint3
    {
        return normalized((m_normals[first] * (1.0F - t)) + (m_normals[second] * t));
    }

    [[nodiscard]] auto probeAlong(std::uint32_t first,
                                  std::uint32_t second,
                                  float t) -> float
    {
        const Slope slope = m_normals.empty() ? Slope {} : Slope::of(normalAlong(first, second, t));
        return probe(pointAlong(first, second, t), slope);
    }

    /**
     * @brief Marks the edges of a triangle along which the interpolated openness is wrong, and the
     * longest one when only its middle is
     */
    void evaluate(const Triangle& triangle)
    {
        const auto& corner = triangle.corner;
        const RE::NiPoint3& a = m_positions[corner[0]];
        const RE::NiPoint3& b = m_positions[corner[1]];
        const RE::NiPoint3& c = m_positions[corner[2]];
        float longest = 0.0F;
        std::size_t longestEdge = 0;
        for (std::size_t edge = 0; edge < 3; ++edge) {
            const float length = (m_positions[corner[edge]] - m_positions[corner[(edge + 1) % 3]]).Length();
            if (length > longest) {
                longest = length;
                longestEdge = edge;
            }
        }
        // Too short to split, or too narrow to show anything: twice the area over the longest
        // edge is the triangle's height across it
        if (longest < 2.0F * m_limits.minEdge || (b - a).Cross(c - a).Length() < m_limits.minHeight * longest) {
            return;
        }

        bool anyMarked = false;
        for (std::size_t edge = 0; edge < 3; ++edge) {
            const std::uint32_t from = corner[edge];
            const std::uint32_t to = corner[(edge + 1) % 3];
            const float length = (m_positions[from] - m_positions[to]).Length();
            const EdgeKey key = edgeKeyOf(from, to);
            if (m_marks.contains(key)) {
                anyMarked = true;
                continue;
            }
            if (!m_seen.insert(key).second || length < 2.0F * m_limits.minEdge) {
                continue; // already found straight enough this round, or too short to split
            }

            // The edge is sampled at its midpoint, and on a long one at the quarters as well, and
            // split where the sample is farthest from the line between its ends: on a plank that
            // runs from the open deep in under a roof that is where the fade band lies, not the
            // middle, and one split there saves the two it would take to get there by halving
            const auto [first, second] = oriented(from, to);
            constexpr std::array<float, 3> SAMPLES {0.5F, 0.25F, 0.75F}; // the midpoint first: ties go to it
            // The quarters only where they can tell anything: on a long edge whose ends differ,
            // where the fade band lies to one side of the middle. An edge whose ends agree is
            // either flat (deep under cover, out in the open) or symmetric, and its midpoint says
            // it all - and those edges are most of any mesh
            constexpr float SAME = 1.0e-3F;
            const bool endsDiffer = std::abs(m_openness[first] - m_openness[second]) > SAME;
            const std::size_t count = endsDiffer && length >= 4.0F * m_limits.minEdge ? SAMPLES.size() : 1;
            float worst = m_limits.tolerance;
            float where = -1.0F;
            for (std::size_t sample = 0; sample < count; ++sample) {
                const float t = SAMPLES.at(sample);
                const float interpolated = m_openness[first] + ((m_openness[second] - m_openness[first]) * t);
                const float deviation = std::abs(probeAlong(first, second, t) - interpolated);
                if (deviation > worst) {
                    worst = deviation;
                    where = t;
                }
            }
            if (where >= 0.0F) {
                m_marks.emplace(key, where);
                anyMarked = true;
            }
        }
        if (anyMarked) {
            return;
        }

        // Edges all straight, the middle maybe not: a roof corner over a large triangle
        constexpr float THIRD = 1.0F / 3.0F;
        const float interpolated = (m_openness[corner[0]] + m_openness[corner[1]] + m_openness[corner[2]]) * THIRD;
        const float measured = probe((a + b + c) * THIRD, Slope::ofTriangle(a, b, c));
        if (std::abs(measured - interpolated) > m_limits.tolerance) {
            constexpr float HALF = 0.5F;
            m_marks.emplace(edgeKeyOf(corner[longestEdge], corner[(longestEdge + 1) % 3]), HALF);
        }
    }

    [[nodiscard]] auto modelOf(std::uint32_t index) const -> const RE::NiPoint3&
    {
        return index < m_sourceCount ? m_model[index] : m_added[index - m_sourceCount].position;
    }

    [[nodiscard]] auto blendOf(std::uint32_t index) const -> Blend
    {
        if (index < m_sourceCount) {
            const auto source = static_cast<std::uint16_t>(index);
            return {.source = {source, source, source}, .weight = {1.0F, 0.0F, 0.0F}};
        }
        const auto& added = m_added[index - m_sourceCount];
        return {.source = added.source, .weight = added.weight};
    }

    /**
     * @brief The blend of two blends; a point on an edge of one of the model's triangles, of which
     * every vertex created inside that triangle is a combination, so three sources suffice
     */
    [[nodiscard]] static auto mix(const Blend& a,
                                  float weightA,
                                  const Blend& b,
                                  float weightB) -> Blend
    {
        std::array<std::uint16_t, 3> source {};
        std::array<float, 3> weight {};
        std::size_t count = 0;
        const auto add = [&](std::uint16_t index, float value) -> void {
            if (value <= 0.0F) {
                return;
            }
            for (std::size_t slot = 0; slot < count; ++slot) {
                if (source.at(slot) == index) {
                    weight.at(slot) += value;
                    return;
                }
            }
            if (count < source.size()) {
                source.at(count) = index;
                weight.at(count) = value;
                ++count;
                return;
            }
            // Cannot happen on a triangle mesh; if it does, the lightest source gives way
            const auto lightest = static_cast<std::size_t>(
                std::distance(weight.begin(), std::ranges::min_element(weight)));
            if (value > weight.at(lightest)) {
                source.at(lightest) = index;
                weight.at(lightest) = value;
            }
        };
        for (std::size_t slot = 0; slot < 3; ++slot) {
            add(a.source.at(slot), a.weight.at(slot) * weightA);
        }
        for (std::size_t slot = 0; slot < 3; ++slot) {
            add(b.source.at(slot), b.weight.at(slot) * weightB);
        }
        // Normalized, and the unused slots made harmless
        float total = 0.0F;
        for (std::size_t slot = 0; slot < count; ++slot) {
            total += weight.at(slot);
        }
        for (std::size_t slot = 0; slot < 3; ++slot) {
            if (slot < count) {
                weight.at(slot) = total > 0.0F ? weight.at(slot) / total : 0.0F;
            } else {
                source.at(slot) = source[0];
                weight.at(slot) = 0.0F;
            }
        }
        if (count == 0) {
            weight[0] = 1.0F;
        }
        return {.source = source, .weight = weight};
    }

    /**
     * @brief The vertex an edge is split at, made once per pair of end vertices
     */
    [[nodiscard]] auto splitVertex(std::uint32_t a,
                                   std::uint32_t b,
                                   float t) -> std::uint32_t
    {
        const std::uint64_t pair = pairKeyOf(a, b);
        if (const auto found = m_records.find(pair); found != m_records.end()) {
            return found->second;
        }
        const auto [first, second] = oriented(a, b);
        const auto index = static_cast<std::uint32_t>(m_positions.size());

        // The same expression on both sides of a seam, from the same numbers: the same point
        const RE::NiPoint3 world = pointAlong(first, second, t);
        const RE::NiPoint3 model = modelOf(first) + ((modelOf(second) - modelOf(first)) * t);
        Slope slope;
        if (!m_normals.empty()) {
            const RE::NiPoint3 normal = normalAlong(first, second, t);
            slope = Slope::of(normal);
            m_normals.push_back(normal);
        }
        const float openness = probe(world, slope);
        m_positions.push_back(world);
        m_keys.push_back(PointKey::of(world));
        m_openness.push_back(openness);
        const Blend blend = mix(blendOf(first), 1.0F - t, blendOf(second), t);
        m_added.push_back({.source = blend.source, .weight = blend.weight, .position = model});
        m_records.emplace(pair, index);
        return index;
    }

    /**
     * @brief Replaces a triangle by the two, three or four its marked edges cut it into
     */
    void split(std::size_t index,
               int round)
    {
        const Triangle parent = m_triangles[index]; // a copy: the vector grows below
        std::array<std::uint32_t, 3> v = parent.corner;
        std::array<std::uint32_t, 3> m {K_NONE, K_NONE, K_NONE};
        std::size_t marked = 0;
        std::size_t single = 0; // the one marked edge, or the one unmarked
        for (std::size_t edge = 0; edge < 3; ++edge) {
            const auto found = m_marks.find(edgeKeyOf(v[edge], v[(edge + 1) % 3]));
            if (found != m_marks.end()) {
                m[edge] = splitVertex(v[edge], v[(edge + 1) % 3], found->second);
                ++marked;
            }
        }
        if (marked == 0) {
            return;
        }
        m_triangles[index].alive = false;

        // Rotate so that the one marked edge is the first, or the one unmarked the last; the
        // children keep the parent's winding
        std::size_t rotation = 0;
        if (marked == 1) {
            for (std::size_t edge = 0; edge < 3; ++edge) {
                if (m[edge] != K_NONE) {
                    single = edge;
                }
            }
            rotation = single;
        } else if (marked == 2) {
            for (std::size_t edge = 0; edge < 3; ++edge) {
                if (m[edge] == K_NONE) {
                    single = edge;
                }
            }
            rotation = (single + 1) % 3;
        }
        const auto rotate = [rotation](const std::array<std::uint32_t, 3>& array) -> std::array<std::uint32_t, 3> {
            return {array[rotation % 3], array[(rotation + 1) % 3], array[(rotation + 2) % 3]};
        };
        v = rotate(v);
        m = rotate(m);

        const auto child = [&](std::uint32_t a, std::uint32_t b, std::uint32_t c) -> void {
            const std::array<std::uint32_t, 3> corners {a, b, c};
            m_triangles.push_back({.corner = corners, .round = round, .alive = true, .holds = holdsSnow(corners)});
        };
        if (marked == 1) {
            child(v[0], m[0], v[2]);
            child(m[0], v[1], v[2]);
        } else if (marked == 2) {
            child(m[0], v[1], m[1]);
            // The quad that is left is cut along its shorter diagonal
            const float across = (m_positions[v[0]] - m_positions[m[1]]).Length();
            const float other = (m_positions[m[0]] - m_positions[v[2]]).Length();
            if (across <= other) {
                child(v[0], m[0], m[1]);
                child(v[0], m[1], v[2]);
            } else {
                child(v[0], m[0], v[2]);
                child(m[0], m[1], v[2]);
            }
        } else {
            child(v[0], m[0], m[2]);
            child(m[0], v[1], m[1]);
            child(m[2], m[1], v[2]);
            child(m[0], m[1], m[2]);
        }
    }

    const Field& m_field;
    std::span<const RE::NiPoint3> m_model;
    std::vector<RE::NiPoint3>& m_positions;
    std::vector<RE::NiPoint3>& m_normals;
    std::vector<float>& m_openness;
    float m_holdsSnowFrom;
    float m_fade;
    const ShelterRefinement::Limits& m_limits;
    std::size_t m_sourceCount;

    std::vector<PointKey> m_keys; /**< One per position */
    std::vector<Triangle> m_triangles;
    std::vector<ShelterRefinement::Vertex> m_added;
    std::unordered_map<PointKey, float, PointKeyHash> m_probed; /**< Openness by sample point */
    std::unordered_map<EdgeKey, float, EdgeKeyHash> m_marks; /**< This round's edges to split, and where along them */
    std::unordered_set<EdgeKey, EdgeKeyHash> m_seen; /**< This round's edges already found straight */
    std::unordered_map<std::uint64_t, std::uint32_t> m_records; /**< This round's split vertices by end vertex pair */
    std::size_t m_probes = 0;
};

} // namespace

auto ShelterRefinement::refine(const ShelterMap::Field& field,
                               std::span<const RE::NiPoint3> modelPositions,
                               std::vector<RE::NiPoint3>& positions,
                               std::vector<RE::NiPoint3>& normals,
                               std::vector<float>& openness,
                               std::span<const std::uint16_t> indices,
                               float holdsSnowFrom,
                               float fade,
                               const Limits& limits) -> std::optional<Result>
{
    if (indices.size() < 3 || positions.empty() || modelPositions.size() != positions.size()
        || openness.size() != positions.size() || (!normals.empty() && normals.size() != positions.size())
        || limits.maxAddedVertices == 0 || limits.maxRounds <= 0) {
        return std::nullopt;
    }
    Refiner refiner(field, modelPositions, positions, normals, openness, holdsSnowFrom, fade, limits);
    return refiner.run(indices);
}
