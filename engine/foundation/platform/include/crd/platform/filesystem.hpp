#pragma once

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/containers/string.hpp>

namespace crd::platform::fs
{
// Path - UTF-8 engine-facing path wrapper.
//
// Internally we store forward slashes (`/`) in `m_generic`, regardless of
// host OS. This keeps logs, serialization, and string comparisons stable.
// OS-native conversions happen at the platform boundary only.
class Path
{
public:
    Path() noexcept = default;
    explicit Path(containers::String s);
    explicit Path(containers::StringView sv);
    explicit Path(const char* cstr);

    [[nodiscard]] bool empty() const noexcept { return m_generic.empty(); }
    [[nodiscard]] containers::StringView generic() const noexcept
    {
        return containers::StringView{m_generic.data(), m_generic.size()};
    }
    [[nodiscard]] containers::String native() const;

    [[nodiscard]] Path operator/(containers::StringView segment) const;

    friend bool operator==(const Path& a, const Path& b) noexcept { return a.m_generic == b.m_generic; }

private:
    static void normalize_in_place(containers::String& s);

    containers::String m_generic{};
};

[[nodiscard]] Path current_working_dir() noexcept;
[[nodiscard]] Path executable_dir() noexcept;
[[nodiscard]] Path user_config_dir(containers::StringView app_name) noexcept;
// The OS scratch directory. ⛔ Anything a test or tool writes "just for now" goes HERE, never to a relative
// path — a relative path resolves to wherever the process was LAUNCHED from, which for ctest on a dev box is
// the repo checkout, and the checkout grows artifact crops that .gitignore then chases pattern by pattern.
[[nodiscard]] Path temp_directory() noexcept;

[[nodiscard]] bool exists(const Path& path) noexcept;
[[nodiscard]] bool is_file(const Path& path) noexcept;
[[nodiscard]] bool is_directory(const Path& path) noexcept;
[[nodiscard]] u64 file_size(const Path& path) noexcept;
// The last write time since the Unix epoch, converted exactly from the file system's clock (0 when unreadable). The
// nanosecond form keeps the file system's own resolution (100 ns on NTFS, 1 ns on ext4), so files written within one
// second still order by age; a time outside its range (1677 to 2262) saturates.
[[nodiscard]] i64 last_modified_unix_seconds(const Path& path) noexcept;
[[nodiscard]] i64 last_modified_unix_ns(const Path& path) noexcept;

[[nodiscard]] bool read_file_text(const Path& path, containers::String& out) noexcept;
[[nodiscard]] bool read_file_binary(const Path& path, containers::Array<u8>& out) noexcept;
[[nodiscard]] bool read_file_range(const Path& path, crd::u64 offset, crd::u64 size, containers::Array<u8>& out) noexcept;
[[nodiscard]] bool write_file_text(const Path& path, containers::StringView contents) noexcept;
[[nodiscard]] bool write_file_binary(const Path& path, containers::ConstSpan<u8> contents) noexcept;

[[nodiscard]] bool create_directories(const Path& path) noexcept;
[[nodiscard]] bool remove_file(const Path& path) noexcept;
// Rename/move `from` to `to`, REPLACING an existing destination (POSIX rename semantics on all platforms).
// Same-volume renames are atomic — the write-temp → rename publish primitive.
[[nodiscard]] bool rename_file(const Path& from, const Path& to) noexcept;
[[nodiscard]] bool remove_all(const Path& path) noexcept;
void list_directory(const Path& path, containers::Array<Path>& out) noexcept;

// Create `path` (and its missing parents) as a directory only its owner can enter: mode 0700 on POSIX, a protected
// DACL with one inheritable full-control entry for the calling user on Windows, which every file created in it and
// every file it already holds inherit. An existing directory is restricted the same way. For secrets at rest (the key
// store of crd-crypto). False when the directory cannot be created or restricted (another owner's directory).
[[nodiscard]] bool create_private_directory(const Path& path) noexcept;

// An exclusive lock on the file `path` (created when missing, never removed), held from acquire() to release() or
// destruction: between processes, and between FileLock objects of one process (LockFileEx on Windows, flock on
// POSIX, each per open handle). Advisory: it orders the writers that take it, and nothing else. acquire() blocks until
// the lock is free; false when the file cannot be opened or locked.
class FileLock
{
public:
    FileLock() noexcept = default;
    ~FileLock() noexcept { release(); }
    FileLock(const FileLock&)            = delete;
    FileLock& operator=(const FileLock&) = delete;
    FileLock(FileLock&&)                 = delete;
    FileLock& operator=(FileLock&&)      = delete;

    [[nodiscard]] bool acquire(const Path& path) noexcept;
    void               release() noexcept;
    [[nodiscard]] bool held() const noexcept { return m_handle != nullptr || m_fd >= 0; }

private:
    void* m_handle = nullptr; // the file's HANDLE on Windows
    int   m_fd     = -1;      // its descriptor on POSIX
};
} // namespace crd::platform::fs
