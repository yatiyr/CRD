#pragma once

#include <crd/core/types.hpp>
#include <crd/platform/filesystem.hpp>

#include <functional>
#include <vector>

namespace crd::platform
{

/// Polling-based file change detector.
///
/// Watches a set of paths and fires a callback when any watched file's
/// last-modified timestamp changes. Changes are detected by calling poll()
/// — typically once per frame. Callbacks are invoked synchronously inside
/// poll().
///
/// Implemented as OS-agnostic mtime polling via fs::last_modified_unix_seconds().
/// For low-frequency hot-reload scenarios this is sufficient; inotify / FSEvents
/// style event-driven watching is out of scope.
class FileWatcher
{
public:
    /// Callback receives the path of the changed file.
    using Callback = std::function<void(const fs::Path& path)>;

    FileWatcher()                              = default;
    ~FileWatcher()                             = default;
    FileWatcher(FileWatcher&&)                 = default;
    FileWatcher& operator=(FileWatcher&&)      = default;
    FileWatcher(const FileWatcher&)            = delete;
    FileWatcher& operator=(const FileWatcher&) = delete;

    /// Register `path` for watching. `callback` fires every time the file's
    /// mtime changes. Returns an opaque handle for use with remove().
    [[nodiscard]] u64 add(fs::Path path, Callback callback);

    /// Unregister the entry identified by `handle`. No-op if not found.
    void remove(u64 handle) noexcept;

    /// Check all watched paths and fire callbacks for any whose mtime has
    /// changed since the last poll(). Call once per frame.
    void poll();

    /// Number of currently watched paths.
    [[nodiscard]] usize count() const noexcept { return m_entries.size(); }

private:
    struct Entry
    {
        u64      handle;
        fs::Path path;
        Callback callback;
        i64      last_mtime;
    };

    u64                m_next_handle = 1U;
    std::vector<Entry> m_entries;
};

} // namespace crd::platform
