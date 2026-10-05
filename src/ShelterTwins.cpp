#include "ShelterTwins.hpp"

#include "PCH.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numeric>
#include <optional>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace XPMF;

namespace {

constexpr float K_KEY_SCALE = 8.0F; /**< Corners are told apart to an eighth of a unit, as ShelterRefinement tells
                                       vertices apart: two copies of one face agree to far better than that */
constexpr std::size_t K_MAX_VERTICES = std::numeric_limits<std::uint16_t>::max(); /**< What a BSTriShape counts to */

using PointKey = std::array<std::int64_t, 3>;

/**
 * @brief A triangle by where its corners are, whichever shape draws it and in whatever order
 */
struct TriangleKey {
    std::array<PointKey, 3> corners {}; /**< Sorted */
    auto operator==(const TriangleKey&) const -> bool = default;
};

struct TriangleKeyHash {
    auto operator()(const TriangleKey& key) const noexcept -> std::size_t
    {
        constexpr std::uint64_t OFFSET_BASIS = 0xCBF29CE484222325ULL;
        constexpr std::uint64_t PRIME = 0x100000001B3ULL;
        std::uint64_t hash = OFFSET_BASIS;
        for (const auto& corner : key.corners) {
            for (const std::int64_t value : corner) {
                hash = (hash ^ static_cast<std::uint64_t>(value)) * PRIME;
            }
        }
        return static_cast<std::size_t>(hash);
    }
};

struct PointKeyHash {
    auto operator()(const PointKey& key) const noexcept -> std::size_t
    {
        constexpr std::uint64_t GOLDEN = 0x9E3779B97F4A7C15ULL;
        std::uint64_t hash = static_cast<std::uint64_t>(key[0]) * GOLDEN;
        hash ^= (static_cast<std::uint64_t>(key[1]) + GOLDEN + (hash << 6U) + (hash >> 2U));
        hash ^= (static_cast<std::uint64_t>(key[2]) + GOLDEN + (hash << 6U) + (hash >> 2U));
        return static_cast<std::size_t>(hash);
    }
};

auto keyOf(const RE::NiPoint3& point) -> PointKey
{
    return {
        std::llround(point.x * K_KEY_SCALE),
        std::llround(point.y * K_KEY_SCALE),
        std::llround(point.z * K_KEY_SCALE),
    };
}

/**
 * @brief The key of a triangle, or std::nullopt for one that is no triangle (a corner doubled or out of range)
 */
auto triangleKeyOf(std::span<const PointKey> keys,
                   std::span<const std::uint16_t> indices,
                   std::size_t triangle) -> std::optional<TriangleKey>
{
    TriangleKey key;
    for (std::size_t slot = 0; slot < 3; ++slot) {
        const std::uint16_t vertex = indices[(triangle * 3) + slot];
        if (vertex >= keys.size()) {
            return std::nullopt;
        }
        key.corners.at(slot) = keys[vertex];
    }
    std::ranges::sort(key.corners);
    if (key.corners[0] == key.corners[1] || key.corners[1] == key.corners[2]) {
        return std::nullopt;
    }
    return key;
}

auto pointKeys(std::span<const RE::NiPoint3> positions) -> std::vector<PointKey>
{
    std::vector<PointKey> keys;
    keys.reserve(positions.size());
    for (const auto& position : positions) {
        keys.push_back(keyOf(position));
    }
    return keys;
}

} // namespace

auto ShelterTwins::groups(std::span<const Shape> shapes) -> std::vector<std::vector<std::size_t>>
{
    std::vector<std::vector<std::size_t>> result;
    if (shapes.size() < 2) {
        return result;
    }

    // Triangles that share an edge of nothing but their corners join their shapes: union-find
    std::vector<std::size_t> parent(shapes.size());
    std::ranges::iota(parent, std::size_t {0});
    const auto rootOf = [&](std::size_t index) -> std::size_t {
        while (parent[index] != index) {
            parent[index] = parent[parent[index]];
            index = parent[index];
        }
        return index;
    };
    std::size_t total = 0;
    for (const Shape& shape : shapes) {
        total += shape.indices.size() / 3;
    }
    std::unordered_map<TriangleKey, std::size_t, TriangleKeyHash> owners;
    owners.reserve(total);
    bool anyShared = false;
    for (std::size_t index = 0; index < shapes.size(); ++index) {
        const Shape& shape = shapes[index];
        const std::vector<PointKey> keys = pointKeys(shape.positions);
        for (std::size_t triangle = 0; triangle < shape.indices.size() / 3; ++triangle) {
            const auto key = triangleKeyOf(keys, shape.indices, triangle);
            if (!key.has_value()) {
                continue;
            }
            const auto [found, inserted] = owners.try_emplace(*key, index);
            if (inserted || found->second == index) {
                continue; // first seen, or a doubled face of this same shape
            }
            parent[rootOf(index)] = rootOf(found->second);
            anyShared = true;
        }
    }
    if (!anyShared) {
        return result;
    }

    std::vector<std::size_t> members(shapes.size(), 0);
    for (std::size_t index = 0; index < shapes.size(); ++index) {
        ++members[rootOf(index)];
    }
    std::unordered_map<std::size_t, std::size_t> groupOf; // root -> index into result
    for (std::size_t index = 0; index < shapes.size(); ++index) {
        const std::size_t root = rootOf(index);
        if (members[root] < 2) {
            continue; // shares nothing
        }
        const auto found = groupOf.find(root);
        if (found == groupOf.end()) {
            groupOf.emplace(root, result.size());
            result.push_back({index});
        } else {
            result[found->second].push_back(index);
        }
    }
    return result;
}

auto ShelterTwins::replay(const ShelterRefinement::Result& refinement,
                          std::span<const RE::NiPoint3> leaderPositions,
                          std::span<const std::uint16_t> leaderIndices,
                          std::span<const RE::NiPoint3> twinPositions,
                          std::span<const std::uint16_t> twinIndices) -> Replayed
{
    Replayed out;
    const std::size_t leaderCount = leaderPositions.size();
    const std::size_t twinCount = twinPositions.size();
    const std::size_t leaderTriangles = leaderIndices.size() / 3;
    const std::size_t twinTriangles = twinIndices.size() / 3;
    const std::size_t refinedTriangles = refinement.indices.size() / 3;
    if (leaderCount == 0 || twinCount == 0 || leaderTriangles == 0 || twinTriangles == 0
        || refinement.origins.size() != refinedTriangles || refinement.indices.size() % 3 != 0) {
        return out;
    }

    // Pieces per model triangle of the refined shape; one left whole is its own only piece
    std::vector<std::vector<std::uint32_t>> piecesOf(leaderTriangles);
    for (std::size_t piece = 0; piece < refinedTriangles; ++piece) {
        const std::uint32_t origin = refinement.origins[piece];
        if (origin >= leaderTriangles) {
            return out;
        }
        piecesOf[origin].push_back(static_cast<std::uint32_t>(piece));
    }

    // The twin's triangles by their corners
    const std::vector<PointKey> twinKeys = pointKeys(twinPositions);
    std::unordered_map<TriangleKey, std::vector<std::uint32_t>, TriangleKeyHash> twinByKey;
    for (std::size_t triangle = 0; triangle < twinTriangles; ++triangle) {
        if (const auto key = triangleKeyOf(twinKeys, twinIndices, triangle); key.has_value()) {
            twinByKey[*key].push_back(static_cast<std::uint32_t>(triangle));
        }
    }

    // Every split triangle of the refined shape claims the twin triangles with its corners, each
    // corner matched to the twin's at the same spot
    struct Claim {
        std::uint32_t leader {K_NONE}; /**< The split model triangle */
        std::array<std::uint32_t, 3> twinCorner {}; /**< The twin's vertex at each of its corners, by its slot */
        bool reversed {}; /**< The twin winds the other way round */
    };
    std::vector<Claim> claims(twinTriangles);
    const std::vector<PointKey> leaderKeys = pointKeys(leaderPositions);
    bool anyClaim = false;
    for (std::size_t leader = 0; leader < leaderTriangles; ++leader) {
        if (piecesOf[leader].size() < 2) {
            continue; // left whole
        }
        const auto key = triangleKeyOf(leaderKeys, leaderIndices, leader);
        if (!key.has_value()) {
            continue;
        }
        const auto found = twinByKey.find(*key);
        if (found == twinByKey.end()) {
            continue;
        }
        for (const std::uint32_t twin : found->second) {
            Claim& claim = claims[twin];
            if (claim.leader != K_NONE) {
                continue; // a doubled face of the refined shape: the first copy's pieces serve
            }
            std::array<std::size_t, 3> twinSlot {};
            for (std::size_t slot = 0; slot < 3; ++slot) {
                const PointKey& corner = leaderKeys[leaderIndices[(leader * 3) + slot]];
                for (std::size_t candidate = 0; candidate < 3; ++candidate) {
                    const std::uint16_t vertex = twinIndices[(twin * 3) + candidate];
                    if (twinKeys[vertex] == corner) {
                        twinSlot.at(slot) = candidate;
                        claim.twinCorner.at(slot) = vertex;
                        break;
                    }
                }
            }
            claim.leader = static_cast<std::uint32_t>(leader);
            claim.reversed = twinSlot[1] != (twinSlot[0] + 1) % 3;
            anyClaim = true;
        }
    }
    if (!anyClaim) {
        out.outcome = Outcome::kNothingToCarry;
        return out;
    }

    // The twin's list: its own triangles, each claimed one replaced by the pieces of its claimant
    ShelterRefinement::Result& result = out.result;
    result.indices.reserve(twinIndices.size() + refinement.indices.size());
    result.origins.reserve(twinTriangles + refinedTriangles);
    std::unordered_map<std::uint64_t, std::uint32_t>
        addedByBlend; // (leader's added vertex, its sources on the twin) -> twin vertex
    const auto cornerSlot = [&](std::uint32_t leader, std::uint32_t vertex) -> std::size_t {
        for (std::size_t slot = 0; slot < 3; ++slot) {
            if (leaderIndices[(leader * 3) + slot] == vertex) {
                return slot;
            }
        }
        return 3; // not a corner of the triangle the piece came from
    };
    for (std::uint32_t twin = 0; twin < twinTriangles; ++twin) {
        const Claim& claim = claims[twin];
        if (claim.leader == K_NONE) {
            for (std::size_t slot = 0; slot < 3; ++slot) {
                result.indices.push_back(twinIndices[(twin * 3) + slot]);
            }
            result.origins.push_back(twin);
            continue;
        }
        for (const std::uint32_t piece : piecesOf[claim.leader]) {
            std::array<std::uint32_t, 3> corners {};
            for (std::size_t slot = 0; slot < 3; ++slot) {
                const std::uint32_t vertex = refinement.indices[(piece * 3) + slot];
                if (vertex < leaderCount) {
                    // One of the model's: the twin's at the same spot
                    const std::size_t corner = cornerSlot(claim.leader, vertex);
                    if (corner == 3) {
                        return out; // kFailed: a piece reaching outside the triangle it came from
                    }
                    corners.at(slot) = claim.twinCorner.at(corner);
                    continue;
                }
                // An added one: the same blend, of the twin's corners at the same spots
                const std::uint32_t added = vertex - static_cast<std::uint32_t>(leaderCount);
                if (added >= refinement.added.size()) {
                    return out;
                }
                const ShelterRefinement::Vertex& source = refinement.added[added];
                std::array<std::uint16_t, 3> sources {};
                for (std::size_t item = 0; item < 3; ++item) {
                    const std::size_t corner = cornerSlot(claim.leader, source.source.at(item));
                    if (corner == 3) {
                        return out;
                    }
                    sources.at(item) = static_cast<std::uint16_t>(claim.twinCorner.at(corner));
                }
                const std::uint64_t blend = (static_cast<std::uint64_t>(added) << 48U)
                    | (static_cast<std::uint64_t>(sources[0]) << 32U) | (static_cast<std::uint64_t>(sources[1]) << 16U)
                    | sources[2];
                const auto [found, inserted]
                    = addedByBlend.try_emplace(blend, static_cast<std::uint32_t>(twinCount + result.added.size()));
                if (inserted) {
                    if (twinCount + result.added.size() >= K_MAX_VERTICES) {
                        return out;
                    }
                    result.added.push_back({.source = sources, .weight = source.weight, .position = source.position});
                    out.addedFrom.push_back(added);
                }
                corners.at(slot) = found->second;
            }
            if (claim.reversed) {
                std::swap(corners[1], corners[2]);
            }
            for (const std::uint32_t corner : corners) {
                result.indices.push_back(static_cast<std::uint16_t>(corner));
            }
            result.origins.push_back(twin);
        }
    }

    // The refined shape's vertex at each of the twin's spots, for what was measured there
    std::unordered_map<PointKey, std::uint32_t, PointKeyHash> leaderByKey;
    leaderByKey.reserve(leaderCount);
    for (std::size_t vertex = 0; vertex < leaderCount; ++vertex) {
        leaderByKey.try_emplace(leaderKeys[vertex], static_cast<std::uint32_t>(vertex));
    }
    out.vertexFrom.assign(twinCount, K_NONE);
    for (std::size_t vertex = 0; vertex < twinCount; ++vertex) {
        if (const auto found = leaderByKey.find(twinKeys[vertex]); found != leaderByKey.end()) {
            out.vertexFrom[vertex] = found->second;
        }
    }
    out.outcome = Outcome::kCarried;
    return out;
}
