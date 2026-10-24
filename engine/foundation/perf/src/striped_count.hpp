#pragma once
// crd-perf -- internal: a relaxed tally that many threads add to on a hot path, shared by the flight recorder's
// left-out events and the jobs adapter's job tallies (DIAG.11b). One counter is a line every thread writes at every
// add; here each thread always adds to the same one of kStripes cache lines and total() sums them (exact once the
// adders are quiescent, a lower bound while they run; reset() needs quiescent adders). The first kOwned threads to add
// in the process each own a stripe and add with a plain load and store: a locked add was nearly all of a left-out
// site's cost. Every later thread adds with fetch_add to one of the kShared stripes after them, which no owner writes.

#include <crd/containers/atomic_array.hpp>
#include <crd/containers/static_array.hpp>
#include <crd/core/types.hpp>

#include <atomic>

namespace crd::perf::detail
{

class StripedCount
{
public:
    void add() noexcept
    {
        const crd::u32 s = stripe();
        std::atomic<crd::u64>& value = m_stripes[s].value;
        if (s < kOwned)
        {
            value.store(value.load(std::memory_order_relaxed) + 1U, std::memory_order_relaxed);
        }
        else
        {
            value.fetch_add(1U, std::memory_order_relaxed);
        }
    }

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
    static constexpr crd::u32 kOwned = 48U; // the reference host's 32 logical processors and the host's own threads
    static constexpr crd::u32 kShared = 16U;
    static constexpr crd::u32 kStripes = kOwned + kShared;

    // A thread's stripe for the process's life. The owned count never passes kOwned, so no later thread reuses one.
    [[nodiscard]] static crd::u32 assign_stripe() noexcept
    {
        static std::atomic<crd::u32> owned{0U};
        static std::atomic<crd::u32> shared{0U};
        crd::u32 n = owned.load(std::memory_order_relaxed);
        while (n < kOwned)
        {
            if (owned.compare_exchange_weak(n, n + 1U, std::memory_order_relaxed, std::memory_order_relaxed))
            {
                return n;
            }
        }
        return kOwned + (shared.fetch_add(1U, std::memory_order_relaxed) % kShared);
    }

    [[nodiscard]] static crd::u32 stripe() noexcept
    {
        thread_local const crd::u32 kStripe = assign_stripe();
        return kStripe;
    }

    crd::containers::StaticArray<crd::containers::CacheLinePadded<std::atomic<crd::u64>>, kStripes> m_stripes{};
};

} // namespace crd::perf::detail
