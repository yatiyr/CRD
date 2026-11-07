// Rewind history sampling (HESAP-NET, capability numerics.motion-sampling, owner crd-hesap-net): where a time falls among
// the last stamped snapshots, for a client's playout delay and a server's lag compensation. The snapshots are the
// caller's, one array per slot; this holds their stamps, so one history serves any state layout and allocates nothing.
//
// A client names the time it saw, so a sample is bounded by what the caller declares. A time past the newest stamp or
// older than `max_rewind` before it is moved to that bound and says so in `moved`: the caller decides, by its
// favour-the-shooter limit, whether to use it or refuse the hit. A span wider than `max_gap` (a stall, a lost run of
// snapshots) has no motion to interpolate, so the sample stays on its older snapshot and says Gap.

#pragma once

#include <crd/core/types.hpp>

#include <bit>

namespace crd::hesap::net
{

struct RewindLimits
{
    crd::i64 max_rewind; // Back from the newest stamp; zero or below stays on the newest.
    crd::i64 max_gap;    // The widest span interpolated; below zero interpolates none.
};

enum class RewindPlace : crd::u8
{
    Empty,   // No stamps.
    Exact,   // On the stamp of `older`, which `newer` repeats.
    Between, // In the span from `older` to `newer`, at `t`.
    Gap      // In a span wider than max_gap; `older` and `newer` are its ends and `t` is 0.
};

enum class RewindMoved : crd::u8
{
    No,
    TooOld, // Older than the window or the oldest stamp held: moved up to whichever is newer.
    Future  // Past the newest stamp: moved back to it.
};

struct RewindSample
{
    crd::u32 older;
    crd::u32 newer;
    crd::f64 t;    // In [0, 1].
    crd::i64 time; // The time sampled, after any move.
    RewindPlace place;
    RewindMoved moved;
};

// The last `Capacity` stamps, strictly increasing, each in a slot below Capacity that a later push reuses.
template <crd::u32 Capacity>
class RewindHistory
{
    static_assert(Capacity >= 2U && Capacity <= (crd::u32{1} << 30U), "a history spans at least one interval");

public:
    [[nodiscard]] crd::u32 size() const noexcept
    {
        return size_;
    }

    // The slot to write this stamp's snapshot in, or false, changing nothing, for a stamp not after the newest.
    [[nodiscard]] bool push(crd::i64 stamp, crd::u32& slot) noexcept
    {
        if (size_ != 0U && stamp <= stamp_at(size_ - 1U))
        {
            return false;
        }
        if (size_ == Capacity)
        {
            slot = first_;
            first_ = (first_ + 1U == Capacity) ? 0U : first_ + 1U;
        }
        else
        {
            slot = slot_at(size_);
            ++size_;
        }
        stamps_[slot] = stamp;
        return true;
    }

    [[nodiscard]] RewindSample sample(crd::i64 at, const RewindLimits& limits) const noexcept
    {
        RewindSample result{};
        if (size_ == 0U)
        {
            return result;
        }
        const crd::i64 newest = stamp_at(size_ - 1U);
        crd::i64 floor = stamp_at(0U);
        if (limits.max_rewind < 0)
        {
            floor = newest;
        }
        else if (distance(floor, newest) > static_cast<crd::u64>(limits.max_rewind))
        {
            floor = newest - limits.max_rewind; // Above the oldest stamp, so it cannot leave the range.
        }
        crd::i64 time = at;
        if (at > newest)
        {
            time = newest;
            result.moved = RewindMoved::Future;
        }
        else if (at < floor)
        {
            time = floor;
            result.moved = RewindMoved::TooOld;
        }
        result.time = time;

        // The last stamp at or before `time`, whose successor is after it.
        crd::u32 low = 0U;
        crd::u32 high = size_;
        while (high - low > 1U)
        {
            const crd::u32 middle = low + ((high - low) / 2U);
            if (stamp_at(middle) <= time)
            {
                low = middle;
            }
            else
            {
                high = middle;
            }
        }
        result.older = slot_at(low);
        const crd::i64 below = stamp_at(low);
        if (below == time)
        {
            result.newer = result.older;
            result.place = RewindPlace::Exact;
            return result;
        }
        result.newer = slot_at(low + 1U);
        const crd::u64 width = distance(below, stamp_at(low + 1U));
        if (limits.max_gap < 0 || width > static_cast<crd::u64>(limits.max_gap))
        {
            result.place = RewindPlace::Gap;
            return result;
        }
        result.place = RewindPlace::Between;
        result.t = static_cast<crd::f64>(distance(below, time)) / static_cast<crd::f64>(width);
        return result;
    }

private:
    // from <= to, so the distance fits even across the whole range.
    [[nodiscard]] static constexpr crd::u64 distance(crd::i64 from, crd::i64 to) noexcept
    {
        return std::bit_cast<crd::u64>(to) - std::bit_cast<crd::u64>(from);
    }

    [[nodiscard]] crd::u32 slot_at(crd::u32 index) const noexcept
    {
        const crd::u32 slot = first_ + index;
        return (slot >= Capacity) ? slot - Capacity : slot;
    }

    [[nodiscard]] crd::i64 stamp_at(crd::u32 index) const noexcept
    {
        return stamps_[slot_at(index)];
    }

    crd::i64 stamps_[Capacity] = {};
    crd::u32 first_ = 0U;
    crd::u32 size_ = 0U;
};

} // namespace crd::hesap::net
