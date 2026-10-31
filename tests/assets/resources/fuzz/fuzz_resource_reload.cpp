// crd-fuzz-resources-reload: the cooked-pack ingress of crd-resources (DIAG.10a) under the bounded fuzz harness
// (REPO.DEV.9; docs/design/test-instruments.md). The production entry points take paths, so every pack an input
// names is written to a per-process file under the platform temp directory and handed to the real ResourceManager.
//
// Input. The first byte picks the mode. Even: the rest is one pack file as bytes -- crdr_read is run on it directly,
// then the file is mounted, its ids loaded, reloaded, evicted, loaded again and unmounted. Odd: the rest is a script
// over two pack files and one manager: write a pack built from a decoded spec (through CrdrWriter, then overridden:
// a lying range, a duplicated id, a torn tail, a dropped or compressed chunk), write raw bytes, corrupt a file in
// place, mount, unmount, load, release, reload_mount_now, drain to a zero budget, pin and unpin.
//
// Oracles, none of them the manager's own verdict:
//   * an independent restatement of the CRDR layout (its checksums included) and the pack rules decides whether
//     crdr_read accepts the bytes, where every chunk's payload lies (an uncompressed payload is a view at exactly
//     that offset; a compressed one is exactly what zstd decodes from it), and whether a pack mounts or reloads;
//   * crdr_read allocates at most eight times its input plus 64 KiB plus what its accepted claims decode to;
//   * a model of the manager: the live table is, for each id, the entry of the newest mount whose manifest names it
//     (find_entry's mount, type, flags, range and name equal it after every operation); a refused mount or reload
//     changes nothing; a reload swaps exactly the loaded payloads of its own type that the mount serves, bumping each
//     generation once and firing each subscriber with it; a re-issue after eviction reads the live range from the
//     file on disk and never crosses types; every payload's bytes are the file's bytes at the range it was loaded
//     from; current_memory_use() is the sum of the resident payloads' ranges;
//   * a payload only ever reaches its own type's loader, and the run returns every byte it allocated.
// Bounds: the compressed chunks an input claims may decode to at most 16 MiB in all (more is skipped as out of scope,
// never run), and at most 16 handles are held.
#include <crd/fuzz/harness.hpp>

#include <crd/containers/array.hpp>
#include <crd/containers/checksum.hpp>
#include <crd/containers/string.hpp>
#include <crd/log/log.hpp>
#include <crd/platform/filesystem.hpp>
#include <crd/resources/crdr.hpp>
#include <crd/resources/loader.hpp>
#include <crd/resources/resource_handle.hpp>
#include <crd/resources/resource_manager.hpp>

#include <zstd.h>

#include <cstdio>
#include <cstring>
#include <memory>
#include <new>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace
{
using crd::u16;
using crd::u32;
using crd::u64;
using crd::u8;
using crd::usize;
using crd::containers::Array;
using crd::containers::ConstSpan;
using crd::containers::String;
using crd::resources::LoadState;
using crd::resources::ResourceId;

namespace res = crd::resources;
namespace fs  = crd::platform::fs;

constexpr u32 kTypeA      = res::make_fourcc('F', 'Z', 'A', '0'); // loader without a placeholder
constexpr u32 kTypeB      = res::make_fourcc('F', 'Z', 'B', '0'); // loader with a placeholder
constexpr u32 kTypeC      = res::make_fourcc('F', 'Z', 'C', '0'); // no loader registered
constexpr u8  kRefuseByte = 0xEEU;                                // an artifact starting with it fails load()
constexpr usize kMaxHandles     = 16U;
constexpr usize kMaxRawUniverse = 16U;
constexpr u64   kMaxDecodedClaim = 16U * 1024U * 1024U;
constexpr usize kSlots          = 2U;
constexpr usize kFixedIds       = 8U;

// ── Little-endian helpers (the harness's own, not crdr.cpp's) ───────────────

[[nodiscard]] u32 get_u32(const u8* p) noexcept
{
    return static_cast<u32>(p[0]) | (static_cast<u32>(p[1]) << 8U) | (static_cast<u32>(p[2]) << 16U) |
           (static_cast<u32>(p[3]) << 24U);
}

[[nodiscard]] u64 get_u64(const u8* p) noexcept
{
    return static_cast<u64>(get_u32(p)) | (static_cast<u64>(get_u32(p + 4U)) << 32U);
}

void put_u32(Array<u8>& out, u32 v)
{
    for (u32 i = 0U; i < 4U; ++i)
    {
        out.push_back(static_cast<u8>(v >> (8U * i)));
    }
}

void put_u64(Array<u8>& out, u64 v)
{
    for (u32 i = 0U; i < 8U; ++i)
    {
        out.push_back(static_cast<u8>(v >> (8U * i)));
    }
}

void append(Array<u8>& out, const u8* data, usize size)
{
    for (usize i = 0U; i < size; ++i)
    {
        out.push_back(data[i]);
    }
}

[[nodiscard]] bool same_bytes(const Array<u8>& a, ConstSpan<u8> b) noexcept
{
    return crd::fuzz::bytes_equal(a.data(), a.size(), b.data(), b.size());
}

[[nodiscard]] ResourceId fixed_id(usize i) noexcept
{
    return ResourceId{0x1000U + i, 0xF00DU + i};
}

// ── The CRDR container and pack rules, restated ─────────────────────────────

struct ModelChunk
{
    u32       fourcc       = 0U;
    u32       flags        = 0U;
    u64       uncompressed = 0U;
    usize     offset       = 0U; // payload's first byte in the file
    u64       stored       = 0U;
    u64       hash         = 0U; // a compressed chunk's decoded XXH3-64
    u32       stored_check = 0U; // the stored bytes' CRC-32C
    Array<u8> decoded;           // a compressed chunk's decoded bytes
};

struct ModelContainer
{
    ResourceId        id{};
    u32               type   = 0U;
    u16               flags  = 0U;
    u64               claims = 0U; // the bytes the compressed chunks claim to decode to
    Array<ModelChunk> chunks;
};

// The CRC-32C of a 40-byte header's first 36 bytes, which its last four hold.
[[nodiscard]] u32 header_check(const u8* header) noexcept
{
    return crd::containers::crc32c(ConstSpan<u8>(header, 36U));
}

// The CRDR layout (crdr.hpp): a 40-byte header (version 3, its last word the CRC-32C of the 36 bytes before it),
// then chunk_count chunks of a 40-byte header (the same check; at +24 the XXH3-64 of a compressed chunk's decoded
// bytes, 0 for a stored one; at +32 the CRC-32C of the stored bytes) and a payload padded to 16 bytes; the padding
// after the last payload and any bytes after it (a pack's artifacts) are not part of it. A stored chunk holds exactly
// its claim. A compressed chunk claims at most what its stored bytes can hold (zstd: 3-byte block headers, 128 KiB
// per block) and must decode to exactly its claim and its hash. Returns false where the reader must refuse; `skip` is
// set when the claims exceed the target's bound (decided before any decoding, from the claims alone).
[[nodiscard]] bool model_container(const u8* data, usize size, ModelContainer& out, crd::memory::IAllocator* a,
                                   bool& skip)
{
    skip = false;
    if (size < 40U || get_u32(data) != res::kFourCC_CRDR || (static_cast<u16>(data[4] | (data[5] << 8U))) != 3U ||
        header_check(data) != get_u32(data + 36U))
    {
        return false;
    }
    out.flags        = static_cast<u16>(data[6] | (data[7] << 8U));
    out.id.hi        = get_u64(data + 8U);
    out.id.lo        = get_u64(data + 16U);
    out.type         = get_u32(data + 24U);
    const u32 count  = get_u32(data + 28U);
    usize     pos    = 40U;
    out.chunks       = Array<ModelChunk>(a);
    out.claims       = 0U;
    for (u32 i = 0U; i < count; ++i)
    {
        if (pos > size || size - pos < 40U || header_check(data + pos) != get_u32(data + pos + 36U))
        {
            return false;
        }
        ModelChunk chunk;
        chunk.decoded      = Array<u8>(a);
        chunk.fourcc       = get_u32(data + pos);
        chunk.flags        = get_u32(data + pos + 4U);
        chunk.uncompressed = get_u64(data + pos + 8U);
        chunk.stored       = get_u64(data + pos + 16U);
        chunk.hash         = get_u64(data + pos + 24U);
        chunk.stored_check = get_u32(data + pos + 32U);
        chunk.offset = pos + 40U;
        if (chunk.stored > size - chunk.offset)
        {
            return false;
        }
        if ((chunk.flags & 1U) != 0U)
        {
            const u64 blocks = chunk.stored / 3U;
            if (chunk.uncompressed > blocks * 131072U)
            {
                return false;
            }
            out.claims += chunk.uncompressed;
        }
        else if (chunk.uncompressed != chunk.stored || chunk.hash != 0U)
        {
            return false;
        }
        const usize stored = static_cast<usize>(chunk.stored);
        pos                = chunk.offset + stored + ((16U - (stored % 16U)) % 16U);
        out.chunks.push_back(std::move(chunk));
    }
    if (out.claims > kMaxDecodedClaim)
    {
        skip = true;
        return false;
    }
    for (ModelChunk& chunk : out.chunks)
    {
        if (crd::containers::crc32c(ConstSpan<u8>(data + chunk.offset, static_cast<usize>(chunk.stored))) !=
            chunk.stored_check)
        {
            return false;
        }
        if ((chunk.flags & 1U) == 0U)
        {
            continue;
        }
        chunk.decoded.resize(static_cast<usize>(chunk.uncompressed));
        const usize got = ZSTD_decompress(chunk.decoded.data(), chunk.decoded.size(), data + chunk.offset,
                                          static_cast<usize>(chunk.stored));
        if (ZSTD_isError(got) != 0U || got != chunk.decoded.size() ||
            crd::containers::xxh3_64(ConstSpan<u8>(chunk.decoded.data(), chunk.decoded.size())) != chunk.hash)
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] ConstSpan<u8> model_payload(const u8* data, const ModelChunk& chunk) noexcept
{
    if ((chunk.flags & 1U) != 0U)
    {
        return ConstSpan<u8>(chunk.decoded.data(), chunk.decoded.size());
    }
    return ConstSpan<u8>(data + chunk.offset, static_cast<usize>(chunk.stored));
}

struct ModelEntry
{
    ResourceId id{};
    u32        type   = 0U;
    u32        flags  = 0U;
    u64        offset = 0U;
    u64        size   = 0U;
    Array<u8>  name;
};

// A pack mounts or reloads when its container is valid, its type is PACK, its first MFST chunk holds whole 48-byte
// entries, every entry's range lies inside the file and no id is named twice. Names come from the first STRP chunk:
// the bytes from the entry's index to the first NUL or the pool's end, empty past the pool.
[[nodiscard]] bool model_pack(const Array<u8>& file, Array<ModelEntry>& entries, crd::memory::IAllocator* a,
                              bool& skip)
{
    entries = Array<ModelEntry>(a);
    ModelContainer container;
    container.chunks = Array<ModelChunk>(a);
    if (!model_container(file.data(), file.size(), container, a, skip) || container.type != res::kFourCC_PACK)
    {
        return false;
    }
    const ModelChunk* mfst = nullptr;
    const ModelChunk* strp = nullptr;
    for (const ModelChunk& chunk : container.chunks)
    {
        if (mfst == nullptr && chunk.fourcc == res::kFourCC_MFST)
        {
            mfst = &chunk;
        }
        if (strp == nullptr && chunk.fourcc == res::kFourCC_STRP)
        {
            strp = &chunk;
        }
    }
    if (mfst == nullptr)
    {
        return false;
    }
    const ConstSpan<u8> table = model_payload(file.data(), *mfst);
    const ConstSpan<u8> pool  = strp != nullptr ? model_payload(file.data(), *strp) : ConstSpan<u8>();
    if (table.size() % 48U != 0U)
    {
        return false;
    }
    const u64 file_size = file.size();
    for (usize at = 0U; at < table.size(); at += 48U)
    {
        ModelEntry e;
        e.name       = Array<u8>(a);
        e.id.hi      = get_u64(table.data() + at);
        e.id.lo      = get_u64(table.data() + at + 8U);
        e.type       = get_u32(table.data() + at + 16U);
        e.flags      = get_u32(table.data() + at + 20U);
        e.offset     = get_u64(table.data() + at + 24U);
        e.size       = get_u64(table.data() + at + 32U);
        const u32 ix = get_u32(table.data() + at + 40U);
        // the range, compared in 128 bits' worth of care: offset + size must not pass the end
        if (e.size > file_size || e.offset > file_size - e.size)
        {
            entries.clear();
            return false;
        }
        for (const ModelEntry& before : entries)
        {
            if (before.id == e.id)
            {
                entries.clear();
                return false;
            }
        }
        for (usize k = ix; k < pool.size() && pool[k] != 0U; ++k)
        {
            e.name.push_back(pool[k]);
        }
        entries.push_back(std::move(e));
    }
    return true;
}

// ── The test loaders ────────────────────────────────────────────────────────

struct FuzzPayload
{
    u32                      type        = 0U;
    bool                     placeholder = false;
    crd::memory::IAllocator* alloc       = nullptr;
    Array<u8>                bytes;
};

class FuzzLoader final : public res::ILoader
{
public:
    FuzzLoader(u32 type, bool has_placeholder) noexcept : m_type(type), m_has_placeholder(has_placeholder) {}

    [[nodiscard]] u32 type_fourcc() const noexcept override { return m_type; }
    [[nodiscard]] u32 loader_version() const noexcept override { return 1U; }

    [[nodiscard]] void* load(const res::LoadContext& ctx) override
    {
        if (ctx.bytes.empty() || ctx.bytes[0] == kRefuseByte)
        {
            return nullptr;
        }
        return make(ctx, false);
    }

    [[nodiscard]] void* load_placeholder(const res::LoadContext& ctx) override
    {
        return m_has_placeholder ? make(ctx, true) : nullptr;
    }

    void unload(void* payload) noexcept override
    {
        auto* p = static_cast<FuzzPayload*>(payload);
        // A payload only ever reaches its own type's loader (a handle's get() casts it to that type).
        CRD_FUZZ_REQUIRE(p->type == m_type);
        crd::memory::IAllocator* const a = p->alloc;
        p->~FuzzPayload();
        a->deallocate(p);
    }

private:
    [[nodiscard]] void* make(const res::LoadContext& ctx, bool placeholder) const
    {
        void* raw     = ctx.allocator->allocate(sizeof(FuzzPayload), alignof(FuzzPayload));
        auto* p       = new (raw) FuzzPayload();
        p->type        = m_type;
        p->placeholder = placeholder;
        p->alloc       = ctx.allocator;
        p->bytes       = Array<u8>(ctx.allocator);
        if (!placeholder)
        {
            append(p->bytes, ctx.bytes.data(), ctx.bytes.size());
        }
        return p;
    }

    u32  m_type;
    bool m_has_placeholder;
};

[[nodiscard]] bool has_loader(u32 type) noexcept
{
    return type == kTypeA || type == kTypeB;
}

// What the loader of `type` makes of `bytes`: Ready with a copy, Placeholder (type B) or Failed.
[[nodiscard]] LoadState model_load(u32 type, ConstSpan<u8> bytes) noexcept
{
    if (!bytes.empty() && bytes[0] != kRefuseByte)
    {
        return LoadState::Ready;
    }
    return type == kTypeB ? LoadState::Placeholder : LoadState::Failed;
}

// ── Pack files on disk ──────────────────────────────────────────────────────

struct Disk
{
    String    path;
    bool      exists = false;
    Array<u8> bytes;
};

[[nodiscard]] unsigned long process_token() noexcept
{
#if defined(_WIN32)
    return static_cast<unsigned long>(_getpid());
#else
    return static_cast<unsigned long>(getpid());
#endif
}

void write_disk(Disk& disk, const u8* data, usize size)
{
    disk.bytes.clear();
    append(disk.bytes, data, size);
    CRD_FUZZ_REQUIRE(fs::write_file_binary(fs::Path(disk.path.c_str()), ConstSpan<u8>(disk.bytes.data(), size)));
    disk.exists = true;
}

// ── The pack builder (seeds and the script's spec op) ───────────────────────

struct SpecEntry
{
    u8 id_index = 0U;
    u8 type     = 0U; // 0 A, 1 B, 2 C
    u8 length   = 0U;
    u8 fill     = 0U;
    u8 name     = 0U; // 0 short, 1 empty, 2 past the pool, 3 long
};

// The spec's override flags.
constexpr u8 kCompress      = 1U << 0U;
constexpr u8 kDropStrp      = 1U << 1U;
constexpr u8 kWrongType     = 1U << 2U;
constexpr u8 kRangeOverride = 1U << 3U;
constexpr u8 kDuplicateId   = 1U << 4U;
constexpr u8 kTruncate      = 1U << 5U;
constexpr u8 kTrailing      = 1U << 6U;
constexpr u8 kDropMfst      = 1U << 7U;

struct Spec
{
    u8        flags = 0U;
    SpecEntry entries[6]{};
    usize     count          = 0U;
    u8        override_entry = 0U;
    u8        override_field = 0U; // 0 offset, 1 size
    u64       override_value = 0U;
    u8        duplicate_of   = 0U; // entry that takes entry 0's id
    u8        cut            = 0U;
    u8        junk           = 0U;
};

[[nodiscard]] u32 spec_type(u8 t) noexcept
{
    if (t % 3U == 0U)
    {
        return kTypeA;
    }
    return t % 3U == 1U ? kTypeB : kTypeC;
}

void artifact_bytes(const SpecEntry& e, usize i, Array<u8>& out)
{
    for (usize j = 0U; j < e.length; ++j)
    {
        out.push_back(static_cast<u8>(e.fill + j * 31U + i * 7U));
    }
}

// Build a pack the way the cooker lays one out (container, then the artifacts in order), then apply the spec's
// overrides. The MFST table is encoded here, not through manifest_write, so the chunks can be compressed or dropped.
void build_pack(const Spec& spec, Array<u8>& out, crd::memory::IAllocator* a)
{
    Array<u8>  pool(a);
    Array<u32> name_index(a);
    Array<u8>  blobs(a);
    Array<u64> blob_at(a);
    for (usize i = 0U; i < spec.count; ++i)
    {
        const SpecEntry& e = spec.entries[i];
        blob_at.push_back(blobs.size());
        artifact_bytes(e, i, blobs);
        const u8 kind = static_cast<u8>(e.name % 4U);
        if (kind == 2U)
        {
            name_index.push_back(0xFFFFFFFFU);
            continue;
        }
        name_index.push_back(static_cast<u32>(pool.size()));
        if (kind == 0U || kind == 3U)
        {
            char text[48];
            const int n = std::snprintf(text, sizeof(text), kind == 0U ? "a%zu" : "assets/fuzz/reload/artifact-%zu.bin",
                                        static_cast<std::size_t>(i));
            append(pool, reinterpret_cast<const u8*>(text), static_cast<usize>(n));
        }
        pool.push_back(0U);
    }

    const auto encode = [&](u64 base, Array<u8>& container)
    {
        Array<u8> table(a);
        for (usize i = 0U; i < spec.count; ++i)
        {
            const SpecEntry& e          = spec.entries[i];
            const bool       duplicated = (spec.flags & kDuplicateId) != 0U && i != 0U &&
                                          i == spec.duplicate_of % spec.count;
            const ResourceId rid        = fixed_id((duplicated ? spec.entries[0].id_index : e.id_index) % kFixedIds);
            u64              offset     = base + blob_at[i];
            u64              size       = e.length;
            if ((spec.flags & kRangeOverride) != 0U && spec.override_entry % spec.count == i)
            {
                if (spec.override_field % 2U == 0U)
                {
                    offset = spec.override_value;
                }
                else
                {
                    size = spec.override_value;
                }
            }
            put_u64(table, rid.hi);
            put_u64(table, rid.lo);
            put_u32(table, spec_type(e.type));
            put_u32(table, 0U);
            put_u64(table, offset);
            put_u64(table, size);
            put_u32(table, name_index[i]);
            put_u32(table, 0U);
        }
        res::CrdrWriter writer(a, ResourceId{0xFACEU, 0xB00CU},
                               (spec.flags & kWrongType) != 0U ? res::kFourCC_BLOB : res::kFourCC_PACK);
        const auto add = [&](u32 fourcc, const Array<u8>& payload)
        {
            const ConstSpan<u8> span(payload.data(), payload.size());
            if ((spec.flags & kCompress) != 0U)
            {
                writer.add_chunk_compressed(fourcc, span);
            }
            else
            {
                writer.add_chunk(fourcc, span);
            }
        };
        if ((spec.flags & kDropMfst) == 0U)
        {
            add(res::kFourCC_MFST, table);
        }
        if ((spec.flags & kDropStrp) == 0U)
        {
            add(res::kFourCC_STRP, pool);
        }
        writer.add_chunk(res::kFourCC_DEPS, ConstSpan<u8>());
        container = writer.finish();
    };

    // The container's size can depend on the offsets it holds (compression), so lay out until it is stable.
    Array<u8> container(a);
    u64       base = 0U;
    for (int pass = 0; pass < 4; ++pass)
    {
        encode(base, container);
        if (container.size() == base)
        {
            break;
        }
        base = container.size();
    }
    out = Array<u8>(a);
    append(out, container.data(), container.size());
    append(out, blobs.data(), blobs.size());
    if ((spec.flags & kTruncate) != 0U)
    {
        const usize cut = static_cast<usize>(spec.cut) + 1U;
        out.resize(out.size() > cut ? out.size() - cut : 0U);
    }
    if ((spec.flags & kTrailing) != 0U)
    {
        for (u8 i = 0U; i < spec.junk; ++i)
        {
            out.push_back(0xA5U);
        }
    }
}

// ── Script decoding ─────────────────────────────────────────────────────────

class Reader
{
public:
    Reader(const u8* data, usize size) noexcept : m_data(data), m_size(size) {}
    [[nodiscard]] bool done() const noexcept { return m_at >= m_size; }
    [[nodiscard]] usize left() const noexcept { return m_at < m_size ? m_size - m_at : 0U; }
    [[nodiscard]] u8    byte() noexcept { return m_at < m_size ? m_data[m_at++] : 0U; }
    [[nodiscard]] u16   word() noexcept
    {
        const u8 lo = byte();
        return static_cast<u16>(lo | (byte() << 8U));
    }
    [[nodiscard]] u64 quad() noexcept
    {
        u64 v = 0U;
        for (u32 i = 0U; i < 8U; ++i)
        {
            v |= static_cast<u64>(byte()) << (8U * i);
        }
        return v;
    }
    [[nodiscard]] const u8* take(usize n) noexcept
    {
        const u8* p = m_data + m_at;
        m_at += n;
        return p;
    }

private:
    const u8* m_data;
    usize     m_size;
    usize     m_at = 0U;
};

[[nodiscard]] Spec decode_spec(Reader& in)
{
    Spec spec;
    spec.flags = in.byte();
    spec.count = in.byte() % 7U;
    for (usize i = 0U; i < spec.count; ++i)
    {
        spec.entries[i].id_index = in.byte();
        spec.entries[i].type     = in.byte();
        spec.entries[i].length   = static_cast<u8>(in.byte() % 48U);
        spec.entries[i].fill     = in.byte();
        spec.entries[i].name     = in.byte();
    }
    if (spec.count == 0U)
    {
        spec.flags = static_cast<u8>(spec.flags & ~(kRangeOverride | kDuplicateId));
    }
    if ((spec.flags & kRangeOverride) != 0U)
    {
        spec.override_entry = in.byte();
        spec.override_field = in.byte();
        spec.override_value = in.quad();
    }
    if ((spec.flags & kDuplicateId) != 0U)
    {
        spec.duplicate_of = in.byte();
    }
    if ((spec.flags & kTruncate) != 0U)
    {
        spec.cut = in.byte();
    }
    if ((spec.flags & kTrailing) != 0U)
    {
        spec.junk = in.byte();
    }
    return spec;
}

// ── The manager model and the run ───────────────────────────────────────────

struct ModelMount
{
    u32               mount_id = 0U;
    usize             slot     = 0U;
    Array<ModelEntry> entries;
};

struct ModelBlock
{
    ResourceId id{};
    u32        type        = 0U;
    LoadState  state       = LoadState::Unloaded;
    u32        generation  = 0U;
    u64        size        = 0U;
    bool       placeholder = false;
    Array<u8>  bytes;
};

struct Held
{
    res::ResourceHandle<FuzzPayload> handle;
    ResourceId                       id{};
    bool                             fresh_failed = false; // the manager's non-permanent Failed block
};

struct ReloadEvent
{
    ResourceId id{};
    u32        generation = 0U;
};

void on_reload(ResourceId id, u32 generation, void* user)
{
    static_cast<Array<ReloadEvent>*>(user)->push_back(ReloadEvent{id, generation});
}

class Run
{
public:
    explicit Run(crd::fuzz::BudgetAllocator& root, crd::memory::IAllocator* model_alloc)
        : m_model(model_alloc), m_rm(&root), m_mounts(model_alloc), m_blocks(model_alloc), m_held(model_alloc),
          m_universe(model_alloc), m_events(model_alloc), m_pins(model_alloc)
    {
        static unsigned long s_serial = 0U;
        ++s_serial;
        for (usize s = 0U; s < kSlots; ++s)
        {
            char name[96];
            std::snprintf(name, sizeof(name), "crd-fuzz-reload-%lu-%lu-%zu.pack", process_token(), s_serial,
                          static_cast<std::size_t>(s));
            const fs::Path path = fs::temp_directory() / crd::containers::StringView(name);
            m_disk[s].path      = String(path.generic().data(), path.generic().size(), model_alloc);
            m_disk[s].bytes     = Array<u8>(model_alloc);
            (void)fs::remove_file(path);
        }
        m_rm.register_loader(std::make_unique<FuzzLoader>(kTypeA, false));
        m_rm.register_loader(std::make_unique<FuzzLoader>(kTypeB, true));
    }

    Run(const Run&)            = delete;
    Run& operator=(const Run&) = delete;

    ~Run()
    {
        for (const Disk& disk : m_disk)
        {
            (void)fs::remove_file(fs::Path(disk.path.c_str()));
        }
    }

    void add_universe(ResourceId id)
    {
        for (const ResourceId& known : m_universe)
        {
            if (known == id)
            {
                return;
            }
        }
        m_universe.push_back(id);
        (void)m_rm.subscribe_reload(id, &on_reload, &m_events);
    }

    [[nodiscard]] Disk& disk(usize slot) noexcept { return m_disk[slot % kSlots]; }

    // Whether the model accepts the pack on disk in `slot`; `skip` when its claims are past the target's bound.
    [[nodiscard]] bool pack_on_disk(usize slot, Array<ModelEntry>& entries, bool& skip)
    {
        skip = false;
        entries = Array<ModelEntry>(m_model);
        return disk(slot).exists && model_pack(disk(slot).bytes, entries, m_model, skip);
    }

    // Returns false when the op was skipped as out of scope (the run stops).
    [[nodiscard]] bool mount(usize slot)
    {
        Array<ModelEntry> entries(m_model);
        bool              skip     = false;
        const bool        accepted = pack_on_disk(slot, entries, skip);
        if (skip)
        {
            return false;
        }
        const res::MountId got = m_rm.mount_manifest(crd::containers::StringView(disk(slot).path.c_str()));
        CRD_FUZZ_REQUIRE(got.is_valid() == accepted);
        if (accepted)
        {
            CRD_FUZZ_REQUIRE(got.value == m_next_mount);
            ModelMount m;
            m.mount_id = m_next_mount++;
            m.slot     = slot % kSlots;
            m.entries  = std::move(entries);
            m_mounts.push_back(std::move(m));
        }
        return true;
    }

    void unmount(u8 pick)
    {
        const usize k = pick % (m_mounts.size() + 1U);
        if (k == m_mounts.size())
        {
            m_rm.unmount(res::MountId{pick % 2U == 0U ? 0U : m_next_mount + 7U});
            return;
        }
        m_rm.unmount(res::MountId{m_mounts[k].mount_id});
        m_mounts.erase(k);
    }

    [[nodiscard]] const ModelEntry* live(ResourceId id, const ModelMount** owner = nullptr) const noexcept
    {
        const ModelEntry* best     = nullptr;
        u32               best_id  = 0U;
        for (const ModelMount& m : m_mounts)
        {
            for (const ModelEntry& e : m.entries)
            {
                if (e.id == id && (best == nullptr || m.mount_id > best_id))
                {
                    best    = &e;
                    best_id = m.mount_id;
                    if (owner != nullptr)
                    {
                        *owner = &m;
                    }
                }
            }
        }
        return best;
    }

    [[nodiscard]] ModelBlock* block(ResourceId id) noexcept
    {
        for (ModelBlock& b : m_blocks)
        {
            if (b.id == id)
            {
                return &b;
            }
        }
        return nullptr;
    }

    // The bytes a load reads: the live range of the serving mount's pack as it is on disk now.
    [[nodiscard]] bool read_range(const ModelMount& m, const ModelEntry& e, ConstSpan<u8>& out) const noexcept
    {
        const Disk& d = m_disk[m.slot];
        if (!d.exists)
        {
            return false;
        }
        if (e.size == 0U)
        {
            out = ConstSpan<u8>();
            return true;
        }
        if (e.offset > d.bytes.size() || e.size > d.bytes.size() - e.offset)
        {
            return false;
        }
        out = ConstSpan<u8>(d.bytes.data() + e.offset, static_cast<usize>(e.size));
        return true;
    }

    void set_block(ModelBlock& b, LoadState state, u64 size, ConstSpan<u8> bytes)
    {
        b.state       = state;
        b.size        = size;
        b.placeholder = state == LoadState::Placeholder;
        b.bytes.clear();
        if (state == LoadState::Ready)
        {
            append(b.bytes, bytes.data(), bytes.size());
        }
    }

    void load(ResourceId id)
    {
        if (m_held.size() >= kMaxHandles)
        {
            return;
        }
        Held held;
        held.id     = id;
        held.handle = m_rm.load_sync<FuzzPayload>(id);
        ModelBlock* b = block(id);
        if (b != nullptr && b->state != LoadState::Unloaded)
        {
            m_held.push_back(std::move(held));
            return;
        }
        const ModelMount* owner = nullptr;
        const ModelEntry* e     = live(id, &owner);
        ConstSpan<u8>     bytes;
        if (e == nullptr || !has_loader(e->type) || (b != nullptr && b->type != e->type))
        {
            held.fresh_failed = true;
        }
        else if (!read_range(*owner, *e, bytes))
        {
            if (b != nullptr)
            {
                set_block(*b, LoadState::Failed, 0U, bytes); // a failed re-issue keeps the block, Failed
            }
            else
            {
                held.fresh_failed = true;
            }
        }
        else
        {
            const LoadState state = model_load(e->type, bytes);
            if (state == LoadState::Failed)
            {
                if (b != nullptr)
                {
                    set_block(*b, LoadState::Failed, 0U, bytes);
                }
                else
                {
                    held.fresh_failed = true;
                }
            }
            else if (b != nullptr)
            {
                set_block(*b, state, e->size, bytes);
                b->generation += 1U;
            }
            else
            {
                ModelBlock fresh;
                fresh.id    = id;
                fresh.type  = e->type;
                fresh.bytes = Array<u8>(m_model);
                set_block(fresh, state, e->size, bytes);
                m_blocks.push_back(std::move(fresh));
            }
        }
        m_held.push_back(std::move(held));
    }

    void release(u8 pick)
    {
        if (!m_held.empty())
        {
            m_held.erase(pick % m_held.size());
        }
    }

    [[nodiscard]] bool reload(u8 pick)
    {
        const usize k = pick % (m_mounts.size() + 1U);
        if (k == m_mounts.size())
        {
            CRD_FUZZ_REQUIRE(m_rm.reload_mount_now(res::MountId{pick % 2U == 0U ? 0U : m_next_mount + 7U}) == 0U);
            return true;
        }
        ModelMount&       m = m_mounts[k];
        Array<ModelEntry> fresh(m_model);
        bool              skip     = false;
        const bool        accepted = pack_on_disk(m.slot, fresh, skip);
        if (skip)
        {
            return false;
        }
        m_events.clear();
        const usize got = m_rm.reload_mount_now(res::MountId{m.mount_id});
        Array<ReloadEvent> expected(m_model);
        if (accepted)
        {
            m.entries = std::move(fresh);
            const Disk& d = m_disk[m.slot];
            for (const ModelEntry& e : m.entries)
            {
                const ModelMount* owner = nullptr;
                const ModelEntry* l     = live(e.id, &owner);
                ModelBlock*       b     = block(e.id);
                if (l == nullptr || owner->mount_id != m.mount_id || b == nullptr ||
                    (b->state != LoadState::Ready && b->state != LoadState::Placeholder) || b->type != e.type ||
                    !has_loader(e.type))
                {
                    continue;
                }
                const ConstSpan<u8> bytes(d.bytes.data() + e.offset, static_cast<usize>(e.size));
                if (model_load(e.type, bytes) != LoadState::Ready)
                {
                    continue;
                }
                set_block(*b, LoadState::Ready, e.size, bytes);
                b->generation += 1U;
                expected.push_back(ReloadEvent{e.id, b->generation});
            }
        }
        CRD_FUZZ_REQUIRE(got == expected.size());
        CRD_FUZZ_REQUIRE(m_events.size() == expected.size());
        for (usize i = 0U; i < expected.size(); ++i)
        {
            CRD_FUZZ_REQUIRE(m_events[i].id == expected[i].id && m_events[i].generation == expected[i].generation);
        }
        m_events.clear();
        return true;
    }

    void drain()
    {
        m_held.clear();
        m_rm.set_memory_budget(0U);
        m_rm.set_memory_budget(~u64{0});
        for (ModelBlock& b : m_blocks)
        {
            if (b.state == LoadState::Ready && pins(b.id) == 0U)
            {
                set_block(b, LoadState::Unloaded, 0U, ConstSpan<u8>());
            }
        }
    }

    [[nodiscard]] u32 pins(ResourceId id) const noexcept
    {
        u32 count = 0U;
        for (const ResourceId& p : m_pins)
        {
            count += p == id ? 1U : 0U;
        }
        return count;
    }

    void pin(ResourceId id, bool add)
    {
        if (add)
        {
            m_rm.pin(id);
            m_pins.push_back(id);
            return;
        }
        m_rm.unpin(id);
        for (usize i = 0U; i < m_pins.size(); ++i)
        {
            if (m_pins[i] == id)
            {
                m_pins.erase(i);
                return;
            }
        }
    }

    // After every operation: the live table, the budget and every held handle equal the model.
    void check()
    {
        CRD_FUZZ_REQUIRE(m_rm.mount_count() == m_mounts.size());
        usize live_ids = 0U;
        for (const ModelMount& m : m_mounts)
        {
            for (const ModelEntry& e : m.entries)
            {
                const ModelMount* owner = nullptr;
                (void)live(e.id, &owner);
                // count each id once: at the newest mount naming it
                live_ids += owner == &m ? 1U : 0U;
            }
        }
        CRD_FUZZ_REQUIRE(m_rm.entry_count() == live_ids);
        for (const ResourceId& id : m_universe)
        {
            const ModelMount*          owner = nullptr;
            const ModelEntry*          want  = live(id, &owner);
            const res::MountEntry*     got   = m_rm.find_entry(id);
            CRD_FUZZ_REQUIRE((got != nullptr) == (want != nullptr));
            if (want != nullptr)
            {
                CRD_FUZZ_REQUIRE(got->mount_id == owner->mount_id && got->type_fourcc == want->type &&
                                 got->flags == want->flags && got->blob_offset == want->offset &&
                                 got->blob_size == want->size);
                CRD_FUZZ_REQUIRE(crd::fuzz::bytes_equal(reinterpret_cast<const u8*>(got->name.data()),
                                                        got->name.size(), want->name.data(), want->name.size()));
            }
        }
        u64 resident = 0U;
        for (const ModelBlock& b : m_blocks)
        {
            resident += (b.state == LoadState::Ready || b.state == LoadState::Placeholder) ? b.size : 0U;
        }
        CRD_FUZZ_REQUIRE(m_rm.current_memory_use() == resident);
        for (const Held& h : m_held)
        {
            const FuzzPayload* const p = h.handle.get();
            if (h.fresh_failed)
            {
                CRD_FUZZ_REQUIRE(h.handle.state() == LoadState::Failed && h.handle.generation() == 0U && p == nullptr);
                continue;
            }
            ModelBlock* const b = block(h.id);
            CRD_FUZZ_REQUIRE(b != nullptr);
            CRD_FUZZ_REQUIRE(h.handle.state() == b->state && h.handle.generation() == b->generation);
            if (b->state == LoadState::Ready || b->state == LoadState::Placeholder)
            {
                CRD_FUZZ_REQUIRE(p != nullptr && p->type == b->type && p->placeholder == b->placeholder);
                CRD_FUZZ_REQUIRE(same_bytes(b->bytes, ConstSpan<u8>(p->bytes.data(), p->bytes.size())));
            }
            else
            {
                CRD_FUZZ_REQUIRE(p == nullptr);
            }
        }
    }

    void finish() { m_held.clear(); }

    [[nodiscard]] const Array<ModelMount>& mounts() const noexcept { return m_mounts; }

private:
    crd::memory::IAllocator* m_model;
    res::ResourceManager     m_rm;
    Disk                     m_disk[kSlots];
    Array<ModelMount>        m_mounts;
    Array<ModelBlock>        m_blocks;
    Array<Held>              m_held;
    Array<ResourceId>        m_universe;
    Array<ReloadEvent>       m_events;
    Array<ResourceId>        m_pins;
    u32                      m_next_mount = 1U;
};

// crdr_read on the raw bytes against the restated layout, with its allocation bound.
// Returns false when the input is out of scope (claims past the bound).
[[nodiscard]] bool check_container(const u8* data, usize size, crd::memory::IAllocator* model_alloc)
{
    ModelContainer model;
    model.chunks = Array<ModelChunk>(model_alloc);
    bool       skip     = false;
    const bool accepted = model_container(data, size, model, model_alloc, skip);
    if (skip)
    {
        return false;
    }
    crd::fuzz::BudgetAllocator reader_root;
    {
        res::CrdrFile        file(&reader_root);
        const res::CrdrError err = res::crdr_read(ConstSpan<u8>(data, size), file, &reader_root);
        CRD_FUZZ_REQUIRE((err == res::CrdrError::Ok) == accepted);
        if (accepted)
        {
            CRD_FUZZ_REQUIRE(file.id == model.id && file.type_fourcc == model.type && file.flags == model.flags &&
                             file.version == 3U && file.chunks.size() == model.chunks.size());
            for (usize i = 0U; i < model.chunks.size(); ++i)
            {
                const ModelChunk&    want = model.chunks[i];
                const res::CrdrChunk& got = file.chunks[i];
                CRD_FUZZ_REQUIRE(got.fourcc == want.fourcc && got.flags == want.flags &&
                                 got.uncompressed_size == want.uncompressed);
                if ((want.flags & 1U) == 0U)
                {
                    CRD_FUZZ_REQUIRE(got.payload.data() == data + want.offset && got.payload.size() == want.stored);
                }
                else
                {
                    CRD_FUZZ_REQUIRE(same_bytes(want.decoded, got.payload));
                }
            }
        }
        CRD_FUZZ_REQUIRE(reader_root.peak_bytes() <= 8U * size + 64U * 1024U + model.claims);
    }
    CRD_FUZZ_REQUIRE(reader_root.live_bytes() == 0U);
    return true;
}

void run_raw(const u8* data, usize size, crd::memory::IAllocator* model_alloc)
{
    if (!check_container(data, size, model_alloc))
    {
        return;
    }
    crd::fuzz::BudgetAllocator root;
    {
        Run run(root, model_alloc);
        write_disk(run.disk(0U), data, size);
        Array<ModelEntry> entries(model_alloc);
        bool              skip = false;
        (void)run.pack_on_disk(0U, entries, skip);
        for (usize i = 0U; i < entries.size() && i < kMaxRawUniverse; ++i)
        {
            run.add_universe(entries[i].id);
        }
        if (run.mount(0U))
        {
            run.check();
            for (usize i = 0U; i < entries.size() && i < kMaxRawUniverse; ++i)
            {
                run.load(entries[i].id);
                run.check();
            }
            if (run.reload(0U))
            {
                run.check();
                run.drain();
                run.check();
                for (usize i = 0U; i < entries.size() && i < kMaxRawUniverse; ++i)
                {
                    run.load(entries[i].id);
                    run.check();
                }
                run.unmount(0U);
                run.check();
            }
        }
        run.finish();
    }
    CRD_FUZZ_REQUIRE(root.live_bytes() == 0U);
}

void run_script(const u8* data, usize size, crd::memory::IAllocator* model_alloc)
{
    crd::fuzz::BudgetAllocator root;
    {
        Run    run(root, model_alloc);
        Reader in(data, size);
        for (usize i = 0U; i < kFixedIds; ++i)
        {
            run.add_universe(fixed_id(i));
        }
        bool in_scope = true;
        while (in_scope && !in.done())
        {
            const u8 op = in.byte();
            switch (op % 10U)
            {
            case 0U:
            {
                const usize slot = in.byte();
                const Spec  spec = decode_spec(in);
                Array<u8>   bytes(model_alloc);
                build_pack(spec, bytes, model_alloc);
                write_disk(run.disk(slot), bytes.data(), bytes.size());
                break;
            }
            case 1U:
            {
                const usize slot = in.byte();
                const usize n    = in.word() % (in.left() + 1U);
                const u8*   raw  = in.take(n);
                write_disk(run.disk(slot), raw, n);
                break;
            }
            case 2U:
                in_scope = run.mount(in.byte());
                break;
            case 3U:
                run.unmount(in.byte());
                break;
            case 4U:
                run.load(fixed_id(in.byte() % kFixedIds));
                break;
            case 5U:
                run.release(in.byte());
                break;
            case 6U:
                in_scope = run.reload(in.byte());
                break;
            case 7U:
                run.drain();
                break;
            case 8U:
            {
                const u8 arg = in.byte();
                run.pin(fixed_id(arg % kFixedIds), arg < 128U);
                break;
            }
            default:
            {
                Disk&     d    = run.disk(in.byte());
                const u8  mode = in.byte();
                const u16 at   = in.word();
                if (d.exists && !d.bytes.empty())
                {
                    Array<u8> bytes(d.bytes, model_alloc);
                    if (mode % 2U == 0U)
                    {
                        bytes.resize(at % bytes.size());
                    }
                    else
                    {
                        bytes[at % bytes.size()] = static_cast<u8>(bytes[at % bytes.size()] ^ (mode | 1U));
                    }
                    write_disk(d, bytes.data(), bytes.size());
                }
                break;
            }
            }
            run.check();
        }
        run.finish();
    }
    CRD_FUZZ_REQUIRE(root.live_bytes() == 0U);
}

// ── Seeds ───────────────────────────────────────────────────────────────────

[[nodiscard]] crd::memory::IAllocator* seed_alloc()
{
    static crd::fuzz::BudgetAllocator s_alloc(256U * 1024U * 1024U);
    return &s_alloc;
}

[[nodiscard]] Spec honest_spec(usize count)
{
    Spec spec;
    spec.count = count;
    for (usize i = 0U; i < count; ++i)
    {
        spec.entries[i] = SpecEntry{static_cast<u8>(i), static_cast<u8>(i % 2U), static_cast<u8>(8U + i * 5U),
                                    static_cast<u8>(0x10U + i), static_cast<u8>(i % 4U == 2U ? 0U : i % 4U)};
    }
    return spec;
}

// The raw-mode input for a pack: mode byte 0, then the file.
void add_raw(crd::fuzz::SeedSink& sink, const char* name, const Array<u8>& pack)
{
    Array<u8> input(seed_alloc());
    input.push_back(0U);
    append(input, pack.data(), pack.size());
    sink.add(name, input.data(), input.size());
}

[[nodiscard]] Array<u8> pack_of(const Spec& spec)
{
    Array<u8> out(seed_alloc());
    build_pack(spec, out, seed_alloc());
    return out;
}

// Close the 40-byte header that starts at `at`: its last word is the CRC-32C of the 36 bytes before it.
void seal_header(Array<u8>& out, usize at)
{
    const u32 check = header_check(out.data() + at);
    for (u32 i = 0U; i < 4U; ++i)
    {
        out[at + 36U + i] = static_cast<u8>(check >> (8U * i));
    }
}

// A chunk header with the fields given as they are (a seed's lie is in them) and a valid header check.
void chunk_header(Array<u8>& out, u32 fourcc, u32 flags, u64 claim, u64 stored, u64 hash, u32 stored_check)
{
    const usize at = out.size();
    put_u32(out, fourcc);
    put_u32(out, flags);
    put_u64(out, claim);
    put_u64(out, stored);
    put_u64(out, hash);
    put_u32(out, stored_check);
    put_u32(out, 0U);
    seal_header(out, at);
}

// `size` stored bytes, padded to 16 bytes of their own size.
void stored_payload(Array<u8>& out, const u8* stored, usize size)
{
    append(out, stored, size);
    for (usize pad = (16U - size % 16U) % 16U; pad > 0U; --pad)
    {
        out.push_back(0U);
    }
}

// A compressed chunk of `fourcc` whose header claims `claim` bytes (zstd of `text`, with the text's hash).
void compressed_chunk(Array<u8>& out, u32 fourcc, const char* text, u64 claim)
{
    const usize n     = std::strlen(text);
    Array<u8>   frame(seed_alloc());
    frame.resize(ZSTD_compressBound(n));
    const usize got = ZSTD_compress(frame.data(), frame.size(), text, n, 3);
    frame.resize(got);
    chunk_header(out, fourcc, 1U, claim, frame.size(),
                 crd::containers::xxh3_64(ConstSpan<u8>(reinterpret_cast<const u8*>(text), n)),
                 crd::containers::crc32c(ConstSpan<u8>(frame.data(), frame.size())));
    stored_payload(out, frame.data(), frame.size());
}

void container_header(Array<u8>& out, u32 type, u32 chunks)
{
    const usize at = out.size();
    put_u32(out, res::kFourCC_CRDR);
    out.push_back(3U);
    out.push_back(0U);
    out.push_back(0U);
    out.push_back(0U);
    put_u64(out, 0x51U);
    put_u64(out, 0x52U);
    put_u32(out, type);
    put_u32(out, chunks);
    put_u32(out, 0U);
    put_u32(out, 0U);
    seal_header(out, at);
}

// Script assembly for the structured seeds.
class Script
{
public:
    Script() : m_bytes(seed_alloc()) { m_bytes.push_back(1U); }
    Script& spec(u8 slot, const Spec& s)
    {
        m_bytes.push_back(0U);
        m_bytes.push_back(slot);
        m_bytes.push_back(s.flags);
        m_bytes.push_back(static_cast<u8>(s.count));
        for (usize i = 0U; i < s.count; ++i)
        {
            const SpecEntry& e = s.entries[i];
            m_bytes.push_back(e.id_index);
            m_bytes.push_back(e.type);
            m_bytes.push_back(e.length);
            m_bytes.push_back(e.fill);
            m_bytes.push_back(e.name);
        }
        if ((s.flags & kRangeOverride) != 0U)
        {
            m_bytes.push_back(s.override_entry);
            m_bytes.push_back(s.override_field);
            put_u64(m_bytes, s.override_value);
        }
        if ((s.flags & kDuplicateId) != 0U)
        {
            m_bytes.push_back(s.duplicate_of);
        }
        if ((s.flags & kTruncate) != 0U)
        {
            m_bytes.push_back(s.cut);
        }
        if ((s.flags & kTrailing) != 0U)
        {
            m_bytes.push_back(s.junk);
        }
        return *this;
    }
    Script& op(u8 code, u8 arg)
    {
        m_bytes.push_back(code);
        m_bytes.push_back(arg);
        return *this;
    }
    Script& mount(u8 slot) { return op(2U, slot); }
    Script& unmount(u8 pick) { return op(3U, pick); }
    Script& load(u8 id) { return op(4U, id); }
    Script& release(u8 pick) { return op(5U, pick); }
    Script& reload(u8 pick) { return op(6U, pick); }
    Script& drain()
    {
        m_bytes.push_back(7U);
        return *this;
    }
    Script& pin(u8 id, bool add) { return op(8U, static_cast<u8>(add ? id : 128U + id)); }
    Script& corrupt(u8 slot, u8 mode, u16 at)
    {
        m_bytes.push_back(9U);
        m_bytes.push_back(slot);
        m_bytes.push_back(mode);
        m_bytes.push_back(static_cast<u8>(at));
        m_bytes.push_back(static_cast<u8>(at >> 8U));
        return *this;
    }
    void add(crd::fuzz::SeedSink& sink, const char* name) const { sink.add(name, m_bytes.data(), m_bytes.size()); }

private:
    Array<u8> m_bytes;
};

// The verdict of a raw seed: the real manager mounts the pack.
[[nodiscard]] bool raw_pack_mounts(const u8* data, usize size)
{
    crd::fuzz::BudgetAllocator root;
    crd::fuzz::BudgetAllocator model;
    bool                       mounted = false;
    {
        Run run(root, &model);
        write_disk(run.disk(0U), data + 1U, size - 1U);
        res::ResourceManager rm(&root);
        mounted = rm.mount_manifest(crd::containers::StringView(run.disk(0U).path.c_str())).is_valid();
    }
    return mounted;
}
} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    if (size == 0U || size > crd::fuzz::kMaxInputBytes)
    {
        return 0;
    }
    crd::log::set_all_channels_level(crd::log::LogLevel::Off);
    crd::fuzz::BudgetAllocator model;
    if ((data[0] & 1U) == 0U)
    {
        run_raw(data + 1U, size - 1U, &model);
    }
    else
    {
        run_script(data + 1U, size - 1U, &model);
    }
    CRD_FUZZ_REQUIRE(model.live_bytes() == 0U);
    return 0;
}

void crd_fuzz_seeds(crd::fuzz::SeedSink& sink)
{
    crd::log::set_all_channels_level(crd::log::LogLevel::Off);
    static const char* const kMounts[] = {
        "pack-honest", "pack-compressed-manifest", "pack-empty", "pack-name-past-pool", "pack-zero-length-entry",
        "pack-refused-artifacts", "pack-trailing-bytes",
    };
    crd::fuzz::VerdictSink raw(sink, &raw_pack_mounts, kMounts, sizeof(kMounts) / sizeof(kMounts[0]));

    // Packs that mount.
    add_raw(raw, "pack-honest", pack_of(honest_spec(3U)));
    Spec compressed = honest_spec(6U);
    compressed.flags = kCompress;
    for (usize i = 0U; i < compressed.count; ++i)
    {
        compressed.entries[i].name = 3U; // long, alike names: the pool and the table compress
    }
    add_raw(raw, "pack-compressed-manifest", pack_of(compressed));
    add_raw(raw, "pack-empty", pack_of(honest_spec(0U)));
    Spec past_pool             = honest_spec(2U);
    past_pool.entries[1].name  = 2U;
    add_raw(raw, "pack-name-past-pool", pack_of(past_pool));
    Spec zero_length            = honest_spec(2U);
    zero_length.entries[0].length = 0U;
    add_raw(raw, "pack-zero-length-entry", pack_of(zero_length));
    Spec refused             = honest_spec(2U);
    refused.entries[0].fill  = kRefuseByte; // type A: Failed
    refused.entries[1].fill  = kRefuseByte; // type B: Placeholder
    add_raw(raw, "pack-refused-artifacts", pack_of(refused));
    Spec trailing  = honest_spec(1U);
    trailing.flags = kTrailing;
    trailing.junk  = 40U;
    add_raw(raw, "pack-trailing-bytes", pack_of(trailing));

    // Packs refused whole.
    Spec torn  = honest_spec(3U);
    torn.flags = kTruncate;
    torn.cut   = 4U;
    add_raw(raw, "pack-torn", pack_of(torn));
    Spec wrap           = honest_spec(2U);
    wrap.flags          = kRangeOverride;
    wrap.override_entry = 1U;
    wrap.override_field = 0U;
    wrap.override_value = ~u64{0} - 7U; // offset + size wraps past zero
    add_raw(raw, "pack-range-wraps", pack_of(wrap));
    Spec duplicate         = honest_spec(3U);
    duplicate.flags        = kDuplicateId;
    duplicate.duplicate_of = 2U;
    add_raw(raw, "pack-duplicate-id", pack_of(duplicate));
    Spec wrong_type  = honest_spec(1U);
    wrong_type.flags = kWrongType;
    add_raw(raw, "pack-not-a-pack", pack_of(wrong_type));
    Spec no_manifest  = honest_spec(1U);
    no_manifest.flags = kDropMfst;
    add_raw(raw, "pack-no-manifest", pack_of(no_manifest));
    {
        Array<u8> odd(seed_alloc());
        container_header(odd, res::kFourCC_PACK, 1U);
        const u8 zeros[47] = {};
        chunk_header(odd, res::kFourCC_MFST, 0U, 47U, 47U, 0U, crd::containers::crc32c(ConstSpan<u8>(zeros, 47U)));
        stored_payload(odd, zeros, sizeof(zeros));
        add_raw(raw, "pack-manifest-not-whole-entries", odd);
    }
    {
        // Compressed chunks whose claims the stored bytes cannot back: one past the format bound (the reader reserved
        // it before decoding), two whose sum wraps to 16 bytes (the reader reserved 16 and decoded 32 into them), one
        // a byte past what the frame decodes to (the reader kept a zero-filled tail).
        Array<u8> huge(seed_alloc());
        container_header(huge, res::kFourCC_PACK, 1U);
        compressed_chunk(huge, res::kFourCC_MFST, "a manifest that is not one", u64{1} << 40U);
        add_raw(raw, "chunk-claim-past-bound", huge);
        Array<u8> wraps(seed_alloc());
        container_header(wraps, res::kFourCC_PACK, 2U);
        compressed_chunk(wraps, res::kFourCC_MFST, "first frame of the wrapping pair", u64{1} << 63U);
        compressed_chunk(wraps, res::kFourCC_STRP, "second frame of the wrapping pair", (u64{1} << 63U) + 16U);
        add_raw(raw, "chunk-claims-wrap", wraps);
        const char* text = "an honest frame that decodes to exactly fifty-one bytes";
        Array<u8>   longer(seed_alloc());
        container_header(longer, res::kFourCC_PACK, 1U);
        compressed_chunk(longer, res::kFourCC_STRP, text, std::strlen(text) + 1U);
        add_raw(raw, "chunk-claim-past-frame", longer);
        Array<u8> exact(seed_alloc());
        container_header(exact, res::kFourCC_BLOB, 1U);
        compressed_chunk(exact, res::kFourCC_STRP, text, std::strlen(text));
        add_raw(raw, "chunk-claim-exact", exact);
    }
    {
        Array<u8> past(seed_alloc());
        container_header(past, res::kFourCC_PACK, 1U);
        chunk_header(past, res::kFourCC_MFST, 0U, 96U, 96U, 0U, 0U);
        add_raw(raw, "chunk-stored-past-end", past);
        Array<u8> header(seed_alloc());
        container_header(header, res::kFourCC_PACK, 3U);
        header.resize(20U);
        add_raw(raw, "container-header-cut", header);
        Array<u8> garbage(seed_alloc());
        append(garbage, reinterpret_cast<const u8*>("CRDR but not a container"), 24U);
        add_raw(raw, "garbage", garbage);
    }

    // Scripts over the manager.
    const Spec base = honest_spec(3U); // ids 0, 1, 2 typed A, B, A
    Spec       edited = base;
    for (usize i = 0U; i < edited.count; ++i)
    {
        edited.entries[i].fill = static_cast<u8>(edited.entries[i].fill + 0x40U);
    }
    Script().spec(0U, base).mount(0U).load(0U).load(1U).load(2U).spec(0U, edited).reload(0U).load(0U).add(
        sink, "script-reload-swaps");
    Spec torn_reload  = edited;
    torn_reload.flags = kTruncate;
    torn_reload.cut   = 2U;
    Script().spec(0U, base).mount(0U).load(0U).load(2U).spec(0U, torn_reload).reload(0U).drain().load(0U).load(2U).add(
        sink, "script-reload-torn");
    Spec wrap_reload           = edited;
    wrap_reload.flags          = kRangeOverride;
    wrap_reload.override_entry = 0U;
    wrap_reload.override_field = 0U;
    wrap_reload.override_value = ~u64{0} - 15U;
    Script().spec(0U, base).mount(0U).load(0U).spec(0U, wrap_reload).reload(0U).load(0U).add(
        sink, "script-reload-range-wraps");
    Spec shadow = honest_spec(1U); // id 0, type A, other bytes
    shadow.entries[0].fill = 0x70U;
    Script().spec(0U, base).spec(1U, shadow).mount(0U).mount(1U).load(0U).spec(0U, edited).reload(0U).add(
        sink, "script-reload-shadowed-id");
    Spec retyped           = edited;
    retyped.entries[0].type = 1U; // id 0 becomes type B
    Script().spec(0U, base).mount(0U).load(0U).spec(0U, retyped).reload(0U).load(0U).add(
        sink, "script-reload-retyped");
    Script().spec(0U, base).mount(0U).load(0U).release(0U).drain().spec(0U, retyped).reload(0U).load(0U).add(
        sink, "script-reissue-retyped");
    Script().spec(0U, base).spec(1U, shadow).mount(0U).mount(1U).load(1U).unmount(1U).load(0U).add(
        sink, "script-unmount-falls-back");
    Spec moved              = base;
    moved.entries[0].id_index = 5U; // drops id 0, adds id 5
    Script().spec(0U, base).mount(0U).load(0U).load(1U).spec(0U, moved).reload(0U).load(5U).drain().load(0U).add(
        sink, "script-reload-drops-and-adds");
    Spec resized             = edited;
    resized.entries[1].length = 40U;
    Script().spec(0U, base).mount(0U).load(1U).spec(0U, resized).reload(0U).drain().add(
        sink, "script-reload-resizes");
    Spec dup_reload         = edited;
    dup_reload.flags        = kDuplicateId;
    dup_reload.duplicate_of = 2U;
    Script().spec(0U, base).mount(0U).load(0U).load(2U).spec(0U, dup_reload).reload(0U).add(
        sink, "script-reload-duplicate-id");
    Script().spec(0U, base).mount(0U).pin(0U, true).load(0U).load(2U).drain().pin(0U, false).drain().load(2U).add(
        sink, "script-pin-and-drain");
    Spec refuse_b             = base;
    refuse_b.entries[1].fill  = kRefuseByte;
    Spec refuse_edit          = edited;
    refuse_edit.entries[0].fill = kRefuseByte;
    Script().spec(0U, refuse_b).mount(0U).load(1U).load(0U).spec(0U, refuse_edit).reload(0U).spec(0U, edited).reload(
        0U).add(sink, "script-placeholder-and-last-good");
    Script().spec(0U, base).mount(0U).load(0U).corrupt(0U, 1U, 70U).reload(0U).corrupt(0U, 0U, 100U).drain().load(0U)
        .reload(0U).add(sink, "script-corrupt-in-place");
    Script().spec(0U, base).mount(1U).mount(0U).unmount(7U).unmount(8U).reload(5U).reload(6U).load(3U).add(
        sink, "script-invalid-mounts");
}
