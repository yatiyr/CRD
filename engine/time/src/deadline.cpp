// crd-time -- sleep / yield implementations (Detour D-006).

#include <crd/time/deadline.hpp>

#include <chrono>
#include <thread>

namespace crd::time
{

void sleep_for(Duration duration) noexcept
{
    if (duration.value <= 0.0)
    {
        return;
    }
    const auto ns = static_cast<long long>(duration.value * 1.0e9);
    std::this_thread::sleep_for(std::chrono::nanoseconds{ns});
}

void sleep_until(Deadline deadline) noexcept
{
    const Duration remaining = deadline.remaining();
    if (remaining.value > 0.0)
    {
        sleep_for(remaining);
    }
}

void yield_thread() noexcept
{
    std::this_thread::yield();
}

} // namespace crd::time
