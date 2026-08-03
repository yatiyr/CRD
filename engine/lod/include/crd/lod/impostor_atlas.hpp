#pragma once

// ---------------------------------------------------------------------------
// crd-lod — REN-40-C5: OCTAHEDRAL IMPOSTOR ATLAS BAKING.
//
// Renders the source mesh from N² orthographic views distributed over the full
// sphere via octahedral mapping (Cigolle 2014) and packs the result into a
// single atlas texture: RGBA8 (albedo.rgb + coverage.a).
//
// ⛔ CPU SOFTWARE RASTERISER, ON PURPOSE. The atlas is baked at LOD chain build
// time, when a GPU context may or may not be available. A CPU rasteriser is
// portable (it runs in a headless cook, a CI pipeline, a unit test), correct
// (the reference IS the output — there is no second implementation to diverge
// from), and bounded (~300 lines of scanline rasterisation). The tiles are
// small (8–128 px) and there are few of them (4–256 views), so speed does not
// matter — correctness does.
// ---------------------------------------------------------------------------

#include <crd/containers/array.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>
#include <crd/resources/mesh_resource.hpp>

namespace crd::lod
{

// ── ⭐⭐ REN-41: THE MIP PYRAMID LAYOUT. An impostor billboard is drawn only when the object is TINY
// on screen, so its `tile`-texel view is always heavily MINIFIED — and point/bilinear sampling of a
// large source into a few pixels aliases (the far-field "black-dot carpet"). A prefiltered mip pyramid
// is the fix: the FS samples the mip whose texel size matches the screen, so the coverage is averaged
// rather than undersampled. Mips are generated PER TILE (each octahedral view is a different direction —
// blending across tile borders would mix unrelated views), so level `m` is `grid × grid` tiles of
// `tile >> m` texels each, laid out as one `grid*(tile>>m)` square, levels concatenated 0..N-1.
// ⛔ ONE formula, three consumers: the bake fills it, the renderer sizes the buffer from it, and the FS
// (cooked with grid/tile as compile-time constants) unrolls its trilinear taps from it. A second copy of
// this arithmetic anywhere is a silent atlas/reader drift — exactly the class of bug this repo scars over.
[[nodiscard]] constexpr crd::u32 impostor_num_mips(crd::u32 tile) noexcept
{
    crd::u32 n = 0U;
    for (crd::u32 t = tile; t >= 1U; t >>= 1U) { ++n; if (t == 1U) { break; } }
    return n == 0U ? 1U : n;
}
// texel offset (into the packed RGBA8-per-u32 atlas) of level `m`'s first texel.
[[nodiscard]] constexpr crd::u32 impostor_level_offset(crd::u32 grid, crd::u32 tile, crd::u32 m) noexcept
{
    crd::u32 off = 0U;
    for (crd::u32 k = 0U; k < m; ++k) { const crd::u32 s = grid * (tile >> k); off += s * s; }
    return off;
}
// total texels across every mip level — the atlas's word count (1 texel = 1 packed u32).
[[nodiscard]] constexpr crd::u32 impostor_atlas_texels(crd::u32 grid, crd::u32 tile) noexcept
{
    return impostor_level_offset(grid, tile, impostor_num_mips(tile));
}

struct ImpostorAtlas
{
    crd::u32                    grid = 0U; // N: the atlas is N×N tiles
    crd::u32                    tile = 0U; // pixels per tile edge (level 0)
    crd::u32                    mips = 0U; // level count (impostor_num_mips(tile))
    crd::containers::Array<crd::u8> pixels; // ALL levels concatenated, RGBA8, row-major, bottom-left origin

    explicit ImpostorAtlas(crd::memory::IAllocator* a) : pixels(a) {}
};

struct ImpostorBakeReport
{
    crd::u32 tiles_baked   = 0U; // N² when everything went well
    crd::u32 tiles_empty   = 0U; // tiles where no triangle covered any pixel (back-face or degenerate)
    crd::u32 total_pixels   = 0U;
    crd::u32 covered_pixels = 0U; // pixels where at least one triangle rasterised (the coverage sum)
};

// Bake the impostor atlas for a mesh. The mesh must have vertices and indices.
// `grid` and `tile` come from the LodPolicy; `scratch` carries working memory.
// Returns a filled atlas + a report. The atlas's allocator is `out_alloc`.
[[nodiscard]] ImpostorBakeReport bake_impostor_atlas(const crd::resources::MeshResource& mesh,
                                                     crd::u32 grid, crd::u32 tile,
                                                     ImpostorAtlas& out,
                                                     crd::memory::IAllocator* scratch);

// CPU-side octahedral decode: (ox, oy) ∈ [−1,1]² → unit direction (dx, dy, dz).
// The same math as `ckir_ddgi.hpp::oct_decode`, in scalar form.
void oct_decode_cpu(crd::f32 ox, crd::f32 oy, crd::f32& dx, crd::f32& dy, crd::f32& dz);

// CPU-side octahedral encode: unit direction → (ox, oy) ∈ [−1,1]².
void oct_encode_cpu(crd::f32 dx, crd::f32 dy, crd::f32 dz, crd::f32& ox, crd::f32& oy);

} // namespace crd::lod
