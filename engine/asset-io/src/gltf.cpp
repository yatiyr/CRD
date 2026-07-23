// gltf.cpp — GEO-3: the owned glTF 2.0 geometry/material parser. See gltf.hpp for the covered surface.

#include <crd/assetio/gltf.hpp>
#include <crd/assetio/json.hpp>

#include <cmath>
#include <cstring>

namespace crd::assetio
{

// GEO-5: promoted OUT of the anonymous namespace — the 3MF parser reuses the SAME decompose (one TRS
// extraction for every bundle format; drift between importers would be a silent scene-corruption class).
// column-major 4×4 → TRS. Scale = column norms (negative-determinant folds into scale.x); rotation via the
// max-trace-branch quaternion extraction on the normalized 3×3. Zero-norm columns warn upstream (identity rotation).
[[nodiscard]] bool decompose_matrix_trs(const crd::f64 m[16], crd::math::Vec3<crd::f32>& t,
                                        crd::math::Vec4<crd::f32>& q, crd::math::Vec3<crd::f32>& s)
{
    t = {static_cast<crd::f32>(m[12]), static_cast<crd::f32>(m[13]), static_cast<crd::f32>(m[14])};
    crd::f64 c[3][3]; // c[j] = column j's xyz
    for (int j = 0; j < 3; ++j)
    {
        for (int i = 0; i < 3; ++i) { c[j][i] = m[(4 * j) + i]; }
    }
    crd::f64 sl[3];
    for (int j = 0; j < 3; ++j) { sl[j] = std::sqrt((c[j][0] * c[j][0]) + (c[j][1] * c[j][1]) + (c[j][2] * c[j][2])); }
    if (sl[0] <= 0.0 || sl[1] <= 0.0 || sl[2] <= 0.0)
    {
        s = {static_cast<crd::f32>(sl[0]), static_cast<crd::f32>(sl[1]), static_cast<crd::f32>(sl[2])};
        q = {0.0F, 0.0F, 0.0F, 1.0F};
        return false; // degenerate — caller warns, keeps going
    }
    const crd::f64 det = (c[0][0] * ((c[1][1] * c[2][2]) - (c[1][2] * c[2][1])))
                       - (c[1][0] * ((c[0][1] * c[2][2]) - (c[0][2] * c[2][1])))
                       + (c[2][0] * ((c[0][1] * c[1][2]) - (c[0][2] * c[1][1])));
    if (det < 0.0) { sl[0] = -sl[0]; } // fold the reflection into scale.x (the standard convention)
    s = {static_cast<crd::f32>(sl[0]), static_cast<crd::f32>(sl[1]), static_cast<crd::f32>(sl[2])};
    crd::f64 r[3][3]; // r[i][j] = row i, col j of the pure rotation
    for (int j = 0; j < 3; ++j)
    {
        for (int i = 0; i < 3; ++i) { r[i][j] = c[j][i] / sl[j]; }
    }
    const crd::f64 tr = r[0][0] + r[1][1] + r[2][2];
    crd::f64       qx = 0.0;
    crd::f64       qy = 0.0;
    crd::f64       qz = 0.0;
    crd::f64       qw = 1.0;
    if (tr > 0.0)
    {
        const crd::f64 big = std::sqrt(tr + 1.0) * 2.0;
        qw               = 0.25 * big;
        qx               = (r[2][1] - r[1][2]) / big;
        qy               = (r[0][2] - r[2][0]) / big;
        qz               = (r[1][0] - r[0][1]) / big;
    }
    else if (r[0][0] > r[1][1] && r[0][0] > r[2][2])
    {
        const crd::f64 big = std::sqrt(1.0 + r[0][0] - r[1][1] - r[2][2]) * 2.0;
        qw               = (r[2][1] - r[1][2]) / big;
        qx               = 0.25 * big;
        qy               = (r[0][1] + r[1][0]) / big;
        qz               = (r[0][2] + r[2][0]) / big;
    }
    else if (r[1][1] > r[2][2])
    {
        const crd::f64 big = std::sqrt(1.0 + r[1][1] - r[0][0] - r[2][2]) * 2.0;
        qw               = (r[0][2] - r[2][0]) / big;
        qx               = (r[0][1] + r[1][0]) / big;
        qy               = 0.25 * big;
        qz               = (r[1][2] + r[2][1]) / big;
    }
    else
    {
        const crd::f64 big = std::sqrt(1.0 + r[2][2] - r[0][0] - r[1][1]) * 2.0;
        qw               = (r[1][0] - r[0][1]) / big;
        qx               = (r[0][2] + r[2][0]) / big;
        qy               = (r[1][2] + r[2][1]) / big;
        qz               = 0.25 * big;
    }
    q = {static_cast<crd::f32>(qx), static_cast<crd::f32>(qy), static_cast<crd::f32>(qz), static_cast<crd::f32>(qw)};
    return true;
}

namespace
{

namespace js = crd::assetio::json;

// ── base64 (data-URI buffers) ───────────────────────────────────────────────────────────────────────────────────────────

[[nodiscard]] int b64_value(char c) noexcept
{
    if (c >= 'A' && c <= 'Z') { return c - 'A'; }
    if (c >= 'a' && c <= 'z') { return 26 + (c - 'a'); }
    if (c >= '0' && c <= '9') { return 52 + (c - '0'); }
    if (c == '+') { return 62; }
    if (c == '/') { return 63; }
    return -1;
}

[[nodiscard]] bool b64_decode(const char* s, crd::usize n, crd::containers::Array<crd::u8>& out)
{
    crd::u32 acc  = 0;
    int      bits = 0;
    for (crd::usize i = 0; i < n; ++i)
    {
        const char c = s[i];
        if (c == '=' || c == '\n' || c == '\r') { continue; }
        const int v = b64_value(c);
        if (v < 0) { return false; }
        acc = (acc << 6U) | static_cast<crd::u32>(v);
        bits += 6;
        if (bits >= 8)
        {
            bits -= 8;
            out.push_back(static_cast<crd::u8>((acc >> static_cast<crd::u32>(bits)) & 0xFFU));
        }
    }
    return true;
}

// ── the parsed-document context ─────────────────────────────────────────────────────────────────────────────────────────

struct GltfCtx
{
    const js::JsonDoc*                  doc;
    crd::u32                            root;
    crd::u32                            j_buffers;
    crd::u32                            j_views;
    crd::u32                            j_accessors;
    crd::containers::ConstSpan<crd::u8> bin;      // GLB BIN chunk or the caller's external buffer 0
    crd::containers::Array<crd::u8>*    decoded0; // owned storage when buffer 0 is a data-URI
    crd::memory::IAllocator*            alloc;

    // buffer index → bytes (only buffer 0 is backed today; others fail)
    [[nodiscard]] bool buffer_bytes(crd::i64 index, crd::containers::ConstSpan<crd::u8>& out_bytes) const noexcept
    {
        if (index != 0) { return false; }
        if (decoded0->size() > 0U)
        {
            out_bytes = crd::containers::ConstSpan<crd::u8>(decoded0->data(), decoded0->size());
            return true;
        }
        out_bytes = bin;
        return bin.size() > 0U;
    }
};

[[nodiscard]] crd::usize component_size(crd::i64 ct) noexcept
{
    switch (ct)
    {
    case 5120: // BYTE
    case 5121: return 1; // UNSIGNED_BYTE
    case 5122: // SHORT
    case 5123: return 2; // UNSIGNED_SHORT
    case 5125: // UNSIGNED_INT
    case 5126: return 4; // FLOAT
    default: return 0;
    }
}

[[nodiscard]] crd::u32 type_components(const js::JsonDoc& d, crd::u32 type_node) noexcept
{
    if (js::str_value_eq(d, type_node, "SCALAR")) { return 1; }
    if (js::str_value_eq(d, type_node, "VEC2")) { return 2; }
    if (js::str_value_eq(d, type_node, "VEC3")) { return 3; }
    if (js::str_value_eq(d, type_node, "VEC4")) { return 4; }
    if (js::str_value_eq(d, type_node, "MAT4")) { return 16; }
    return 0;
}

// one component at `p` decoded to f64 (normalized per spec when `normalized`)
[[nodiscard]] crd::f64 read_component(const crd::u8* p, crd::i64 ct, bool normalized) noexcept
{
    switch (ct)
    {
    case 5120: {
        crd::i8 v = 0;
        std::memcpy(&v, p, 1);
        if (!normalized) { return static_cast<crd::f64>(v); }
        if (v <= -127) { return -1.0; } // the spec's signed-normalized clamp (−128 maps to −1, not −128/127)
        return static_cast<crd::f64>(v) / 127.0;
    }
    case 5121: {
        const crd::u8 v = *p;
        return normalized ? static_cast<crd::f64>(v) / 255.0 : static_cast<crd::f64>(v);
    }
    case 5122: {
        crd::i16 v = 0;
        std::memcpy(&v, p, 2);
        if (!normalized) { return static_cast<crd::f64>(v); }
        if (v <= -32767) { return -1.0; }
        return static_cast<crd::f64>(v) / 32767.0;
    }
    case 5123: {
        crd::u16 v = 0;
        std::memcpy(&v, p, 2);
        return normalized ? static_cast<crd::f64>(v) / 65535.0 : static_cast<crd::f64>(v);
    }
    case 5125: {
        crd::u32 v = 0;
        std::memcpy(&v, p, 4);
        return static_cast<crd::f64>(v);
    }
    case 5126: {
        crd::f32 v = 0;
        std::memcpy(&v, p, 4);
        return static_cast<crd::f64>(v);
    }
    default: return 0.0;
    }
}

// Read accessor `acc_index` as f32 components into `out` (count × want_comps; missing trailing components pad 0, extra
// drop — VEC3-position vs VEC4-tangent callers pass their own width). Handles strided views + SPARSE substitution.
[[nodiscard]] ImportStatus read_accessor_f32(const GltfCtx& g, crd::i64 acc_index, crd::u32 want_comps,
                                             crd::containers::Array<crd::f32>& out, crd::u32& out_count)
{
    const js::JsonDoc& d   = *g.doc;
    const crd::u32     acc = js::at(d, g.j_accessors, static_cast<crd::u32>(acc_index));
    if (acc == js::kInvalid) { return ImportStatus::Malformed; }
    const crd::i64 ct         = js::as_i64(d, js::find(d, acc, "componentType"), 0);
    const bool     normalized = js::as_bool(d, js::find(d, acc, "normalized"), false);
    const crd::i64 count      = js::as_i64(d, js::find(d, acc, "count"), 0);
    const crd::u32 comps      = type_components(d, js::find(d, acc, "type"));
    const crd::usize csz      = component_size(ct);
    if (count <= 0 || comps == 0U || csz == 0U) { return ImportStatus::Malformed; }

    out.clear();
    out.resize(static_cast<crd::usize>(count) * want_comps, 0.0F);
    out_count = static_cast<crd::u32>(count);

    const crd::u32 view = js::find(d, acc, "bufferView") != js::kInvalid
                              ? js::at(d, g.j_views, static_cast<crd::u32>(js::as_i64(d, js::find(d, acc, "bufferView"), 0)))
                              : js::kInvalid;
    if (view != js::kInvalid) // an accessor without a view is legally all-zeros (sparse-only)
    {
        crd::containers::ConstSpan<crd::u8> buf;
        if (!g.buffer_bytes(js::as_i64(d, js::find(d, view, "buffer"), 0), buf)) { return ImportStatus::Malformed; }
        const crd::i64   voff    = js::as_i64(d, js::find(d, view, "byteOffset"), 0);
        const crd::i64   vlen    = js::as_i64(d, js::find(d, view, "byteLength"), 0);
        const crd::i64   aoff    = js::as_i64(d, js::find(d, acc, "byteOffset"), 0);
        const crd::usize elem    = csz * comps;
        const crd::i64   stride0 = js::as_i64(d, js::find(d, view, "byteStride"), 0);
        const crd::usize stride  = stride0 > 0 ? static_cast<crd::usize>(stride0) : elem;
        if (voff < 0 || vlen < 0 || aoff < 0) { return ImportStatus::Malformed; }
        const crd::usize need_end = static_cast<crd::usize>(voff) + static_cast<crd::usize>(aoff)
                                  + (static_cast<crd::usize>(count) - 1U) * stride + elem;
        if (need_end > buf.size()
            || static_cast<crd::usize>(aoff) + (static_cast<crd::usize>(count) - 1U) * stride + elem
                   > static_cast<crd::usize>(vlen))
        {
            return ImportStatus::Truncated;
        }
        const crd::u8* base = buf.data() + voff + aoff;
        for (crd::i64 i = 0; i < count; ++i)
        {
            const crd::u8* rec = base + static_cast<crd::usize>(i) * stride;
            const crd::u32 n   = comps < want_comps ? comps : want_comps;
            for (crd::u32 k = 0; k < n; ++k)
            {
                out[static_cast<crd::usize>(i) * want_comps + k] =
                    static_cast<crd::f32>(read_component(rec + k * csz, ct, normalized));
            }
        }
    }

    // SPARSE substitution: indices[] name the records replaced by values[]
    const crd::u32 sparse = js::find(d, acc, "sparse");
    if (sparse != js::kInvalid)
    {
        const crd::i64 scount = js::as_i64(d, js::find(d, sparse, "count"), 0);
        const crd::u32 jsi    = js::find(d, sparse, "indices");
        const crd::u32 jsv    = js::find(d, sparse, "values");
        if (scount <= 0 || jsi == js::kInvalid || jsv == js::kInvalid) { return ImportStatus::Malformed; }
        const auto read_side = [&](crd::u32 side, crd::i64 side_ct, const crd::u8*& base, crd::usize& csz_out,
                                   crd::usize& stride_out) -> ImportStatus {
            const crd::u32 v = js::at(d, g.j_views, static_cast<crd::u32>(js::as_i64(d, js::find(d, side, "bufferView"), 0)));
            if (v == js::kInvalid) { return ImportStatus::Malformed; }
            crd::containers::ConstSpan<crd::u8> buf;
            if (!g.buffer_bytes(js::as_i64(d, js::find(d, v, "buffer"), 0), buf)) { return ImportStatus::Malformed; }
            const crd::i64 voff = js::as_i64(d, js::find(d, v, "byteOffset"), 0);
            const crd::i64 soff = js::as_i64(d, js::find(d, side, "byteOffset"), 0);
            csz_out             = component_size(side_ct);
            stride_out          = csz_out; // sparse sides are tightly packed
            if (csz_out == 0U || voff < 0 || soff < 0) { return ImportStatus::Malformed; }
            if (static_cast<crd::usize>(voff + soff) >= buf.size()) { return ImportStatus::Truncated; }
            base = buf.data() + voff + soff;
            return ImportStatus::Ok;
        };
        const crd::i64 ict = js::as_i64(d, js::find(d, jsi, "componentType"), 0);
        const crd::u8* ibase = nullptr;
        const crd::u8* vbase = nullptr;
        crd::usize     icsz  = 0;
        crd::usize     istr  = 0;
        crd::usize     vcsz  = 0;
        crd::usize     vstr  = 0;
        ImportStatus   st    = read_side(jsi, ict, ibase, icsz, istr);
        if (st != ImportStatus::Ok) { return st; }
        st = read_side(jsv, ct, vbase, vcsz, vstr);
        if (st != ImportStatus::Ok) { return st; }
        for (crd::i64 s = 0; s < scount; ++s)
        {
            const crd::i64 rec = static_cast<crd::i64>(read_component(ibase + static_cast<crd::usize>(s) * icsz, ict, false));
            if (rec < 0 || rec >= count) { return ImportStatus::Malformed; }
            const crd::u32 n = comps < want_comps ? comps : want_comps;
            for (crd::u32 k = 0; k < n; ++k)
            {
                out[static_cast<crd::usize>(rec) * want_comps + k] = static_cast<crd::f32>(
                    read_component(vbase + (static_cast<crd::usize>(s) * comps + k) * vcsz, ct, normalized));
            }
        }
    }
    return ImportStatus::Ok;
}

// ── images (GEO-3 stage 2b: the encoded bytes travel; the COOK decodes via ldr_decode) ─────────────────────────────────

// bufferView index → raw bytes (images use unstrided views)
[[nodiscard]] bool view_bytes(const GltfCtx& g, crd::i64 view_index, crd::containers::ConstSpan<crd::u8>& out_bytes)
{
    const js::JsonDoc& d    = *g.doc;
    const crd::u32     view = js::at(d, g.j_views, static_cast<crd::u32>(view_index));
    if (view == js::kInvalid) { return false; }
    crd::containers::ConstSpan<crd::u8> buf;
    if (!g.buffer_bytes(js::as_i64(d, js::find(d, view, "buffer"), 0), buf)) { return false; }
    const crd::i64 voff = js::as_i64(d, js::find(d, view, "byteOffset"), 0);
    const crd::i64 vlen = js::as_i64(d, js::find(d, view, "byteLength"), 0);
    if (voff < 0 || vlen <= 0) { return false; }
    if (static_cast<crd::usize>(voff) + static_cast<crd::usize>(vlen) > buf.size()) { return false; }
    out_bytes = crd::containers::ConstSpan<crd::u8>(buf.data() + voff, static_cast<crd::usize>(vlen));
    return true;
}

// glTF uris are percent-encoded (RFC 3986); decode %XX so the cook sees a plain relative path
void percent_decode_append(const char* s, crd::u32 n, crd::containers::String& out)
{
    const auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') { return c - '0'; }
        if (c >= 'a' && c <= 'f') { return 10 + (c - 'a'); }
        if (c >= 'A' && c <= 'F') { return 10 + (c - 'A'); }
        return -1;
    };
    for (crd::u32 i = 0; i < n; ++i)
    {
        if (s[i] == '%' && i + 2U < n)
        {
            const int hi = hex(s[i + 1U]);
            const int lo = hex(s[i + 2U]);
            if (hi >= 0 && lo >= 0)
            {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2U;
                continue;
            }
        }
        out.push_back(s[i]);
    }
}

[[nodiscard]] ImportStatus parse_images(const GltfCtx& g, ImportedAsset& out)
{
    const js::JsonDoc& d      = *g.doc;
    const crd::u32     j_imgs = js::find(d, g.root, "images");
    const crd::u32     n      = js::count_of(d, j_imgs);
    for (crd::u32 i = 0; i < n; ++i)
    {
        const crd::u32 ji = js::at(d, j_imgs, i);
        ImportedImage  img(g.alloc);
        char           name[256];
        (void)js::str_value(d, js::find(d, ji, "name"), name, sizeof(name));
        img.name.append(name);

        const crd::u32 j_view = js::find(d, ji, "bufferView");
        const crd::u32 j_uri  = js::find(d, ji, "uri");
        if (j_view != js::kInvalid) // embedded in the binary payload
        {
            crd::containers::ConstSpan<crd::u8> bytes;
            if (!view_bytes(g, js::as_i64(d, j_view, 0), bytes)) { return ImportStatus::Malformed; }
            img.bytes.resize(bytes.size());
            std::memcpy(img.bytes.data(), bytes.data(), bytes.size());
        }
        else if (j_uri != js::kInvalid)
        {
            const js::JsonNode& un = d.nodes[j_uri];
            const char*         s  = d.strings.data() + un.str_off;
            if (un.str_len >= 5U && std::strncmp(s, "data:", 5) == 0) // base64 data-URI
            {
                const char* marker = ";base64,";
                const char* found  = nullptr;
                for (crd::u32 k = 0; k + 8U <= un.str_len; ++k)
                {
                    if (std::strncmp(s + k, marker, 8) == 0)
                    {
                        found = s + k + 8;
                        break;
                    }
                }
                if (found == nullptr) { return ImportStatus::Malformed; }
                if (!b64_decode(found, static_cast<crd::usize>((s + un.str_len) - found), img.bytes))
                {
                    return ImportStatus::Malformed;
                }
            }
            else { percent_decode_append(s, un.str_len, img.uri); } // external file — the cook resolves + reads it
        }
        else { return ImportStatus::Malformed; } // spec: an image is bufferView-backed or uri-backed, never neither
        out.images.push_back(static_cast<ImportedImage&&>(img));
    }
    return ImportStatus::Ok;
}

// textures[i].source → image index (-1 when the texture carries no core source, e.g. basisu-only — caller warns)
[[nodiscard]] crd::i32 texture_image_index(const js::JsonDoc& d, crd::u32 root, crd::i64 tex_index)
{
    const crd::u32 j_texs = js::find(d, root, "textures");
    const crd::u32 jt     = js::at(d, j_texs, static_cast<crd::u32>(tex_index));
    if (jt == js::kInvalid) { return -1; }
    return static_cast<crd::i32>(js::as_i64(d, js::find(d, jt, "source"), -1));
}

// one material texture slot: {"index": t, "texCoord": n, ...} → image index; texCoord != 0 warns (uv0-only today)
[[nodiscard]] crd::i32 parse_tex_slot(const GltfCtx& g, crd::u32 j_slot, ImportedAsset& out)
{
    if (j_slot == js::kInvalid) { return -1; }
    const js::JsonDoc& d   = *g.doc;
    const crd::i64     tex = js::as_i64(d, js::find(d, j_slot, "index"), -1);
    if (tex < 0) { return -1; }
    if (js::as_i64(d, js::find(d, j_slot, "texCoord"), 0) != 0) { ++out.warning_count; }
    const crd::i32 img = texture_image_index(d, g.root, tex);
    if (img < 0 || img >= static_cast<crd::i32>(out.images.size()))
    {
        ++out.warning_count; // a texture without a resolvable core image (extension-only) — slot stays empty
        return -1;
    }
    return img;
}

// ── materials ───────────────────────────────────────────────────────────────────────────────────────────────────────────

void parse_materials(const GltfCtx& g, ImportedAsset& out)
{
    const js::JsonDoc& d      = *g.doc;
    const crd::u32     j_mats = js::find(d, g.root, "materials");
    const crd::u32     n      = js::count_of(d, j_mats);
    for (crd::u32 i = 0; i < n; ++i)
    {
        const crd::u32   jm = js::at(d, j_mats, i);
        ImportedMaterial m(g.alloc);
        char             name[256];
        (void)js::str_value(d, js::find(d, jm, "name"), name, sizeof(name));
        m.name.append(name);
        const crd::u32 pbr = js::find(d, jm, "pbrMetallicRoughness");
        if (pbr != js::kInvalid)
        {
            const crd::u32 bc = js::find(d, pbr, "baseColorFactor");
            if (bc != js::kInvalid)
            {
                m.base_color.x = static_cast<crd::f32>(js::as_f64(d, js::at(d, bc, 0), 1.0));
                m.base_color.y = static_cast<crd::f32>(js::as_f64(d, js::at(d, bc, 1), 1.0));
                m.base_color.z = static_cast<crd::f32>(js::as_f64(d, js::at(d, bc, 2), 1.0));
                m.base_alpha   = static_cast<crd::f32>(js::as_f64(d, js::at(d, bc, 3), 1.0));
            }
            m.metallic  = static_cast<crd::f32>(js::as_f64(d, js::find(d, pbr, "metallicFactor"), 1.0));
            m.roughness = static_cast<crd::f32>(js::as_f64(d, js::find(d, pbr, "roughnessFactor"), 1.0));
            m.base_color_image = parse_tex_slot(g, js::find(d, pbr, "baseColorTexture"), out);
            m.mr_image         = parse_tex_slot(g, js::find(d, pbr, "metallicRoughnessTexture"), out);
        }
        const crd::u32 jn = js::find(d, jm, "normalTexture");
        m.normal_image    = parse_tex_slot(g, jn, out);
        if (jn != js::kInvalid) { m.normal_scale = static_cast<crd::f32>(js::as_f64(d, js::find(d, jn, "scale"), 1.0)); }
        const crd::u32 jo  = js::find(d, jm, "occlusionTexture");
        m.occlusion_image  = parse_tex_slot(g, jo, out);
        if (jo != js::kInvalid)
        {
            m.occlusion_strength = static_cast<crd::f32>(js::as_f64(d, js::find(d, jo, "strength"), 1.0));
        }
        m.emissive_image = parse_tex_slot(g, js::find(d, jm, "emissiveTexture"), out);
        const crd::u32 em = js::find(d, jm, "emissiveFactor");
        if (em != js::kInvalid)
        {
            m.emissive.x = static_cast<crd::f32>(js::as_f64(d, js::at(d, em, 0), 0.0));
            m.emissive.y = static_cast<crd::f32>(js::as_f64(d, js::at(d, em, 1), 0.0));
            m.emissive.z = static_cast<crd::f32>(js::as_f64(d, js::at(d, em, 2), 0.0));
        }
        const crd::u32 ext = js::find(d, jm, "extensions");
        if (ext != js::kInvalid) // the KHR set that maps 1:1 onto the OpenPBR slab
        {
            const crd::u32 es = js::find(d, ext, "KHR_materials_emissive_strength");
            if (es != js::kInvalid)
            {
                m.emissive_strength = static_cast<crd::f32>(js::as_f64(d, js::find(d, es, "emissiveStrength"), 1.0));
            }
            const crd::u32 ji = js::find(d, ext, "KHR_materials_ior");
            if (ji != js::kInvalid) { m.ior = static_cast<crd::f32>(js::as_f64(d, js::find(d, ji, "ior"), 1.5)); }
            const crd::u32 jt = js::find(d, ext, "KHR_materials_transmission");
            if (jt != js::kInvalid)
            {
                m.transmission = static_cast<crd::f32>(js::as_f64(d, js::find(d, jt, "transmissionFactor"), 0.0));
            }
        }
        out.materials.push_back(static_cast<ImportedMaterial&&>(m));
    }
}

// ── meshes (one ImportedMesh per triangle primitive) ────────────────────────────────────────────────────────────────────

[[nodiscard]] ImportStatus parse_meshes(const GltfCtx& g, ImportedAsset& out)
{
    const js::JsonDoc& d        = *g.doc;
    const crd::u32     j_meshes = js::find(d, g.root, "meshes");
    const crd::u32     nm       = js::count_of(d, j_meshes);
    if (nm == 0U) { return ImportStatus::Malformed; } // a mesh cook of a meshless file fails honestly

    crd::containers::Array<crd::f32> scratch(g.alloc);
    for (crd::u32 mi = 0; mi < nm; ++mi)
    {
        const crd::u32 jm = js::at(d, j_meshes, mi);
        char           mesh_name[256];
        (void)js::str_value(d, js::find(d, jm, "name"), mesh_name, sizeof(mesh_name));
        const crd::u32 j_prims = js::find(d, jm, "primitives");
        const crd::u32 np      = js::count_of(d, j_prims);
        for (crd::u32 pi = 0; pi < np; ++pi)
        {
            const crd::u32 jp   = js::at(d, j_prims, pi);
            const crd::i64 mode = js::as_i64(d, js::find(d, jp, "mode"), 4);
            if (mode != 4) // TRIANGLES only — points/lines/strips/fans skip with a warning (strip/fan: widen on demand)
            {
                ++out.warning_count;
                continue;
            }
            const crd::u32 j_attr = js::find(d, jp, "attributes");
            const crd::u32 j_pos  = js::find(d, j_attr, "POSITION");
            if (j_pos == js::kInvalid) { return ImportStatus::Malformed; } // POSITION is mandatory per spec

            ImportedMesh mesh(g.alloc);
            mesh.name.append(mesh_name);
            mesh.source_mesh = static_cast<crd::i32>(mi); // the library index nodes reference (stage 3)
            if (np > 1U) // disambiguate multi-primitive meshes
            {
                char suffix[16];
                suffix[0] = '_';
                suffix[1] = 'p';
                suffix[2] = static_cast<char>('0' + (pi % 10U));
                suffix[3] = '\0';
                mesh.name.append(suffix);
            }
            mesh.material = static_cast<crd::i32>(js::as_i64(d, js::find(d, jp, "material"), -1));
            if (mesh.material >= static_cast<crd::i32>(out.materials.size())) { return ImportStatus::Malformed; }

            crd::u32     vc = 0;
            ImportStatus st = read_accessor_f32(g, js::as_i64(d, j_pos, 0), 3, scratch, vc);
            if (st != ImportStatus::Ok) { return st; }
            mesh.positions.reserve(vc);
            for (crd::u32 v = 0; v < vc; ++v)
            {
                const crd::f32 x = scratch[v * 3U + 0U];
                const crd::f32 y = scratch[v * 3U + 1U];
                const crd::f32 z = scratch[v * 3U + 2U];
                if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) { return ImportStatus::NonFiniteData; }
                mesh.positions.push_back(crd::math::Vec3<crd::f32>{x, y, z});
            }
            const crd::u32 j_nrm = js::find(d, j_attr, "NORMAL");
            if (j_nrm != js::kInvalid)
            {
                crd::u32 nc = 0;
                st          = read_accessor_f32(g, js::as_i64(d, j_nrm, 0), 3, scratch, nc);
                if (st != ImportStatus::Ok) { return st; }
                if (nc != vc) { return ImportStatus::Malformed; }
                for (crd::u32 v = 0; v < vc; ++v)
                {
                    mesh.normals.push_back(
                        crd::math::Vec3<crd::f32>{scratch[v * 3U], scratch[v * 3U + 1U], scratch[v * 3U + 2U]});
                }
            }
            const crd::u32 j_uv = js::find(d, j_attr, "TEXCOORD_0");
            if (j_uv != js::kInvalid)
            {
                crd::u32 tc = 0;
                st          = read_accessor_f32(g, js::as_i64(d, j_uv, 0), 2, scratch, tc);
                if (st != ImportStatus::Ok) { return st; }
                if (tc != vc) { return ImportStatus::Malformed; }
                for (crd::u32 v = 0; v < vc; ++v)
                {
                    mesh.uv0.push_back(crd::math::Vec2<crd::f32>{scratch[v * 2U], scratch[v * 2U + 1U]});
                }
            }
            const crd::u32 j_tan = js::find(d, j_attr, "TANGENT"); // AUTHORED tangents import as-is (vec4, w=±1)
            if (j_tan != js::kInvalid)
            {
                crd::u32 tc = 0;
                st          = read_accessor_f32(g, js::as_i64(d, j_tan, 0), 4, scratch, tc);
                if (st != ImportStatus::Ok) { return st; }
                if (tc != vc) { return ImportStatus::Malformed; }
                for (crd::u32 v = 0; v < vc; ++v)
                {
                    mesh.tangent.push_back(crd::math::Vec4<crd::f32>{scratch[v * 4U], scratch[v * 4U + 1U],
                                                                     scratch[v * 4U + 2U], scratch[v * 4U + 3U]});
                }
            }
            const crd::u32 j_idx = js::find(d, jp, "indices");
            if (j_idx != js::kInvalid)
            {
                crd::u32 icount = 0;
                st              = read_accessor_f32(g, js::as_i64(d, j_idx, 0), 1, scratch, icount);
                if (st != ImportStatus::Ok) { return st; }
                if ((icount % 3U) != 0U) { return ImportStatus::Malformed; }
                mesh.indices.reserve(icount);
                for (crd::u32 i = 0; i < icount; ++i)
                {
                    const crd::i64 idx = static_cast<crd::i64>(scratch[i]);
                    if (idx < 0 || idx >= static_cast<crd::i64>(vc)) { return ImportStatus::Malformed; }
                    mesh.indices.push_back(static_cast<crd::u32>(idx));
                }
            }
            else // non-indexed: identity per spec
            {
                if ((vc % 3U) != 0U) { return ImportStatus::Malformed; }
                for (crd::u32 i = 0; i < vc; ++i) { mesh.indices.push_back(i); }
            }
            if (!mesh.is_consistent()) { return ImportStatus::Malformed; }
            out.meshes.push_back(static_cast<ImportedMesh&&>(mesh));
        }
    }
    return ImportStatus::Ok;
}

// ── the scene graph (GEO-3 stage 3: nodes + cameras + lights + the default scene's roots) ──────────────────────────────


void parse_cameras(const GltfCtx& g, ImportedAsset& out)
{
    const js::JsonDoc& d      = *g.doc;
    const crd::u32     j_cams = js::find(d, g.root, "cameras");
    const crd::u32     n      = js::count_of(d, j_cams);
    for (crd::u32 i = 0; i < n; ++i)
    {
        const crd::u32 jc = js::at(d, j_cams, i);
        ImportedCamera cam(g.alloc);
        char           name[256];
        (void)js::str_value(d, js::find(d, jc, "name"), name, sizeof(name));
        cam.name.append(name);
        const crd::u32 jp = js::find(d, jc, "perspective");
        const crd::u32 jo = js::find(d, jc, "orthographic");
        if (jo != js::kInvalid)
        {
            cam.is_ortho = true;
            cam.xmag     = static_cast<crd::f32>(js::as_f64(d, js::find(d, jo, "xmag"), 1.0));
            cam.ymag     = static_cast<crd::f32>(js::as_f64(d, js::find(d, jo, "ymag"), 1.0));
            cam.znear    = static_cast<crd::f32>(js::as_f64(d, js::find(d, jo, "znear"), 0.1));
            cam.zfar     = static_cast<crd::f32>(js::as_f64(d, js::find(d, jo, "zfar"), 0.0));
        }
        else if (jp != js::kInvalid)
        {
            cam.yfov   = static_cast<crd::f32>(js::as_f64(d, js::find(d, jp, "yfov"), 1.0));
            cam.aspect = static_cast<crd::f32>(js::as_f64(d, js::find(d, jp, "aspectRatio"), 0.0));
            cam.znear  = static_cast<crd::f32>(js::as_f64(d, js::find(d, jp, "znear"), 0.1));
            cam.zfar   = static_cast<crd::f32>(js::as_f64(d, js::find(d, jp, "zfar"), 0.0)); // absent = infinite
        }
        else { ++out.warning_count; } // typeless camera — imported with defaults
        out.cameras.push_back(static_cast<ImportedCamera&&>(cam));
    }
}

void parse_lights(const GltfCtx& g, ImportedAsset& out)
{
    const js::JsonDoc& d    = *g.doc;
    const crd::u32     ext  = js::find(d, g.root, "extensions");
    const crd::u32     khr  = js::find(d, ext, "KHR_lights_punctual");
    const crd::u32     jarr = js::find(d, khr, "lights");
    const crd::u32     n    = js::count_of(d, jarr);
    for (crd::u32 i = 0; i < n; ++i)
    {
        const crd::u32 jl = js::at(d, jarr, i);
        ImportedLight  light(g.alloc);
        char           name[256];
        (void)js::str_value(d, js::find(d, jl, "name"), name, sizeof(name));
        light.name.append(name);
        const crd::u32 jt = js::find(d, jl, "type");
        if (js::str_value_eq(d, jt, "point")) { light.type = 1; }
        else if (js::str_value_eq(d, jt, "spot")) { light.type = 2; }
        else { light.type = 0; } // directional (the spec default reading)
        const crd::u32 jcol = js::find(d, jl, "color");
        if (jcol != js::kInvalid)
        {
            light.color.x = static_cast<crd::f32>(js::as_f64(d, js::at(d, jcol, 0), 1.0));
            light.color.y = static_cast<crd::f32>(js::as_f64(d, js::at(d, jcol, 1), 1.0));
            light.color.z = static_cast<crd::f32>(js::as_f64(d, js::at(d, jcol, 2), 1.0));
        }
        light.intensity = static_cast<crd::f32>(js::as_f64(d, js::find(d, jl, "intensity"), 1.0));
        light.range     = static_cast<crd::f32>(js::as_f64(d, js::find(d, jl, "range"), 0.0));
        const crd::u32 jspot = js::find(d, jl, "spot");
        if (jspot != js::kInvalid)
        {
            light.inner_cone = static_cast<crd::f32>(js::as_f64(d, js::find(d, jspot, "innerConeAngle"), 0.0));
            light.outer_cone =
                static_cast<crd::f32>(js::as_f64(d, js::find(d, jspot, "outerConeAngle"), 0.78539816339));
        }
        out.lights.push_back(static_cast<ImportedLight&&>(light));
    }
}

[[nodiscard]] ImportStatus parse_nodes_and_scene(const GltfCtx& g, ImportedAsset& out)
{
    const js::JsonDoc& d       = *g.doc;
    const crd::u32     j_nodes = js::find(d, g.root, "nodes");
    const crd::u32     n       = js::count_of(d, j_nodes);
    for (crd::u32 i = 0; i < n; ++i)
    {
        const crd::u32 jn = js::at(d, j_nodes, i);
        ImportedNode   node(g.alloc);
        char           name[256];
        (void)js::str_value(d, js::find(d, jn, "name"), name, sizeof(name));
        node.name.append(name);

        const crd::u32 jmat = js::find(d, jn, "matrix");
        if (jmat != js::kInvalid) // matrix XOR trs per spec — matrix wins if both appear (and we warn)
        {
            crd::f64 m[16];
            for (crd::u32 k = 0; k < 16U; ++k) { m[k] = js::as_f64(d, js::at(d, jmat, k), (k % 5U == 0U) ? 1.0 : 0.0); }
            if (!decompose_matrix_trs(m, node.translation, node.rotation, node.scale)) { ++out.warning_count; }
            if (js::find(d, jn, "translation") != js::kInvalid || js::find(d, jn, "rotation") != js::kInvalid
                || js::find(d, jn, "scale") != js::kInvalid)
            {
                ++out.warning_count;
            }
        }
        else
        {
            const crd::u32 jt = js::find(d, jn, "translation");
            if (jt != js::kInvalid)
            {
                node.translation.x = static_cast<crd::f32>(js::as_f64(d, js::at(d, jt, 0), 0.0));
                node.translation.y = static_cast<crd::f32>(js::as_f64(d, js::at(d, jt, 1), 0.0));
                node.translation.z = static_cast<crd::f32>(js::as_f64(d, js::at(d, jt, 2), 0.0));
            }
            const crd::u32 jr = js::find(d, jn, "rotation");
            if (jr != js::kInvalid)
            {
                node.rotation.x = static_cast<crd::f32>(js::as_f64(d, js::at(d, jr, 0), 0.0));
                node.rotation.y = static_cast<crd::f32>(js::as_f64(d, js::at(d, jr, 1), 0.0));
                node.rotation.z = static_cast<crd::f32>(js::as_f64(d, js::at(d, jr, 2), 0.0));
                node.rotation.w = static_cast<crd::f32>(js::as_f64(d, js::at(d, jr, 3), 1.0));
            }
            const crd::u32 jsc = js::find(d, jn, "scale");
            if (jsc != js::kInvalid)
            {
                node.scale.x = static_cast<crd::f32>(js::as_f64(d, js::at(d, jsc, 0), 1.0));
                node.scale.y = static_cast<crd::f32>(js::as_f64(d, js::at(d, jsc, 1), 1.0));
                node.scale.z = static_cast<crd::f32>(js::as_f64(d, js::at(d, jsc, 2), 1.0));
            }
        }

        node.mesh   = static_cast<crd::i32>(js::as_i64(d, js::find(d, jn, "mesh"), -1));
        node.camera = static_cast<crd::i32>(js::as_i64(d, js::find(d, jn, "camera"), -1));
        const crd::u32 jext = js::find(d, jn, "extensions");
        const crd::u32 jkhr = js::find(d, jext, "KHR_lights_punctual");
        if (jkhr != js::kInvalid) { node.light = static_cast<crd::i32>(js::as_i64(d, js::find(d, jkhr, "light"), -1)); }
        if (node.camera >= static_cast<crd::i32>(out.cameras.size())) { return ImportStatus::Malformed; }
        if (node.light >= static_cast<crd::i32>(out.lights.size())) { return ImportStatus::Malformed; }

        const crd::u32 jch = js::find(d, jn, "children");
        const crd::u32 nc  = js::count_of(d, jch);
        for (crd::u32 k = 0; k < nc; ++k)
        {
            const crd::i64 ci = js::as_i64(d, js::at(d, jch, k), -1);
            if (ci < 0 || ci >= static_cast<crd::i64>(n)) { return ImportStatus::Malformed; }
            node.children.push_back(static_cast<crd::u32>(ci));
        }
        out.nodes.push_back(static_cast<ImportedNode&&>(node));
    }

    // mesh references validate against the LIBRARY count (the meshes[] array length, not the primitive fan-out)
    const crd::u32 n_lib = js::count_of(d, js::find(d, g.root, "meshes"));
    for (crd::usize i = 0; i < out.nodes.size(); ++i)
    {
        if (out.nodes[i].mesh >= static_cast<crd::i32>(n_lib)) { return ImportStatus::Malformed; }
    }

    // the default scene's roots; scene-less files fall back to parentless nodes (legal per spec)
    const crd::u32 j_scenes = js::find(d, g.root, "scenes");
    const crd::u32 ns       = js::count_of(d, j_scenes);
    if (ns > 0U)
    {
        const crd::i64 def = js::as_i64(d, js::find(d, g.root, "scene"), 0);
        if (def < 0 || def >= static_cast<crd::i64>(ns)) { return ImportStatus::Malformed; }
        const crd::u32 jroots = js::find(d, js::at(d, j_scenes, static_cast<crd::u32>(def)), "nodes");
        const crd::u32 nr     = js::count_of(d, jroots);
        for (crd::u32 k = 0; k < nr; ++k)
        {
            const crd::i64 ri = js::as_i64(d, js::at(d, jroots, k), -1);
            if (ri < 0 || ri >= static_cast<crd::i64>(n)) { return ImportStatus::Malformed; }
            out.roots.push_back(static_cast<crd::u32>(ri));
        }
    }
    else if (n > 0U)
    {
        crd::containers::Array<crd::u8> has_parent(g.alloc);
        has_parent.resize(n, 0);
        for (crd::usize i = 0; i < out.nodes.size(); ++i)
        {
            for (crd::usize k = 0; k < out.nodes[i].children.size(); ++k)
            {
                has_parent[out.nodes[i].children[k]] = 1;
            }
        }
        for (crd::u32 i = 0; i < n; ++i)
        {
            if (has_parent[i] == 0U) { out.roots.push_back(i); }
        }
    }
    return ImportStatus::Ok;
}

[[nodiscard]] ImportStatus parse_document(crd::containers::ConstSpan<crd::u8> json_bytes,
                                          crd::containers::ConstSpan<crd::u8> bin, crd::memory::IAllocator* alloc,
                                          ImportedAsset& out)
{
    js::JsonDoc doc(alloc);
    if (!js::parse(json_bytes, doc)) { return ImportStatus::Malformed; }
    const crd::u32 root = doc.root;
    if (js::find(doc, root, "asset") == js::kInvalid) { return ImportStatus::NotRecognized; } // every glTF has `asset`

    crd::containers::Array<crd::u8> decoded0(alloc);
    GltfCtx                         g{&doc,
              root,
              js::find(doc, root, "buffers"),
              js::find(doc, root, "bufferViews"),
              js::find(doc, root, "accessors"),
              bin,
              &decoded0,
              alloc};

    // buffers: buffer 0 = BIN chunk / caller's external / an embedded data-URI; anything more → honest Malformed
    const crd::u32 nbuf = js::count_of(doc, g.j_buffers);
    if (nbuf > 1U) { return ImportStatus::Malformed; }
    if (nbuf == 1U)
    {
        const crd::u32 jb  = js::at(doc, g.j_buffers, 0);
        const crd::u32 uri = js::find(doc, jb, "uri");
        if (uri != js::kInvalid)
        {
            char head[64];
            (void)js::str_value(doc, uri, head, sizeof(head));
            const char* marker = ";base64,";
            if (std::strncmp(head, "data:", 5) == 0)
            {
                // decode the full URI value from the string pool (str_value truncates; use the pool directly)
                const js::JsonNode& un    = doc.nodes[uri];
                const char*         s     = doc.strings.data() + un.str_off;
                const char*         found = nullptr;
                for (crd::u32 i = 0; i + 8U <= un.str_len; ++i)
                {
                    if (std::strncmp(s + i, marker, 8) == 0)
                    {
                        found = s + i + 8;
                        break;
                    }
                }
                if (found == nullptr) { return ImportStatus::Malformed; }
                if (!b64_decode(found, static_cast<crd::usize>((s + un.str_len) - found), decoded0))
                {
                    return ImportStatus::Malformed;
                }
            }
            else
            {
                // a plain-file uri: REPORT it (percent-decoded) so the caller can resolve the ACTUAL reference —
                // the GEO-6 uri-general case (the cook reads it through the declared-input seam, stem fallback dies)
                const js::JsonNode& un = doc.nodes[uri];
                out.buffer_uri.clear();
                percent_decode_append(doc.strings.data() + un.str_off, un.str_len, out.buffer_uri);
                if (bin.size() == 0U)
                {
                    return ImportStatus::Malformed; // an external .bin the caller did not supply
                }
            }
        }
    }

    const ImportStatus ist = parse_images(g, out); // before materials: the slots validate against the image library
    if (ist != ImportStatus::Ok) { return ist; }
    parse_materials(g, out);
    parse_cameras(g, out); // before nodes: node camera/light refs validate against the libraries
    parse_lights(g, out);
    const ImportStatus nst = parse_nodes_and_scene(g, out);
    if (nst != ImportStatus::Ok) { return nst; }
    return parse_meshes(g, out);
}

} // namespace

ImportStatus parse_glb(crd::containers::ConstSpan<crd::u8> bytes, crd::memory::IAllocator* alloc, ImportedAsset& out)
{
    // 12-byte header: magic 'glTF' · version 2 · total length; then chunks: u32 len · u32 type ('JSON' / 'BIN\0')
    if (bytes.size() < 12U) { return ImportStatus::NotRecognized; }
    crd::u32 magic   = 0;
    crd::u32 version = 0;
    crd::u32 total   = 0;
    std::memcpy(&magic, bytes.data(), 4);
    std::memcpy(&version, bytes.data() + 4, 4);
    std::memcpy(&total, bytes.data() + 8, 4);
    if (magic != 0x46546C67U) { return ImportStatus::NotRecognized; } // 'glTF'
    if (version != 2U) { return ImportStatus::Malformed; }
    if (total > bytes.size()) { return ImportStatus::Truncated; }

    crd::containers::ConstSpan<crd::u8> json_span;
    crd::containers::ConstSpan<crd::u8> bin_span;
    crd::usize                          off = 12;
    while (off + 8U <= total)
    {
        crd::u32 clen  = 0;
        crd::u32 ctype = 0;
        std::memcpy(&clen, bytes.data() + off, 4);
        std::memcpy(&ctype, bytes.data() + off + 4, 4);
        off += 8;
        if (off + clen > total) { return ImportStatus::Truncated; }
        if (ctype == 0x4E4F534AU) { json_span = crd::containers::ConstSpan<crd::u8>(bytes.data() + off, clen); } // 'JSON'
        else if (ctype == 0x004E4942U) { bin_span = crd::containers::ConstSpan<crd::u8>(bytes.data() + off, clen); } // 'BIN'
        off += clen + ((4U - (clen % 4U)) % 4U); // chunks are 4-byte aligned
    }
    if (json_span.size() == 0U) { return ImportStatus::Malformed; }
    return parse_document(json_span, bin_span, alloc, out);
}

ImportStatus parse_gltf(crd::containers::ConstSpan<crd::u8> json_bytes, crd::containers::ConstSpan<crd::u8> external_bin,
                        crd::memory::IAllocator* alloc, ImportedAsset& out)
{
    return parse_document(json_bytes, external_bin, alloc, out);
}

} // namespace crd::assetio
