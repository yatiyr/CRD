#pragma once

// ---------------------------------------------------------------------------
// crd-perf -- DIAG.5d(e1): the trustworthy bundle importer.
//
// A BOUNDED, NON-EXECUTING reader over UNTRUSTED bundle bytes. It wraps read_bundle and adds the
// checks a merely-trusting reader lacks: oversized / too-many-sections / schema-mismatch rejection,
// a per-section byte cap, duplicate-tag and compressed-section handling, and decode of the Manifest
// (tag 1) and SymbolIndex (tag 5). It NEVER executes anything -- it decodes POD and bounds-checks;
// every payload it exposes BORROWS a view into `buf`, which must outlive the result. noexcept
// throughout (post-mortem code lets no exception escape).
//
// This is the acceptance's "CLI import/inspect is bounded and does not execute content" as a LIBRARY;
// the CLI itself is routed to a later slice (ADR: the bundle is "built by DIAG.5d/6c"). The adversarial
// corpus + libFuzzer target over import_bundle() is DIAG.5d(e2).
//
// Contract: docs/design/runtime-diagnostics.md (DIAG.5d); ADR-0133 ID-1, ID-8.
// ---------------------------------------------------------------------------

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>
#include <crd/perf/bundle.hpp>
#include <crd/perf/bundle_manifest.hpp>
#include <crd/perf/symbol_index.hpp>

namespace crd::perf
{

enum class ImportStatus : crd::u32
{
    Ok,                 // fully parsed; every declared section present and valid
    RecoveredTruncated, // a valid prefix recovered (truncation, or a refused compressed tail)
    Rejected,           // structurally unusable (see ImportReject)
};

enum class ImportReject : crd::u32
{
    None,
    Oversized,          // buf larger than limits.max_total_bytes (refused before reading)
    BadMagic,           // not a cerid-diagnostics bundle
    BadHeader,          // header too small or header crc mismatch
    UnsupportedVersion, // container format_version != current
    SchemaMismatch,     // cerid-diagnostics schema_version != current
    TooManySections,    // header.section_count > limits.max_sections
};

// Per-section import notes (reported; not fatal).
inline constexpr crd::u32 kImportSectionOversized = 1U << 0; // payload over limits.max_section_bytes -> not decoded
inline constexpr crd::u32 kImportSectionDuplicate = 1U << 1; // a later section repeats an earlier tag -> first wins

struct ImportedSection
{
    BundleSectionTag                    tag;
    crd::u32                            section_flags;   // bundle section flags (Absent/Truncated/Compressed)
    crd::u32                            import_flags;    // kImportSection*
    crd::u64                            original_length; // pre-truncation logical size
    crd::containers::ConstSpan<crd::u8> payload;         // borrowed; empty when oversized-skipped
};

struct BundleImport
{
    ImportStatus status = ImportStatus::Rejected;
    ImportReject reject = ImportReject::None;
    BundleHeader header{};
    crd::u32     declared_section_count = 0;
    crd::u64     stop_offset            = 0;

    bool     total_len_mismatch         = false; // header.total_len != buf.size() (reported, not fatal -- truncation)
    bool     compressed_unsupported     = false; // a section carried the compressed flag; prefix kept (v1 bomb defence)
    crd::u32 manifest_absent_mismatches = 0;     // manifest absent_tags with no matching Absent section (reported)
    crd::u32 unsafe_names               = 0;     // decoded symbols whose lookup name is a traversal (reported)

    bool               has_manifest = false;
    ManifestReadResult manifest;
    bool               has_symbols = false;

    crd::containers::Array<ModuleIdentity>  symbols;
    crd::containers::Array<ImportedSection> sections;
};

// Import `buf`. Rejects structurally-unusable input up front; otherwise returns the recovered sections (borrowing
// into `buf`) plus the decoded Manifest/SymbolIndex and a set of reported (non-fatal) observations.
[[nodiscard]] BundleImport import_bundle(crd::containers::ConstSpan<crd::u8> buf, BundleLimits limits = {},
                                         crd::memory::IAllocator* alloc = crd::memory::default_allocator()) noexcept;

} // namespace crd::perf
