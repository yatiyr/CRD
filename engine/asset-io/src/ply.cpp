// ply.cpp — GEO-1: the schema-driven PLY parser (ASCII + binary LE/BE). See ply.hpp for the policy.

#include <crd/assetio/ply.hpp>

#include <cmath>
#include <cstdlib>
#include <cstring>

namespace crd::assetio
{
namespace
{

// ── scalar types ────────────────────────────────────────────────────────────────────────────────────────────────────────

enum class PlyType : crd::u8
{
    I8,
    U8,
    I16,
    U16,
    I32,
    U32,
    F32,
    F64,
    None,
};

[[nodiscard]] crd::usize type_size(PlyType t) noexcept
{
    switch (t)
    {
    case PlyType::I8:
    case PlyType::U8: return 1;
    case PlyType::I16:
    case PlyType::U16: return 2;
    case PlyType::I32:
    case PlyType::U32:
    case PlyType::F32: return 4;
    case PlyType::F64: return 8;
    default: return 0;
    }
}

[[nodiscard]] PlyType type_from(const char* s) noexcept
{
    if (std::strcmp(s, "char") == 0 || std::strcmp(s, "int8") == 0) { return PlyType::I8; }
    if (std::strcmp(s, "uchar") == 0 || std::strcmp(s, "uint8") == 0) { return PlyType::U8; }
    if (std::strcmp(s, "short") == 0 || std::strcmp(s, "int16") == 0) { return PlyType::I16; }
    if (std::strcmp(s, "ushort") == 0 || std::strcmp(s, "uint16") == 0) { return PlyType::U16; }
    if (std::strcmp(s, "int") == 0 || std::strcmp(s, "int32") == 0) { return PlyType::I32; }
    if (std::strcmp(s, "uint") == 0 || std::strcmp(s, "uint32") == 0) { return PlyType::U32; }
    if (std::strcmp(s, "float") == 0 || std::strcmp(s, "float32") == 0) { return PlyType::F32; }
    if (std::strcmp(s, "double") == 0 || std::strcmp(s, "float64") == 0) { return PlyType::F64; }
    return PlyType::None;
}

// ── header schema ───────────────────────────────────────────────────────────────────────────────────────────────────────

// Which vertex/face channel a property feeds; everything else is Skip (read-and-discarded exactly).
enum class Sem : crd::u8
{
    Skip = 0,
    X,
    Y,
    Z,
    NX,
    NY,
    NZ,
    U,
    V,
    FaceList,
};

[[nodiscard]] Sem vertex_sem(const char* name) noexcept
{
    if (std::strcmp(name, "x") == 0) { return Sem::X; }
    if (std::strcmp(name, "y") == 0) { return Sem::Y; }
    if (std::strcmp(name, "z") == 0) { return Sem::Z; }
    if (std::strcmp(name, "nx") == 0) { return Sem::NX; }
    if (std::strcmp(name, "ny") == 0) { return Sem::NY; }
    if (std::strcmp(name, "nz") == 0) { return Sem::NZ; }
    if (std::strcmp(name, "u") == 0 || std::strcmp(name, "s") == 0 || std::strcmp(name, "texture_u") == 0) { return Sem::U; }
    if (std::strcmp(name, "v") == 0 || std::strcmp(name, "t") == 0 || std::strcmp(name, "texture_v") == 0) { return Sem::V; }
    return Sem::Skip;
}

struct PlyProperty
{
    PlyType type       = PlyType::None; // item type (list: the per-item type)
    PlyType count_type = PlyType::None; // None = scalar; else this is a LIST property
    Sem     sem        = Sem::Skip;
};

constexpr crd::u32 kMaxProps    = 32;
constexpr crd::u32 kMaxElements = 16;

struct PlyElement
{
    crd::u64    count   = 0;
    crd::u32    n_props = 0;
    bool        is_vertex = false;
    bool        is_face   = false;
    PlyProperty props[kMaxProps];
};

enum class PlyFormat : crd::u8
{
    Ascii,
    BinaryLE,
    BinaryBE,
};

struct PlySchema
{
    PlyFormat  format     = PlyFormat::Ascii;
    crd::u32   n_elements = 0;
    PlyElement elements[kMaxElements];
};

// ── header parse (line/token based, mirrors the OBJ scanner) ────────────────────────────────────────────────────────────

struct LineScan
{
    const crd::u8* p;
    const crd::u8* end;

    bool next_line(const crd::u8*& ls, const crd::u8*& le) noexcept
    {
        if (p == end) { return false; }
        ls               = p;
        const crd::u8* q = p;
        while (q < end && *q != '\n') { ++q; }
        le = q;
        p  = (q < end) ? q + 1 : q;
        while (le > ls && (*(le - 1) == '\r' || *(le - 1) == ' ')) { --le; }
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
};

// Parse the header; on success `body` points at the first byte after "end_header\n". Malformed/NotRecognized on failure.
[[nodiscard]] ImportStatus parse_header(crd::containers::ConstSpan<crd::u8> bytes, PlySchema& schema,
                                        const crd::u8*& body) noexcept
{
    LineScan scan{bytes.data(), bytes.data() + bytes.size()};
    const crd::u8* ls = nullptr;
    const crd::u8* le = nullptr;
    char           kw[64];
    char           tok[64];

    if (!scan.next_line(ls, le)) { return ImportStatus::NotRecognized; }
    LineTok first{ls, le};
    if (!first.next(kw, sizeof(kw)) || std::strcmp(kw, "ply") != 0) { return ImportStatus::NotRecognized; }

    bool have_format = false;
    while (scan.next_line(ls, le))
    {
        LineTok lt{ls, le};
        if (!lt.next(kw, sizeof(kw))) { continue; }
        if (std::strcmp(kw, "comment") == 0 || std::strcmp(kw, "obj_info") == 0) { continue; }
        if (std::strcmp(kw, "format") == 0)
        {
            if (!lt.next(tok, sizeof(tok))) { return ImportStatus::Malformed; }
            if (std::strcmp(tok, "ascii") == 0) { schema.format = PlyFormat::Ascii; }
            else if (std::strcmp(tok, "binary_little_endian") == 0) { schema.format = PlyFormat::BinaryLE; }
            else if (std::strcmp(tok, "binary_big_endian") == 0) { schema.format = PlyFormat::BinaryBE; }
            else { return ImportStatus::Malformed; }
            have_format = true;
            continue;
        }
        if (std::strcmp(kw, "element") == 0)
        {
            if (schema.n_elements >= kMaxElements) { return ImportStatus::Malformed; }
            if (!lt.next(tok, sizeof(tok))) { return ImportStatus::Malformed; }
            PlyElement& e = schema.elements[schema.n_elements++];
            e.is_vertex   = std::strcmp(tok, "vertex") == 0;
            e.is_face     = std::strcmp(tok, "face") == 0;
            if (!lt.next(tok, sizeof(tok))) { return ImportStatus::Malformed; }
            char*      endp = nullptr;
            const long long n = std::strtoll(tok, &endp, 10);
            if (endp == tok || *endp != '\0' || n < 0) { return ImportStatus::Malformed; }
            e.count = static_cast<crd::u64>(n);
            continue;
        }
        if (std::strcmp(kw, "property") == 0)
        {
            if (schema.n_elements == 0) { return ImportStatus::Malformed; } // property before any element
            PlyElement& e = schema.elements[schema.n_elements - 1];
            if (e.n_props >= kMaxProps) { return ImportStatus::Malformed; }
            if (!lt.next(tok, sizeof(tok))) { return ImportStatus::Malformed; }
            PlyProperty& p = e.props[e.n_props];
            if (std::strcmp(tok, "list") == 0)
            {
                if (!lt.next(tok, sizeof(tok))) { return ImportStatus::Malformed; }
                p.count_type = type_from(tok);
                if (!lt.next(tok, sizeof(tok))) { return ImportStatus::Malformed; }
                p.type = type_from(tok);
                if (p.count_type == PlyType::None || p.type == PlyType::None) { return ImportStatus::Malformed; }
                if (!lt.next(tok, sizeof(tok))) { return ImportStatus::Malformed; } // list name
                const bool is_idx = std::strcmp(tok, "vertex_indices") == 0 || std::strcmp(tok, "vertex_index") == 0;
                p.sem             = (e.is_face && is_idx) ? Sem::FaceList : Sem::Skip;
            }
            else
            {
                p.type = type_from(tok);
                if (p.type == PlyType::None) { return ImportStatus::Malformed; }
                if (!lt.next(tok, sizeof(tok))) { return ImportStatus::Malformed; } // property name
                p.sem = e.is_vertex ? vertex_sem(tok) : Sem::Skip;
            }
            ++e.n_props;
            continue;
        }
        if (std::strcmp(kw, "end_header") == 0)
        {
            if (!have_format) { return ImportStatus::Malformed; }
            body = scan.p;
            return ImportStatus::Ok;
        }
        return ImportStatus::Malformed; // unknown header keyword
    }
    return ImportStatus::Truncated; // no end_header
}

// ── body readers ────────────────────────────────────────────────────────────────────────────────────────────────────────

// One scalar of `t` from the binary body (endian-corrected) as f64. False = out of bytes.
[[nodiscard]] bool read_binary_scalar(const crd::u8*& p, const crd::u8* end, PlyType t, bool swap, crd::f64& out) noexcept
{
    const crd::usize sz = type_size(t);
    if (static_cast<crd::usize>(end - p) < sz) { return false; }
    crd::u8 raw[8];
    for (crd::usize i = 0; i < sz; ++i) { raw[i] = swap ? p[sz - 1 - i] : p[i]; }
    p += sz;
    switch (t)
    {
    case PlyType::I8: out = static_cast<crd::f64>(static_cast<crd::i8>(raw[0])); return true;
    case PlyType::U8: out = static_cast<crd::f64>(raw[0]); return true;
    case PlyType::I16: {
        crd::i16 v;
        std::memcpy(&v, raw, 2);
        out = static_cast<crd::f64>(v);
        return true;
    }
    case PlyType::U16: {
        crd::u16 v;
        std::memcpy(&v, raw, 2);
        out = static_cast<crd::f64>(v);
        return true;
    }
    case PlyType::I32: {
        crd::i32 v;
        std::memcpy(&v, raw, 4);
        out = static_cast<crd::f64>(v);
        return true;
    }
    case PlyType::U32: {
        crd::u32 v;
        std::memcpy(&v, raw, 4);
        out = static_cast<crd::f64>(v);
        return true;
    }
    case PlyType::F32: {
        crd::f32 v;
        std::memcpy(&v, raw, 4);
        out = static_cast<crd::f64>(v);
        return true;
    }
    case PlyType::F64: {
        crd::f64 v;
        std::memcpy(&v, raw, 8);
        out = v;
        return true;
    }
    default: return false;
    }
}

// ASCII scalar: next whitespace token (crossing lines) as f64. Distinguishes truncation (false) at the token level;
// non-numeric tokens report through `bad`.
struct AsciiTok
{
    const crd::u8* p;
    const crd::u8* end;

    [[nodiscard]] bool next_f64(crd::f64& out, bool& bad) noexcept
    {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) { ++p; }
        if (p == end) { return false; }
        char       buf[64];
        crd::usize n = 0;
        while (p < end && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n')
        {
            if (n + 1 < sizeof(buf)) { buf[n++] = static_cast<char>(*p); }
            ++p;
        }
        buf[n]      = '\0';
        char* endp  = nullptr;
        out         = std::strtod(buf, &endp);
        bad         = (endp == buf || *endp != '\0');
        return true;
    }
};

} // namespace

ImportStatus parse_ply(crd::containers::ConstSpan<crd::u8> bytes, crd::memory::IAllocator* alloc, ImportedAsset& out)
{
    PlySchema      schema;
    const crd::u8* body = nullptr;
    {
        const ImportStatus st = parse_header(bytes, schema, body);
        if (st != ImportStatus::Ok) { return st; }
    }

    ImportedMesh mesh(alloc);
    bool         any_n  = false;
    bool         any_uv = false;

    const crd::u8* p    = body;
    const crd::u8* end  = bytes.data() + bytes.size();
    const bool     bin  = schema.format != PlyFormat::Ascii;
    const bool     swap = schema.format == PlyFormat::BinaryBE;
    AsciiTok       at{body, end};

    // one generic scalar from the body in the declared format
    const auto read_scalar = [&](PlyType t, crd::f64& v, ImportStatus& fail) -> bool {
        if (bin)
        {
            if (!read_binary_scalar(p, end, t, swap, v))
            {
                fail = ImportStatus::Truncated;
                return false;
            }
            return true;
        }
        bool bad = false;
        if (!at.next_f64(v, bad))
        {
            fail = ImportStatus::Truncated;
            return false;
        }
        if (bad)
        {
            fail = ImportStatus::Malformed;
            return false;
        }
        return true;
    };

    for (crd::u32 ei = 0; ei < schema.n_elements; ++ei)
    {
        const PlyElement& e = schema.elements[ei];
        for (crd::u64 r = 0; r < e.count; ++r)
        {
            crd::f32 vx = 0.0F;
            crd::f32 vy = 0.0F;
            crd::f32 vz = 0.0F;
            crd::f32 nx = 0.0F;
            crd::f32 ny = 0.0F;
            crd::f32 nz = 0.0F;
            crd::f32 tu = 0.0F;
            crd::f32 tv = 0.0F;
            bool     has_n  = false;
            bool     has_uv = false;

            for (crd::u32 pi = 0; pi < e.n_props; ++pi)
            {
                const PlyProperty& prop = e.props[pi];
                ImportStatus       fail = ImportStatus::Ok;
                if (prop.count_type != PlyType::None) // LIST property
                {
                    crd::f64 cnt = 0.0;
                    if (!read_scalar(prop.count_type, cnt, fail)) { return fail; }
                    const crd::i64 n = static_cast<crd::i64>(cnt);
                    if (n < 0 || n > 255) { return ImportStatus::Malformed; } // an insane list count is corruption
                    if (prop.sem == Sem::FaceList)
                    {
                        crd::u32 fan_first = 0;
                        crd::u32 fan_prev  = 0;
                        for (crd::i64 k = 0; k < n; ++k)
                        {
                            crd::f64 vd = 0.0;
                            if (!read_scalar(prop.type, vd, fail)) { return fail; }
                            const crd::i64 idx = static_cast<crd::i64>(vd);
                            if (idx < 0 || static_cast<crd::u64>(idx) >= static_cast<crd::u64>(mesh.positions.size()))
                            {
                                return ImportStatus::Malformed;
                            }
                            const crd::u32 u = static_cast<crd::u32>(idx);
                            if (k == 0) { fan_first = u; }
                            else if (k >= 2)
                            {
                                mesh.indices.push_back(fan_first);
                                mesh.indices.push_back(fan_prev);
                                mesh.indices.push_back(u);
                            }
                            fan_prev = u;
                        }
                        if (n > 0 && n < 3) { ++out.warning_count; } // degenerate face record: skipped, not fatal
                    }
                    else
                    {
                        for (crd::i64 k = 0; k < n; ++k) // skipped list: read-and-discard EXACTLY
                        {
                            crd::f64 dump = 0.0;
                            if (!read_scalar(prop.type, dump, fail)) { return fail; }
                        }
                    }
                    continue;
                }
                crd::f64 v = 0.0;
                if (!read_scalar(prop.type, v, fail)) { return fail; }
                switch (prop.sem)
                {
                case Sem::X: vx = static_cast<crd::f32>(v); break;
                case Sem::Y: vy = static_cast<crd::f32>(v); break;
                case Sem::Z: vz = static_cast<crd::f32>(v); break;
                case Sem::NX:
                    nx    = static_cast<crd::f32>(v);
                    has_n = true;
                    break;
                case Sem::NY:
                    ny    = static_cast<crd::f32>(v);
                    has_n = true;
                    break;
                case Sem::NZ:
                    nz    = static_cast<crd::f32>(v);
                    has_n = true;
                    break;
                case Sem::U:
                    tu     = static_cast<crd::f32>(v);
                    has_uv = true;
                    break;
                case Sem::V:
                    tv     = static_cast<crd::f32>(v);
                    has_uv = true;
                    break;
                default: break; // Skip
                }
            }

            if (e.is_vertex)
            {
                if (!std::isfinite(vx) || !std::isfinite(vy) || !std::isfinite(vz)) { return ImportStatus::NonFiniteData; }
                mesh.positions.push_back(crd::math::Vec3<crd::f32>{vx, vy, vz});
                mesh.normals.push_back(crd::math::Vec3<crd::f32>{nx, ny, nz});
                mesh.uv0.push_back(crd::math::Vec2<crd::f32>{tu, tv});
                any_n  = any_n || has_n;
                any_uv = any_uv || has_uv;
            }
        }
    }

    if (!any_n) { mesh.normals.clear(); }
    if (!any_uv) { mesh.uv0.clear(); }
    if (!mesh.is_consistent()) { return ImportStatus::Malformed; }
    out.meshes.push_back(static_cast<ImportedMesh&&>(mesh));
    return ImportStatus::Ok;
}

} // namespace crd::assetio
