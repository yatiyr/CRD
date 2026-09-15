#pragma once

#include <crd/core/types.hpp>
#include <crd/jobs/jobs.hpp> // ProgressSample, WaitGraphNode, HangKind

#include <span>

namespace crd::jobs::detail
{

// How a progress-sensitive watchdog classifies one sampling window from the ProgressSample at the window's
// start and end. Pure by design: the verdicts that cannot be reproduced in a live test -- a debugger pause,
// an OS suspend, a wall-clock change -- are exercised by feeding a synthetic elapsed time. The watchdog thread
// (a thin wrapper) calls this once per window and requires several consecutive SuspectedHang windows before it
// reports anything, so a single transient window never raises a false alarm. This classifier does NOT detect
// livelock (a spin loop keeps a worker executing, so it reads Progressing); that needs job-declared progress
// and is a separate concern.
enum class HangVerdict : crd::u8
{
    None,          // nothing outstanding to make progress on (quiescent) -- never a hang
    Progressing,   // real progress this window: completions advanced, or a worker is executing a (long) job
    Paused,        // the watchdog's own sleep overran badly -> the process was frozen; re-baseline, don't count
    SuspectedHang, // work outstanding, nothing completed, and no worker executing -- the deadlock shape
};

// before / after : progress samples taken at the window's start and end.
// requested_ms   : the sleep the watchdog asked for this window.
// actual_ms      : the wall time the sleep actually took (a steady_clock delta measured around the sleep).
// pause_factor   : how many times over the requested sleep counts as the process having been frozen.
//
// A frozen process (debugger break, OS suspend) freezes the watchdog thread too, so its sleep overshoots and
// its "no progress" is an artifact of the freeze rather than a real stall -- that window is Paused and must not
// count toward a hang. Pause is therefore checked first. A steady clock keeps a wall-clock change from being
// read as elapsed time.
[[nodiscard]] constexpr HangVerdict hang_verdict(const ProgressSample& before, const ProgressSample& after,
                                                 crd::u32 requested_ms, crd::u32 actual_ms,
                                                 crd::u32 pause_factor = 3U) noexcept
{
    if (requested_ms != 0U && actual_ms > requested_ms * pause_factor)
        return HangVerdict::Paused;
    if (after.outstanding == 0U)
        return HangVerdict::None; // no jobs left to run/finish -- a held-but-satisfied counter is not a hang
    if (after.completions != before.completions)
        return HangVerdict::Progressing; // real work finished this window
    if (after.executing != 0U)
        return HangVerdict::Progressing; // a long job is still running -- progressing, not deadlocked
    return HangVerdict::SuspectedHang;   // outstanding work, nothing finished, nothing executing
}

// Classify a confirmed hang into the shape distinguishable from parked-fiber evidence alone. Pure and
// header-only so the whole decision table is unit-tested without a running pool. Called by the watchdog only
// after a SuspectedHang is confirmed, so `executing` is 0 by construction; the parameter is kept for a defensive
// contract and future refinement. Order matters: a wait CYCLE is a deadlock no matter what else is queued, so it
// is checked first. Then, with no cycle: nothing parked means work sits in a queue with no worker draining it
// (ExecutorStarved); parked fibers with no closing cycle are blocked on work that was never dispatched
// (ParkedStalled). This does NOT attempt livelock/starvation/exhaustion -- those need signals the wait graph
// does not carry (see the HangKind comment in jobs.hpp).
//
// The edge model: node i (a parked fiber) waits on the counter with task id `waiting_on_task_id[i]`; a node j
// OWNS that counter when `own_task_id[j] == waiting_on_task_id[i]` (and the owner id is non-zero -- a zero id is
// an unresolved chain end, never an edge target). Several parked fibers may share one owner id (many jobs on one
// batch counter); any path back to a visited node closes a cycle. A self-edge (a job waiting on its own counter)
// is a real deadlock and is caught the same way.
// `outstanding` and `executing` are the caller's confirmed-hang context (outstanding > 0, executing == 0 by the
// time this is reached); the classification is a pure function of the parked-fiber graph, so they are unread
// today and kept for the contract / future refinement.
[[nodiscard]] inline HangKind classify_hang(std::span<const WaitGraphNode> parked,
                                            [[maybe_unused]] crd::u32 outstanding,
                                            [[maybe_unused]] crd::u32 executing) noexcept
{
    const crd::usize raw = parked.size();
    const crd::usize n   = raw < 64U ? raw : 64U; // reachability masks are u64; cap defensively (buffer is 64)

    // succ[i] = bitmask of successors j where own_task_id[j] == waiting_on_task_id[i] (owner id non-zero).
    crd::u64 reach[64] = {};
    for (crd::usize i = 0U; i < n; ++i)
    {
        const crd::u64 target = parked[i].waiting_on_task_id;
        crd::u64       succ   = 0U;
        for (crd::usize j = 0U; j < n; ++j)
        {
            const crd::u64 owner = parked[j].own_task_id;
            if (owner != 0U && owner == target)
                succ |= (crd::u64{1} << j);
        }
        reach[i] = succ;
    }

    // Warshall transitive closure over >=1-edge reachability: if bit i ends up set in reach[i], a path of length
    // >= 1 returns to i -- a cycle. O(n^3) with n <= 64, run only on a confirmed (rare) hang.
    for (crd::usize k = 0U; k < n; ++k)
        for (crd::usize i = 0U; i < n; ++i)
            if (reach[i] & (crd::u64{1} << k))
                reach[i] |= reach[k];

    for (crd::usize i = 0U; i < n; ++i)
        if (reach[i] & (crd::u64{1} << i))
            return HangKind::WaitCycle;

    if (raw == 0U)
        return HangKind::ExecutorStarved; // outstanding work (by the caller's contract) but nothing parked
    return HangKind::ParkedStalled;
}

// K-consecutive-window debouncer over the per-window verdicts. A single stale window is never enough (a brief
// lull between a completion and the next dispatch can look momentarily idle); the watchdog reports only after
// K back-to-back SuspectedHang windows. Any non-hang window (progress, quiescence, or a pause) ends the
// episode and resets the count. Pure and header-only so the whole fire policy is unit-tested without a clock.
class HangDetector
{
public:
    explicit constexpr HangDetector(crd::u32 k = 3U) noexcept : m_k(k == 0U ? 1U : k) {}

    // Feed one window's verdict. Returns true EXACTLY ONCE per stall episode -- on the K-th consecutive
    // SuspectedHang -- then stays silent until a non-hang window resets it, so a persistent stall produces one
    // report, not one per window.
    [[nodiscard]] constexpr bool feed(HangVerdict v) noexcept
    {
        if (v != HangVerdict::SuspectedHang)
        {
            m_stale = 0U;
            m_fired = false;
            return false;
        }
        if (m_fired)
            return false; // already reported this episode
        if (++m_stale >= m_k)
        {
            m_fired = true;
            return true;
        }
        return false;
    }

    [[nodiscard]] constexpr crd::u32 stale_windows() const noexcept { return m_stale; }

private:
    crd::u32 m_k;
    crd::u32 m_stale = 0U;
    bool     m_fired = false;
};

// Per-window priority-starvation check over two LaneSamples. Returns a bitmask over the three priority lanes:
// bit L set means lane L had work waiting (backlog > 0) yet made NO dispatch progress (pops flat) this window,
// WHILE the system as a whole did make progress (some lane's pops advanced, or a job completed). Flat-everything
// is a hang, not starvation, so it returns 0 there -- the boundary the tests pin. Pure and window-based (never
// wall-clock), consistent with the hang watchdog: a starved system keeps completing other work, so the hang
// verdict reads Progressing and would never flag this on its own.
[[nodiscard]] constexpr crd::u8 starvation_verdict(const LaneSample& before, const LaneSample& after,
                                                   crd::u64 completions_before,
                                                   crd::u64 completions_after) noexcept
{
    bool system_progressed = completions_after != completions_before;
    for (crd::u32 i = 0U; i < 3U; ++i)
        system_progressed = system_progressed || (after.pops[i] != before.pops[i]);
    if (!system_progressed)
        return 0U; // nothing anywhere advanced -> hang territory, not starvation

    crd::u8 mask = 0U;
    for (crd::u32 i = 0U; i < 3U; ++i)
        if (after.backlog[i] > 0U && after.pops[i] == before.pops[i])
            mask |= static_cast<crd::u8>(1U << i);
    return mask;
}

// K-consecutive-window debouncer for per-lane starvation, mirroring HangDetector but tracking the three lanes
// independently. feed() takes a per-window starvation bitmask and returns the bitmask of lanes that FIRE this
// window -- each lane reaches K consecutive starved windows and fires exactly once per episode; any non-starved
// window for a lane resets that lane. Pure and header-only so the whole fire policy is unit-tested without a
// clock.
class StarvationDetector
{
public:
    explicit constexpr StarvationDetector(crd::u32 k = 3U) noexcept : m_k(k == 0U ? 1U : k) {}

    [[nodiscard]] constexpr crd::u8 feed(crd::u8 starved_mask) noexcept
    {
        crd::u8 fired = 0U;
        for (crd::u32 i = 0U; i < 3U; ++i)
        {
            const crd::u8 bit = static_cast<crd::u8>(1U << i);
            if ((starved_mask & bit) == 0U)
            {
                m_stale[i] = 0U;
                m_fired[i] = false;
                continue;
            }
            if (m_fired[i])
                continue; // already reported this episode for this lane
            if (++m_stale[i] >= m_k)
            {
                m_fired[i] = true;
                fired |= bit;
            }
        }
        return fired;
    }

    [[nodiscard]] constexpr crd::u32 stale_windows(crd::u32 lane) const noexcept { return m_stale[lane]; }

private:
    crd::u32 m_k;
    crd::u32 m_stale[3] = {0U, 0U, 0U};
    bool     m_fired[3] = {false, false, false};
};

// Per-task livelock debouncer over successive monitored_snapshot() windows. A monitored task (one that opted in
// via note_progress(), so it appears in the snapshot) whose progress_epoch stays flat for K consecutive windows
// is livelocked -- spinning without progress. The tracker keeps a fixed table keyed by (tier, fiber_index,
// task_id): a task advancing its epoch resets; a task absent this window (completed or parked) is evicted; a
// reused fiber slot running a new task_id is a fresh entry (never inherits the old task's staleness). feed()
// takes the window's monitored nodes and writes the nodes that FIRE this window into fired_out (each fires once
// per episode), returning how many fired. Pure and header-only so the fire policy is unit-tested without a
// clock; a fixed 64-entry table so it never allocates (monitored tasks beyond that are simply not tracked --
// the report's monitored_total stays honest).
class LivelockTracker
{
public:
    static constexpr crd::u32 kCapacity = 64U;

    explicit constexpr LivelockTracker(crd::u32 k = 3U) noexcept : m_k(k == 0U ? 1U : k) {}

    [[nodiscard]] crd::usize feed(std::span<const crd::jobs::ProgressNode> seen,
                                  std::span<crd::jobs::ProgressNode>       fired_out) noexcept
    {
        for (crd::u32 i = 0U; i < kCapacity; ++i)
            m_slots[i].seen = false;

        crd::usize fired_count = 0U;
        for (const crd::jobs::ProgressNode& node : seen)
        {
            Slot* slot = find(node);
            if (slot == nullptr)
            {
                slot = allocate();
                if (slot == nullptr)
                    continue; // table full: this task is untracked (monitored_total reports the truth)
                slot->used        = true;
                slot->tier        = node.tier;
                slot->fiber_index = node.fiber_index;
                slot->task_id     = node.task_id;
                slot->last_epoch  = node.progress_epoch;
                slot->stale       = 0U;
                slot->fired       = false;
            }
            else if (node.progress_epoch != slot->last_epoch)
            {
                slot->last_epoch = node.progress_epoch; // real progress -> reset the episode
                slot->stale      = 0U;
                slot->fired      = false;
            }
            else if (!slot->fired && ++slot->stale >= m_k)
            {
                slot->fired = true;
                if (fired_count < fired_out.size())
                    fired_out[fired_count] = node; // the current node carries parent_task_id / epoch for the report
                ++fired_count;
            }
            slot->seen = true;
        }

        for (crd::u32 i = 0U; i < kCapacity; ++i)
            if (m_slots[i].used && !m_slots[i].seen)
                m_slots[i] = Slot{}; // evict tasks that completed / parked / had their slot reused
        return fired_count;
    }

    [[nodiscard]] crd::u32 stale_windows(crd::u64 task_id) const noexcept
    {
        for (crd::u32 i = 0U; i < kCapacity; ++i)
            if (m_slots[i].used && m_slots[i].task_id == task_id)
                return m_slots[i].stale;
        return 0U;
    }

private:
    struct Slot
    {
        bool     used        = false;
        bool     seen        = false;
        crd::u8  tier        = 0U;
        crd::u32 fiber_index = 0U;
        crd::u64 task_id     = 0U;
        crd::u32 last_epoch  = 0U;
        crd::u32 stale       = 0U;
        bool     fired       = false;
    };

    [[nodiscard]] Slot* find(const crd::jobs::ProgressNode& n) noexcept
    {
        for (crd::u32 i = 0U; i < kCapacity; ++i)
            if (m_slots[i].used && m_slots[i].task_id == n.task_id && m_slots[i].tier == n.tier &&
                m_slots[i].fiber_index == n.fiber_index)
                return &m_slots[i];
        return nullptr;
    }

    [[nodiscard]] Slot* allocate() noexcept
    {
        for (crd::u32 i = 0U; i < kCapacity; ++i)
            if (!m_slots[i].used)
                return &m_slots[i];
        return nullptr;
    }

    crd::u32 m_k;
    Slot     m_slots[kCapacity] = {};
};

} // namespace crd::jobs::detail
