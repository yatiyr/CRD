// stl.cpp — GEO-1: the STL parser (binary + ASCII). See stl.hpp for the dirty-file policy this implements.

#include <crd/assetio/stl.hpp>

#include <cmath>
#include <cstdlib>
#include <cstring>

namespace crd::assetio
{
namespace
{

// ── shared helpers ──────────────────────────────────────────────────────────────────────────────────────────────────────

[[nodiscard]] bool finite3(const crd::math::Vec3<crd::f32>& v) noexcept
{
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

// Facet normal policy: use the file's normal when it is finite and meaningfully non-zero; otherwise recompute from the
// winding (right-hand rule, normalized). A degenerate triangle (zero cross product) gets a zero normal — the triangle is
// kept (GEO-2 conditioning decides degeneracy policy), the normal is honestly zero rather than invented.
[[nodiscard]] crd::math::Vec3<crd::f32> facet_normal(const crd::math::Vec3<crd::f32>& file_n,
                                                     const crd::math::Vec3<crd::f32>& a,
                                                     const crd::math::Vec3<crd::f32>& b,
                                                     const crd::math::Vec3<crd::f32>& c) noexcept
{
    constexpr crd::f32 len_eps_sq = 1.0e-12F;
    if (finite3(file_n))
    {
        const crd::f32 l2 = file_n.x * file_n.x + file_n.y * file_n.y + file_n.z * file_n.z;
        if (l2 > len_eps_sq) { return file_n; }
    }
    const crd::math::Vec3<crd::f32> e1{b.x - a.x, b.y - a.y, b.z - a.z};
    const crd::math::Vec3<crd::f32> e2{c.x - a.x, c.y - a.y, c.z - a.z};
    crd::math::Vec3<crd::f32>       n{e1.y * e2.z - e1.z * e2.y, e1.z * e2.x - e1.x * e2.z, e1.x * e2.y - e1.y * e2.x};
    const crd::f32                  l2 = n.x * n.x + n.y * n.y + n.z * n.z;
    if (l2 > len_eps_sq)
    {
        const crd::f32 inv = 1.0F / std::sqrt(l2);
        n.x *= inv;
        n.y *= inv;
        n.z *= inv;
        return n;
    }
    return crd::math::Vec3<crd::f32>{0.0F, 0.0F, 0.0F};
}

// Append one triangle (soup: 3 fresh vertices + 3 fresh indices; welding is GEO-2's job).
void push_triangle(ImportedMesh& m, const crd::math::Vec3<crd::f32>& n, const crd::math::Vec3<crd::f32>& a,
                   const crd::math::Vec3<crd::f32>& b, const crd::math::Vec3<crd::f32>& c)
{
    const crd::u32 base = static_cast<crd::u32>(m.positions.size());
    m.positions.push_back(a);
    m.positions.push_back(b);
    m.positions.push_back(c);
    const crd::math::Vec3<crd::f32> fn = facet_normal(n, a, b, c);
    m.normals.push_back(fn);
    m.normals.push_back(fn);
    m.normals.push_back(fn);
    m.indices.push_back(base + 0U);
    m.indices.push_back(base + 1U);
    m.indices.push_back(base + 2U);
}

// ── binary STL ──────────────────────────────────────────────────────────────────────────────────────────────────────────
// Layout: 80-byte header · u32 triangle count · count × { f32 normal[3] · f32 v0[3] · f32 v1[3] · f32 v2[3] · u16 attr }.
// All little-endian (every Cerid target is LE; a BE port would swap here). Detection is STRUCTURAL: the size equation —
// never the header text ("solid"-prefixed binaries exist in the wild).

constexpr crd::usize kBinHeaderBytes   = 84;
constexpr crd::usize kBinTriangleBytes = 50;

[[nodiscard]] bool binary_size_matches(crd::usize size, crd::u32 count) noexcept
{
    return static_cast<crd::u64>(size)
        == static_cast<crd::u64>(kBinHeaderBytes) + static_cast<crd::u64>(kBinTriangleBytes) * count;
}

[[nodiscard]] ImportStatus parse_stl_binary(crd::containers::ConstSpan<crd::u8> bytes, crd::u32 count, ImportedMesh& mesh)
{
    const crd::u8* p = bytes.data() + kBinHeaderBytes;
    for (crd::u32 t = 0; t < count; ++t, p += kBinTriangleBytes)
    {
        crd::f32 f[12]; // normal, v0, v1, v2
        std::memcpy(f, p, sizeof(f));
        const crd::math::Vec3<crd::f32> n{f[0], f[1], f[2]};
        const crd::math::Vec3<crd::f32> a{f[3], f[4], f[5]};
        const crd::math::Vec3<crd::f32> b{f[6], f[7], f[8]};
        const crd::math::Vec3<crd::f32> c{f[9], f[10], f[11]};
        if (!finite3(a) || !finite3(b) || !finite3(c)) { return ImportStatus::NonFiniteData; }
        push_triangle(mesh, n, a, b, c); // the 2-byte attribute word is ignored (no portable meaning)
    }
    return ImportStatus::Ok;
}

// ── ASCII STL ───────────────────────────────────────────────────────────────────────────────────────────────────────────
// Grammar: solid [name] { facet normal x y z / outer loop / vertex×3 / endloop / endfacet } endsolid [name]. Tokenized
// over the byte span; keywords case-insensitive (exporters disagree on case); floats via strtod on a bounded stack copy.

struct Tok
{
    const crd::u8* p   = nullptr;
    const crd::u8* end = nullptr;

    [[nodiscard]] bool at_end() const noexcept
    {
        const crd::u8* q = p;
        while (q < end && (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n')) { ++q; }
        return q == end;
    }

    // Next whitespace-delimited token into `buf` (truncated to cap-1, NUL-terminated). False at end of input.
    bool next(char* buf, crd::usize cap) noexcept
    {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) { ++p; }
        if (p == end) { return false; }
        crd::usize n = 0;
        while (p < end && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n')
        {
            if (n + 1 < cap) { buf[n++] = static_cast<char>(*p); }
            ++p;
        }
        buf[n] = '\0';
        return true;
    }

    // Rest of the current line (for solid names, which may contain spaces). Leading whitespace skipped; trailing CR trimmed.
    void rest_of_line(char* buf, crd::usize cap) noexcept
    {
        while (p < end && (*p == ' ' || *p == '\t')) { ++p; }
        crd::usize n = 0;
        while (p < end && *p != '\n')
        {
            if (*p != '\r' && n + 1 < cap) { buf[n++] = static_cast<char>(*p); }
            ++p;
        }
        buf[n] = '\0';
    }
};

[[nodiscard]] bool tok_eq(const char* tok, const char* kw) noexcept
{
    // case-insensitive ASCII compare (exporters emit Solid/FACET/etc.)
    while (*tok != '\0' && *kw != '\0')
    {
        const char a = (*tok >= 'A' && *tok <= 'Z') ? static_cast<char>(*tok + 32) : *tok;
        if (a != *kw) { return false; }
        ++tok;
        ++kw;
    }
    return *tok == '\0' && *kw == '\0';
}

[[nodiscard]] bool parse_f32(const char* tok, crd::f32& out) noexcept
{
    char*        endp = nullptr;
    const double v    = std::strtod(tok, &endp);
    if (endp == tok || *endp != '\0') { return false; }
    out = static_cast<crd::f32>(v);
    return true;
}

// Read three floats (one Vec3) from the token stream. Distinguishes "stream ended" (Truncated) from "bad token" (Malformed).
[[nodiscard]] ImportStatus read_vec3(Tok& tk, crd::math::Vec3<crd::f32>& v) noexcept
{
    char      buf[64];
    crd::f32* comp[3] = {&v.x, &v.y, &v.z};
    for (int i = 0; i < 3; ++i)
    {
        if (!tk.next(buf, sizeof(buf))) { return ImportStatus::Truncated; }
        if (!parse_f32(buf, *comp[i])) { return ImportStatus::Malformed; }
    }
    return ImportStatus::Ok;
}

[[nodiscard]] ImportStatus parse_stl_ascii(crd::containers::ConstSpan<crd::u8> bytes, ImportedMesh& mesh)
{
    Tok  tk{bytes.data(), bytes.data() + bytes.size()};
    char buf[64];
    if (!tk.next(buf, sizeof(buf)) || !tok_eq(buf, "solid")) { return ImportStatus::NotRecognized; }
    char name[256];
    tk.rest_of_line(name, sizeof(name));
    mesh.name.append(name);

    for (;;)
    {
        if (!tk.next(buf, sizeof(buf))) { return ImportStatus::Truncated; } // must reach endsolid
        if (tok_eq(buf, "endsolid"))
        {
            char tail[256];
            tk.rest_of_line(tail, sizeof(tail)); // optional trailing name — ignored
            return ImportStatus::Ok;
        }
        if (!tok_eq(buf, "facet")) { return ImportStatus::Malformed; }
        if (!tk.next(buf, sizeof(buf)) || !tok_eq(buf, "normal")) { return ImportStatus::Malformed; }
        crd::math::Vec3<crd::f32> n;
        ImportStatus              st = read_vec3(tk, n);
        if (st != ImportStatus::Ok) { return st; }
        if (!tk.next(buf, sizeof(buf))) { return ImportStatus::Truncated; }
        if (!tok_eq(buf, "outer")) { return ImportStatus::Malformed; }
        if (!tk.next(buf, sizeof(buf)) || !tok_eq(buf, "loop")) { return ImportStatus::Malformed; }
        crd::math::Vec3<crd::f32> v[3];
        for (int i = 0; i < 3; ++i)
        {
            if (!tk.next(buf, sizeof(buf))) { return ImportStatus::Truncated; }
            if (!tok_eq(buf, "vertex")) { return ImportStatus::Malformed; }
            st = read_vec3(tk, v[i]);
            if (st != ImportStatus::Ok) { return st; }
            if (!finite3(v[i])) { return ImportStatus::NonFiniteData; }
        }
        if (!tk.next(buf, sizeof(buf)) || !tok_eq(buf, "endloop")) { return ImportStatus::Malformed; }
        if (!tk.next(buf, sizeof(buf)) || !tok_eq(buf, "endfacet")) { return ImportStatus::Malformed; }
        push_triangle(mesh, n, v[0], v[1], v[2]);
    }
}

} // namespace

ImportStatus parse_stl(crd::containers::ConstSpan<crd::u8> bytes, crd::memory::IAllocator* alloc, ImportedAsset& out)
{
    ImportedMesh mesh(alloc);

    // 1. STRUCTURAL binary detection: the size equation is the truth ("solid"-prefixed binaries exist; header text lies).
    if (bytes.size() >= kBinHeaderBytes)
    {
        crd::u32 count = 0;
        std::memcpy(&count, bytes.data() + 80, sizeof(count));
        if (binary_size_matches(bytes.size(), count))
        {
            const ImportStatus st = parse_stl_binary(bytes, count, mesh);
            if (st != ImportStatus::Ok) { return st; }
            if (!mesh.is_consistent()) { return ImportStatus::Malformed; } // defensive: the parser contract
            out.meshes.push_back(static_cast<ImportedMesh&&>(mesh));
            return ImportStatus::Ok;
        }
    }

    // 2. ASCII attempt. A file that starts with "solid" but fails the binary size equation is either real ASCII or a
    //    truncated/padded binary; the ASCII grammar decides (a binary body is not valid ASCII tokens → Malformed, which
    //    is the honest answer for a corrupt binary too).
    const ImportStatus st = parse_stl_ascii(bytes, mesh);
    if (st != ImportStatus::Ok) { return st; }
    if (!mesh.is_consistent()) { return ImportStatus::Malformed; }
    out.meshes.push_back(static_cast<ImportedMesh&&>(mesh));
    return ImportStatus::Ok;
}

} // namespace crd::assetio
