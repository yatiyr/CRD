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

#include <crd/containers/array.hpp>
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
#include <cwchar>
#include <cwctype>
#include <fstream>

namespace crd_test_minidump
{
// Paths are native wide strings (std::filesystem::path::c_str() on Windows); bytes live in a Cerid Array.
using Bytes = crd::containers::Array<unsigned char>;

inline Bytes read_file_bytes(const wchar_t* path)
{
    Bytes         bytes;
    std::ifstream f{path, std::ios::binary | std::ios::ate};
    if (!f)
        return bytes;
    const std::streamoff size = f.tellg();
    if (size <= 0)
        return bytes;
    bytes.resize(static_cast<std::size_t>(size));
    f.seekg(0);
    if (!f.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size)))
        bytes.clear();
    return bytes;
}

// Read the whole ExceptionStream (type 6) of the dump into a struct; false if it is absent or short.
inline bool read_exception_stream(const wchar_t* dump_path, MINIDUMP_EXCEPTION_STREAM& out)
{
    constexpr std::uint32_t exception_stream = 6U; // MINIDUMP_STREAM_TYPE::ExceptionStream
    const std::size_t       sz               = crd::crash::read_dump_stream(dump_path, exception_stream, nullptr, 0);
    if (sz < sizeof(MINIDUMP_EXCEPTION_STREAM))
        return false;
    Bytes buf;
    buf.resize(sz);
    if (crd::crash::read_dump_stream(dump_path, exception_stream, buf.data(), buf.size()) != sz)
        return false;
    std::memcpy(&out, buf.data(), sizeof(out));
    return true;
}

// The faulting exception code recorded in the dump, or 0 if the ExceptionStream is absent.
inline std::uint32_t exception_code(const wchar_t* dump_path)
{
    MINIDUMP_EXCEPTION_STREAM es{};
    if (!read_exception_stream(dump_path, es))
        return 0;
    return static_cast<std::uint32_t>(es.ExceptionRecord.ExceptionCode);
}

// The thread id recorded in the dump's ExceptionStream (the faulting thread, not the writer), or 0 if absent.
inline std::uint32_t exception_thread_id(const wchar_t* dump_path)
{
    MINIDUMP_EXCEPTION_STREAM es{};
    if (!read_exception_stream(dump_path, es))
        return 0;
    return static_cast<std::uint32_t>(es.ThreadId);
}

// Case-insensitive test that the `n` wide characters at `name` end with the NUL-terminated `suffix`.
inline bool ends_with_icase(const wchar_t* name, std::size_t n, const wchar_t* suffix)
{
    const std::size_t m = std::wcslen(suffix);
    if (n < m)
        return false;
    for (std::size_t i = 0; i < m; ++i)
    {
        if (::towlower(static_cast<wint_t>(name[n - m + i])) != ::towlower(static_cast<wint_t>(suffix[i])))
            return false;
    }
    return true;
}

// True iff the dump's module list names a module whose path ends with want_basename (case-insensitive) AND that
// module carries a CV record in RSDS format (>= 24 bytes: 'RSDS' magic + a 16-byte GUID + a 4-byte age). That is the
// binary identity a symbol server keys on -- present without any frame being symbolized here.
inline bool names_module_with_cv(const wchar_t* dump_path, const wchar_t* want_basename)
{
    const Bytes file = read_file_bytes(dump_path);
    if (file.size() < sizeof(std::uint32_t))
        return false;

    constexpr std::uint32_t module_list_stream = 4U; // MINIDUMP_STREAM_TYPE::ModuleListStream
    const std::size_t       list_size = crd::crash::read_dump_stream(dump_path, module_list_stream, nullptr, 0);
    if (list_size < sizeof(std::uint32_t))
        return false;
    Bytes list;
    list.resize(list_size);
    if (crd::crash::read_dump_stream(dump_path, module_list_stream, list.data(), list.size()) != list_size)
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
        // FILE (not the stream), so index the raw bytes. The buffer is not guaranteed wchar_t-aligned there, so the
        // characters are copied out before they are compared.
        const std::size_t nrva = static_cast<std::size_t>(mod.ModuleNameRva);
        if (nrva + sizeof(std::uint32_t) > file.size())
            continue;
        std::uint32_t nlen_bytes = 0;
        std::memcpy(&nlen_bytes, file.data() + nrva, sizeof(nlen_bytes));
        const std::size_t chars = nlen_bytes / sizeof(wchar_t);
        const std::size_t soff  = nrva + sizeof(std::uint32_t);
        if (soff + static_cast<std::size_t>(chars) * sizeof(wchar_t) > file.size())
            continue;
        crd::containers::Array<wchar_t> name;
        name.resize(chars);
        std::memcpy(name.data(), file.data() + soff, chars * sizeof(wchar_t));
        if (!ends_with_icase(name.data(), chars, want_basename))
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
