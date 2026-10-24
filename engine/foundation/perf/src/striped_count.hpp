#pragma once
// crd-perf -- internal: a relaxed tally that many threads add to on a hot path, shared by the flight recorder's
// left-out events and the jobs adapter's job tallies (DIAG.11b). One counter is a line every thread writes at every
// add; here each thread always adds to the same one of kStripes cache lines and total() sums them (exact once the
// adders are quiescent, a lower bound while they run).

#include <crd/containers/atomic_array.hpp>
#include <crd/containers/static_array.hpp>
#include <crd/core/types.hpp>

#include <atomic>

namespace crd::perf::detail
{

class StripedCount
{
public:
    void add() noexcept { m_stripes[stripe()].value.fetch_add(1U, std::memory_order_relaxed); }

    [[nodiscard]] crd::u64 total() const noexcept
    {
        crd::u64 sum = 0U;
        for (const auto& s : m_stripes)
        {
            sum += s.value.load(std::memory_order_relaxed);
        }
        return sum;
    }

    void reset() noexcept
    {
        for (auto& s : m_stripes)
        {
            s.value.store(0U, std::memory_order_relaxed);
        }
    }

private:
    static constexpr crd::u32 kStripes = 16U;

    [[nodiscard]] static crd::u32 stripe() noexcept
    {
        static std::atomic<crd::u32> next{0U};
        thread_local const crd::u32 kStripe = next.fetch_add(1U, std::memory_order_relaxed) % kStripes;
        return kStripe;
    }

    crd::containers::StaticArray<crd::containers::CacheLinePadded<std::atomic<crd::u64>>, kStripes> m_stripes{};
};

} // namespace crd::perf::detail
