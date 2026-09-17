// DIAG.5d(d): the crash dump retains UNLOADED module generations. Verified from OUTSIDE the process: the unload_av
// specimen loads winhttp.dll, frees it (a traced unload), reloads it, then faults -- so at crash time the dump names
// winhttp.dll in BOTH the UnloadedModuleListStream (the old generation) and the ModuleListStream (the live one). Two
// generations, one dump. This is the "retain old DLL symbols after reload" acceptance. The crd-native parser for the
// same stream lives in crd-perf (test_diag_symbol_index.cpp); crd-core-tests does not link crd-perf, so this proves
// CAPTURE with its own minimal readback via read_dump_stream (DbgHelp types only, no dbghelp.lib link).

#include <crd/diag/specimen_runner.hpp>

#include <crd/containers/array.hpp>
#include <crd/containers/string.hpp>

#include <catch2/catch_test_macros.hpp>

#if defined(_WIN32)

#include <crd/core/crash.hpp> // read_dump_stream

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// clang-format off
#include <DbgHelp.h> // MINIDUMP_* struct layouts only (no DbgHelp call -> no link); must follow windows.h
// clang-format on

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string_view>

namespace cont = crd::containers;
namespace cd   = crd::diag;
namespace fs   = std::filesystem;

namespace
{
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

fs::path fresh_dir(const char* mode)
{
    fs::path p = fs::temp_directory_path();
    char     name[64];
    std::snprintf(name, sizeof(name), "crd_unl_%s_%u", mode, g_uniq.fetch_add(1U));
    p /= name;
    std::error_code ec;
    fs::remove_all(p, ec); // install() creates it
    return p;
}

cd::Outcome run_mode(const char* mode, const fs::path& dir)
{
    cont::Array<cont::String> args;
    args.push_back(cont::String{dir.string().c_str()});
    args.push_back(cont::String{mode});
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

// Read a dump stream's bytes into an Array (the stream body; RVAs inside it are FILE offsets).
cont::Array<crd::u8> read_stream(const fs::path& dmp, std::uint32_t stream_type)
{
    cont::Array<crd::u8> out;
    const std::size_t    sz = crd::crash::read_dump_stream(dmp.wstring().c_str(), stream_type, nullptr, 0);
    if (sz == 0)
        return out;
    out.resize_uninitialized(sz);
    const std::size_t got = crd::crash::read_dump_stream(dmp.wstring().c_str(), stream_type, out.data(), sz);
    out.resize(got);
    return out;
}

// A MINIDUMP_STRING at file offset `rva` in `whole`: is its basename (case-insensitive) `want` (lowercase ASCII)?
bool name_matches(const cont::Array<crd::u8>& whole, DWORD rva, const char* want)
{
    if (rva == 0 || static_cast<std::size_t>(rva) + sizeof(ULONG32) > whole.size())
        return false;
    ULONG32 nbytes = 0;
    std::memcpy(&nbytes, whole.data() + rva, sizeof(nbytes));
    const std::size_t soff  = static_cast<std::size_t>(rva) + sizeof(ULONG32);
    const std::size_t chars = nbytes / sizeof(wchar_t);
    if (soff + chars * sizeof(wchar_t) > whole.size())
        return false;
    cont::String name;
    for (std::size_t i = 0; i < chars; ++i)
    {
        wchar_t wc = 0;
        std::memcpy(&wc, whole.data() + soff + i * sizeof(wchar_t), sizeof(wc));
        char c = (wc < 128) ? static_cast<char>(wc) : '?';
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c + 32);
        name.push_back(c);
    }
    std::string_view sv{name.c_str(), name.size()};
    const std::size_t pos = sv.find_last_of("/\\");
    const std::string_view bn = (pos == std::string_view::npos) ? sv : sv.substr(pos + 1);
    return bn == want;
}

bool loaded_list_has(const cont::Array<crd::u8>& whole, const cont::Array<crd::u8>& stream, const char* want)
{
    if (stream.size() < sizeof(ULONG32))
        return false;
    ULONG32 count = 0;
    std::memcpy(&count, stream.data(), sizeof(count));
    for (ULONG32 i = 0; i < count; ++i)
    {
        const std::size_t off = sizeof(ULONG32) + static_cast<std::size_t>(i) * sizeof(MINIDUMP_MODULE);
        if (off + sizeof(MINIDUMP_MODULE) > stream.size())
            break;
        MINIDUMP_MODULE mod{};
        std::memcpy(&mod, stream.data() + off, sizeof(mod));
        if (name_matches(whole, mod.ModuleNameRva, want))
            return true;
    }
    return false;
}

bool unloaded_list_has(const cont::Array<crd::u8>& whole, const cont::Array<crd::u8>& stream, const char* want)
{
    if (stream.size() < sizeof(MINIDUMP_UNLOADED_MODULE_LIST))
        return false;
    MINIDUMP_UNLOADED_MODULE_LIST ul{};
    std::memcpy(&ul, stream.data(), sizeof(ul));
    if (ul.SizeOfHeader < sizeof(ul) || ul.SizeOfEntry < sizeof(MINIDUMP_UNLOADED_MODULE))
        return false;
    for (ULONG32 i = 0; i < ul.NumberOfEntries; ++i)
    {
        const std::size_t off = ul.SizeOfHeader + static_cast<std::size_t>(i) * ul.SizeOfEntry; // self-describing stride
        if (off + sizeof(MINIDUMP_UNLOADED_MODULE) > stream.size())
            break;
        MINIDUMP_UNLOADED_MODULE em{};
        std::memcpy(&em, stream.data() + off, sizeof(em));
        if (name_matches(whole, em.ModuleNameRva, want))
            return true;
    }
    return false;
}
} // namespace

TEST_CASE("crash dump (windows): retains an unloaded module generation across a reload", "[core][diag][unloaded]")
{
    const fs::path    dir = fresh_dir("unload");
    const cd::Outcome o   = run_mode("unload_av", dir);
    INFO("verdict=" << cd::verdict_name(o.verdict) << " exit=" << o.exit_code);

    if (built_with_asan())
    {
        CHECK(o.exit_code != 0); // ASan owns the AV; no crd dump is written (asserted, never skipped)
        std::error_code ec;
        fs::remove_all(dir, ec);
        return;
    }

    REQUIRE(o.verdict == cd::Verdict::Crashed);
    const fs::path dmp = first_dump(dir);
    REQUIRE_FALSE(dmp.empty());

    const cont::Array<crd::u8> whole = read_bytes(dmp);
    REQUIRE(whole.size() > 0U);

    // UnloadedModuleListStream (14): the OLD generation. Its presence at all proves MiniDumpWithUnloadedModules is set.
    const cont::Array<crd::u8> unloaded = read_stream(dmp, 14U);
    REQUIRE(unloaded.size() >= sizeof(MINIDUMP_UNLOADED_MODULE_LIST));
    CHECK(unloaded_list_has(whole, unloaded, "winhttp.dll"));

    // ModuleListStream (4): the LIVE generation (winhttp was reloaded before the fault).
    const cont::Array<crd::u8> loaded = read_stream(dmp, 4U);
    REQUIRE(loaded.size() >= sizeof(ULONG32));
    CHECK(loaded_list_has(whole, loaded, "winhttp.dll")); // two generations, one dump

    std::error_code ec;
    fs::remove_all(dir, ec);
}

#endif // defined(_WIN32)
