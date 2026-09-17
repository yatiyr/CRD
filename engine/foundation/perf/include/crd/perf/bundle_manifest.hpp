#pragma once

// ---------------------------------------------------------------------------
// crd-perf -- DIAG.5d(e1): the Manifest bundle section (tag 1) payload format.
//
// The manifest records bundle-level metadata: the cerid-diagnostics schema version, which sections
// are EXPLICITLY ABSENT (so a reader distinguishes "omitted on purpose" from "lost to truncation"),
// and whether the MANIFEST's OWN fields were redacted.
//
// ID-8 invariant (ADR-0133: "A raw dump is not sanitized merely because the accompanying manifest is
// redacted"). This is enforced STRUCTURALLY: there is NO field, bit, or API in this format that can
// declare the raw dump (the RawDump section) redacted or safe. kManifestFlagFieldsRedacted refers
// ONLY to the manifest's own strings. The flag set is a CLOSED known-mask (kManifestKnownFlags);
// unknown bits are REPORTED by the importer, never interpreted as a safety claim.
//
// On-disk (little-endian, pinned POD):
//   [ManifestHeader]                 -- versions + flags + absent_count
//   u32 absent_tags[absent_count]    -- BundleSectionTag values declared explicitly absent
//
// Contract: docs/design/runtime-diagnostics.md (DIAG.5d); ADR-0133 ID-1, ID-8.
// ---------------------------------------------------------------------------

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::perf
{

inline constexpr crd::u32 kManifestVersion = 1U;

// Manifest flag bits. The ONLY meaningful bit concerns the manifest's own strings; none can say anything about the
// raw dump (ID-8). kManifestKnownFlags is the closed mask the importer validates against.
inline constexpr crd::u32 kManifestFlagFieldsRedacted = 1U << 0; // the manifest's OWN string fields were redacted
inline constexpr crd::u32 kManifestKnownFlags         = kManifestFlagFieldsRedacted;

// A sane upper bound on how many absent-tag entries a manifest may declare (there are only a handful of section tags;
// this bounds a hostile absent_count before any allocation).
inline constexpr crd::u32 kMaxManifestAbsentTags = 256U;

struct ManifestHeader
{
    crd::u32 manifest_version; // 0  kManifestVersion
    crd::u32 schema_version;   // 4  cerid-diagnostics/N (== kDiagnosticSchemaVersion)
    crd::u32 flags;            // 8  kManifestFlag*
    crd::u32 absent_count;     // 12 number of u32 tags that follow
};

static_assert(sizeof(ManifestHeader) == 16, "ManifestHeader is 16 B; on-disk pin");
static_assert(alignof(ManifestHeader) == 4, "ManifestHeader is 4-aligned");

// The decoded/authored manifest (in-memory form).
struct BundleManifest
{
    crd::u32                         schema_version = 0;
    crd::u32                         flags          = 0;
    crd::containers::Array<crd::u32> absent_tags;
};

// Serialise a manifest to its section payload (header + absent_tags).
[[nodiscard]] crd::containers::Array<crd::u8>
serialize_manifest(const BundleManifest& m, crd::memory::IAllocator* alloc = crd::memory::default_allocator()) noexcept;

struct ManifestReadResult
{
    bool                             ok               = false;
    crd::u32                         manifest_version = 0;
    crd::u32                         schema_version   = 0;
    crd::u32                         flags            = 0;
    crd::u32                         unknown_flags    = 0; // flags & ~kManifestKnownFlags -- reported, never interpreted
    crd::containers::Array<crd::u32> absent_tags;
};

// Decode a manifest section payload. Untrusted: absent_count is bounded by kMaxManifestAbsentTags AND the payload
// size; a short/over-long/wrong-version payload yields ok == false. Never interprets unknown flag bits.
[[nodiscard]] ManifestReadResult
read_manifest(crd::containers::ConstSpan<crd::u8> payload,
              crd::memory::IAllocator* alloc = crd::memory::default_allocator()) noexcept;

} // namespace crd::perf
