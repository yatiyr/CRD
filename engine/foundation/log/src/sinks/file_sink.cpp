#include "../log_formatter.hpp"

#include <crd/containers/string.hpp>
#include <crd/log/sinks/file_sink.hpp>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <format>
#include <string_view>
#include <iterator>
#include <utility>

namespace crd::log
{
namespace
{
// Path of "name.N.ext" given the base path "name.ext".
// We treat the substring after the final dot as the extension; if there's
// no dot, we just append ".N".
crd::containers::String rotated_path(const crd::containers::String& base, u32 idx)
{
    const std::string_view b{base};
    const usize dot = b.find_last_of('.');
    crd::containers::String out;
    if (dot == std::string_view::npos)
    {
        std::format_to(std::back_inserter(out), "{}.{}", b, idx);
    }
    else
    {
        std::format_to(std::back_inserter(out), "{}.{}{}", b.substr(0, dot), idx, b.substr(dot));
    }
    return out;
}
} // namespace

FileSink::FileSink(std::string_view path, u64 max_bytes, u32 max_files) noexcept
    : m_path(crd::containers::String{path}), m_max_bytes(max_bytes), m_max_files(max_files == 0 ? 1 : max_files)
{
    open_file();
}

FileSink::~FileSink()
{
    close_file();
}

void FileSink::open_file() noexcept
{
#if defined(_WIN32)
    if (::fopen_s(&m_file, m_path.c_str(), "ab") != 0)
    {
        m_file = nullptr;
    }
#else
    m_file = std::fopen(m_path.c_str(), "ab");
#endif
    if (m_file)
    {
        std::fseek(m_file, 0, SEEK_END);
        const long pos = std::ftell(m_file);
        m_bytes_written = pos > 0 ? static_cast<u64>(pos) : 0;
    }
}

void FileSink::close_file() noexcept
{
    if (m_file)
    {
        std::fclose(m_file);
        m_file = nullptr;
    }
}

void FileSink::rotate_if_needed(u64 next_write_size) noexcept
{
    if (m_max_bytes == 0)
    {
        return; // rotation disabled
    }
    if (m_bytes_written + next_write_size <= m_max_bytes)
    {
        return;
    }

    close_file();

    // Every `rotated_path` below builds a crd::String, so rotation can throw bad_alloc — and this is
    // `noexcept`, which would make that std::terminate. A rotation we cannot perform must degrade to
    // "keep writing to the current file", never to a crash. (bugprone-exception-escape; the check only
    // became visible once the tidy gate stopped dropping `/EHsc` — see CMakeLists.)
    try
    {
        // Shift: oldest gets removed, others shift up by one.
        // file.log -> file.1.log -> file.2.log -> ...
        const crd::containers::String oldest = rotated_path(m_path, m_max_files);
        std::remove(oldest.c_str()); // ignore failure (may not exist)

        for (u32 i = m_max_files; i > 1; --i)
        {
            const crd::containers::String src = rotated_path(m_path, i - 1);
            const crd::containers::String dst = rotated_path(m_path, i);
            std::remove(dst.c_str());
            std::rename(src.c_str(), dst.c_str()); // ok if src missing
        }

        const crd::containers::String first_rot = rotated_path(m_path, 1);
        std::remove(first_rot.c_str());
        std::rename(m_path.c_str(), first_rot.c_str());
    }
    catch (...)
    {
        // Rotation is best-effort; the log STREAM is not. Reopen and keep writing to the current file
        // rather than terminating -- the size cap is the only thing lost.
        m_bytes_written = 0;
        open_file();
        return;
    }

    m_bytes_written = 0;
    open_file();
}

void FileSink::write(const LogRecord& rec)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_file)
    {
        return;
    }
    crd::containers::String line = detail::format_record(rec, /*color*/ false, /*short*/ true);
    line.push_back('\n');

    rotate_if_needed(line.size());
    if (!m_file)
    {
        return;
    }

    const usize n = std::fwrite(line.data(), 1, line.size(), m_file);
    m_bytes_written += n;
}

void FileSink::flush()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_file)
    {
        std::fflush(m_file);
    }
}
} // namespace crd::log
