// DIAG.5d(e1): the trustworthy bundle importer + Manifest format + golden bundles. Verifies the acceptance clauses
// this slice owns: reject oversized / version-mismatch / decompression-bomb (compressed flag) / too-many-sections,
// recover truncated bundles, and "golden manifests preserve schema migration and explicit absent sections". The
// manifest is redaction-honest by construction (no field can claim the raw dump is redacted). Traversal rejection is
// exercised in test_diag_symbol_index.cpp (debug_file_path / is_safe_lookup_name); here the importer COUNTS unsafe
// names. Adversarial corpus + libFuzzer is DIAG.5d(e2).

#include <crd/containers/array.hpp>
#include <crd/containers/string.hpp>
#include <crd/perf/bundle.hpp>
#include <crd/perf/bundle_import.hpp>
#include <crd/perf/bundle_manifest.hpp>
#include <crd/perf/diagnostics.hpp>
#include <crd/perf/symbol_index.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstring>
#include <utility>

namespace cont = crd::containers;
using namespace crd::perf;

namespace
{
template <typename T> cont::ConstSpan<T> span_of(const T* p, crd::usize n) noexcept
{
    return cont::ConstSpan<T>{p, n};
}

crd::u32 test_crc32(const crd::u8* p, crd::usize n) noexcept
{
    crd::u32 c = 0xFFFFFFFFU;
    for (crd::usize i = 0; i < n; ++i)
    {
        c ^= p[i];
        for (int k = 0; k < 8; ++k)
            c = (c & 1U) ? (0xEDB88320U ^ (c >> 1)) : (c >> 1);
    }
    return c ^ 0xFFFFFFFFU;
}

const crd::u8 kCrashBytes[16] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};

// A deterministic golden bundle: Manifest (section 0, declaring LogTail+SymbolIndex absent) + a known CrashRecord +
// the two absent sections. finish(0) stamps a fixed timestamp so the bytes are reproducible.
cont::Array<crd::u8> build_golden()
{
    BundleManifest man;
    man.schema_version = kDiagnosticSchemaVersion;
    man.flags          = 0U;
    man.absent_tags.push_back(static_cast<crd::u32>(BundleSectionTag::LogTail));
    man.absent_tags.push_back(static_cast<crd::u32>(BundleSectionTag::SymbolIndex));
    const cont::Array<crd::u8> mpayload = serialize_manifest(man);

    BundleWriter w;
    REQUIRE(w.add_section(BundleSectionTag::Manifest, span_of(mpayload.data(), mpayload.size())) ==
            BundleWriter::AddStatus::Ok);
    REQUIRE(w.add_section(BundleSectionTag::CrashRecord, span_of(kCrashBytes, sizeof(kCrashBytes))) ==
            BundleWriter::AddStatus::Ok);
    REQUIRE(w.add_absent(BundleSectionTag::LogTail) == BundleWriter::AddStatus::StoredAbsent);
    REQUIRE(w.add_absent(BundleSectionTag::SymbolIndex) == BundleWriter::AddStatus::StoredAbsent);
    return w.finish(0U);
}

// Frozen golden v1 bytes (captured once from build_golden() with finish(0)). The drift guard below rebuilds and
// compares: any format change flips this test, forcing a deliberate schema bump + a re-freeze (the migration gate).
const crd::u8 kGoldenV1[] = {
    67,  68,  66,  49,  1,   0,   0,   0,   1,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   208, 0,   0,   0,   0,   0,   0,   0,   4,   0,   0,   0,   158, 4,   0,   42,  1,   0,   0,   0,
    0,   0,   0,   0,   24,  0,   0,   0,   0,   0,   0,   0,   24,  0,   0,   0,   0,   0,   0,   0,   22,  244,
    44,  24,  0,   0,   0,   0,   1,   0,   0,   0,   1,   0,   0,   0,   0,   0,   0,   0,   2,   0,   0,   0,
    4,   0,   0,   0,   5,   0,   0,   0,   2,   0,   0,   0,   0,   0,   0,   0,   16,  0,   0,   0,   0,   0,
    0,   0,   16,  0,   0,   0,   0,   0,   0,   0,   136, 226, 206, 206, 0,   0,   0,   0,   0,   1,   2,   3,
    4,   5,   6,   7,   8,   9,   10,  11,  12,  13,  14,  15,  4,   0,   0,   0,   1,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    5,   0,   0,   0,   1,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
};
} // namespace

TEST_CASE("manifest round-trips and reports unknown flags without interpreting them", "[perf][diag][import]")
{
    BundleManifest man;
    man.schema_version = kDiagnosticSchemaVersion;
    man.flags          = kManifestFlagFieldsRedacted;
    man.absent_tags.push_back(4U);
    man.absent_tags.push_back(5U);
    const cont::Array<crd::u8> payload = serialize_manifest(man);

    const ManifestReadResult r = read_manifest(span_of(payload.data(), payload.size()));
    REQUIRE(r.ok);
    CHECK(r.manifest_version == kManifestVersion);
    CHECK(r.schema_version == kDiagnosticSchemaVersion);
    CHECK((r.flags & kManifestFlagFieldsRedacted) != 0U);
    CHECK(r.unknown_flags == 0U);
    REQUIRE(r.absent_tags.size() == 2U);
    CHECK(r.absent_tags[0] == 4U);
    CHECK(r.absent_tags[1] == 5U);

    SECTION("unknown flag bits are reported, never interpreted (ID-8: no raw-dump-redacted claim exists)")
    {
        cont::Array<crd::u8> bad = payload;
        const crd::u32       all = 0xFFFFFFFFU;
        std::memcpy(&bad[offsetof(ManifestHeader, flags)], &all, sizeof(all));
        const ManifestReadResult rr = read_manifest(span_of(bad.data(), bad.size()));
        REQUIRE(rr.ok);
        CHECK(rr.unknown_flags == (0xFFFFFFFFU & ~kManifestKnownFlags)); // surfaced, not acted on
    }
    SECTION("a hostile absent_count is refused, not trusted")
    {
        cont::Array<crd::u8> bad = payload;
        const crd::u32       huge = 0xFFFFFFFFU;
        std::memcpy(&bad[offsetof(ManifestHeader, absent_count)], &huge, sizeof(huge));
        CHECK_FALSE(read_manifest(span_of(bad.data(), bad.size())).ok);
    }
    SECTION("a short payload is rejected")
    {
        CHECK_FALSE(read_manifest(span_of(payload.data(), 8)).ok);
    }
}

TEST_CASE("golden bundle: schema + explicit absent sections survive import", "[perf][diag][import]")
{
    const cont::Array<crd::u8> g = build_golden();

    const BundleImport imp = import_bundle(span_of(g.data(), g.size()));
    REQUIRE(imp.status == ImportStatus::Ok);
    REQUIRE(imp.reject == ImportReject::None);
    REQUIRE(imp.sections.size() == 4U);

    REQUIRE(imp.has_manifest);
    CHECK(imp.manifest.schema_version == kDiagnosticSchemaVersion);
    REQUIRE(imp.manifest.absent_tags.size() == 2U);
    CHECK(imp.manifest.absent_tags[0] == static_cast<crd::u32>(BundleSectionTag::LogTail));
    CHECK(imp.manifest.absent_tags[1] == static_cast<crd::u32>(BundleSectionTag::SymbolIndex));
    CHECK(imp.manifest_absent_mismatches == 0U); // the manifest agrees with the actual Absent sections

    CHECK(imp.sections[1].tag == BundleSectionTag::CrashRecord);
    REQUIRE(imp.sections[1].payload.size() == sizeof(kCrashBytes));
    CHECK(std::memcmp(imp.sections[1].payload.data(), kCrashBytes, sizeof(kCrashBytes)) == 0);
    CHECK((imp.sections[2].section_flags & kSectionFlagAbsent) != 0U); // LogTail explicitly absent
    CHECK((imp.sections[3].section_flags & kSectionFlagAbsent) != 0U); // SymbolIndex explicitly absent
}

TEST_CASE("golden bundle: the format has not drifted (migration guard)", "[perf][diag][import]")
{
    const cont::Array<crd::u8> g = build_golden();
    REQUIRE(g.size() == sizeof(kGoldenV1)); // a size change means the layout changed -> bump kSymbolIndex/schema
    CHECK(std::memcmp(g.data(), kGoldenV1, sizeof(kGoldenV1)) == 0);
}

TEST_CASE("golden bundle: truncated after the manifest recovers the manifest, drops the rest", "[perf][diag][import]")
{
    const cont::Array<crd::u8> g = build_golden();
    // Cut just past section 0 (Manifest): header + section-header + manifest payload.
    const cont::Array<crd::u8> man = serialize_manifest([] {
        BundleManifest m;
        m.schema_version = kDiagnosticSchemaVersion;
        m.absent_tags.push_back(static_cast<crd::u32>(BundleSectionTag::LogTail));
        m.absent_tags.push_back(static_cast<crd::u32>(BundleSectionTag::SymbolIndex));
        return m;
    }());
    const crd::usize cut = sizeof(BundleHeader) + sizeof(BundleSectionHeader) + man.size();

    const BundleImport imp = import_bundle(span_of(g.data(), cut));
    CHECK(imp.status == ImportStatus::RecoveredTruncated);
    REQUIRE(imp.has_manifest); // the manifest (section 0) is intact
    CHECK(imp.sections.size() == 1U);
    CHECK(imp.manifest.absent_tags.size() == 2U);
}

TEST_CASE("golden bundle: an N+1 container version is rejected", "[perf][diag][import]")
{
    cont::Array<crd::u8> g = build_golden();
    BundleHeader         hdr{};
    std::memcpy(&hdr, g.data(), sizeof(hdr));
    hdr.format_version = kBundleFormatVersion + 1U;
    hdr.header_crc32   = test_crc32(reinterpret_cast<const crd::u8*>(&hdr), offsetof(BundleHeader, header_crc32));
    std::memcpy(g.data(), &hdr, sizeof(hdr));

    const BundleImport imp = import_bundle(span_of(g.data(), g.size()));
    CHECK(imp.status == ImportStatus::Rejected);
    CHECK(imp.reject == ImportReject::UnsupportedVersion); // N-1 reader is future work; v1 is the first version
}

TEST_CASE("importer rejects structurally unusable bundles", "[perf][diag][import]")
{
    const cont::Array<crd::u8> good = build_golden();

    SECTION("oversized: refused before reading")
    {
        BundleLimits lim;
        lim.max_total_bytes = good.size() - 1U;
        const BundleImport imp = import_bundle(span_of(good.data(), good.size()), lim);
        CHECK(imp.status == ImportStatus::Rejected);
        CHECK(imp.reject == ImportReject::Oversized);
    }
    SECTION("bad magic")
    {
        cont::Array<crd::u8> bad = good;
        bad[0] ^= 0xFFU;
        CHECK(import_bundle(span_of(bad.data(), bad.size())).reject == ImportReject::BadMagic);
    }
    SECTION("too many sections: bounds the walk")
    {
        BundleLimits lim;
        lim.max_sections = 2U; // golden has 4
        const BundleImport imp = import_bundle(span_of(good.data(), good.size()), lim);
        CHECK(imp.status == ImportStatus::Rejected);
        CHECK(imp.reject == ImportReject::TooManySections);
    }
    SECTION("schema mismatch: a wrong cerid-diagnostics schema is refused")
    {
        cont::Array<crd::u8> bad = good;
        BundleHeader         hdr{};
        std::memcpy(&hdr, bad.data(), sizeof(hdr));
        hdr.schema_version = kDiagnosticSchemaVersion + 7U;
        hdr.header_crc32   = test_crc32(reinterpret_cast<const crd::u8*>(&hdr), offsetof(BundleHeader, header_crc32));
        std::memcpy(bad.data(), &hdr, sizeof(hdr));
        CHECK(import_bundle(span_of(bad.data(), bad.size())).reject == ImportReject::SchemaMismatch);
    }
}

TEST_CASE("importer flags an oversized section but keeps the rest", "[perf][diag][import]")
{
    // A bundle with a big-ish section, imported under a tiny per-section cap.
    cont::Array<crd::u8> big;
    big.resize(2048, static_cast<crd::u8>(0xCD));
    BundleWriter w;
    w.add_section(BundleSectionTag::CrashRecord, span_of(big.data(), big.size()));
    w.add_section(BundleSectionTag::RawDump, span_of(kCrashBytes, sizeof(kCrashBytes)));
    const cont::Array<crd::u8> b = w.finish(0U);

    BundleLimits lim;
    lim.max_section_bytes = 1024U; // < 2048 -> the first section is flagged, not decoded
    const BundleImport imp = import_bundle(span_of(b.data(), b.size()), lim);
    REQUIRE(imp.status == ImportStatus::Ok);
    REQUIRE(imp.sections.size() == 2U);
    CHECK((imp.sections[0].import_flags & kImportSectionOversized) != 0U);
    CHECK(imp.sections[0].payload.size() == 0U);            // not exposed
    CHECK(imp.sections[1].payload.size() == sizeof(kCrashBytes)); // the sibling survives
}

TEST_CASE("importer refuses a compressed section (v1 decompression-bomb defence)", "[perf][diag][import]")
{
    BundleWriter w;
    w.add_section(BundleSectionTag::CrashRecord, span_of(kCrashBytes, sizeof(kCrashBytes)));
    w.add_section(BundleSectionTag::RawDump, span_of(kCrashBytes, sizeof(kCrashBytes)));
    cont::Array<crd::u8> b = w.finish(0U);

    // Set the compressed flag on section 1 (flags field is at section-header offset 4; header + section0 payload).
    const crd::usize f1 = sizeof(BundleHeader) + sizeof(BundleSectionHeader) + sizeof(kCrashBytes) + 4U;
    std::memcpy(&b[f1], &kSectionFlagCompressed, 4U);

    const BundleImport imp = import_bundle(span_of(b.data(), b.size()));
    CHECK(imp.compressed_unsupported);              // a compressed section is refused (no codec edge in v1)
    CHECK(imp.status == ImportStatus::RecoveredTruncated);
    CHECK(imp.sections.size() == 1U);               // section 0 recovered before the refusal
}

TEST_CASE("importer reports duplicate tags, unsafe symbol names, and manifest/section disagreement",
          "[perf][diag][import]")
{
    SECTION("a duplicate tag: the first section wins, the second is flagged")
    {
        const crd::u8 a[4] = {0xAA, 0xAA, 0xAA, 0xAA};
        const crd::u8 b[4] = {0xBB, 0xBB, 0xBB, 0xBB};
        BundleWriter  w;
        REQUIRE(w.add_section(BundleSectionTag::CrashRecord, span_of(a, sizeof(a))) == BundleWriter::AddStatus::Ok);
        REQUIRE(w.add_section(BundleSectionTag::CrashRecord, span_of(b, sizeof(b))) == BundleWriter::AddStatus::Ok);
        const cont::Array<crd::u8> bytes = w.finish(0U);

        const BundleImport imp = import_bundle(span_of(bytes.data(), bytes.size()));
        REQUIRE(imp.status == ImportStatus::Ok);
        REQUIRE(imp.sections.size() == 2U);
        CHECK((imp.sections[0].import_flags & kImportSectionDuplicate) == 0U);
        CHECK((imp.sections[1].import_flags & kImportSectionDuplicate) != 0U); // second occurrence flagged
        REQUIRE(imp.sections[0].payload.size() == sizeof(a));
        CHECK(imp.sections[0].payload.data()[0] == 0xAAU); // first section's bytes, not the duplicate's
    }

    SECTION("an unsafe (traversal) symbol name is counted, never resolved")
    {
        ModuleIdentity m;
        m.id_kind    = SymbolIdKind::Rsds;
        m.id_len     = 16U;
        m.name       = cont::String("mod.dll");
        m.debug_file = cont::String(".."); // a parent-dir traversal that the lookup path must refuse
        cont::Array<ModuleIdentity> mods;
        mods.push_back(std::move(m));
        const cont::Array<crd::u8> payload =
            serialize_symbol_index(cont::ConstSpan<ModuleIdentity>{mods.data(), mods.size()});

        BundleWriter w;
        REQUIRE(w.add_section(BundleSectionTag::SymbolIndex, span_of(payload.data(), payload.size())) ==
                BundleWriter::AddStatus::Ok);
        const cont::Array<crd::u8> bytes = w.finish(0U);

        const BundleImport imp = import_bundle(span_of(bytes.data(), bytes.size()));
        REQUIRE(imp.status == ImportStatus::Ok);
        REQUIRE(imp.has_symbols);
        REQUIRE(imp.symbols.size() == 1U);
        CHECK(imp.unsafe_names == 1U); // the importer's end of the traversal clause (debug_file_path is the other)
    }

    SECTION("a manifest that disagrees with the sections is reported, not rejected")
    {
        BundleManifest man;
        man.schema_version = kDiagnosticSchemaVersion;
        man.absent_tags.push_back(static_cast<crd::u32>(BundleSectionTag::LogTail)); // manifest: LogTail absent...
        const cont::Array<crd::u8> mpayload = serialize_manifest(man);

        BundleWriter w;
        REQUIRE(w.add_section(BundleSectionTag::Manifest, span_of(mpayload.data(), mpayload.size())) ==
                BundleWriter::AddStatus::Ok);
        REQUIRE(w.add_section(BundleSectionTag::LogTail, span_of(kCrashBytes, sizeof(kCrashBytes))) ==
                BundleWriter::AddStatus::Ok); // ...but LogTail is actually PRESENT
        const cont::Array<crd::u8> bytes = w.finish(0U);

        const BundleImport imp = import_bundle(span_of(bytes.data(), bytes.size()));
        CHECK(imp.status == ImportStatus::Ok); // report, never reject: the disagreement is surfaced, data still read
        REQUIRE(imp.has_manifest);
        CHECK(imp.manifest_absent_mismatches == 1U);
    }
}
