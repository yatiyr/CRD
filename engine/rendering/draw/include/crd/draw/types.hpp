#pragma once

// crd-draw -- primitive renderer (Phase 3.1 v1a-draw, ADR-0066).
//
// Public types for the retained `RenderBuffer` model:
//   - Color (packed RGBA8, 4 bytes -- PhysX `PxDebugLine` convention)
//   - PrimFlags (u32 bitfield: depth mode + category + width units + picking)
//   - DebugPoint / DebugLine / DebugTriangle / DebugText (the four primitive
//     records the renderer consumes)
//
// These are the durable on-the-wire format. Every immediate-mode `line()`,
// `box_wire()`, `sphere_solid()` etc. call ends up appending one or more
// of these records to the active `RenderBuffer`. The buffer is then either
// rendered (via `add_draw_overlay_pass`) or serialised (for replay capture
// per ADR-0063).

#include <crd/core/types.hpp>
#include <crd/math/vec.hpp>

namespace crd::draw
{
// ---------------------------------------------------------------------------
// Color -- 4-byte packed RGBA8.
//
// Why packed u32 not 4 floats: GPU upload bandwidth is the bottleneck for
// high-volume debug viz. PhysX's `PxDebugLine` learned this lesson; we
// inherit it. 4x memory savings on every primitive vs Vec4f color.
// ---------------------------------------------------------------------------

struct Color
{
    crd::u8 r = 0;
    crd::u8 g = 0;
    crd::u8 b = 0;
    crd::u8 a = 255;

    [[nodiscard]] constexpr crd::u32 packed_rgba() const noexcept
    {
        return (static_cast<crd::u32>(a) << 24) | (static_cast<crd::u32>(b) << 16)
             | (static_cast<crd::u32>(g) << 8)  |  static_cast<crd::u32>(r);
    }

    [[nodiscard]] constexpr bool operator==(const Color& other) const noexcept = default;
};

static_assert(sizeof(Color) == 4, "Color must pack to 4 bytes");

// ---------------------------------------------------------------------------
// Public API version -- frozen at d4 close (ADR-0066 §19.6).
//
// Bumped only on a deliberate API-breaking release. Phase 7 capture / replay
// tooling uses this to decide if a stored capture is loadable. Keep in sync
// with `serialize_render_buffer`'s wire-format version when that lands.
// ---------------------------------------------------------------------------
inline constexpr crd::u32 kDrawApiVersion = 1;

// Pack an 8-bit RGB(A) triple to a single u32 in the engine's RGBA8 wire
// format (the same byte order `Color::packed_rgba()` produces). Useful
// when callers have raw bytes from a hex literal / color picker / theme
// JSON and want the GPU-bound integer directly without spelling
// `Color{r,g,b,a}.packed_rgba()` everywhere. Alpha defaults to fully
// opaque.
//
// Note: `crd::math` does not currently expose an integer Vec3 (MathScalar
// is float-only by design — see `math/scalar.hpp`), so this helper takes
// three scalars rather than a Vec3<u8>. If/when an integer Vec3 lands an
// overload here is the natural extension point.
[[nodiscard]] constexpr crd::u32 pack_rgb_u8(crd::u8 r, crd::u8 g, crd::u8 b,
                                             crd::u8 a = 255) noexcept
{
    return Color{r, g, b, a}.packed_rgba();
}

// Named conventions -- tuned for physics debug viz readability against a
// neutral grey clear. Add more as concrete consumers need them.
inline constexpr Color kBlack      = {  0,   0,   0, 255};
inline constexpr Color kWhite      = {255, 255, 255, 255};
inline constexpr Color kRed        = {255,   0,   0, 255};
inline constexpr Color kGreen      = {  0, 255,   0, 255};
inline constexpr Color kBlue       = {  0,   0, 255, 255};
inline constexpr Color kYellow     = {255, 255,   0, 255};
inline constexpr Color kCyan       = {  0, 255, 255, 255};
inline constexpr Color kMagenta    = {255,   0, 255, 255};
inline constexpr Color kOrange     = {255, 128,   0, 255};
inline constexpr Color kGrey       = {128, 128, 128, 255};

// Physics-specific named colors (consumers override per scene; these are
// the defaults eylem v1c+ broadphase / narrow-phase / solver visualizers
// will reach for). RGB triad goes red=X / green=Y / blue=Z; the literal
// hues match Blender's 3D View axis gizmo (release/datafiles/userdef/
// userdef_default_theme.c -- TH_AXIS_X/Y/Z) for cross-tool muscle memory:
//   X = (255,  51,  82)  reddish-pink   (0xff3352)
//   Y = (139, 220,   0)  yellow-green   (0x8bdc00)
//   Z = ( 40, 144, 255)  sky-blue       (0x2890ff)
// Per ADR-0066 sec 8.
inline constexpr Color kAxisX           = {255,  51,  82, 255};
inline constexpr Color kAxisY           = {139, 220,   0, 255};
inline constexpr Color kAxisZ           = { 40, 144, 255, 255};
inline constexpr Color kBodyDynamic     = {200, 200, 200, 255};
inline constexpr Color kBodyStatic      = {120, 120, 120, 255};
inline constexpr Color kBodyKinematic   = { 80, 200, 240, 255};
inline constexpr Color kBodyAsleep      = {100, 100, 100, 200};
inline constexpr Color kContactPoint    = {255, 200,   0, 255};
inline constexpr Color kContactNormal   = {255, 100,   0, 255};
inline constexpr Color kJointFrame      = {255,   0, 255, 255};
inline constexpr Color kVelocityArrow   = {255, 255, 100, 255};
inline constexpr Color kAabb            = {  0, 200,   0, 200};

// ---------------------------------------------------------------------------
// DepthMode -- controls how each primitive interacts with scene depth.
// ---------------------------------------------------------------------------

enum class DepthMode : crd::u8
{
    // Standard: depth-tested, occluded by world geometry. Default.
    Test    = 0,
    // Always-on-top: ignores scene depth. Used for selection outlines,
    // pinned annotations.
    Always  = 1,
    // X-ray: dual-pass -- dimmed for occluded portions, bright for visible.
    // Reads as "I see the whole shape; I can tell which part is behind walls."
    XRay    = 2
};

// ---------------------------------------------------------------------------
// Category -- primitive tag for UI filtering. Closed enum in 4 bits;
// matches ADR-0066 sec 13. User0..User2 reserved for app-specific uses.
// ---------------------------------------------------------------------------

enum class Category : crd::u8
{
    Physics  = 0,
    Audio    = 1,
    Sdf      = 2,
    Nav      = 3,
    Scene    = 4,
    Renderer = 5,
    User0    = 6,
    User1    = 7,
    User2    = 8,
    Debug    = 9,
    Gizmo    = 10,
    Brush    = 11
    // Range: 0..15 (4-bit field).
};

// ---------------------------------------------------------------------------
// PrimFlags -- packed u32 header carried by every primitive.
//
//   bits  0- 1 : DepthMode (Test / Always / XRay)
//   bits  2- 5 : Category   (Physics / Audio / ... / Brush)
//   bit      6 : width_units (0 = pixels, 1 = world units; lines + arrows
//                only -- solid primitives ignore)
//   bits  7-22 : picking_id (16 bits; 0 = no picking, reserved for Phase 7
//                editor click-to-select)
//   bits 23-31 : reserved
// ---------------------------------------------------------------------------

struct PrimFlags
{
    crd::u32 raw = 0;

    [[nodiscard]] static constexpr PrimFlags make(DepthMode depth, Category cat,
                                                  bool width_in_world_units = false,
                                                  crd::u16 picking = 0) noexcept
    {
        crd::u32 r = 0;
        r |= static_cast<crd::u32>(depth) & 0x3U;
        r |= (static_cast<crd::u32>(cat) & 0xFU) << 2;
        r |= (width_in_world_units ? 1U : 0U) << 6;
        r |= static_cast<crd::u32>(picking) << 7;
        return PrimFlags{r};
    }

    [[nodiscard]] constexpr DepthMode depth() const noexcept
    {
        return static_cast<DepthMode>(raw & 0x3U);
    }
    [[nodiscard]] constexpr Category category() const noexcept
    {
        return static_cast<Category>((raw >> 2) & 0xFU);
    }
    [[nodiscard]] constexpr bool width_in_world_units() const noexcept
    {
        return ((raw >> 6) & 0x1U) != 0U;
    }
    [[nodiscard]] constexpr crd::u16 picking_id() const noexcept
    {
        return static_cast<crd::u16>((raw >> 7) & 0xFFFFU);
    }

    [[nodiscard]] constexpr bool operator==(const PrimFlags& other) const noexcept = default;
};

static_assert(sizeof(PrimFlags) == 4, "PrimFlags must pack to 4 bytes");

// Default = Always (visible regardless of scene depth). Debug primitives
// almost always want to be visible -- consumers opt INTO DepthMode::Test
// (depth-aware) or DepthMode::XRay (dual-pass dimmed-occluded + full-visible)
// for scene-integrated overlays. d2-depth (ADR-0066 sec 19.1) decision.
inline constexpr PrimFlags kDefaultFlags = PrimFlags::make(DepthMode::Always, Category::Debug);

// ---------------------------------------------------------------------------
// Primitive records -- the four kinds of debug primitive the renderer
// understands. Layout pinned via static_asserts; consumers + the GPU
// instance buffer rely on these.
// ---------------------------------------------------------------------------

struct DebugPoint
{
    crd::math::Vec3f pos{0.0F, 0.0F, 0.0F};
    crd::u32         color = kWhite.packed_rgba(); // packed RGBA8 (Color::packed_rgba())
    PrimFlags        flags = kDefaultFlags;
    crd::f32         size_px    = 4.0F;
    crd::f32         lifetime_s = 0.0F; // 0 = single-frame; >0 fades over N seconds
};

struct DebugLine
{
    crd::math::Vec3f a{0.0F, 0.0F, 0.0F};
    crd::math::Vec3f b{0.0F, 0.0F, 0.0F};
    crd::u32         color = kWhite.packed_rgba();
    PrimFlags        flags = kDefaultFlags;
    crd::f32         width      = 1.0F; // pixels by default; world units if flags.width_in_world_units
    crd::f32         lifetime_s = 0.0F;
};

struct DebugTriangle
{
    crd::math::Vec3f a{0.0F, 0.0F, 0.0F};
    crd::math::Vec3f b{0.0F, 0.0F, 0.0F};
    crd::math::Vec3f c{0.0F, 0.0F, 0.0F};
    crd::u32         color = kWhite.packed_rgba();
    PrimFlags        flags = kDefaultFlags;
    crd::f32         lifetime_s = 0.0F;
};

// String storage is caller-owned; the renderer copies it into its own pool
// at submission time. Acceptable to point at static literals.
struct DebugText
{
    crd::math::Vec3f pos{0.0F, 0.0F, 0.0F};
    const char*      str = nullptr;
    crd::u8          size_px = 14;
    crd::u8          anchor  = 0; // 0 = top-left; bit 0 = right-align, bit 1 = bottom-align
    crd::u32         color   = kWhite.packed_rgba();
    PrimFlags        flags   = kDefaultFlags;
    crd::f32         lifetime_s = 0.0F;
};

// API surface freeze pins (ADR-0066 sec 5.1).
//
// DebugPoint:    Vec3(12) + u32(4) + PrimFlags(4) + f32(4) + f32(4) = 28
//                rounded to 32 (4-byte align is fine; 28 is the natural size)
// DebugLine:     Vec3(12) + Vec3(12) + u32(4) + PrimFlags(4) + f32(4) + f32(4) = 40
// DebugTriangle: Vec3(12)*3 + u32(4) + PrimFlags(4) + f32(4) = 48
// DebugText:     Vec3(12) + ptr(8) + u8 + u8 + u32(4) + PrimFlags(4) + f32(4)
//                = 12+8+1+1+(2 pad)+4+4+4 = 36 (with platform padding)
//
// The point + line numbers MUST stay under 32 / 48 bytes respectively for
// the GPU instance-buffer layout to remain efficient. If you find yourself
// growing one of these, push the new field into a sidecar buffer instead.
static_assert(sizeof(DebugPoint)    == 28, "DebugPoint must pack to 28 bytes");
static_assert(sizeof(DebugLine)     == 40, "DebugLine must pack to 40 bytes");
static_assert(sizeof(DebugTriangle) == 48, "DebugTriangle must pack to 48 bytes");

// DebugText size depends on pointer size; only assert the trivially-copyable
// nature, not exact size.
static_assert(alignof(DebugPoint)    == 4, "DebugPoint alignment");
static_assert(alignof(DebugLine)     == 4, "DebugLine alignment");
static_assert(alignof(DebugTriangle) == 4, "DebugTriangle alignment");

} // namespace crd::draw
