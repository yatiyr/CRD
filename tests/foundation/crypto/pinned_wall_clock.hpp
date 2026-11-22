#pragma once
// The wall clock TLS and its provider read (crd-time's WallClock), pinned for a scope: a test that moves a ticket's
// clock, and a fuzz target whose oracles cross tickets, for which a host clock that steps between a ticket and its
// offer (WSL's time sync does, about every 31 s) would have the provider drop the ticket as one from the future.
#include <crd/core/types.hpp>
#include <crd/time/clocks.hpp>

namespace crd::crypto::test
{
inline constexpr u64 kPinnedUnixSeconds = 1'790'000'000U;

class PinnedWallClock
{
public:
    explicit PinnedWallClock(u64 seconds = kPinnedUnixSeconds) noexcept { set(seconds); }
    ~PinnedWallClock() { time::WallClock::unpin(); }
    PinnedWallClock(const PinnedWallClock&)            = delete;
    PinnedWallClock& operator=(const PinnedWallClock&) = delete;
    PinnedWallClock(PinnedWallClock&&)                 = delete;
    PinnedWallClock& operator=(PinnedWallClock&&)      = delete;

    void set(u64 seconds) noexcept { time::WallClock::pin(time::Instant{static_cast<i64>(seconds) * 1'000'000'000}); }
};
} // namespace crd::crypto::test
