// DIAG.9b -- the build and symbol identity of a flight bundle. A capture's replay recipe and events only mean something
// on the build and the binaries that recorded them, so a flight bundle carries a BuildIdentity section (tag 7) and a
// SymbolIndex (tag 5) of the host's module snapshot. The live module enumeration behind that snapshot is held to
// oracles that share nothing with it: on Windows a minidump of this same process written by the OS (DbgHelp) and read
// by the DIAG.5d parser; on every platform the module FILES on disk read by a separate parser
// (module_file_identity.hpp). The build identity is held to the build's own macros and to an independent probe of the
// fault seam, and a reader refuses every malformed payload.
//
// Contract: docs/design/runtime-diagnostics.md (DIAG.9b); ADR-0133.

#include "module_file_identity.hpp"

#include <crd/perf/build_identity.hpp>
#include <crd/perf/bundle.hpp>
#include <crd/perf/bundle_import.hpp>
#include <crd/perf/diagnostics.hpp>
#include <crd/perf/flight_commands.hpp>
#include <crd/perf/flight_recorder.hpp>
#include <crd/perf/symbol_index.hpp>

#include <crd/core/build_config.hpp>
#include <crd/core/crash.hpp>
#include <crd/core/fault_seam.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <system_error>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h> // LoadLibraryExW, FreeLibrary, GetModuleHandleW
#endif

namespace
{
namespace cp   = crd::perf;
namespace cont = crd::containers;
namespace fs   = std::filesystem;
namespace mf   = crd::test::module_file;

cont::StringView view(const cont::String& s)
{
    return cont::StringView{s.data(), s.size()};
}

// Whether the I/O fault seam is compiled into this build, probed by installing a hook rather than asking the identity.
bool seam_installs()
{
    const auto hook = [](void*, crd::IoFaultOp, const char*, crd::usize, crd::u64, crd::u64) noexcept
    {
        return crd::IoFault{};
    };
    if (!crd::install_io_fault_hook(+hook, nullptr))
    {
        return false;
    }
    crd::uninstall_io_fault_hook();
    return true;
}

#if defined(_WIN32)
// The Windows cases' helpers: a dump directory, module lookups and the case-blind name comparison.
fs::path scratch_dir(const char* tag)
{
    fs::path p = fs::temp_directory_path();
    char     name[64];
    (void)std::snprintf(name, sizeof(name), "crd_flight_id_%s_%u", tag,
                        static_cast<unsigned>(std::chrono::steady_clock::now().time_since_epoch().count() & 0xFFFFFF));
    p /= name;
    std::error_code ec;
    fs::remove_all(p, ec);
    fs::create_directories(p, ec);
    return p;
}

// Windows module and PDB names compare without case: a dump records the mapped file's case, the loader the case the
// module was loaded by.
bool same_name_ignoring_case(cont::StringView a, cont::StringView b)
{
    if (a.size() != b.size())
    {
        return false;
    }
    for (crd::usize i = 0U; i < a.size(); ++i)
    {
        const auto lower = [](char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; };
        if (lower(a[i]) != lower(b[i]))
        {
            return false;
        }
    }
    return true;
}

const cp::ModuleIdentity* find_by_base(const cont::Array<cp::ModuleIdentity>& list, crd::u64 base)
{
    for (const cp::ModuleIdentity& m : list)
    {
        if (m.base == base)
        {
            return &m;
        }
    }
    return nullptr;
}

const cp::ModuleIdentity* find_by_name(const cont::Array<cp::ModuleIdentity>& list, cont::StringView name,
                                       bool unloaded)
{
    for (const cp::ModuleIdentity& m : list)
    {
        if (view(m.name) == name && m.unloaded == unloaded)
        {
            return &m;
        }
    }
    return nullptr;
}
#endif

cp::ModuleIdentity module(crd::u64 base, const char* name, crd::u8 id_byte)
{
    cp::ModuleIdentity m;
    m.base    = base;
    m.size    = 0x1000U;
    m.id_kind = cp::SymbolIdKind::GnuBuildId;
    m.id_len  = 20U;
    std::memset(m.id, id_byte, 20U);
    m.name = cont::String(name);
    return m;
}

cp::FlightCapture small_capture()
{
    cp::FlightRecorder r;
    cp::FlightPolicy   p;
    p.capacity       = 16U;
    p.pre_events     = 8U;
    p.post_events    = 2U;
    p.post_window_ns = 0U;
    REQUIRE(r.init(p) == cp::FlightPolicyStatus::Committed);
    r.record(0x0042U, cp::Severity::Info, cont::StringView{"before"});
    cp::FlightTriggerInput in;
    in.message = cont::StringView{"identity"};
    REQUIRE(r.trigger(in) == cp::FlightTriggerResult::Started);
    r.record(0x0042U, cp::Severity::Info, cont::StringView{"after 1"});
    r.record(0x0042U, cp::Severity::Info, cont::StringView{"after 2"});
    cp::FlightCapture c;
    REQUIRE(r.take_capture(c));
    return c;
}

const cp::ImportedSection* section(const cp::BundleImport& imp, cp::BundleSectionTag tag)
{
    for (const cp::ImportedSection& s : imp.sections)
    {
        if (s.tag == tag)
        {
            return &s;
        }
    }
    return nullptr;
}
} // namespace

TEST_CASE("build identity: this build's fields come from its own configuration", "[perf][diag][diag9b][identity]")
{
    const cp::BuildIdentity b = cp::current_build_identity();
    CHECK(b.diagnostic_schema == cp::kDiagnosticSchemaVersion);
    CHECK(view(b.version) == CRD_VERSION_STRING);
#if defined(_WIN32)
    CHECK(view(b.platform) == "WINDOWS");
#elif defined(__linux__)
    CHECK(view(b.platform) == "LINUX");
#endif
#if defined(CRD_DEBUG)
    CHECK(view(b.config) == "debug");
#else
    CHECK(view(b.config) == "release");
#endif
    CHECK(((b.flags & cp::kBuildFlagAsserts) != 0U) == (CRD_ENABLE_ASSERTS != 0));
    CHECK(((b.flags & cp::kBuildFlagProfiling) != 0U) == (CRD_ENABLE_PROFILING != 0));
    // Independent of crd::fault_seams_compiled(): whether a hook actually installs.
    CHECK(((b.flags & cp::kBuildFlagFaultSeams) != 0U) == seam_installs());
    CHECK_FALSE(b.compiler_version.empty());
    CHECK((b.flags & ~cp::kBuildKnownFlags) == 0U);

    cont::String json;
    cp::build_identity_json(b, json);
    CHECK(view(json).find("\"config\":\"") != cont::StringView::npos);
    CHECK(view(json).find("\"fault_seams\":") != cont::StringView::npos);
}

TEST_CASE("build identity: the section round-trips and every malformed payload is refused",
          "[perf][diag][diag9b][identity]")
{
    const cp::BuildIdentity    b       = cp::current_build_identity();
    const cont::Array<crd::u8> payload = cp::encode_build_identity(b);
    const cp::BuildIdentityRead back   = cp::decode_build_identity({payload.data(), payload.size()});
    REQUIRE(back.ok);
    cont::String differing;
    CHECK(cp::same_build_identity(back.identity, b, differing));
    CHECK(differing.empty());

    int accepted_prefixes = 0;
    for (crd::usize n = 0U; n < payload.size(); ++n)
    {
        if (cp::decode_build_identity({payload.data(), n}).ok)
        {
            ++accepted_prefixes;
        }
    }
    CHECK(accepted_prefixes == 0);

    cont::Array<crd::u8> bad = payload;
    bad[0]                   = 0x00U;
    CHECK(cp::decode_build_identity({bad.data(), bad.size()}).error == "not a build identity payload (magic)");

    bad    = payload;
    bad[4] = 2U; // version
    CHECK(cp::decode_build_identity({bad.data(), bad.size()}).error == "unsupported build identity version");

    bad     = payload;
    bad[15] = 0x80U; // the top flag bit: not a known flag
    CHECK(cp::decode_build_identity({bad.data(), bad.size()}).error == "unknown build identity flags");

    bad     = payload;
    bad[16] = static_cast<crd::u8>(cp::kBuildFieldBytes + 1U); // the version field's length, over its bound
    bad[17] = 0U;
    CHECK(cp::decode_build_identity({bad.data(), bad.size()}).error ==
          "a build identity field is over its bound or past the payload");

    bad = payload;
    bad.push_back(0U);
    CHECK(cp::decode_build_identity({bad.data(), bad.size()}).error == "trailing bytes after the build identity");

    // An over-long field is clipped on encode, so the payload stays readable.
    cp::BuildIdentity longer = b;
    longer.compiler_version.clear();
    for (crd::u32 i = 0U; i < 200U; ++i)
    {
        longer.compiler_version.push_back('v');
    }
    const cont::Array<crd::u8>  clipped = cp::encode_build_identity(longer);
    const cp::BuildIdentityRead read    = cp::decode_build_identity({clipped.data(), clipped.size()});
    REQUIRE(read.ok);
    CHECK(read.identity.compiler_version.size() == cp::kBuildFieldBytes);
}

TEST_CASE("build identity: a comparison names every differing field", "[perf][diag][diag9b][identity]")
{
    const cp::BuildIdentity base = cp::current_build_identity();
    struct Edit
    {
        const char* field;
        void (*apply)(cp::BuildIdentity&);
    };
    const Edit edits[] = {
        {"diagnostic_schema", [](cp::BuildIdentity& b) { b.diagnostic_schema += 1U; }},
        {"version", [](cp::BuildIdentity& b) { b.version.push_back('x'); }},
        {"platform", [](cp::BuildIdentity& b) { b.platform.push_back('x'); }},
        {"compiler", [](cp::BuildIdentity& b) { b.compiler.push_back('x'); }},
        {"compiler_version", [](cp::BuildIdentity& b) { b.compiler_version.push_back('x'); }},
        {"arch", [](cp::BuildIdentity& b) { b.arch.push_back('x'); }},
        {"config", [](cp::BuildIdentity& b) { b.config.push_back('x'); }},
        {"asserts", [](cp::BuildIdentity& b) { b.flags ^= cp::kBuildFlagAsserts; }},
        {"profiling", [](cp::BuildIdentity& b) { b.flags ^= cp::kBuildFlagProfiling; }},
        {"fault_seams", [](cp::BuildIdentity& b) { b.flags ^= cp::kBuildFlagFaultSeams; }},
        {"asan", [](cp::BuildIdentity& b) { b.flags ^= cp::kBuildFlagAsan; }},
        {"tsan", [](cp::BuildIdentity& b) { b.flags ^= cp::kBuildFlagTsan; }},
    };
    for (const Edit& e : edits)
    {
        cp::BuildIdentity other = base;
        e.apply(other);
        cont::String differing;
        INFO("field " << e.field);
        CHECK_FALSE(cp::same_build_identity(base, other, differing));
        CHECK(view(differing) == e.field);
    }
    cp::BuildIdentity two = base;
    two.config.push_back('x');
    two.flags ^= cp::kBuildFlagTsan;
    cont::String differing;
    CHECK_FALSE(cp::same_build_identity(base, two, differing));
    CHECK(view(differing) == "config,tsan");
}

TEST_CASE("live modules: the enumeration names this executable and matches its file on disk",
          "[perf][diag][diag9b][identity][symbols]")
{
    const cp::LiveModules live = cp::enumerate_loaded_modules();
    REQUIRE(live.available);
    CHECK_FALSE(live.overflow);
    REQUIRE(live.modules.size() >= 2U);

    // The first module is the executable itself; its file, read by the separate parser, has the same identity.
#if defined(_WIN32)
    wchar_t wide[MAX_PATH] = {};
    REQUIRE(::GetModuleFileNameW(nullptr, wide, MAX_PATH) > 0U);
    const cont::String exe_path{fs::path(wide).string().c_str()};
#else
    const cont::String exe_path{fs::read_symlink("/proc/self/exe").string().c_str()};
#endif
    const cp::ModuleIdentity& exe = live.modules[0];
    CHECK(view(exe.name) == fs::path(exe_path.c_str()).filename().string());
    cp::ModuleIdentity from_file;
    REQUIRE(mf::file_identity(exe_path.c_str(), from_file));
    CHECK(mf::same_identity(exe, from_file));
#if defined(_WIN32)
    // The file decides which identity there is to name. A build linked with a PDB carries a CodeView record; one linked
    // without (win-release: plain Release keeps no debug information) has none, and then the live walk must not
    // invent one: the module stays named by its PE fields and can never match a debug file.
    if (from_file.id_kind != cp::SymbolIdKind::None)
    {
        CHECK(from_file.id_kind == cp::SymbolIdKind::Rsds);
        CHECK(exe.id_kind == cp::SymbolIdKind::Rsds);
        CHECK(view(exe.debug_file) == view(from_file.debug_file));
        CHECK(view(exe.debug_file).find('\\') == cont::StringView::npos); // a basename: no directory leaves the machine
        CHECK(cp::identity_matches(exe, from_file));
    }
    else
    {
        CHECK(exe.id_kind == cp::SymbolIdKind::None);
        CHECK(view(exe.debug_file).empty());
        CHECK(exe.timestamp != 0U);
        CHECK_FALSE(cp::identity_matches(exe, from_file));
    }
#else
    CHECK(from_file.id_kind == cp::SymbolIdKind::GnuBuildId);
    CHECK(exe.id_kind == cp::SymbolIdKind::GnuBuildId);
    CHECK(cp::identity_matches(exe, from_file));
#endif
    for (const cp::ModuleIdentity& m : live.modules)
    {
        CHECK(view(m.name).find('/') == cont::StringView::npos);
        CHECK(view(m.name).find('\\') == cont::StringView::npos);
        CHECK(m.size > 0U);
        CHECK_FALSE(m.unloaded);
    }
}

#if defined(_WIN32)
TEST_CASE("live modules: every module matches a minidump of this process written by the OS",
          "[perf][diag][diag9b][identity][symbols]")
{
    namespace crash = crd::crash;
    const fs::path dir = scratch_dir("dump");
    crash::uninstall();
    REQUIRE(crash::install(dir.string().c_str()) == crash::InstallResult::Ok);
    const cp::LiveModules before = cp::enumerate_loaded_modules();
    const wchar_t*        dump   = nullptr;
    const crash::WriteResult w   = crash::capture_dump(&dump);
    const cp::LiveModules after  = cp::enumerate_loaded_modules();
    crash::uninstall();
    REQUIRE(w == crash::WriteResult::Ok);
    REQUIRE(dump != nullptr);
    const cont::Array<crd::u8> bytes = mf::read_whole_file(fs::path(dump).string().c_str());
    REQUIRE(bytes.size() > 0U);
    const cont::Array<cp::ModuleIdentity> dumped = cp::parse_minidump_modules({bytes.data(), bytes.size()});
    REQUIRE(dumped.size() >= 2U);

    // Modules loaded in both enumerations were loaded while the OS wrote the dump: each must be there, identical.
    crd::usize compared = 0U;
    crd::usize with_rsds = 0U;
    for (const cp::ModuleIdentity& m : before.modules)
    {
        if (find_by_base(after.modules, m.base) == nullptr)
        {
            continue;
        }
        const cp::ModuleIdentity* d = find_by_base(dumped, m.base);
        INFO("module " << m.name.c_str());
        REQUIRE(d != nullptr);
        CHECK(same_name_ignoring_case(fs::path(d->name.c_str()).filename().string(), view(m.name)));
        CHECK(d->size == m.size);
        CHECK(d->timestamp == m.timestamp);
        CHECK(d->checksum == m.checksum);
        CHECK(d->id_kind == m.id_kind);
        CHECK(d->age == m.age);
        CHECK(std::memcmp(d->id, m.id, cp::kMaxSymbolIdBytes) == 0);
        if (d->id_kind == cp::SymbolIdKind::Rsds)
        {
            CHECK(same_name_ignoring_case(fs::path(d->debug_file.c_str()).filename().string(), view(m.debug_file)));
            ++with_rsds;
        }
        ++compared;
    }
    CHECK(compared >= dumped.size() - 2U); // at most a module or two came and went around the dump
    CHECK(with_rsds >= 2U);                // the executable and system DLLs carry CodeView records
    CHECK(dumped[0].base == before.modules[0].base); // both list the executable first

    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("module snapshot: a DLL loaded and freed stays named, marked unloaded, in the flight bundle",
          "[perf][diag][diag9b][identity][symbols]")
{
    // A system DLL this process has not loaded (each is one the test exe never links).
    const wchar_t* const candidates[] = {L"winhttp.dll", L"wtsapi32.dll", L"mscms.dll", L"netapi32.dll"};
    const wchar_t*       chosen       = nullptr;
    for (const wchar_t* c : candidates)
    {
        if (::GetModuleHandleW(c) == nullptr)
        {
            chosen = c;
            break;
        }
    }
    REQUIRE(chosen != nullptr);
    const cont::String name{fs::path(chosen).string().c_str()};

    cp::FlightRecorder    recorder;
    cp::FlightCommandHost host(recorder);
    REQUIRE(host.refresh_modules());
    CHECK(find_by_name(host.modules(), view(name), false) == nullptr);

    HMODULE dll = ::LoadLibraryExW(chosen, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    REQUIRE(dll != nullptr);
    REQUIRE(host.refresh_modules());
    const cont::Array<cp::ModuleIdentity> loaded = host.modules();
    const cp::ModuleIdentity*             live   = find_by_name(loaded, view(name), false);
    REQUIRE(live != nullptr);
    CHECK(live->base == reinterpret_cast<crd::u64>(dll));
    CHECK(live->id_kind == cp::SymbolIdKind::Rsds);
    const cp::ModuleIdentity kept = *live;

    REQUIRE(::FreeLibrary(dll) != FALSE);
    REQUIRE(::GetModuleHandleW(chosen) == nullptr); // nothing else held it
    REQUIRE(host.refresh_modules());
    const cont::Array<cp::ModuleIdentity> after = host.modules();
    CHECK(find_by_name(after, view(name), false) == nullptr);
    const cp::ModuleIdentity* gone = find_by_name(after, view(name), true);
    REQUIRE(gone != nullptr);
    CHECK(gone->base == kept.base);
    CHECK(mf::same_identity(*gone, kept));
    CHECK(after.size() == loaded.size());

    // A bundle written now still names the freed DLL, with its identity, as an unloaded generation.
    const cp::FlightCapture    c      = small_capture();
    const cont::Array<crd::u8> bundle = cp::write_flight_bundle(c, {after.data(), after.size()});
    const cp::BundleImport     imp    = cp::import_bundle({bundle.data(), bundle.size()});
    REQUIRE(imp.status == cp::ImportStatus::Ok);
    REQUIRE(imp.has_symbols);
    const cp::ModuleIdentity* named = find_by_name(imp.symbols, view(name), true);
    REQUIRE(named != nullptr);
    CHECK(cp::identity_matches(*named, kept));
}
#else
TEST_CASE("live modules: each module with a build-id matches its mapped file on disk",
          "[perf][diag][diag9b][identity][symbols]")
{
    const cp::LiveModules live = cp::enumerate_loaded_modules();
    REQUIRE(live.available);
    // The files mapped into this process, by basename, from the kernel's own list.
    const cont::Array<crd::u8> maps = mf::read_whole_file("/proc/self/maps");
    REQUIRE(maps.size() > 0U);
    const cont::StringView text{reinterpret_cast<const char*>(maps.data()), maps.size()};
    crd::usize              compared = 0U;
    for (const cp::ModuleIdentity& m : live.modules)
    {
        if (m.id_kind != cp::SymbolIdKind::GnuBuildId)
        {
            continue;
        }
        // Find a mapping line whose path ends in "/<name>" and whose range holds the module's base.
        cont::String path;
        crd::usize   at = 0U;
        while (at < text.size() && path.empty())
        {
            const crd::usize       eol  = text.find('\n', at);
            const cont::StringView line = text.substr(at, (eol == cont::StringView::npos ? text.size() : eol) - at);
            at                          = eol == cont::StringView::npos ? text.size() : eol + 1U;
            const crd::usize slash      = line.rfind('/');
            if (slash == cont::StringView::npos || line.substr(slash + 1U) != view(m.name))
            {
                continue;
            }
            // "lo-hi perms ...": both hex, read from a NUL-terminated copy of the range field.
            char                   range[64] = {};
            const cont::StringView field     = line.substr(0U, line.find(' '));
            std::memcpy(range, field.data(), field.size() < sizeof(range) - 1U ? field.size() : sizeof(range) - 1U);
            const char*              dash = std::strchr(range, '-');
            const unsigned long long lo   = std::strtoull(range, nullptr, 16);
            const unsigned long long hi   = dash != nullptr ? std::strtoull(dash + 1, nullptr, 16) : 0U;
            if (m.base >= lo && m.base < hi)
            {
                path = cont::String(line.substr(line.find('/')));
            }
        }
        if (path.empty())
        {
            continue; // e.g. the vDSO: no file
        }
        INFO("module " << m.name.c_str() << " at " << path.c_str());
        cp::ModuleIdentity from_file;
        REQUIRE(mf::file_identity(path.c_str(), from_file));
        CHECK(mf::same_identity(m, from_file));
        ++compared;
    }
    CHECK(compared >= 2U); // the executable and libc at least
}
#endif

TEST_CASE("module snapshot: a merge keeps unloaded generations and appends new ones, bounded",
          "[perf][diag][diag9b][identity][symbols]")
{
    cont::Array<cp::ModuleIdentity> known;
    cont::Array<cp::ModuleIdentity> live;
    live.push_back(module(0x1000U, "a.so", 1U));
    live.push_back(module(0x2000U, "b.so", 2U));
    bool overflow = false;
    CHECK(cp::merge_module_snapshot(known, {live.data(), live.size()}, overflow) == 2U);
    CHECK(cp::merge_module_snapshot(known, {live.data(), live.size()}, overflow) == 0U); // stable
    REQUIRE(known.size() == 2U);

    // b unloads; c loads at b's old base with another identity; a reloads unchanged.
    live.clear();
    live.push_back(module(0x1000U, "a.so", 1U));
    live.push_back(module(0x2000U, "c.so", 3U));
    CHECK(cp::merge_module_snapshot(known, {live.data(), live.size()}, overflow) == 2U);
    REQUIRE(known.size() == 3U);
    CHECK_FALSE(known[0].unloaded);
    CHECK(known[1].unloaded);
    CHECK(view(known[1].name) == "b.so");
    CHECK_FALSE(known[2].unloaded);

    // b loads again at the same place: a new generation beside the unloaded one.
    live.push_back(module(0x2000U, "b.so", 2U));
    CHECK(cp::merge_module_snapshot(known, {live.data(), live.size()}, overflow) == 1U);
    REQUIRE(known.size() == 4U);
    CHECK(known[1].unloaded);
    CHECK_FALSE(known[3].unloaded);
    CHECK_FALSE(overflow);

    // The bound: a full snapshot drops new modules and says so.
    cont::Array<cp::ModuleIdentity> many;
    for (crd::u32 i = 0U; i < cp::kMaxLiveModules; ++i)
    {
        many.push_back(module(0x100000ULL + i * 0x1000ULL, "m.so", 4U));
    }
    cont::Array<cp::ModuleIdentity> full;
    CHECK(cp::merge_module_snapshot(full, {many.data(), many.size()}, overflow) == cp::kMaxLiveModules);
    CHECK_FALSE(overflow);
    many.push_back(module(0x9000000ULL, "extra.so", 5U));
    (void)cp::merge_module_snapshot(full, {many.data(), many.size()}, overflow);
    CHECK(overflow);
    CHECK(full.size() == cp::kMaxLiveModules);
}

TEST_CASE("flight bundle: it carries this build's identity and the module snapshot, or declares the snapshot absent",
          "[perf][diag][diag9b][identity][flight]")
{
    const cp::FlightCapture c    = small_capture();
    const cp::LiveModules   live = cp::enumerate_loaded_modules();
    REQUIRE(live.available);

    const cont::Array<crd::u8> bundle = cp::write_flight_bundle(c, {live.modules.data(), live.modules.size()});
    const cp::BundleImport     imp    = cp::import_bundle({bundle.data(), bundle.size()});
    REQUIRE(imp.status == cp::ImportStatus::Ok);
    REQUIRE(section(imp, cp::BundleSectionTag::FlightRecord) != nullptr);
    REQUIRE(section(imp, cp::BundleSectionTag::BuildIdentity) != nullptr);
    REQUIRE(imp.has_build);
    CHECK_FALSE(imp.build_refused);
    cont::String differing;
    CHECK(cp::same_build_identity(imp.build, cp::current_build_identity(), differing));
    CHECK(differing.empty());
    REQUIRE(imp.has_symbols);
    REQUIRE(imp.symbols.size() == live.modules.size());
    for (crd::usize i = 0U; i < live.modules.size(); ++i)
    {
        CHECK(cp::identity_matches(imp.symbols[i], live.modules[i]) ==
              (live.modules[i].id_kind != cp::SymbolIdKind::None));
        CHECK(imp.symbols[i].base == live.modules[i].base);
        CHECK(view(imp.symbols[i].name) == view(live.modules[i].name));
    }
    CHECK(imp.unsafe_names == 0U);
    CHECK(imp.manifest.absent_tags.empty());

    // Without a snapshot the SymbolIndex is an explicit absent section the manifest declares.
    const cont::Array<crd::u8> bare     = cp::write_flight_bundle(c);
    const cp::BundleImport     bare_imp = cp::import_bundle({bare.data(), bare.size()});
    REQUIRE(bare_imp.status == cp::ImportStatus::Ok);
    REQUIRE(bare_imp.has_build);
    const cp::ImportedSection* absent = section(bare_imp, cp::BundleSectionTag::SymbolIndex);
    REQUIRE(absent != nullptr);
    CHECK((absent->section_flags & cp::kSectionFlagAbsent) != 0U);
    CHECK_FALSE(bare_imp.has_symbols);
    REQUIRE(bare_imp.manifest.absent_tags.size() == 1U);
    CHECK(bare_imp.manifest.absent_tags[0] == static_cast<crd::u32>(cp::BundleSectionTag::SymbolIndex));
    CHECK(bare_imp.manifest_absent_mismatches == 0U);

    // A corrupt build identity is reported as refused, never decoded into a plausible build.
    cont::Array<crd::u8> payload = cp::encode_build_identity(cp::current_build_identity());
    payload.push_back(0U);
    cp::BundleWriter writer;
    (void)writer.add_section(cp::BundleSectionTag::BuildIdentity, {payload.data(), payload.size()});
    const cont::Array<crd::u8> corrupt     = writer.finish();
    const cp::BundleImport     corrupt_imp = cp::import_bundle({corrupt.data(), corrupt.size()});
    CHECK_FALSE(corrupt_imp.has_build);
    CHECK(corrupt_imp.build_refused);
}
