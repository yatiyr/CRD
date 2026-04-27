#include <crd/platform/threading.hpp>

#include <thread>

namespace crd::platform::threading
{
void set_current_thread_name(containers::StringView /*name*/) noexcept
{
    // v1d baseline: naming is a best-effort no-op.
    //
    // The API lands now so callers can standardise on it before the job
    // system arrives. Platform-specific debugger/profiler integration can
    // replace this implementation later without changing user code.
}

u32 current_thread_id() noexcept
{
    return static_cast<u32>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
}

u32 hardware_concurrency() noexcept
{
    const unsigned int n = std::thread::hardware_concurrency();
    return n == 0 ? 1u : static_cast<u32>(n);
}

u32 logical_core_count() noexcept
{
    return hardware_concurrency();
}

u32 physical_core_count() noexcept
{
    // Baseline heuristic until platform-specific topology queries are worth
    // the SDK complexity. Better to return a conservative usable value than
    // ship brittle OS-specific code before the job system exists.
    return hardware_concurrency();
}

bool set_thread_affinity(u32 /*core_index*/) noexcept
{
    // Baseline stub. Affinity becomes meaningful when the job system and
    // worker-thread ownership model land in Phase 2.3.
    return false;
}

void cpu_pause() noexcept
{
    std::this_thread::yield();
}
} // namespace crd::platform::threading
