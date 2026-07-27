#pragma once

// lighting_asset.hpp — REN-38-E1..E6 (D-007 row 141): THE LIGHTING VOCABULARY AS AN AUTHORED ASSET.
//
// ⛔⛔ THE GAP THIS ROW CLOSES IS NOT "a few light types are missing". `ckir_lighting.hpp` is 1100 lines of
// gold-standard shading — Filament's punctual attenuation, Heitz LTC rect/line/disk area lights, Karis split-sum
// IBL, SH-L2 irradiance, PCF/PCSS/EVSM/Moment shadows, CSM, contact shadows — and the TECHNIQUE ABI carries
// **exactly one directional light** (`kTiLightDir` + `kTiLightColor`). Every one of those functions is
// UNREACHABLE from any asset, not because it is unfinished but because there is no vocabulary to name it.
//
// ⭐ SAME SHAPE AS `.crdv`, on purpose. Lights are RUNTIME data, so what is authored is the LAYOUT and the
// COMPOSITION: the light record's word map, how many of each type the variant is cooked for, which shadow scheme
// and filter each type uses, whether IBL/decals/clustering participate. `cook_lighting` turns that into the CKIR
// subgraph a technique body returns.
//
// ⛔ WHY PER-TYPE COUNTS AND NOT A RUNTIME LOOP. A fragment program is a DAG; a loop over N lights has to be
// unrolled at cook time. Unrolling one loop that branches per light on a runtime `type` word would evaluate an
// area-light LTC solve for every point light. Counts declared PER TYPE unroll exactly the lobes that exist —
// and "declared, never discovered" is the rule `ckir_variant.hpp` states for every variant axis.
//
// ⛔ THE LIGHT BUFFER IS TYPE-SORTED, and that is load-bearing: record `i` of a type is at
// `light_off + (first_of_type + i) * stride`, so the cook needs no per-light type test at all.

#include <crd/kir/ckir.hpp>

namespace crd::kir
{
struct ShapeIssue; // ckir_shape.hpp — the REN-38 shape check's offending-node report
}

#include <crd/containers/array.hpp>
#include <crd/containers/string.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::lightcook
{

inline constexpr crd::u32 kLightingSchemaVersion = 1U;

enum class LightType : crd::u8
{
    Directional = 0,
    Point,
    Spot,
    Rect, // Heitz LTC polygonal
    Tube, // Heitz LTC line
    Disk, // Heitz LTC disk / sphere
    Count
};
inline constexpr crd::u32 kLightTypeCount = static_cast<crd::u32>(LightType::Count);

// ⛔ Caps stated rather than hidden — an unrolled loop is CODE SIZE, so an unbounded count is an unbounded shader.
inline constexpr crd::u32 kMaxPerType     = 16U;
inline constexpr crd::u32 kMaxTotalLights = 32U;
inline constexpr crd::u32 kMaxDecals      = 8U;
inline constexpr crd::u32 kMaxContactSteps = 16U;
inline constexpr crd::u32 kMaxCascades    = 4U;

enum class ShadowMode : crd::u8
{
    None = 0,
    Csm,  // directional: cascaded, the atlas + per-cascade light_vp bindings
    Map,  // spot: one atlas slice, projected by the light's own shadow_vp
    Cube, // point: RADIAL DISTANCE compare against the light's 6-face slice run
};

// ⛔ THE FILTER DECIDES WHAT THE ATLAS *IS*. Hard/PCF/PCSS need a COMPARISON-sampled depth texture; EVSM and MSM
// need a filterable COLOUR texture of moments. Binding the wrong one compiles and renders — a comparison sampler
// on a moment map returns a depth test against a variance number, which reads as shadows that are simply wrong
// rather than as an error. `lighting_shadow_is_comparison` exists so a caller can never guess.
enum class ShadowFilter : crd::u8
{
    Hard = 0,
    Pcf,
    Pcss, // blocker search → penumbra estimate → variable-radius PCF
    Evsm, // Lauritzen exp-warped variance — filterable, pre-blurrable
    Msm,  // Peters-Klein 4-moment — filterable without EVSM's exp overflow
};

// One field of the light record. ⛔ EVERY offset is declared, exactly as the vertex record's is: a field that
// reads past the stride pulls the NEXT LIGHT's words, so a spotlight takes its neighbour's cone and the scene
// still renders.
struct LightRecordDesc
{
    crd::u32 stride = 16U;

    crd::u32 position    = 0U;  // vec3 — point/spot/area
    crd::u32 color       = 4U;  // vec3 — RGB premultiplied by intensity
    crd::u32 direction   = 8U;  // vec3 — the direction light TRAVELS (directional/spot)
    crd::u32 falloff     = 3U;  // float — 1/radius²
    crd::u32 spot_scale  = 7U;  // float
    crd::u32 spot_offset = 11U; // float

    // ⛔ AREA LIGHTS NEED A SHAPE, and it is four points for a rect, two endpoints for a tube, three for a disk.
    // A set that declares rect lights over a record without them would run the LTC solve on adjacent lights'
    // words — an area light illuminating from a polygon that does not exist.
    bool     has_points = false;
    crd::u32 p0 = 12U, p1 = 15U, p2 = 18U, p3 = 21U;
    bool     has_radius = false;
    crd::u32 radius     = 24U;

    bool     has_shadow_index = false;
    crd::u32 shadow_index     = 25U; // atlas slice (Cube: the FIRST of six)
    bool     has_shadow_vp    = false;
    crd::u32 shadow_vp        = 26U; // mat4, 16 words — Map mode's own projection
    bool     has_shadow_range = false;
    crd::u32 shadow_range     = 42U; // float — Cube mode's far distance, the radial compare's denominator
    bool     has_ies          = false;
    crd::u32 ies_index        = 43U; // float — row in the IES profile atlas
};

// Where the frame's sections live. ⛔ Declared for the same reason the vertex program's header map is: the moment
// a shader hardcodes a word, growing the append-only header feeds every consumer the wrong field.
struct LightingHeaderMap
{
    crd::u32 view_proj  = 6U;  // 16 floats — contact shadows and clustering need the screen projection
    crd::u32 csm_splits = 28U; // 4 floats
    crd::u32 light_off  = 23U; // word offset of the light section
    crd::u32 decal_off  = 24U;
    crd::u32 cluster_off = 21U;
};

struct LightSetDesc
{
    crd::u32 count[kLightTypeCount] = {1U, 0U, 0U, 0U, 0U, 0U};
};

struct ShadowDesc
{
    ShadowMode   mode[kLightTypeCount] = {ShadowMode::None, ShadowMode::None, ShadowMode::None,
                                          ShadowMode::None, ShadowMode::None, ShadowMode::None};
    ShadowFilter filter        = ShadowFilter::Pcf;
    crd::u32     taps          = 8U;   // 1 | 4 | 8 | 16
    crd::u32     cascades      = 4U;   // Csm only
    bool         contact       = false;
    crd::u32     contact_steps = 8U;
};

struct IblDesc
{
    bool diffuse  = false; // SH-L2 irradiance
    bool specular = false; // Karis split-sum prefiltered radiance
};

// ⭐ E5: clustered/froxel culling. The loop reads a per-cluster INDEX LIST instead of walking every light, so the
// unrolled bound is `max_per_cluster` rather than the scene's light count.
struct ClusterDesc
{
    bool     enabled         = false;
    crd::u32 grid[3]         = {16U, 9U, 24U};
    crd::u32 max_per_cluster = 8U;
};

// ⭐ E4: decals as a PROJECTED MATERIAL — applied to the surface BEFORE shading, which is what makes a decal a
// material feature rather than a light. ⛔ Applying one after shading would paint over the lighting and the decal
// would glow in shadow.
struct DecalDesc
{
    crd::u32 count      = 0U;
    crd::u32 stride     = 20U;
    crd::u32 projection = 0U;  // mat4 (world → decal clip)
    crd::u32 tint       = 16U; // vec4 — rgb tint + blend weight
};

struct LightingDesc
{
    crd::containers::String name;
    crd::u32                schema = kLightingSchemaVersion;
    LightingHeaderMap       header;
    LightRecordDesc         record;
    LightSetDesc            set;
    ShadowDesc              shadow;
    IblDesc                 ibl;
    ClusterDesc             cluster;
    DecalDesc               decal;

    explicit LightingDesc(crd::memory::IAllocator* a) : name(a) {}
};

enum class LightingCookError : crd::u8
{
    Ok = 0,
    ParseFailed,
    BadSchema,
    MissingName,
    NoLights,          // a lighting technique that lights nothing is a black screen with no error
    TooManyLights,     // an unrolled loop is CODE SIZE
    FieldOutOfRecord,  // a declared field reads past the stride — pulls the NEXT light's words
    MissingField,      // a declared TYPE needs a field the record does not have
    BadShadowMode,     // a shadow scheme that does not apply to that light type
    BadFilter,         // a tap count that is not 1/4/8/16
    BadCascades,
    BadCluster,
    BadDecal,
    BadContact,
};

[[nodiscard]] const char* lighting_cook_error_text(LightingCookError e) noexcept;

[[nodiscard]] LightingCookError parse_lighting_toml(crd::containers::StringView toml_text, LightingDesc& out,
                                                    crd::containers::String* where = nullptr);
[[nodiscard]] LightingCookError validate_lighting(const LightingDesc& desc, crd::containers::String* where = nullptr);
[[nodiscard]] crd::containers::String emit_lighting_toml(const LightingDesc& desc, crd::memory::IAllocator* a);

// ── The surface + view the lighting composes over — the technique ABI's fixed inputs, by name. ────────────────
struct LightingInputs
{
    int base_color = -1; // vec3
    int metallic   = -1; // float
    int roughness  = -1; // float (perceptual)
    int normal     = -1; // vec3, normalized
    int view_dir   = -1; // vec3, normalized, surface → eye
    int world_pos  = -1; // vec3
    int emissive   = -1; // vec3
    int frag_xy    = -1; // vec2 — the PCF/contact dither and the cluster index need the pixel
};

// Everything the declared composition needs that is NOT in the light buffer. ⛔ A binding left at -1 that the
// declaration requires makes `cook_lighting` FAIL rather than silently drop the term: a missing shadow atlas that
// degraded to "no shadows" is a scene that renders, and nobody looks for a bug in a scene that renders.
struct LightingBindings
{
    int shadow_atlas   = -1; // Texture2DArrayShadow (Hard/Pcf/Pcss) or Texture2DArray (Evsm/Msm)
    int shadow_sampler = -1;
    int csm_light_vp[kMaxCascades] = {-1, -1, -1, -1};
    int csm_map_size   = -1;
    int sh[9]          = {-1, -1, -1, -1, -1, -1, -1, -1, -1};
    int prefiltered    = -1; // TextureCube
    int env_sampler    = -1;
    int ltc_lut        = -1; // Texture2D — the Heitz LTC Minv fit
    int ltc_sampler    = -1;
    int ies_atlas      = -1; // Texture2D — one profile per row
    int ies_sampler    = -1;
    int depth_tex      = -1; // Texture2D — contact shadows march this
    int depth_sampler  = -1;
    int decal_atlas    = -1; // Texture2DArray
    int decal_sampler  = -1;
    int cluster_grid   = -1; // float — clusters' z slice scale (the froxel depth mapping)
    // ⛔⛔ PCSS reads the atlas TWO WAYS IN ONE PROGRAM: comparison taps (the test) AND plain depth reads (the
    // blocker search). The combined GLSL type follows the SAMPLER node, so the blocker search through the
    // comparison sampler emitted `texture(sampler2DArrayShadow, vec3)` — an overload that does not exist. The
    // shape checker (REN-38 audit) is what finally caught it: the cook returned a valid id and the shader
    // failed to compile, the exact 38-E7 class one filter over. A PCSS declaration REQUIRES this second,
    // NON-comparison sampler; every other filter ignores it. APPENDED at the end (struct is caller-initialized).
    int shadow_plain_sampler = -1;
};

// ── ⭐ THE COOK: a validated declaration → the LIT RGB node. ──────────────────────────────────────────────────
// Exactly what a `TechniqueBody` returns, so an authored light set drops into the technique seam with no new
// mechanism. Returns <0 when the declaration is invalid or a required binding/input is missing.
// `shape_issue` (optional) receives the offending node + reason when the REN-38 shape check refuses the built
// graph — a refusal with nothing pointing at the cause is the exact failure mode the check exists to end.
[[nodiscard]] int cook_lighting(const LightingDesc& desc, crd::kir::KGraph& g, const LightingInputs& in,
                                const LightingBindings& b, crd::kir::ShapeIssue* shape_issue = nullptr);

// ── What the declaration REQUIRES, so a caller builds its binding list from the asset. ────────────────────────
[[nodiscard]] bool lighting_needs_shadow_atlas(const LightingDesc& desc) noexcept;
[[nodiscard]] bool lighting_needs_csm(const LightingDesc& desc) noexcept;
[[nodiscard]] bool lighting_needs_ltc(const LightingDesc& desc) noexcept;
[[nodiscard]] bool lighting_needs_ies(const LightingDesc& desc) noexcept;
[[nodiscard]] bool lighting_needs_depth(const LightingDesc& desc) noexcept;
// ⛔ Comparison vs moment: see the ShadowFilter note. Hard/PCF/PCSS sample a COMPARISON texture; EVSM/MSM sample
// a filterable colour texture of moments. Binding one where the other belongs renders wrongly without erroring.
[[nodiscard]] bool lighting_shadow_is_comparison(const LightingDesc& desc) noexcept;
// ⛔ PCSS only: the blocker search needs `shadow_plain_sampler` (a NON-comparison sampler over the same atlas).
[[nodiscard]] bool lighting_needs_plain_shadow_sampler(const LightingDesc& desc) noexcept;
[[nodiscard]] crd::u32 lighting_total_lights(const LightingDesc& desc) noexcept;
// The first record index of a type in the TYPE-SORTED buffer.
[[nodiscard]] crd::u32 lighting_type_first(const LightingDesc& desc, LightType t) noexcept;

// The variant identity — everything that changes the cooked graph. ⛔ Hashed field by field: a POD memcpy folds
// PADDING (stack history) into the id, so the same declaration would hash differently every run.
[[nodiscard]] crd::u64 lighting_variant_id(const LightingDesc& desc) noexcept;

} // namespace crd::lightcook
