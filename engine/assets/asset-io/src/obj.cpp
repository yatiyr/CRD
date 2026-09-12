// obj.cpp — GEO-1: the Wavefront OBJ (+MTL) parser. See obj.hpp for the dirty-file policy.

#include <crd/assetio/obj.hpp>

#include <crd/containers/hash.hpp>
#include <crd/containers/hash_map.hpp>

#include <cmath>
#include <cstdlib>
#include <cstring>

namespace crd::assetio
{
namespace
{

// ── line scanner ────────────────────────────────────────────────────────────────────────────────────────────────────────
// OBJ/MTL are line-oriented: fetch one line, then tokenize inside it. '#' comments run to end of line; '\r' is trimmed.

struct LineScan
{
    const crd::u8* p   = nullptr;
    const crd::u8* end = nullptr;

    // Next line as [ls, le) with comments stripped. False at end of input.
    bool next_line(const crd::u8*& ls, const crd::u8*& le) noexcept
    {
        if (p == end) { return false; }
        ls               = p;
        const crd::u8* q = p;
        while (q < end && *q != '\n') { ++q; }
        le = q;
        p  = (q < end) ? q + 1 : q;
        for (const crd::u8* c = ls; c < le; ++c)
        {
            if (*c == '#')
            {
                le = c;
                break;
            }
        }
        while (le > ls && (*(le - 1) == '\r' || *(le - 1) == ' ' || *(le - 1) == '\t')) { --le; }
        return true;
    }
};

struct LineTok
{
    const crd::u8* p;
    const crd::u8* end;

    bool next(char* buf, crd::usize cap) noexcept
    {
        while (p < end && (*p == ' ' || *p == '\t')) { ++p; }
        if (p == end) { return false; }
        crd::usize n = 0;
        while (p < end && *p != ' ' && *p != '\t')
        {
            if (n + 1 < cap) { buf[n++] = static_cast<char>(*p); }
            ++p;
        }
        buf[n] = '\0';
        return true;
    }

    // Rest of the line (object/material names may contain spaces).
    void rest(char* buf, crd::usize cap) noexcept
    {
        while (p < end && (*p == ' ' || *p == '\t')) { ++p; }
        crd::usize n = 0;
        while (p < end)
        {
            if (n + 1 < cap) { buf[n++] = static_cast<char>(*p); }
            ++p;
        }
        buf[n] = '\0';
    }
};

[[nodiscard]] bool parse_f32(const char* tok, crd::f32& out) noexcept
{
    char*        endp = nullptr;
    const double v    = std::strtod(tok, &endp);
    if (endp == tok || *endp != '\0') { return false; }
    out = static_cast<crd::f32>(v);
    return true;
}

[[nodiscard]] crd::f32 clamp01(crd::f32 v) noexcept
{
    if (v < 0.0F) { return 0.0F; }
    if (v > 1.0F) { return 1.0F; }
    return v;
}

// ── OBJ corner key (v,vt,vn triple → single output vertex) ─────────────────────────────────────────────────────────────

struct ObjCorner
{
    crd::i32 v  = 0; // 1-based resolved absolute indices; 0 = absent
    crd::i32 vt = 0;
    crd::i32 vn = 0;

    [[nodiscard]] bool operator==(const ObjCorner&) const noexcept = default;
};

struct ObjCornerHash
{
    [[nodiscard]] crd::u64 operator()(const ObjCorner& c) const noexcept
    {
        return crd::containers::fnv1a_64(&c, sizeof(ObjCorner));
    }
};

// Parse one face-corner token "v", "v/vt", "v//vn", "v/vt/vn" with negative-index resolution. `counts` are the current
// sizes of the source attribute arrays. Absent components stay 0; out-of-range/zero → false (Malformed).
[[nodiscard]] bool parse_corner(const char* tok, const crd::i32 counts[3], ObjCorner& out) noexcept
{
    crd::i32   comp[3] = {0, 0, 0};
    int        ci      = 0;
    const char* s      = tok;
    while (*s != '\0' && ci < 3)
    {
        if (*s == '/')
        {
            ++ci;
            ++s;
            continue;
        }
        char*          endp = nullptr;
        const long     v    = std::strtol(s, &endp, 10);
        if (endp == s) { return false; }
        comp[ci] = static_cast<crd::i32>(v);
        s        = endp;
    }
    if (*s != '\0') { return false; }
    for (int i = 0; i < 3; ++i)
    {
        if (comp[i] == 0) { continue; } // absent
        const crd::i32 resolved = comp[i] > 0 ? comp[i] : counts[i] + comp[i] + 1; // negative = relative to current count
        if (resolved < 1 || resolved > counts[i]) { return false; }
        comp[i] = resolved;
    }
    out = ObjCorner{comp[0], comp[1], comp[2]};
    return out.v != 0; // a corner MUST reference a position
}

// ── the per-object×material output mesh builder (corner-deduplicated) ──────────────────────────────────────────────────

struct MeshBuild
{
    ImportedMesh                                              mesh;
    crd::containers::HashMap<ObjCorner, crd::u32, ObjCornerHash> corner_map;
    bool                                                      any_vn = false;
    bool                                                      any_vt = false;

    explicit MeshBuild(crd::memory::IAllocator* a) : mesh(a), corner_map(a) {}
};

} // namespace

// ── MTL ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────

ImportStatus parse_mtl(crd::containers::ConstSpan<crd::u8> bytes, crd::memory::IAllocator* alloc, ImportedAsset& out)
{
    LineScan scan{bytes.data(), bytes.data() + bytes.size()};
    const crd::u8* ls = nullptr;
    const crd::u8* le = nullptr;
    char           kw[64];
    char           tok[256];
    bool           any_recognized = false;
    bool           have_current   = false;
    bool           current_has_pr = false; // an explicit Pr beats the Ns fallback regardless of line order

    while (scan.next_line(ls, le))
    {
        LineTok lt{ls, le};
        if (!lt.next(kw, sizeof(kw))) { continue; } // blank/comment line
        if (std::strcmp(kw, "newmtl") == 0)
        {
            ImportedMaterial m(alloc);
            char             name[256];
            lt.rest(name, sizeof(name));
            m.name.append(name);
            out.materials.push_back(static_cast<ImportedMaterial&&>(m));
            have_current   = true;
            current_has_pr = false;
            any_recognized = true;
            continue;
        }
        const bool is_kd = std::strcmp(kw, "Kd") == 0;
        const bool is_ns = std::strcmp(kw, "Ns") == 0;
        const bool is_pr = std::strcmp(kw, "Pr") == 0;
        const bool is_pm = std::strcmp(kw, "Pm") == 0;
        if (is_kd || is_ns || is_pr || is_pm)
        {
            if (!have_current) { return ImportStatus::Malformed; } // property before any newmtl
            ImportedMaterial& m = out.materials[out.materials.size() - 1];
            crd::f32          f[3] = {0.0F, 0.0F, 0.0F};
            const int         want = is_kd ? 3 : 1;
            for (int i = 0; i < want; ++i)
            {
                if (!lt.next(tok, sizeof(tok)) || !parse_f32(tok, f[i])) { return ImportStatus::Malformed; }
                if (!std::isfinite(f[i])) { return ImportStatus::NonFiniteData; }
            }
            if (is_kd) { m.base_color = crd::math::Vec3<crd::f32>{f[0], f[1], f[2]}; }
            else if (is_pr)
            {
                m.roughness    = clamp01(f[0]);
                current_has_pr = true;
            }
            else if (is_pm) { m.metallic = clamp01(f[0]); }
            else if (!current_has_pr) // Ns fallback: Blinn-Phong shininess → GGX roughness ≈ sqrt(2/(2+Ns))
            {
                const crd::f32 ns = f[0] < 0.0F ? 0.0F : f[0];
                const crd::f32 r  = std::sqrt(2.0F / (2.0F + ns));
                m.roughness       = r > 1.0F ? 1.0F : r;
            }
            any_recognized = true;
            continue;
        }
        // Ka/Ks/Ke/d/Ni/illum/map_* and friends: standard but unused today (textures land at GEO-3) — silently skipped.
    }
    return any_recognized ? ImportStatus::Ok : ImportStatus::NotRecognized;
}

// ── OBJ ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────

ImportStatus parse_obj(crd::containers::ConstSpan<crd::u8> bytes, crd::memory::IAllocator* alloc, ImportedAsset& out)
{
    // source attribute pools (OBJ's three separate index spaces)
    crd::containers::Array<crd::math::Vec3<crd::f32>> src_v(alloc);
    crd::containers::Array<crd::math::Vec2<crd::f32>> src_vt(alloc);
    crd::containers::Array<crd::math::Vec3<crd::f32>> src_vn(alloc);

    MeshBuild build(alloc);
    char      current_name[256] = {'\0'};
    crd::i32  current_material  = -1;
    bool      any_recognized    = false;

    // finalize the current build into `out` (only runs with ≥1 triangle emit — point-cloud/empty runs vanish silently)
    const auto finalize = [&]() {
        if (build.mesh.triangle_count() == 0U) { return; }
        if (current_name[0] != '\0') { build.mesh.name.append(current_name); }
        build.mesh.material = current_material;
        if (!build.any_vn) { build.mesh.normals.clear(); }
        if (!build.any_vt) { build.mesh.uv0.clear(); }
        out.meshes.push_back(static_cast<ImportedMesh&&>(build.mesh));
        build = MeshBuild(alloc);
    };

    // one corner → deduplicated output vertex index (missing attributes fill zero + warn; GEO-2 recomputes)
    const auto emit_corner = [&](const ObjCorner& c) -> crd::u32 {
        if (const crd::u32* found = build.corner_map.find(c)) { return *found; }
        const crd::u32 idx = static_cast<crd::u32>(build.mesh.positions.size());
        build.mesh.positions.push_back(src_v[static_cast<crd::usize>(c.v - 1)]);
        if (c.vt != 0)
        {
            build.mesh.uv0.push_back(src_vt[static_cast<crd::usize>(c.vt - 1)]);
            build.any_vt = true;
        }
        else
        {
            build.mesh.uv0.push_back(crd::math::Vec2<crd::f32>{0.0F, 0.0F});
            if (build.any_vt) { ++out.warning_count; }
        }
        if (c.vn != 0)
        {
            build.mesh.normals.push_back(src_vn[static_cast<crd::usize>(c.vn - 1)]);
            build.any_vn = true;
        }
        else
        {
            build.mesh.normals.push_back(crd::math::Vec3<crd::f32>{0.0F, 0.0F, 0.0F});
            if (build.any_vn) { ++out.warning_count; }
        }
        build.corner_map.insert(c, idx);
        return idx;
    };

    LineScan scan{bytes.data(), bytes.data() + bytes.size()};
    const crd::u8* ls = nullptr;
    const crd::u8* le = nullptr;
    char           kw[64];
    char           tok[256];

    while (scan.next_line(ls, le))
    {
        LineTok lt{ls, le};
        if (!lt.next(kw, sizeof(kw))) { continue; }

        if (std::strcmp(kw, "v") == 0 || std::strcmp(kw, "vn") == 0)
        {
            crd::f32 f[3];
            for (int i = 0; i < 3; ++i)
            {
                if (!lt.next(tok, sizeof(tok)) || !parse_f32(tok, f[i])) { return ImportStatus::Malformed; }
                if (!std::isfinite(f[i])) { return ImportStatus::NonFiniteData; }
            }
            if (kw[1] == '\0') { src_v.push_back(crd::math::Vec3<crd::f32>{f[0], f[1], f[2]}); }
            else { src_vn.push_back(crd::math::Vec3<crd::f32>{f[0], f[1], f[2]}); }
            any_recognized = true;
        }
        else if (std::strcmp(kw, "vt") == 0)
        {
            crd::f32 f[2];
            for (int i = 0; i < 2; ++i)
            {
                if (!lt.next(tok, sizeof(tok)) || !parse_f32(tok, f[i])) { return ImportStatus::Malformed; }
                if (!std::isfinite(f[i])) { return ImportStatus::NonFiniteData; }
            }
            src_vt.push_back(crd::math::Vec2<crd::f32>{f[0], f[1]}); // optional third component ignored
            any_recognized = true;
        }
        else if (std::strcmp(kw, "f") == 0)
        {
            const crd::i32 counts[3] = {static_cast<crd::i32>(src_v.size()), static_cast<crd::i32>(src_vt.size()),
                                        static_cast<crd::i32>(src_vn.size())};
            crd::u32       fan[2]    = {0, 0}; // fan triangulation: (first, prev, current)
            crd::u32       first     = 0;
            int            corner_n  = 0;
            while (lt.next(tok, sizeof(tok)))
            {
                ObjCorner c;
                if (!parse_corner(tok, counts, c)) { return ImportStatus::Malformed; }
                const crd::u32 idx = emit_corner(c);
                if (corner_n == 0) { first = idx; }
                else if (corner_n >= 2)
                {
                    build.mesh.indices.push_back(first);
                    build.mesh.indices.push_back(fan[1]);
                    build.mesh.indices.push_back(idx);
                }
                fan[0] = fan[1];
                fan[1] = idx;
                ++corner_n;
            }
            if (corner_n < 3) { return ImportStatus::Malformed; } // a face needs ≥3 corners
            any_recognized = true;
        }
        else if (std::strcmp(kw, "o") == 0 || std::strcmp(kw, "g") == 0)
        {
            finalize();
            lt.rest(current_name, sizeof(current_name));
            any_recognized = true;
        }
        else if (std::strcmp(kw, "usemtl") == 0)
        {
            char name[256];
            lt.rest(name, sizeof(name));
            crd::i32 resolved = -1;
            for (crd::usize i = 0; i < out.materials.size(); ++i)
            {
                if (std::strcmp(out.materials[i].name.c_str(), name) == 0)
                {
                    resolved = static_cast<crd::i32>(i);
                    break;
                }
            }
            if (resolved == -1) { ++out.warning_count; } // missing .mtl / unknown name must not kill the geometry
            if (resolved != current_material)
            {
                finalize(); // material change splits the mesh (the render-submission granularity)
                current_material = resolved;
            }
            any_recognized = true;
        }
        else if (std::strcmp(kw, "mtllib") == 0 || std::strcmp(kw, "s") == 0)
        {
            any_recognized = true; // recorded/smoothing: the caller handles mtllib I/O; smoothing groups are GEO-2's concern
        }
        else if (std::strcmp(kw, "l") == 0 || std::strcmp(kw, "p") == 0)
        {
            ++out.warning_count; // valid OBJ, but lines/points are not triangle geometry — skipped honestly
            any_recognized = true;
        }
        else
        {
            ++out.warning_count; // unknown keyword: skip the line, never the file
        }
    }

    if (!any_recognized) { return ImportStatus::NotRecognized; }
    finalize();
    for (crd::usize i = 0; i < out.meshes.size(); ++i)
    {
        if (!out.meshes[i].is_consistent()) { return ImportStatus::Malformed; } // defensive: the parser contract
    }
    return ImportStatus::Ok;
}

} // namespace crd::assetio
