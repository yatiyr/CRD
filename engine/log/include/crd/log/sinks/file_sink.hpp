#pragma once

#include <crd/core/types.hpp>
#include <crd/log/log_sink.hpp>

#include <cstdio>
#include <mutex>
#include <string>

namespace crd::log
{
// Writes formatted records to a file. Supports size-based rotation:
// when the current file would exceed max_bytes, it is closed and renamed
// (game.log -> game.1.log, etc., up to max_files), then a fresh game.log
// is opened.
//
// Rotation policy:
//   max_bytes  : 10 MB by default; set to 0 to disable rotation entirely.
//   max_files  : keep N rotated files; the oldest is unlinked.
class FileSink : public ISink
{
public:
    FileSink(std::string path, u64 max_bytes = 10ull * 1024ull * 1024ull, u32 max_files = 5) noexcept;
    ~FileSink() override;

    FileSink(const FileSink&) = delete;
    FileSink& operator=(const FileSink&) = delete;

    void write(const LogRecord& rec) override;
    void flush() override;

    bool is_open() const noexcept { return m_file != nullptr; }
    const std::string& path() const noexcept { return m_path; }

private:
    void open_file() noexcept;
    void close_file() noexcept;
    void rotate_if_needed(u64 next_write_size) noexcept;

    std::string m_path;
    u64 m_max_bytes;
    u32 m_max_files;
    std::FILE* m_file = nullptr;
    u64 m_bytes_written = 0;
    std::mutex m_mutex;
};
} // namespace crd::log
