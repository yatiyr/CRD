#pragma once

// CRDR — Cerid chunked binary container format (ADR-0038).
//
// All cooked artifacts share one format: a 32-byte header + N typed chunks.
// Every chunk stores a fourcc, flags, sizes, and a zero-padded payload.
// The container itself carries the ResourceId and a type fourcc ('PACK', 'SHDR', …).
//
// File layout (little-endian):
//
//   Header (32 bytes):
//     +0  u32  magic       = make_fourcc('C','R','D','R')
//     +4  u16  version     = 1
//     +6  u16  flags       (bit 0: payloads are zstd-compressed — v1b)
//     +8  u64  uuid_hi     ← ResourceId::hi
//     +16 u64  uuid_lo     ← ResourceId::lo
//     +24 u32  type_fourcc ← 'PACK', 'SHDR', 'MATR', 'BLOB', …
//     +28 u32  chunk_count
//
//   Each chunk (24-byte header + payload):
//     +0  u32  fourcc
//     +4  u32  flags            (bit 0: payload compressed — unused until v1b)
//     +8  u64  uncompressed_size
//     +16 u64  compressed_size  (== uncompressed_size when bit 0 of flags is clear)
//     +24 u8[] payload          (compressed_size bytes, zero-padded to 16-byte alignment)
//
// Determinism: chunk order is sorted ascending by fourcc at write time.
//              Padding bytes are zero-filled. No timestamps inside any chunk.

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>
#include <crd/resources/resource_id.hpp>

namespace crd::resources
{

// ── FourCC helpers ─────────────────────────────────────────────────────────

constexpr crd::u32 make_fourcc(char a, char b, char c, char d) noexcept
{
    return static_cast<crd::u32>(static_cast<unsigned char>(a))
         | (static_cast<crd::u32>(static_cast<unsigned char>(b)) << 8U)
         | (static_cast<crd::u32>(static_cast<unsigned char>(c)) << 16U)
         | (static_cast<crd::u32>(static_cast<unsigned char>(d)) << 24U);
}

// Registered FourCCs
inline constexpr crd::u32 kFourCC_CRDR = make_fourcc('C', 'R', 'D', 'R'); // magic
inline constexpr crd::u32 kFourCC_PACK = make_fourcc('P', 'A', 'C', 'K');
inline constexpr crd::u32 kFourCC_BLOB = make_fourcc('B', 'L', 'O', 'B');
inline constexpr crd::u32 kFourCC_SHDR = make_fourcc('S', 'H', 'D', 'R');
inline constexpr crd::u32 kFourCC_MATR = make_fourcc('M', 'A', 'T', 'R');
inline constexpr crd::u32 kFourCC_AUDO = make_fourcc('A', 'U', 'D', 'O');
inline constexpr crd::u32 kFourCC_MFST = make_fourcc('M', 'F', 'S', 'T');
inline constexpr crd::u32 kFourCC_STRP = make_fourcc('S', 'T', 'R', 'P');
inline constexpr crd::u32 kFourCC_DEPS = make_fourcc('D', 'E', 'P', 'S');
inline constexpr crd::u32 kFourCC_META = make_fourcc('M', 'E', 'T', 'A');
inline constexpr crd::u32 kFourCC_SPVV = make_fourcc('S', 'P', 'V', 'V'); // vertex SPIRV chunk
inline constexpr crd::u32 kFourCC_SPVF = make_fourcc('S', 'P', 'V', 'F'); // fragment SPIRV chunk
inline constexpr crd::u32 kFourCC_SPVC = make_fourcc('S', 'P', 'V', 'C'); // compute SPIRV chunk

// Phase 2.7 — texture artifact FourCCs (ADR-0042)
inline constexpr crd::u32 kFourCC_TXTR = make_fourcc('T', 'X', 'T', 'R');
inline constexpr crd::u32 kFourCC_HEAD = make_fourcc('H', 'E', 'A', 'D');

// Mip-level chunk FourCCs for TXTR artifacts.
// Level 0 = full-res → level 15 = smallest (1×1).
// Digits 0–9 use ASCII '0'–'9'; levels 10–15 use 'A'–'F'.
[[nodiscard]] constexpr crd::u32 make_mip_fourcc(crd::u8 level) noexcept
{
    const char digit = (level < 10U) ? static_cast<char>('0' + level)
                                     : static_cast<char>('A' + (level - 10U));
    return make_fourcc('M', 'I', 'P', digit);
}

inline constexpr crd::u32 kFourCC_MIP0  = make_mip_fourcc(0U);
inline constexpr crd::u32 kFourCC_MIP1  = make_mip_fourcc(1U);
inline constexpr crd::u32 kFourCC_MIP2  = make_mip_fourcc(2U);
inline constexpr crd::u32 kFourCC_MIP3  = make_mip_fourcc(3U);
inline constexpr crd::u32 kFourCC_MIP4  = make_mip_fourcc(4U);
inline constexpr crd::u32 kFourCC_MIP5  = make_mip_fourcc(5U);
inline constexpr crd::u32 kFourCC_MIP6  = make_mip_fourcc(6U);
inline constexpr crd::u32 kFourCC_MIP7  = make_mip_fourcc(7U);
inline constexpr crd::u32 kFourCC_MIP8  = make_mip_fourcc(8U);
inline constexpr crd::u32 kFourCC_MIP9  = make_mip_fourcc(9U);
inline constexpr crd::u32 kFourCC_MIP10 = make_mip_fourcc(10U);
inline constexpr crd::u32 kFourCC_MIP11 = make_mip_fourcc(11U);
inline constexpr crd::u32 kFourCC_MIP12 = make_mip_fourcc(12U);
inline constexpr crd::u32 kFourCC_MIP13 = make_mip_fourcc(13U);
inline constexpr crd::u32 kFourCC_MIP14 = make_mip_fourcc(14U);
inline constexpr crd::u32 kFourCC_MIP15 = make_mip_fourcc(15U);

// Phase 2.7 — mesh artifact FourCCs (ADR-0043)
inline constexpr crd::u32 kFourCC_MESH = make_fourcc('M', 'E', 'S', 'H');
inline constexpr crd::u32 kFourCC_VERT = make_fourcc('V', 'E', 'R', 'T');
inline constexpr crd::u32 kFourCC_INDX = make_fourcc('I', 'N', 'D', 'X');
inline constexpr crd::u32 kFourCC_PRIM = make_fourcc('P', 'R', 'I', 'M');

// Phase 2.7 v1c — material artifact FourCCs (ADR-0048)
inline constexpr crd::u32 kFourCC_INFO = make_fourcc('I', 'N', 'F', 'O'); // domain + version + flags
inline constexpr crd::u32 kFourCC_PRMS = make_fourcc('P', 'R', 'M', 'S'); // parameter schema
inline constexpr crd::u32 kFourCC_DFLT = make_fourcc('D', 'F', 'L', 'T'); // default values blob
inline constexpr crd::u32 kFourCC_PASS = make_fourcc('P', 'A', 'S', 'S'); // pass-keyed shader pairs
inline constexpr crd::u32 kFourCC_PSOS = make_fourcc('P', 'S', 'O', 'S'); // per-pass PSO state
inline constexpr crd::u32 kFourCC_OPTS = make_fourcc('O', 'P', 'T', 'S'); // shader option decls

// Convert a fourcc back to a 4-char printable string (for logging).
// Result is null-terminated; stored in caller-provided buf[5].
void fourcc_to_str(crd::u32 fourcc, char (&buf)[5]) noexcept;

// ── Container version ───────────────────────────────────────────────────────

inline constexpr crd::u16 kCrdrVersion = 1U;

// ── CRDR chunk (read view) ──────────────────────────────────────────────────

struct CrdrChunk
{
    crd::u32 fourcc            = 0;
    crd::u32 flags             = 0;
    crd::u64 uncompressed_size = 0;
    crd::containers::ConstSpan<crd::u8> payload{}; // view into the source byte span
};

// ── CrdrFile — parsed representation ───────────────────────────────────────

struct CrdrFile
{
    ResourceId id{};
    crd::u32   type_fourcc  = 0;
    crd::u16   version      = 0;
    crd::u16   flags        = 0;
    crd::containers::Array<CrdrChunk> chunks;
    // Owns decompressed bytes for compressed chunks; chunk payloads point here.
    crd::containers::Array<crd::u8>   decompressed_backing;

    explicit CrdrFile(crd::memory::IAllocator* a = crd::memory::default_allocator())
        : chunks(a), decompressed_backing(a)
    {
    }
};

// ── Error codes ─────────────────────────────────────────────────────────────

enum class CrdrError : crd::u8
{
    Ok,
    BadMagic,
    BadVersion,
    Truncated,
    InvalidChunk,
    DecompressFailed,
};

// ── Reader ──────────────────────────────────────────────────────────────────

// Parse a CRDR blob. Chunk payloads are views into `bytes` — keep `bytes`
// alive as long as any CrdrChunk from `out` is accessed.
[[nodiscard]] CrdrError crdr_read(
    crd::containers::ConstSpan<crd::u8> bytes,
    CrdrFile&                           out,
    crd::memory::IAllocator*            a = crd::memory::default_allocator());

// Find the first chunk with the given fourcc, or nullptr.
[[nodiscard]] const CrdrChunk* crdr_find_chunk(
    const CrdrFile& file, crd::u32 fourcc) noexcept;

// ── Writer ──────────────────────────────────────────────────────────────────

// Assemble a CRDR container. Call add_chunk() for each chunk, then finish()
// to produce the binary blob. Chunks are sorted ascending by fourcc at finish().
class CrdrWriter
{
public:
    explicit CrdrWriter(
        crd::memory::IAllocator* a,
        ResourceId               id,
        crd::u32                 type_fourcc);

    void add_chunk(
        crd::u32                              fourcc,
        crd::containers::ConstSpan<crd::u8>  payload,
        crd::u32                             chunk_flags = 0U);

    // Compress `payload` with zstd (level `zstd_level`) and store the chunk.
    // Falls back to uncompressed storage if compression doesn't help.
    void add_chunk_compressed(
        crd::u32                             fourcc,
        crd::containers::ConstSpan<crd::u8> payload,
        int                                  zstd_level = 3);

    // Finalise: sort chunks by fourcc and assemble the binary.
    [[nodiscard]] crd::containers::Array<crd::u8> finish();

private:
    struct PendingChunk
    {
        crd::u32                        fourcc;
        crd::u32                        flags;
        crd::u64                        uncompressed_size; // original size before compression
        crd::containers::Array<crd::u8> payload;           // compressed bytes (or raw if uncompressed)
    };

    crd::memory::IAllocator*                 m_alloc;
    ResourceId                               m_id;
    crd::u32                                 m_type_fourcc;
    crd::containers::Array<PendingChunk>     m_chunks;
};

// ── Manifest helpers (type='PACK', chunks: MFST + STRP + DEPS) ─────────────

// One entry from the MFST chunk (48 bytes on disk).
struct ManifestEntry
{
    ResourceId  id{};
    crd::u32    type_fourcc    = 0;
    crd::u32    flags          = 0;
    crd::u64    blob_offset    = 0; // byte offset within the PACK file
    crd::u64    blob_size      = 0;
    crd::u32    name_strp_idx  = 0; // byte offset into the STRP string pool
};

// Write the three manifest chunks (MFST, STRP, DEPS) into a CrdrWriter.
// `entries` must have their name_strp_idx already set to the byte offset of
// their name inside `string_pool`.
void manifest_write(
    CrdrWriter&                                      writer,
    crd::containers::ConstSpan<ManifestEntry>        entries,
    crd::containers::ConstSpan<crd::u8>              string_pool,
    crd::containers::ConstSpan<crd::u8>              deps_payload = {});

// Parse a MFST chunk payload into an array of ManifestEntry.
[[nodiscard]] bool manifest_read_entries(
    crd::containers::ConstSpan<crd::u8>   mfst_payload,
    crd::containers::Array<ManifestEntry>& out,
    crd::memory::IAllocator*              a = crd::memory::default_allocator());

} // namespace crd::resources
