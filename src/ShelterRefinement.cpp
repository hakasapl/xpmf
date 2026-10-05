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
#include <numeric>
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
constexpr std::size_t K_MAX_VERTICES = std::numeric_limits<std::uint16_t>::max(); /**< What a BSTriShape counts to */
constexpr float K_CREASE = 0.9F; /**< Cosine of the angle between the planes of two triangles past which the edge
                                    they share is a crease - the rim of a plank, the corner of a beam - and one
                                    surface ends there; under it they are one surface, flat or gently curved */
constexpr float K_BROAD = 2.0F; /**< How many times the least height worth testing a surface has to be wide for its
                                   slivers to be tested: room for two such triangles side by side. The top of a
                                   beam cut into two slivers just under that height is the one strip it always
                                   was */
constexpr float K_PAIR = 0.5F; /**< The share of the tolerance past which the second long edge of a sliver is split
                                  along with the first (see Refiner::pairSlivers) */
constexpr float K_SAME_TILT = 0.05F; /**< Rise per unit, summed over both axes, within which two tilts are one: what
                                        a normal packed into bytes is off by on a flat floor. Further apart, whether
                                        a spot reads the same along both is looked up (see Refiner::confirm) */

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

    [[nodiscard]] auto operator<(const EdgeKey& other) const -> bool
    {
        return first == other.first ? second < other.second : first < other.first;
    }
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
    std::array<bool, 3> cut {}; /**< Per edge, from the corner of that index to the next: made by splitting a
                                   sliver, across it. Such an edge runs the sliver's length between its two long
                                   edges and says nothing they do not, so it is never tested: splitting it would
                                   put a vertex a unit or two from theirs, round after round */
    int round {}; /**< The round it was made in; a triangle is tested in the round after */
    bool alive {}; /**< Not yet split */
    bool holds {}; /**< Has a corner that can hold snow, so its interpolation shows */
    bool broad {}; /**< Lies on a snow holding surface wide enough to show a wrong interpolation, however narrow
                      the triangle itself: a sliver of the fan a floor is cut into (findBroadSurfaces) */
    std::uint32_t origin {}; /**< The model's triangle it descends from (Result::origins) */
};

/**
 * @brief An edge to be split this round
 */
struct Mark {
    float where {}; /**< How far along the edge, from the first of its ends (see Refiner::oriented) */
    float deviation {}; /**< How far off the interpolated openness was there; the worst go first when the budget
                           does not cover a round */
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
            std::vector<bool>& measured,
            float holdsSnowFrom,
            const ShelterMap::Fade& fade,
            const ShelterRefinement::Limits& limits)
        : m_field(field)
        , m_model(model)
        , m_positions(positions)
        , m_normals(normals)
        , m_openness(openness)
        , m_measured(measured)
        , m_holdsSnowFrom(holdsSnowFrom)
        , m_fade(fade)
        , m_limits(limits)
        , m_sourceCount(positions.size())
    {
        m_keys.reserve(positions.size());
        for (const auto& position : positions) {
            m_keys.push_back(PointKey::of(position));
        }
        m_refuted.assign(positions.size(), false);
    }

    [[nodiscard]] auto run(std::span<const std::uint16_t> indices) -> std::optional<ShelterRefinement::Result>
    {
        m_triangles.reserve(indices.size() / 3 * 2);
        bool anyHolder = false;
        for (std::size_t at = 0; at + 2 < indices.size(); at += 3) {
            Triangle triangle {.corner = {indices[at], indices[at + 1], indices[at + 2]},
                               .round = 0,
                               .alive = true,
                               .holds = false,
                               .origin = static_cast<std::uint32_t>(m_triangles.size())};
            const bool valid = std::ranges::all_of(triangle.corner,
                                                   [&](std::uint32_t index) -> bool { return index < m_sourceCount; });
            triangle.holds = valid && holdsSnow(triangle.corner);
            anyHolder = anyHolder || triangle.holds;
            m_triangles.push_back(triangle);
        }
        if (!anyHolder) {
            return std::nullopt;
        }
        findBroadSurfaces();

        std::size_t added = 0;
        std::size_t alive = m_triangles.size();
        bool anySplit = false;
        for (int round = 0; round < m_limits.maxRounds; ++round) {
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
            pairSlivers();

            // Every snow holding triangle that shares a marked edge splits it - the ones tested
            // this round and any older neighbor alike - which keeps the refined surface watertight.
            // What that comes to is counted first. A round the budget does not cover is cut down
            // to the edges that are furthest off, as many as there is room for, and is the last:
            // an edge is left out whole, in every triangle that shares it, so no seam opens
            Plan work = planRound();
            const std::size_t vertexRoom
                = std::min(m_limits.maxAddedVertices - std::min(added, m_limits.maxAddedVertices),
                           K_MAX_VERTICES - std::min(m_positions.size(), K_MAX_VERTICES));
            const std::size_t triangleRoom = m_limits.maxTriangles - std::min(alive, m_limits.maxTriangles);
            const bool overBudget = work.vertices > vertexRoom || work.added > triangleRoom;
            if (overBudget) {
                keepWorstMarks(vertexRoom, triangleRoom);
                if (m_marks.empty()) {
                    break;
                }
                work = planRound();
            }

            m_records.clear();
            for (const std::size_t index : work.triangles) {
                split(index, round + 1);
            }
            turnCuts(round + 1);
            added += work.vertices;
            alive += work.added;
            anySplit = true;
            if (overBudget) {
                break;
            }
        }
        if (!anySplit) {
            return std::nullopt;
        }

        ShelterRefinement::Result result;
        result.added = std::move(m_added);
        result.indices.reserve(alive * 3);
        result.origins.reserve(alive);
        for (const Triangle& triangle : m_triangles) {
            if (triangle.alive) {
                for (const std::uint32_t index : triangle.corner) {
                    result.indices.push_back(static_cast<std::uint16_t>(index));
                }
                result.origins.push_back(triangle.origin);
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
     * @brief Whether an edge is long enough to split: each half at least the shortest allowed
     */
    [[nodiscard]] auto splittable(float length) const -> bool
    {
        constexpr float HALVES = 2.0F;
        return length >= HALVES * m_limits.minEdge;
    }

    /**
     * @brief Whether a triangle is too narrow to show a wedge by itself: twice its area over its
     * longest edge is its height across that edge
     */
    [[nodiscard]] auto isSliver(const std::array<std::uint32_t, 3>& corners) const -> bool
    {
        const RE::NiPoint3& a = m_positions[corners[0]];
        const RE::NiPoint3& b = m_positions[corners[1]];
        const RE::NiPoint3& c = m_positions[corners[2]];
        const float longest = std::max({(b - a).Length(), (c - b).Length(), (a - c).Length()});
        return (b - a).Cross(c - a).Length() < m_limits.minHeight * longest;
    }

    /**
     * @brief Tells the triangles that lie on a broad surface (Triangle::broad)
     *
     * A surface is what snow holding triangles make up across the edges they share, for as long
     * as they lie in one plane or nearly so (K_CREASE). How wide it is does not depend on the
     * triangles it happens to be cut into: it is the width of the rectangle that has the surface's
     * area and outline, a plank's width for the top of a plank and the floor's for a floor. The
     * game's walkway floors are one polygon each, cut into a fan of slivers a few units wide that
     * run from one rim to the other; every one of them is as much floor as a triangle of a finer
     * mesh, and a rope or the top of a beam is as narrow as it ever was.
     */
    void findBroadSurfaces()
    {
        constexpr float MIN_AREA = 1.0e-3F; /**< Twice the area of a triangle that is no triangle */

        /**
         * One triangle's use of an edge
         */
        struct Use {
            EdgeKey key;
            std::uint32_t triangle {};
            bool forward {}; /**< Whether the triangle runs along the edge from the key's first point to its second */
            float length {};
        };
        std::vector<Use> uses;
        std::vector<RE::NiPoint3> normal(m_triangles.size());
        std::vector<float> area(m_triangles.size(), 0.0F);
        for (std::uint32_t index = 0; index < m_triangles.size(); ++index) {
            const Triangle& triangle = m_triangles[index];
            if (!triangle.holds) {
                continue;
            }
            const auto& corner = triangle.corner;
            const RE::NiPoint3& apex = m_positions[corner[0]];
            const RE::NiPoint3 cross = (m_positions[corner[1]] - apex).Cross(m_positions[corner[2]] - apex);
            const float twice = cross.Length();
            if (twice < MIN_AREA) {
                continue;
            }
            normal[index] = cross * (1.0F / twice);
            area[index] = 0.5F * twice;
            for (std::size_t edge = 0; edge < 3; ++edge) {
                const std::uint32_t from = corner[edge];
                const std::uint32_t to = corner[(edge + 1) % 3];
                if (m_keys[from] == m_keys[to]) {
                    continue; // a doubled vertex, not an edge
                }
                const auto [first, second] = oriented(from, to);
                uses.push_back({.key = {.first = m_keys[first], .second = m_keys[second]},
                                .triangle = index,
                                .forward = first == from,
                                .length = (m_positions[to] - m_positions[from]).Length()});
            }
        }
        if (uses.empty()) {
            return;
        }
        std::ranges::sort(uses, [](const Use& lhs, const Use& rhs) -> bool { return lhs.key < rhs.key; });
        const auto forEachEdge = [&](const auto& visit) -> void {
            for (std::size_t begin = 0; begin < uses.size();) {
                std::size_t end = begin + 1;
                while (end < uses.size() && uses[end].key == uses[begin].key) {
                    ++end;
                }
                visit(std::span<const Use> {uses}.subspan(begin, end - begin));
                begin = end;
            }
        };

        // Triangles that share an edge without a crease are one surface
        std::vector<std::uint32_t> surface(m_triangles.size());
        std::ranges::iota(surface, 0U);
        const auto surfaceOf = [&](std::uint32_t index) -> std::uint32_t {
            while (surface[index] != index) {
                surface[index] = surface[surface[index]];
                index = surface[index];
            }
            return index;
        };
        forEachEdge([&](std::span<const Use> sharing) -> void {
            for (std::size_t one = 0; one < sharing.size(); ++one) {
                for (std::size_t other = one + 1; other < sharing.size(); ++other) {
                    if (normal[sharing[one].triangle].Dot(normal[sharing[other].triangle]) >= K_CREASE) {
                        surface[surfaceOf(sharing[one].triangle)] = surfaceOf(sharing[other].triangle);
                    }
                }
            }
        });

        // A surface's outline is every edge it does not go on across: one that no other of its
        // triangles runs along the other way (a doubled triangle runs the same way, and closes
        // nothing)
        std::vector<float> surfaceArea(m_triangles.size(), 0.0F);
        std::vector<float> outline(m_triangles.size(), 0.0F);
        for (std::uint32_t index = 0; index < m_triangles.size(); ++index) {
            if (area[index] > 0.0F) {
                surfaceArea[surfaceOf(index)] += area[index];
            }
        }
        forEachEdge([&](std::span<const Use> sharing) -> void {
            for (const Use& use : sharing) {
                const std::uint32_t own = surfaceOf(use.triangle);
                const bool goesOn = std::ranges::any_of(sharing, [&](const Use& other) -> bool {
                    return other.forward != use.forward && surfaceOf(other.triangle) == own;
                });
                if (!goesOn) {
                    outline[own] += use.length;
                }
            }
        });

        for (std::uint32_t index = 0; index < m_triangles.size(); ++index) {
            if (area[index] <= 0.0F) {
                continue;
            }
            // The width of the rectangle with the surface's area and outline; a surface more
            // compact than any rectangle is as wide as the square of its area
            const std::uint32_t own = surfaceOf(index);
            const float half = 0.5F * outline[own];
            const float spread = (half * half) - (4.0F * surfaceArea[own]);
            const float width = spread > 0.0F ? 0.5F * (half - std::sqrt(spread)) : std::sqrt(surfaceArea[own]);
            m_triangles[index].broad = width >= K_BROAD * m_limits.minHeight;
        }
    }

    /**
     * @brief What splitting the marked edges comes to
     */
    struct Plan {
        std::vector<std::size_t> triangles; /**< The triangles that have a marked edge */
        std::size_t vertices {}; /**< Vertices to add: one per marked edge and pair of end vertices */
        std::size_t added {}; /**< Triangles to add: one per marked edge of each of those triangles */
    };

    [[nodiscard]] auto planRound() const -> Plan
    {
        Plan plan;
        std::unordered_set<std::uint64_t> pairs;
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
                    pairs.insert(pairKeyOf(a, b));
                }
            }
            if (marked > 0) {
                plan.triangles.push_back(index);
                plan.added += marked;
            }
        }
        plan.vertices = pairs.size();
        return plan;
    }

    /**
     * @brief Cuts the round's marks down to what the budget covers, the edges furthest off first
     *
     * @param vertexRoom Vertices that may still be added
     * @param triangleRoom Triangles that may still be added
     */
    void keepWorstMarks(std::size_t vertexRoom,
                        std::size_t triangleRoom)
    {
        // What each marked edge costs: a vertex per pair of end vertices that runs along it (two
        // on a seam), a triangle per triangle that shares it
        struct Cost {
            std::size_t vertices {};
            std::size_t triangles {};
        };
        std::unordered_map<EdgeKey, Cost, EdgeKeyHash> costs;
        std::unordered_set<std::uint64_t> pairs;
        for (const Triangle& triangle : m_triangles) {
            if (!triangle.alive || !triangle.holds) {
                continue;
            }
            for (std::size_t edge = 0; edge < 3; ++edge) {
                const std::uint32_t a = triangle.corner[edge];
                const std::uint32_t b = triangle.corner[(edge + 1) % 3];
                const EdgeKey key = edgeKeyOf(a, b);
                if (!m_marks.contains(key)) {
                    continue;
                }
                Cost& cost = costs[key];
                ++cost.triangles;
                if (pairs.insert(pairKeyOf(a, b)).second) {
                    ++cost.vertices;
                }
            }
        }

        // By deviation, and by position where two are as far off, so that the same shape under
        // the same roof always gets the same vertices
        std::vector<std::pair<EdgeKey, Mark>> order(m_marks.begin(), m_marks.end());
        std::ranges::sort(order, [](const auto& lhs, const auto& rhs) -> bool {
            return lhs.second.deviation != rhs.second.deviation ? lhs.second.deviation > rhs.second.deviation
                                                                : lhs.first < rhs.first;
        });
        m_marks.clear();
        std::size_t vertices = 0;
        std::size_t triangles = 0;
        for (const auto& [key, mark] : order) {
            const Cost& cost = costs[key];
            if (vertices + cost.vertices <= vertexRoom && triangles + cost.triangles <= triangleRoom) {
                vertices += cost.vertices;
                triangles += cost.triangles;
                m_marks.emplace(key, mark);
            }
        }
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
     * @brief Turns the cuts across a sliver that have come to run its length
     *
     * A sliver split at both of its long edges is left with a quad that is cut along a diagonal as
     * long as the quad, and when the long edges are split again further out, that cut stays: an
     * edge from the rim of a floor to its middle, never tested, with vertices a unit to either
     * side of it that know better, and a hairline of snow along it. A cut lies between two pieces
     * of one sliver and nothing else, so it can be traded for the other diagonal of the two
     * without a vertex being added or a neighbor noticing. Wherever that diagonal is the shorter
     * by enough, the cut is turned, and the cuts that are left run across the sliver from one
     * long edge to the other.
     *
     * @param round The round the turned pieces count as made in
     */
    void turnCuts(int round)
    {
        constexpr float SHORTER = 0.75F; /**< Of the cut's length: turning for less trades one long cut for another */
        constexpr float MIN_SHARE = 0.05F; /**< Of the two triangles' area, the least either may have after a turn */

        /**
         * One triangle's side of a cut
         */
        struct Side {
            std::uint64_t pair {};
            std::uint32_t triangle {};
            std::uint32_t edge {};
        };
        std::vector<Side> sides;
        for (bool turned = true; turned;) {
            turned = false;
            sides.clear();
            for (std::uint32_t index = 0; index < m_triangles.size(); ++index) {
                const Triangle& triangle = m_triangles[index];
                if (!triangle.alive) {
                    continue;
                }
                for (std::uint32_t edge = 0; edge < 3; ++edge) {
                    if (triangle.cut[edge]) {
                        sides.push_back({.pair = pairKeyOf(triangle.corner[edge], triangle.corner[(edge + 1) % 3]),
                                         .triangle = index,
                                         .edge = edge});
                    }
                }
            }
            std::ranges::sort(sides, {}, &Side::pair);
            for (std::size_t at = 0; at + 1 < sides.size(); ++at) {
                const Side& here = sides[at];
                const Side& there = sides[at + 1];
                const bool crowded = (at > 0 && sides[at - 1].pair == here.pair)
                    || (at + 2 < sides.size() && sides[at + 2].pair == here.pair);
                if (here.pair != there.pair || crowded) {
                    continue; // the cut of exactly two triangles
                }
                const Triangle one = m_triangles[here.triangle]; // copies: the vector grows below
                const Triangle other = m_triangles[there.triangle];
                if (!one.alive || !other.alive) {
                    continue; // turned with another cut in this sweep; the next sweep sees what came of it
                }
                // The one runs along the cut from end to end and has its third corner to one side
                // of it, the other runs back and has its own across the cut from that
                const std::uint32_t from = one.corner[here.edge];
                const std::uint32_t to = one.corner[(here.edge + 1) % 3];
                const std::uint32_t apex = one.corner[(here.edge + 2) % 3];
                const std::uint32_t across = other.corner[(there.edge + 2) % 3];
                if (other.corner[there.edge] != to || other.corner[(there.edge + 1) % 3] != from) {
                    continue;
                }
                const RE::NiPoint3& start = m_positions[from];
                const RE::NiPoint3& end = m_positions[to];
                const RE::NiPoint3& side = m_positions[apex];
                const RE::NiPoint3& opposite = m_positions[across];
                if ((opposite - side).Length() >= SHORTER * (end - start).Length()) {
                    continue;
                }
                // The other diagonal lies inside the two only if they make a convex quad: both of
                // the triangles it would make have to face the way these do, and be triangles - an
                // end of the old cut that lies on the new one, or as good as, would be left
                // standing in the middle of an edge
                const RE::NiPoint3 facing = (end - start).Cross(side - start);
                const float whole = facing.Length();
                if (whole <= 0.0F) {
                    continue;
                }
                const float atStart = (start - side).Cross(opposite - side).Dot(facing) / whole;
                const float atEnd = (end - opposite).Cross(side - opposite).Dot(facing) / whole;
                if (std::min(atStart, atEnd) <= MIN_SHARE * (atStart + atEnd)) {
                    continue;
                }

                // Each new triangle keeps one outer edge of either old one, as it was, and has the
                // new cut for its third
                m_triangles[here.triangle].alive = false;
                m_triangles[there.triangle].alive = false;
                const auto piece
                    = [&](const std::array<std::uint32_t, 3>& corners, bool firstEdge, bool secondEdge) -> void {
                    m_triangles.push_back({.corner = corners,
                                           .cut = {firstEdge, secondEdge, true},
                                           .round = round,
                                           .alive = true,
                                           .holds = holdsSnow(corners),
                                           .broad = one.broad,
                                           .origin = one.origin});
                };
                piece({apex, from, across}, one.cut[(here.edge + 2) % 3], other.cut[(there.edge + 1) % 3]);
                piece({across, to, apex}, other.cut[(there.edge + 2) % 3], one.cut[(here.edge + 1) % 3]);
                turned = true;
            }
        }
    }

    /**
     * @brief The sample of an edge that is farthest from the line between its ends
     *
     * The edge is sampled at its midpoint, and on a long one at the quarters as well: on a plank
     * that runs from the open deep in under a roof the fade band lies to one side of the middle,
     * and one split there saves the two it would take to get there by halving.
     *
     * @param length The edge's length
     * @return Mark Where along the edge that sample is, and how far off
     */
    [[nodiscard]] auto worstSample(std::uint32_t from,
                                   std::uint32_t to,
                                   float length) -> Mark
    {
        const auto [first, second] = oriented(from, to);
        constexpr std::array<float, 3> SAMPLES {0.5F, 0.25F, 0.75F}; // the midpoint first: ties go to it
        // The quarters only where they can tell anything: on a long edge whose ends differ. An
        // edge whose ends agree is either flat (deep under cover, out in the open) or symmetric,
        // and its midpoint says it all - and those edges are most of any mesh
        constexpr float SAME = 1.0e-3F;
        const bool endsDiffer = std::abs(m_openness[first] - m_openness[second]) > SAME;
        const std::size_t count = endsDiffer && length >= 4.0F * m_limits.minEdge ? SAMPLES.size() : 1;
        Mark worst {.where = SAMPLES.front(), .deviation = -1.0F};
        for (std::size_t sample = 0; sample < count; ++sample) {
            const float t = SAMPLES.at(sample);
            const float interpolated = m_openness[first] + ((m_openness[second] - m_openness[first]) * t);
            const float deviation = std::abs(probeAlong(first, second, t) - interpolated);
            if (deviation > worst.deviation) {
                worst = {.where = t, .deviation = deviation};
            }
        }
        return worst;
    }

    /**
     * @brief Sees to it that a sliver of a broad surface splits both of its long edges or neither
     *
     * The two long edges of a sliver run a unit or two apart through the same fade, and are about
     * as far off as each other - so that sooner or later one is just over the tolerance and the
     * other just under it. The sliver between a split edge and a whole one then shows the
     * difference: a hairline of snow as wide as the sliver and as long as the edge, pointing in
     * under the roof from every corner a fan starts at. So where one long edge of a sliver is
     * marked, the other is marked as well, unless it is straight by a wide margin (K_PAIR), and
     * the next sliver's after it: the vertices come in rings across the fan, as hand-placed loops
     * do.
     */
    void pairSlivers()
    {
        // The long edges that are a triangle's own to test, marked or not
        struct Long {
            std::uint32_t from {};
            std::uint32_t to {};
            float length {};
            bool marked {};
        };
        for (bool changed = true; changed;) {
            changed = false;
            for (const Triangle& triangle : m_triangles) {
                if (!triangle.alive || !triangle.holds || !triangle.broad || !isSliver(triangle.corner)) {
                    continue;
                }
                std::array<Long, 3> edges {};
                std::size_t count = 0;
                bool anyMarked = false;
                for (std::size_t edge = 0; edge < 3; ++edge) {
                    const std::uint32_t from = triangle.corner[edge];
                    const std::uint32_t to = triangle.corner[(edge + 1) % 3];
                    const float length = (m_positions[from] - m_positions[to]).Length();
                    if (triangle.cut[edge] || !splittable(length)) {
                        continue;
                    }
                    const bool marked = m_marks.contains(edgeKeyOf(from, to));
                    anyMarked = anyMarked || marked;
                    edges.at(count++) = {.from = from, .to = to, .length = length, .marked = marked};
                }
                if (!anyMarked) {
                    continue;
                }
                for (std::size_t edge = 0; edge < count; ++edge) {
                    const Long& other = edges.at(edge);
                    if (other.marked) {
                        continue;
                    }
                    if (const Mark worst = worstSample(other.from, other.to, other.length);
                        worst.deviation > K_PAIR * m_limits.tolerance) {
                        m_marks.emplace(edgeKeyOf(other.from, other.to), worst);
                        changed = true;
                    }
                }
            }
        }
    }

    /**
     * @brief Holds a vertex's openness against a triangle it is a corner of: whether its spot reads
     * the same along the triangle's tilt as along its own
     *
     * A vertex is read along the tilt its normal gives; a triangle shows what lies along its own.
     * On a floor, a roof, a plank the two are one tilt. On a mesh whose normals are bent away
     * from its faces - smoothed over the rim of a ledge, tipped up on a wall to catch the light -
     * they are not, and the reading is of a slope that is not there: a wall under an overhang,
     * read as a hillside open to the sky. One triangle that reads the spot otherwise, and the
     * vertex is no measurement: it is left to ShelterMap's settling, which weighs it against the
     * triangles around it as it does every vertex of a shape this class is not let at.
     */
    void confirm(std::uint32_t vertex,
                 const Slope& tilt)
    {
        if (m_refuted[vertex]) {
            return;
        }
        const Slope own = m_normals.empty() ? Slope {} : Slope::of(m_normals[vertex]);
        bool same = std::abs(own.dzdx - tilt.dzdx) + std::abs(own.dzdy - tilt.dzdy) <= K_SAME_TILT;
        if (!same) {
            ++m_probes;
            same = std::abs(m_field.opennessAt(m_positions[vertex], tilt, m_fade) - m_openness[vertex])
                <= m_limits.tolerance;
        }
        m_measured[vertex] = same;
        m_refuted[vertex] = !same;
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
        const std::array<float, 3> length {(b - a).Length(), (c - b).Length(), (a - c).Length()};
        const auto longestEdge
            = static_cast<std::size_t>(std::distance(length.begin(), std::ranges::max_element(length)));
        // Too short to split, or too narrow to show anything, unless the surface goes on to
        // either side of it: a sliver of a fan is as wide as the floor the fan is cut from
        const bool sliver = isSliver(corner);
        if (!splittable(length.at(longestEdge)) || (sliver && !triangle.broad)) {
            return;
        }
        // Tested, whatever comes of it: its corners' openness is the measured one from here on,
        // where it is this triangle's to go by. The model's own corners are held against the
        // triangle here; the ones a split adds were, as they were made (split). Three corners out
        // in the open need no holding: the settling has nothing to move on such a triangle either
        const Slope tilt = Slope::ofTriangle(a, b, c);
        if (triangle.round == 0) {
            const bool anyCover
                = std::ranges::any_of(corner, [&](std::uint32_t index) -> bool { return m_openness[index] < 1.0F; });
            for (const std::uint32_t index : corner) {
                if (anyCover) {
                    confirm(index, tilt);
                } else if (!m_refuted[index]) {
                    m_measured[index] = true;
                }
            }
        }

        bool anyMarked = false;
        for (std::size_t edge = 0; edge < 3; ++edge) {
            if (triangle.cut[edge]) {
                continue; // made across a sliver: its two long edges speak for it
            }
            const std::uint32_t from = corner[edge];
            const std::uint32_t to = corner[(edge + 1) % 3];
            const EdgeKey key = edgeKeyOf(from, to);
            if (m_marks.contains(key)) {
                anyMarked = true;
                continue;
            }
            if (!m_seen.insert(key).second || !splittable(length.at(edge))) {
                continue; // already found straight enough this round, or too short to split
            }
            if (const Mark worst = worstSample(from, to, length.at(edge)); worst.deviation > m_limits.tolerance) {
                m_marks.emplace(key, worst);
                anyMarked = true;
            }
        }
        if (anyMarked || sliver) {
            return; // a sliver has no middle that its edges are not next to
        }

        // Edges all straight, the middle maybe not: a roof corner over a large triangle
        constexpr float THIRD = 1.0F / 3.0F;
        const float interpolated = (m_openness[corner[0]] + m_openness[corner[1]] + m_openness[corner[2]]) * THIRD;
        const float deviation = std::abs(probe((a + b + c) * THIRD, tilt) - interpolated);
        if (deviation > m_limits.tolerance) {
            constexpr float HALF = 0.5F;
            m_marks.emplace(edgeKeyOf(corner[longestEdge], corner[(longestEdge + 1) % 3]),
                            Mark {.where = HALF, .deviation = deviation});
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
        m_measured.push_back(true); // until a triangle it is put into says otherwise (confirm)
        m_refuted.push_back(false);
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
        // A triangle that is tested has a say in whether the vertices put on its edges are
        // measurements (confirm); one that only splits along with its neighbors has none
        const bool made = isSliver(parent.corner);
        const bool tested = !made || parent.broad;
        const Slope tilt = Slope::ofTriangle(m_positions[v[0]], m_positions[v[1]], m_positions[v[2]]);
        for (std::size_t edge = 0; edge < 3; ++edge) {
            const auto found = m_marks.find(edgeKeyOf(v[edge], v[(edge + 1) % 3]));
            if (found != m_marks.end()) {
                m[edge] = splitVertex(v[edge], v[(edge + 1) % 3], found->second.where);
                if (tested) {
                    confirm(m[edge], tilt);
                }
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
        const auto rotate = [rotation]<typename T>(const std::array<T, 3>& array) -> std::array<T, 3> {
            return {array[rotation % 3], array[(rotation + 1) % 3], array[(rotation + 2) % 3]};
        };
        v = rotate(v);
        m = rotate(m);

        // A child's edge is a stretch of one of the parent's, and is what that edge was (Triangle::cut),
        // or it is new: made across the parent, which for a sliver is an edge never to be tested.
        // The children lie on the parent's surface: a piece of a broad one is no narrower for being
        // cut small
        const std::array<bool, 3> was = rotate(parent.cut);
        const auto child = [&](const std::array<std::uint32_t, 3>& corners, const std::array<bool, 3>& cut) -> void {
            m_triangles.push_back({.corner = corners,
                                   .cut = cut,
                                   .round = round,
                                   .alive = true,
                                   .holds = holdsSnow(corners),
                                   .broad = parent.broad,
                                   .origin = parent.origin});
        };
        if (marked == 1) {
            child({v[0], m[0], v[2]}, {was[0], made, was[2]});
            child({m[0], v[1], v[2]}, {was[0], was[1], made});
        } else if (marked == 2) {
            child({m[0], v[1], m[1]}, {was[0], was[1], made});
            // The quad that is left is cut along its shorter diagonal
            const float across = (m_positions[v[0]] - m_positions[m[1]]).Length();
            const float other = (m_positions[m[0]] - m_positions[v[2]]).Length();
            if (across <= other) {
                child({v[0], m[0], m[1]}, {was[0], made, made});
                child({v[0], m[1], v[2]}, {made, was[1], was[2]});
            } else {
                child({v[0], m[0], v[2]}, {was[0], made, was[2]});
                child({m[0], m[1], v[2]}, {made, was[1], made});
            }
        } else {
            child({v[0], m[0], m[2]}, {was[0], made, was[2]});
            child({m[0], v[1], m[1]}, {was[0], was[1], made});
            child({m[2], m[1], v[2]}, {made, was[1], was[2]});
            child({m[0], m[1], m[2]}, {made, made, made});
        }
    }

    const Field& m_field;
    std::span<const RE::NiPoint3> m_model;
    std::vector<RE::NiPoint3>& m_positions;
    std::vector<RE::NiPoint3>& m_normals;
    std::vector<float>& m_openness;
    std::vector<bool>& m_measured;
    float m_holdsSnowFrom;
    ShelterMap::Fade m_fade;
    const ShelterRefinement::Limits& m_limits;
    std::size_t m_sourceCount;

    std::vector<PointKey> m_keys; /**< One per position */
    std::vector<bool> m_refuted; /**< One per position: a triangle it was tested with reads its spot otherwise */
    std::vector<Triangle> m_triangles;
    std::vector<ShelterRefinement::Vertex> m_added;
    std::unordered_map<PointKey, float, PointKeyHash> m_probed; /**< Openness by sample point */
    std::unordered_map<EdgeKey, Mark, EdgeKeyHash> m_marks; /**< This round's edges to split, and where along them */
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
                               std::vector<bool>& measured,
                               std::span<const std::uint16_t> indices,
                               float holdsSnowFrom,
                               const ShelterMap::Fade& fade,
                               const Limits& limits) -> std::optional<Result>
{
    measured.assign(positions.size(), false);
    if (indices.size() < 3 || positions.empty() || modelPositions.size() != positions.size()
        || openness.size() != positions.size() || (!normals.empty() && normals.size() != positions.size())
        || limits.maxAddedVertices == 0 || limits.maxRounds <= 0) {
        return std::nullopt;
    }
    Refiner refiner(field, modelPositions, positions, normals, openness, measured, holdsSnowFrom, fade, limits);
    return refiner.run(indices);
}
