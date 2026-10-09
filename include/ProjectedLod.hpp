#pragma once

#include "ConfigLoader.hpp"

#include "PCH.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <deque>
#include <string>
#include <utility>
#include <vector>

namespace XPMF {

/**
 * @brief Projects a profile's LOD material onto the object LOD shapes that carry its identifier
 *
 * Object LOD is built offline into *.bto meshes, one BSSubIndexTriShape per material and LOD
 * block, and the game knows four of them by name. When a LOD block attaches,
 * BGSDistantObjectBlock::Prepare compares every shape's name with objsnow, objsnowHD, objash and
 * objashHD (and the same four with a -LargeRef suffix, which the LOD generator puts on the shapes
 * of large references) and gives a match the six single pass values of SnowLODMaterial,
 * SnowLODMaterialHD, AshLODMaterialMtns1P or AshMaterialSolstheimMtns1P - four material objects
 * the default object record points at - through the helper Clone3D projects a material with:
 * Projected_UV (and Snow) on the shape's lighting property, the falloff scale and bias, one over
 * the noise UV scale, the color, and for the angle not a static's but the INI's
 * fLODSnowThresholdAngle, or fHDLODSnowThresholdAngle for the HD names, which the generator gives
 * the shapes it builds with full textures (the nearest level, where the vertex alpha it painted
 * is the intensity). The names are compiled in, so a fifth material cannot be added by data.
 *
 * This class is that lookup for every other identifier, on the game's own pattern. A profile has
 * an identifier (lodIdentifier, "obj" and its name unless it says otherwise - objMoss for a
 * profile named moss) and names the material objects its statics' LOD is to carry (lodMaterial,
 * lodMaterialHD), and the LOD generator - DynDOLOD, reading the profiles - names the shapes it
 * builds for those statics objMoss, objMossHD, objMoss-LargeRef and objMossHD-LargeRef, exactly
 * as it names the game's. The hook is a vtable slot on BSSubIndexTriShape, the one class object
 * LOD shapes are: PostLinkObject, which runs for every shape of a *.bto as it is read, once its
 * shader property is linked and before the block is prepared. Only the beginning of a shape's
 * name is read: one that begins with a profile's identifier gets the profile's LOD material
 * exactly as Prepare gives the game's own - CommonLib's NiAVObject::SetProjectedUVData is that
 * helper - and one with HD right after the identifier gets the HD material and the HD angle, as
 * Prepare does for objsnowHD; whatever follows, -LargeRef or anything else the generator may
 * append, is not looked at, and of two identifiers a name begins with the longer one counts.
 * Prepare then runs as always and leaves the shape alone, its name being none of the game's; a
 * name beginning with objsnow or objash is left to it, whatever a profile says. The draw hook
 * (ProjectedTextures) swaps the profile's textures in for such a shape as for every projected
 * draw, the material's color carrying the tag.
 *
 * Only the material is this plugin's business. Which statics get LOD, which shapes are HD and
 * what vertex alpha they carry are the generator's; the material's falloff and noise UV scale
 * are the record author's, and want tuning for distance (SnowLODMaterial has a noise UV scale
 * of 1500 where SnowMaterialObject1P has 48).
 */
class ProjectedLod {
public:
    ProjectedLod() = delete;

    /**
     * @brief One identifier object LOD shapes may carry, and the material objects it stands for
     */
    struct Entry {
        std::string identifier; /**< Lower case: objmoss */
        RE::BGSMaterialObject* material {}; /**< Single pass; its values are read, patched, as a shape is loaded */
        RE::BGSMaterialObject* materialHD {}; /**< Same, for the HD name; the plain one again when the profile
                                                 names no other */
        const ConfigLoader::Profile* profile {}; /**< For the log */
    };

    constexpr static std::array<const char*, 2> K_GAME_IDENTIFIERS {"objsnow", "objash"}; /**< The game's own: a
                                                                                               name beginning with
                                                                                               either is Prepare's,
                                                                                               never this hook's */

    /**
     * @brief Installs the PostLinkObject hook; SKSE load callback. Does nothing until onMaterialsReady()
     */
    static void install();

    /**
     * @brief Hands the hook the identifiers to look for; kDataLoaded, once the materials are patched
     */
    static void onMaterialsReady(std::vector<Entry> entries);

private:
    constexpr static const char* K_HD_SUFFIX = "hd"; /**< objsnowHD - right after the identifier: the HD material
                                                        and the HD angle */
    constexpr static const char* K_LOD_ANGLE = "fLODSnowThresholdAngle:Terrain"; /**< What Prepare takes for the max
                                                                                    angle on a plain name... */
    constexpr static const char* K_HD_LOD_ANGLE = "fHDLODSnowThresholdAngle:Terrain"; /**< ...and on an HD one */
    constexpr static float DEFAULT_LOD_ANGLE = 100.0F; /**< The engine's own defaults (1.7.99), for a setting that
                                                          cannot be found */
    constexpr static float DEFAULT_HD_LOD_ANGLE = 87.0F;

    /**
     * @brief Vtable hook on BSSubIndexTriShape::PostLinkObject
     */
    struct PostLinkObjectHook {
        static void thunk(RE::BSSubIndexTriShape* shape,
                          RE::NiStream& stream);
        static inline REL::Relocation<decltype(thunk)> s_func;
        constexpr static std::size_t SLOT = 0x1E;
    };

    /**
     * @brief An identifier the hook looks for, with what it has done for it
     */
    struct Registered {
        explicit Registered(Entry given)
            : entry(std::move(given))
        {
        }
        Entry entry;
        std::atomic<bool> logged {false}; /**< Whether the first shape to carry the identifier was reported */
        std::atomic<bool> loggedHD {false}; /**< Same for the HD name */
    };

    /**
     * @brief A name object LOD shapes may begin with, and whose it is
     */
    struct Candidate {
        std::string identifier;
        Registered* registered {}; /**< nullptr: the game's own, Prepare's */
    };

    /**
     * @brief Gives a freshly read LOD shape its material, if it carries an identifier
     */
    static void onShapeLinked(RE::BSSubIndexTriShape& shape);

    /**
     * @brief The max angle the engine projects object LOD at, in degrees
     */
    [[nodiscard]] static auto angleOf(bool hd) -> float;

    static inline std::deque<Registered> s_registered; /**< Stable addresses: the map points into it */
    static inline std::vector<Candidate> s_candidates; /**< Longest identifier first: of two a name begins with, the
                                                          longer one counts */
    static inline RE::Setting* s_lodAngle = nullptr; /**< The two INI settings, resolved once */
    static inline RE::Setting* s_hdLodAngle = nullptr;
    static inline std::atomic<bool> s_ready {false}; /**< Gates the hook until the names are final */
};

} // namespace XPMF
