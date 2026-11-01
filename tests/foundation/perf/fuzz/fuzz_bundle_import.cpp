// crd-fuzz-perf-bundle: import_bundle -- the trustworthy diagnostic-bundle importer -- under the bounded fuzz harness
// (REPO.DEV.9; docs/design/test-instruments.md; DIAG.5d(e2)). This is the fuzzed, corpus-replayed proof of the DIAG.5d
// acceptance clause "reject traversal / oversized / decompression-bomb inputs" (and "recover truncated bundles").
//
// Oracle (import_bundle is a bounded, non-executing READER, so the oracle is self-consistency, not a round-trip):
//   * it never crashes, never reads outside the input buffer (every non-empty raw section payload is a borrowed view
//     that lies strictly inside [data, data+size)); a compressed section is never also truncated, claims no more
//     than DEFLATE reaches from its stored bytes, and decodes to exactly its claim inside the import's own decoded
//     bytes, which hold nothing else, unless its claim passed a cap and it is flagged oversized instead (OWN.1);
//   * a reject code is set iff the status is Rejected;
//   * a non-rejected import carries the cerid-diagnostics schema (a schema mismatch always rejects);
//   * unsafe_names and manifest_absent_mismatches are bounded by what the input actually declared;
//   * importing the same bytes twice yields the same verdict (determinism).
// Seeds: the golden v1 bundle plus the adversarial corpus the acceptance names -- truncated (after the manifest and
// mid-section), a traversal SymbolIndex (debug_file ".."), an oversized section length, a decompression bomb (the
// compressed flag with a ~4 GiB original_length), a wrong cerid-diagnostics schema, and a hostile section_count (the
// pre-walk peek path added in (e1)), plus a DIAG.9b BuildIdentity section and one with a trailing byte, and a
// compressed section as the writer stores it and with its claim one byte long (OWN.1). crc32s are
// recomputed where a case must reach a deep check rather than BadHeader. A decoded build identity stays within its
// bounds and is never also reported refused.
#include <crd/fuzz/harness.hpp>

#include <crd/compression/deflate.hpp>
#include <crd/containers/array.hpp>
#include <crd/containers/checksum.hpp>
#include <crd/containers/string.hpp>
#include <crd/perf/build_identity.hpp>
#include <crd/perf/bundle.hpp>
#include <crd/perf/bundle_import.hpp>
#include <crd/perf/bundle_manifest.hpp>
#include <crd/perf/diagnostics.hpp>
#include <crd/perf/symbol_index.hpp>

#include <cstring>
#include <utility>

namespace
{
using crd::containers::Array;
using crd::containers::ConstSpan;
using crd::containers::String;
using crd::u32;
using crd::u64;
using crd::u8;
using crd::usize;
using namespace crd::perf;

template <typename T> ConstSpan<T> span_of(const T* p, usize n) noexcept
{
    return ConstSpan<T>{p, n};
}

// Rewrite header_crc32 over the first 36 bytes so a header field mutated above reaches the check it targets (schema
// mismatch, the section_count peek) instead of being rejected earlier as BadHeader.
void refresh_header_crc(Array<u8>& b) noexcept
{
    if (b.size() < sizeof(BundleHeader))
    {
        return;
    }
    const u32 crc = crd::containers::crc32(span_of(b.data(), 36U));
    std::memcpy(&b[36], &crc, sizeof(crc));
}

const u8 kCrash[16] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};

// The seeds from before the bundle compressed its sections keep their committed bytes: they are written raw, and the
// compressed seeds are written by the default writer.
BundleWriter raw_writer() noexcept
{
    return BundleWriter{crd::memory::default_allocator(), BundleLimits{}, BundleCompression::None};
}

Array<u8> build_golden()
{
    BundleManifest man;
    man.schema_version = kDiagnosticSchemaVersion;
    man.absent_tags.push_back(static_cast<u32>(BundleSectionTag::LogTail));
    man.absent_tags.push_back(static_cast<u32>(BundleSectionTag::SymbolIndex));
    const Array<u8> mp = serialize_manifest(man);

    BundleWriter w = raw_writer();
    w.add_section(BundleSectionTag::Manifest, span_of(mp.data(), mp.size()));
    w.add_section(BundleSectionTag::CrashRecord, span_of(kCrash, sizeof(kCrash)));
    w.add_absent(BundleSectionTag::LogTail);
    w.add_absent(BundleSectionTag::SymbolIndex);
    return w.finish(0U);
}

Array<u8> build_symbol_bundle_traversal()
{
    ModuleIdentity m;
    m.id_kind    = SymbolIdKind::Rsds;
    m.id_len     = 16U;
    m.name       = String("mod.dll");
    m.debug_file = String(".."); // a parent-dir traversal the importer must count, never resolve
    Array<ModuleIdentity> mods;
    mods.push_back(std::move(m));
    const Array<u8> payload = serialize_symbol_index(ConstSpan<ModuleIdentity>{mods.data(), mods.size()});

    BundleWriter w = raw_writer();
    w.add_section(BundleSectionTag::SymbolIndex, span_of(payload.data(), payload.size()));
    return w.finish(0U);
}

// A fixed build identity (never this lane's own: the committed seeds must be the same bytes on every lane).
Array<u8> build_identity_bundle(bool trailing_byte)
{
    BuildIdentity id;
    id.diagnostic_schema = kDiagnosticSchemaVersion;
    id.flags             = kBuildFlagAsserts | kBuildFlagFaultSeams;
    id.version           = String("0.1.0");
    id.platform          = String("WINDOWS");
    id.compiler          = String("MSVC");
    id.compiler_version  = String("msvc 195000000");
    id.arch              = String("X64");
    id.config            = String("debug");
    Array<u8> payload    = encode_build_identity(id);
    if (trailing_byte)
    {
        payload.push_back(0U);
    }
    BundleManifest man;
    man.schema_version = kDiagnosticSchemaVersion;
    man.absent_tags.push_back(static_cast<u32>(BundleSectionTag::SymbolIndex));
    const Array<u8> mp = serialize_manifest(man);

    BundleWriter w = raw_writer();
    w.add_section(BundleSectionTag::Manifest, span_of(mp.data(), mp.size()));
    w.add_section(BundleSectionTag::BuildIdentity, span_of(payload.data(), payload.size()));
    w.add_absent(BundleSectionTag::SymbolIndex);
    return w.finish(0U);
}

// The importer's self-consistency oracle, applied to every result.
void check_result(const BundleImport& r, const u8* data, usize size) noexcept
{
    CRD_FUZZ_REQUIRE((r.status == ImportStatus::Rejected) == (r.reject != ImportReject::None));
    // A build identity is either decoded within its bounds or refused, never both and never a plausible half.
    CRD_FUZZ_REQUIRE(!(r.has_build && r.build_refused));
    if (r.has_build)
    {
        CRD_FUZZ_REQUIRE((r.build.flags & ~kBuildKnownFlags) == 0U);
        CRD_FUZZ_REQUIRE(r.build.version.size() <= kBuildFieldBytes);
        CRD_FUZZ_REQUIRE(r.build.platform.size() <= kBuildFieldBytes);
        CRD_FUZZ_REQUIRE(r.build.compiler.size() <= kBuildFieldBytes);
        CRD_FUZZ_REQUIRE(r.build.compiler_version.size() <= kBuildFieldBytes);
        CRD_FUZZ_REQUIRE(r.build.arch.size() <= kBuildFieldBytes);
        CRD_FUZZ_REQUIRE(r.build.config.size() <= kBuildFieldBytes);
    }
    if (r.status != ImportStatus::Rejected)
    {
        CRD_FUZZ_REQUIRE(r.header.schema_version == kDiagnosticSchemaVersion);
    }
    CRD_FUZZ_REQUIRE(r.unsafe_names <= r.symbols.size());
    if (r.has_manifest)
    {
        CRD_FUZZ_REQUIRE(r.manifest_absent_mismatches <= r.manifest.absent_tags.size());
    }
    usize decoded = 0U;
    for (usize i = 0; i < r.sections.size(); ++i)
    {
        const ImportedSection& s = r.sections[i];
        const ConstSpan<u8>& p = s.payload;
        if ((s.section_flags & kSectionFlagCompressed) != 0U)
        {
            // Decoded into the import's own bytes, exactly its bounded claim; one whose claim passed a cap is left
            // undecoded and flagged oversized, and holds nothing.
            CRD_FUZZ_REQUIRE((s.section_flags & kSectionFlagTruncated) == 0U);
            CRD_FUZZ_REQUIRE(s.original_length <= crd::compression::deflate_max_inflated_size(s.stored_length));
            const bool hidden = (s.import_flags & kImportSectionOversized) != 0U || s.original_length == 0U;
            CRD_FUZZ_REQUIRE(p.empty() == hidden);
            CRD_FUZZ_REQUIRE(p.empty() || (p.size() == s.original_length && p.data() >= r.decoded.data() &&
                                           p.data() + p.size() <= r.decoded.data() + r.decoded.size()));
            decoded += p.size();
        }
        else if (p.size() != 0U) // a borrowed view -- it must lie strictly inside the input buffer
        {
            CRD_FUZZ_REQUIRE(p.data() >= data && p.data() + p.size() <= data + size);
        }
    }
    CRD_FUZZ_REQUIRE(decoded == r.decoded.size());
}
} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    if (size > crd::fuzz::kMaxInputBytes)
    {
        return 0;
    }

    crd::fuzz::BudgetAllocator root;

    const BundleImport a = import_bundle(ConstSpan<u8>(data, size), BundleLimits{}, &root);
    check_result(a, data, size);

    // Determinism: the same bytes must produce the same verdict.
    const BundleImport b = import_bundle(ConstSpan<u8>(data, size), BundleLimits{}, &root);
    CRD_FUZZ_REQUIRE(a.status == b.status && a.reject == b.reject);
    CRD_FUZZ_REQUIRE(a.sections.size() == b.sections.size());
    CRD_FUZZ_REQUIRE(a.unsafe_names == b.unsafe_names && a.has_manifest == b.has_manifest);
    CRD_FUZZ_REQUIRE(a.has_build == b.has_build && a.build_refused == b.build_refused);

    // Reject-path coverage: tight limits exercise every reject enum on the committed corpus -- a low total cap so the
    // larger seeds hit Oversized (which the 64 KiB kMaxInputBytes would otherwise leave as dead code), while the small
    // seeds fall through to TooManySections and the per-section byte cap.
    BundleLimits tight;
    tight.max_total_bytes   = 128U;
    tight.max_sections      = 2U;
    tight.max_section_bytes = 64U;
    const BundleImport c = import_bundle(ConstSpan<u8>(data, size), tight, &root);
    check_result(c, data, size);
    return 0;
}

void crd_fuzz_seeds(crd::fuzz::SeedSink& sink)
{
    const Array<u8> golden = build_golden();
    sink.add("golden-v1", golden.data(), golden.size());

    // Truncated right after the manifest section: header + section-0 header + manifest payload.
    {
        BundleManifest man;
        man.schema_version = kDiagnosticSchemaVersion;
        man.absent_tags.push_back(static_cast<u32>(BundleSectionTag::LogTail));
        man.absent_tags.push_back(static_cast<u32>(BundleSectionTag::SymbolIndex));
        const Array<u8> mp  = serialize_manifest(man);
        const usize     cut = sizeof(BundleHeader) + sizeof(BundleSectionHeader) + mp.size();
        if (cut < golden.size())
        {
            sink.add("truncated-after-manifest", golden.data(), cut);
        }
    }
    if (golden.size() > 3U)
    {
        sink.add("truncated-mid-section", golden.data(), golden.size() - 3U);
    }

    sink.add_text("empty", "");
    sink.add_text("garbage", "this is not a diagnostic bundle");

    // DIAG.9b: a flight bundle's BuildIdentity section, well formed and with a trailing byte its decoder must refuse.
    const Array<u8> build = build_identity_bundle(false);
    sink.add("build-identity", build.data(), build.size());
    const Array<u8> build_trailing = build_identity_bundle(true);
    sink.add("build-identity-trailing", build_trailing.data(), build_trailing.size());

    // Traversal: a SymbolIndex whose one module's debug_file is "..".
    const Array<u8> trav = build_symbol_bundle_traversal();
    sink.add("traversal-dotdot", trav.data(), trav.size());

    // Wrong cerid-diagnostics schema (crc recomputed so it reaches the schema check, not BadHeader).
    {
        Array<u8> b = golden;
        u32       sv = 0U;
        std::memcpy(&sv, &b[4], sizeof(sv)); // schema_version @ offset 4
        sv += 7U;
        std::memcpy(&b[4], &sv, sizeof(sv));
        refresh_header_crc(b);
        sink.add("wrong-schema", b.data(), b.size());
    }

    // Hostile section_count (crc recomputed so it reaches the pre-walk peek from (e1)).
    {
        Array<u8> b    = golden;
        const u32 huge = 0xFFFFFFFFU;
        std::memcpy(&b[32], &huge, sizeof(huge)); // section_count @ offset 32
        refresh_header_crc(b);
        sink.add("too-many-sections", b.data(), b.size());
    }

    // Oversized-length: section 0 claims a payload far larger than the buffer holds (a lying length).
    {
        Array<u8>   b    = golden;
        const usize s0   = sizeof(BundleHeader);
        const u64   huge = 0xFFFFFFFFULL;
        std::memcpy(&b[s0 + 8], &huge, sizeof(huge)); // BundleSectionHeader::payload_length @ +8
        sink.add("oversized-length", b.data(), b.size());
    }

    // Decompression bomb: a section flagged compressed with a ~4 GiB original_length and a tiny stored payload.
    {
        BundleWriter w = raw_writer();
        w.add_section(BundleSectionTag::CrashRecord, span_of(kCrash, sizeof(kCrash)));
        w.add_section(BundleSectionTag::RawDump, span_of(kCrash, sizeof(kCrash)));
        Array<u8>   b    = w.finish(0U);
        const usize s1   = sizeof(BundleHeader) + sizeof(BundleSectionHeader) + sizeof(kCrash);
        std::memcpy(&b[s1 + 4], &kSectionFlagCompressed, sizeof(u32)); // BundleSectionHeader::flags @ +4
        const u64 bomb = 4ULL * 1024 * 1024 * 1024;
        std::memcpy(&b[s1 + 16], &bomb, sizeof(bomb)); // BundleSectionHeader::original_length @ +16
        sink.add("bomb-compressed", b.data(), b.size());
    }

    // A compressed section the writer stored (a repetitive log tail after the raw crash bytes), and the same with its
    // claim one byte long: the decoder's exact-size refusal (OWN.1).
    {
        Array<u8> tail;
        for (u32 line = 0; line < 32U; ++line)
        {
            for (const char c : "frame complete, nothing to report")
            {
                tail.push_back(static_cast<u8>(c));
            }
        }
        BundleWriter w;
        w.add_section(BundleSectionTag::CrashRecord, span_of(kCrash, sizeof(kCrash)));
        w.add_section(BundleSectionTag::LogTail, span_of(tail.data(), tail.size()));
        Array<u8> b = w.finish(0U);
        sink.add("compressed", b.data(), b.size());
        const usize s1 = sizeof(BundleHeader) + sizeof(BundleSectionHeader) + sizeof(kCrash);
        const u64 claim = tail.size() + 1U;
        std::memcpy(&b[s1 + 16], &claim, sizeof(claim)); // BundleSectionHeader::original_length @ +16
        sink.add("compressed-claim-long", b.data(), b.size());
    }
}
