// DIAG.5d(f): the "symbolize an optimized fresh-machine crash" acceptance clause, end to end on a REAL minidump (not
// synthetic bytes). It runs the unload_av capture specimen -> a real dump carrying two winhttp generations -> the
// crd-perf symbolization pipeline (build_symbol_index_from_minidump -> a SymbolIndex bundle section -> import_bundle),
// and proves, on that real dump: (1) EVERY identity-bearing module resolves to a safe, non-empty debug-file lookup
// path -- "every module, not just the faulting binary"; a module with no CV/build-id record resolves to "" (refuse,
// don't guess); (2) at least one module is an unloaded generation -- "retain old DLL symbols after reload"; (3) a
// wrong identity (age+1) is rejected by identity_matches -- "reject wrong symbols instead of giving plausible lines".
// The frame symbolizer itself is the offline consumer's job (5b's rule: symbolization is offline, out of the handler);
// this test proves the identity + matching-file path + refusal a fresh-machine symbolizer consumes. Ties (c1)+(d)+(e1).
#include <crd/diag/specimen_runner.hpp>

#include <crd/containers/array.hpp>
#include <crd/containers/string.hpp>
#include <crd/perf/bundle.hpp>
#include <crd/perf/bundle_import.hpp>
#include <crd/perf/symbol_index.hpp>

#include <catch2/catch_test_macros.hpp>

#if defined(_WIN32)

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <system_error>

namespace cont = crd::containers;
namespace cd   = crd::diag;
namespace fs   = std::filesystem;
using namespace crd::perf;

namespace
{
template <typename T> cont::ConstSpan<T> span_of(const T* p, crd::usize n) noexcept
{
    return cont::ConstSpan<T>{p, n};
}

bool built_with_asan()
{
#if defined(__SANITIZE_ADDRESS__)
    return true;
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
    return true;
#else
    return false;
#endif
#else
    return false;
#endif
}

cont::String specimen_path()
{
    return cont::String{CRD_DIAG_CRASH_CAPTURE_SPECIMEN};
}

std::atomic<unsigned> g_uniq{0};

fs::path fresh_dir()
{
    fs::path p = fs::temp_directory_path();
    char     name[64];
    std::snprintf(name, sizeof(name), "crd_sym_e2e_%u", g_uniq.fetch_add(1U));
    p /= name;
    std::error_code ec;
    fs::remove_all(p, ec); // the specimen's install() creates it
    return p;
}

cd::Outcome run_unload_av(const fs::path& dir)
{
    cont::Array<cont::String> args;
    args.push_back(cont::String{dir.string().c_str()});
    args.push_back(cont::String{"unload_av"});
    cd::Expectation e;
    e.want       = cd::Expectation::Want::Crash;
    e.timeout_ms = 15000U;
    return cd::run_specimen(specimen_path(), args, e);
}

fs::path first_dump(const fs::path& dir)
{
    std::error_code ec;
    for (fs::directory_iterator it{dir, ec}, end; it != end; it.increment(ec))
        if (it->path().extension() == ".dmp")
            return it->path();
    return {};
}

cont::Array<crd::u8> read_bytes(const fs::path& p)
{
    cont::Array<crd::u8> out;
    std::FILE*           f = nullptr;
    if (_wfopen_s(&f, p.wstring().c_str(), L"rb") != 0 || f == nullptr)
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

TEST_CASE("symbolize (windows): a real dump yields every-module identity + matching path, rejects wrong symbols",
          "[perf][diag][symbols][e2e]")
{
    const fs::path    dir = fresh_dir();
    const cd::Outcome o   = run_unload_av(dir);
    INFO("verdict=" << cd::verdict_name(o.verdict) << " exit=" << o.exit_code);

    std::error_code ec;
    if (built_with_asan())
    {
        CHECK(o.exit_code != 0); // ASan owns the AV; no crd dump is written (asserted, never skipped as pass)
        fs::remove_all(dir, ec);
        return;
    }

    REQUIRE(o.verdict == cd::Verdict::Crashed);
    const fs::path dmp = first_dump(dir);
    REQUIRE_FALSE(dmp.empty());

    const cont::Array<crd::u8> dump = read_bytes(dmp);
    REQUIRE(dump.size() > 0U);

    // The crd-perf pipeline: parse every module + unloaded generation -> serialize -> a SymbolIndex bundle section ->
    // import back (bounded, non-executing). This is exactly the path a fresh-machine symbolizer would consume.
    const cont::Array<crd::u8> section = build_symbol_index_from_minidump(span_of(dump.data(), dump.size()));
    REQUIRE(section.size() > 0U);

    BundleWriter w;
    REQUIRE(w.add_section(BundleSectionTag::SymbolIndex, span_of(section.data(), section.size())) ==
            BundleWriter::AddStatus::Ok);
    const cont::Array<crd::u8> bundle = w.finish(0U);

    const BundleImport imp = import_bundle(span_of(bundle.data(), bundle.size()));
    REQUIRE(imp.status == ImportStatus::Ok);
    REQUIRE(imp.has_symbols);
    REQUIRE(imp.symbols.size() > 0U);
    CHECK(imp.unsafe_names == 0U); // real module basenames are safe

    // (1) "every module": every identity-bearing module resolves to a safe, non-empty candidate path; a module with
    //     no CV/build-id identity resolves to "" (refuse, don't guess -- never a plausible-but-wrong path).
    crd::usize identified = 0U;
    crd::usize unloaded   = 0U;
    for (crd::usize i = 0; i < imp.symbols.size(); ++i)
    {
        const ModuleIdentity& m = imp.symbols[i];
        if (m.unloaded)
            ++unloaded;
        const cont::String path = debug_file_path(m, cont::StringView{"S:/sym"});
        if (m.id_kind == SymbolIdKind::None)
        {
            CHECK(path.empty()); // nothing to look up -> no guess
            continue;
        }
        ++identified;
        CHECK_FALSE(path.empty());
        const cont::StringView key = (m.id_kind == SymbolIdKind::Rsds)
                                         ? cont::StringView{m.debug_file.c_str(), m.debug_file.size()}
                                         : cont::StringView{m.name.c_str(), m.name.size()};
        CHECK(is_safe_lookup_name(key));
    }
    CHECK(identified > 0U); // the real dump carried real identities
    CHECK(unloaded > 0U);   // (2) winhttp's old generation survived the reload

    // (3) reject wrong symbols: the same module with a bumped age must not match (never a plausible-but-wrong line).
    const ModuleIdentity* rsds = nullptr;
    for (crd::usize i = 0; i < imp.symbols.size(); ++i)
        if (imp.symbols[i].id_kind == SymbolIdKind::Rsds)
        {
            rsds = &imp.symbols[i];
            break;
        }
    REQUIRE(rsds != nullptr); // a debug-built specimen always carries an RSDS record -- never skip this clause
    ModuleIdentity wrong = *rsds;
    wrong.age += 1U;
    CHECK(identity_matches(*rsds, *rsds));
    CHECK_FALSE(identity_matches(*rsds, wrong));

    fs::remove_all(dir, ec);
}

#endif // _WIN32
