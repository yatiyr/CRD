#include <crd/meshgen/meshgen.hpp>

#include <crd/containers/array.hpp>
#include <crd/containers/hash_map.hpp>
#include <crd/resources/mesh_resource.hpp>

#include <algorithm>
#include <crd/math/cmath.hpp>
#include <numbers>

namespace crd::meshgen
{
namespace
{

// Compact 48-byte vertex matching kMeshVertexStride layout.
struct V
{
    float pos[3];
    float nrm[3];
    float uv[2];
    float tan[4]; // tan[3] = bitangent sign (+1 or -1)
};
static_assert(sizeof(V) == 48U);

void push_v(crd::containers::Array<crd::u8>& buf, const V& v)
{
    const auto* bytes = reinterpret_cast<const crd::u8*>(&v);
    for (crd::u32 i = 0; i < sizeof(V); ++i)
    {
        buf.push_back(bytes[i]);
    }
}

void push_i(crd::containers::Array<crd::u8>& buf, crd::u32 idx)
{
    const auto* bytes = reinterpret_cast<const crd::u8*>(&idx);
    for (crd::u32 i = 0; i < 4U; ++i)
    {
        buf.push_back(bytes[i]);
    }
}

crd::u32 vcount(const crd::containers::Array<crd::u8>& buf)
{
    return static_cast<crd::u32>(buf.size() / sizeof(V));
}

crd::resources::MeshResource make_mesh(crd::memory::IAllocator* a)
{
    crd::resources::MeshResource m(a);
    return m;
}

void finalize(crd::resources::MeshResource& m)
{
    crd::resources::MeshPrimitive prim;
    prim.vertex_count       = static_cast<crd::u32>(m.vertices.size() / crd::resources::kMeshVertexStride);
    prim.index_count        = static_cast<crd::u32>(m.indices.size() / 4U);
    prim.vertex_byte_offset = 0U;
    prim.index_byte_offset  = 0U;
    m.primitives.push_back(prim);
}

} // namespace

// ---------------------------------------------------------------------------
// Plane  (xz-plane, normal +Y)
// Grid of quads subdivided into CCW triangles (winding verified: i0,i2,i1 + i1,i2,i3).
// ---------------------------------------------------------------------------
crd::resources::MeshResource make_plane(
    crd::memory::IAllocator* a, float width, float depth, crd::u32 divs_x, crd::u32 divs_z)
{
    auto m = make_mesh(a);
    const crd::u32 nx    = divs_x + 1U;
    const crd::u32 nz    = divs_z + 1U;
    const float    dx    = width / static_cast<float>(divs_x);
    const float    dz    = depth / static_cast<float>(divs_z);
    const float    du    = 1.0F / static_cast<float>(divs_x);
    const float    dv    = 1.0F / static_cast<float>(divs_z);
    const float    hx    = width * 0.5F;
    const float    hz    = depth * 0.5F;

    for (crd::u32 iz = 0; iz < nz; ++iz)
    {
        for (crd::u32 ix = 0; ix < nx; ++ix)
        {
            V v{};
            v.pos[0]  = -hx + static_cast<float>(ix) * dx;
            v.pos[1]  = 0.0F;
            v.pos[2]  = -hz + static_cast<float>(iz) * dz;
            v.nrm[0]  = 0.0F;
            v.nrm[1]  = 1.0F;
            v.nrm[2]  = 0.0F;
            v.uv[0]   = static_cast<float>(ix) * du;
            v.uv[1]   = static_cast<float>(iz) * dv;
            v.tan[0]  = 1.0F; // +X is the tangent direction
            v.tan[1]  = 0.0F;
            v.tan[2]  = 0.0F;
            v.tan[3]  = 1.0F;
            push_v(m.vertices, v);
        }
    }

    // Quad corners: i0=row iz,col ix; i1=same row next col; i2=next row same col; i3=next row next col
    // CCW from +Y: Tri1=(i0,i2,i1)  Tri2=(i1,i2,i3)
    for (crd::u32 iz = 0; iz < divs_z; ++iz)
    {
        for (crd::u32 ix = 0; ix < divs_x; ++ix)
        {
            const crd::u32 i0 = iz * nx + ix;
            const crd::u32 i1 = i0 + 1U;
            const crd::u32 i2 = i0 + nx;
            const crd::u32 i3 = i2 + 1U;
            push_i(m.indices, i0);
            push_i(m.indices, i2);
            push_i(m.indices, i1);
            push_i(m.indices, i1);
            push_i(m.indices, i2);
            push_i(m.indices, i3);
        }
    }

    finalize(m);
    return m;
}

// ---------------------------------------------------------------------------
// Box  (6 faces, 24 unique vertices, 36 indices)
// Each face has its own vertices so normals/tangents are face-constant.
// All faces use CCW winding when viewed from outside.
// ---------------------------------------------------------------------------
crd::resources::MeshResource make_box(
    crd::memory::IAllocator* a, float width, float height, float depth)
{
    auto m = make_mesh(a);
    const float hw = width  * 0.5F;
    const float hh = height * 0.5F;
    const float hd = depth  * 0.5F;

    // Helper: emit a face quad with CCW winding (viewed from face normal direction).
    // v0..v3 are corners; i0,i2,i1 + i1,i2,i3 or i0,i1,i2 + i0,i2,i3 depending on
    // which side the normal faces — we'll just use explicit triangle order per face.
    struct FaceVert { float px, py, pz, nx, ny, nz, u, v, tx, ty, tz; };

    auto emit_face = [&](const FaceVert fv[4])
    {
        const crd::u32 base = vcount(m.vertices);
        for (int k = 0; k < 4; ++k)
        {
            V vtx{};
            vtx.pos[0] = fv[k].px; vtx.pos[1] = fv[k].py; vtx.pos[2] = fv[k].pz;
            vtx.nrm[0] = fv[k].nx; vtx.nrm[1] = fv[k].ny; vtx.nrm[2] = fv[k].nz;
            vtx.uv[0]  = fv[k].u;  vtx.uv[1]  = fv[k].v;
            vtx.tan[0] = fv[k].tx; vtx.tan[1] = fv[k].ty; vtx.tan[2] = fv[k].tz;
            vtx.tan[3] = 1.0F;
            push_v(m.vertices, vtx);
        }
        // CCW quad: 0,1,2  0,2,3
        push_i(m.indices, base + 0U);
        push_i(m.indices, base + 1U);
        push_i(m.indices, base + 2U);
        push_i(m.indices, base + 0U);
        push_i(m.indices, base + 2U);
        push_i(m.indices, base + 3U);
    };

    // +X face (normal +X, tangent +Z)
    {
        const FaceVert fv[4] = {
            { hw, -hh,  hd,  1,0,0, 0,1,  0,0,1},
            { hw, -hh, -hd,  1,0,0, 1,1,  0,0,1},
            { hw,  hh, -hd,  1,0,0, 1,0,  0,0,1},
            { hw,  hh,  hd,  1,0,0, 0,0,  0,0,1},
        };
        emit_face(fv);
    }
    // -X face (normal -X, tangent -Z)
    {
        const FaceVert fv[4] = {
            {-hw, -hh, -hd, -1,0,0, 0,1,  0,0,-1},
            {-hw, -hh,  hd, -1,0,0, 1,1,  0,0,-1},
            {-hw,  hh,  hd, -1,0,0, 1,0,  0,0,-1},
            {-hw,  hh, -hd, -1,0,0, 0,0,  0,0,-1},
        };
        emit_face(fv);
    }
    // +Y face (normal +Y, tangent +X)
    {
        const FaceVert fv[4] = {
            {-hw,  hh,  hd,  0,1,0, 0,1,  1,0,0},
            { hw,  hh,  hd,  0,1,0, 1,1,  1,0,0},
            { hw,  hh, -hd,  0,1,0, 1,0,  1,0,0},
            {-hw,  hh, -hd,  0,1,0, 0,0,  1,0,0},
        };
        emit_face(fv);
    }
    // -Y face (normal -Y, tangent +X)
    {
        const FaceVert fv[4] = {
            {-hw, -hh, -hd,  0,-1,0, 0,1,  1,0,0},
            { hw, -hh, -hd,  0,-1,0, 1,1,  1,0,0},
            { hw, -hh,  hd,  0,-1,0, 1,0,  1,0,0},
            {-hw, -hh,  hd,  0,-1,0, 0,0,  1,0,0},
        };
        emit_face(fv);
    }
    // +Z face (normal +Z, tangent -X)
    {
        const FaceVert fv[4] = {
            { hw, -hh,  hd,  0,0,1, 0,1,  -1,0,0},
            {-hw, -hh,  hd,  0,0,1, 1,1,  -1,0,0},
            {-hw,  hh,  hd,  0,0,1, 1,0,  -1,0,0},
            { hw,  hh,  hd,  0,0,1, 0,0,  -1,0,0},
        };
        emit_face(fv);
    }
    // -Z face (normal -Z, tangent +X)
    {
        const FaceVert fv[4] = {
            {-hw, -hh, -hd,  0,0,-1, 0,1,  1,0,0},
            { hw, -hh, -hd,  0,0,-1, 1,1,  1,0,0},
            { hw,  hh, -hd,  0,0,-1, 1,0,  1,0,0},
            {-hw,  hh, -hd,  0,0,-1, 0,0,  1,0,0},
        };
        emit_face(fv);
    }

    finalize(m);
    return m;
}

// ---------------------------------------------------------------------------
// UV Sphere  (lat_bands x lon_bands, normal = normalized position)
// CCW from outside: for each quad, winding is (i0,i1,i2)+(i2,i1,i3)
// where i1=+lat, i2=+lon neighbor.
// ---------------------------------------------------------------------------
crd::resources::MeshResource make_sphere(
    crd::memory::IAllocator* a, float radius, crd::u32 lat_bands, crd::u32 lon_bands)
{
    auto m = make_mesh(a);
    using F = float;
    constexpr F k_pi = std::numbers::pi_v<F>;

    for (crd::u32 lat = 0; lat <= lat_bands; ++lat)
    {
        const F theta     = k_pi * static_cast<F>(lat) / static_cast<F>(lat_bands);
        const F sin_theta = crd::math::sin(theta);
        const F cos_theta = crd::math::cos(theta);

        for (crd::u32 lon = 0; lon <= lon_bands; ++lon)
        {
            const F phi     = 2.0F * k_pi * static_cast<F>(lon) / static_cast<F>(lon_bands);
            const F sin_phi = crd::math::sin(phi);
            const F cos_phi = crd::math::cos(phi);

            V v{};
            v.nrm[0] = cos_phi * sin_theta;
            v.nrm[1] = cos_theta;
            v.nrm[2] = sin_phi * sin_theta;
            v.pos[0] = radius * v.nrm[0];
            v.pos[1] = radius * v.nrm[1];
            v.pos[2] = radius * v.nrm[2];
            v.uv[0]  = static_cast<F>(lon) / static_cast<F>(lon_bands);
            v.uv[1]  = static_cast<F>(lat) / static_cast<F>(lat_bands);
            // Tangent = d/dphi = (-sin_phi, 0, cos_phi) * sin_theta (non-zero away from poles)
            v.tan[0] = -sin_phi;
            v.tan[1] = 0.0F;
            v.tan[2] =  cos_phi;
            v.tan[3] = 1.0F;
            push_v(m.vertices, v);
        }
    }

    const crd::u32 stride = lon_bands + 1U;
    for (crd::u32 lat = 0; lat < lat_bands; ++lat)
    {
        for (crd::u32 lon = 0; lon < lon_bands; ++lon)
        {
            const crd::u32 i0 = lat * stride + lon;
            const crd::u32 i1 = i0 + 1U;           // +lon
            const crd::u32 i2 = i0 + stride;        // +lat
            const crd::u32 i3 = i2 + 1U;            // +lat +lon
            // CCW from outside: (i0, i2, i1) + (i1, i2, i3)
            push_i(m.indices, i0);
            push_i(m.indices, i2);
            push_i(m.indices, i1);
            push_i(m.indices, i1);
            push_i(m.indices, i2);
            push_i(m.indices, i3);
        }
    }

    finalize(m);
    return m;
}

// ---------------------------------------------------------------------------
// Icosphere  (icosahedron + midpoint subdivision)
// Uses temporary position-only storage during subdivision, then converts.
// ---------------------------------------------------------------------------
crd::resources::MeshResource make_icosphere(
    crd::memory::IAllocator* a, float radius, crd::u32 subdivisions)
{
    // Build icosahedron vertices (unit sphere, 12 verts, 20 faces).
    constexpr float gr = 1.6180339887F; // golden ratio
    crd::containers::Array<float> positions(a); // xyz triples
    crd::containers::Array<crd::u32> faces(a);  // triangle indices (3 per face)

    auto add_pos = [&](float x, float y, float z) -> crd::u32
    {
        const float len = crd::math::sqrt(x*x + y*y + z*z);
        const float inv = 1.0F / len;
        const crd::u32 idx = static_cast<crd::u32>(positions.size() / 3U);
        positions.push_back(x * inv);
        positions.push_back(y * inv);
        positions.push_back(z * inv);
        return idx;
    };

    // 12 icosahedron vertices
    add_pos(-1.0F,  gr,  0.0F);
    add_pos( 1.0F,  gr,  0.0F);
    add_pos(-1.0F, -gr,  0.0F);
    add_pos( 1.0F, -gr,  0.0F);
    add_pos( 0.0F, -1.0F,  gr);
    add_pos( 0.0F,  1.0F,  gr);
    add_pos( 0.0F, -1.0F, -gr);
    add_pos( 0.0F,  1.0F, -gr);
    add_pos( gr,  0.0F, -1.0F);
    add_pos( gr,  0.0F,  1.0F);
    add_pos(-gr,  0.0F, -1.0F);
    add_pos(-gr,  0.0F,  1.0F);

    // 20 faces (CCW winding)
    const crd::u32 ico_faces[20][3] = {
        {0,11,5}, {0,5,1},  {0,1,7},  {0,7,10}, {0,10,11},
        {1,5,9},  {5,11,4}, {11,10,2},{10,7,6},  {7,1,8},
        {3,9,4},  {3,4,2},  {3,2,6},  {3,6,8},   {3,8,9},
        {4,9,5},  {2,4,11}, {6,2,10}, {8,6,7},   {9,8,1},
    };
    for (const auto& f : ico_faces)
    {
        faces.push_back(f[0]);
        faces.push_back(f[1]);
        faces.push_back(f[2]);
    }

    // Subdivide: each triangle → 4 triangles via edge midpoints.
    crd::containers::HashMap<crd::u64, crd::u32> midpoint_cache(a);

    auto get_midpoint = [&](crd::u32 p1, crd::u32 p2) -> crd::u32
    {
        const crd::u32 lo   = std::min(p1, p2);
        const crd::u32 hi   = std::max(p1, p2);
        const crd::u64 key  = (static_cast<crd::u64>(lo) << 32U) | static_cast<crd::u64>(hi);
        crd::u32* cached    = midpoint_cache.find(key);
        if (cached != nullptr)
        {
            return *cached;
        }
        // Midpoint of unit-sphere positions, then re-normalize.
        const float* pa = &positions[static_cast<crd::usize>(p1) * 3U];
        const float* pb = &positions[static_cast<crd::usize>(p2) * 3U];
        const crd::u32 mid = add_pos(
            (pa[0] + pb[0]) * 0.5F,
            (pa[1] + pb[1]) * 0.5F,
            (pa[2] + pb[2]) * 0.5F);
        midpoint_cache.insert(key, mid);
        return mid;
    };

    for (crd::u32 s = 0; s < subdivisions; ++s)
    {
        crd::containers::Array<crd::u32> new_faces(a);
        const crd::usize face_count = faces.size() / 3U;
        for (crd::usize fi = 0; fi < face_count; ++fi)
        {
            const crd::u32 v0  = faces[fi * 3U + 0U];
            const crd::u32 v1  = faces[fi * 3U + 1U];
            const crd::u32 v2  = faces[fi * 3U + 2U];
            const crd::u32 ab  = get_midpoint(v0, v1);
            const crd::u32 bc  = get_midpoint(v1, v2);
            const crd::u32 ca  = get_midpoint(v2, v0);
            // 4 CCW sub-triangles (maintaining parent winding)
            new_faces.push_back(v0); new_faces.push_back(ab); new_faces.push_back(ca);
            new_faces.push_back(v1); new_faces.push_back(bc); new_faces.push_back(ab);
            new_faces.push_back(v2); new_faces.push_back(ca); new_faces.push_back(bc);
            new_faces.push_back(ab); new_faces.push_back(bc); new_faces.push_back(ca);
        }
        faces = std::move(new_faces);
    }

    // Convert to MeshResource: normal = position (unit sphere), tangent = arbitrary perpendicular.
    auto mesh = make_mesh(a);
    const crd::usize vert_count = positions.size() / 3U;
    for (crd::usize vi = 0; vi < vert_count; ++vi)
    {
        const float nx = positions[vi * 3U + 0U];
        const float ny = positions[vi * 3U + 1U];
        const float nz = positions[vi * 3U + 2U];

        // Compute a tangent perpendicular to normal.
        float tx;
        float ty;
        float tz;
        if (std::abs(ny) < 0.999F)
        {
            // Cross(N, world_up) = (-nz, 0, nx)
            tx = -nz; ty = 0.0F; tz = nx;
        }
        else
        {
            // Near poles: use world right
            tx = 1.0F; ty = 0.0F; tz = 0.0F;
        }
        float tlen = crd::math::sqrt(tx*tx + ty*ty + tz*tz);
        const float tinv = (tlen > 1e-6F) ? 1.0F / tlen : 1.0F;
        tx *= tinv; ty *= tinv; tz *= tinv;

        V v{};
        v.pos[0] = nx * radius;
        v.pos[1] = ny * radius;
        v.pos[2] = nz * radius;
        v.nrm[0] = nx; v.nrm[1] = ny; v.nrm[2] = nz;
        v.uv[0]  = 0.5F + crd::math::atan2(nz, nx) / (2.0F * std::numbers::pi_v<float>);
        v.uv[1]  = 0.5F - crd::math::asin(ny) / std::numbers::pi_v<float>;
        v.tan[0] = tx; v.tan[1] = ty; v.tan[2] = tz;
        v.tan[3] = 1.0F;
        push_v(mesh.vertices, v);
    }
    for (crd::usize i = 0; i < faces.size(); ++i)
    {
        push_i(mesh.indices, faces[i]);
    }

    finalize(mesh);
    return mesh;
}

// ---------------------------------------------------------------------------
// Cylinder  (side + top cap + bottom cap)
// Side CCW from outside: Tri1=(bot[j],top[j],bot[j+1]); Tri2=(top[j],top[j+1],bot[j+1])
// ---------------------------------------------------------------------------
crd::resources::MeshResource make_cylinder(
    crd::memory::IAllocator* a, float radius, float height, crd::u32 segs)
{
    auto m = make_mesh(a);
    constexpr float k_pi = std::numbers::pi_v<float>;
    const float half_h = height * 0.5F;

    // Side vertices: segs+1 columns, 2 rows (bottom/top)
    for (crd::u32 j = 0; j <= segs; ++j)
    {
        const float phi     = 2.0F * k_pi * static_cast<float>(j) / static_cast<float>(segs);
        const float cos_phi = crd::math::cos(phi);
        const float sin_phi = crd::math::sin(phi);
        const float u       = static_cast<float>(j) / static_cast<float>(segs);

        // Bottom ring vertex
        {
            V v{};
            v.pos[0] = radius * cos_phi; v.pos[1] = -half_h; v.pos[2] = radius * sin_phi;
            v.nrm[0] = cos_phi;          v.nrm[1] = 0.0F;    v.nrm[2] = sin_phi;
            v.uv[0]  = u;                v.uv[1]  = 1.0F;
            // Tangent = d/dphi normalized = (-sin_phi, 0, cos_phi)
            v.tan[0] = -sin_phi; v.tan[1] = 0.0F; v.tan[2] = cos_phi; v.tan[3] = 1.0F;
            push_v(m.vertices, v);
        }
        // Top ring vertex
        {
            V v{};
            v.pos[0] = radius * cos_phi; v.pos[1] =  half_h; v.pos[2] = radius * sin_phi;
            v.nrm[0] = cos_phi;          v.nrm[1] = 0.0F;    v.nrm[2] = sin_phi;
            v.uv[0]  = u;                v.uv[1]  = 0.0F;
            v.tan[0] = -sin_phi; v.tan[1] = 0.0F; v.tan[2] = cos_phi; v.tan[3] = 1.0F;
            push_v(m.vertices, v);
        }
    }

    // Side indices: 2 verts per column, stride=2
    // bot[j]=j*2, top[j]=j*2+1
    for (crd::u32 j = 0; j < segs; ++j)
    {
        const crd::u32 b0 = j * 2U;
        const crd::u32 t0 = b0 + 1U;
        const crd::u32 b1 = b0 + 2U;
        const crd::u32 t1 = b0 + 3U;
        // CCW from outside: (b0, t0, b1) + (t0, t1, b1)
        push_i(m.indices, b0); push_i(m.indices, t0); push_i(m.indices, b1);
        push_i(m.indices, t0); push_i(m.indices, t1); push_i(m.indices, b1);
    }

    // Bottom cap (normal -Y): center + segs rim verts
    const crd::u32 bot_center_idx = vcount(m.vertices);
    {
        V v{};
        v.pos[0] = 0.0F; v.pos[1] = -half_h; v.pos[2] = 0.0F;
        v.nrm[0] = 0.0F; v.nrm[1] = -1.0F;   v.nrm[2] = 0.0F;
        v.uv[0]  = 0.5F; v.uv[1]  = 0.5F;
        v.tan[0] = 1.0F; v.tan[1] = 0.0F; v.tan[2] = 0.0F; v.tan[3] = 1.0F;
        push_v(m.vertices, v);
    }
    const crd::u32 bot_rim_start = vcount(m.vertices);
    for (crd::u32 j = 0; j <= segs; ++j)
    {
        const float phi = 2.0F * k_pi * static_cast<float>(j) / static_cast<float>(segs);
        const float cp  = crd::math::cos(phi);
        const float sp  = crd::math::sin(phi);
        V v{};
        v.pos[0] = radius * cp; v.pos[1] = -half_h; v.pos[2] = radius * sp;
        v.nrm[0] = 0.0F; v.nrm[1] = -1.0F; v.nrm[2] = 0.0F;
        v.uv[0]  = 0.5F + 0.5F * cp; v.uv[1] = 0.5F + 0.5F * sp;
        v.tan[0] = 1.0F; v.tan[1] = 0.0F; v.tan[2] = 0.0F; v.tan[3] = 1.0F;
        push_v(m.vertices, v);
    }
    // CCW from -Y: looking down from below, CCW means going right first.
    for (crd::u32 j = 0; j < segs; ++j)
    {
        push_i(m.indices, bot_center_idx);
        push_i(m.indices, bot_rim_start + j + 1U);
        push_i(m.indices, bot_rim_start + j);
    }

    // Top cap (normal +Y)
    const crd::u32 top_center_idx = vcount(m.vertices);
    {
        V v{};
        v.pos[0] = 0.0F; v.pos[1] = half_h; v.pos[2] = 0.0F;
        v.nrm[0] = 0.0F; v.nrm[1] = 1.0F;  v.nrm[2] = 0.0F;
        v.uv[0]  = 0.5F; v.uv[1] = 0.5F;
        v.tan[0] = 1.0F; v.tan[1] = 0.0F; v.tan[2] = 0.0F; v.tan[3] = 1.0F;
        push_v(m.vertices, v);
    }
    const crd::u32 top_rim_start = vcount(m.vertices);
    for (crd::u32 j = 0; j <= segs; ++j)
    {
        const float phi = 2.0F * k_pi * static_cast<float>(j) / static_cast<float>(segs);
        const float cp  = crd::math::cos(phi);
        const float sp  = crd::math::sin(phi);
        V v{};
        v.pos[0] = radius * cp; v.pos[1] = half_h; v.pos[2] = radius * sp;
        v.nrm[0] = 0.0F; v.nrm[1] = 1.0F; v.nrm[2] = 0.0F;
        v.uv[0]  = 0.5F + 0.5F * cp; v.uv[1] = 0.5F - 0.5F * sp;
        v.tan[0] = 1.0F; v.tan[1] = 0.0F; v.tan[2] = 0.0F; v.tan[3] = 1.0F;
        push_v(m.vertices, v);
    }
    // CCW from +Y: center → rim[j] → rim[j+1]
    for (crd::u32 j = 0; j < segs; ++j)
    {
        push_i(m.indices, top_center_idx);
        push_i(m.indices, top_rim_start + j);
        push_i(m.indices, top_rim_start + j + 1U);
    }

    finalize(m);
    return m;
}

// ---------------------------------------------------------------------------
// Cone  (side + bottom cap, apex at +Y)
// ---------------------------------------------------------------------------
crd::resources::MeshResource make_cone(
    crd::memory::IAllocator* a, float radius, float height, crd::u32 segs)
{
    auto m = make_mesh(a);
    constexpr float k_pi = std::numbers::pi_v<float>;
    const float half_h = height * 0.5F;

    // Slant normal: (height, radius, 0) normalized (in XY plane at phi=0).
    const float slant_len = crd::math::sqrt(height * height + radius * radius);
    const float ny_slant  = radius / slant_len;  // Y component
    const float nr_slant  = height / slant_len;  // radial component

    // Side vertices: apex repeated segs+1 times (unique for correct UVs) + base ring
    // Each side segment uses: apex vertex + 2 base ring vertices
    for (crd::u32 j = 0; j <= segs; ++j)
    {
        const float phi = 2.0F * k_pi * static_cast<float>(j) / static_cast<float>(segs);
        const float cp  = crd::math::cos(phi);
        const float sp  = crd::math::sin(phi);
        const float u   = static_cast<float>(j) / static_cast<float>(segs);

        // Apex vertex
        {
            V v{};
            v.pos[0] = 0.0F; v.pos[1] = half_h; v.pos[2] = 0.0F;
            v.nrm[0] = nr_slant * cp; v.nrm[1] = ny_slant; v.nrm[2] = nr_slant * sp;
            v.uv[0]  = u + 0.5F / static_cast<float>(segs); v.uv[1] = 0.0F;
            v.tan[0] = -sp; v.tan[1] = 0.0F; v.tan[2] = cp; v.tan[3] = 1.0F;
            push_v(m.vertices, v);
        }
        // Base ring vertex
        {
            V v{};
            v.pos[0] = radius * cp; v.pos[1] = -half_h; v.pos[2] = radius * sp;
            v.nrm[0] = nr_slant * cp; v.nrm[1] = ny_slant; v.nrm[2] = nr_slant * sp;
            v.uv[0]  = u; v.uv[1] = 1.0F;
            v.tan[0] = -sp; v.tan[1] = 0.0F; v.tan[2] = cp; v.tan[3] = 1.0F;
            push_v(m.vertices, v);
        }
    }

    // Side indices: apex=j*2, base=j*2+1
    for (crd::u32 j = 0; j < segs; ++j)
    {
        const crd::u32 a0 = j * 2U;
        const crd::u32 b0 = a0 + 1U;
        const crd::u32 b1 = a0 + 3U;
        // CCW from outside: (apex, base[j], base[j+1])
        push_i(m.indices, a0);
        push_i(m.indices, b0);
        push_i(m.indices, b1);
    }

    // Bottom cap (normal -Y)
    const crd::u32 bot_center_idx = vcount(m.vertices);
    {
        V v{};
        v.pos[0] = 0.0F; v.pos[1] = -half_h; v.pos[2] = 0.0F;
        v.nrm[0] = 0.0F; v.nrm[1] = -1.0F;   v.nrm[2] = 0.0F;
        v.uv[0]  = 0.5F; v.uv[1]  = 0.5F;
        v.tan[0] = 1.0F; v.tan[1] = 0.0F; v.tan[2] = 0.0F; v.tan[3] = 1.0F;
        push_v(m.vertices, v);
    }
    const crd::u32 bot_rim_start = vcount(m.vertices);
    for (crd::u32 j = 0; j <= segs; ++j)
    {
        const float phi = 2.0F * k_pi * static_cast<float>(j) / static_cast<float>(segs);
        V v{};
        v.pos[0] = radius * crd::math::cos(phi); v.pos[1] = -half_h; v.pos[2] = radius * crd::math::sin(phi);
        v.nrm[0] = 0.0F; v.nrm[1] = -1.0F; v.nrm[2] = 0.0F;
        v.uv[0]  = 0.5F + 0.5F * crd::math::cos(phi); v.uv[1] = 0.5F + 0.5F * crd::math::sin(phi);
        v.tan[0] = 1.0F; v.tan[1] = 0.0F; v.tan[2] = 0.0F; v.tan[3] = 1.0F;
        push_v(m.vertices, v);
    }
    for (crd::u32 j = 0; j < segs; ++j)
    {
        push_i(m.indices, bot_center_idx);
        push_i(m.indices, bot_rim_start + j + 1U);
        push_i(m.indices, bot_rim_start + j);
    }

    finalize(m);
    return m;
}

// ---------------------------------------------------------------------------
// Capsule  (cylinder body + hemispherical top/bottom caps)
// ---------------------------------------------------------------------------
crd::resources::MeshResource make_capsule(
    crd::memory::IAllocator* a, float radius, float height, crd::u32 segs, crd::u32 rings)
{
    auto m = make_mesh(a);
    constexpr float k_pi = std::numbers::pi_v<float>;
    const float half_h = height * 0.5F; // half of the *cylinder* body

    auto emit_ring = [&](float y_offset, float theta_start, float theta_end, crd::u32 ring_count)
    {
        // Emit vertices for the hemisphere from theta_start to theta_end
        for (crd::u32 ri = 0; ri <= ring_count; ++ri)
        {
            const float theta = theta_start + (theta_end - theta_start) * static_cast<float>(ri) / static_cast<float>(ring_count);
            const float sin_t = crd::math::sin(theta);
            const float cos_t = crd::math::cos(theta);

            for (crd::u32 j = 0; j <= segs; ++j)
            {
                const float phi = 2.0F * k_pi * static_cast<float>(j) / static_cast<float>(segs);
                const float cp  = crd::math::cos(phi);
                const float sp  = crd::math::sin(phi);

                V v{};
                v.nrm[0] = cp * sin_t; v.nrm[1] = cos_t; v.nrm[2] = sp * sin_t;
                v.pos[0]  = radius * v.nrm[0];
                v.pos[1]  = radius * v.nrm[1] + y_offset;
                v.pos[2]  = radius * v.nrm[2];
                v.uv[0]   = static_cast<float>(j) / static_cast<float>(segs);
                v.uv[1]   = (y_offset + radius + half_h) / (2.0F * (radius + half_h));
                v.tan[0]  = -sp; v.tan[1] = 0.0F; v.tan[2] = cp; v.tan[3] = 1.0F;
                push_v(m.vertices, v);
            }
        }
    };

    // Top hemisphere: theta [0, pi/2] with center at +half_h
    const crd::u32 top_start = vcount(m.vertices);
    emit_ring(half_h, 0.0F, k_pi * 0.5F, rings);

    // Bottom hemisphere: theta [pi/2, pi] with center at -half_h
    const crd::u32 bot_start = vcount(m.vertices);
    emit_ring(-half_h, k_pi * 0.5F, k_pi, rings);

    const crd::u32 stride = segs + 1U;

    // Emit quad indices for a band of rings starting at base_vert
    auto emit_band = [&](crd::u32 base_vert, crd::u32 ring_count)
    {
        for (crd::u32 ri = 0; ri < ring_count; ++ri)
        {
            for (crd::u32 j = 0; j < segs; ++j)
            {
                const crd::u32 i0 = base_vert + ri * stride + j;
                const crd::u32 i1 = i0 + 1U;
                const crd::u32 i2 = i0 + stride;
                const crd::u32 i3 = i2 + 1U;
                push_i(m.indices, i0); push_i(m.indices, i2); push_i(m.indices, i1);
                push_i(m.indices, i1); push_i(m.indices, i2); push_i(m.indices, i3);
            }
        }
    };

    emit_band(top_start, rings);
    emit_band(bot_start, rings);

    // Stitch the two hemispheres together (top last ring → bot first ring).
    const crd::u32 top_last_row = top_start + rings * stride;
    const crd::u32 bot_first_row = bot_start;
    for (crd::u32 j = 0; j < segs; ++j)
    {
        const crd::u32 i0 = top_last_row + j;
        const crd::u32 i1 = top_last_row + j + 1U;
        const crd::u32 i2 = bot_first_row + j;
        const crd::u32 i3 = bot_first_row + j + 1U;
        push_i(m.indices, i0); push_i(m.indices, i2); push_i(m.indices, i1);
        push_i(m.indices, i1); push_i(m.indices, i2); push_i(m.indices, i3);
    }

    finalize(m);
    return m;
}

// ---------------------------------------------------------------------------
// Torus  (major_r = tube center distance, minor_r = tube radius)
// CCW from outside: same winding as UV sphere quads.
// ---------------------------------------------------------------------------
crd::resources::MeshResource make_torus(
    crd::memory::IAllocator* a, float major_r, float minor_r, crd::u32 maj_segs, crd::u32 min_segs)
{
    auto m = make_mesh(a);
    constexpr float k_pi = std::numbers::pi_v<float>;

    for (crd::u32 i = 0; i <= maj_segs; ++i)
    {
        const float phi = 2.0F * k_pi * static_cast<float>(i) / static_cast<float>(maj_segs);
        const float cp  = crd::math::cos(phi);
        const float sp  = crd::math::sin(phi);

        for (crd::u32 j = 0; j <= min_segs; ++j)
        {
            const float theta = 2.0F * k_pi * static_cast<float>(j) / static_cast<float>(min_segs);
            const float ct    = crd::math::cos(theta);
            const float st    = crd::math::sin(theta);

            // Position
            const float px = (major_r + minor_r * ct) * cp;
            const float py =  minor_r * st;
            const float pz = (major_r + minor_r * ct) * sp;

            // Normal: point on tube surface minus ring center
            const float nx = ct * cp;
            const float ny = st;
            const float nz = ct * sp;

            // Tangent: d/dphi at this theta = (-sin_phi * (major_r + minor_r*ct), 0, cos_phi * (major_r + minor_r*ct)) normalized
            const float ring_r = major_r + minor_r * ct;
            float tx = -sp * ring_r;
            float ty = 0.0F;
            float tz =  cp * ring_r;
            const float tlen = crd::math::sqrt(tx*tx + ty*ty + tz*tz);
            const float tinv = (tlen > 1e-6F) ? 1.0F / tlen : 1.0F;
            tx *= tinv; tz *= tinv;

            V v{};
            v.pos[0] = px; v.pos[1] = py; v.pos[2] = pz;
            v.nrm[0] = nx; v.nrm[1] = ny; v.nrm[2] = nz;
            v.uv[0]  = static_cast<float>(i) / static_cast<float>(maj_segs);
            v.uv[1]  = static_cast<float>(j) / static_cast<float>(min_segs);
            v.tan[0] = tx; v.tan[1] = ty; v.tan[2] = tz; v.tan[3] = 1.0F;
            push_v(m.vertices, v);
        }
    }

    const crd::u32 stride = min_segs + 1U;
    for (crd::u32 i = 0; i < maj_segs; ++i)
    {
        for (crd::u32 j = 0; j < min_segs; ++j)
        {
            const crd::u32 i0 = i * stride + j;
            const crd::u32 i1 = i0 + 1U;
            const crd::u32 i2 = i0 + stride;
            const crd::u32 i3 = i2 + 1U;
            push_i(m.indices, i0); push_i(m.indices, i2); push_i(m.indices, i1);
            push_i(m.indices, i1); push_i(m.indices, i2); push_i(m.indices, i3);
        }
    }

    finalize(m);
    return m;
}

} // namespace crd::meshgen
