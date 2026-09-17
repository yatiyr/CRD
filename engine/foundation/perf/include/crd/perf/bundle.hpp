#pragma once

// ---------------------------------------------------------------------------
// crd-perf -- cerid-diagnostics/1 post-mortem bundle container (DIAG.5d(b)).
//
// A "bundle" packages the separate post-mortem artifacts DIAG.5a/5b produce
// (an async-signal-safe crash record, a raw dump/core, a log tail, a manifest,
// symbol identities) into ONE self-describing, versioned archive that can be
// written atomically and recovered even when truncated. This slice ((b)) is
// the CONTAINER + WRITER plus a round-trip reader with truncation recovery;
// exact symbol identities (c), unloaded generations (d) and the trustworthy /
// adversarial importer (e) build on this format. This file adds NO capture of
// live process state -- it packages bytes handed to it.
//
// Format -- cerid-diagnostics/1 (sequential TLV, NOT a trailing directory, so a
// truncated file recovers its complete prefix by construction -- the exact
// failure the acceptance's "recover truncated bundles" names):
//
//   [BundleHeader]                          -- magic + schema/format version + section_count
//   repeated section_count times:
//     [BundleSectionHeader]                 -- tag + flags + payload_length + original_length + crc32
//     [payload bytes]                       -- payload_length bytes (crc32 covers exactly these)
//
// Endianness: little-endian only (x64 + ARM64 little), matching CPROF capture
// (capture.hpp). Cross-arch big-endian devices are not supported by Cerid today.
//
// Bounds (ADR-0133 line 134): a bundle is hard-capped per-section and in total
// (default total 256 MiB) with HONEST truncation markers -- a section too large
// for the remaining room is stored truncated, kSectionFlagTruncated is set and
// original_length records the pre-truncation size; nothing silently grows or
// drops. Retention keeps at most kDefaultMaxBundles (10) bundles in a directory.
//
// Compression: kSectionFlagCompressed is DEFINED but NOT produced by v1 (every
// section is written uncompressed -- the deflate codec lives downstream in
// crd-resources, which crd-perf must not depend on). The v1 reader REJECTS a
// section that carries the flag (UnsupportedFeature), so the decompression-bomb
// defence (e) owns is already inherited here: a compressed section is refused
// until a codec edge is deliberately added.
//
// Contract: docs/design/runtime-diagnostics.md (DIAG.5d); ADR-0133 ID-1.
// ---------------------------------------------------------------------------

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::perf
{

// 'CDB1' little-endian -- Cerid Diagnostics Bundle, format v1. Distinct from
// CPROF capture's 'CPRO' so the two files can never be confused.
inline constexpr crd::u32 kBundleMagic = 0x31424443U; // bytes 'C','D','B','1'

// Container layout version. Bump on any change to BundleHeader /
// BundleSectionHeader or the meaning of the section stream; a bump ships a
// reader for N-1 (ADR ID-1). Separate from the cerid-diagnostics SCHEMA version
// (perf::kDiagnosticSchemaVersion), which the header also carries.
inline constexpr crd::u32 kBundleFormatVersion = 1U;

// Section identity. Foundation owns the low reserved range [1, 0xFFFF]; later
// slices/modules register tags above 0x00010000 from their own side. 0 is
// reserved (invalid) so a zeroed buffer never reads as a real section.
enum class BundleSectionTag : crd::u32
{
    Invalid     = 0,
    Manifest    = 1, // cerid-diagnostics manifest (redaction-honest; never claims a raw dump is redacted)
    CrashRecord = 2, // the 5a/5b async-signal-safe record bytes
    RawDump     = 3, // Windows minidump / Linux core (raw; ID-8: never *declared* redacted)
    LogTail     = 4, // recent log records (bytes; the composer that fills it is a later slice)
    SymbolIndex = 5, // module identities (filled by (c))
    // 6 .. 0x0000FFFF reserved for foundation; modules register above 0x00010000.
};

// Per-section flags (bitset in BundleSectionHeader::flags).
inline constexpr crd::u32 kSectionFlagAbsent     = 1U << 0; // tag present, payload_length 0 (explicit absent section)
inline constexpr crd::u32 kSectionFlagTruncated  = 1U << 1; // payload cut to a cap; original_length = pre-cut size
inline constexpr crd::u32 kSectionFlagCompressed = 1U << 2; // payload is deflate_raw -- DEFINED, not produced by v1

// ---- On-disk POD layouts (little-endian, explicit padding, pinned sizes) ----

struct BundleHeader
{
    crd::u32 magic;          // 4  kBundleMagic
    crd::u32 schema_version; // 4  cerid-diagnostics/N (== perf::kDiagnosticSchemaVersion)
    crd::u32 format_version; // 4  kBundleFormatVersion
    crd::u32 flags;          // 4  reserved (0 in v1)
    crd::u64 created_at_ns;  // 8  monotonic clock at write (diagnostic_now_ns)
    crd::u64 total_len;      // 8  total bytes produced: sizeof(BundleHeader) + all sections
    crd::u32 section_count;  // 4  number of section headers that follow
    crd::u32 header_crc32;   // 4  crc32 of the preceding 36 header bytes
};

static_assert(sizeof(BundleHeader) == 40, "BundleHeader is 40 B; on-disk pin");
static_assert(alignof(BundleHeader) == 8, "BundleHeader is 8-aligned");

struct BundleSectionHeader
{
    crd::u32 tag;             // 4  BundleSectionTag
    crd::u32 flags;           // 4  kSectionFlag*
    crd::u64 payload_length;  // 8  bytes of payload stored immediately after this header
    crd::u64 original_length; // 8  logical size before truncation (== payload_length when not truncated)
    crd::u32 payload_crc32;   // 4  crc32 of the payload bytes (0 when payload_length == 0)
    crd::u32 _pad_a;          // 4  reserved (0)
};

static_assert(sizeof(BundleSectionHeader) == 32, "BundleSectionHeader is 32 B; on-disk pin");
static_assert(alignof(BundleSectionHeader) == 8, "BundleSectionHeader is 8-aligned");

// ---- Bounds ----

inline constexpr crd::u64 kDefaultMaxSectionBytes = 64ULL * 1024 * 1024;  // per-section cap
inline constexpr crd::u64 kDefaultMaxTotalBytes   = 256ULL * 1024 * 1024; // ADR-0133 line 134 total cap
inline constexpr crd::u32 kDefaultMaxSections     = 32U;
inline constexpr crd::u32 kDefaultMaxBundles      = 10U; // ADR-0133 line 134 retention cap

struct BundleLimits
{
    crd::u64 max_section_bytes = kDefaultMaxSectionBytes;
    crd::u64 max_total_bytes   = kDefaultMaxTotalBytes;
    crd::u32 max_sections      = kDefaultMaxSections;
};

// ---- Writer ----
//
// Accumulates sections into an in-memory buffer, then serialises a self-contained
// bundle. Enforces per-section + total caps with honest truncation markers.
// noexcept throughout: post-mortem code must not let an exception escape.
class BundleWriter
{
public:
    enum class AddStatus : crd::u32
    {
        Ok,              // stored in full
        StoredTruncated, // stored, truncated to fit a cap; Truncated flag + original_length set
        StoredAbsent,    // explicit absent section recorded (payload_length 0)
        RejectedTooMany, // max_sections already reached; nothing stored
        RejectedNoRoom,  // no room even for the section header within max_total_bytes; nothing stored
    };

    explicit BundleWriter(crd::memory::IAllocator* alloc = crd::memory::default_allocator(),
                          BundleLimits             limits = {}) noexcept;

    // Append a section. `bytes` beyond the per-section cap OR the remaining total
    // room is stored truncated (kSectionFlagTruncated + original_length =
    // bytes.size()). An empty span records an explicit absent section.
    AddStatus add_section(BundleSectionTag tag, crd::containers::ConstSpan<crd::u8> bytes) noexcept;

    // Record an explicit absent section (tag present, payload_length 0).
    AddStatus add_absent(BundleSectionTag tag) noexcept;

    // Serialise header + sections into one buffer, fixing up total_len,
    // section_count, created_at_ns and all CRCs. Call once: a second finish()
    // returns an empty buffer, and add_* after finish() returns RejectedNoRoom.
    [[nodiscard]] crd::containers::Array<crd::u8> finish() noexcept;

    // Deterministic variant: stamp `created_at_ns` explicitly instead of reading the clock, so a
    // bundle written from fixed inputs is byte-reproducible (golden bundles / migration guards).
    [[nodiscard]] crd::containers::Array<crd::u8> finish(crd::u64 created_at_ns) noexcept;

    [[nodiscard]] crd::u32 section_count() const noexcept { return m_section_count; }
    [[nodiscard]] crd::u64 total_bytes() const noexcept { return m_total; }

private:
    // Append one section header + payload (already clamped) to m_sections.
    void emit(BundleSectionTag tag, crd::u32 flags, crd::containers::ConstSpan<crd::u8> payload,
              crd::u64 original_length) noexcept;

    crd::memory::IAllocator*        m_alloc;
    BundleLimits                    m_limits;
    crd::containers::Array<crd::u8> m_sections;                         // concatenated [hdr][payload]... (no file header)
    crd::u32                        m_section_count = 0;
    crd::u64                        m_total         = sizeof(BundleHeader); // running serialised size
    bool                            m_finished      = false;
};

// ---- Reader (round-trip + truncation recovery) ----
//
// This is (b)'s minimal reader: it round-trips the writer's output and recovers
// a truncated bundle's complete prefix. The BOUNDED, NON-EXECUTING, adversarial
// importer (traversal / oversized / bomb / version-mismatch rejection, golden
// manifests, fuzz corpus) is (e); it builds on these types.

enum class BundleReadStatus : crd::u32
{
    Ok,                 // header valid and every declared section present + crc-verified
    RecoveredTruncated, // header valid; a valid prefix of sections recovered, then parsing stopped
    BadMagic,           // not a cerid-diagnostics bundle
    HeaderTooSmall,     // fewer than sizeof(BundleHeader) bytes, or the header crc mismatched
    UnsupportedVersion, // format_version != kBundleFormatVersion
    UnsupportedFeature, // a section carried kSectionFlagCompressed (no codec edge in v1)
};

struct BundleSectionView
{
    BundleSectionTag                    tag;
    crd::u32                            flags;
    crd::u64                            original_length; // pre-truncation logical size
    crd::containers::ConstSpan<crd::u8> payload;         // borrowed view into the input buffer
};

struct BundleReadResult
{
    BundleReadStatus                          status;
    BundleHeader                              header;                 // valid iff status not BadMagic/HeaderTooSmall
    crd::u32                                  declared_section_count; // header.section_count as read
    crd::u64                                  stop_offset;            // byte offset where parsing stopped
    crd::containers::Array<BundleSectionView> sections;               // the recovered (valid, crc-checked) prefix
};

// Parse `buf`. Never executes or allocates from the payload; the returned views
// borrow into `buf`, which must outlive the result. Stops at the first
// structurally-invalid section (short header, short payload, crc mismatch) and
// returns the recovered prefix (RecoveredTruncated) rather than failing the whole
// read. A compressed-flagged section stops the read with UnsupportedFeature.
[[nodiscard]] BundleReadResult read_bundle(crd::containers::ConstSpan<crd::u8> buf,
                                           crd::memory::IAllocator* alloc = crd::memory::default_allocator()) noexcept;

// ---- Atomic publish + retention ----

// Write `bundle` to `path` atomically: bytes go to "<path>.tmp", are flushed to
// stable storage, then renamed over `path` (replace-existing). A crash mid-write
// leaves only the .tmp, never a half-final bundle read as whole. Returns true on
// success (on failure the .tmp is removed best-effort).
[[nodiscard]] bool write_bundle_atomic(const char*                         path,
                                       crd::containers::ConstSpan<crd::u8> bundle) noexcept;

// Enforce retention in `dir`: keep the newest `max_bundles` regular files whose
// name begins with `prefix` and ends with `suffix`, deleting the rest oldest-first
// (by last-write time). Returns the number deleted, or a negative value on a
// directory-open error. Post-mortem hygiene, not a hot path.
[[nodiscard]] crd::i64 enforce_bundle_retention(const char* dir, const char* prefix, const char* suffix,
                                                crd::u32 max_bundles = kDefaultMaxBundles) noexcept;

} // namespace crd::perf
