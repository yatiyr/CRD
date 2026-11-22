#pragma once
// The wall clock TLS and its provider read (crd-time's WallClock), pinned for a scope: a test that moves a ticket's
// clock, and a fuzz target whose oracles cross tickets, for which a host clock that steps between a ticket and its
// offer (WSL's time sync does, about every 31 s) would have the provider drop the ticket as one from the future.
#include <crd/core/types.hpp>
#include <crd/time/clocks.hpp>

namespace crd::crypto::test
{
inline constexpr u64 kPinnedUnixSeconds = 1'790'000'000U;

// Holders nest (a fixture's replay filter inside a test case that pins too): the clock is unpinned when the last one
// goes, so an inner holder never hands the outer one's scope back to the host clock.
class PinnedWallClock
{
public:
    explicit PinnedWallClock(u64 seconds = kPinnedUnixSeconds) noexcept
    {
        ++holders();
        set(seconds);
    }
    ~PinnedWallClock()
    {
        if (--holders() == 0U)
        {
            time::WallClock::unpin();
        }
    }
    PinnedWallClock(const PinnedWallClock&) = delete;
    PinnedWallClock& operator=(const PinnedWallClock&) = delete;
    PinnedWallClock(PinnedWallClock&&) = delete;
    PinnedWallClock& operator=(PinnedWallClock&&) = delete;

    void set(u64 seconds) noexcept { time::WallClock::pin(time::Instant{static_cast<i64>(seconds) * 1'000'000'000}); }

private:
    // Built and destroyed on the test's own thread (a case's threads start after its holders and end before them).
    static u32& holders() noexcept
    {
        static u32 count = 0U;
        return count;
    }
};
} // namespace crd::crypto::test
