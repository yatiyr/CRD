#pragma once

// crd-gpu-context -- GPU queue depth at the completion seams (DIAG.10c).
//
// A submission is in flight from the moment a completion seam queued it until a seam observed it complete. The seams
// (DX12: dx12_submit, dx12_signal, dx12_wait; Vulkan: vk_submit, vk_wait, vk_wait_complete and the idle waits) report
// each step here, keyed by opaque handles (device, queue, timeline), so the tracker shares no code or header with a
// backend and is tested without a device.
//
// The model. A queue runs its submissions in order, so a timeline (a DX12 fence, a Vulkan fence) observed complete
// through a value retires the step that signalled it and every earlier step on the same queue. A DX12 fence signals
// increasing values; a Vulkan fence is binary and signals value 0 on every submission (a reset re-arms it). A
// submission with no timeline (a Vulkan submit without a fence) is retired by a later timeline on its queue or by an
// idle wait. A bare signal with no work (dx12_signal) is a step too.
//
// What the reading means: in flight is the steps queued and not yet observed complete at a seam. Work the GPU has
// finished but no seam has looked at still counts: the reading is the engine's own view, which is what a missing
// wait, a context destroyed with work queued or a CPU running ahead of its frames leaves behind.
//
// Every other end is counted, never hidden. A device recorded lost or forgotten, a queue forgotten, or a timeline that
// starts again while it has steps pending (a fence created again at a reused address; a binary fence re-armed before
// a seam observed it) abandons those steps. A completion observed with nothing pending is counted as unmatched (not
// an error: a second wait on a signalled fence is legal). A step the full table cannot hold is untracked, and the
// reading of its device is then unread rather than low.
//
// The tracker is a fixed table under one mutex: no allocation, callable from any thread.
// Contract: docs/design/runtime-diagnostics.md (DIAG.10c).

#include <crd/core/budget_pressure.hpp>
#include <crd/core/types.hpp>

namespace crd::gpu
{

inline constexpr crd::u32 kQueueTrackedSteps   = 512U; // pending steps across the process
inline constexpr crd::u32 kQueueTrackedQueues  = 64U;  // live queues across the process
inline constexpr crd::u32 kQueueTrackedDevices = 32U;  // devices with counts at once

// `queue_timeline_completed` through every value the timeline signalled (a Vulkan fence).
inline constexpr crd::u64 kQueueThroughAll = ~0ULL;

struct QueueDepthCounts
{
    crd::u64 submitted  = 0U;    // steps the seams queued
    crd::u64 retired    = 0U;    // ... observed complete at a seam
    crd::u64 abandoned  = 0U;    // ... that ended without an observed completion (loss, forget, restart)
    crd::u64 in_flight  = 0U;    // tracked steps neither retired nor abandoned
    crd::u64 high_water = 0U;    // the most in flight at once
    crd::u64 untracked  = 0U;    // steps a full table could not hold (in flight under-reads by this many at most)
    crd::u64 unmatched  = 0U;    // timeline completions observed with nothing pending
    crd::u32 queues     = 0U;    // live queues the counts cover
    bool     lost       = false; // a covered device was recorded lost
};

// The whole process (counts accumulate across devices), or one device (zero for a device no seam has reported, and
// fresh again once the device or its last live queue was forgotten).
[[nodiscard]] QueueDepthCounts queue_depth_process() noexcept;
[[nodiscard]] QueueDepthCounts queue_depth_of_device(const void* device) noexcept;

// The counts as a budget reading (crd/core/budget_pressure.hpp): unbounded (a bare queue has no bound, so its depth is
// reported and never judged against one), in use = in flight, high water tracked. Unread, with a reason, when the
// device was lost (its steps were abandoned, not completed) or a step went untracked.
struct QueueDepthReading
{
    BudgetReading    depth;
    QueueDepthCounts counts;
    const char*      reason = "backend does not track queue depth"; // static; empty once `depth` was read
};
[[nodiscard]] QueueDepthReading queue_depth_reading(const QueueDepthCounts& counts) noexcept;

// --- Seam reports (the completion seams call these; tests drive them with fake handles) ---

// A step was queued on `queue` of `device`, signalling `timeline` (may be null) at `value`.
void queue_step_submitted(const void* device, const void* queue, const void* timeline, crd::u64 value) noexcept;
// `timeline` of `device` was observed complete through `value`.
void queue_timeline_completed(const void* device, const void* timeline, crd::u64 value) noexcept;
// `queue` of `device` was observed idle (vkQueueWaitIdle), or the whole device (vkDeviceWaitIdle).
void queue_idle_observed(const void* device, const void* queue) noexcept;
void queue_device_idle_observed(const void* device) noexcept;
// `device` was recorded lost: its pending steps are abandoned and its counts say so until it is forgotten.
void queue_device_lost(const void* device) noexcept;
// `queue` is being destroyed: its pending steps are abandoned; the device's counts end with its last live queue.
void queue_forget_queue(const void* device, const void* queue) noexcept;
// `device` is being destroyed: its pending steps are abandoned, its queues and counts end.
void queue_forget_device(const void* device) noexcept;

} // namespace crd::gpu
