// crd-fuzz-perf-bundle: import_bundle -- the trustworthy diagnostic-bundle importer -- under the bounded fuzz harness
// (REPO.DEV.9; docs/design/test-instruments.md; DIAG.5d(e2)). This is the fuzzed, corpus-replayed proof of the DIAG.5d
// acceptance clause "reject traversal / oversized / decompression-bomb inputs" (and "recover truncated bundles").
//
// Oracle (import_bundle is a bounded, non-executing READER, so the oracle is self-consistency, not a round-trip):
//   * it never crashes, never reads outside the input buffer (every non-empty section payload is a borrowed view
//     that lies strictly inside [data, data+size));
//   * a reject code is set iff the status is Rejected;
//   * a non-rejected import carries the cerid-diagnostics schema (a schema mismatch always rejects);
//   * unsafe_names and manifest_absent_mismatches are bounded by what the input actually declared;
//   * importing the same bytes twice yields the same verdict (determinism).
// Seeds: the golden v1 bundle plus the adversarial corpus the acceptance names -- truncated (after the manifest and
// mid-section), a traversal SymbolIndex (debug_file ".."), an oversized section length, a decompression bomb (the
// compressed flag with a ~4 GiB original_length), a wrong cerid-diagnostics schema, and a hostile section_count (the
// pre-walk peek path added in (e1)). crc32s are recomputed where a case must reach a deep check rather than BadHeader.
#include <crd/fuzz/harness.hpp>

#include <crd/containers/array.hpp>
#include <crd/containers/string.hpp>
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

u32 crc32(const u8* p, usize n) noexcept
{
    u32 c = 0xFFFFFFFFU;
    for (usize i = 0; i < n; ++i)
    {
        c ^= p[i];
        for (int k = 0; k < 8; ++k)
            c = (c & 1U) ? (0xEDB88320U ^ (c >> 1)) : (c >> 1);
    }
    return c ^ 0xFFFFFFFFU;
}

// Rewrite header_crc32 over the first 36 bytes so a header field mutated above reaches the check it targets (schema
// mismatch, the section_count peek) instead of being rejected earlier as BadHeader.
void refresh_header_crc(Array<u8>& b) noexcept
{
    if (b.size() < sizeof(BundleHeader))
        return;
    const u32 crc = crc32(b.data(), 36U);
    std::memcpy(&b[36], &crc, sizeof(crc));
}

const u8 kCrash[16] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};

Array<u8> build_golden()
{
    BundleManifest man;
    man.schema_version = kDiagnosticSchemaVersion;
    man.absent_tags.push_back(static_cast<u32>(BundleSectionTag::LogTail));
    man.absent_tags.push_back(static_cast<u32>(BundleSectionTag::SymbolIndex));
    const Array<u8> mp = serialize_manifest(man);

    BundleWriter w;
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

    BundleWriter w;
    w.add_section(BundleSectionTag::SymbolIndex, span_of(payload.data(), payload.size()));
    return w.finish(0U);
}

// The importer's self-consistency oracle, applied to every result.
void check_result(const BundleImport& r, const u8* data, usize size) noexcept
{
    CRD_FUZZ_REQUIRE((r.status == ImportStatus::Rejected) == (r.reject != ImportReject::None));
    if (r.status != ImportStatus::Rejected)
        CRD_FUZZ_REQUIRE(r.header.schema_version == kDiagnosticSchemaVersion);
    CRD_FUZZ_REQUIRE(r.unsafe_names <= r.symbols.size());
    if (r.has_manifest)
        CRD_FUZZ_REQUIRE(r.manifest_absent_mismatches <= r.manifest.absent_tags.size());
    for (usize i = 0; i < r.sections.size(); ++i)
    {
        const ConstSpan<u8>& p = r.sections[i].payload;
        if (p.size() != 0U) // a borrowed view -- it must lie strictly inside the input buffer
            CRD_FUZZ_REQUIRE(p.data() >= data && p.data() + p.size() <= data + size);
    }
}
} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    if (size > crd::fuzz::kMaxInputBytes)
        return 0;

    crd::fuzz::BudgetAllocator root;

    const BundleImport a = import_bundle(ConstSpan<u8>(data, size), BundleLimits{}, &root);
    check_result(a, data, size);

    // Determinism: the same bytes must produce the same verdict.
    const BundleImport b = import_bundle(ConstSpan<u8>(data, size), BundleLimits{}, &root);
    CRD_FUZZ_REQUIRE(a.status == b.status && a.reject == b.reject);
    CRD_FUZZ_REQUIRE(a.sections.size() == b.sections.size());
    CRD_FUZZ_REQUIRE(a.unsafe_names == b.unsafe_names && a.has_manifest == b.has_manifest);

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
            sink.add("truncated-after-manifest", golden.data(), cut);
    }
    if (golden.size() > 3U)
        sink.add("truncated-mid-section", golden.data(), golden.size() - 3U);

    sink.add_text("empty", "");
    sink.add_text("garbage", "this is not a diagnostic bundle");

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
        BundleWriter w;
        w.add_section(BundleSectionTag::CrashRecord, span_of(kCrash, sizeof(kCrash)));
        w.add_section(BundleSectionTag::RawDump, span_of(kCrash, sizeof(kCrash)));
        Array<u8>   b    = w.finish(0U);
        const usize s1   = sizeof(BundleHeader) + sizeof(BundleSectionHeader) + sizeof(kCrash);
        std::memcpy(&b[s1 + 4], &kSectionFlagCompressed, sizeof(u32)); // BundleSectionHeader::flags @ +4
        const u64 bomb = 4ULL * 1024 * 1024 * 1024;
        std::memcpy(&b[s1 + 16], &bomb, sizeof(bomb)); // BundleSectionHeader::original_length @ +16
        sink.add("bomb-compressed", b.data(), b.size());
    }
}
