#include <crd/log/log.hpp>
#include <crd/platform/filesystem.hpp>
#include <crd/platform/log_channel.hpp>

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#if CRD_OS_WINDOWS
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
[[nodiscard]] std::wstring utf8_to_wide(containers::StringView sv) noexcept
{
    try
    {
        if (sv.empty())
        {
            return {};
        }
        const int needed = MultiByteToWideChar(CP_UTF8, 0, sv.data(), static_cast<int>(sv.size()), nullptr, 0);
        if (needed <= 0)
        {
            return {};
        }
        std::wstring out(static_cast<crd::usize>(needed), L'\0');
        const int written =
            MultiByteToWideChar(CP_UTF8, 0, sv.data(), static_cast<int>(sv.size()), out.data(), needed);
        if (written != needed)
        {
            return {};
        }
        return out;
    }
    catch (...)
    {
        return {}; // allocation failure under noexcept: an empty path, never std::terminate
    }
}

[[nodiscard]] containers::String wide_to_utf8(const std::wstring& ws) noexcept
{
    if (ws.empty())
    {
        return containers::String{};
    }
    const int needed =
        WideCharToMultiByte(CP_UTF8, 0, ws.data(), static_cast<int>(ws.size()), nullptr, 0, nullptr, nullptr);
    if (needed <= 0)
    {
        return containers::String{};
    }
    containers::String out;
    out.resize(static_cast<crd::usize>(needed));
    const int written =
        WideCharToMultiByte(CP_UTF8, 0, ws.data(), static_cast<int>(ws.size()), out.data(), needed, nullptr, nullptr);
    if (written != needed)
    {
        return containers::String{};
    }
    return out;
}

[[nodiscard]] stdfs::path to_native_path(const Path& path)
{
    return stdfs::path(utf8_to_wide(path.generic()));
}

[[nodiscard]] Path from_native_path(const stdfs::path& path)
{
    return Path{wide_to_utf8(path.wstring())};
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
    std::wstring buffer(512, L'\0');
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
            buffer.resize(written);
            return from_native_path(stdfs::path(buffer).parent_path());
        }
        buffer.resize(buffer.size() * 2);
    }
#elif CRD_OS_MAC
    // _NSGetExecutablePath needs a buffer big enough for the path; it tells us
    // the required size via the in/out length on the first (failing) call.
    std::uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::string buffer(size, '\0');
    if (_NSGetExecutablePath(buffer.data(), &size) != 0)
    {
        CRD_LOG_ERROR(g_log_platform, "_NSGetExecutablePath failed");
        return current_working_dir();
    }
    std::error_code ec;
    const stdfs::path resolved = stdfs::canonical(stdfs::path(buffer.c_str()), ec);
    return from_native_path((ec ? stdfs::path(buffer.c_str()) : resolved).parent_path());
#else // Linux / other POSIX with /proc
    std::string buffer(1024, '\0');
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
            return from_native_path(stdfs::path(buffer).parent_path());
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
#if CRD_OS_WINDOWS
    char* raw = nullptr;
    std::size_t len = 0;
    const errno_t rc = _dupenv_s(&raw, &len, "APPDATA");
    if (rc != 0 || raw == nullptr)
    {
        return current_working_dir() / app_name;
    }
    containers::String base(raw);
    free(raw);
    return Path{std::move(base)} / app_name;
#elif CRD_OS_MAC
    const char* home = std::getenv("HOME");
    if (home == nullptr)
    {
        return current_working_dir() / app_name;
    }
    return Path(home) / "Library" / "Application Support" / app_name;
#else
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    if (xdg != nullptr && xdg[0] != '\0')
    {
        return Path(xdg) / app_name;
    }
    const char* home = std::getenv("HOME");
    if (home == nullptr)
    {
        return current_working_dir() / app_name;
    }
    return Path(home) / ".config" / app_name;
#endif
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

i64 last_modified_unix_seconds(const Path& path) noexcept
{
    std::error_code ec;
    const auto tp = stdfs::last_write_time(to_native_path(path), ec);
    if (ec)
    {
        return 0;
    }
    const auto sys = std::chrono::time_point_cast<std::chrono::seconds>(tp - decltype(tp)::clock::now() +
                                                                        std::chrono::system_clock::now());
    return static_cast<i64>(sys.time_since_epoch().count());
}

bool read_file_text(const Path& path, containers::String& out) noexcept
{
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
            in.read(reinterpret_cast<char*>(out.data()), size);
            if (!in)
            {
                out.clear();
                return false;
            }
        }
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
        out.resize(static_cast<crd::usize>(size));
        if (size > 0)
        {
            in.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(size));
            if (!in)
            {
                out.clear();
                return false;
            }
        }
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
} // namespace crd::platform::fs
