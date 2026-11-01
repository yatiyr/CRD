#include "filesystem_detail.hpp"
#include "wide_text.hpp"

#include <crd/core/environment.hpp>
#include <crd/core/fault_seam.hpp>
#include <crd/log/log.hpp>
#include <crd/platform/filesystem.hpp>
#include <crd/platform/log_channel.hpp>

#include <chrono>
#include <thread>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <crd/containers/array.hpp>
#include <crd/containers/string.hpp>

#include <string_view>

#if CRD_OS_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif CRD_OS_MAC
#include <mach-o/dyld.h>
#else // Linux / other POSIX
#include <unistd.h>
#endif

namespace crd::platform::fs
{
namespace
{
namespace stdfs = std::filesystem;

#if CRD_OS_WINDOWS
[[nodiscard]] stdfs::path to_native_path(const Path& path)
{
    return stdfs::path(wide_text::utf8_to_wide(path.generic()).data());
}

[[nodiscard]] Path from_native_path(const stdfs::path& path)
{
    return Path{wide_text::wide_to_utf8(path.wstring())};
}
#else
[[nodiscard]] stdfs::path to_native_path(const Path& path)
{
    return stdfs::path(path.generic());
}

[[nodiscard]] Path from_native_path(const stdfs::path& path)
{
    return Path{containers::String(path.generic_string())};
}
#endif

} // namespace

Path::Path(containers::String s) : m_generic(std::move(s))
{
    normalize_in_place(m_generic);
}

Path::Path(containers::StringView sv) : m_generic(sv)
{
    normalize_in_place(m_generic);
}

Path::Path(const char* cstr) : m_generic(cstr)
{
    normalize_in_place(m_generic);
}

containers::String Path::native() const
{
    containers::String out(m_generic);
#if CRD_OS_WINDOWS
    for (crd::usize i = 0; i < out.size(); ++i)
    {
        if (out.data()[i] == '/')
        {
            out.data()[i] = '\\';
        }
    }
#endif
    return out;
}

Path Path::operator/(containers::StringView segment) const
{
    containers::String joined(m_generic);
    crd::usize start = 0;
    while (start < segment.size() && (segment[start] == '/' || segment[start] == '\\'))
    {
        ++start;
    }

    const bool needs_separator = !joined.empty() && joined.data()[joined.size() - 1] != '/';
    joined.reserve(joined.size() + (needs_separator ? 1U : 0U) + (segment.size() - start));

    if (!joined.empty() && joined.data()[joined.size() - 1] != '/')
    {
        joined.push_back('/');
    }
    joined.append(segment.data() + start, segment.size() - start);
    return Path{std::move(joined)};
}

void Path::normalize_in_place(containers::String& s)
{
    for (crd::usize i = 0; i < s.size(); ++i)
    {
        if (s.data()[i] == '\\')
        {
            s.data()[i] = '/';
        }
    }

    while (s.size() > 1 && s.data()[s.size() - 1] == '/')
    {
        const bool is_drive_root = s.size() == 3 && s.data()[1] == ':' && s.data()[2] == '/';
        if (is_drive_root)
        {
            break;
        }
        s.pop_back();
    }
}

Path current_working_dir() noexcept
{
    try
    {
        std::error_code ec;
        const stdfs::path cwd = stdfs::current_path(ec);
        if (ec)
        {
            CRD_LOG_ERROR(g_log_platform, "current_path() failed: {}", ec.message());
            return Path{};
        }
        return from_native_path(cwd);
    }
    catch (...)
    {
        return Path{}; // allocation failure under noexcept: an empty path, never std::terminate
    }
}

Path executable_dir() noexcept
{
    try
    {
#if CRD_OS_WINDOWS
    crd::containers::Array<wchar_t> buffer;
    buffer.resize(512);
    for (;;)
    {
        const DWORD written = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (written == 0)
        {
            CRD_LOG_ERROR(g_log_platform, "GetModuleFileNameW failed");
            return Path{};
        }
        if (written < buffer.size())
        {
            return from_native_path(stdfs::path(buffer.data()).parent_path());
        }
        buffer.resize(buffer.size() * 2);
    }
#elif CRD_OS_MAC
    // _NSGetExecutablePath needs a buffer big enough for the path; it tells us
    // the required size via the in/out length on the first (failing) call.
    std::uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    crd::containers::String buffer;
    buffer.resize(size);
    if (_NSGetExecutablePath(buffer.data(), &size) != 0)
    {
        CRD_LOG_ERROR(g_log_platform, "_NSGetExecutablePath failed");
        return current_working_dir();
    }
    std::error_code ec;
    const stdfs::path resolved = stdfs::canonical(stdfs::path(buffer.c_str()), ec);
    return from_native_path((ec ? stdfs::path(buffer.c_str()) : resolved).parent_path());
#else // Linux / other POSIX with /proc
    crd::containers::String buffer;
    buffer.resize(1024);
    for (;;)
    {
        const ssize_t written = ::readlink("/proc/self/exe", buffer.data(), buffer.size());
        if (written < 0)
        {
            CRD_LOG_ERROR(g_log_platform, "readlink(/proc/self/exe) failed");
            return current_working_dir();
        }
        if (static_cast<std::size_t>(written) < buffer.size())
        {
            buffer.resize(static_cast<std::size_t>(written));
            return from_native_path(stdfs::path(buffer.c_str()).parent_path());
        }
        buffer.resize(buffer.size() * 2); // truncated — grow and retry
    }
#endif
    }
    catch (...)
    {
        return Path{}; // allocation failure under noexcept: an empty path, never std::terminate
    }
}

Path user_config_dir(containers::StringView app_name) noexcept
{
    containers::String base;
#if CRD_OS_WINDOWS
    if (!read_environment_text("APPDATA", base))
    {
        return current_working_dir() / app_name;
    }
    return Path{std::move(base)} / app_name;
#elif CRD_OS_MAC
    if (!read_environment_text("HOME", base))
    {
        return current_working_dir() / app_name;
    }
    return Path{std::move(base)} / "Library" / "Application Support" / app_name;
#else
    if (read_environment_text("XDG_CONFIG_HOME", base))
    {
        return Path{std::move(base)} / app_name;
    }
    if (!read_environment_text("HOME", base))
    {
        return current_working_dir() / app_name;
    }
    return Path{std::move(base)} / ".config" / app_name;
#endif
}

Path temp_directory() noexcept
{
    try
    {
        std::error_code ec;
        const stdfs::path t = stdfs::temp_directory_path(ec);
        if (ec)
        {
            CRD_LOG_ERROR(g_log_platform, "temp_directory_path() failed: {}", ec.message());
            return current_working_dir();
        }
        return from_native_path(t);
    }
    catch (...)
    {
        return Path{}; // allocation failure under noexcept: an empty path, never std::terminate
    }
}

bool exists(const Path& path) noexcept
{
    std::error_code ec;
    const bool ok = stdfs::exists(to_native_path(path), ec);
    return !ec && ok;
}

bool is_file(const Path& path) noexcept
{
    std::error_code ec;
    const bool ok = stdfs::is_regular_file(to_native_path(path), ec);
    return !ec && ok;
}

bool is_directory(const Path& path) noexcept
{
    std::error_code ec;
    const bool ok = stdfs::is_directory(to_native_path(path), ec);
    return !ec && ok;
}

u64 file_size(const Path& path) noexcept
{
    std::error_code ec;
    const auto size = stdfs::file_size(to_native_path(path), ec);
    return ec ? 0ULL : static_cast<u64>(size);
}

namespace
{
// The last write time on the system clock, in `Duration` units since the Unix epoch, saturated where they do not fit
// (nanoseconds span 1677 to 2262; a file clock reaches further). clock_cast is exact (through file_clock::to_sys on
// libstdc++, to_utc on the Microsoft library), where shifting by the two clocks' now() (the earlier form) moved each
// reading by the microseconds between the two calls, so one file could read as two different seconds. The Microsoft
// library's UTC step reads its leap-second table, which can allocate, so a throw reads as an unreadable time.
template <typename Duration>
[[nodiscard]] i64 last_write_since_epoch(const Path& path) noexcept
{
    try
    {
        std::error_code ec;
        const stdfs::file_time_type tp = stdfs::last_write_time(to_native_path(path), ec);
        if (ec)
        {
            return 0;
        }
        const auto written = std::chrono::clock_cast<std::chrono::system_clock>(tp);
        const auto whole = std::chrono::floor<std::chrono::seconds>(written);
        constexpr i64 per_second = Duration{std::chrono::seconds{1}}.count();
        constexpr i64 most = std::numeric_limits<i64>::max();
        constexpr i64 least = std::numeric_limits<i64>::min();
        const i64 seconds = static_cast<i64>(whole.time_since_epoch().count());
        const i64 part = static_cast<i64>(std::chrono::floor<Duration>(written - whole).count()); // [0, per_second)
        if (seconds > (most - part) / per_second)
        {
            return most;
        }
        return seconds < least / per_second ? least : seconds * per_second + part;
    }
    catch (...)
    {
        return 0;
    }
}
} // namespace

i64 last_modified_unix_seconds(const Path& path) noexcept
{
    return last_write_since_epoch<std::chrono::seconds>(path);
}

i64 last_modified_unix_ns(const Path& path) noexcept
{
    return last_write_since_epoch<std::chrono::nanoseconds>(path);
}

bool read_file_text(const Path& path, containers::String& out) noexcept
{
    // DIAG.9b fault seam: the same whole-file consult as read_file_binary, so a text read (configuration, a cook's
    // .meta sidecars and database, an authored render asset) can be failed, cut short or delayed like a binary one.
    const containers::StringView generic = path.generic();
    const IoFault fault = consult_io_fault(IoFaultOp::ReadWhole, generic.data(), generic.size(), 0U, 0U);
    if (fault.action == IoFaultAction::Fail || fault.action == IoFaultAction::Short ||
        fault.action == IoFaultAction::Cancel)
    {
        out.clear();
        return false;
    }
    if (fault.action == IoFaultAction::Delay)
    {
        detail::sleep_for_injected_delay(fault.value);
    }
    // stream/path machinery can throw (allocation, ios failure) — under noexcept that is std::terminate; an I/O
    // failure must be a FALSE return, never a process kill (same contract as the write side)
    try
    {
        std::ifstream in(to_native_path(path), std::ios::binary | std::ios::ate);
        if (!in)
        {
            return false;
        }
        const std::streamsize size = in.tellg();
        if (size < 0)
        {
            return false;
        }
        out.resize(static_cast<crd::usize>(size));
        in.seekg(0, std::ios::beg);
        if (size > 0)
        {
            in.read(out.data(), size);
            if (!in)
            {
                out.clear();
                return false;
            }
        }
        apply_io_flip(fault, reinterpret_cast<u8*>(out.data()), out.size());
        return true;
    }
    catch (...)
    {
        out.clear();
        return false;
    }
}

bool read_file_binary(const Path& path, containers::Array<u8>& out) noexcept
{
    // DIAG.9b fault seam: a failed, short or cancelled read reports false like a real one; a delay is slept here.
    const containers::StringView generic = path.generic();
    const IoFault fault = consult_io_fault(IoFaultOp::ReadWhole, generic.data(), generic.size(), 0U, 0U);
    if (fault.action == IoFaultAction::Fail || fault.action == IoFaultAction::Short ||
        fault.action == IoFaultAction::Cancel)
    {
        out.clear();
        return false;
    }
    if (fault.action == IoFaultAction::Delay)
    {
        detail::sleep_for_injected_delay(fault.value);
    }
    try
    {
        std::ifstream in(to_native_path(path), std::ios::binary | std::ios::ate);
        if (!in)
        {
            return false;
        }
        const std::streamsize size = in.tellg();
        if (size < 0)
        {
            return false;
        }
        // No zero pass: the read below fills every byte, or out is cleared.
        out.resize_uninitialized(static_cast<crd::usize>(size));
        in.seekg(0, std::ios::beg);
        if (size > 0)
        {
            in.read(reinterpret_cast<char*>(out.data()), size);
            if (!in)
            {
                out.clear();
                return false;
            }
        }
        apply_io_flip(fault, out.data(), out.size());
        return true;
    }
    catch (...)
    {
        out.clear();
        return false;
    }
}

bool read_file_range(const Path& path, crd::u64 offset, crd::u64 size, containers::Array<u8>& out) noexcept
{
    // DIAG.9b fault seam: as read_file_binary.
    const containers::StringView generic = path.generic();
    const IoFault fault = consult_io_fault(IoFaultOp::ReadRange, generic.data(), generic.size(), offset, size);
    if (fault.action == IoFaultAction::Fail || fault.action == IoFaultAction::Short ||
        fault.action == IoFaultAction::Cancel)
    {
        out.clear();
        return false;
    }
    if (fault.action == IoFaultAction::Delay)
    {
        detail::sleep_for_injected_delay(fault.value);
    }
    try
    {
        std::ifstream in(to_native_path(path), std::ios::binary);
        if (!in)
        {
            return false;
        }
        in.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
        if (!in)
        {
            return false;
        }
        out.resize_uninitialized(static_cast<crd::usize>(size)); // as read_file_binary
        if (size > 0)
        {
            in.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(size));
            if (!in)
            {
                out.clear();
                return false;
            }
        }
        apply_io_flip(fault, out.data(), out.size());
        return true;
    }
    catch (...)
    {
        out.clear();
        return false;
    }
}

bool write_file_text(const Path& path, containers::StringView contents) noexcept
{
    // the stream/path machinery can throw (allocation, ios failure) — under noexcept that is std::terminate;
    // a filesystem failure must be a FALSE return, never a process kill
    try
    {
        std::ofstream out(to_native_path(path), std::ios::binary | std::ios::trunc);
        if (!out)
        {
            return false;
        }
        out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        return static_cast<bool>(out);
    }
    catch (...)
    {
        return false;
    }
}

bool write_file_binary(const Path& path, containers::ConstSpan<u8> contents) noexcept
{
    try
    {
        std::ofstream out(to_native_path(path), std::ios::binary | std::ios::trunc);
        if (!out)
        {
            return false;
        }
        if (!contents.empty())
        {
            out.write(reinterpret_cast<const char*>(contents.data()), static_cast<std::streamsize>(contents.size()));
        }
        return static_cast<bool>(out);
    }
    catch (...)
    {
        return false;
    }
}

bool create_directories(const Path& path) noexcept
{
    try
    {
        std::error_code ec;
        const bool ok = stdfs::create_directories(to_native_path(path), ec);
        return !ec && (ok || is_directory(path));
    }
    catch (...)
    {
        return false;
    }
}

bool remove_file(const Path& path) noexcept
{
    std::error_code ec;
    const bool ok = stdfs::remove(to_native_path(path), ec);
    return !ec && ok;
}

bool rename_file(const Path& from, const Path& to) noexcept
{
    // replaces an existing destination on both platforms (POSIX rename semantics; std::filesystem guarantees the
    // overwrite) — the atomic-publish primitive the GEO-6 cook database/artifact writes lean on
    std::error_code ec;
    stdfs::rename(to_native_path(from), to_native_path(to), ec);
    return !ec;
}

bool remove_all(const Path& path) noexcept
{
    std::error_code ec;
    (void)stdfs::remove_all(to_native_path(path), ec);
    return !ec;
}

void list_directory(const Path& path, containers::Array<Path>& out) noexcept
{
    // range-for advances via the THROWING operator++ (the error_code ctor only covers construction) — a directory
    // that mutates mid-walk must yield a partial listing, never std::terminate
    try
    {
        std::error_code ec;
        const auto native = to_native_path(path);
        if (!stdfs::exists(native, ec) || ec)
        {
            return;
        }
        for (const auto& entry : stdfs::directory_iterator(native, ec))
        {
            if (ec)
            {
                return;
            }
            out.push_back(from_native_path(entry.path()));
        }
    }
    catch (...)
    {
        out.clear(); // consistent failure contract with the read_* family: empty output, never std::terminate
    }
}
namespace detail
{
RangeRead read_range_into(const Path& path, crd::u64 offset, crd::u8* dst, crd::u64 size) noexcept
{
    RangeRead result;
    try
    {
        std::ifstream in(to_native_path(path), std::ios::binary);
        if (!in)
        {
            return result;
        }
        in.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
        if (!in)
        {
            return result;
        }
        result.opened = true;
        if (size > 0U)
        {
            in.read(reinterpret_cast<char*>(dst), static_cast<std::streamsize>(size));
            const std::streamsize got = in.gcount();
            result.delivered          = got > 0 ? static_cast<crd::u64>(got) : 0U;
        }
        return result;
    }
    catch (...)
    {
        return result;
    }
}

void sleep_for_injected_delay(crd::u64 ns) noexcept
{
    std::this_thread::sleep_for(std::chrono::nanoseconds(static_cast<std::chrono::nanoseconds::rep>(ns)));
}

std::filesystem::path native_path(const Path& path)
{
    return to_native_path(path);
}
} // namespace detail

} // namespace crd::platform::fs
