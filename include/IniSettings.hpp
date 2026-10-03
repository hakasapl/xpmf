#pragma once

#include "PCH.h"

#include <array>

namespace XPMF {

/**
 * @brief Holds two of the game's display settings at the values the projection needs
 *
 * The projected textures are only ever sampled with bEnableProjecteUVDiffuseNormals on (the
 * engine's spelling): with it off the lighting shader paints a covered pixel the material's flat
 * color wherever the coverage is above zero - no fade, no texture - and every profile's diffuse,
 * normal and shelter fade goes unseen. With bEnableImprovedSnow on the game renders snow flagged
 * shapes through its own snow technique and subsurface pass, over what the profiles set, so it
 * is held off, as Community Shaders holds it off every frame.
 *
 * Done the way Community Shaders does it: through the game's own Setting objects
 * (RE::GetINISetting), at kDataLoaded, before MaterialMatcher reads the first of the two. The
 * values then stand for the session: the game reads SkyrimPrefs.ini again only for the
 * RefreshINI console command, and writes the whole collection - these values included - whenever
 * it saves the file (at exit, after a screenshot, from the settings menu), so they land in the
 * user's SkyrimPrefs.ini as well, as they do for Community Shaders' users.
 */
class IniSettings {
public:
    IniSettings() = delete;

    /**
     * @brief Sets each setting that is not at its value, logging the change; one the game does
     * not have is left alone, with a warning
     */
    static void onDataLoaded();

private:
    /**
     * @brief One setting and the value it is held at
     */
    struct Hold {
        const char* name {}; /**< As the engine spells it, section included */
        bool value {};
        const char* reason {}; /**< For the log */
    };

    constexpr static std::array<Hold, 2> K_HOLDS {
        {{.name = "bEnableProjecteUVDiffuseNormals:Display",
          .value = true,
          .reason = "with it off the shader samples no projected texture and paints a covered pixel the material's "
                    "flat color, with no fade"},
         {.name = "bEnableImprovedSnow:Display",
          .value = false,
          .reason = "with it on the game renders snow flagged shapes through its own snow technique and subsurface "
                    "pass, over what the profiles set; Community Shaders turns it off as well"}}};
};

} // namespace XPMF
