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
// drops. The caps bound the DECODED bundle (what a reader holds), so compression
// shrinks the file and never admits more content. Retention keeps at most
// kDefaultMaxBundles (10) bundles in a directory.
//
// Compression (OWN.1, through crd-compression's own DEFLATE): v1 defined
// kSectionFlagCompressed from the start as "the payload is raw DEFLATE", and the
// writer now produces it. A section stored whole is compressed when DEFLATE makes
// it smaller (else it is stored raw, the ZIP container's rule); its
// original_length is then the decoded size and payload_crc32 covers the stored,
// compressed bytes. A section cut to a cap is stored raw: Compressed and
// Truncated never meet. The reader decodes a compressed section into an
// exact-size buffer only after bounding its claim (what DEFLATE can reach from
// its bytes, the section cap and the total cap), so a decompression bomb is
// refused or left undecoded before anything is allocated (read_bundle). The
// container layout is unchanged, so the format stays v1: a reader
// from before the codec edge refuses such a section, as it was built to, and
// keeps the prefix.
//
// Contract: docs/design/runtime-diagnostics.md (DIAG.5d); ADR-0133 ID-1.
// ---------------------------------------------------------------------------

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/platform.hpp>
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
// NOLINTNEXTLINE(performance-enum-size): the on-disk tag field is u32 and modules register tags above 0x00010000.
enum class BundleSectionTag : crd::u32
{
    Invalid       = 0,
    Manifest      = 1, // cerid-diagnostics manifest (redaction-honest; never claims a raw dump is redacted)
    CrashRecord   = 2, // the 5a/5b async-signal-safe record bytes
    RawDump       = 3, // Windows minidump / Linux core (raw; ID-8: never *declared* redacted)
    LogTail       = 4, // recent log records (bytes; the composer that fills it is a later slice)
    SymbolIndex   = 5, // module identities (filled by (c))
    FlightRecord  = 6, // DIAG.9b triggered flight capture (flight_recorder.hpp: encode/decode_flight_record)
    BuildIdentity = 7, // DIAG.9b the writing build (build_identity.hpp: encode/decode_build_identity)
    HardwareIdentity = 8, // HW-RESILIENCE.2 the writing machine (hardware_identity.hpp: encode/decode)
    // 9 .. 0x0000FFFF reserved for foundation; modules register above 0x00010000.
};

// Per-section flags (bitset in BundleSectionHeader::flags).
inline constexpr crd::u32 kSectionFlagAbsent     = 1U << 0; // tag present, payload_length 0 (explicit absent section)
inline constexpr crd::u32 kSectionFlagTruncated  = 1U << 1; // payload cut to a cap; original_length = pre-cut size
inline constexpr crd::u32 kSectionFlagCompressed = 1U << 2; // payload is raw DEFLATE; original_length = decoded size

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
static_assert(CRD_LITTLE_ENDIAN, "the bundle is copied as host bytes");

struct BundleSectionHeader
{
    crd::u32 tag;             // 4  BundleSectionTag
    crd::u32 flags;           // 4  kSectionFlag*
    crd::u64 payload_length;  // 8  bytes of payload stored immediately after this header
    crd::u64 original_length; // 8  logical size: before truncation, or decoded when compressed (else payload_length)
    crd::u32 payload_crc32;   // 4  crc32 of the stored payload bytes (0 when payload_length == 0)
    crd::u32 _pad_a;          // 4  reserved (0) NOLINT(readability-identifier-naming): pinned on-disk spare slot
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

// How the writer stores a section it keeps whole.
enum class BundleCompression : crd::u8
{
    None,    // every section raw
    Deflate, // raw DEFLATE when it makes the section smaller (kSectionFlagCompressed), else raw
};

// ---- Writer ----
//
// Accumulates sections into an in-memory buffer, then serialises a self-contained
// bundle. Enforces per-section + total caps with honest truncation markers.
// noexcept throughout: post-mortem code must not let an exception escape.
class BundleWriter
{
public:
    enum class AddStatus : crd::u8
    {
        Ok,              // stored in full
        StoredTruncated, // stored, truncated to fit a cap; Truncated flag + original_length set
        StoredAbsent,    // explicit absent section recorded (payload_length 0)
        RejectedTooMany, // max_sections already reached; nothing stored
        RejectedNoRoom,  // no room even for the section header within max_total_bytes; nothing stored
    };

    explicit BundleWriter(crd::memory::IAllocator* alloc = crd::memory::default_allocator(), BundleLimits limits = {},
                          BundleCompression compression = BundleCompression::Deflate) noexcept;

    // Append a section. `bytes` beyond the per-section cap OR the remaining total
    // room is stored truncated and raw (kSectionFlagTruncated + original_length =
    // bytes.size()); a section that fits is stored whole, compressed when the
    // writer's BundleCompression makes it smaller. An empty span records an
    // explicit absent section.
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
    // The serialised size so far (what finish() returns), and the decoded size a reader holds, which the caps bound.
    [[nodiscard]] crd::u64 total_bytes() const noexcept { return m_total; }
    [[nodiscard]] crd::u64 decoded_bytes() const noexcept { return m_decoded; }

private:
    // Append one section header + payload (already clamped) to m_sections.
    void emit(BundleSectionTag tag, crd::u32 flags, crd::containers::ConstSpan<crd::u8> payload,
              crd::u64 original_length) noexcept;

    crd::memory::IAllocator*        m_alloc;
    BundleLimits                    m_limits;
    BundleCompression               m_compression;
    crd::containers::Array<crd::u8> m_sections;                         // concatenated [hdr][payload]... (no file header)
    crd::u32                        m_section_count = 0;
    crd::u64                        m_total         = sizeof(BundleHeader); // running serialised size
    crd::u64                        m_decoded       = sizeof(BundleHeader); // running decoded size (the caps bound it)
    bool                            m_finished      = false;
};

// ---- Reader (round-trip + truncation recovery) ----
//
// This is (b)'s minimal reader: it round-trips the writer's output and recovers
// a truncated bundle's complete prefix. The BOUNDED, NON-EXECUTING, adversarial
// importer (traversal / oversized / bomb / version-mismatch rejection, golden
// manifests, fuzz corpus) is (e); it builds on these types.

enum class BundleReadStatus : crd::u8
{
    Ok,                 // header valid and every declared section present + crc-verified
    RecoveredTruncated, // header valid; a valid prefix of sections recovered, then parsing stopped
    BadMagic,           // not a cerid-diagnostics bundle
    HeaderTooSmall,     // fewer than sizeof(BundleHeader) bytes, or the header crc mismatched
    UnsupportedVersion, // format_version != kBundleFormatVersion
    RefusedCompressed,  // a compressed section refused, the valid prefix kept: it claimed more than DEFLATE reaches
                        // from its bytes, it was flagged truncated too, its stream did not decode to the claim, or
                        // the decoded bytes could not be allocated
};

struct BundleSectionView
{
    BundleSectionTag                    tag;
    crd::u32                            flags;
    crd::u64                            original_length; // pre-truncation logical size (decoded size when compressed)
    crd::u64                            stored_length;   // bytes stored in the bundle (compressed when compressed)
    crd::containers::ConstSpan<crd::u8> payload;         // the section's decoded bytes: borrowed from the input
                                                         // buffer, or from BundleReadResult::decoded when compressed;
                                                         // empty for a compressed section left undecoded (a cap)
};

struct BundleReadResult
{
    explicit BundleReadResult(crd::memory::IAllocator* alloc) noexcept : sections(alloc), decoded(alloc) {}

    BundleReadStatus                          status = BundleReadStatus::Ok;
    BundleHeader                              header{};                   // valid iff not BadMagic/HeaderTooSmall
    crd::u32                                  declared_section_count = 0; // header.section_count as read
    crd::u64                                  stop_offset            = 0; // byte offset where parsing stopped
    crd::containers::Array<BundleSectionView> sections;               // the recovered (valid, crc-checked) prefix
    crd::containers::Array<crd::u8>           decoded;                // the compressed sections' decoded bytes
};

// Parse `buf`. Never executes anything; the returned views borrow into `buf`,
// which must outlive the result, and into the result's `decoded` (moving the
// result or its `decoded` keeps them valid). Stops at the first
// structurally-invalid section (short header, short payload, crc mismatch) and
// returns the recovered prefix (RecoveredTruncated) rather than failing the whole
// read. The compressed sections decode into one allocation of exactly their
// claims, bounded before it is made (the decompression-bomb defence): a claim
// past what DEFLATE reaches from the stored bytes stops the read with
// RefusedCompressed, and a section whose decoded size would pass
// max_section_bytes, or take the decoded bundle past max_total_bytes, is listed
// undecoded (an empty payload, original_length its claim) and the walk goes on.
[[nodiscard]] BundleReadResult read_bundle(crd::containers::ConstSpan<crd::u8> buf, BundleLimits limits = {},
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

// Whether `path` names an existing directory, and whether anything (a file or a directory) exists at `path`. A null
// path is neither. Used by bundle writers to check a capture directory and to never write over an existing name.
[[nodiscard]] bool bundle_directory_exists(const char* path) noexcept;
[[nodiscard]] bool bundle_path_exists(const char* path) noexcept;

} // namespace crd::perf
