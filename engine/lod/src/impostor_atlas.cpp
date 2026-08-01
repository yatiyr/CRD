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

crd::f32 clampf(crd::f32 v, crd::f32 lo, crd::f32 hi)
{
    if (v < lo) { return lo; }
    if (v > hi) { return hi; }
    return v;
}

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

    if (grid == 0U || tile == 0U) { return report; }

    const crd::u32 atlas_w = grid * tile;
    const crd::u32 atlas_h = grid * tile;
    const crd::u32 n_pixels = atlas_w * atlas_h;
    out.pixels.clear();
    out.pixels.resize(static_cast<crd::usize>(n_pixels) * 4U);
    std::memset(out.pixels.data(), 0, out.pixels.size());
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

    crd::containers::Array<crd::f32> depth_buf(scratch);
    depth_buf.resize(static_cast<crd::usize>(tile) * tile);

    // for each octahedral tile
    for (crd::u32 ty = 0; ty < grid; ++ty)
    {
        for (crd::u32 tx = 0; tx < grid; ++tx)
        {
            // octahedral UV → view direction
            const crd::f32 ox = (static_cast<crd::f32>(tx) + 0.5F) / static_cast<crd::f32>(grid) * 2.0F - 1.0F;
            const crd::f32 oy = (static_cast<crd::f32>(ty) + 0.5F) / static_cast<crd::f32>(grid) * 2.0F - 1.0F;
            crd::f32 dx, dy, dz;
            oct_decode_cpu(ox, oy, dx, dy, dz);
            V3 fwd = {dx, dy, dz};

            V3 right, up;
            build_basis(fwd, right, up);

            // initialise the tile's depth buffer
            for (crd::u32 di = 0; di < tile * tile; ++di) { depth_buf[di] = 1.0e30F; }

            // the tile's pixel region in the atlas
            const crd::u32 base_x = tx * tile;
            const crd::u32 base_y = ty * tile;

            Tile t{};
            t.rgba    = out.pixels.data() + (static_cast<crd::usize>(base_y) * atlas_w + base_x) * 4U;
            t.depth   = depth_buf.data();
            t.w       = tile;
            t.h       = tile;
            t.covered = 0U;

            // ⛔ The tile's rgba pointer points into the atlas at the tile's top-left corner, but
            // the rasteriser writes ROW-MAJOR within the tile. We need the tile to be contiguous,
            // so use a scratch tile and copy out.
            crd::containers::Array<crd::u8> tile_rgba(scratch);
            tile_rgba.resize(static_cast<crd::usize>(tile) * tile * 4U);
            std::memset(tile_rgba.data(), 0, tile_rgba.size());
            t.rgba = tile_rgba.data();

            // rasterise every triangle
            for (crd::u32 ti = 0; ti < tri_count; ++ti)
            {
                const crd::u32 i0 = indices[ti * 3U + 0U];
                const crd::u32 i1 = indices[ti * 3U + 1U];
                const crd::u32 i2 = indices[ti * 3U + 2U];
                if (i0 >= vert_count || i1 >= vert_count || i2 >= vert_count) { continue; }

                V3 p0 = v3_sub(read_position(verts, crd::resources::kMeshVertexStride, i0), center);
                V3 p1 = v3_sub(read_position(verts, crd::resources::kMeshVertexStride, i1), center);
                V3 p2 = v3_sub(read_position(verts, crd::resources::kMeshVertexStride, i2), center);

                // project onto the (right, up) plane — orthographic from `fwd`
                crd::f32 proj[3][2];
                crd::f32 zvals[3];
                proj[0][0] = v3_dot(p0, right); proj[0][1] = v3_dot(p0, up); zvals[0] = v3_dot(p0, fwd);
                proj[1][0] = v3_dot(p1, right); proj[1][1] = v3_dot(p1, up); zvals[1] = v3_dot(p1, fwd);
                proj[2][0] = v3_dot(p2, right); proj[2][1] = v3_dot(p2, up); zvals[2] = v3_dot(p2, fwd);

                // face normal for this triangle
                V3 edge1 = v3_sub(p1, p0);
                V3 edge2 = v3_sub(p2, p0);
                V3 fn    = v3_norm(v3_cross(edge1, edge2));

                // back-face cull: skip triangles facing away from the view direction
                if (v3_dot(fn, fwd) >= 0.0F) { continue; }

                rasterise_triangle(t, proj, zvals, half_extent);
            }

            // copy the tile into the atlas
            for (crd::u32 row = 0; row < tile; ++row)
            {
                const crd::usize src = static_cast<crd::usize>(row) * tile * 4U;
                const crd::usize dst = (static_cast<crd::usize>(base_y + row) * atlas_w
                                        + base_x) * 4U;
                std::memcpy(out.pixels.data() + dst, tile_rgba.data() + src,
                            static_cast<crd::usize>(tile) * 4U);
            }

            report.covered_pixels += t.covered;
            ++report.tiles_baked;
            if (t.covered == 0U) { ++report.tiles_empty; }
        }
    }

    return report;
}

} // namespace crd::lod
