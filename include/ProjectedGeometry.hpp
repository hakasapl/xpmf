#pragma once

#include "ConfigLoader.hpp"
#include "ProjectedVertexData.hpp"
#include "ShelterMap.hpp"
#include "VertexLayout.hpp"

#include "PCH.h"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

namespace XPMF {

/**
 * @brief Gives shapes with a projected material on them - snow, ash, whatever the profiles name -
 * the vertex colors this plugin wants them to have
 *
 * Three jobs, all done by swapping a shape's renderer data for a variant (see
 * ProjectedVertexData): keeping vertex colors from tinting the projected material (a profile's
 * neutralizeVertexColors), taking a vertex alpha the mesh's author painted against it out of
 * its way (its neutralizeVertexAlpha), and keeping it out from under roofs through that alpha
 * (its roofShelter). Which of the three a shape gets, and how far in under a roof its
 * projection fades, are the settings of the profile its static's material object belongs to -
 * each setting less the statics its skip list names by EditorID (neutralizeVertexColorsSkip,
 * neutralizeVertexAlphaSkip, roofShelterSkip: a mesh whose colors or mask are wanted as they
 * are, a porch that is to stay snowed under its roof), see settingsOf. A fourth setting, specularMult,
 * scales the specular strength of the shapes the projection is on, since the shader lights a
 * covered pixel with the mesh's own specular unless the material carries the Snow flag - on the
 * game's own materials only: Community Shaders' PBR materials keep their roughness scale in that
 * field, and their projected snow takes roughness and specular from CS's material object
 * configuration instead (scaleSpecular). "Snow" below stands for any of them.
 *
 * The first pass happens inside TESBoundObject::Clone3D, hooked through TESObjectSTAT's vtable
 * (slot 0x40; statics are the only forms the engine applies a material object to). The engine
 * has just set Projected_UV on the clone's shader properties, nothing else can see the clone
 * yet, and it gets the shared (white) variant there and then - so snow has the right color from the
 * first frame, on large references too. What cannot be known at that point is where the
 * clone will end up, which way will be up for it, and what will stand above it.
 *
 * That is the cell pass, laid out like SmoothTerrain's mesh contact detection because it has
 * the same constraints:
 *  - Gathering walks a cell's references on the main thread - the only thread that may read
 *    the scene graph - in slices of K_SLICE_BUDGET, recording pointers only. "A cell's
 *    references" includes the large references standing in it, which are not in the cell's own
 *    set: the engine moves a large reference into the worldspace's sky cell once it is loaded,
 *    and nearly every building is one. Per reference: for every solid
 *    static shape an occluder (the CPU vertex and index copies the engine keeps for decals),
 *    for every snow-projected shape a receiver. Both pin what they point at. A cell is
 *    gathered once references have stopped loading into it for K_QUIET_PERIOD, and once more
 *    after K_SETTLE_RECHECK, because 3D streams in for seconds after a cell attaches. Behind a
 *    loading screen nothing is seen until it goes, so the pass hurries while one is up: shorter
 *    waits, longer slices, the worker at normal priority - the point being that what is under a
 *    roof is bare by the time the screen fades in, not a second after.
 *  - A below-normal-priority worker rasterizes the occluders into height layers (one per cell
 *    they reach, see ShelterMap) and, once the 3x3 cells around a receiver's cell are quiet,
 *    judges each receiver by the vertices that can hold snow (ShelterMap::judge). A cell's first
 *    judgement waits for its own roofs only, its neighbors' coming in over the next seconds and a
 *    second pass following them; every later one waits for the whole neighborhood. In the open ->
 *    keeps the shared variant, partly covered -> a private variant whose alpha fades with the
 *    distance under cover, sheltered -> its projected snow is switched off (the Projected_UV
 *    and Snow shader flags the engine set in Clone3D are cleared again) and it goes back to the
 *    model's own vertex data - and onto a list (s_switchedOff), since the next gather has to take
 *    it for a receiver still, and nothing on the property tells it from a shape the engine never
 *    projected onto. That switch is also all that can be done for a shape whose vertex
 *    alpha is already spoken for - a blended overlay, an alpha tested cutout - where lowering
 *    alpha would make the shape itself fade or vanish: such a shape loses its snow as a whole
 *    once half of what could hold snow on it is under cover.
 *  - Results are applied, and every pinned pointer let go, back on the main thread in batches.
 *
 * Exterior cells only: indoors everything is under a roof, and the snow that is there was put
 * there on purpose.
 *
 * Community Shaders: its True PBR hooks the same static vtable, but slot 0x4A - the one argument
 * Clone3D, which does nothing but call slot 0x40 - so its hook wraps this one: engine, then this
 * class (vertex data), then True PBR (material parameters of PBR shapes). Neither touches what
 * the other does. Statics under a material object True PBR has a configuration for are handled
 * like any other static with a projection on it here; only their material's record is left alone
 * (MaterialMatcher, PbrMaterialObjects).
 */
class ProjectedGeometry {
public:
    ProjectedGeometry() = delete;

    /**
     * @brief What MaterialMatcher found out about one single pass material object of a profile
     *
     * A static carrying such a material has its projection on every lit shape, and those shapes
     * get the profile's vertex colors and shelter - whether the record was patched or left alone.
     */
    struct Treatment {
        const ConfigLoader::Profile* profile {}; /**< Never nullptr; points into ConfigLoader's list */
        float meanNoise {}; /**< Average of the coverage noise the material's draws sample */
        bool untouched {}; /**< Record left as it was (a True PBR configuration, no material setting given,
                              textures missing): its shapes get the vertex colors and shelter like any other,
                              but Seasons of Skyrim's winter snow is not re-colored to it (adoptWinterSnow) */
    };

    /**
     * @brief Every single pass material object of a profile, and what was found out about it
     */
    using Materials = std::unordered_map<const RE::BGSMaterialObject*, Treatment>;

    /**
     * @brief Installs the Clone3D hooks; at kPostPostLoad, after the config was read
     *
     * Not at plugin load like the draw hook: Seasons of Skyrim writes the same vtable slots at
     * kPostLoad, and its winter snow can only be seen on a clone by a hook that wraps its own
     * (see SeasonsOfSkyrim). Nothing is cloned before the main menu, so nothing is missed.
     */
    static void install();

    /**
     * @brief Takes over what MaterialMatcher found and starts the cell pass
     *
     * @param winterSnow The record Seasons of Skyrim's single pass winter snow takes its values
     *        from; nullptr when the load order has none
     */
    static void onMaterialsReady(Materials materials,
                                 const RE::BGSMaterialObject* winterSnow);

private:
    using Clock = std::chrono::steady_clock;
    using Data = ProjectedVertexData::Data;
    using CellKey = std::uint64_t;

    constexpr static std::chrono::microseconds K_SLICE_BUDGET {400}; /**< Main thread time one slice may spend
                                                                        gathering... */
    constexpr static std::chrono::milliseconds K_LOADING_SLICE_BUDGET {4}; /**< ...behind a loading screen */
    constexpr static std::chrono::milliseconds K_SLICE_SPACING {4}; /**< Pause between slices while there is work,
                                                                       so two never share a frame's task drain */
    constexpr static std::chrono::milliseconds K_IDLE_TICK {250}; /**< Slice interval with nothing to do */
    constexpr static std::chrono::milliseconds K_QUIET_PERIOD {300}; /**< No new 3D for this long before a gather */
    constexpr static std::chrono::seconds K_MAX_DIRTY_WAIT {3}; /**< ...but never wait longer than this */
    constexpr static std::chrono::milliseconds K_LOADING_QUIET_PERIOD {100}; /**< The same two behind a loading
                                                                                screen, where 3D streams in
                                                                                without pause and nothing shows */
    constexpr static std::chrono::milliseconds K_LOADING_DIRTY_WAIT {500};
    constexpr static std::chrono::seconds K_SETTLE_RECHECK {5}; /**< One more gather this long after the first */
    constexpr static std::chrono::seconds K_RECEIVER_TIMEOUT {6}; /**< Receivers stop waiting for busy neighbors */
    constexpr static std::chrono::seconds K_GRID_RESCAN {1}; /**< Loaded grid poll when no event asked for one */
    constexpr static std::chrono::seconds K_GARBAGE_INTERVAL {10}; /**< Unused variants are freed this often */
    constexpr static int K_MAX_UNREADY_RETRIES = 3; /**< Re-gathers for references whose 3D had no world transform */
    constexpr static std::size_t K_MAX_NODES_PER_REF = 4096; /**< Scene graph nodes walked per reference */
    constexpr static std::size_t K_APPLY_BATCH = 48; /**< Renderer data swaps per slice */
    constexpr static std::size_t K_RETIRE_BATCH = 256; /**< Pinned records let go per slice */
    constexpr static float K_BLEND_FLOOR = -0.1F; /**< Projection weight at which the shader's snow blend
                                                     reaches nothing: smoothstep(0, 1, 5 * (0.1 + weight)) */
    constexpr static float K_NORMAL_MAP_SAFETY = 0.85F; /**< Per pixel normals lean further up than the vertex
                                                           normal; "just gone" alpha is lowered by this much
                                                           so bumps under cover stay bare */

    /**
     * @brief Vtable hook on TESObjectSTAT's TESBoundObject::Clone3D slot
     */
    struct Clone3DHook {
        static auto thunk(RE::TESBoundObject* base,
                          RE::TESObjectREFR* ref,
                          bool arg3) -> RE::NiAVObject*;
        static inline REL::Relocation<decltype(thunk)> s_func; /**< Whatever occupied the slot before */
        constexpr static std::size_t SLOT = 0x40; /**< TESBoundObject::Clone3D(TESObjectREFR*, bool) */
    };

    /**
     * @brief Vtable hook on the Clone3D slot of the two other base forms Seasons of Skyrim snows on
     *
     * @tparam N Tells the instantiations apart - movable static, container - since each needs an
     *         s_func of its own
     */
    template <std::size_t N> struct WinterClone3DHook {
        static auto thunk(RE::TESBoundObject* base,
                          RE::TESObjectREFR* ref,
                          bool arg3) -> RE::NiAVObject*;
        static inline REL::Relocation<decltype(thunk)> s_func; /**< Whatever occupied the slot before */
    };

    constexpr static std::size_t MOVABLE_STATIC_VTABLE = 2; /**< BGSMovableStatic is a TESFullName and a
                                                               BGSDestructibleObjectForm before it is a
                                                               TESObjectSTAT; the bound object's vtable, the
                                                               one Clone3D is called through, is its third */

    /**
     * @brief Requests a grid scan whenever cells attach or detach
     */
    class CellSink : public RE::BSTEventSink<RE::TESCellAttachDetachEvent> {
    public:
        auto ProcessEvent(const RE::TESCellAttachDetachEvent* event,
                          RE::BSTEventSource<RE::TESCellAttachDetachEvent>* source)
            -> RE::BSEventNotifyControl override;
    };

    /**
     * @brief A solid shape whose triangles go into the height layers
     */
    struct Occluder {
        RE::NiPointer<RE::BSTriShape> keepAlive; /**< Let go on the main thread only */
        Data* pinned {}; /**< The renderer data the raw arrays belong to, with a reference of its own: the
                            shape may be handed a variant while the job runs */
        std::uint32_t vertexCount {};
        std::uint32_t triangleCount {};
        VertexLayout layout;
        RE::NiTransform world;
    };

    /**
     * @brief A shape with projected snow on it
     */
    struct Receiver {
        RE::NiPointer<RE::BSTriShape> keepAlive; /**< Let go on the main thread only */
        ProjectedVertexData::Shape shape; /**< shape.source is pinned by a reference of its own */
        const Data* current {}; /**< What the shape renders with; compared, never dereferenced */
        bool projected {}; /**< Whether the shape has projected snow switched on (its Projected_UV flag) */
        std::uint64_t currentFingerprint {}; /**< ...and its fingerprint when that is a private variant */
        RE::NiTransform world;
        float threshold {}; /**< The shader's cos + (1 - cos) * bias for this static and material: what
                               dot(normal, up) * alpha has to exceed for snow. 0.4 for a standard object at
                               90 degrees, 0.93 for the 30 degree farmhouse walkways */
        float noiseAmplitude {}; /**< (1 - cos) * scale: how far the noise texture can raise that */
        float meanNoise {}; /**< ...on average: the mean of the coverage noise the shape's draws sample */
        float fade {}; /**< The profile's shelterFade */
        bool isSnow {}; /**< Whether the material's snow flag is set, i.e. whether the engine set the Snow shader
                           flag next to Projected_UV */
        bool alphaTest {}; /**< The shape is alpha tested (ShapeView::alphaTest): its threshold follows its alpha */
        std::uint8_t alphaThreshold {}; /**< ...the mesh's threshold, and */
        std::uint8_t currentAlphaThreshold {}; /**< ...the one the shape has now */
    };

    struct RasterJob {
        CellKey source {};
        int cellX {};
        int cellY {};
        std::uint64_t epoch {};
        std::vector<Occluder> occluders;
    };

    struct ReceiverJob {
        CellKey cell {};
        std::uint64_t epoch {};
        ShelterMap::Field field;
        std::vector<Receiver> receivers;
    };

    struct RasterResult {
        CellKey source {};
        int cellX {};
        int cellY {};
        std::uint64_t epoch {};
        std::array<std::shared_ptr<const ShelterMap::Heights>, ShelterMap::K_BLOCK_CELLS> layers;
        std::vector<Occluder> retired;
    };

    /**
     * @brief One renderer data swap waiting for the main thread
     */
    struct Swap {
        RE::NiPointer<RE::BSTriShape> shape;
        Data* data {}; /**< Carries a reference, consumed by ProjectedVertexData::install */
        bool projected {true}; /**< Whether the shape is to have projected snow */
        bool isSnow {}; /**< Whether Snow goes with Projected_UV (the receiver's) */
        std::optional<std::uint8_t> alphaThreshold; /**< Alpha test threshold to set, for an alpha tested shape */
    };

    struct ReceiverResult {
        CellKey cell {};
        std::uint64_t epoch {};
        std::vector<Swap> swaps;
        std::vector<Receiver> receivers; /**< Handed back, current / fingerprint brought up to date */
    };

    using Job = std::variant<RasterJob, ReceiverJob>;
    using Result = std::variant<RasterResult, ReceiverResult>;

    /**
     * @brief Everything known about one loaded exterior cell; main thread only
     */
    struct Cell {
        int cellX {};
        int cellY {};
        RE::FormID formId {};
        bool dirty {true}; /**< Wants a gather */
        Clock::time_point firstDirtyAt;
        Clock::time_point lastDirtyAt;
        std::optional<Clock::time_point> recheckAt; /**< The one unconditional re-gather */
        int unreadyRetries {};
        std::uint64_t epoch {}; /**< Bumped by every gather; older results are dropped */
        bool rasterInFlight {};
        bool receiversInFlight {};
        bool everJudged {}; /**< Whether its receivers were ever computed; the first time does not wait for the
                               neighbors' roofs, see scheduleReceivers */
        std::vector<Receiver> receivers; /**< From the latest gather, unless in flight */
        Clock::time_point receiversSince; /**< When they were gathered, for K_RECEIVER_TIMEOUT */
        std::unordered_map<CellKey, std::shared_ptr<const ShelterMap::Heights>> layers; /**< By source cell */
        std::shared_ptr<const ShelterMap::Heights> map; /**< Maximum over the layers */
        std::uint64_t mapVersion {}; /**< Bumped whenever map changes */
        std::uint64_t computedAgainst {}; /**< Field stamp the receivers were last computed against */
    };

    /**
     * @brief The gather in progress (one at a time); main thread only
     */
    struct Gather {
        CellKey key {};
        int cellX {};
        int cellY {};
        std::vector<RE::NiPointer<RE::TESObjectREFR>> refs;
        std::size_t next {};
        bool unready {};
        std::vector<Occluder> occluders;
        std::vector<Receiver> receivers;
    };

    //
    // Clone pass (loader threads)
    //
    /**
     * @brief What is to be done for the statics under a material; nullptr for nothing
     */
    [[nodiscard]] static auto treatmentOf(const RE::BGSMaterialObject* material) -> const Treatment*;

    /**
     * @brief A treatment, if its profile changes vertex colors or alpha at all; nullptr otherwise
     */
    [[nodiscard]] static auto withGeometry(const Treatment& treatment) -> const Treatment*;

    /**
     * @brief What a profile's geometry settings come to for one static
     */
    struct Settings {
        bool neutralizeColors {}; /**< Its shapes get white vertex colors */
        bool neutralizeAlpha {}; /**< Its shapes start from a vertex alpha of 1 */
        bool shelter {}; /**< Its shapes lose the projection under cover */
        std::optional<float> specularMult; /**< What its shapes' specular strength is multiplied by; std::nullopt
                                              leaves it (no skip list: the profile's own value or nothing) */
    };

    /**
     * @brief The base forms a profile's skip lists name
     */
    struct Kept {
        std::unordered_set<const RE::TESForm*> colors; /**< neutralizeVertexColorsSkip */
        std::unordered_set<const RE::TESForm*> alpha; /**< neutralizeVertexAlphaSkip */
        std::unordered_set<const RE::TESForm*> shelter; /**< roofShelterSkip */
    };

    /**
     * @brief The treatment's profile's geometry settings, each unless its skip list names the static
     *
     * @param base The static - or, under Seasons of Skyrim's winter snow, whatever base form the
     *        snow went on; nullptr for one not known, which gets every setting
     */
    [[nodiscard]] static auto settingsOf(const Treatment& treatment,
                                         const RE::TESForm* base) -> Settings;

    /**
     * @brief Finds the base forms every profile's skip lists name; once, before s_ready
     */
    static void findKept();

    /**
     * @brief Gives the shapes of a fresh clone the shared variant of its settings
     *
     * @param settings settingsOf the clone's base form
     */
    static void dressClone(RE::NiAVObject& root,
                           const Settings& settings);

    /**
     * @brief Scales the specular strength of a shape's material, on a copy of the material
     *
     * The clone shares the model's material with every other instance of the model, snowed or
     * not, so the shape is given a material of its own first (the way po3's Papyrus Extender
     * swaps materials: a copy handed to SetMaterial, which copies it once more into one the
     * property owns). The shader reads the strength at every draw, so nothing is set up again.
     * A material of a class the game does not own - Community Shaders' PBR material, whose
     * roughness scale lives in the field - is left as it is.
     *
     * @param factor The profile's specularMult: 1 leaves the strength as it is
     */
    static void scaleSpecular(RE::BSLightingShaderProperty& shader,
                              float factor);

    /**
     * @brief Whether any of this class's hooks are needed
     */
    [[nodiscard]] static auto isWanted() -> bool;

    /**
     * @brief Does for a clone Seasons of Skyrim has put its winter snow on what is done for one the
     * engine projected a profile's material onto
     *
     * @param base The clone's base form, for settingsOf; nullptr when not known
     * @return bool Whether the clone carries that snow; false leaves it to its base form's material
     */
    static auto dressWinterSnow(RE::NiAVObject& root,
                                const RE::TESForm* base) -> bool;

    /**
     * @brief Gives the shapes under a root Seasons of Skyrim snowed on the projection color its
     * record has now, which makes their draws the profile's, and the falloff values the profile
     * overrides (see SeasonsOfSkyrim)
     *
     * @param switchedOffToo Whether shapes this plugin switched off (s_switchedOff) get them as
     *        well; true on the main thread only, which alone may read that list
     */
    static void adoptWinterSnow(RE::NiAVObject& root,
                                bool switchedOffToo);

    /**
     * @brief What a shape has to be for this plugin to touch or read it
     */
    struct ShapeView {
        RE::BSTriShape* shape {};
        RE::BSLightingShaderProperty* shader {};
        Data* data {};
        VertexLayout layout;
        std::uint32_t vertexCount {};
        std::uint32_t triangleCount {};
        bool keepAlpha {}; /**< See ProjectedVertexData::Shape::keepAlpha */
        RE::NiAlphaProperty* alphaTest {}; /**< The shape's alpha property when its alpha is tested against a
                                              threshold and nothing else, and the mesh paints none: such a shape
                                              is masked like any other, with the threshold scaled to match (see
                                              scaledAlphaThreshold); nullptr otherwise */
    };

    /**
     * @brief Looks at a scene graph object; std::nullopt for anything but a rigid, lit, plain tri shape
     */
    [[nodiscard]] static auto view(RE::NiAVObject& object) -> std::optional<ShapeView>;

    /**
     * @brief Whether an alpha property does nothing but cut pixels whose alpha falls below its threshold
     */
    [[nodiscard]] static auto isPlainAlphaTest(const RE::NiAlphaProperty& alpha) -> bool;

    /**
     * @brief Whether a model's vertex data has any vertex alpha below 1
     */
    [[nodiscard]] static auto paintsAlpha(const Data& source,
                                          std::uint32_t vertexCount) -> bool;

    /**
     * @brief The threshold an alpha property had before this plugin scaled it, if it did
     */
    [[nodiscard]] static auto originalAlphaThreshold(const RE::NiAlphaProperty& alpha) -> std::uint8_t;

    /**
     * @brief Sets an alpha property's threshold, remembering the original the first time; main thread
     */
    static void setAlphaThreshold(RE::NiAlphaProperty& alpha,
                                  std::uint8_t threshold);

    /**
     * @brief The alpha test threshold that keeps a shape's pixels exactly where they were once its
     * vertex alpha has been lowered
     *
     * An alpha tested shape cuts a pixel whose texture alpha times its vertex alpha (times the
     * material's) falls below the threshold - so a vertex alpha written for the projection's sake
     * would cut the shape itself, and every porch floor of a vanilla farmhouse is such a shape
     * (the walkway texture's gaps). But the test is a product against a constant: with the
     * threshold scaled by the lowest vertex alpha the shape gets, every pixel that passed before
     * still passes, exactly so where the alpha is lowest and with a little to spare where it is
     * higher. What changes is that on the vertices left at 1 a texel whose alpha lies between the
     * scaled and the original threshold now shows; for the usual cut-out textures (alpha 0 in the
     * gaps, 1 on the wood) that is nothing, and at worst it is a gap's blurred edge at a distant mip.
     *
     * @param original The mesh's threshold
     * @param lowestAlpha The lowest vertex alpha written to the shape, 0..255
     */
    [[nodiscard]] static auto scaledAlphaThreshold(std::uint8_t original,
                                                   std::uint8_t lowestAlpha) -> std::uint8_t;

    /**
     * @brief Swaps a shape's renderer data and brings its shader flags in line with it
     *
     * Projected_UV follows `projected`, and Snow with it where the material had asked for it (ash
     * has not: on its own the flag would switch the improved snow shading on); Vertex_Colors is
     * switched on for a variant built for a shape that did not show its colors, and off again when
     * such a shape gets the model's data back.
     */
    static void apply(const Swap& swap);

    //
    // Cell pass, main thread
    //
    static void slice();
    static void drainResults();
    static void retireSome();
    static void scanGrid(Clock::time_point now);
    static void markDirty(Cell& cell,
                          Clock::time_point now);
    [[nodiscard]] static auto startGather(Clock::time_point now) -> bool;
    [[nodiscard]] static auto advanceGather(Clock::time_point deadline) -> bool;
    static void collectReference(RE::TESObjectREFR& ref);

    static void finishGather(Clock::time_point now);
    static void scheduleReceivers(Clock::time_point now);
    static void dropCell(CellKey key);
    static void rebuildMap(Cell& cell);
    [[nodiscard]] static auto fieldStamp(const Cell& cell) -> std::uint64_t;
    [[nodiscard]] static auto neighborhoodBusy(const Cell& cell) -> bool;
    static void retire(std::vector<Occluder>& occluders);
    static void retire(std::vector<Receiver>& receivers);
    [[nodiscard]] static auto hasWork() -> bool;

    //
    // Worker
    //
    static void workerLoop();
    static void pumpSlice();
    static void requestSlice();
    static void submit(Job job);
    [[nodiscard]] static auto run(RasterJob& job) -> RasterResult;
    [[nodiscard]] static auto run(ReceiverJob& job) -> ReceiverResult;

    /**
     * @brief Per vertex openness of one receiver: its vertices put into world space and handed to
     * ShelterMap::Field::measureOpenness, which has the details
     *
     * @param normals World space vertex normals, or empty for a mesh without them
     * @param edgeOpenness Openness at which snow visibly ends on a flat surface of this static
     * @param positions Scratch buffer for the world space positions
     * @param openness Out: the result
     * @return bool Whether any vertex is under cover
     */
    [[nodiscard]] static auto measureOpenness(const Receiver& receiver,
                                              const ShelterMap::Field& field,
                                              const VertexLayout& layout,
                                              std::span<const RE::NiPoint3> normals,
                                              float edgeOpenness,
                                              std::vector<RE::NiPoint3>& positions,
                                              std::vector<float>& openness) -> bool;

    [[nodiscard]] static auto keyOf(int cellX,
                                    int cellY) -> CellKey;

    static inline Materials s_materials; /**< Written once, before s_ready */
    static inline const Materials::value_type* s_winterSnow
        = nullptr; /**< The entry of the record behind Seasons of Skyrim's single pass winter snow; ditto */
    static inline std::unordered_map<const ConfigLoader::Profile*, Kept> s_kept; /**< Per profile; ditto */
    static inline std::atomic<bool> s_ready {false}; /**< Gates the hook until s_materials is final */
    static inline std::atomic<bool> s_cellPass {false}; /**< Whether the cell pass runs at all */
    static inline CellSink s_cellSink;

    /**
     * @brief An alpha property whose threshold this plugin has scaled
     */
    struct ScaledAlphaTest {
        RE::NiPointer<RE::NiAlphaProperty> property; /**< Pinned, so that its address stays its own */
        std::uint8_t original {}; /**< The mesh's threshold */
    };

    // Main thread only
    static inline std::unordered_map<const RE::NiAlphaProperty*, ScaledAlphaTest> s_alphaTests;
    /**
     * @brief The shapes whose projection this plugin switched off (apply), pinned so that their
     * addresses stay their own
     *
     * A receiver the next gather would otherwise not recognize: its Projected_UV flag is off, and
     * its projection color - all Clone3D leaves behind - is nothing to go by, since the engine's
     * property constructor gives every property one, alpha 1 included (see collectReference).
     * Entries whose shape is gone are dropped with the other garbage.
     */
    static inline std::unordered_map<const RE::BSTriShape*, RE::NiPointer<RE::BSTriShape>> s_switchedOff;
    static inline std::unordered_map<CellKey, Cell> s_cells;
    static inline std::optional<Gather> s_gather;
    static inline std::vector<Occluder> s_retiredOccluders;
    static inline std::vector<Receiver> s_retiredReceivers;
    static inline std::deque<Swap> s_swaps;
    static inline Clock::time_point s_nextGridScan;
    static inline Clock::time_point s_nextGarbage;

    // Shared with the worker, the loader threads and the event sink, behind s_queueMutex
    static inline std::mutex s_queueMutex;
    static inline std::condition_variable s_queueSignal;
    static inline std::deque<Job> s_jobs;
    static inline std::deque<Result> s_results;
    static inline std::vector<CellKey> s_touched; /**< Cells a static was cloned into (from the hook) */
    static inline bool s_workerStarted = false;

    static inline std::atomic<bool> s_gridChanged {true}; /**< Set by the sink */
    static inline std::atomic<bool> s_loading {false}; /**< Whether a loading screen is up (read in slice): the
                                                          pass hurries while it is */
    static inline std::atomic<bool> s_slicePending {false};
    static inline std::atomic<bool> s_sliceQueued {false};
    static inline std::atomic<Clock::rep> s_lastSliceEnd {0};
};

} // namespace XPMF
