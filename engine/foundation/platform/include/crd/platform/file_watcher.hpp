#pragma once

#include <crd/core/types.hpp>
#include <crd/platform/filesystem.hpp>

#include <functional>
#include <crd/containers/array.hpp>

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
///
/// Each watched path's callback is a callback registration in the lifetime ledger
/// (crd/core/lifetime_ledger.hpp, DIAG.10c): add() records it, remove() and the
/// watcher's destruction release it.
class FileWatcher
{
public:
    /// Callback receives the path of the changed file.
    using Callback = std::function<void(const fs::Path& path)>;

    FileWatcher()                              = default;
    ~FileWatcher();
    FileWatcher(FileWatcher&& other) noexcept;
    FileWatcher& operator=(FileWatcher&& other) noexcept;
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
    void release_all() noexcept;

    struct Entry
    {
        u64      handle;
        fs::Path path;
        Callback callback;
        i64      last_mtime;
    };

    u64                m_next_handle = 1U;
    crd::containers::Array<Entry> m_entries;
};

} // namespace crd::platform
