// DIAG.5d(c1): exact symbol identities. Verifies the acceptance clause this slice owns -- "reject wrong symbols
// instead of giving plausible lines" (identity_matches) and "matching debug files" (debug_file_path, symsrv +
// .build-id conventions) -- plus the crd-native minidump byte parser (every module's RSDS GUID/age/PDB, untrusted
// input bounds-checked so a truncated dump yields a partial index) and round-trip through the SymbolIndex section and
// a full bundle. Runs on every lane (the parser is platform-neutral). Linux build-id CAPTURE at install() is (c2).

#include <crd/containers/array.hpp>
#include <crd/containers/string.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/perf/bundle.hpp>
#include <crd/perf/symbol_index.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstring>

namespace cont = crd::containers;
using namespace crd::perf;

namespace
{
template <typename T> cont::ConstSpan<T> span_of(const T* p, crd::usize n) noexcept
{
    return cont::ConstSpan<T>{p, n};
}

void put_u32(cont::Array<crd::u8>& a, crd::u32 v) noexcept
{
    for (int i = 0; i < 4; ++i)
    {
        a.push_back(static_cast<crd::u8>(v >> (8 * i)));
    }
}
void put_u64(cont::Array<crd::u8>& a, crd::u64 v) noexcept
{
    for (int i = 0; i < 8; ++i)
    {
        a.push_back(static_cast<crd::u8>(v >> (8 * i)));
    }
}
void put_utf16(cont::Array<crd::u8>& a, const char* s) noexcept
{
    for (; *s != '\0'; ++s)
    {
        a.push_back(static_cast<crd::u8>(*s));
        a.push_back(0);
    }
}
void put_cstr(cont::Array<crd::u8>& a, const char* s) noexcept
{
    for (; *s != '\0'; ++s)
    {
        a.push_back(static_cast<crd::u8>(*s));
    }
    a.push_back(0); // NUL
}

// One MINIDUMP_MODULE (112 B), fields at the pinned offsets the parser reads.
void put_module(cont::Array<crd::u8>& a, crd::u64 base, crd::u32 size, crd::u32 checksum, crd::u32 timestamp,
                crd::u32 name_rva, crd::u32 cv_size, crd::u32 cv_rva) noexcept
{
    put_u64(a, base);
    put_u32(a, size);
    put_u32(a, checksum);
    put_u32(a, timestamp);
    put_u32(a, name_rva);
    for (int i = 0; i < 13; ++i)
    {
        put_u32(a, 0); // VS_FIXEDFILEINFO
    }
    put_u32(a, cv_size);
    put_u32(a, cv_rva); // cv_record
    put_u32(a, 0);
    put_u32(a, 0); // misc_record
    put_u64(a, 0); // reserved0 (no pad: MINIDUMP_MODULE is pack(4) -> 108 B)
    put_u64(a, 0); // reserved1
}

const crd::u8 kGuid[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};

// A minimal but real minidump: header + 1 directory (ModuleListStream) + 2 modules. Module 0 carries an RSDS CV
// record (GUID kGuid, age 7, pdb "app.pdb"); module 1 carries no CV record.
cont::Array<crd::u8> make_minidump()
{
    // Fixed layout (all RVAs are file offsets). Module records are kMinidumpModuleRecordBytes (108) B.
    const crd::u32 mrec     = kMinidumpModuleRecordBytes;
    const crd::u32 dir_rva  = 32;                        // header is 32 B
    const crd::u32 list_rva = dir_rva + 12;              // 44 -- after one 12-B directory
    const crd::u32 mod0_rva = list_rva + 4;              // 48 -- after the u32 count
    const crd::u32 mod1_rva = mod0_rva + mrec;           // 156
    const crd::u32 name0_rva = mod1_rva + mrec;          // 264
    const crd::u32 cv0_rva   = name0_rva + 4 + 14;       // 282 -- "app.exe" MINIDUMP_STRING is 4 + 7*2
    const crd::u32 name1_rva = cv0_rva + 4 + 16 + 4 + 8; // 314 -- RSDS(4)+GUID(16)+age(4)+"app.pdb\0"(8)
    const crd::u32 list_size = 4 + 2 * mrec;             // count + two modules

    cont::Array<crd::u8> d;
    // MdHeader
    put_u32(d, 0x504D444DU); // 'MDMP'
    put_u32(d, 0x0000A793U); // version (arbitrary)
    put_u32(d, 1);           // number_of_streams
    put_u32(d, dir_rva);     // stream_directory_rva
    put_u32(d, 0);           // checksum
    put_u32(d, 0);           // time_date_stamp
    put_u64(d, 0);           // flags
    // MdDirectory[0]
    put_u32(d, 4);           // ModuleListStream
    put_u32(d, list_size);   // location.data_size
    put_u32(d, list_rva);    // location.rva
    // module list
    put_u32(d, 2);           // count
    put_module(d, 0x140000000ULL, 0x10000, 0xABCD, 0x1234, name0_rva, 32, cv0_rva);
    put_module(d, 0x180000000ULL, 0x8000, 0, 0, name1_rva, 0, 0);
    // name0
    put_u32(d, 14);
    put_utf16(d, "app.exe");
    // cv0 (RSDS)
    put_u32(d, 0x53445352U); // 'RSDS'
    for (crd::u8 b : kGuid)
    {
        d.push_back(b);
    }
    put_u32(d, 7); // age
    put_cstr(d, "app.pdb");
    // name1
    put_u32(d, 18);
    put_utf16(d, "other.dll");
    return d;
}
// One MdUnloadedModule (24 B: base, size, checksum, timestamp, name_rva), padded to `stride`.
void put_unloaded_entry(cont::Array<crd::u8>& a, crd::u64 base, crd::u32 size, crd::u32 ts, crd::u32 name_rva,
                        crd::u32 stride) noexcept
{
    const crd::usize start = a.size();
    put_u64(a, base);
    put_u32(a, size);
    put_u32(a, 0); // checksum
    put_u32(a, ts);
    put_u32(a, name_rva);
    while (a.size() - start < stride) // forward-compat pad to the declared stride
    {
        a.push_back(0);
    }
}

// A minidump carrying ONLY an UnloadedModuleListStream (stream 14): two unloaded modules (winhttp.dll, oldmod.dll).
// `stride` is the self-describing size_of_entry; `declared_count` is the header's number_of_entries (default 2, raised
// to test bounded parsing).
cont::Array<crd::u8> make_unloaded_dump(crd::u32 stride, crd::u32 declared_count = 2)
{
    const crd::u32 dir_rva     = 32;
    const crd::u32 list_rva    = 44;                       // header(32) + one directory(12)
    const crd::u32 entries_rva = list_rva + 12;            // after MdUnloadedModuleList
    const crd::u32 names_rva    = entries_rva + 2 * stride; // two real entries
    const crd::u32 uname0_rva   = names_rva;                // "winhttp.dll" -> 4 + 11*2 = 26
    const crd::u32 uname1_rva   = uname0_rva + 26;          // "oldmod.dll"  -> 4 + 10*2 = 24
    const crd::u32 data_size    = 12 + 2 * stride;          // list header + two entries

    cont::Array<crd::u8> d;
    // MdHeader
    put_u32(d, 0x504D444DU);
    put_u32(d, 0x0000A793U);
    put_u32(d, 1);       // number_of_streams
    put_u32(d, dir_rva);
    put_u32(d, 0);
    put_u32(d, 0);
    put_u64(d, 0);
    // MdDirectory[0] -> UnloadedModuleListStream
    put_u32(d, 14);
    put_u32(d, data_size);
    put_u32(d, list_rva);
    // MdUnloadedModuleList
    put_u32(d, 12);            // size_of_header
    put_u32(d, stride);        // size_of_entry
    put_u32(d, declared_count); // number_of_entries
    // entries
    put_unloaded_entry(d, 0x70000000ULL, 0x30000U, 0xAABBCCDDU, uname0_rva, stride);
    put_unloaded_entry(d, 0x71000000ULL, 0x20000U, 0x11223344U, uname1_rva, stride);
    // names
    put_u32(d, 22);
    put_utf16(d, "winhttp.dll");
    put_u32(d, 20);
    put_utf16(d, "oldmod.dll");
    return d;
}
} // namespace

TEST_CASE("minidump parser reads every module's RSDS identity", "[perf][diag][symbols]")
{
    const cont::Array<crd::u8>        dump = make_minidump();
    const cont::Array<ModuleIdentity> mods = parse_minidump_modules(span_of(dump.data(), dump.size()));

    REQUIRE(mods.size() == 2U);

    CHECK(mods[0].base == 0x140000000ULL);
    CHECK(mods[0].size == 0x10000U);
    CHECK(mods[0].checksum == 0xABCDU);
    CHECK(mods[0].timestamp == 0x1234U);
    CHECK(std::strcmp(mods[0].name.c_str(), "app.exe") == 0);
    CHECK(mods[0].id_kind == SymbolIdKind::Rsds);
    CHECK(mods[0].id_len == 16U);
    CHECK(mods[0].age == 7U);
    CHECK(std::memcmp(mods[0].id, kGuid, 16) == 0);
    CHECK(std::strcmp(mods[0].debug_file.c_str(), "app.pdb") == 0);

    CHECK(std::strcmp(mods[1].name.c_str(), "other.dll") == 0);
    CHECK(mods[1].id_kind == SymbolIdKind::None); // no CV record -> refuse, don't guess
    CHECK(mods[1].debug_file.empty());
}

TEST_CASE("minidump parser treats a truncated dump as untrusted (partial, no overrun)", "[perf][diag][symbols]")
{
    const cont::Array<crd::u8> dump = make_minidump();

    SECTION("cut inside the header -> no modules")
    {
        CHECK(parse_minidump_modules(span_of(dump.data(), 20)).size() == 0U);
    }
    SECTION("cut before the module count -> no modules")
    {
        CHECK(parse_minidump_modules(span_of(dump.data(), 46)).size() == 0U);
    }
    SECTION("cut inside the module list -> only the complete records")
    {
        // 200 bytes: module 0 (48..160) is complete, module 1 (160..272) is not.
        const cont::Array<ModuleIdentity> mods = parse_minidump_modules(span_of(dump.data(), 200));
        CHECK(mods.size() == 1U);
    }
    SECTION("cut inside the CV record -> module kept but identity refused")
    {
        // 300 bytes: both module records (<=272) parse, but cv0 (290..322) is cut -> no RSDS for module 0.
        const cont::Array<ModuleIdentity> mods = parse_minidump_modules(span_of(dump.data(), 300));
        REQUIRE(mods.size() == 2U);
        CHECK(mods[0].id_kind == SymbolIdKind::None);
    }
}

TEST_CASE("SymbolIndex round-trips through its section and a bundle", "[perf][diag][symbols]")
{
    const cont::Array<crd::u8>        dump = make_minidump();
    const cont::Array<ModuleIdentity> mods = parse_minidump_modules(span_of(dump.data(), dump.size()));

    // Direct section round-trip.
    const cont::Array<crd::u8> payload = serialize_symbol_index(span_of(mods.data(), mods.size()));
    const cont::Array<ModuleIdentity> back = read_symbol_index(span_of(payload.data(), payload.size()));
    REQUIRE(back.size() == mods.size());
    for (crd::usize i = 0; i < mods.size(); ++i)
    {
        CHECK(back[i].base == mods[i].base);
        CHECK(back[i].size == mods[i].size);
        CHECK(back[i].checksum == mods[i].checksum);
        CHECK(back[i].timestamp == mods[i].timestamp);
        CHECK(back[i].age == mods[i].age);
        CHECK(back[i].id_kind == mods[i].id_kind);
        CHECK(back[i].id_len == mods[i].id_len);
        CHECK(std::memcmp(back[i].id, mods[i].id, kMaxSymbolIdBytes) == 0);
        CHECK(std::strcmp(back[i].name.c_str(), mods[i].name.c_str()) == 0);
        CHECK(std::strcmp(back[i].debug_file.c_str(), mods[i].debug_file.c_str()) == 0);
    }

    // Through a bundle: build_symbol_index_from_minidump -> tag-5 section -> read_bundle -> read_symbol_index.
    const cont::Array<crd::u8> sec = build_symbol_index_from_minidump(span_of(dump.data(), dump.size()));
    BundleWriter               w;
    REQUIRE(w.add_section(BundleSectionTag::SymbolIndex, span_of(sec.data(), sec.size())) ==
            BundleWriter::AddStatus::Ok);
    const cont::Array<crd::u8> bundle = w.finish();
    const BundleReadResult     r      = read_bundle(span_of(bundle.data(), bundle.size()));
    REQUIRE(r.status == BundleReadStatus::Ok);
    REQUIRE(r.sections.size() == 1U);
    REQUIRE(r.sections[0].tag == BundleSectionTag::SymbolIndex);
    const cont::Array<ModuleIdentity> from_bundle =
        read_symbol_index(span_of(r.sections[0].payload.data(), r.sections[0].payload.size()));
    REQUIRE(from_bundle.size() == 2U);
    CHECK(from_bundle[0].id_kind == SymbolIdKind::Rsds);
    CHECK(std::memcmp(from_bundle[0].id, kGuid, 16) == 0);
    CHECK(std::strcmp(from_bundle[0].debug_file.c_str(), "app.pdb") == 0);
}

TEST_CASE("identity_matches rejects wrong symbols instead of guessing", "[perf][diag][symbols]")
{
    ModuleIdentity a;
    a.id_kind = SymbolIdKind::Rsds;
    a.id_len  = 16;
    std::memcpy(a.id, kGuid, 16);
    a.age = 7;

    ModuleIdentity same = a;
    CHECK(identity_matches(a, same));

    ModuleIdentity wrong_age = a;
    wrong_age.age = 8;
    CHECK_FALSE(identity_matches(a, wrong_age)); // same GUID, different age -> different binary

    ModuleIdentity wrong_guid = a;
    wrong_guid.id[0] ^= 0xFFU;
    CHECK_FALSE(identity_matches(a, wrong_guid));

    ModuleIdentity none; // id_kind None
    CHECK_FALSE(identity_matches(a, none));
    CHECK_FALSE(identity_matches(none, none)); // None never matches, even itself

    ModuleIdentity g;
    g.id_kind = SymbolIdKind::GnuBuildId;
    g.id_len  = 20;
    for (crd::u8 i = 0; i < 20; ++i)
    {
        g.id[i] = static_cast<crd::u8>(0x40U + i);
    }
    ModuleIdentity g_same = g;
    CHECK(identity_matches(g, g_same));
    ModuleIdentity g_flip = g;
    g_flip.id[19] ^= 0x01U;
    CHECK_FALSE(identity_matches(g, g_flip));

    CHECK_FALSE(identity_matches(a, g)); // different kinds never match

    // A zero-length identity is not an identity: two Rsds records with id_len 0 and equal age must NOT match
    // (else a corrupt/blank record would certify any PDB by age alone).
    ModuleIdentity empty_a;
    empty_a.id_kind = SymbolIdKind::Rsds;
    empty_a.id_len  = 0;
    empty_a.age     = 7;
    ModuleIdentity empty_b = empty_a;
    CHECK_FALSE(identity_matches(empty_a, empty_b));
}

TEST_CASE("read_symbol_index bounds-checks an untrusted payload", "[perf][diag][symbols]")
{
    const cont::Array<crd::u8>        dump = make_minidump();
    const cont::Array<ModuleIdentity> mods = parse_minidump_modules(span_of(dump.data(), dump.size()));
    const cont::Array<crd::u8>        good = serialize_symbol_index(span_of(mods.data(), mods.size()));

    SECTION("record table truncated -> empty")
    {
        CHECK(read_symbol_index(span_of(good.data(), sizeof(SymbolIndexHeader) + 10)).size() == 0U);
    }
    SECTION("a lying name_len yields an empty field, not an overrun")
    {
        cont::Array<crd::u8> bad = good;
        const crd::u32       huge = 0xFFFFFFFFU;
        const crd::usize     off  = sizeof(SymbolIndexHeader) + offsetof(SymbolRecord, name_len);
        std::memcpy(&bad[off], &huge, sizeof(huge));
        const cont::Array<ModuleIdentity> back = read_symbol_index(span_of(bad.data(), bad.size()));
        REQUIRE(back.size() == 2U);
        CHECK(back[0].name.empty());                                    // out-of-range -> ""
        CHECK(std::strcmp(back[1].name.c_str(), "other.dll") == 0);     // sibling intact
    }
    SECTION("unknown version -> empty")
    {
        cont::Array<crd::u8> bad = good;
        const crd::u32       ver = 999U;
        std::memcpy(&bad[0], &ver, sizeof(ver)); // version is the first field
        CHECK(read_symbol_index(span_of(bad.data(), bad.size())).size() == 0U);
    }
    SECTION("a corrupt id_kind is clamped to None (never a forged matchable kind)")
    {
        cont::Array<crd::u8> bad = good;
        const crd::u8        bogus = 99U;
        const crd::usize     off   = sizeof(SymbolIndexHeader) + offsetof(SymbolRecord, id_kind);
        std::memcpy(&bad[off], &bogus, sizeof(bogus));
        const cont::Array<ModuleIdentity> back = read_symbol_index(span_of(bad.data(), bad.size()));
        REQUIRE(back.size() == 2U);
        CHECK(back[0].id_kind == SymbolIdKind::None);
    }
}

TEST_CASE("debug_file_path builds the symbol-store and build-id conventions", "[perf][diag][symbols]")
{
    ModuleIdentity m;
    m.id_kind = SymbolIdKind::Rsds;
    m.id_len  = 16;
    std::memcpy(m.id, kGuid, 16);
    m.age = 7;
    m.debug_file.append("app.pdb");
    const cont::String rsds = debug_file_path(m, cont::StringView{"C:/sym"});
    // GUID text: Data1(LE) Data2(LE) Data3(LE) Data4(BE) uppercase, + age hex, symsrv layout.
    CHECK(std::strcmp(rsds.c_str(), "C:/sym/app.pdb/0403020106050807090A0B0C0D0E0F107/app.pdb") == 0);

    // A dir-qualified pdb name is reduced to its basename in both path segments.
    ModuleIdentity mp = m;
    mp.debug_file.clear();
    mp.debug_file.append("obj\\release\\app.pdb");
    const cont::String rsds2 = debug_file_path(mp, cont::StringView{"C:/sym"});
    CHECK(std::strcmp(rsds2.c_str(), "C:/sym/app.pdb/0403020106050807090A0B0C0D0E0F107/app.pdb") == 0);

    ModuleIdentity none;
    CHECK(debug_file_path(none, cont::StringView{"C:/sym"}).empty()); // nothing to look up

    // Traversal guard (the acceptance's "reject traversal ... inputs"): an unsafe pdb name yields NO path.
    ModuleIdentity dotdot = m;
    dotdot.debug_file.clear();
    dotdot.debug_file.append("..");
    CHECK(debug_file_path(dotdot, cont::StringView{"C:/sym"}).empty());

    ModuleIdentity dot = m;
    dot.debug_file.clear();
    dot.debug_file.append(".");
    CHECK(debug_file_path(dot, cont::StringView{"C:/sym"}).empty());

    // A directory-qualified traversal reduces to its (safe) basename.
    ModuleIdentity qualified = m;
    qualified.debug_file.clear();
    qualified.debug_file.append("..\\x.pdb");
    CHECK(std::strcmp(debug_file_path(qualified, cont::StringView{"C:/sym"}).c_str(),
                      "C:/sym/x.pdb/0403020106050807090A0B0C0D0E0F107/x.pdb") == 0);

    // An embedded NUL in the basename would truncate the built c_str path -> refuse.
    ModuleIdentity nul = m;
    nul.debug_file.clear();
    nul.debug_file.push_back('a');
    nul.debug_file.push_back('\0');
    nul.debug_file.push_back('b');
    nul.debug_file.push_back('.');
    nul.debug_file.push_back('p');
    nul.debug_file.push_back('d');
    nul.debug_file.push_back('b');
    CHECK(debug_file_path(nul, cont::StringView{"C:/sym"}).empty());

    // is_safe_lookup_name mirrors the guard.
    CHECK(is_safe_lookup_name(cont::StringView{"winhttp.dll"}));
    CHECK_FALSE(is_safe_lookup_name(cont::StringView{".."}));
    CHECK_FALSE(is_safe_lookup_name(cont::StringView{""}));
    CHECK(is_safe_lookup_name(cont::StringView{"..\\x.pdb"})); // basename "x.pdb" is safe

    ModuleIdentity g;
    g.id_kind = SymbolIdKind::GnuBuildId;
    g.id_len  = 4;
    g.id[0]   = 0xABU;
    g.id[1]   = 0xCDU;
    g.id[2]   = 0xEFU;
    g.id[3]   = 0x01U;
    const cont::String bid = debug_file_path(g, cont::StringView{"/usr/lib/debug"});
    CHECK(std::strcmp(bid.c_str(), "/usr/lib/debug/.build-id/ab/cdef01.debug") == 0);
}

TEST_CASE("unloaded-module stream parses PE image identities (DIAG.5d(d))", "[perf][diag][symbols]")
{
    const cont::Array<crd::u8>        dump = make_unloaded_dump(24);
    const cont::Array<ModuleIdentity> u    = parse_minidump_unloaded_modules(span_of(dump.data(), dump.size()));

    REQUIRE(u.size() == 2U);
    CHECK(u[0].unloaded);
    CHECK(u[0].id_kind == SymbolIdKind::PeImage);
    CHECK(u[0].id_len == 0U); // no CV/GUID bytes on an unloaded entry -- identity is timestamp+size
    CHECK(u[0].base == 0x70000000ULL);
    CHECK(u[0].size == 0x30000U);
    CHECK(u[0].timestamp == 0xAABBCCDDU);
    CHECK(std::strcmp(u[0].name.c_str(), "winhttp.dll") == 0);
    CHECK(u[1].timestamp == 0x11223344U);
    CHECK(std::strcmp(u[1].name.c_str(), "oldmod.dll") == 0);
}

TEST_CASE("unloaded stream honours the self-describing stride and rejects a bad one", "[perf][diag][symbols]")
{
    SECTION("a larger size_of_entry (forward-compat) still lands on every entry")
    {
        const cont::Array<crd::u8>        dump = make_unloaded_dump(32); // 8 pad bytes per entry
        const cont::Array<ModuleIdentity> u    = parse_minidump_unloaded_modules(span_of(dump.data(), dump.size()));
        REQUIRE(u.size() == 2U);
        CHECK(u[0].timestamp == 0xAABBCCDDU); // stride honoured: entry 1 not misread
        CHECK(u[1].timestamp == 0x11223344U);
    }
    SECTION("a size_of_entry below the v1 record is refused")
    {
        const cont::Array<crd::u8> dump = make_unloaded_dump(16); // < sizeof(MdUnloadedModule)
        CHECK(parse_minidump_unloaded_modules(span_of(dump.data(), dump.size())).size() == 0U);
    }
    SECTION("a bogus number_of_entries is bounded, not a spin/overrun")
    {
        const cont::Array<crd::u8>        dump = make_unloaded_dump(24, 0xFFFFFFFFU);
        const cont::Array<ModuleIdentity> u    = parse_minidump_unloaded_modules(span_of(dump.data(), dump.size()));
        CHECK(u.size() == 2U); // only the entries the declared stream bytes actually hold
    }
}

TEST_CASE("unloaded generation round-trips through the SymbolIndex section and a bundle", "[perf][diag][symbols]")
{
    const cont::Array<crd::u8> dump = make_unloaded_dump(24);
    // build_symbol_index_from_minidump emits loaded (none here) then unloaded.
    const cont::Array<crd::u8> sec  = build_symbol_index_from_minidump(span_of(dump.data(), dump.size()));

    BundleWriter w;
    REQUIRE(w.add_section(BundleSectionTag::SymbolIndex, span_of(sec.data(), sec.size())) ==
            BundleWriter::AddStatus::Ok);
    const cont::Array<crd::u8> bundle = w.finish();
    const BundleReadResult     r      = read_bundle(span_of(bundle.data(), bundle.size()));
    REQUIRE(r.status == BundleReadStatus::Ok);
    REQUIRE(r.sections.size() == 1U);

    const cont::Array<ModuleIdentity> back =
        read_symbol_index(span_of(r.sections[0].payload.data(), r.sections[0].payload.size()));
    REQUIRE(back.size() == 2U);
    CHECK(back[0].unloaded); // the flag survives serialize -> bundle -> read
    CHECK(back[0].id_kind == SymbolIdKind::PeImage);
    CHECK(back[0].timestamp == 0xAABBCCDDU);
    CHECK(back[0].size == 0x30000U);
    CHECK(std::strcmp(back[0].name.c_str(), "winhttp.dll") == 0);
}

TEST_CASE("identity_matches: PE image identity is timestamp+size, and refuses a zero stamp", "[perf][diag][symbols]")
{
    ModuleIdentity a;
    a.id_kind   = SymbolIdKind::PeImage;
    a.timestamp = 0xAABBCCDDU;
    a.size      = 0x30000U;

    ModuleIdentity same = a;
    CHECK(identity_matches(a, same));

    ModuleIdentity ts1 = a;
    ts1.timestamp += 1U;
    CHECK_FALSE(identity_matches(a, ts1)); // a rebuilt DLL has a new stamp

    ModuleIdentity sz1 = a;
    sz1.size += 1U;
    CHECK_FALSE(identity_matches(a, sz1));

    ModuleIdentity z0 = a;
    z0.timestamp = 0U;
    ModuleIdentity z1 = z0;
    CHECK_FALSE(identity_matches(z0, z1)); // a zero stamp (reproducible/stripped) is not an identity

    ModuleIdentity rsds;
    rsds.id_kind = SymbolIdKind::Rsds;
    rsds.id_len  = 16;
    CHECK_FALSE(identity_matches(a, rsds)); // different kinds never match
}

TEST_CASE("debug_file_path builds the PE image (symsrv) key", "[perf][diag][symbols]")
{
    ModuleIdentity m;
    m.id_kind   = SymbolIdKind::PeImage;
    m.timestamp = 0xAABBCCDDU;
    m.size      = 0x30000U;
    m.name.append("winhttp.dll");
    const cont::String p = debug_file_path(m, cont::StringView{"C:/sym"});
    // <root>/<name>/<TimeDateStamp:08X><SizeOfImage:X>/<name>
    CHECK(std::strcmp(p.c_str(), "C:/sym/winhttp.dll/AABBCCDD30000/winhttp.dll") == 0);
}

TEST_CASE("read_symbol_index decodes the PeImage kind and clamps only the unknown ones", "[perf][diag][symbols]")
{
    // A single PeImage record round-trips as PeImage (guards the clamp bound moving to kMaxSymbolIdKind).
    ModuleIdentity m;
    m.id_kind   = SymbolIdKind::PeImage;
    m.timestamp = 0x2233U;
    m.size      = 0x1000U;
    m.unloaded  = true;
    m.name.append("x.dll");
    const cont::Array<crd::u8>        payload = serialize_symbol_index(span_of(&m, 1));
    const cont::Array<ModuleIdentity> back    = read_symbol_index(span_of(payload.data(), payload.size()));
    REQUIRE(back.size() == 1U);
    CHECK(back[0].id_kind == SymbolIdKind::PeImage); // 3 is valid, not clamped
    CHECK(back[0].unloaded);

    // id_kind just past the max clamps to None.
    cont::Array<crd::u8> bad = payload;
    const crd::u8        bogus = static_cast<crd::u8>(kMaxSymbolIdKind + 1);
    const crd::usize     off   = sizeof(SymbolIndexHeader) + offsetof(SymbolRecord, id_kind);
    std::memcpy(&bad[off], &bogus, sizeof(bogus));
    const cont::Array<ModuleIdentity> clamped = read_symbol_index(span_of(bad.data(), bad.size()));
    REQUIRE(clamped.size() == 1U);
    CHECK(clamped[0].id_kind == SymbolIdKind::None);
}
