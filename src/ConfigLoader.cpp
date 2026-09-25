#include "ConfigLoader.hpp"

#include "Text.hpp"

#include "PCH.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <format>
#include <fstream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

using namespace XPMF;

namespace {

using Json = nlohmann::json;

/**
 * @brief Strips surrounding whitespace
 */
auto trim(std::string_view value) -> std::string_view
{
    constexpr std::string_view BLANKS = " \t\r\n";
    const auto first = value.find_first_not_of(BLANKS);
    if (first == std::string_view::npos) {
        return {};
    }
    return value.substr(first, value.find_last_not_of(BLANKS) - first + 1);
}

/**
 * @brief Converts the file's UTF-8 to the game's ANSI code page, the form its paths and EditorIDs are in
 */
auto toGameCodePage(std::string_view utf8) -> std::string
{
    constexpr unsigned char FIRST_NON_ASCII = 0x80;
    if (std::ranges::all_of(utf8, [](char ch) -> bool { return static_cast<unsigned char>(ch) < FIRST_NON_ASCII; })) {
        return std::string(utf8); // which is every path and EditorID anyone has ever seen
    }

    // Numbers rather than REX::W32's CP_* constants: Windows.h reaches this file through the
    // precompiled header, and its macros of the same names would break them
    constexpr std::uint32_t UTF8_CODE_PAGE = 65001; /**< CP_UTF8 */
    constexpr std::uint32_t ANSI_CODE_PAGE = 0; /**< CP_ACP: the system's, which the game's strings are in */
    const int utf8Length = static_cast<int>(utf8.size());
    const int wideLength = REX::W32::MultiByteToWideChar(UTF8_CODE_PAGE, 0, utf8.data(), utf8Length, nullptr, 0);
    if (wideLength <= 0) {
        return std::string(utf8);
    }
    std::wstring wide(static_cast<std::size_t>(wideLength), L'\0');
    REX::W32::MultiByteToWideChar(UTF8_CODE_PAGE, 0, utf8.data(), utf8Length, wide.data(), wideLength);

    const int size
        = REX::W32::WideCharToMultiByte(ANSI_CODE_PAGE, 0, wide.data(), wideLength, nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        return std::string(utf8);
    }
    std::string narrow(static_cast<std::size_t>(size), '\0');
    REX::W32::WideCharToMultiByte(ANSI_CODE_PAGE, 0, wide.data(), wideLength, narrow.data(), size, nullptr, nullptr);
    return narrow;
}

/**
 * @brief Joins a list back together for the log
 */
auto joinList(const std::vector<std::string>& list,
              std::string_view separator = ", ") -> std::string
{
    std::string joined;
    for (const auto& entry : list) {
        if (!joined.empty()) {
            joined += separator;
        }
        joined += entry;
    }
    return joined.empty() ? "(none)" : joined;
}

/**
 * @brief Parses one JSON file
 *
 * @return std::optional<Json> The file's root object; std::nullopt (after saying why) for
 *         anything else
 */
auto parseFile(const std::filesystem::path& path) -> std::optional<Json>
{
    const std::string name = path.filename().string();
    std::ifstream file {path};
    if (!file) {
        spdlog::error("{} was rejected: it could not be opened", name);
        return std::nullopt;
    }
    try {
        Json root = Json::parse(file);
        if (root.is_object()) {
            return root;
        }
        spdlog::error("{} was rejected: it has to hold an object", name);
    } catch (const std::exception& exception) {
        spdlog::error("{} was rejected: it is not valid JSON - {}", name, exception.what());
    }
    return std::nullopt;
}

/**
 * @brief Reads the fields of one JSON object and keeps a list of everything that is wrong with them
 *
 * Validation is strict about what is there: a field that is present has to have its type, and
 * a file with any problem at all is rejected as a whole - half a profile is not something anyone
 * asked for. A field that is left out falls back to the default the reader is handed, except
 * with the readers that say the field is required. The readers never fail; they note the
 * problem and return something harmless, and the caller looks at problems() once it has asked
 * for every field, which lets the log name all of a file's mistakes at once rather than one per
 * game start.
 */
class Fields {
public:
    explicit Fields(const Json& object)
        : m_object(object)
    {
    }

    /**
     * @brief true or false; fallback when the key is left out or null
     */
    [[nodiscard]] auto boolean(const char* key,
                               bool fallback) -> bool
    {
        const auto* const value = find(key, false);
        if (value == nullptr || value->is_null()) {
            return fallback;
        }
        if (!value->is_boolean()) {
            return wrong<bool>(key, "true, false, or left out");
        }
        return value->get<bool>();
    }

    /**
     * @brief true, false, or "no opinion" for null or a key left out
     */
    [[nodiscard]] auto optionalBoolean(const char* key) -> std::optional<bool>
    {
        const auto* const value = find(key, false);
        if (value == nullptr || value->is_null()) {
            return std::nullopt;
        }
        if (!value->is_boolean()) {
            return wrong<std::optional<bool>>(key, "true, false, null, or left out");
        }
        return value->get<bool>();
    }

    /**
     * @brief A number in range; fallback when the key is left out or null
     */
    [[nodiscard]] auto number(const char* key,
                              float lowest,
                              float highest,
                              float fallback) -> float
    {
        const auto* const value = find(key, false);
        if (value == nullptr || value->is_null()) {
            return fallback;
        }
        // Anything that is not a plain finite number in range, NaN included, fails the comparison
        const double parsed = value->is_number() ? value->get<double>() : std::nan("");
        if (!(parsed >= lowest && parsed <= highest)) {
            return wrong<float>(key, std::format("a number from {} to {}, or left out", lowest, highest));
        }
        return static_cast<float>(parsed);
    }

    /**
     * @brief A number, or "no opinion" for null or a key left out
     *
     * @param above What the number has to lie above, if anything
     * @return std::optional<float> Finite, and one a float can hold
     */
    [[nodiscard]] auto optionalNumber(const char* key,
                                      std::optional<double> above = std::nullopt) -> std::optional<float>
    {
        const auto* const value = find(key, false);
        if (value == nullptr || value->is_null()) {
            return std::nullopt;
        }
        // Anything that is not a plain number a float can hold, NaN and infinities included, fails
        // the comparisons
        constexpr double LARGEST = std::numeric_limits<float>::max();
        const double parsed = value->is_number() ? value->get<double>() : std::nan("");
        if (!(parsed >= -LARGEST && parsed <= LARGEST) || (above.has_value() && !(parsed > *above))) {
            return wrong<std::optional<float>>(key,
                                               above.has_value()
                                                   ? std::format("a number above {}, null, or left out", *above)
                                                   : "a number, null, or left out");
        }
        return static_cast<float>(parsed);
    }

    /**
     * @brief A number in a range, or "no opinion" for null or a key left out
     */
    [[nodiscard]] auto optionalNumberBetween(const char* key,
                                             double lowest,
                                             double highest) -> std::optional<float>
    {
        const auto* const value = find(key, false);
        if (value == nullptr || value->is_null()) {
            return std::nullopt;
        }
        const double parsed = value->is_number() ? value->get<double>() : std::nan("");
        if (!(parsed >= lowest && parsed <= highest)) {
            return wrong<std::optional<float>>(
                key, std::format("a number from {} to {}, null, or left out", lowest, highest));
        }
        return static_cast<float>(parsed);
    }

    /**
     * @brief A string that says something: blank is a problem (a name)
     */
    [[nodiscard]] auto string(const char* key) -> std::string
    {
        const auto* const value = find(key);
        if (value == nullptr) {
            return {};
        }
        if (!value->is_string()) {
            return wrong<std::string>(key, "a string");
        }
        std::string text(trim(value->get_ref<const std::string&>()));
        if (text.empty()) {
            return wrong<std::string>(key, "a string that says something");
        }
        return text;
    }

    /**
     * @brief A string that may be left out altogether, or be null or blank (a texture that is not replaced)
     */
    [[nodiscard]] auto optionalString(const char* key) -> std::string
    {
        const auto* const value = find(key, false);
        if (value == nullptr || value->is_null()) {
            return {};
        }
        if (!value->is_string()) {
            return wrong<std::string>(key, "a string, or left out");
        }
        return std::string(trim(value->get_ref<const std::string&>()));
    }

    /**
     * @brief A list of strings, which has to be there unless it may be left out (then: none)
     *
     * @return std::vector<std::string> Trimmed; entries that are blank are dropped
     */
    [[nodiscard]] auto strings(const char* key,
                               bool required) -> std::vector<std::string>
    {
        std::vector<std::string> entries;
        const auto* const value = find(key, required);
        if (value == nullptr || (!required && value->is_null())) {
            return entries;
        }
        if (!value->is_array()
            || !std::ranges::all_of(*value, [](const Json& item) -> bool { return item.is_string(); })) {
            return wrong<std::vector<std::string>>(key,
                                                   required ? "a list of strings" : "a list of strings, or left out");
        }
        for (const auto& item : *value) {
            if (const auto entry = trim(item.get_ref<const std::string&>()); !entry.empty()) {
                entries.emplace_back(entry);
            }
        }
        return entries;
    }

    /**
     * @brief Everything wrong with the fields asked for so far; empty when the object is fine
     */
    [[nodiscard]] auto problems() const -> const std::vector<std::string>& { return m_problems; }

    /**
     * @brief A key that is no setting but has every right to be there - "$schema" for an editor,
     * "comment" for a person - so that nobody warns about it either
     */
    void allowed(const char* key) { static_cast<void>(find(key, false)); } // looked up, which is all it takes

    /**
     * @brief Keys nothing asked for. Not a reason to reject a file, but worth a warning: in a hand
     * edited file it is usually a typo
     */
    [[nodiscard]] auto unknownKeys() const -> std::vector<std::string>
    {
        std::vector<std::string> unknown;
        for (const auto& [key, value] : m_object.items()) {
            if (std::ranges::find(m_asked, key) == m_asked.end()) {
                unknown.push_back(key);
            }
        }
        return unknown;
    }

private:
    /**
     * @brief The value under a key; nullptr when there is none, which is a problem unless the
     * field may be left out
     */
    [[nodiscard]] auto find(const char* key,
                            bool required = true) -> const Json*
    {
        m_asked.emplace_back(key);
        const auto found = m_object.find(key);
        if (found == m_object.end()) {
            if (required) {
                m_problems.push_back(std::format("\"{}\" is missing", key));
            }
            return nullptr;
        }
        return &*found;
    }

    template <typename T>
    [[nodiscard]] auto wrong(const char* key,
                             std::string_view expected) -> T
    {
        m_problems.push_back(std::format("\"{}\" has to be {}", key, expected));
        return T {};
    }

    const Json& m_object;
    std::vector<std::string> m_asked;
    std::vector<std::string> m_problems;
};

/**
 * @brief Says what became of a file's fields: rejected with every reason, or accepted (with a
 * warning about keys that are not settings)
 *
 * @return bool Whether the file is good
 */
auto accept(const Fields& fields,
            const std::string& fileName) -> bool
{
    if (!fields.problems().empty()) {
        spdlog::error("{} was rejected: {}", fileName, joinList(fields.problems(), "; "));
        return false;
    }
    for (const auto& key : fields.unknownKeys()) {
        spdlog::warn("{}: \"{}\" is not a setting and was ignored", fileName, key);
    }
    return true;
}

} // namespace

void ConfigLoader::loadConfig()
{
    // The folder sits next to the plugin DLL; current_path is the game root at load time
    const auto profilesPath = std::filesystem::current_path() / "Data" / "SKSE" / "Plugins" / "XPMF";

    s_profiles.clear();
    std::error_code error;

    // The profiles: one per file
    if (!std::filesystem::is_directory(profilesPath, error)) {
        spdlog::info("No XPMF folder next to the plugin; using the built-in profiles");
        s_profiles = builtInProfiles();
    } else {
        std::vector<std::filesystem::path> files;
        for (std::filesystem::directory_iterator entry {profilesPath, error}, last; !error && entry != last;
             entry.increment(error)) {
            if (entry->is_regular_file(error) && Text::toLower(entry->path().extension().string()) == ".json") {
                files.push_back(entry->path());
            }
        }
        // Whatever order the file system lists them in, the same files always mean the same thing:
        // a.json before z.json, which is also who wins a tie between two profiles
        std::ranges::sort(files, {}, [](const std::filesystem::path& path) -> std::string {
            return Text::toLower(path.filename().string());
        });
        for (const auto& path : files) {
            const auto root = parseFile(path);
            if (!root.has_value()) {
                continue;
            }

            Fields fields {*root};
            Profile profile;
            profile.file = Text::toLower(path.filename().string());
            profile.name = fields.string("name");
            // Only the name and the patterns have to be there; everything else has a default
            for (const auto& pattern : fields.strings("editorIds", true)) {
                profile.editorIds.push_back(Text::toLower(toGameCodePage(pattern)));
            }
            for (const auto& pattern : fields.strings("excludeEditorIds", false)) {
                profile.excludeEditorIds.push_back(Text::toLower(toGameCodePage(pattern)));
            }
            profile.pbr = fields.boolean("pbr", DEFAULT_PBR);
            profile.diffuseTexture = normalizeTexturePath(fields.optionalString("diffuseTexture"));
            profile.normalTexture = normalizeTexturePath(fields.optionalString("normalTexture"));
            profile.noiseTexture = normalizeTexturePath(fields.optionalString("noiseTexture"));
            profile.detailNormalTexture = normalizeTexturePath(fields.optionalString("detailNormalTexture"));
            profile.isSnow = fields.optionalBoolean("isSnow");
            // The record's own falloff values unless the profile gives one; the engine divides by
            // the noise UV scale
            profile.falloffScale = fields.optionalNumber("falloffScale");
            profile.falloffBias = fields.optionalNumber("falloffBias");
            profile.noiseUVScale = fields.optionalNumber("noiseUVScale", 0.0);
            // The static's angle, the fourth value the single pass path reads: written into the
            // statics themselves (MaterialMatcher)
            profile.maxAngle = fields.optionalNumberBetween("maxAngle", 0.0, MAX_ANGLE_LIMIT);
            // The three geometry settings, each with the statics it is not applied to
            const auto patterns = [&](const char* key) -> std::vector<std::string> {
                std::vector<std::string> lowered;
                for (const auto& pattern : fields.strings(key, false)) {
                    lowered.push_back(Text::toLower(toGameCodePage(pattern)));
                }
                return lowered;
            };
            profile.neutralizeVertexColors = fields.boolean("neutralizeVertexColors", DEFAULT_NEUTRALIZE_VERTEX_COLORS);
            profile.neutralizeVertexColorsSkip = patterns("neutralizeVertexColorsSkip");
            profile.neutralizeVertexAlpha = fields.boolean("neutralizeVertexAlpha", DEFAULT_NEUTRALIZE_VERTEX_ALPHA);
            profile.neutralizeVertexAlphaSkip = patterns("neutralizeVertexAlphaSkip");
            profile.roofShelter = fields.boolean("roofShelter", DEFAULT_ROOF_SHELTER);
            profile.roofShelterSkip = patterns("roofShelterSkip");
            profile.shelterFade = fields.number("shelterFade", 0.0F, MAX_SHELTER_FADE, DEFAULT_SHELTER_FADE);
            // The mesh's specular on the shapes the projection is on (ProjectedGeometry)
            profile.specularMult = fields.optionalNumberBetween("specularMult", 0.0, MAX_SPECULAR_MULT);
            // An editor's pointer to schema/profile.schema.json, and a note
            fields.allowed("$schema");
            fields.allowed("comment");

            if (!accept(fields, path.filename().string())) {
                continue;
            }
            if (profile.editorIds.empty()) {
                spdlog::warn("{}: \"editorIds\" is empty, so the profile matches no material object",
                             path.filename().string());
            }
            // A skip list without its setting names statics nothing would be done to anyway
            const auto idle = [&](const char* skipKey, const char* settingKey, bool setting, const auto& skip) -> void {
                if (!setting && !skip.empty()) {
                    spdlog::warn("{}: \"{}\" names statics, but \"{}\" is off, so it does nothing",
                                 path.filename().string(),
                                 skipKey,
                                 settingKey);
                }
            };
            idle("neutralizeVertexColorsSkip",
                 "neutralizeVertexColors",
                 profile.neutralizeVertexColors,
                 profile.neutralizeVertexColorsSkip);
            idle("neutralizeVertexAlphaSkip",
                 "neutralizeVertexAlpha",
                 profile.neutralizeVertexAlpha,
                 profile.neutralizeVertexAlphaSkip);
            idle("roofShelterSkip", "roofShelter", profile.roofShelter, profile.roofShelterSkip);

            s_profiles.push_back(std::move(profile));
        }
        if (s_profiles.empty()) {
            spdlog::warn("The XPMF folder holds no usable *.json profile, so there is nothing to do");
        }
    }

    // Naming one of the engine's own projected textures asks for nothing to be substituted, which
    // is what leaving the texture out says too - and saves loading a second copy to swap in for
    // the first
    for (auto& profile : s_profiles) {
        const std::array<std::pair<std::string*, const char*>, 4> textures {
            {{&profile.diffuseTexture, GAME_DIFFUSE},
             {&profile.normalTexture, GAME_NORMAL},
             {&profile.noiseTexture, GAME_NOISE},
             {&profile.detailNormalTexture, GAME_DETAIL_NORMAL}}};
        for (const auto& [path, game] : textures) {
            if (*path == game) {
                path->clear();
            }
        }
    }

    // Log the effective values so user reports include them
    const auto orGame = [](const std::string& path) -> std::string_view {
        return path.empty() ? std::string_view {"(not replaced)"} : std::string_view {path};
    };
    const auto orRecord = [](const std::optional<float>& value) -> std::string {
        return value.has_value() ? std::format("{}", *value) : "(as the record has it)";
    };
    spdlog::info("Config Loaded: {} profiles", s_profiles.size());
    for (const auto& profile : s_profiles) {
        spdlog::info("Config Loaded: [{}] File: {}", profile.name, profile.file.empty() ? "(built in)" : profile.file);
        spdlog::info("Config Loaded: [{}] EditorIDs: {}", profile.name, joinList(profile.editorIds));
        spdlog::info("Config Loaded: [{}] Exclude EditorIDs: {}", profile.name, joinList(profile.excludeEditorIds));
        spdlog::info("Config Loaded: [{}] PBR: {}", profile.name, profile.pbr);
        spdlog::info("Config Loaded: [{}] Diffuse Texture: {}", profile.name, orGame(profile.diffuseTexture));
        spdlog::info("Config Loaded: [{}] Normal Texture: {}", profile.name, orGame(profile.normalTexture));
        spdlog::info("Config Loaded: [{}] Noise Texture: {}", profile.name, orGame(profile.noiseTexture));
        spdlog::info(
            "Config Loaded: [{}] Detail Normal Texture: {}", profile.name, orGame(profile.detailNormalTexture));
        spdlog::info("Config Loaded: [{}] Is Snow: {}",
                     profile.name,
                     !profile.isSnow.has_value() ? "(as the record has it)"
                         : *profile.isSnow       ? "true"
                                                 : "false");
        spdlog::info("Config Loaded: [{}] Falloff Scale: {}", profile.name, orRecord(profile.falloffScale));
        spdlog::info("Config Loaded: [{}] Falloff Bias: {}", profile.name, orRecord(profile.falloffBias));
        spdlog::info("Config Loaded: [{}] Noise UV Scale: {}", profile.name, orRecord(profile.noiseUVScale));
        spdlog::info("Config Loaded: [{}] Max Angle: {}", profile.name, orRecord(profile.maxAngle));
        spdlog::info("Config Loaded: [{}] Neutralize Vertex Colors: {}", profile.name, profile.neutralizeVertexColors);
        spdlog::info("Config Loaded: [{}] Neutralize Vertex Colors Skip: {}",
                     profile.name,
                     joinList(profile.neutralizeVertexColorsSkip));
        spdlog::info("Config Loaded: [{}] Neutralize Vertex Alpha: {}", profile.name, profile.neutralizeVertexAlpha);
        spdlog::info("Config Loaded: [{}] Neutralize Vertex Alpha Skip: {}",
                     profile.name,
                     joinList(profile.neutralizeVertexAlphaSkip));
        spdlog::info("Config Loaded: [{}] Roof Shelter: {}", profile.name, profile.roofShelter);
        spdlog::info("Config Loaded: [{}] Roof Shelter Skip: {}", profile.name, joinList(profile.roofShelterSkip));
        spdlog::info("Config Loaded: [{}] Shelter Fade: {}", profile.name, profile.shelterFade);
        spdlog::info("Config Loaded: [{}] Specular Mult: {}",
                     profile.name,
                     profile.specularMult.has_value() ? std::format("{}", *profile.specularMult)
                                                      : "(as the mesh has it)");
    }
}

auto ConfigLoader::getProfiles() -> const std::vector<Profile>& { return s_profiles; }

auto ConfigLoader::isAnyMaterialPatched() -> bool
{
    return std::ranges::any_of(s_profiles, [](const Profile& profile) -> bool { return profile.patchesMaterial(); });
}

auto ConfigLoader::isAnyGeometryChanged() -> bool
{
    return std::ranges::any_of(s_profiles, [](const Profile& profile) -> bool {
        return profile.neutralizeVertexColors || profile.neutralizeVertexAlpha || profile.roofShelter;
    });
}

auto ConfigLoader::isAnySpecularChanged() -> bool
{
    return std::ranges::any_of(s_profiles,
                               [](const Profile& profile) -> bool { return profile.specularMult.has_value(); });
}

auto ConfigLoader::isAnyRoofSheltered() -> bool { return std::ranges::any_of(s_profiles, &Profile::roofShelter); }

auto ConfigLoader::builtInProfiles() -> std::vector<Profile>
{
    // What the shipped snow.json and ash.json say; the PBR ones need PBR textures, so they are
    // not built in
    Profile snow;
    snow.name = "snow";
    snow.editorIds = {DEFAULT_SNOW_PATTERN};
    snow.pbr = DEFAULT_PBR;
    snow.diffuseTexture = DEFAULT_SNOW_DIFFUSE;
    snow.normalTexture = DEFAULT_SNOW_NORMAL;
    snow.isSnow = true;
    snow.neutralizeVertexColors = DEFAULT_NEUTRALIZE_VERTEX_COLORS;
    snow.neutralizeVertexAlpha = DEFAULT_NEUTRALIZE_VERTEX_ALPHA;
    snow.roofShelter = DEFAULT_ROOF_SHELTER;
    snow.shelterFade = DEFAULT_SHELTER_FADE;

    // Ash falls like snow and lies like snow, so it gets everything snow gets - except the snow
    // shading, which is sparkle and rim light
    Profile ash = snow;
    ash.name = "ash";
    ash.editorIds = {DEFAULT_ASH_PATTERN_MATERIAL, DEFAULT_ASH_PATTERN_DLC, DEFAULT_ASH_PATTERN_LOD};
    ash.diffuseTexture = DEFAULT_ASH_DIFFUSE;
    ash.normalTexture = DEFAULT_ASH_NORMAL;
    ash.isSnow = false;

    return {std::move(ash), std::move(snow)}; // the order of their file names
}

auto ConfigLoader::normalizeTexturePath(std::string_view raw) -> std::string
{
    std::string path = Text::toLower(toGameCodePage(trim(raw)));
    if (path.empty()) {
        return path;
    }
    std::ranges::replace(path, '/', '\\');

    // Resource paths are relative to Data
    constexpr std::string_view DATA_PREFIX = "data\\";
    if (path.starts_with(DATA_PREFIX)) {
        path.erase(0, DATA_PREFIX.size());
    }
    while (path.starts_with('\\')) {
        path.erase(0, 1);
    }

    constexpr std::string_view TEXTURES_PREFIX = "textures\\";
    if (!path.starts_with(TEXTURES_PREFIX)) {
        path.insert(0, TEXTURES_PREFIX);
    }
    constexpr std::string_view DDS_EXTENSION = ".dds";
    if (!path.ends_with(DDS_EXTENSION)) {
        path += DDS_EXTENSION;
    }
    return path;
}
