// tests/asset-io/test_condition.cpp — GEO-2 gates: exact weld, crease-angle weighted normals (analytic), the
// MikkTSpace-compatible tangent frame (analytic + the REFERENCE mikktspace.c as ORACLE — the compatibility contract every
// DCC bakes normal maps against), UV-mirror seam splitting, and BIT-STABILITY under face reordering (the determinism gate:
// canonical-sorted accumulation makes a permuted import byte-identical).
//
// mikktspace.c is 3rd-party TEST-ORACLE code only (same posture as Eigen/MKL oracles elsewhere): the production pipeline
// ships OUR implementation; the reference certifies it.

#include <catch2/catch_test_macros.hpp>

#include <crd/assetio/condition.hpp>
#include <crd/assetio/imported_asset.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <cmath>
#include <cstring>

extern "C"
{
// NOLINTBEGIN(misc-include-cleaner)
#include <mikktspace.h>
#include "mikktspace.c" // NOLINT — the oracle implementation compiled as C++ within this TU (the cooker's mesh.cpp pattern)
// NOLINTEND(misc-include-cleaner)
}

namespace aio = crd::assetio;
using V2      = crd::math::Vec2<crd::f32>;
using V3      = crd::math::Vec3<crd::f32>;

namespace
{

void push_v(aio::ImportedMesh& m, float px, float py, float pz, float nx, float ny, float nz, float u, float v)
{
    m.positions.push_back(V3{px, py, pz});
    m.normals.push_back(V3{nx, ny, nz});
    m.uv0.push_back(V2{u, v});
}

// a unit quad in XY (+Z normal), straight UVs, as INDEXED two triangles: (0,1,2) (0,2,3)
void build_quad(aio::ImportedMesh& m)
{
    push_v(m, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F);
    push_v(m, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 1.0F, 0.0F);
    push_v(m, 1.0F, 1.0F, 0.0F, 0.0F, 0.0F, 1.0F, 1.0F, 1.0F);
    push_v(m, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 1.0F);
    const crd::u32 idx[6] = {0, 1, 2, 0, 2, 3};
    for (crd::u32 i : idx) { m.indices.push_back(i); }
}

// ── the mikktspace.c oracle bridge (face-varying getters over our indexed mesh) ────────────────────────────────────────
struct MikkOracle
{
    const aio::ImportedMesh* mesh;
    float*                   out_t;    // corner*4: xyz + sign
};

int mo_num_faces(const SMikkTSpaceContext* c)
{
    return static_cast<int>(static_cast<const MikkOracle*>(c->m_pUserData)->mesh->triangle_count());
}
int mo_verts_of_face(const SMikkTSpaceContext*, int) { return 3; }
void mo_position(const SMikkTSpaceContext* c, float* out, int f, int v)
{
    const auto* o = static_cast<const MikkOracle*>(c->m_pUserData);
    const V3&   p = o->mesh->positions[o->mesh->indices[static_cast<crd::usize>(f) * 3U + static_cast<crd::usize>(v)]];
    out[0]        = p.x;
    out[1]        = p.y;
    out[2]        = p.z;
}
void mo_normal(const SMikkTSpaceContext* c, float* out, int f, int v)
{
    const auto* o = static_cast<const MikkOracle*>(c->m_pUserData);
    const V3&   n = o->mesh->normals[o->mesh->indices[static_cast<crd::usize>(f) * 3U + static_cast<crd::usize>(v)]];
    out[0]        = n.x;
    out[1]        = n.y;
    out[2]        = n.z;
}
void mo_texcoord(const SMikkTSpaceContext* c, float* out, int f, int v)
{
    const auto* o = static_cast<const MikkOracle*>(c->m_pUserData);
    const V2&   t = o->mesh->uv0[o->mesh->indices[static_cast<crd::usize>(f) * 3U + static_cast<crd::usize>(v)]];
    out[0]        = t.x;
    out[1]        = t.y;
}
void mo_set(const SMikkTSpaceContext* c, const float* t, float sign, int f, int v)
{
    auto*            o      = static_cast<MikkOracle*>(c->m_pUserData);
    const crd::usize corner = static_cast<crd::usize>(f) * 3U + static_cast<crd::usize>(v);
    o->out_t[corner * 4U + 0U] = t[0];
    o->out_t[corner * 4U + 1U] = t[1];
    o->out_t[corner * 4U + 2U] = t[2];
    o->out_t[corner * 4U + 3U] = sign;
}

// run the REFERENCE MikkTSpace over `mesh`, per-corner output into `out_t` (corner_count*4 floats)
bool run_mikkt_oracle(const aio::ImportedMesh& mesh, float* out_t)
{
    SMikkTSpaceInterface itf{};
    itf.m_getNumFaces          = mo_num_faces;
    itf.m_getNumVerticesOfFace = mo_verts_of_face;
    itf.m_getPosition          = mo_position;
    itf.m_getNormal            = mo_normal;
    itf.m_getTexCoord          = mo_texcoord;
    itf.m_setTSpaceBasic       = mo_set;
    MikkOracle          user{&mesh, out_t};
    SMikkTSpaceContext ctx{&itf, &user};
    return genTangSpaceDefault(&ctx) != 0;
}

} // namespace

TEST_CASE("assetio: weld_exact -- soup collapses, hard edges stay split, derived tangents drop", "[assetio][condition]")
{
    crd::memory::TlsfAllocator alloc(8U << 20U);

    SECTION("coplanar soup quad welds 6 -> 4")
    {
        aio::ImportedMesh m(&alloc);
        // STL-style soup: two coplanar triangles, identical facet normals; shared corners bit-identical
        const float q[4][3] = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}};
        const int   tri[2][3] = {{0, 1, 2}, {0, 2, 3}};
        for (auto& t : tri)
        {
            for (int c : t)
            {
                m.positions.push_back(V3{q[c][0], q[c][1], q[c][2]});
                m.normals.push_back(V3{0.0F, 0.0F, 1.0F});
                m.indices.push_back(static_cast<crd::u32>(m.positions.size() - 1U));
            }
        }
        const crd::u32 removed = aio::weld_exact(m, &alloc);
        CHECK(removed == 2U);
        CHECK(m.positions.size() == 4U);
        CHECK(m.indices.size() == 6U);
        CHECK(m.is_consistent());
        CHECK(m.has_normals());
    }
    SECTION("a hard edge (different facet normals at one position) STAYS split")
    {
        aio::ImportedMesh m(&alloc);
        // two triangles sharing an edge but with different facet normals (a 90-degree fold)
        const V3 n0{0.0F, 0.0F, 1.0F};
        const V3 n1{0.0F, 1.0F, 0.0F};
        const V3 shared_a{0.0F, 0.0F, 0.0F};
        const V3 shared_b{1.0F, 0.0F, 0.0F};
        m.positions.push_back(shared_a); m.normals.push_back(n0);
        m.positions.push_back(shared_b); m.normals.push_back(n0);
        m.positions.push_back(V3{0.0F, 1.0F, 0.0F}); m.normals.push_back(n0);
        m.positions.push_back(shared_a); m.normals.push_back(n1);
        m.positions.push_back(shared_b); m.normals.push_back(n1);
        m.positions.push_back(V3{0.0F, 0.0F, -1.0F}); m.normals.push_back(n1);
        for (crd::u32 i = 0; i < 6U; ++i) { m.indices.push_back(i); }
        const crd::u32 removed = aio::weld_exact(m, &alloc);
        CHECK(removed == 0U); // every tuple differs (normal differs at the fold) — hard edge preserved
        CHECK(m.positions.size() == 6U);
    }
}

TEST_CASE("assetio: generate_normals -- crease angle drives faceted vs smoothed (analytic)", "[assetio][condition]")
{
    crd::memory::TlsfAllocator alloc(8U << 20U);
    // three faces of a unit-cube corner meeting at the origin: +Z, +X, +Y face normals; dihedral tilts are 90 degrees
    const auto build_corner = [&](aio::ImportedMesh& m) {
        const V3 o{0.0F, 0.0F, 0.0F};
        // XY face (+Z), YZ face (+X), ZX face (+Y) — each a triangle with its wedge at the origin
        const V3 t0[3] = {o, V3{1, 0, 0}, V3{0, 1, 0}};
        const V3 t1[3] = {o, V3{0, 1, 0}, V3{0, 0, 1}};
        const V3 t2[3] = {o, V3{0, 0, 1}, V3{1, 0, 0}};
        for (const auto* t : {t0, t1, t2})
        {
            for (int i = 0; i < 3; ++i)
            {
                m.positions.push_back(t[i]);
                m.indices.push_back(static_cast<crd::u32>(m.positions.size() - 1U));
            }
        }
    };

    SECTION("30-degree crease: 90-degree folds stay FACETED (3 split verts at the corner)")
    {
        aio::ImportedMesh m(&alloc);
        build_corner(m);
        aio::generate_normals(m, &alloc, 30.0F * 3.14159265F / 180.0F);
        REQUIRE(m.has_normals());
        // the corner position appears 3 times, each with its own face normal (+Z / +X / +Y)
        crd::u32 corner_verts = 0;
        for (crd::usize v = 0; v < m.positions.size(); ++v)
        {
            if (m.positions[v].x == 0.0F && m.positions[v].y == 0.0F && m.positions[v].z == 0.0F) { ++corner_verts; }
        }
        CHECK(corner_verts == 3U);
    }
    SECTION("95-degree crease: the corner SMOOTHES to the (1,1,1)/sqrt(3) average")
    {
        aio::ImportedMesh m(&alloc);
        build_corner(m);
        aio::generate_normals(m, &alloc, 95.0F * 3.14159265F / 180.0F);
        REQUIRE(m.has_normals());
        crd::u32 corner_verts = 0;
        for (crd::usize v = 0; v < m.positions.size(); ++v)
        {
            if (m.positions[v].x == 0.0F && m.positions[v].y == 0.0F && m.positions[v].z == 0.0F)
            {
                ++corner_verts;
                const float s = 1.0F / std::sqrt(3.0F); // equal wedge angles (90 degrees each) => the symmetric average
                CHECK(std::abs(m.normals[v].x - s) < 1e-5F);
                CHECK(std::abs(m.normals[v].y - s) < 1e-5F);
                CHECK(std::abs(m.normals[v].z - s) < 1e-5F);
            }
        }
        CHECK(corner_verts == 1U); // the smoothed corner WELDS to one vertex
    }
}

TEST_CASE("assetio: generate_tangents -- straight UVs give T=+X w=+1 (analytic)", "[assetio][condition]")
{
    crd::memory::TlsfAllocator alloc(8U << 20U);
    aio::ImportedMesh          m(&alloc);
    build_quad(m);
    REQUIRE(aio::generate_tangents(m, &alloc));
    REQUIRE(m.tangent.size() == m.positions.size());
    for (crd::usize v = 0; v < m.tangent.size(); ++v)
    {
        CHECK(std::abs(m.tangent[v].x - 1.0F) < 1e-6F); // dP/du = +X
        CHECK(std::abs(m.tangent[v].y) < 1e-6F);
        CHECK(std::abs(m.tangent[v].z) < 1e-6F);
        CHECK(m.tangent[v].w == 1.0F);
    }
    CHECK(m.is_consistent());
}

TEST_CASE("assetio: generate_tangents -- a MIRRORED chart splits the seam and flips the sign", "[assetio][condition]")
{
    crd::memory::TlsfAllocator alloc(8U << 20U);
    aio::ImportedMesh          m(&alloc);
    // two quads side by side sharing the x=1 edge; the RIGHT quad's u runs MIRRORED (u: 1 -> 0)
    push_v(m, 0.0F, 0.0F, 0.0F, 0, 0, 1, 0.0F, 0.0F); // left quad: u = x
    push_v(m, 1.0F, 0.0F, 0.0F, 0, 0, 1, 1.0F, 0.0F);
    push_v(m, 1.0F, 1.0F, 0.0F, 0, 0, 1, 1.0F, 1.0F);
    push_v(m, 0.0F, 1.0F, 0.0F, 0, 0, 1, 0.0F, 1.0F);
    push_v(m, 2.0F, 0.0F, 0.0F, 0, 0, 1, 0.0F, 0.0F); // right quad: u = 2 - x (mirrored)
    push_v(m, 2.0F, 1.0F, 0.0F, 0, 0, 1, 0.0F, 1.0F);
    const crd::u32 idx[12] = {0, 1, 2, 0, 2, 3, /* right: */ 1, 4, 5, 1, 5, 2};
    for (crd::u32 i : idx) { m.indices.push_back(i); }

    const crd::usize before = m.positions.size(); // 6
    REQUIRE(aio::generate_tangents(m, &alloc));
    CHECK(m.positions.size() == before + 2U); // the two seam vertices (1 and 2) DUPLICATE per handedness
    REQUIRE(m.tangent.size() == m.positions.size());
    // every vertex used by the left chart: w=+1, T=+X; by the right (mirrored) chart: w=-1, T=-X
    for (crd::usize f = 0; f < m.triangle_count(); ++f)
    {
        const bool mirrored = f >= 2U;
        for (int c = 0; c < 3; ++c)
        {
            const crd::u32 v = m.indices[f * 3U + static_cast<crd::usize>(c)];
            CHECK(m.tangent[v].w == (mirrored ? -1.0F : 1.0F));
            CHECK(std::abs(m.tangent[v].x - (mirrored ? -1.0F : 1.0F)) < 1e-5F);
        }
    }
}

TEST_CASE("assetio: conditioning is BIT-STABLE under face reordering (the determinism gate)", "[assetio][condition]")
{
    crd::memory::TlsfAllocator alloc(16U << 20U);
    // a 3x3 vertex grid (2x2 quads = 8 triangles) with slightly irregular positions + UVs
    const auto build_grid = [&](aio::ImportedMesh& m, bool permute) {
        for (int y = 0; y < 3; ++y)
        {
            for (int x = 0; x < 3; ++x)
            {
                const float fx = static_cast<float>(x) + 0.1F * static_cast<float>(y);
                const float fy = static_cast<float>(y);
                const float fz = 0.15F * static_cast<float>(x * y);
                push_v(m, fx, fy, fz, 0, 0, 1, fx * 0.5F, fy * 0.45F);
            }
        }
        crd::u32 tris[8][3];
        int      t = 0;
        for (int y = 0; y < 2; ++y)
        {
            for (int x = 0; x < 2; ++x)
            {
                const crd::u32 a = static_cast<crd::u32>(y * 3 + x);
                const crd::u32 b = a + 1U;
                const crd::u32 c = a + 3U;
                const crd::u32 d = a + 4U;
                tris[t][0] = a; tris[t][1] = b; tris[t][2] = d; ++t;
                tris[t][0] = a; tris[t][1] = d; tris[t][2] = c; ++t;
            }
        }
        const int order_fwd[8]  = {0, 1, 2, 3, 4, 5, 6, 7};
        const int order_perm[8] = {5, 2, 7, 0, 6, 3, 1, 4};
        const int* order        = permute ? order_perm : order_fwd;
        for (int i = 0; i < 8; ++i)
        {
            for (int c = 0; c < 3; ++c) { m.indices.push_back(tris[order[i]][c]); }
        }
    };

    aio::ImportedMesh a(&alloc);
    aio::ImportedMesh b(&alloc);
    build_grid(a, false);
    build_grid(b, true);
    aio::generate_normals(a, &alloc, 0.6F);
    aio::generate_normals(b, &alloc, 0.6F);
    REQUIRE(aio::generate_tangents(a, &alloc));
    REQUIRE(aio::generate_tangents(b, &alloc));

    // identical vertex COUNTS and, for every position, BIT-identical normal + tangent (vertex order may differ; match by
    // position bits — unique in this grid)
    REQUIRE(a.positions.size() == b.positions.size());
    for (crd::usize va = 0; va < a.positions.size(); ++va)
    {
        bool matched = false;
        for (crd::usize vb = 0; vb < b.positions.size(); ++vb)
        {
            if (std::memcmp(&a.positions[va], &b.positions[vb], 12) != 0) { continue; }
            matched = true;
            CHECK(std::memcmp(&a.normals[va], &b.normals[vb], 12) == 0);   // BIT-identical
            CHECK(std::memcmp(&a.tangent[va], &b.tangent[vb], 16) == 0);   // BIT-identical
            break;
        }
        CHECK(matched);
    }
}

TEST_CASE("assetio: OUR tangents vs the REFERENCE mikktspace.c ORACLE", "[assetio][condition][oracle]")
{
    crd::memory::TlsfAllocator alloc(16U << 20U);
    // a nontrivial patch: a 3x3 grid with irregular positions/UVs PLUS a mirrored strip appended on the right — covers
    // smooth accumulation, varied UV gradients, and the mirror handedness path in one mesh
    aio::ImportedMesh m(&alloc);
    for (int y = 0; y < 3; ++y)
    {
        for (int x = 0; x < 3; ++x)
        {
            const float fx = static_cast<float>(x);
            const float fy = static_cast<float>(y) + 0.05F * static_cast<float>(x);
            const float fz = 0.1F * static_cast<float>(x + y);
            push_v(m, fx, fy, fz, 0, 0, 1, fx * 0.4F + 0.02F * fy, fy * 0.5F);
        }
    }
    for (int y = 0; y < 2; ++y)
    {
        for (int x = 0; x < 2; ++x)
        {
            const crd::u32 a = static_cast<crd::u32>(y * 3 + x);
            const crd::u32 b = a + 1U;
            const crd::u32 c = a + 3U;
            const crd::u32 d = a + 4U;
            const crd::u32 idx[6] = {a, b, d, a, d, c};
            for (crd::u32 i : idx) { m.indices.push_back(i); }
        }
    }
    // the mirrored strip: two extra columns at x=3..4 whose u DECREASES with x (opposite handedness), NOT edge-sharing
    // vertices with the grid (its own chart)
    const crd::u32 base = static_cast<crd::u32>(m.positions.size());
    for (int y = 0; y < 2; ++y)
    {
        for (int x = 0; x < 2; ++x)
        {
            const float fx = 3.0F + static_cast<float>(x);
            const float fy = static_cast<float>(y);
            push_v(m, fx, fy, 0.0F, 0, 0, 1, 1.0F - 0.3F * static_cast<float>(x), fy * 0.5F);
        }
    }
    {
        const crd::u32 idx[6] = {base + 0U, base + 1U, base + 3U, base + 0U, base + 3U, base + 2U};
        for (crd::u32 i : idx) { m.indices.push_back(i); }
    }

    // normalize the shading normals through OUR pipeline first (both sides must see the same normals)
    aio::generate_normals(m, &alloc, 0.9F);

    // the reference, face-varying
    crd::containers::Array<float> ref(&alloc);
    ref.resize(m.triangle_count() * 3U * 4U, 0.0F);
    REQUIRE(run_mikkt_oracle(m, ref.data()));

    // ours, per (split) vertex
    REQUIRE(aio::generate_tangents(m, &alloc));

    // per corner: direction agreement (dot > 0.999) + EXACT sign agreement. (Semantics note: mikktspace groups by
    // edge-connected same-orientation faces; ours by shared-vertex same-orientation — identical on these charts; exotic
    // vertex-only-touching chart pairs could differ, which the import path never produces post-weld.)
    crd::u32 corners_checked = 0;
    for (crd::usize c = 0; c < m.indices.size(); ++c)
    {
        const crd::u32 v  = m.indices[c];
        const float*   rt = ref.data() + c * 4U;
        const float    d  = rt[0] * m.tangent[v].x + rt[1] * m.tangent[v].y + rt[2] * m.tangent[v].z;
        CHECK(d > 0.999F);
        CHECK(m.tangent[v].w == rt[3]);
        ++corners_checked;
    }
    CHECK(corners_checked == m.indices.size());
}

// ── REN-3.2-b: NORMALS MUST POINT OUTWARD, whatever the source winding. ──────────────────────────────────────
// ⛔ `generate_normals` takes each face normal as cross(b-a, c-a) — a COUNTER-CLOCKWISE assumption. A
// clockwise-wound source produced inward normals on every face, which makes dot(N, L) <= 0 for real lights, so
// the mesh renders at flat ambient and looks permanently shadowed. The sandbox's OBJ torus did exactly that,
// and NO existing test caught it: the suite covered smoothing and crease behaviour but never asserted
// ORIENTATION. The check has to use a metric independent of the winding assumption or it just restates the
// bug — signed volume (divergence theorem) never consults the generated normals.
TEST_CASE("assetio: generated normals point OUTWARD for BOTH source windings", "[assetio][condition][normals]")
{
    crd::memory::TlsfAllocator alloc(8U << 20U);
    // a closed tetrahedron, CCW (outward) — 4 verts, 4 faces
    const auto make_tet = [&](bool clockwise) {
        aio::ImportedMesh m(&alloc);
        m.positions.push_back({0.0F, 0.0F, 0.0F});
        m.positions.push_back({1.0F, 0.0F, 0.0F});
        m.positions.push_back({0.0F, 1.0F, 0.0F});
        m.positions.push_back({0.0F, 0.0F, 1.0F});
        const crd::u32 ccw[12] = {0, 2, 1, 0, 1, 3, 0, 3, 2, 1, 2, 3};
        for (crd::u32 f = 0; f < 4U; ++f)
        {
            const crd::u32 a = ccw[f * 3U + 0U];
            const crd::u32 b = ccw[f * 3U + 1U];
            const crd::u32 c = ccw[f * 3U + 2U];
            m.indices.push_back(a);
            m.indices.push_back(clockwise ? c : b); // swapping two corners REVERSES the winding
            m.indices.push_back(clockwise ? b : c);
        }
        return m;
    };

    for (int cw = 0; cw < 2; ++cw)
    {
        aio::ImportedMesh m = make_tet(cw != 0);
        aio::generate_normals(m, &alloc, 1.0F);
        REQUIRE(m.has_normals());
        REQUIRE(m.triangle_count() == 4U);

        // centroid of the tetrahedron; every face normal must point AWAY from it
        float cx = 0.0F;
        float cy = 0.0F;
        float cz = 0.0F;
        for (const auto& p : m.positions)
        {
            cx += p.x;
            cy += p.y;
            cz += p.z;
        }
        const auto n = static_cast<float>(m.positions.size());
        cx /= n;
        cy /= n;
        cz /= n;

        for (crd::u32 i = 0; i < static_cast<crd::u32>(m.indices.size()); ++i)
        {
            const crd::u32 vi = m.indices[i];
            const auto&    p  = m.positions[vi];
            const auto&    nr = m.normals[vi];
            const float d = nr.x * (p.x - cx) + nr.y * (p.y - cy) + nr.z * (p.z - cz);
            // ⛔ outward: the normal at a vertex points away from the interior. An INWARD normal makes this
            // NEGATIVE, which is precisely the state that renders as permanent shadow.
            CHECK(d > 0.0F);
        }
    }
}

// ── REN-38: the TORUS class — orientation on a CLOSED, NON-CONVEX, genus-1 surface. ─────────────────────────
// The tetrahedron gate above proves the flip logic, but the artifact that shipped broken was a TORUS: the
// centroid metric used up there is meaningless on it (a torus is not star-shaped — half its outward normals
// point TOWARD the centroid), and `mesh_is_closed` has real work to do on a welded 576-face grid with wrap
// seams, where it found only 12 distinct indices on the tetrahedron. The independent metric here is the RING
// distance: for a torus of ring radius R about the z-axis, the outward direction at p is p minus its nearest
// point on the ring circle — pure geometry, never consulting the winding or the generated normals.
// (`assets/source/torus.obj` is wound CW — signed 6V = -18.35 — so the CW half of this gate IS that asset.)
TEST_CASE("assetio: generated normals point OUTWARD on a genus-1 torus, BOTH windings", "[assetio][condition][normals]")
{
    crd::memory::TlsfAllocator alloc(8U << 20U);
    constexpr float            ring = 1.0F;  // ring radius R
    constexpr float            tube = 0.4F;  // tube radius r
    constexpr crd::u32         nu   = 24U;   // segments around the ring
    constexpr crd::u32         nv   = 12U;   // segments around the tube
    constexpr float            two_pi = 6.28318530717958F;

    const auto make_torus = [&](bool clockwise) {
        aio::ImportedMesh m(&alloc);
        for (crd::u32 iu = 0; iu < nu; ++iu)
        {
            const float u  = two_pi * static_cast<float>(iu) / static_cast<float>(nu);
            const float cu = std::cos(u);
            const float su = std::sin(u);
            for (crd::u32 iv = 0; iv < nv; ++iv)
            {
                const float v  = two_pi * static_cast<float>(iv) / static_cast<float>(nv);
                const float cv = std::cos(v);
                const float sv = std::sin(v);
                m.positions.push_back({(ring + tube * cv) * cu, (ring + tube * cv) * su, tube * sv});
            }
        }
        for (crd::u32 iu = 0; iu < nu; ++iu)
        {
            for (crd::u32 iv = 0; iv < nv; ++iv)
            {
                const crd::u32 a = iu * nv + iv;
                const crd::u32 b = ((iu + 1U) % nu) * nv + iv;
                const crd::u32 c = ((iu + 1U) % nu) * nv + ((iv + 1U) % nv);
                const crd::u32 d = iu * nv + ((iv + 1U) % nv);
                // CCW for the outward parameterization above; `clockwise` swaps two corners of each triangle
                const crd::u32 t[6] = {a, b, c, a, c, d};
                for (crd::u32 k = 0; k < 2U; ++k)
                {
                    m.indices.push_back(t[k * 3U + 0U]);
                    m.indices.push_back(clockwise ? t[k * 3U + 2U] : t[k * 3U + 1U]);
                    m.indices.push_back(clockwise ? t[k * 3U + 1U] : t[k * 3U + 2U]);
                }
            }
        }
        return m;
    };

    for (int cw = 0; cw < 2; ++cw)
    {
        aio::ImportedMesh m = make_torus(cw != 0);
        aio::generate_normals(m, &alloc, 1.0F);
        REQUIRE(m.has_normals());
        REQUIRE(m.triangle_count() == nu * nv * 2U);

        crd::u32 outward_ok = 0U;
        crd::u32 total      = 0U;
        for (crd::u32 vi = 0; vi < static_cast<crd::u32>(m.positions.size()); ++vi)
        {
            const auto& p  = m.positions[vi];
            const auto& nr = m.normals[vi];
            // nearest ring point: R * normalize(p.xy, 0); outward = p - that
            const float len_xy = std::sqrt(p.x * p.x + p.y * p.y);
            REQUIRE(len_xy > 0.1F); // the parameterization never touches the axis
            const float qx = ring * p.x / len_xy;
            const float qy = ring * p.y / len_xy;
            const float ox = p.x - qx;
            const float oy = p.y - qy;
            const float oz = p.z;
            const float d  = nr.x * ox + nr.y * oy + nr.z * oz;
            ++total;
            if (d > 0.0F) { ++outward_ok; }
        }
        INFO((cw != 0 ? "CW" : "CCW") << " torus: " << outward_ok << "/" << total << " normals outward");
        // ⛔ ALL of them, not most: one inward vertex is one permanently-black patch in the frame
        CHECK(outward_ok == total);
    }
}

// ── REN-39: the IN-PLACE smooth generator — the ONLY normal generator legal for skinned meshes ─────────────────────────
// The scar this pins: the Khronos Fox ships POSITION/UV/JOINTS/WEIGHTS and NO normals, and the "skinned meshes are
// fully authored" rule skipped the whole conditioning chain — zero normals cooked through, the forward BRDF computed
// normalize(0) = NaN, and every fox rendered PURE BLACK (the old fixed-function ambient floor is what used to mask it).
// The fix must not weld, split or reorder (that desyncs the per-vertex joint mapping), so the contract under test is:
//   1. topology + every position-parallel attribute are BIT-UNTOUCHED (positions, uv, joints, weights, counts)
//   2. every produced normal is unit length
//   3. seam-DUPLICATED vertices (same position, different uv — the fox's texture seams) get the IDENTICAL normal
//   4. the result is analytic where symmetry decides it (octahedron => radial normals)
//   5. bit-identical under face reordering (the conditioning chain's determinism DNA)
TEST_CASE("assetio: generate_normals_smooth_inplace -- skinned-safe, seam-shared, analytic, deterministic",
          "[assetio][condition][normals][skin]")
{
    crd::memory::TlsfAllocator alloc(8U << 20U);

    // a unit octahedron (closed, radial symmetry) with vertex 6 a SEAM DUPLICATE of vertex 0 (+X, different uv)
    const auto build = [&](bool permuted) {
        aio::ImportedMesh m(&alloc);
        const V3 verts[7] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}, {1, 0, 0}};
        for (crd::u32 v = 0; v < 7U; ++v)
        {
            m.positions.push_back(verts[v]);
            m.uv0.push_back(V2{static_cast<float>(v) * 0.125F, v == 6U ? 1.0F : 0.0F});
            for (crd::u32 k = 0; k < 4U; ++k)
            {
                m.joints0.push_back(static_cast<crd::u16>(v + k));
                m.weights0.push_back(0.4F - 0.1F * static_cast<float>(k));
            }
        }
        // outward CCW; the two −Y-side faces reference the duplicate (6) instead of 0
        const crd::u32 faces[8][3] = {{0, 2, 4}, {2, 1, 4}, {1, 3, 4}, {3, 6, 4},
                                      {2, 0, 5}, {1, 2, 5}, {3, 1, 5}, {6, 3, 5}};
        for (crd::u32 f = 0; f < 8U; ++f)
        {
            const crd::u32 fi = permuted ? 7U - f : f; // reversed face order — same mesh
            for (crd::u32 k = 0; k < 3U; ++k) { m.indices.push_back(faces[fi][k]); }
        }
        return m;
    };

    aio::ImportedMesh m = build(false);
    REQUIRE(m.has_skin());
    REQUIRE(!m.has_normals());

    // snapshot every array the contract says must not move
    crd::containers::Array<V3>       pos_before(&alloc);
    crd::containers::Array<V2>       uv_before(&alloc);
    crd::containers::Array<crd::u16> joints_before(&alloc);
    crd::containers::Array<crd::f32> weights_before(&alloc);
    for (const V3& p : m.positions) { pos_before.push_back(p); }
    for (const V2& t : m.uv0) { uv_before.push_back(t); }
    for (crd::u16 j : m.joints0) { joints_before.push_back(j); }
    for (crd::f32 w : m.weights0) { weights_before.push_back(w); }

    aio::generate_normals_smooth_inplace(m, &alloc);

    // 1. topology + parallel attributes untouched
    REQUIRE(m.positions.size() == 7U);
    REQUIRE(m.indices.size() == 24U);
    REQUIRE(m.has_skin());
    for (crd::u32 v = 0; v < 7U; ++v)
    {
        CHECK(std::memcmp(&m.positions[v], &pos_before[v], sizeof(V3)) == 0);
        CHECK(std::memcmp(&m.uv0[v], &uv_before[v], sizeof(V2)) == 0);
    }
    for (crd::u32 i = 0; i < 28U; ++i)
    {
        CHECK(m.joints0[i] == joints_before[i]);
        CHECK(m.weights0[i] == weights_before[i]);
    }

    // 2 + 4. unit, and radial by symmetry (the octahedron's angle-weighted smooth normal IS the vertex direction)
    REQUIRE(m.has_normals());
    for (crd::u32 v = 0; v < 7U; ++v)
    {
        const V3&   n   = m.normals[v];
        const float len = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
        CHECK(std::abs(len - 1.0F) < 1.0e-5F);
        const V3&   p = m.positions[v];
        const float d = n.x * p.x + n.y * p.y + n.z * p.z; // |p| == 1 for every octahedron vertex
        INFO("vertex " << v);
        CHECK(d > 0.999F);
    }

    // 3. the seam duplicate shades identically to its position twin — BITWISE
    CHECK(std::memcmp(m.normals.data(), &m.normals[6], sizeof(V3)) == 0);

    // 5. bit-identical under face reordering
    aio::ImportedMesh mp = build(true);
    aio::generate_normals_smooth_inplace(mp, &alloc);
    REQUIRE(mp.normals.size() == 7U);
    for (crd::u32 v = 0; v < 7U; ++v)
    {
        CHECK(std::memcmp(&m.normals[v], &mp.normals[v], sizeof(V3)) == 0);
    }
}
