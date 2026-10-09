#include "MaterialClassifier.hpp"

#include "ConfigLoader.hpp"
#include "EditorIdLookup.hpp"
#include "PbrMaterialObjects.hpp"
#include "Text.hpp"

#include "PCH.h"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace XPMF;

auto MaterialClassifier::classify(const RE::BGSMaterialObject& material) -> Verdict
{
    Verdict verdict {.editorId = EditorIdLookup::find(&material)};
    if (verdict.editorId.empty()) {
        return verdict; // nothing to go by
    }
    verdict.pbr = PbrMaterialObjects::contains(verdict.editorId);
    const std::string lowerId = Text::toLower(verdict.editorId);

    // The most specific of the patterns of one profile that match
    const auto bestMatch = [&](const std::vector<std::string>& patterns) -> const std::string* {
        const std::string* best = nullptr;
        for (const auto& pattern : patterns) {
            if (matches(pattern, lowerId) && (best == nullptr || specificity(pattern) > specificity(*best))) {
                best = &pattern;
            }
        }
        return best;
    };

    // ...and of all profiles the one whose best pattern is most specific; a pbr profile beats
    // a general one at the same pattern (it matches fewer materials), and the first file name in
    // alphabetical order - the order ConfigLoader hands the profiles out in - beats a later one
    std::optional<Specificity> best;
    for (const auto& profile : ConfigLoader::getProfiles()) {
        const std::string* const pattern = bestMatch(profile.editorIds);
        if (pattern == nullptr) {
            continue;
        }
        if (bestMatch(profile.excludeEditorIds) != nullptr) {
            verdict.reason = verdict.profile == nullptr ? Reason::EXCLUDED : verdict.reason;
            continue;
        }
        if (profile.pbr && !verdict.pbr) {
            verdict.reason = verdict.profile == nullptr ? Reason::NOT_PBR : verdict.reason;
            continue;
        }
        const Specificity candidate = specificity(*pattern);
        const bool better
            = !best.has_value() || candidate > *best || (candidate == *best && profile.pbr && !verdict.profile->pbr);
        if (better) {
            best = candidate;
            verdict.profile = &profile;
            verdict.pattern = *pattern;
            verdict.reason = Reason::EDITOR_ID;
        }
    }
    return verdict;
}

auto MaterialClassifier::isSinglePass(const RE::BGSMaterialObject& material) -> bool
{
    return material.directionalData.singlePass != 0;
}

auto MaterialClassifier::describe(Reason reason) -> std::string_view
{
    switch (reason) {
    case Reason::EDITOR_ID:
        return "EditorID pattern";
    case Reason::LOD_MATERIAL:
        return "named as a LOD material";
    case Reason::EXCLUDED:
        return "excluded by EditorID pattern";
    case Reason::NOT_PBR:
        return "matched a PBR profile without a True PBR configuration";
    case Reason::NONE:
        break;
    }
    return "no profile matches";
}

auto MaterialClassifier::matches(std::string_view pattern,
                                 std::string_view text) -> bool
{
    // The classic two pointer walk: on a mismatch go back to the last * and let it take one more
    // character. Linear in practice, no recursion, no allocation.
    std::size_t patternAt = 0;
    std::size_t textAt = 0;
    std::size_t starAt = std::string_view::npos;
    std::size_t starText = 0;
    while (textAt < text.size()) {
        if (patternAt < pattern.size() && (pattern[patternAt] == '?' || pattern[patternAt] == text[textAt])) {
            ++patternAt;
            ++textAt;
        } else if (patternAt < pattern.size() && pattern[patternAt] == '*') {
            starAt = patternAt++;
            starText = textAt;
        } else if (starAt != std::string_view::npos) {
            patternAt = starAt + 1;
            textAt = ++starText;
        } else {
            return false;
        }
    }
    while (patternAt < pattern.size() && pattern[patternAt] == '*') {
        ++patternAt;
    }
    return patternAt == pattern.size();
}

auto MaterialClassifier::specificity(std::string_view pattern) -> Specificity
{
    Specificity result;
    for (const char ch : pattern) {
        (ch == '*' || ch == '?' ? result.wildcards : result.literals) += 1;
    }
    return result;
}
