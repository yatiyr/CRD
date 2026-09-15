// Shared Windows-only minidump content probe for the DIAG.5a crash tests. It reads a written dump back with the
// public read_dump_stream() and proves the dump is READABLE and CORRECTLY IDENTIFIED: the ExceptionStream carries the
// faulting exception code and thread id, and the ModuleListStream names the crashing binary and carries its RSDS CV
// record (GUID + age). That CV identity is exactly what a later symbolization step (DIAG.5d) matches against or
// rejects -- so this answers the "missing symbols" acceptance (the dump carries enough to find or refuse symbols)
// WITHOUT claiming a symbolized frame here. Header-only; both crash test translation units in this directory include
// it via a quoted path (no CMake change). Struct layouts come from <DbgHelp.h> (types only -- no DbgHelp call, so no
// dbghelp.lib link is needed); the RVAs in a module list are file offsets into the whole .dmp (not into the stream),
// so the raw file bytes are indexed directly for names and CV records.
#pragma once

#if defined(_WIN32)

#include <crd/core/crash.hpp>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <DbgHelp.h> // MINIDUMP_* struct layouts (types only; no function call -> no link dependency)

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace crd_test_minidump
{
inline std::vector<unsigned char> read_file_bytes(const std::wstring& path)
{
    std::ifstream f{path, std::ios::binary};
    return std::vector<unsigned char>{std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

// Read the whole ExceptionStream (type 6) of the dump into a struct; false if it is absent or short.
inline bool read_exception_stream(const std::wstring& dump_path, MINIDUMP_EXCEPTION_STREAM& out)
{
    constexpr std::uint32_t kExceptionStream = 6U; // MINIDUMP_STREAM_TYPE::ExceptionStream
    const std::size_t       sz               = crd::crash::read_dump_stream(dump_path.c_str(), kExceptionStream, nullptr, 0);
    if (sz < sizeof(MINIDUMP_EXCEPTION_STREAM))
        return false;
    std::vector<unsigned char> buf(sz);
    if (crd::crash::read_dump_stream(dump_path.c_str(), kExceptionStream, buf.data(), buf.size()) != sz)
        return false;
    std::memcpy(&out, buf.data(), sizeof(out));
    return true;
}

// The faulting exception code recorded in the dump, or 0 if the ExceptionStream is absent.
inline std::uint32_t exception_code(const std::wstring& dump_path)
{
    MINIDUMP_EXCEPTION_STREAM es{};
    if (!read_exception_stream(dump_path, es))
        return 0;
    return static_cast<std::uint32_t>(es.ExceptionRecord.ExceptionCode);
}

// The thread id recorded in the dump's ExceptionStream (the faulting thread, not the writer), or 0 if absent.
inline std::uint32_t exception_thread_id(const std::wstring& dump_path)
{
    MINIDUMP_EXCEPTION_STREAM es{};
    if (!read_exception_stream(dump_path, es))
        return 0;
    return static_cast<std::uint32_t>(es.ThreadId);
}

inline std::wstring to_lower(std::wstring s)
{
    for (wchar_t& c : s)
        c = static_cast<wchar_t>(::towlower(static_cast<wint_t>(c)));
    return s;
}

// True iff the dump's module list names a module whose path ends with want_basename (case-insensitive) AND that
// module carries a CV record in RSDS format (>= 24 bytes: 'RSDS' magic + a 16-byte GUID + a 4-byte age). That is the
// binary identity a symbol server keys on -- present without any frame being symbolized here.
inline bool names_module_with_cv(const std::wstring& dump_path, const std::wstring& want_basename)
{
    const std::wstring               want = to_lower(want_basename);
    const std::vector<unsigned char> file = read_file_bytes(dump_path);
    if (file.size() < sizeof(std::uint32_t))
        return false;

    constexpr std::uint32_t kModuleListStream = 4U; // MINIDUMP_STREAM_TYPE::ModuleListStream
    const std::size_t       list_size = crd::crash::read_dump_stream(dump_path.c_str(), kModuleListStream, nullptr, 0);
    if (list_size < sizeof(std::uint32_t))
        return false;
    std::vector<unsigned char> list(list_size);
    if (crd::crash::read_dump_stream(dump_path.c_str(), kModuleListStream, list.data(), list.size()) != list_size)
        return false;

    std::uint32_t count = 0;
    std::memcpy(&count, list.data(), sizeof(count));
    for (std::uint32_t i = 0; i < count; ++i)
    {
        const std::size_t off = sizeof(std::uint32_t) + static_cast<std::size_t>(i) * sizeof(MINIDUMP_MODULE);
        if (off + sizeof(MINIDUMP_MODULE) > list.size())
            break;
        MINIDUMP_MODULE mod{};
        std::memcpy(&mod, list.data() + off, sizeof(mod));

        // The module name lives at ModuleNameRva as a MINIDUMP_STRING { u32 Length(bytes); wchar_t Buffer[] } in the
        // FILE (not the stream), so index the raw bytes.
        const std::size_t nrva = static_cast<std::size_t>(mod.ModuleNameRva);
        if (nrva + sizeof(std::uint32_t) > file.size())
            continue;
        std::uint32_t nlen_bytes = 0;
        std::memcpy(&nlen_bytes, file.data() + nrva, sizeof(nlen_bytes));
        const std::size_t chars = nlen_bytes / sizeof(wchar_t);
        const std::size_t soff  = nrva + sizeof(std::uint32_t);
        if (soff + static_cast<std::size_t>(chars) * sizeof(wchar_t) > file.size())
            continue;
        std::wstring name(chars, L'\0');
        std::memcpy(name.data(), file.data() + soff, chars * sizeof(wchar_t));
        const std::wstring name_l = to_lower(name);
        if (name_l.size() < want.size() || name_l.compare(name_l.size() - want.size(), want.size(), want) != 0)
            continue;

        // The named module: require an RSDS CV record -- the identity later symbolization matches or refuses.
        if (mod.CvRecord.DataSize < 24U)
            return false;
        const std::size_t crva = static_cast<std::size_t>(mod.CvRecord.Rva);
        if (crva + 4U > file.size())
            return false;
        const unsigned char* cv = file.data() + crva;
        return cv[0] == 'R' && cv[1] == 'S' && cv[2] == 'D' && cv[3] == 'S';
    }
    return false;
}
} // namespace crd_test_minidump

#endif // defined(_WIN32)
