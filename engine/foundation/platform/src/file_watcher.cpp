#include <crd/platform/file_watcher.hpp>

#include <crd/core/lifetime_ledger.hpp>

#include <algorithm>

namespace crd::platform
{

FileWatcher::~FileWatcher()
{
    release_all();
}

FileWatcher::FileWatcher(FileWatcher&& other) noexcept
    : m_next_handle(other.m_next_handle)
    , m_entries(std::move(other.m_entries))
{
}

FileWatcher& FileWatcher::operator=(FileWatcher&& other) noexcept
{
    if (this != &other)
    {
        release_all(); // the callbacks this watcher held end here
        m_next_handle = other.m_next_handle;
        m_entries     = std::move(other.m_entries);
    }
    return *this;
}

void FileWatcher::release_all() noexcept
{
    for (crd::usize i = 0; i < m_entries.size(); ++i)
    {
        lifetime_ledger().released(LifetimeClass::CallbackRegistration);
    }
    m_entries.clear();
}

u64 FileWatcher::add(fs::Path path, Callback callback)
{
    const u64 handle    = m_next_handle++;
    const i64 init_mtime = fs::last_modified_unix_seconds(path);
    m_entries.push_back(Entry{handle, std::move(path), std::move(callback), init_mtime});
    lifetime_ledger().acquired(LifetimeClass::CallbackRegistration);
    return handle;
}

void FileWatcher::remove(u64 handle) noexcept
{
    auto it = std::find_if(m_entries.begin(), m_entries.end(),
                           [handle](const Entry& e) { return e.handle == handle; });
    if (it != m_entries.end())
    {
        m_entries.erase(static_cast<crd::usize>(it - m_entries.begin()));
        lifetime_ledger().released(LifetimeClass::CallbackRegistration);
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
