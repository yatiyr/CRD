// DIAG.5d(b): the cerid-diagnostics/1 bundle container + writer + round-trip reader with truncation
// recovery + atomic publish + retention. This asserts the acceptance clauses this slice owns:
// sections round-trip byte-exact; explicit absent sections round-trip; an over-cap section is stored
// TRUNCATED with the pre-cut original_length (never silently dropped); a truncated file recovers its
// complete valid prefix (the sequential-TLV format's whole point); a corrupt/compressed section stops
// the read honestly; write_bundle_atomic leaves no .tmp and a crc-valid final; and retention evicts the
// oldest beyond the cap. The bounded, non-executing ADVERSARIAL importer (traversal / oversized / bomb /
// version-mismatch, golden manifests, fuzz corpus) is DIAG.5d(e) and builds on read_bundle().

#include <crd/containers/array.hpp>
#include <crd/containers/string.hpp>
#include <crd/perf/bundle.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <filesystem>

namespace cont = crd::containers;
namespace fs   = std::filesystem;
using namespace crd::perf;

namespace
{
cont::ConstSpan<crd::u8> span_of(const crd::u8* p, crd::usize n) noexcept
{
    return cont::ConstSpan<crd::u8>{p, n};
}

// A distinct, checkable payload of length n (byte i = seed*31 + i).
cont::Array<crd::u8> pattern(crd::u8 seed, crd::usize n)
{
    cont::Array<crd::u8> a;
    a.resize_uninitialized(n);
    for (crd::usize i = 0; i < n; ++i)
        a[i] = static_cast<crd::u8>(seed * 31U + i);
    return a;
}

// IEEE 802.3 crc32 -- mirrors bundle.cpp so the test can forge a header whose crc is valid but whose
// version is unknown, reaching the reader's version check instead of stopping at the crc check.
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

std::atomic<unsigned> g_uniq{0};

fs::path fresh_dir(const char* tag)
{
    fs::path p = fs::temp_directory_path();
    char     name[64];
    std::snprintf(name, sizeof(name), "crd_bundle_%s_%u", tag, g_uniq.fetch_add(1U));
    p /= name;
    std::error_code ec;
    fs::remove_all(p, ec);
    fs::create_directories(p, ec);
    return p;
}

cont::Array<crd::u8> read_file_bytes(const char* path)
{
    cont::Array<crd::u8> out;
    std::FILE*           f = nullptr;
#if defined(_MSC_VER)
    if (fopen_s(&f, path, "rb") != 0)
        f = nullptr;
#else
    f = std::fopen(path, "rb");
#endif
    if (f == nullptr)
        return out;
    std::fseek(f, 0, SEEK_END);
    const long sz = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (sz > 0)
    {
        out.resize_uninitialized(static_cast<crd::usize>(sz));
        const crd::usize got = std::fread(out.data(), 1, static_cast<crd::usize>(sz), f);
        out.resize(got);
    }
    std::fclose(f);
    return out;
}
} // namespace

TEST_CASE("bundle round-trips multiple sections byte-exact", "[perf][diag][bundle]")
{
    const cont::Array<crd::u8> a = pattern(1, 100);
    const cont::Array<crd::u8> b = pattern(7, 4096);

    BundleWriter w;
    REQUIRE(w.add_section(BundleSectionTag::CrashRecord, span_of(a.data(), a.size())) == BundleWriter::AddStatus::Ok);
    REQUIRE(w.add_section(BundleSectionTag::RawDump, span_of(b.data(), b.size())) == BundleWriter::AddStatus::Ok);
    const cont::Array<crd::u8> buf = w.finish();

    const BundleReadResult r = read_bundle(span_of(buf.data(), buf.size()));
    REQUIRE(r.status == BundleReadStatus::Ok);
    REQUIRE(r.header.magic == kBundleMagic);
    REQUIRE(r.header.format_version == kBundleFormatVersion);
    REQUIRE(r.declared_section_count == 2U);
    REQUIRE(r.sections.size() == 2U);

    CHECK(r.sections[0].tag == BundleSectionTag::CrashRecord);
    CHECK(r.sections[0].payload.size() == a.size());
    CHECK(r.sections[0].original_length == a.size());
    CHECK((r.sections[0].flags & kSectionFlagTruncated) == 0U);
    CHECK(std::memcmp(r.sections[0].payload.data(), a.data(), a.size()) == 0);

    CHECK(r.sections[1].tag == BundleSectionTag::RawDump);
    CHECK(r.sections[1].payload.size() == b.size());
    CHECK(std::memcmp(r.sections[1].payload.data(), b.data(), b.size()) == 0);

    // total_len is the exact serialised size.
    CHECK(r.header.total_len == buf.size());
}

TEST_CASE("bundle records explicit absent sections", "[perf][diag][bundle]")
{
    const cont::Array<crd::u8> a = pattern(3, 64);

    BundleWriter w;
    REQUIRE(w.add_absent(BundleSectionTag::Manifest) == BundleWriter::AddStatus::StoredAbsent);
    REQUIRE(w.add_section(BundleSectionTag::LogTail, span_of(a.data(), a.size())) == BundleWriter::AddStatus::Ok);
    // An empty span through add_section is also an absent section.
    REQUIRE(w.add_section(BundleSectionTag::SymbolIndex, span_of(nullptr, 0)) == BundleWriter::AddStatus::StoredAbsent);
    const cont::Array<crd::u8> buf = w.finish();

    const BundleReadResult r = read_bundle(span_of(buf.data(), buf.size()));
    REQUIRE(r.status == BundleReadStatus::Ok);
    REQUIRE(r.sections.size() == 3U);

    CHECK(r.sections[0].tag == BundleSectionTag::Manifest);
    CHECK((r.sections[0].flags & kSectionFlagAbsent) != 0U);
    CHECK(r.sections[0].payload.size() == 0U);
    CHECK(r.sections[0].original_length == 0U);

    CHECK(r.sections[2].tag == BundleSectionTag::SymbolIndex);
    CHECK((r.sections[2].flags & kSectionFlagAbsent) != 0U);
    CHECK(r.sections[2].payload.size() == 0U);
}

TEST_CASE("bundle truncates an over-cap section with an honest marker", "[perf][diag][bundle]")
{
    const cont::Array<crd::u8> big = pattern(9, 8192);

    BundleLimits limits;
    limits.max_section_bytes = 1024; // force truncation
    BundleWriter w{crd::memory::default_allocator(), limits};
    REQUIRE(w.add_section(BundleSectionTag::RawDump, span_of(big.data(), big.size())) ==
            BundleWriter::AddStatus::StoredTruncated);
    const cont::Array<crd::u8> buf = w.finish();

    const BundleReadResult r = read_bundle(span_of(buf.data(), buf.size()));
    REQUIRE(r.status == BundleReadStatus::Ok);
    REQUIRE(r.sections.size() == 1U);
    CHECK((r.sections[0].flags & kSectionFlagTruncated) != 0U);
    CHECK(r.sections[0].payload.size() == 1024U);       // cut to the cap
    CHECK(r.sections[0].original_length == big.size()); // pre-cut size preserved -- never silent
    CHECK(std::memcmp(r.sections[0].payload.data(), big.data(), 1024) == 0);
}

TEST_CASE("bundle rejects sections beyond max_sections", "[perf][diag][bundle]")
{
    const cont::Array<crd::u8> a = pattern(2, 8);
    BundleLimits               limits;
    limits.max_sections = 2;
    BundleWriter w{crd::memory::default_allocator(), limits};
    CHECK(w.add_section(BundleSectionTag::CrashRecord, span_of(a.data(), a.size())) == BundleWriter::AddStatus::Ok);
    CHECK(w.add_section(BundleSectionTag::RawDump, span_of(a.data(), a.size())) == BundleWriter::AddStatus::Ok);
    CHECK(w.add_section(BundleSectionTag::LogTail, span_of(a.data(), a.size())) ==
          BundleWriter::AddStatus::RejectedTooMany);
    CHECK(w.section_count() == 2U);
}

TEST_CASE("bundle recovers the valid prefix of a truncated file", "[perf][diag][bundle]")
{
    const cont::Array<crd::u8> s0 = pattern(1, 32);
    const cont::Array<crd::u8> s1 = pattern(2, 48);
    const cont::Array<crd::u8> s2 = pattern(3, 64);

    BundleWriter w;
    w.add_section(BundleSectionTag::CrashRecord, span_of(s0.data(), s0.size()));
    w.add_section(BundleSectionTag::RawDump, span_of(s1.data(), s1.size()));
    w.add_section(BundleSectionTag::LogTail, span_of(s2.data(), s2.size()));
    const cont::Array<crd::u8> buf = w.finish();

    const crd::usize sh   = sizeof(BundleSectionHeader);
    const crd::usize end0 = sizeof(BundleHeader) + sh + s0.size();          // just past section 0
    const crd::usize end1 = end0 + sh + s1.size();                          // just past section 1
    const crd::usize mid1 = end0 + sh + (s1.size() / 2);                    // inside section 1's payload
    const crd::usize midh = end1 + (sh / 2);                                // inside section 2's header

    SECTION("cut between sections -> full prefix recovered")
    {
        const BundleReadResult r = read_bundle(span_of(buf.data(), end1));
        CHECK(r.status == BundleReadStatus::RecoveredTruncated);
        CHECK(r.declared_section_count == 3U);
        CHECK(r.sections.size() == 2U); // sections 0 and 1 are complete
        CHECK(r.stop_offset == end1);
    }
    SECTION("cut mid-payload -> the incomplete section is dropped")
    {
        const BundleReadResult r = read_bundle(span_of(buf.data(), mid1));
        CHECK(r.status == BundleReadStatus::RecoveredTruncated);
        CHECK(r.sections.size() == 1U); // only section 0 is complete
        CHECK(r.stop_offset == end0);
    }
    SECTION("cut mid-header -> the incomplete section is dropped")
    {
        const BundleReadResult r = read_bundle(span_of(buf.data(), midh));
        CHECK(r.status == BundleReadStatus::RecoveredTruncated);
        CHECK(r.sections.size() == 2U); // sections 0 and 1 complete; section 2 header is cut
        CHECK(r.stop_offset == end1);
    }
    SECTION("zero-padded tail is not read as real sections")
    {
        // A full-length buffer whose section-2 region is all zeros: tag 0 is reserved, so the reader
        // must stop rather than accept the zeroed slot (its empty-payload crc happens to be 0).
        cont::Array<crd::u8> z = buf;
        for (crd::usize i = end1; i < z.size(); ++i)
            z[i] = 0;
        const BundleReadResult r = read_bundle(span_of(z.data(), z.size()));
        CHECK(r.status == BundleReadStatus::RecoveredTruncated);
        CHECK(r.sections.size() == 2U);
        CHECK(r.stop_offset == end1);
    }
}

TEST_CASE("bundle enforces the total byte cap (ADR-0133 line 134)", "[perf][diag][bundle]")
{
    BundleLimits limits;
    limits.max_total_bytes = sizeof(BundleHeader) + sizeof(BundleSectionHeader) + 100; // room for one 100-byte payload
    BundleWriter w{crd::memory::default_allocator(), limits};

    const cont::Array<crd::u8> big = pattern(5, 1000);
    CHECK(w.add_section(BundleSectionTag::RawDump, span_of(big.data(), big.size())) ==
          BundleWriter::AddStatus::StoredTruncated); // cut to the 100 bytes of total room left

    // The bundle is now exactly full: a further non-empty (or empty) section has no room.
    const cont::Array<crd::u8> more = pattern(6, 10);
    CHECK(w.add_section(BundleSectionTag::LogTail, span_of(more.data(), more.size())) ==
          BundleWriter::AddStatus::RejectedNoRoom);

    const cont::Array<crd::u8> buf = w.finish();
    CHECK(buf.size() == limits.max_total_bytes); // exactly the cap, never over

    const BundleReadResult r = read_bundle(span_of(buf.data(), buf.size()));
    REQUIRE(r.status == BundleReadStatus::Ok);
    REQUIRE(r.sections.size() == 1U);
    CHECK((r.sections[0].flags & kSectionFlagTruncated) != 0U);
    CHECK(r.sections[0].payload.size() == 100U);
    CHECK(r.sections[0].original_length == big.size());
}

TEST_CASE("empty sections also respect the total cap", "[perf][diag][bundle]")
{
    BundleLimits limits;
    limits.max_total_bytes = sizeof(BundleHeader) + sizeof(BundleSectionHeader); // room for exactly one header
    BundleWriter w{crd::memory::default_allocator(), limits};

    CHECK(w.add_absent(BundleSectionTag::Manifest) == BundleWriter::AddStatus::StoredAbsent);
    // Now full: an empty add_section must be rejected too, not silently pushed over the cap.
    CHECK(w.add_section(BundleSectionTag::LogTail, span_of(nullptr, 0)) == BundleWriter::AddStatus::RejectedNoRoom);
    CHECK(w.add_absent(BundleSectionTag::SymbolIndex) == BundleWriter::AddStatus::RejectedNoRoom);
    CHECK(w.finish().size() == limits.max_total_bytes);
}

TEST_CASE("bundle reader flags corruption and bad framing", "[perf][diag][bundle]")
{
    const cont::Array<crd::u8> s0 = pattern(4, 40);
    const cont::Array<crd::u8> s1 = pattern(5, 40);
    BundleWriter               w;
    w.add_section(BundleSectionTag::CrashRecord, span_of(s0.data(), s0.size()));
    w.add_section(BundleSectionTag::RawDump, span_of(s1.data(), s1.size()));
    cont::Array<crd::u8> buf = w.finish();

    SECTION("bad magic")
    {
        cont::Array<crd::u8> bad = buf;
        bad[0] ^= 0xFFU;
        CHECK(read_bundle(span_of(bad.data(), bad.size())).status == BundleReadStatus::BadMagic);
    }
    SECTION("too small for a header")
    {
        CHECK(read_bundle(span_of(buf.data(), 8)).status == BundleReadStatus::HeaderTooSmall);
    }
    SECTION("corrupt header -> crc mismatch")
    {
        cont::Array<crd::u8> bad = buf;
        bad[8] ^= 0xFFU; // flip a byte inside the header, before header_crc32
        CHECK(read_bundle(span_of(bad.data(), bad.size())).status == BundleReadStatus::HeaderTooSmall);
    }
    SECTION("unsupported format version")
    {
        cont::Array<crd::u8> bad = buf;
        BundleHeader         hdr{};
        std::memcpy(&hdr, bad.data(), sizeof(hdr));
        hdr.format_version = 999U;
        // Recompute the header crc (over the first offsetof(header_crc32) bytes) so parsing reaches
        // the version check rather than stopping at the crc check.
        hdr.header_crc32 = test_crc32(reinterpret_cast<const crd::u8*>(&hdr), offsetof(BundleHeader, header_crc32));
        std::memcpy(bad.data(), &hdr, sizeof(hdr));
        const BundleReadResult r = read_bundle(span_of(bad.data(), bad.size()));
        CHECK(r.status == BundleReadStatus::UnsupportedVersion);
        CHECK(r.declared_section_count == 2U);
    }
    SECTION("corrupt section payload -> recovered prefix stops there")
    {
        cont::Array<crd::u8> bad = buf;
        // Flip a byte inside section 1's payload (after header + section0 + section1 header).
        const crd::usize p1 = sizeof(BundleHeader) + sizeof(BundleSectionHeader) + s0.size() +
                              sizeof(BundleSectionHeader) + 4;
        bad[p1] ^= 0xFFU;
        const BundleReadResult r = read_bundle(span_of(bad.data(), bad.size()));
        CHECK(r.status == BundleReadStatus::RecoveredTruncated);
        CHECK(r.sections.size() == 1U); // section 0 survives, section 1's crc fails
    }
    SECTION("compressed flag is rejected (no codec edge in v1)")
    {
        cont::Array<crd::u8> bad = buf;
        // Section 1's flags field: header + section0(hdr+payload) + offsetof(flags)=4.
        const crd::usize f1 = sizeof(BundleHeader) + sizeof(BundleSectionHeader) + s0.size() + 4;
        std::memcpy(&bad[f1], &kSectionFlagCompressed, 4); // set exactly the compressed bit
        const BundleReadResult r = read_bundle(span_of(bad.data(), bad.size()));
        CHECK(r.status == BundleReadStatus::UnsupportedFeature);
        CHECK(r.sections.size() == 1U); // section 0 recovered before the rejection
    }
}

TEST_CASE("write_bundle_atomic publishes durably with no leftover temp", "[perf][diag][bundle]")
{
    const fs::path dir = fresh_dir("atomic");
    const fs::path out = dir / "session.cdb";

    const cont::Array<crd::u8> a = pattern(6, 256);
    BundleWriter               w;
    w.add_section(BundleSectionTag::Manifest, span_of(a.data(), a.size()));
    const cont::Array<crd::u8> buf = w.finish();

    cont::String path{out.string().c_str()};
    REQUIRE(write_bundle_atomic(path.c_str(), span_of(buf.data(), buf.size())));

    std::error_code ec;
    CHECK(fs::exists(out, ec));
    cont::String tmp{path};
    tmp.append(".tmp");
    CHECK_FALSE(fs::exists(fs::path{tmp.c_str()}, ec)); // no leftover temp

    // Read it back byte-exact and re-parse.
    const cont::Array<crd::u8> disk = read_file_bytes(path.c_str());
    REQUIRE(disk.size() == buf.size());
    CHECK(std::memcmp(disk.data(), buf.data(), buf.size()) == 0);
    CHECK(read_bundle(span_of(disk.data(), disk.size())).status == BundleReadStatus::Ok);

    fs::remove_all(dir, ec);
}

TEST_CASE("retention keeps only the newest bundles", "[perf][diag][bundle]")
{
    const fs::path dir = fresh_dir("retain");

    const cont::Array<crd::u8> a   = pattern(8, 32);
    BundleWriter               w;
    w.add_section(BundleSectionTag::Manifest, span_of(a.data(), a.size()));
    const cont::Array<crd::u8> buf = w.finish();

    constexpr int kCount = 13;
    for (int i = 0; i < kCount; ++i)
    {
        char name[32];
        std::snprintf(name, sizeof(name), "bundle_%02d.cdb", i);
        cont::String path{(dir / name).string().c_str()};
        REQUIRE(write_bundle_atomic(path.c_str(), span_of(buf.data(), buf.size())));
        // Strictly increasing mtimes (1s apart) so "oldest" is unambiguous on any filesystem.
        std::error_code ec;
        fs::last_write_time(dir / name, fs::file_time_type{} + std::chrono::seconds(1'000'000 + i), ec);
    }

    cont::String dirs{dir.string().c_str()};
    const crd::i64 deleted = enforce_bundle_retention(dirs.c_str(), "bundle_", ".cdb", 10U);
    CHECK(deleted == 3);

    std::error_code ec;
    // The three oldest (00,01,02) are gone; 03..12 remain.
    for (int i = 0; i < kCount; ++i)
    {
        char name[32];
        std::snprintf(name, sizeof(name), "bundle_%02d.cdb", i);
        const bool present = fs::exists(dir / name, ec);
        if (i < 3)
            CHECK_FALSE(present);
        else
            CHECK(present);
    }

    fs::remove_all(dir, ec);
}
