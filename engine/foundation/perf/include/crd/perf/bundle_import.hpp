#pragma once

// ---------------------------------------------------------------------------
// crd-perf -- DIAG.5d(e1): the trustworthy bundle importer.
//
// A BOUNDED, NON-EXECUTING reader over UNTRUSTED bundle bytes. It wraps read_bundle and adds the
// checks a merely-trusting reader lacks: oversized / too-many-sections / schema-mismatch rejection,
// a per-section byte cap, duplicate-tag detection, bounded decoding of compressed sections, and decode of the
// Manifest (tag 1), SymbolIndex (tag 5), BuildIdentity (tag 7) and HardwareIdentity (tag 8). It NEVER executes
// anything -- it decodes POD and bounds-checks; every payload it exposes BORROWS a view into `buf`, which must outlive
// the result, or into the result's own `decoded` bytes (a compressed section).
// noexcept throughout (post-mortem code lets no exception escape).
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
#include <crd/perf/build_identity.hpp>
#include <crd/perf/bundle.hpp>
#include <crd/perf/bundle_manifest.hpp>
#include <crd/perf/hardware_identity.hpp>
#include <crd/perf/state_hash.hpp>
#include <crd/perf/symbol_index.hpp>

namespace crd::perf
{

enum class ImportStatus : crd::u8
{
    Ok,                 // fully parsed; every declared section present and valid
    RecoveredTruncated, // a valid prefix recovered (truncation, or a refused compressed section)
    Rejected,           // structurally unusable (see ImportReject)
};

enum class ImportReject : crd::u8
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
// kImportSectionOversized: a payload over limits.max_section_bytes, or a compressed section the read left undecoded
// (its claim passed the section or the total cap: the decompression-bomb defence) -> not exposed, not decoded.
inline constexpr crd::u32 kImportSectionOversized = 1U << 0;
inline constexpr crd::u32 kImportSectionDuplicate = 1U << 1; // a later section repeats an earlier tag -> first wins

struct ImportedSection
{
    BundleSectionTag                    tag;
    crd::u32                            section_flags;   // bundle section flags (Absent/Truncated/Compressed)
    crd::u32                            import_flags;    // kImportSection*
    crd::u64                            original_length; // pre-truncation logical size (decoded when compressed)
    crd::u64                            stored_length;   // payload bytes in the bundle (compressed when compressed)
    crd::containers::ConstSpan<crd::u8> payload;         // decoded bytes, borrowed; empty when oversized-skipped
};

struct BundleImport
{
    ImportStatus status = ImportStatus::Rejected;
    ImportReject reject = ImportReject::None;
    BundleHeader header{};
    crd::u32     declared_section_count = 0;
    crd::u64     stop_offset            = 0;

    bool     total_len_mismatch         = false; // header.total_len != buf.size() (reported, not fatal -- truncation)
    bool     compressed_refused         = false; // a compressed section refused (a lying claim or stream); prefix kept
    crd::u32 manifest_absent_mismatches = 0;     // manifest absent_tags with no matching Absent section (reported)
    crd::u32 unsafe_names               = 0;     // decoded symbols whose lookup name is a traversal (reported)

    bool               has_manifest = false;
    ManifestReadResult manifest;
    bool               has_symbols   = false; // a SymbolIndex section with a payload (an explicit absent one: false)
    bool               has_build     = false; // a BuildIdentity section (tag 7) decoded into `build`
    bool               build_refused = false; // a BuildIdentity section was present and its decoder refused it
    BuildIdentity      build;
    bool               has_hardware     = false; // a HardwareIdentity section (tag 8) decoded into `hardware`
    bool               hardware_refused = false; // a HardwareIdentity section was present and its decoder refused it
    HardwareIdentity   hardware;

    bool has_state_ledger = false;                  // a StateHashLedger section (tag 9) decoded into `state_ledger`
    bool state_ledger_refused = false;              // a StateHashLedger section was present and its decoder refused it
    crd::containers::StringView state_ledger_error; // why (static text)
    StateHashLedger state_ledger{crd::containers::StringView{}, 1U};

    crd::containers::Array<ModuleIdentity>  symbols;
    crd::containers::Array<ImportedSection> sections;
    crd::containers::Array<crd::u8> decoded; // the compressed sections' decoded bytes their payloads view
};

// Import `buf`. Rejects structurally-unusable input up front; otherwise returns the recovered sections (borrowing
// into `buf` and the result's `decoded`, whose claims `limits` bounds before they are allocated) plus the decoded
// Manifest/SymbolIndex/BuildIdentity and a set of reported (non-fatal) observations.
[[nodiscard]] BundleImport import_bundle(crd::containers::ConstSpan<crd::u8> buf, BundleLimits limits = {},
                                         crd::memory::IAllocator* alloc = crd::memory::default_allocator()) noexcept;

} // namespace crd::perf
