#include <crd/log/log_channel.hpp>

#include <atomic>
#include <cstring>

namespace crd::log
{
namespace
{
// Function-local static head pointer. Lives in the data section, no SIOF risk.
// Static-init-order safe because static-storage POD is zero-initialised before
// any of our registrars run (zero-init phase precedes dynamic-init phase).
std::atomic<Channel*>& head() noexcept
{
    static std::atomic<Channel*> s_head{nullptr};
    return s_head;
}
} // namespace

namespace detail
{
void register_channel(Channel* ch) noexcept
{
    if (!ch || ch->next != nullptr)
    {
        return; // already linked or null
    }
    // Lock-free CAS-style intrusive list push.
    Channel* expected = head().load(std::memory_order_relaxed);
    do
    {
        ch->next = expected;
    } while (!head().compare_exchange_weak(expected, ch, std::memory_order_release, std::memory_order_relaxed));
}
} // namespace detail

Channel* find_channel(const char* name) noexcept
{
    if (!name)
    {
        return nullptr;
    }
    for (Channel* ch = head().load(std::memory_order_acquire); ch != nullptr; ch = ch->next)
    {
        if (ch->name && std::strcmp(ch->name, name) == 0)
        {
            return ch;
        }
    }
    return nullptr;
}

bool set_channel_level(const char* name, LogLevel level) noexcept
{
    Channel* ch = find_channel(name);
    if (!ch)
    {
        return false;
    }
    ch->runtime_level = level;
    return true;
}

void set_all_channels_level(LogLevel level) noexcept
{
    for (Channel* ch = head().load(std::memory_order_acquire); ch != nullptr; ch = ch->next)
    {
        ch->runtime_level = level;
    }
}

Channel* first_channel() noexcept
{
    return head().load(std::memory_order_acquire);
}

usize channel_count() noexcept
{
    usize n = 0;
    for (Channel* ch = head().load(std::memory_order_acquire); ch != nullptr; ch = ch->next)
    {
        ++n;
    }
    return n;
}
} // namespace crd::log
