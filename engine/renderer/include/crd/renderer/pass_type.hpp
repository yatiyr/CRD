#pragma once

#include <crd/core/types.hpp>

namespace crd::renderer
{

// Which rendering pass is being dispatched.
// Frozen values — do not reorder. Stored on disk in PASS / PSOS chunks (ADR-0048).
enum class PassType : crd::u8
{
    DepthPrepass = 0,
    Shadow       = 1, // reserved (Phase 3.1+)
    Forward      = 2,
};

inline constexpr crd::u8 kPassTypeCount = 3U;

// ── Raster state enums ─────────────────────────────────────────────────────

enum class AlphaMode : crd::u8
{
    Opaque      = 0,
    Masked      = 1,
    Transparent = 2,
};

enum class CullMode : crd::u8
{
    Back  = 0,
    Front = 1,
    None  = 2,
};

enum class FillMode : crd::u8
{
    Solid     = 0,
    Wireframe = 1,
};

enum class BlendMode : crd::u8
{
    Zero             = 0,
    One              = 1,
    SrcAlpha         = 2,
    OneMinusSrcAlpha = 3,
};

// Per-pass pipeline state stored in the PSOS chunk.
// Must be exactly 8 bytes — static_assert in material_template.hpp enforces this.
struct RasterState
{
    AlphaMode alpha_mode  = AlphaMode::Opaque;
    CullMode  cull_mode   = CullMode::Back;
    FillMode  fill_mode   = FillMode::Solid;
    crd::u8   depth_test  = 1U;
    crd::u8   depth_write = 1U;
    BlendMode src_blend   = BlendMode::One;
    BlendMode dst_blend   = BlendMode::Zero;
    crd::u8   pad         = 0U;
};

} // namespace crd::renderer
