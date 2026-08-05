// impostor_atlas.cpp — REN-40-C5. See impostor_atlas.hpp for the rationale.

#include <crd/lod/impostor_atlas.hpp>
#include <crd/resources/mesh_resource.hpp>

#include <cmath>
#include <cstring>

namespace crd::lod
{
namespace
{

// ── CPU-side helpers ──────────────────────────────────────────────────────────

struct V3
{
    crd::f32 x, y, z;
};

V3 v3_sub(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 v3_cross(V3 a, V3 b)
{
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
crd::f32 v3_dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
crd::f32 v3_len(V3 a)
{
    const crd::f32 d = v3_dot(a, a);
    return d > 0.0F ? std::sqrt(d) : 0.0F;
}
V3 v3_norm(V3 a)
{
    const crd::f32 l = v3_len(a);
    if (l < 1.0e-12F) { return {0.0F, 0.0F, 1.0F}; }
    const crd::f32 inv = 1.0F / l;
    return {a.x * inv, a.y * inv, a.z * inv};
}

crd::f32 fabsf_crd(crd::f32 v) { return v < 0.0F ? -v : v; }
crd::f32 signf_crd(crd::f32 v) { return v >= 0.0F ? 1.0F : -1.0F; }

// Build an orthonormal basis from a view direction `fwd` (camera looks TOWARD `fwd`).
// Returns right, up, forward as columns of a rotation matrix.
void build_basis(V3 fwd, V3& right, V3& up)
{
    fwd = v3_norm(fwd);
    V3 hint = {0.0F, 1.0F, 0.0F};
    if (fabsf_crd(v3_dot(fwd, hint)) > 0.99F) { hint = {1.0F, 0.0F, 0.0F}; }
    right = v3_norm(v3_cross(hint, fwd));
    up    = v3_cross(fwd, right);
}

// Read a vertex position from the mesh's interleaved vertex buffer.
V3 read_position(const crd::u8* verts, crd::u32 stride, crd::u32 idx)
{
    const crd::u8* p = verts + static_cast<crd::usize>(idx) * stride;
    crd::f32       pos[3];
    std::memcpy(pos, p, sizeof(pos));
    return {pos[0], pos[1], pos[2]};
}

// ── The scanline rasteriser ───────────────────────────────────────────────────
// Renders a single triangle into a tile. Orthographic projection along `fwd`:
// project each vertex onto the (right, up) plane, clip to the tile, scanline
// with barycentric interpolation for the face normal and depth.

struct Tile
{
    crd::u8* rgba;         // tile_w × tile_h × 4
    crd::f32* depth;       // tile_w × tile_h, initialised to +inf
    crd::u32 w, h;
    crd::u32 covered;
};

void rasterise_triangle(Tile& tile, const crd::f32 p[3][2], const crd::f32 z[3],
                         crd::f32 half_extent)
{
    const crd::f32 tw = static_cast<crd::f32>(tile.w);
    const crd::f32 th = static_cast<crd::f32>(tile.h);
    const crd::f32 inv_ext = (half_extent > 1.0e-12F) ? 0.5F / half_extent : 0.0F;

    // screen coords: map [-half_extent, half_extent] → [0, tile_w/h)
    crd::f32 sx[3], sy[3];
    for (int i = 0; i < 3; ++i)
    {
        sx[i] = (p[i][0] * inv_ext + 0.5F) * tw;
        sy[i] = (p[i][1] * inv_ext + 0.5F) * th;
    }

    // bounding box, clipped to tile
    crd::f32 min_x = sx[0], max_x = sx[0], min_y = sy[0], max_y = sy[0];
    for (int i = 1; i < 3; ++i)
    {
        if (sx[i] < min_x) { min_x = sx[i]; }
        if (sx[i] > max_x) { max_x = sx[i]; }
        if (sy[i] < min_y) { min_y = sy[i]; }
        if (sy[i] > max_y) { max_y = sy[i]; }
    }
    auto ix0 = static_cast<crd::i32>(min_x);
    auto ix1 = static_cast<crd::i32>(max_x + 1.0F);
    auto iy0 = static_cast<crd::i32>(min_y);
    auto iy1 = static_cast<crd::i32>(max_y + 1.0F);
    if (ix0 < 0) { ix0 = 0; }
    if (iy0 < 0) { iy0 = 0; }
    if (ix1 > static_cast<crd::i32>(tile.w)) { ix1 = static_cast<crd::i32>(tile.w); }
    if (iy1 > static_cast<crd::i32>(tile.h)) { iy1 = static_cast<crd::i32>(tile.h); }

    // edge function: e(p) = (v1-v0) × (p-v0) — positive inside for CCW winding
    const crd::f32 dx01 = sx[1] - sx[0], dy01 = sy[1] - sy[0];
    const crd::f32 dx12 = sx[2] - sx[1], dy12 = sy[2] - sy[1];
    const crd::f32 dx20 = sx[0] - sx[2], dy20 = sy[0] - sy[2];
    const crd::f32 area2 = dx01 * (sy[2] - sy[0]) - dy01 * (sx[2] - sx[0]);
    if (fabsf_crd(area2) < 1.0e-6F) { return; } // degenerate
    const crd::f32 inv_area2 = 1.0F / area2;
    // ⛔ Handle BOTH windings: if area2 < 0, the triangle is CW and we must accept negative edge
    // values as "inside". Flipping the sign of inv_area2 makes the barycentrics positive for CW,
    // but it's simpler to flip the edge test: test `e * sign(area2) >= 0` for all three edges.
    const crd::f32 sign_a = (area2 > 0.0F) ? 1.0F : -1.0F;

    for (crd::i32 py = iy0; py < iy1; ++py)
    {
        const crd::f32 cy = static_cast<crd::f32>(py) + 0.5F;
        for (crd::i32 px = ix0; px < ix1; ++px)
        {
            const crd::f32 cx = static_cast<crd::f32>(px) + 0.5F;
            // edge functions at pixel centre
            const crd::f32 e0 = dx12 * (cy - sy[1]) - dy12 * (cx - sx[1]);
            const crd::f32 e1 = dx20 * (cy - sy[2]) - dy20 * (cx - sx[2]);
            const crd::f32 e2 = dx01 * (cy - sy[0]) - dy01 * (cx - sx[0]);
            if (e0 * sign_a < 0.0F || e1 * sign_a < 0.0F || e2 * sign_a < 0.0F) { continue; }

            // barycentrics
            const crd::f32 b0 = e0 * inv_area2;
            const crd::f32 b1 = e1 * inv_area2;
            const crd::f32 b2 = 1.0F - b0 - b1;
            const crd::f32 zp = b0 * z[0] + b1 * z[1] + b2 * z[2];

            const auto idx = static_cast<crd::u32>(py) * tile.w + static_cast<crd::u32>(px);
            if (zp >= tile.depth[idx]) { continue; } // depth test
            tile.depth[idx] = zp;

            const bool was_empty = tile.rgba[idx * 4U + 3U] == 0U;
            // ⛔ ALBEDO = neutral grey; per-instance colour applied at draw time.
            tile.rgba[idx * 4U + 0U] = 204U; // 0.8 * 255
            tile.rgba[idx * 4U + 1U] = 204U;
            tile.rgba[idx * 4U + 2U] = 204U;
            tile.rgba[idx * 4U + 3U] = 255U; // coverage = opaque

            if (was_empty) { ++tile.covered; }
        }
    }
}

} // namespace

// ── Public API ────────────────────────────────────────────────────────────────

void oct_decode_cpu(crd::f32 ox, crd::f32 oy, crd::f32& dx, crd::f32& dy, crd::f32& dz)
{
    const crd::f32 z0 = 1.0F - fabsf_crd(ox) - fabsf_crd(oy);
    if (z0 < 0.0F)
    {
        dx = (1.0F - fabsf_crd(oy)) * signf_crd(ox);
        dy = (1.0F - fabsf_crd(ox)) * signf_crd(oy);
    }
    else
    {
        dx = ox;
        dy = oy;
    }
    dz = z0;
    const crd::f32 len = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (len > 1.0e-12F)
    {
        const crd::f32 inv = 1.0F / len;
        dx *= inv;
        dy *= inv;
        dz *= inv;
    }
    else
    {
        dx = 0.0F;
        dy = 0.0F;
        dz = 1.0F;
    }
}

void oct_encode_cpu(crd::f32 dx, crd::f32 dy, crd::f32 dz, crd::f32& ox, crd::f32& oy)
{
    const crd::f32 s = fabsf_crd(dx) + fabsf_crd(dy) + fabsf_crd(dz);
    if (s < 1.0e-12F) { ox = 0.0F; oy = 0.0F; return; }
    const crd::f32 px = dx / s;
    const crd::f32 py = dy / s;
    if (dz < 0.0F)
    {
        ox = (1.0F - fabsf_crd(py)) * signf_crd(px);
        oy = (1.0F - fabsf_crd(px)) * signf_crd(py);
    }
    else
    {
        ox = px;
        oy = py;
    }
}

ImpostorBakeReport bake_impostor_atlas(const crd::resources::MeshResource& mesh,
                                        crd::u32 grid, crd::u32 tile,
                                        ImpostorAtlas& out,
                                        crd::memory::IAllocator* scratch)
{
    ImpostorBakeReport report{};
    out.grid = grid;
    out.tile = tile;
    out.mips = 0U;

    if (grid == 0U || tile == 0U) { return report; }

    // ⭐⭐ REN-41: the atlas is now a MIP PYRAMID (see impostor_atlas.hpp). Allocate every level up front;
    // level 0 is baked antialiased (supersampled coverage → fractional alpha), the rest box-downsampled
    // PER TILE. The albedo is a constant grey, so the only signal that varies is coverage (alpha) — which
    // is exactly what makes averaging a correct prefilter.
    out.mips = impostor_num_mips(tile);
    const crd::u32 total_texels = impostor_atlas_texels(grid, tile);
    out.pixels.clear();
    out.pixels.resize(static_cast<crd::usize>(total_texels) * 4U);
    std::memset(out.pixels.data(), 0, out.pixels.size());
    // level-0 dimensions for the coverage/report accounting below
    const crd::u32 atlas_w = grid * tile;
    const crd::u32 n_pixels = atlas_w * atlas_w;
    report.total_pixels = n_pixels;

    const crd::u32 vert_count = static_cast<crd::u32>(mesh.vertices.size() / crd::resources::kMeshVertexStride);
    const crd::u32 idx_count  = static_cast<crd::u32>(mesh.indices.size() / sizeof(crd::u32));
    if (vert_count == 0U || idx_count < 3U) { return report; }

    const crd::u8* verts   = mesh.vertices.data();
    const crd::u32* indices = reinterpret_cast<const crd::u32*>(mesh.indices.data());
    const crd::u32 tri_count = idx_count / 3U;

    // compute mesh bounding sphere (for fitting the orthographic projection)
    V3 center = {0.0F, 0.0F, 0.0F};
    if (mesh.has_bounds())
    {
        center.x = (mesh.bounds_min[0] + mesh.bounds_max[0]) * 0.5F;
        center.y = (mesh.bounds_min[1] + mesh.bounds_max[1]) * 0.5F;
        center.z = (mesh.bounds_min[2] + mesh.bounds_max[2]) * 0.5F;
    }
    crd::f32 max_r2 = 0.0F;
    for (crd::u32 vi = 0; vi < vert_count; ++vi)
    {
        V3 p = read_position(verts, crd::resources::kMeshVertexStride, vi);
        V3 d = v3_sub(p, center);
        const crd::f32 r2 = v3_dot(d, d);
        if (r2 > max_r2) { max_r2 = r2; }
    }
    crd::f32 radius = (max_r2 > 0.0F) ? std::sqrt(max_r2) : 1.0F;
    if (radius < 1.0e-6F) { radius = 1.0F; }
    const crd::f32 half_extent = radius * 1.05F; // 5% margin for rasterisation

    // ⭐⭐ REN-41: SUPERSAMPLE level 0. Rasterise each tile at `tile*SS` and box-downsample to `tile`, so the
    // silhouette coverage becomes FRACTIONAL (antialiased) instead of a binary 0/255 edge — the single biggest
    // per-impostor quality fix. SS=4 → 16 subsamples/texel, enough for a clean edge at impostor scale.
    constexpr crd::u32 kSS = 4U;
    const crd::u32     hi  = tile * kSS;
    crd::containers::Array<crd::f32> depth_buf(scratch);
    depth_buf.resize(static_cast<crd::usize>(hi) * hi);
    crd::containers::Array<crd::u8> hi_rgba(scratch);
    hi_rgba.resize(static_cast<crd::usize>(hi) * hi * 4U);

    const auto lvl_texel = [&](crd::u32 m, crd::u32 tcol, crd::u32 trow, crd::u32 x, crd::u32 y) -> crd::u8* {
        const crd::u32 tpx  = tile >> m;              // this level's tile edge
        const crd::u32 dim  = grid * tpx;             // this level's full square edge
        const crd::u32 gx   = tcol * tpx + x;
        const crd::u32 gy   = trow * tpx + y;
        const crd::usize idx = static_cast<crd::usize>(impostor_level_offset(grid, tile, m))
                               + static_cast<crd::usize>(gy) * dim + gx;
        return out.pixels.data() + idx * 4U;
    };

    for (crd::u32 ty = 0; ty < grid; ++ty)
    {
        for (crd::u32 tx = 0; tx < grid; ++tx)
        {
            const crd::f32 ox = (static_cast<crd::f32>(tx) + 0.5F) / static_cast<crd::f32>(grid) * 2.0F - 1.0F;
            const crd::f32 oy = (static_cast<crd::f32>(ty) + 0.5F) / static_cast<crd::f32>(grid) * 2.0F - 1.0F;
            crd::f32 dx, dy, dz;
            oct_decode_cpu(ox, oy, dx, dy, dz);
            V3 fwd = {dx, dy, dz};
            V3 right, up;
            build_basis(fwd, right, up);

            for (crd::u32 di = 0; di < hi * hi; ++di) { depth_buf[di] = 1.0e30F; }
            std::memset(hi_rgba.data(), 0, hi_rgba.size());

            Tile t{};
            t.rgba    = hi_rgba.data();
            t.depth   = depth_buf.data();
            t.w       = hi;
            t.h       = hi;
            t.covered = 0U;

            for (crd::u32 ti = 0; ti < tri_count; ++ti)
            {
                const crd::u32 i0 = indices[ti * 3U + 0U];
                const crd::u32 i1 = indices[ti * 3U + 1U];
                const crd::u32 i2 = indices[ti * 3U + 2U];
                if (i0 >= vert_count || i1 >= vert_count || i2 >= vert_count) { continue; }
                V3 p0 = v3_sub(read_position(verts, crd::resources::kMeshVertexStride, i0), center);
                V3 p1 = v3_sub(read_position(verts, crd::resources::kMeshVertexStride, i1), center);
                V3 p2 = v3_sub(read_position(verts, crd::resources::kMeshVertexStride, i2), center);
                crd::f32 proj[3][2];
                crd::f32 zvals[3];
                proj[0][0] = v3_dot(p0, right); proj[0][1] = v3_dot(p0, up); zvals[0] = v3_dot(p0, fwd);
                proj[1][0] = v3_dot(p1, right); proj[1][1] = v3_dot(p1, up); zvals[1] = v3_dot(p1, fwd);
                proj[2][0] = v3_dot(p2, right); proj[2][1] = v3_dot(p2, up); zvals[2] = v3_dot(p2, fwd);
                V3 edge1 = v3_sub(p1, p0);
                V3 edge2 = v3_sub(p2, p0);
                V3 fn    = v3_norm(v3_cross(edge1, edge2));
                if (v3_dot(fn, fwd) >= 0.0F) { continue; }
                rasterise_triangle(t, proj, zvals, half_extent);
            }

            // downsample hi → level 0: coverage = mean of the SS×SS block's alpha; rgb = the constant albedo.
            crd::u32 tile_covered = 0U;
            for (crd::u32 y = 0; y < tile; ++y)
            {
                for (crd::u32 x = 0; x < tile; ++x)
                {
                    crd::u32 asum = 0U;
                    for (crd::u32 sy = 0; sy < kSS; ++sy)
                    {
                        for (crd::u32 sx = 0; sx < kSS; ++sx)
                        {
                            const crd::usize si = (static_cast<crd::usize>(y * kSS + sy) * hi + (x * kSS + sx)) * 4U;
                            asum += hi_rgba[si + 3U];
                        }
                    }
                    const crd::u32 cov = asum / (kSS * kSS); // 0..255 fractional coverage
                    crd::u8* d = lvl_texel(0U, tx, ty, x, y);
                    d[0] = 204U; d[1] = 204U; d[2] = 204U; d[3] = static_cast<crd::u8>(cov);
                    if (cov > 0U) { ++tile_covered; }
                }
            }

            // mips: box 2×2 average of coverage, per tile (no cross-tile bleed — see the header note).
            for (crd::u32 m = 1U; m < out.mips; ++m)
            {
                const crd::u32 tpx = tile >> m;
                for (crd::u32 y = 0; y < tpx; ++y)
                {
                    for (crd::u32 x = 0; x < tpx; ++x)
                    {
                        crd::u32 asum = 0U;
                        for (crd::u32 sy = 0; sy < 2U; ++sy)
                        {
                            for (crd::u32 sx = 0; sx < 2U; ++sx)
                            {
                                asum += lvl_texel(m - 1U, tx, ty, x * 2U + sx, y * 2U + sy)[3];
                            }
                        }
                        crd::u8* d = lvl_texel(m, tx, ty, x, y);
                        d[0] = 204U; d[1] = 204U; d[2] = 204U; d[3] = static_cast<crd::u8>(asum / 4U);
                    }
                }
            }

            report.covered_pixels += tile_covered;
            ++report.tiles_baked;
            if (tile_covered == 0U) { ++report.tiles_empty; }
        }
    }

    return report;
}

} // namespace crd::lod
