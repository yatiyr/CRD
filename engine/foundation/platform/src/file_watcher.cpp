#include <crd/platform/file_watcher.hpp>

#include <algorithm>

namespace crd::platform
{

u64 FileWatcher::add(fs::Path path, Callback callback)
{
    const u64 handle    = m_next_handle++;
    const i64 init_mtime = fs::last_modified_unix_seconds(path);
    m_entries.push_back(Entry{handle, std::move(path), std::move(callback), init_mtime});
    return handle;
}

void FileWatcher::remove(u64 handle) noexcept
{
    auto it = std::find_if(m_entries.begin(), m_entries.end(),
                           [handle](const Entry& e) { return e.handle == handle; });
    if (it != m_entries.end())
    {
        m_entries.erase(it);
    }
}

void FileWatcher::poll()
{
    for (Entry& e : m_entries)
    {
        const i64 current_mtime = fs::last_modified_unix_seconds(e.path);
        if (current_mtime != e.last_mtime)
        {
            e.last_mtime = current_mtime;
            e.callback(e.path);
        }
    }
}

} // namespace crd::platform
