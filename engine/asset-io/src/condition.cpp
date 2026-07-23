// condition.cpp — GEO-2: weld · crease-angle normals · MikkTSpace-compatible tangents. See condition.hpp for the
// contracts. Determinism note: every per-vertex accumulation SORTS its contributions canonically (by the contribution's
// own bit pattern) before summing, so face-order permutations produce BIT-IDENTICAL results.

#include <crd/assetio/condition.hpp>

#include <crd/containers/hash.hpp>
#include <crd/containers/hash_map.hpp>
#include <crd/math/cmath.hpp>

#include <cstring>

namespace crd::assetio
{
namespace
{

using V2 = crd::math::Vec2<crd::f32>;
using V3 = crd::math::Vec3<crd::f32>;
using V4 = crd::math::Vec4<crd::f32>;

[[nodiscard]] V3 sub(const V3& a, const V3& b) noexcept { return V3{a.x - b.x, a.y - b.y, a.z - b.z}; }
[[nodiscard]] V3 cross(const V3& a, const V3& b) noexcept
{
    return V3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
[[nodiscard]] crd::f32 dot(const V3& a, const V3& b) noexcept { return a.x * b.x + a.y * b.y + a.z * b.z; }
[[nodiscard]] crd::f32 len(const V3& a) noexcept { return static_cast<crd::f32>(crd::math::sqrt(static_cast<crd::f64>(dot(a, a)))); }
[[nodiscard]] V3       scaled(const V3& a, crd::f32 s) noexcept { return V3{a.x * s, a.y * s, a.z * s}; }
[[nodiscard]] bool     normalize_into(V3& a) noexcept
{
    const crd::f32 l = len(a);
    if (l < 1.0e-20F) { return false; }
    a = scaled(a, 1.0F / l);
    return true;
}

// The wedge angle at corner `a` between edges a→b and a→c (the Thürmer-Wüthrich weight).
[[nodiscard]] crd::f32 wedge_angle(const V3& a, const V3& b, const V3& c) noexcept
{
    V3 e1 = sub(b, a);
    V3 e2 = sub(c, a);
    if (!normalize_into(e1) || !normalize_into(e2)) { return 0.0F; }
    crd::f32 d = dot(e1, e2);
    if (d > 1.0F) { d = 1.0F; }
    if (d < -1.0F) { d = -1.0F; }
    return static_cast<crd::f32>(crd::math::acos(static_cast<crd::f64>(d)));
}

// ── canonical-sort-then-sum (the determinism device) ───────────────────────────────────────────────────────────────────
// Contributions are ordered by their own bit patterns (any total order works — it only has to be face-order-free),
// insertion-sorted in place (valence-sized buckets), then summed in that canonical order.

[[nodiscard]] crd::u64 v3_bits_key(const V3& v) noexcept
{
    crd::u32 b[3];
    std::memcpy(b, &v, 12);
    return (static_cast<crd::u64>(b[0]) << 32U) ^ (static_cast<crd::u64>(b[1]) << 16U) ^ b[2];
}

void sort_span_canonical(V3* vals, crd::u32 n) noexcept
{
    for (crd::u32 i = 1U; i < n; ++i)
    {
        const V3       v = vals[i];
        const crd::u64 k = v3_bits_key(v);
        crd::u32       j = i;
        while (j > 0U && v3_bits_key(vals[j - 1U]) > k)
        {
            vals[j] = vals[j - 1U];
            --j;
        }
        vals[j] = v;
    }
}

[[nodiscard]] V3 sum_span(const V3* vals, crd::u32 n) noexcept
{
    V3 s{0.0F, 0.0F, 0.0F};
    for (crd::u32 i = 0U; i < n; ++i) { s = V3{s.x + vals[i].x, s.y + vals[i].y, s.z + vals[i].z}; }
    return s;
}

// ── the weld key: a bit-identical (position, normal, uv) corner tuple ──────────────────────────────────────────────────

struct CornerKey // the attribute BIT PATTERNS (u32) — exact-weld identity is bitwise by definition (−0≠+0, NaN payloads)
{
    crd::u32 p[3];
    crd::u32 n[3];
    crd::u32 t[2];

    [[nodiscard]] bool operator==(const CornerKey&) const noexcept = default;
};

struct CornerKeyHash
{
    [[nodiscard]] crd::u64 operator()(const CornerKey& k) const noexcept
    {
        return crd::containers::fnv1a_64(&k, sizeof(CornerKey));
    }
};

// ── position-identity CSR adjacency (position → the corners that touch it) ─────────────────────────────────────────────

struct PosKey // position bit pattern (u32) — same exact-identity semantics as CornerKey
{
    crd::u32 p[3];

    [[nodiscard]] bool operator==(const PosKey&) const noexcept = default;
};

struct PosKeyHash
{
    [[nodiscard]] crd::u64 operator()(const PosKey& k) const noexcept
    {
        return crd::containers::fnv1a_64(&k, sizeof(PosKey));
    }
};

} // namespace

// ── weld ────────────────────────────────────────────────────────────────────────────────────────────────────────────────

crd::u32 weld_exact(ImportedMesh& mesh, crd::memory::IAllocator* alloc)
{
    const crd::usize old_vc = mesh.positions.size();
    if (old_vc == 0U || mesh.indices.size() == 0U) { return 0U; }
    const bool has_n = mesh.has_normals();
    const bool has_t = mesh.has_uv0();

    crd::containers::HashMap<CornerKey, crd::u32, CornerKeyHash> map(alloc);
    map.reserve(old_vc);
    crd::containers::Array<V3>       new_pos(alloc);
    crd::containers::Array<V3>       new_nrm(alloc);
    crd::containers::Array<V2>       new_uv(alloc);
    crd::containers::Array<crd::u32> new_idx(alloc);
    new_idx.reserve(mesh.indices.size());

    for (crd::usize i = 0; i < mesh.indices.size(); ++i)
    {
        const crd::u32 v = mesh.indices[i];
        CornerKey      key{};
        std::memcpy(key.p, &mesh.positions[v], 12);
        if (has_n) { std::memcpy(key.n, &mesh.normals[v], 12); }
        if (has_t) { std::memcpy(key.t, &mesh.uv0[v], 8); }
        if (const crd::u32* found = map.find(key))
        {
            new_idx.push_back(*found);
            continue;
        }
        const crd::u32 nv = static_cast<crd::u32>(new_pos.size());
        new_pos.push_back(mesh.positions[v]);
        if (has_n) { new_nrm.push_back(mesh.normals[v]); }
        if (has_t) { new_uv.push_back(mesh.uv0[v]); }
        map.insert(key, nv);
        new_idx.push_back(nv);
    }

    const crd::u32 removed = static_cast<crd::u32>(old_vc - new_pos.size());
    mesh.positions          = static_cast<crd::containers::Array<V3>&&>(new_pos);
    mesh.normals            = static_cast<crd::containers::Array<V3>&&>(new_nrm);
    mesh.uv0                = static_cast<crd::containers::Array<V2>&&>(new_uv);
    mesh.indices            = static_cast<crd::containers::Array<crd::u32>&&>(new_idx);
    mesh.tangent.clear(); // derived — regenerate after any topology change
    return removed;
}

// ── crease-angle weighted normals ───────────────────────────────────────────────────────────────────────────────────────

void generate_normals(ImportedMesh& mesh, crd::memory::IAllocator* alloc, crd::f32 smooth_angle_rad)
{
    const crd::u32 nf = mesh.triangle_count();
    if (nf == 0U) { return; }
    const bool has_uv = mesh.has_uv0();

    // 1. face normals (zero for degenerate faces — they contribute nothing)
    crd::containers::Array<V3> face_n(alloc);
    face_n.reserve(nf);
    for (crd::u32 f = 0; f < nf; ++f)
    {
        const V3& a = mesh.positions[mesh.indices[f * 3U + 0U]];
        const V3& b = mesh.positions[mesh.indices[f * 3U + 1U]];
        const V3& c = mesh.positions[mesh.indices[f * 3U + 2U]];
        V3        n = cross(sub(b, a), sub(c, a));
        if (!normalize_into(n)) { n = V3{0.0F, 0.0F, 0.0F}; }
        face_n.push_back(n);
    }

    // 2. position identity + CSR corner adjacency (position-id → corners touching it)
    crd::containers::HashMap<PosKey, crd::u32, PosKeyHash> pos_ids(alloc);
    pos_ids.reserve(mesh.positions.size());
    crd::containers::Array<crd::u32> corner_pos(alloc); // corner index → position-id
    corner_pos.reserve(nf * 3U);
    crd::u32 n_pos = 0U;
    for (crd::u32 c = 0; c < nf * 3U; ++c)
    {
        PosKey key{};
        std::memcpy(key.p, &mesh.positions[mesh.indices[c]], 12);
        if (const crd::u32* found = pos_ids.find(key)) { corner_pos.push_back(*found); }
        else
        {
            pos_ids.insert(key, n_pos);
            corner_pos.push_back(n_pos);
            ++n_pos;
        }
    }
    crd::containers::Array<crd::u32> counts(alloc);
    counts.resize(n_pos, 0U);
    for (crd::u32 c = 0; c < nf * 3U; ++c) { ++counts[corner_pos[c]]; }
    crd::containers::Array<crd::u32> offsets(alloc);
    offsets.resize(n_pos + 1U, 0U);
    for (crd::u32 p = 0; p < n_pos; ++p) { offsets[p + 1U] = offsets[p] + counts[p]; }
    crd::containers::Array<crd::u32> bucket(alloc); // CSR payload: corner indices grouped by position-id
    bucket.resize(nf * 3U, 0U);
    {
        crd::containers::Array<crd::u32> cursor(alloc);
        cursor.resize(n_pos, 0U);
        for (crd::u32 c = 0; c < nf * 3U; ++c)
        {
            const crd::u32 p          = corner_pos[c];
            bucket[offsets[p] + cursor[p]] = c;
            ++cursor[p];
        }
    }

    // 3. per corner: angle-weighted sum of the face normals within the crease threshold of the corner's own face
    crd::f64 ct = crd::math::cos(static_cast<crd::f64>(smooth_angle_rad));
    ct -= 1.0e-6; // the corner's own face (dot exactly 1) must always pass, float-safely — 0 rad stays "flat+coplanar"
    crd::containers::Array<V3> corner_n(alloc);
    corner_n.reserve(nf * 3U);
    crd::containers::Array<V3> contrib(alloc); // scratch: this corner's contributions (canonically sorted before summing)
    for (crd::u32 c = 0; c < nf * 3U; ++c)
    {
        const crd::u32 f  = c / 3U;
        const V3&      nf_own = face_n[f];
        contrib.clear();
        const crd::u32 p     = corner_pos[c];
        const crd::u32 begin = offsets[p];
        const crd::u32 end   = offsets[p + 1U];
        for (crd::u32 k = begin; k < end; ++k)
        {
            const crd::u32 oc = bucket[k];
            const crd::u32 g  = oc / 3U;
            const V3&      ng = face_n[g];
            if (ng.x == 0.0F && ng.y == 0.0F && ng.z == 0.0F) { continue; } // degenerate face
            if (static_cast<crd::f64>(dot(ng, nf_own)) < ct) { continue; }  // across the crease — a hard edge
            const crd::u32 l = oc % 3U;                                     // the wedge corner of g touching this position
            const V3&      a = mesh.positions[mesh.indices[g * 3U + l]];
            const V3&      b = mesh.positions[mesh.indices[g * 3U + ((l + 1U) % 3U)]];
            const V3&      d = mesh.positions[mesh.indices[g * 3U + ((l + 2U) % 3U)]];
            const crd::f32 w = wedge_angle(a, b, d);
            if (w <= 0.0F) { continue; }
            contrib.push_back(scaled(ng, w));
        }
        sort_span_canonical(contrib.data(), static_cast<crd::u32>(contrib.size()));
        V3 sum = sum_span(contrib.data(), static_cast<crd::u32>(contrib.size()));
        if (!normalize_into(sum)) { sum = nf_own; } // all-degenerate neighborhood: fall back to the face normal
        corner_n.push_back(sum);
    }

    // 4. rebuild face-varying (position, computed normal, uv) corners, then weld back to indexed
    crd::containers::Array<V3>       out_pos(alloc);
    crd::containers::Array<V3>       out_nrm(alloc);
    crd::containers::Array<V2>       out_uv(alloc);
    crd::containers::Array<crd::u32> out_idx(alloc);
    out_pos.reserve(nf * 3U);
    out_nrm.reserve(nf * 3U);
    for (crd::u32 c = 0; c < nf * 3U; ++c)
    {
        out_pos.push_back(mesh.positions[mesh.indices[c]]);
        out_nrm.push_back(corner_n[c]);
        if (has_uv) { out_uv.push_back(mesh.uv0[mesh.indices[c]]); }
        out_idx.push_back(c);
    }
    mesh.positions = static_cast<crd::containers::Array<V3>&&>(out_pos);
    mesh.normals   = static_cast<crd::containers::Array<V3>&&>(out_nrm);
    mesh.uv0       = static_cast<crd::containers::Array<V2>&&>(out_uv);
    mesh.indices   = static_cast<crd::containers::Array<crd::u32>&&>(out_idx);
    mesh.tangent.clear();
    (void)weld_exact(mesh, alloc);
}

// ── MikkTSpace-compatible tangents ──────────────────────────────────────────────────────────────────────────────────────

bool generate_tangents(ImportedMesh& mesh, crd::memory::IAllocator* alloc)
{
    const crd::u32 nf = mesh.triangle_count();
    if (nf == 0U || !mesh.has_normals() || !mesh.has_uv0()) { return false; }

    // 1. per-face tangent DIRECTION + orientation sign from the UV gradient. dP/du = (Δv2·e1 − Δv1·e2)/det — the sign of
    //    det folds into the direction; |det| < eps ⇒ a degenerate UV mapping (no contribution, orientation-neutral).
    crd::containers::Array<V3>      face_t(alloc);
    crd::containers::Array<crd::i8> face_s(alloc); // +1 / -1 / 0 (neutral)
    face_t.reserve(nf);
    face_s.reserve(nf);
    for (crd::u32 f = 0; f < nf; ++f)
    {
        const crd::u32 i0 = mesh.indices[f * 3U + 0U];
        const crd::u32 i1 = mesh.indices[f * 3U + 1U];
        const crd::u32 i2 = mesh.indices[f * 3U + 2U];
        const V3       e1 = sub(mesh.positions[i1], mesh.positions[i0]);
        const V3       e2 = sub(mesh.positions[i2], mesh.positions[i0]);
        const crd::f32 du1 = mesh.uv0[i1].x - mesh.uv0[i0].x;
        const crd::f32 dv1 = mesh.uv0[i1].y - mesh.uv0[i0].y;
        const crd::f32 du2 = mesh.uv0[i2].x - mesh.uv0[i0].x;
        const crd::f32 dv2 = mesh.uv0[i2].y - mesh.uv0[i0].y;
        const crd::f32 det = du1 * dv2 - du2 * dv1;
        V3             t{0.0F, 0.0F, 0.0F};
        crd::i8        s = 0;
        if (det > 1.0e-12F || det < -1.0e-12F)
        {
            t = V3{dv2 * e1.x - dv1 * e2.x, dv2 * e1.y - dv1 * e2.y, dv2 * e1.z - dv1 * e2.z};
            if (det < 0.0F) { t = scaled(t, -1.0F); } // fold the det sign ⇒ t is the true dP/du direction
            s = det >= 0.0F ? crd::i8{1} : crd::i8{-1};
            if (!normalize_into(t))
            {
                t = V3{0.0F, 0.0F, 0.0F};
                s = 0;
            }
        }
        face_t.push_back(t);
        face_s.push_back(s);
    }

    // 2. the MIRROR-SEAM SPLIT: a vertex referenced by corners of BOTH orientations duplicates per handedness (the
    //    negative-orientation corners move to the duplicate). Neutral corners stay on the original (+) vertex.
    const crd::u32                   vc0 = static_cast<crd::u32>(mesh.positions.size());
    crd::containers::Array<crd::u32> neg_twin(alloc); // vertex → its negative-orientation duplicate (or self / unset)
    neg_twin.resize(vc0, 0xFFFFFFFFU);
    {
        crd::containers::Array<crd::u8> seen(alloc); // bit 0: a + corner touches, bit 1: a − corner touches
        seen.resize(vc0, 0U);
        for (crd::u32 c = 0; c < nf * 3U; ++c)
        {
            const crd::i8 s = face_s[c / 3U];
            if (s > 0) { seen[mesh.indices[c]] |= 1U; }
            else if (s < 0) { seen[mesh.indices[c]] |= 2U; }
        }
        for (crd::u32 v = 0; v < vc0; ++v)
        {
            if (seen[v] == 3U) // both orientations ⇒ split
            {
                neg_twin[v] = static_cast<crd::u32>(mesh.positions.size());
                mesh.positions.push_back(mesh.positions[v]);
                mesh.normals.push_back(mesh.normals[v]);
                mesh.uv0.push_back(mesh.uv0[v]);
            }
            else if (seen[v] == 2U) { neg_twin[v] = v; } // negative-only: keeps its vertex, carries sign −1
        }
        for (crd::u32 c = 0; c < nf * 3U; ++c)
        {
            const crd::u32 v = mesh.indices[c];
            if (face_s[c / 3U] < 0 && neg_twin[v] != 0xFFFFFFFFU && neg_twin[v] != v) { mesh.indices[c] = neg_twin[v]; }
        }
    }
    const crd::u32 vc = static_cast<crd::u32>(mesh.positions.size());

    // per-vertex orientation sign for the output w
    crd::containers::Array<crd::i8> vert_s(alloc);
    vert_s.resize(vc, 0);
    for (crd::u32 c = 0; c < nf * 3U; ++c)
    {
        const crd::i8 s = face_s[c / 3U];
        if (s != 0) { vert_s[mesh.indices[c]] = s; }
    }

    // 3. per-vertex angle-weighted accumulation (CSR over corners, canonical-sorted before summing)
    crd::containers::Array<crd::u32> counts(alloc);
    counts.resize(vc, 0U);
    for (crd::u32 c = 0; c < nf * 3U; ++c) { ++counts[mesh.indices[c]]; }
    crd::containers::Array<crd::u32> offsets(alloc);
    offsets.resize(vc + 1U, 0U);
    for (crd::u32 v = 0; v < vc; ++v) { offsets[v + 1U] = offsets[v] + counts[v]; }
    crd::containers::Array<V3> contrib(alloc);
    contrib.resize(nf * 3U, V3{0.0F, 0.0F, 0.0F});
    {
        crd::containers::Array<crd::u32> cursor(alloc);
        cursor.resize(vc, 0U);
        for (crd::u32 c = 0; c < nf * 3U; ++c)
        {
            const crd::u32 f = c / 3U;
            const crd::u32 v = mesh.indices[c];
            V3             w{0.0F, 0.0F, 0.0F};
            if (face_s[f] != 0)
            {
                const V3&      a  = mesh.positions[mesh.indices[f * 3U + (c % 3U)]];
                const V3&      b  = mesh.positions[mesh.indices[f * 3U + ((c % 3U + 1U) % 3U)]];
                const V3&      d  = mesh.positions[mesh.indices[f * 3U + ((c % 3U + 2U) % 3U)]];
                const crd::f32 wa = wedge_angle(a, b, d);
                w                 = scaled(face_t[f], wa);
            }
            contrib[offsets[v] + cursor[v]] = w;
            ++cursor[v];
        }
    }

    mesh.tangent.clear();
    mesh.tangent.reserve(vc);
    for (crd::u32 v = 0; v < vc; ++v)
    {
        sort_span_canonical(contrib.data() + offsets[v], offsets[v + 1U] - offsets[v]);
        V3        sum = sum_span(contrib.data() + offsets[v], offsets[v + 1U] - offsets[v]);
        const V3& n   = mesh.normals[v];
        // Gram-Schmidt against the shading normal
        V3 t = sub(sum, scaled(n, dot(n, sum)));
        if (!normalize_into(t))
        {
            // no valid UV contribution: an arbitrary frame perpendicular to n (deterministic fallback)
            const V3 axis = (n.x > 0.9F || n.x < -0.9F) ? V3{0.0F, 1.0F, 0.0F} : V3{1.0F, 0.0F, 0.0F};
            t             = sub(axis, scaled(n, dot(n, axis)));
            (void)normalize_into(t);
        }
        const crd::f32 w = vert_s[v] < 0 ? -1.0F : 1.0F;
        mesh.tangent.push_back(V4{t.x, t.y, t.z, w});
    }
    return true;
}

} // namespace crd::assetio
