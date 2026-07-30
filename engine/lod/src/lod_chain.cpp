// lod_chain.cpp — REN-40-C1. See lod_chain.hpp for the design and the numbers.

#include <crd/lod/lod_chain.hpp>

#include <crd/containers/array.hpp>
#include <crd/geometry/mesh_processing/half_edge_mesh.hpp>
#include <crd/geometry/mesh_processing/qem_decimate.hpp>
#include <crd/math/select.hpp> // crd::math::sqrt (IEEE-exact, deterministic)
#include <crd/math/vec.hpp>

#include <cstring>

namespace crd::lod
{
namespace
{
namespace mp = crd::geometry::mesh_processing;
using V3     = crd::math::Vec3<crd::f32>;
using crd::resources::kMeshVertexStride;

// The cooked 48-byte record: pos3 | normal3 | uv2 | tangent4.
constexpr crd::u32 kOffPos     = 0U;
constexpr crd::u32 kOffNormal  = 12U;
constexpr crd::u32 kOffUv      = 24U;
constexpr crd::u32 kOffTangent = 32U;

[[nodiscard]] V3 read_v3(const crd::u8* rec, crd::u32 off) noexcept
{
    V3 v{};
    std::memcpy(static_cast<void*>(&v), static_cast<const void*>(rec + off), sizeof(crd::f32) * 3U);
    return v;
}

void write_v3(crd::u8* rec, crd::u32 off, const V3& v) noexcept
{
    std::memcpy(static_cast<void*>(rec + off), static_cast<const void*>(&v), sizeof(crd::f32) * 3U);
}

// ⛔ RE-DERIVED, never carried: an interpolated normal of a simplified surface is
// the normal of a surface that no longer exists, and lighting the coarse mesh with
// it reads as a shading POP at exactly the level change. Area-weighted because an
// un-weighted average lets a sliver triangle outvote the face that actually
// describes the surface there.
// The sign that makes a DERIVED normal agree with the mesh's own authored ones.
//
// ⛔⛔ THE WINDING CONVENTION IS NOT AN ASSUMPTION TO MAKE, IT IS DATA TO READ.
// `cross(b - a, c - a)` points outward only for counter-clockwise winding, and a
// mesh whose triangles are wound the other way relative to its authored normals
// makes every derived level face INWARD - the coarse levels then light as though
// lit from behind, with the flip appearing exactly at the level change. The gate
// caught this at `1 - n.p = 1.99992` on a sphere, i.e. n = -p, dead inverted.
// So the sign is measured against the SOURCE mesh's own normals, once, and the
// builder is correct for either convention rather than for one of them.
[[nodiscard]] crd::f32 winding_sign(const crd::containers::Array<V3>& pos, const crd::containers::Array<V3>& nrm,
                                    const crd::containers::Array<crd::u32>& idx) noexcept
{
    crd::f32 agree = 0.0F;
    for (crd::usize t = 0; t + 2U < idx.size(); t += 3U)
    {
        const crd::u32 a = idx[t];
        const crd::u32 b = idx[t + 1U];
        const crd::u32 c = idx[t + 2U];
        if (a >= pos.size() || b >= pos.size() || c >= pos.size()) { continue; }
        const V3 e1{pos[b].x - pos[a].x, pos[b].y - pos[a].y, pos[b].z - pos[a].z};
        const V3 e2{pos[c].x - pos[a].x, pos[c].y - pos[a].y, pos[c].z - pos[a].z};
        const V3 fn = crd::math::cross(e1, e2); // area-weighted by construction
        // against all three corners, so one bad authored normal cannot decide it
        for (const crd::u32 v : {a, b, c})
        {
            agree += (fn.x * nrm[v].x) + (fn.y * nrm[v].y) + (fn.z * nrm[v].z);
        }
    }
    // a mesh with no authored normals at all (agree == 0) keeps the CCW default
    return (agree < 0.0F) ? -1.0F : 1.0F;
}

// Group vertices that occupy the SAME SURFACE POINT, whatever the index buffer
// says.
//
// ⛔⛔ A UV SEAM IS ONE POINT ON THE SURFACE AND TWO VERTICES IN THE BUFFER. It has
// to be: a vertex carries one UV, and the two sides of a seam need different ones.
// Accumulating face normals per BUFFER vertex therefore gives every seam vertex
// only HALF its fan, and its normal tilts toward whichever side it happens to sit
// on - a bright/dark line straight down the seam of every simplified level, and a
// visible SHADING SEAM appearing exactly when the level changes. Measured on a UV
// sphere: mean n.p fell to 0.81 at 50% and 0.45 at 10%, with fully inverted
// corners at the poles, purely from one-sided fans.
//
// Welding is on a QUANTISED grid rather than exact bits because a decimated level's
// seam vertices are moved by their own collapses and drift apart by an epsilon.
// Integer keys in ascending vertex order keep the grouping DETERMINISTIC, which the
// chain's byte-identical contract requires.
void build_weld_groups(const crd::containers::Array<V3>& pos, crd::containers::Array<crd::u32>& group_of)
{
    group_of.clear();
    group_of.resize(pos.size(), 0U);
    if (pos.empty()) { return; }
    V3 lo = pos[0];
    V3 hi = pos[0];
    for (crd::usize v = 1; v < pos.size(); ++v)
    {
        lo.x = pos[v].x < lo.x ? pos[v].x : lo.x;
        lo.y = pos[v].y < lo.y ? pos[v].y : lo.y;
        lo.z = pos[v].z < lo.z ? pos[v].z : lo.z;
        hi.x = pos[v].x > hi.x ? pos[v].x : hi.x;
        hi.y = pos[v].y > hi.y ? pos[v].y : hi.y;
        hi.z = pos[v].z > hi.z ? pos[v].z : hi.z;
    }
    const crd::f32 ex    = hi.x - lo.x;
    const crd::f32 ey    = hi.y - lo.y;
    const crd::f32 ez    = hi.z - lo.z;
    crd::f32       ext   = ex > ey ? ex : ey;
    ext                  = ext > ez ? ext : ez;
    const crd::f32 cell  = (ext > 0.0F) ? (ext * 1.0e-4F) : 1.0e-4F;
    const crd::f32 inv_c = 1.0F / cell;

    // open-addressed integer hash; sized to a power of two above the vertex count
    crd::usize cap = 16U;
    while (cap < (pos.size() * 2U)) { cap <<= 1U; }
    crd::containers::Array<crd::u32> slot(group_of.allocator());
    slot.resize(cap, 0xFFFFFFFFU);
    const auto key_of = [&](const V3& p, crd::i32 (&k)[3]) {
        k[0] = static_cast<crd::i32>(p.x * inv_c + (p.x < 0.0F ? -0.5F : 0.5F));
        k[1] = static_cast<crd::i32>(p.y * inv_c + (p.y < 0.0F ? -0.5F : 0.5F));
        k[2] = static_cast<crd::i32>(p.z * inv_c + (p.z < 0.0F ? -0.5F : 0.5F));
    };
    for (crd::u32 v = 0; v < pos.size(); ++v)
    {
        crd::i32 k[3]{};
        key_of(pos[v], k);
        crd::u32 h = 2166136261U; // FNV-1a over the three integer coordinates
        for (const crd::i32 c : k)
        {
            const auto uc = static_cast<crd::u32>(c);
            for (crd::u32 b = 0; b < 4U; ++b)
            {
                h ^= (uc >> (b * 8U)) & 0xFFU;
                h *= 16777619U;
            }
        }
        crd::usize i = static_cast<crd::usize>(h) & (cap - 1U);
        crd::u32   found = 0xFFFFFFFFU;
        while (slot[i] != 0xFFFFFFFFU)
        {
            crd::i32 ok[3]{};
            key_of(pos[slot[i]], ok);
            if (ok[0] == k[0] && ok[1] == k[1] && ok[2] == k[2])
            {
                found = slot[i];
                break;
            }
            i = (i + 1U) & (cap - 1U);
        }
        if (found == 0xFFFFFFFFU)
        {
            slot[i]     = v;
            group_of[v] = v; // its own representative
        }
        else
        {
            group_of[v] = found;
        }
    }
}

void derive_normals(const crd::containers::Array<V3>& pos, const crd::containers::Array<crd::u32>& idx,
                    crd::f32 sign, crd::containers::Array<V3>& out)
{
    crd::containers::Array<crd::u32> group_of(out.allocator());
    build_weld_groups(pos, group_of);
    out.clear();
    out.resize(pos.size(), V3{0.0F, 0.0F, 0.0F});
    for (crd::usize t = 0; t + 2U < idx.size(); t += 3U)
    {
        const crd::u32 a = idx[t];
        const crd::u32 b = idx[t + 1U];
        const crd::u32 c = idx[t + 2U];
        if (a >= pos.size() || b >= pos.size() || c >= pos.size()) { continue; }
        const V3 e1{pos[b].x - pos[a].x, pos[b].y - pos[a].y, pos[b].z - pos[a].z};
        const V3 e2{pos[c].x - pos[a].x, pos[c].y - pos[a].y, pos[c].z - pos[a].z};
        // the un-normalised cross product IS twice the area times the unit normal,
        // so accumulating it directly gives the area weighting for free
        const V3 raw = crd::math::cross(e1, e2);
        const V3 n{raw.x * sign, raw.y * sign, raw.z * sign};
        for (const crd::u32 v : {a, b, c})
        {
            // into the GROUP's accumulator, so both sides of a seam see one fan
            const crd::u32 g = group_of[v];
            out[g].x += n.x;
            out[g].y += n.y;
            out[g].z += n.z;
        }
    }
    // normalise the representatives, then hand the result to every member
    for (crd::usize v = 0; v < out.size(); ++v)
    {
        if (group_of[v] != v) { continue; }
        const crd::f32 len = crd::math::length(out[v]);
        // a vertex with no surviving area keeps a unit +Y rather than a NaN: it
        // cannot be shaded meaningfully either way, and a NaN normal poisons every
        // pixel it touches
        out[v] = (len > 1.0e-20F) ? V3{out[v].x / len, out[v].y / len, out[v].z / len} : V3{0.0F, 1.0F, 0.0F};
    }
    for (crd::usize v = 0; v < out.size(); ++v)
    {
        if (group_of[v] != v) { out[v] = out[group_of[v]]; }
    }
}

// The standard per-triangle tangent from positions + UVs, accumulated per vertex,
// then Gram-Schmidt orthogonalised against the (already derived) normal. `w` is
// the handedness the bitangent needs — dropping it mirrors normal maps on every
// mirrored UV shell, which is a whole-surface artefact, not a subtle one.
void derive_tangents(const crd::containers::Array<V3>& pos, const crd::containers::Array<crd::f32>& uv,
                     const crd::containers::Array<crd::u32>& idx, const crd::containers::Array<V3>& nrm,
                     crd::containers::Array<V3>& tan_out, crd::containers::Array<crd::f32>& sign_out)
{
    crd::containers::Array<V3> bitan(tan_out.allocator());
    tan_out.clear();
    tan_out.resize(pos.size(), V3{0.0F, 0.0F, 0.0F});
    bitan.resize(pos.size(), V3{0.0F, 0.0F, 0.0F});
    sign_out.clear();
    sign_out.resize(pos.size(), 1.0F);

    for (crd::usize t = 0; t + 2U < idx.size(); t += 3U)
    {
        const crd::u32 a = idx[t];
        const crd::u32 b = idx[t + 1U];
        const crd::u32 c = idx[t + 2U];
        if (a >= pos.size() || b >= pos.size() || c >= pos.size()) { continue; }
        const V3       e1{pos[b].x - pos[a].x, pos[b].y - pos[a].y, pos[b].z - pos[a].z};
        const V3       e2{pos[c].x - pos[a].x, pos[c].y - pos[a].y, pos[c].z - pos[a].z};
        const crd::f32 du1 = uv[(b * 2U) + 0U] - uv[(a * 2U) + 0U];
        const crd::f32 dv1 = uv[(b * 2U) + 1U] - uv[(a * 2U) + 1U];
        const crd::f32 du2 = uv[(c * 2U) + 0U] - uv[(a * 2U) + 0U];
        const crd::f32 dv2 = uv[(c * 2U) + 1U] - uv[(a * 2U) + 1U];
        const crd::f32 det = (du1 * dv2) - (du2 * dv1);
        // a degenerate UV triangle contributes NOTHING rather than an infinity —
        // one such face would otherwise dominate the accumulation for its vertices
        if (crd::math::abs(det) < 1.0e-20F) { continue; }
        const crd::f32 r = 1.0F / det;
        const V3       tv{((e1.x * dv2) - (e2.x * dv1)) * r, ((e1.y * dv2) - (e2.y * dv1)) * r,
                    ((e1.z * dv2) - (e2.z * dv1)) * r};
        const V3       bv{((e2.x * du1) - (e1.x * du2)) * r, ((e2.y * du1) - (e1.y * du2)) * r,
                    ((e2.z * du1) - (e1.z * du2)) * r};
        for (const crd::u32 v : {a, b, c})
        {
            tan_out[v].x += tv.x;
            tan_out[v].y += tv.y;
            tan_out[v].z += tv.z;
            bitan[v].x += bv.x;
            bitan[v].y += bv.y;
            bitan[v].z += bv.z;
        }
    }

    for (crd::usize v = 0; v < tan_out.size(); ++v)
    {
        const V3&      n   = nrm[v];
        const V3&      t   = tan_out[v];
        const crd::f32 ndt = (n.x * t.x) + (n.y * t.y) + (n.z * t.z);
        V3             o{t.x - (n.x * ndt), t.y - (n.y * ndt), t.z - (n.z * ndt)};
        const crd::f32 len = crd::math::length(o);
        if (len > 1.0e-20F) { o = V3{o.x / len, o.y / len, o.z / len}; }
        else
        {
            // no usable tangent here: build ANY frame orthogonal to the normal
            // rather than emit a zero vector the shader would normalise into a NaN
            const V3 axis = (crd::math::abs(n.x) < 0.9F) ? V3{1.0F, 0.0F, 0.0F} : V3{0.0F, 1.0F, 0.0F};
            o             = crd::math::cross(n, axis);
            const crd::f32 l2 = crd::math::length(o);
            o                 = (l2 > 1.0e-20F) ? V3{o.x / l2, o.y / l2, o.z / l2} : V3{1.0F, 0.0F, 0.0F};
        }
        tan_out[v]        = o;
        const V3 cross_nt = crd::math::cross(n, o);
        const crd::f32 hd = (cross_nt.x * bitan[v].x) + (cross_nt.y * bitan[v].y) + (cross_nt.z * bitan[v].z);
        sign_out[v]       = (hd < 0.0F) ? -1.0F : 1.0F;
    }
}
} // namespace

LodBuildReport build_lod_chain(crd::resources::MeshResource& mesh, const LodPolicy& policy,
                               crd::memory::IAllocator* scratch)
{
    LodBuildReport report{};
    if (mesh.lods.size() > 0U)
    {
        report.status = LodBuildStatus::AlreadyBuilt;
        return report;
    }
    const crd::u32 src_vertices = static_cast<crd::u32>(mesh.vertices.size() / kMeshVertexStride);
    const crd::u32 src_indices  = static_cast<crd::u32>(mesh.indices.size() / 4U);
    if (src_vertices == 0U || src_indices == 0U)
    {
        report.status = LodBuildStatus::EmptyMesh;
        return report;
    }
    if ((src_indices % 3U) != 0U)
    {
        report.status = LodBuildStatus::NotTriangles;
        return report;
    }

    // ⭐ Level 0 IS the source range, recorded rather than rebuilt — so a chain
    // never changes what the near view draws, byte for byte.
    crd::resources::MeshLod lod0{};
    lod0.first_index   = 0U;
    lod0.index_count   = src_indices;
    lod0.error         = 0.0F;
    // ⛔⛔ THE FIELD IS "PICK THIS LEVEL WHILE px IS BELOW THIS" (MeshLod's own contract), so level 0's value is
    // "always" — it is the level that applies at any size. It used to hold `policy.screen_height[0]`, which is
    // the threshold for entering level ONE, and every level below did the same: the whole table was shifted by
    // one, so a selector asking "should I be at level s?" was handed level s+1's number. Measured, not argued:
    // a probe policy that pins the scene to level 1 rendered BIT-IDENTICALLY to level 0 (0 of 921600 pixels),
    // because the level-1 test was reading level 2's threshold. The producer and the consumer disagreed while
    // both looked reasonable in isolation — which is why the field carries its meaning in a comment right here.
    lod0.screen_height = 3.0e38F; // effectively +inf; nothing selects "not level 0"
    mesh.lods.push_back(lod0);
    report.levels_built  = 1U;
    report.triangles[0]  = src_indices / 3U;

    if (policy.extra_levels == 0U)
    {
        report.status = LodBuildStatus::NoLevelsRequested;
        return report;
    }

    // ── the source, in the decimator's terms ──
    crd::containers::Array<V3>       pos(scratch);
    crd::containers::Array<crd::f32> uv(scratch);
    crd::containers::Array<crd::u32> idx(scratch);
    pos.reserve(src_vertices);
    uv.reserve(static_cast<crd::usize>(src_vertices) * 2U);
    idx.resize(src_indices, 0U);
    for (crd::u32 v = 0; v < src_vertices; ++v)
    {
        const crd::u8* rec = mesh.vertices.data() + (static_cast<crd::usize>(v) * kMeshVertexStride);
        pos.push_back(read_v3(rec, kOffPos));
        crd::f32 t[2]{};
        std::memcpy(static_cast<void*>(t), static_cast<const void*>(rec + kOffUv), sizeof(t));
        uv.push_back(t[0]);
        uv.push_back(t[1]);
    }
    std::memcpy(static_cast<void*>(idx.data()), static_cast<const void*>(mesh.indices.data()),
                static_cast<crd::usize>(src_indices) * 4U);

    // Which way this mesh winds, measured rather than assumed - see `winding_sign`.
    crd::containers::Array<V3> src_nrm(scratch);
    src_nrm.reserve(src_vertices);
    for (crd::u32 v = 0; v < src_vertices; ++v)
    {
        src_nrm.push_back(
            read_v3(mesh.vertices.data() + (static_cast<crd::usize>(v) * kMeshVertexStride), kOffNormal));
    }
    const crd::f32 wsign = winding_sign(pos, src_nrm, idx);

    mp::HalfEdgeMesh<crd::f32> source(scratch);
    const auto                 bs = source.build_from(crd::containers::ConstSpan<V3>{pos.data(), pos.size()},
                                                      crd::containers::ConstSpan<crd::u32>{idx.data(), idx.size()});
    if (bs != mp::BuildStatus::Ok && bs != mp::BuildStatus::NonManifoldEdge)
    {
        report.status = LodBuildStatus::NonManifoldInput;
        return report;
    }
    if (!source.is_manifold())
    {
        // ⛔ REPORTED, not worked around. The decimator's own contract refuses a
        // non-manifold surface, and silently shipping a one-level chain here would
        // make the fps board say "LOD did nothing" with no explanation anywhere.
        report.status = LodBuildStatus::NonManifoldInput;
        return report;
    }
    const crd::u32 source_faces = source.face_count();

    const crd::u32 levels = policy.extra_levels < (kMaxLodLevels - 1U) ? policy.extra_levels : (kMaxLodLevels - 1U);
    // ⛔⛔ THE TRIANGLE FLOOR — the defect this stops, measured on screen. A ratio applied blindly took the
    // sandbox's 12-triangle CUBES to SIX, and a closed box cannot be six triangles: "level 1 of a cube" was a
    // degenerate sliver, so every cube VANISHED the moment it crossed the first threshold. The same blindness in
    // the other direction gave a 20-triangle icosahedron a 20 -> 20 -> 20 chain — three levels that reduce
    // nothing and cost a draw each. ⭐ The floor is AUTHORED (`min_triangles`), because what counts as "still a
    // surface" is a property of the content, not of the engine.
    const crd::u32 floor_tris = policy.min_triangles;
    if (source_faces <= floor_tris)
    {
        // Already at or below the floor: NO chain. ⛔ Reported as Ok with one level rather than as a failure —
        // "this mesh is too small to level" is a correct answer, and dressing it as an error would train the
        // reader to ignore the ones that are not.
        report.status = LodBuildStatus::Ok;
        return report;
    }
    // the SOURCE's extent, once — the reference the shape test below measures each level against
    V3 src_lo = pos.empty() ? V3{0.0F, 0.0F, 0.0F} : pos[0];
    V3 src_hi = src_lo;
    for (crd::usize v = 1; v < pos.size(); ++v)
    {
        src_lo.x = pos[v].x < src_lo.x ? pos[v].x : src_lo.x;
        src_lo.y = pos[v].y < src_lo.y ? pos[v].y : src_lo.y;
        src_lo.z = pos[v].z < src_lo.z ? pos[v].z : src_lo.z;
        src_hi.x = pos[v].x > src_hi.x ? pos[v].x : src_hi.x;
        src_hi.y = pos[v].y > src_hi.y ? pos[v].y : src_hi.y;
        src_hi.z = pos[v].z > src_hi.z ? pos[v].z : src_hi.z;
    }
    // the SOURCE's summed triangle AREA — the reference the shape test measures each level against.
    const auto tri_area = [](const crd::containers::Array<V3>& vp, const crd::containers::Array<crd::u32>& ix) {
        double sum = 0.0;
        for (crd::usize t = 0; t + 2U < ix.size(); t += 3U)
        {
            if (ix[t] >= vp.size() || ix[t + 1U] >= vp.size() || ix[t + 2U] >= vp.size()) { continue; }
            const V3& a = vp[ix[t]];
            const V3& b = vp[ix[t + 1U]];
            const V3& c = vp[ix[t + 2U]];
            const V3  e1{b.x - a.x, b.y - a.y, b.z - a.z};
            const V3  e2{c.x - a.x, c.y - a.y, c.z - a.z};
            const V3  n = crd::math::cross(e1, e2);
            sum += 0.5 * static_cast<double>(crd::math::sqrt(static_cast<double>((n.x * n.x) + (n.y * n.y) + (n.z * n.z))));
        }
        return sum;
    };
    const double src_area = tri_area(pos, idx);
    crd::u32 prev_faces = source_faces;
    for (crd::u32 l = 0; l < levels; ++l)
    {
        // ⛔ Ratios are of the SOURCE, so the chain does not compound rounding
        const crd::f32 ratio  = policy.ratio[l] > 0.0F ? policy.ratio[l] : 0.5F;
        auto           target = static_cast<crd::u32>(static_cast<crd::f32>(source_faces) * ratio);
        if (target < 4U) { break; } // below a tetrahedron there is nothing to keep
        if (target < floor_tris) { target = floor_tris; } // clamp INTO the floor rather than through it
        // ⛔ A level that does not actually REDUCE is not a level: it is a second copy of its predecessor with
        // its own draw call, its own indirect command and its own visible list. Stop the chain here instead.
        if (target >= prev_faces) { break; }

        mp::QemDecimateOptions<crd::f32> opts{};
        opts.target_face_count = target;
        opts.boundary_weight   = policy.boundary_weight;
        opts.output_allocator  = scratch;

        crd::containers::Array<crd::f32> uv_out(scratch);
        mp::QemDecimateReport            dec{};
        // ⛔ ALWAYS FROM THE SOURCE, never from the previous level. Decimating a
        // decimation compounds its own error and its own boundary drift, so level 3
        // would carry level 1's mistakes amplified twice.
        mp::HalfEdgeMesh<crd::f32> out =
            mp::qem_decimate_attr<crd::f32, 2U>(source, uv.data(), opts, &uv_out, &dec);
        if (dec.status == mp::QemDecimateStatus::TargetUnreachable) { ++report.levels_short_of_target; }

        crd::containers::Array<V3>       lpos(scratch);
        crd::containers::Array<crd::u32> lidx(scratch);
        out.to_indexed(lpos, lidx);
        if (lpos.empty() || lidx.empty()) { break; }
        // ⛔⛔ THE SHAPE TEST — a triangle count is not a quality bar, and this is the measurement that says so.
        // Measured on the LOD showcase: the 6,036-triangle source decimated to 104 triangles cleared
        // `min_triangles` comfortably and rendered as a flat SLIVER — every instance in the line collapsed. The
        // count was fine; the OBJECT was gone. A level that no longer occupies the source's space is not a
        // coarser version of it, so the chain STOPS here rather than publishing something the selector will
        // faithfully choose.
        {
            V3 lo = lpos[0];
            V3 hi = lpos[0];
            for (crd::usize v = 1; v < lpos.size(); ++v)
            {
                lo.x = lpos[v].x < lo.x ? lpos[v].x : lo.x;
                lo.y = lpos[v].y < lo.y ? lpos[v].y : lo.y;
                lo.z = lpos[v].z < lo.z ? lpos[v].z : lo.z;
                hi.x = lpos[v].x > hi.x ? lpos[v].x : hi.x;
                hi.y = lpos[v].y > hi.y ? lpos[v].y : hi.y;
                hi.z = lpos[v].z > hi.z ? lpos[v].z : hi.z;
            }
            const crd::f32 le[3] = {hi.x - lo.x, hi.y - lo.y, hi.z - lo.z};
            const crd::f32 se[3] = {src_hi.x - src_lo.x, src_hi.y - src_lo.y, src_hi.z - src_lo.z};
            bool           kept  = true;
            crd::f32       worst = 1.0F;
            for (crd::u32 ax = 0; ax < 3U; ++ax)
            {
                if (se[ax] > 1.0e-6F)
                {
                    const crd::f32 r_ax = le[ax] / se[ax];
                    if (r_ax < worst) { worst = r_ax; }
                }
                // ⛔ A degenerate SOURCE axis (a flat plane) is not a failure — only a level that lost extent
                // the source HAD is.
                if (se[ax] > 1.0e-6F && le[ax] < se[ax] * policy.min_extent_ratio) { kept = false; }
            }
            // ⛔⛔ AND THE AREA. The extent test alone is NOT sufficient — the 104-triangle level that rendered
            // as slivers PASSED it, because a few surviving vertices still sat at the source's extremes while
            // the shell between them was gone. Summed triangle area is what separates "a coarse version of
            // the object" from "a handful of slivers spanning the same box".
            const double lvl_area = tri_area(lpos, lidx);
            if (src_area > 1.0e-9 && lvl_area < src_area * static_cast<double>(policy.min_area_ratio))
            {
                kept = false;
            }
            if (!kept)
            {
                // ⛔ REPORTED WITH ITS NUMBERS, not merely counted — "the chain stopped early" and "the policy
                // asked for fewer levels" look identical on an fps board and have different fixes.
                ++report.levels_refused_shape;
                report.refused_extent_ratio = worst;
                report.refused_area_ratio =
                    src_area > 1.0e-9 ? static_cast<crd::f32>(lvl_area / src_area) : 0.0F;
                break;
            }
        }

        crd::containers::Array<V3>       lnrm(scratch);
        crd::containers::Array<V3>       ltan(scratch);
        crd::containers::Array<crd::f32> lsign(scratch);
        derive_normals(lpos, lidx, wsign, lnrm);
        derive_tangents(lpos, uv_out, lidx, lnrm, ltan, lsign);

        // ── append the level's vertex block, then its indices REBASED onto it ──
        const auto base_vertex = static_cast<crd::u32>(mesh.vertices.size() / kMeshVertexStride);
        const crd::usize old_vbytes = mesh.vertices.size();
        mesh.vertices.resize(old_vbytes + (lpos.size() * kMeshVertexStride), crd::u8{0});
        for (crd::usize v = 0; v < lpos.size(); ++v)
        {
            crd::u8* rec = mesh.vertices.data() + old_vbytes + (v * kMeshVertexStride);
            write_v3(rec, kOffPos, lpos[v]);
            write_v3(rec, kOffNormal, lnrm[v]);
            const crd::f32 t[2]{uv_out[(v * 2U) + 0U], uv_out[(v * 2U) + 1U]};
            std::memcpy(static_cast<void*>(rec + kOffUv), static_cast<const void*>(t), sizeof(t));
            const crd::f32 tan4[4]{ltan[v].x, ltan[v].y, ltan[v].z, lsign[v]};
            std::memcpy(static_cast<void*>(rec + kOffTangent), static_cast<const void*>(tan4), sizeof(tan4));
        }

        const auto       first_index = static_cast<crd::u32>(mesh.indices.size() / 4U);
        const crd::usize old_ibytes  = mesh.indices.size();
        mesh.indices.resize(old_ibytes + (lidx.size() * 4U), crd::u8{0});
        for (crd::usize k = 0; k < lidx.size(); ++k)
        {
            // ⛔ ABSOLUTE into the combined stream — see the header note on why
            // `base_vertex` must stay unrepresentable.
            const crd::u32 abs_index = base_vertex + lidx[k];
            std::memcpy(static_cast<void*>(mesh.indices.data() + old_ibytes + (k * 4U)),
                        static_cast<const void*>(&abs_index), 4U);
        }

        crd::resources::MeshLod entry{};
        entry.first_index   = first_index;
        entry.index_count   = static_cast<crd::u32>(lidx.size());
        entry.error         = opts.max_error_threshold; // unset here; the reported error rides the report
        // ⛔ THIS level's own threshold — `policy.screen_height[l]` is the l-th `[[level]]` entry, and the l-th
        // entry describes level l+1, which is exactly the level being appended here. (Was `[l + 1U]`: level 1
        // published level 2's number, and the deepest level published an UNSET zero, so the coarsest level could
        // never be selected at all.)
        entry.screen_height = policy.screen_height[l];
        entry.error         = 0.0F;
        mesh.lods.push_back(entry);
        report.triangles[report.levels_built] = static_cast<crd::u32>(lidx.size() / 3U);
        prev_faces                            = static_cast<crd::u32>(lidx.size() / 3U);
        ++report.levels_built;
    }

    report.status = LodBuildStatus::Ok;
    return report;
}

} // namespace crd::lod
