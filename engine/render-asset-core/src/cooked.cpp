#include <crd/renderasset/cooked.hpp>

#include <crd/containers/hash.hpp> // fnv1a_64

namespace crd::renderasset
{
namespace
{
// Little-endian field writers/readers — the cooked bytes are byte-identical across compilers/platforms (never a
// struct memcpy: padding is not in the blob). ⛔ struct-padding-in-content-hash scar.
void put_u32(u8* d, u32 v) noexcept
{
    d[0] = static_cast<u8>(v);
    d[1] = static_cast<u8>(v >> 8);
    d[2] = static_cast<u8>(v >> 16);
    d[3] = static_cast<u8>(v >> 24);
}
void put_u64(u8* d, u64 v) noexcept
{
    for (u32 i = 0; i < 8; ++i)
    {
        d[i] = static_cast<u8>(v >> (8U * i));
    }
}
u32 get_u32(const u8* d) noexcept
{
    return static_cast<u32>(d[0]) | (static_cast<u32>(d[1]) << 8) | (static_cast<u32>(d[2]) << 16) |
           (static_cast<u32>(d[3]) << 24);
}
u64 get_u64(const u8* d) noexcept
{
    u64 v = 0;
    for (u32 i = 0; i < 8; ++i)
    {
        v |= static_cast<u64>(d[i]) << (8U * i);
    }
    return v;
}
} // namespace

InterfaceHash interface_hash_of(const void* bytes, usize n) noexcept
{
    return InterfaceHash{crd::containers::fnv1a_64(bytes, n)};
}

ContentHash content_hash_of(const void* bytes, usize n) noexcept
{
    return ContentHash{crd::containers::fnv1a_64(bytes, n)};
}

usize cooked_blob_header_size(u32 dependency_count) noexcept
{
    return kCookedHeaderBytes + static_cast<usize>(dependency_count) * kCookedDependencyBytes;
}

usize write_cooked_header(u8* dst, usize cap, const CookedHeader& h, const AssetId* deps) noexcept
{
    const usize need = cooked_blob_header_size(h.dependency_count);
    if (dst == nullptr || cap < need)
    {
        return 0;
    }
    usize o = 0;
    put_u32(dst + o, h.magic);
    o += 4;
    put_u32(dst + o, static_cast<u32>(h.type));
    o += 4;
    put_u32(dst + o, h.schema.value);
    o += 4;
    put_u64(dst + o, h.iface.value);
    o += 8;
    put_u64(dst + o, h.content.value);
    o += 8;
    put_u64(dst + o, h.id.value);
    o += 8;
    put_u32(dst + o, h.dependency_count);
    o += 4;
    for (u32 i = 0; i < h.dependency_count; ++i)
    {
        put_u64(dst + o, deps[i].value);
        o += 8;
    }
    return o;
}

bool read_cooked_header(const u8* src, usize size, AssetType expected_type, SchemaVersion expected_schema,
                        CookedHeader& out, Array<AssetId>& out_deps, DiagnosticList& diags)
{
    if (src == nullptr || size < kCookedHeaderBytes)
    {
        diags.error(DiagCode::TruncatedBlob, "cooked blob is shorter than its fixed header");
        return false;
    }
    usize o = 0;
    CookedHeader h;
    h.magic = get_u32(src + o);
    o += 4;
    if (h.magic != kCookedMagic)
    {
        diags.error(DiagCode::MalformedBlob, "cooked blob has a bad magic");
        return false;
    }
    h.type = static_cast<AssetType>(get_u32(src + o));
    o += 4;
    h.schema.value = get_u32(src + o);
    o += 4;
    h.iface.value = get_u64(src + o);
    o += 8;
    h.content.value = get_u64(src + o);
    o += 8;
    h.id.value = get_u64(src + o);
    o += 8;
    h.dependency_count = get_u32(src + o);
    o += 4;

    if (h.type != expected_type)
    {
        diags.emit(Severity::Error, DiagCode::TypeMismatch, "cooked asset type mismatch", {}, {},
                   asset_type_name(expected_type), asset_type_name(h.type));
        return false;
    }
    if (!(h.schema == expected_schema))
    {
        diags.error(DiagCode::SchemaMismatch, "cooked schema version mismatch");
        return false;
    }
    if (size < cooked_blob_header_size(h.dependency_count))
    {
        diags.error(DiagCode::TruncatedBlob, "cooked blob is shorter than its declared dependency list");
        return false;
    }
    out_deps.clear();
    for (u32 i = 0; i < h.dependency_count; ++i)
    {
        out_deps.push_back(AssetId{get_u64(src + o)});
        o += 8;
    }
    out = h;
    return true;
}
} // namespace crd::renderasset
