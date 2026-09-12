// ---------------------------------------------------------------------------
// crd-jobs -- JobObserver registration (added for D-003 v0c).
//
// Single-subscriber, plain function-pointer-table observer. Designed so the
// scheduler's hot path is one null-check + one indirect call when set, and
// one null-check + branch-not-taken when clear.
//
// The observer pointer is `std::atomic<const JobObserver*>` so set / clear
// is well-defined w.r.t. concurrent reads from worker threads. Acquire on
// read, release on write.
// ---------------------------------------------------------------------------

#include <crd/jobs/observer.hpp>

#include <atomic>

namespace crd::jobs
{
namespace
{

std::atomic<const JobObserver*> g_observer{nullptr};

} // namespace

void set_observer(const JobObserver* observer) noexcept
{
    g_observer.store(observer, std::memory_order_release);
}

[[nodiscard]] const JobObserver* current_observer() noexcept
{
    return g_observer.load(std::memory_order_acquire);
}

} // namespace crd::jobs
