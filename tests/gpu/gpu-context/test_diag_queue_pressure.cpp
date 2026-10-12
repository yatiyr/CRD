// DIAG.10c GPU queue depth at the completion seams, device-free.
// Declared contract (docs/design/runtime-diagnostics.md, DIAG.10c; crd/gpu/queue_pressure.hpp):
// - A step is in flight from the seam that queued it until a seam observed it complete. A queue runs its steps in
//   order, so a timeline observed complete through a value retires the step that signalled it and every earlier step
//   on the same queue, and no step on another queue. A DX12 fence signals increasing values; a Vulkan fence is binary
//   (value 0, observed through every value). A step without a timeline retires with a later timeline on its queue or
//   an idle wait.
// - A loss, a forgotten queue or device, and a timeline that starts again while it has steps pending abandon them,
//   counted; a completion with nothing pending is unmatched; a full table leaves a step untracked and the reading
//   unread.
// The handles below are addresses of the test's own objects, standing in for a device, queues and fences; every
// expected count is worked out by hand from the rule above. The tracker is process-wide, so each case uses devices
// of its own and forgets them at its end, and process counts are compared as differences.

#include <crd/core/budget_pressure.hpp>
#include <crd/gpu/queue_pressure.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstring>

namespace
{
namespace g = crd::gpu;

// Distinct addresses for fake handles: one object per handle, never dereferenced.
struct Handles
{
    char device{};
    char other_device{};
    char queue_a{};
    char queue_b{};
    char fence_x{};
    char fence_y{};
};

void check_counts(const g::QueueDepthCounts& c, crd::u64 submitted, crd::u64 retired, crd::u64 abandoned,
                  crd::u64 in_flight)
{
    CHECK(c.submitted == submitted);
    CHECK(c.retired == retired);
    CHECK(c.abandoned == abandoned);
    CHECK(c.in_flight == in_flight);
    CHECK(c.submitted == c.retired + c.abandoned + c.in_flight);
}
} // namespace

TEST_CASE("diag10c: a fence retires its own steps through the observed value, in queue order",
          "[gpu][queue][pressure][diag10c]")
{
    Handles                   h;
    const g::QueueDepthCounts process_before = g::queue_depth_process();
    const void*               dev            = &h.device;

    // DX12 shape: one queue, one fence, values 1..3 signalled in order.
    g::queue_step_submitted(dev, &h.queue_a, &h.fence_x, 1U);
    g::queue_step_submitted(dev, &h.queue_a, &h.fence_x, 2U);
    g::queue_step_submitted(dev, &h.queue_a, &h.fence_x, 3U);
    g::QueueDepthCounts c = g::queue_depth_of_device(dev);
    check_counts(c, 3U, 0U, 0U, 3U);
    CHECK(c.high_water == 3U);
    CHECK(c.queues == 1U);

    // Observed through 2: steps 1 and 2 retire, 3 stays in flight.
    g::queue_timeline_completed(dev, &h.fence_x, 2U);
    c = g::queue_depth_of_device(dev);
    check_counts(c, 3U, 2U, 0U, 1U);
    CHECK(c.high_water == 3U); // the high water is the most ever in flight, not the current depth

    // The reading: unbounded (reported, never judged), in use = in flight, high water tracked.
    const g::QueueDepthReading r = g::queue_depth_reading(c);
    CHECK(r.depth.read);
    CHECK_FALSE(r.depth.bounded);
    CHECK(r.depth.in_use == 1U);
    CHECK(r.depth.high_water_tracked);
    CHECK(r.depth.high_water == 3U);
    CHECK(crd::pressure_level(r.depth, r.depth.in_use) == crd::PressureLevel::Unbounded);
    CHECK(std::strlen(r.reason) == 0U);

    // Observing 2 again finds nothing pending at or below it: unmatched, nothing changes.
    g::queue_timeline_completed(dev, &h.fence_x, 2U);
    c = g::queue_depth_of_device(dev);
    check_counts(c, 3U, 2U, 0U, 1U);
    CHECK(c.unmatched == 1U);

    g::queue_timeline_completed(dev, &h.fence_x, 3U);
    c = g::queue_depth_of_device(dev);
    check_counts(c, 3U, 3U, 0U, 0U);

    // The process counts moved by exactly this device's steps.
    const g::QueueDepthCounts process_after = g::queue_depth_process();
    CHECK(process_after.submitted - process_before.submitted == 3U);
    CHECK(process_after.retired - process_before.retired == 3U);
    CHECK(process_after.unmatched - process_before.unmatched == 1U);
    CHECK(process_after.in_flight == process_before.in_flight);

    g::queue_forget_device(dev);
    check_counts(g::queue_depth_of_device(dev), 0U, 0U, 0U, 0U); // forgotten: fresh counts
}

TEST_CASE("diag10c: a later timeline retires earlier steps of its queue only, fence-less ones included",
          "[gpu][queue][pressure][diag10c]")
{
    Handles     h;
    const void* dev = &h.device;

    // Vulkan shape on queue A: two fence-less submissions, then one with fence X, then one more fence-less.
    g::queue_step_submitted(dev, &h.queue_a, nullptr, 0U);
    g::queue_step_submitted(dev, &h.queue_a, nullptr, 0U);
    g::queue_step_submitted(dev, &h.queue_a, &h.fence_x, 0U);
    g::queue_step_submitted(dev, &h.queue_a, nullptr, 0U);
    // Queue B holds one step on fence Y.
    g::queue_step_submitted(dev, &h.queue_b, &h.fence_y, 0U);
    g::QueueDepthCounts c = g::queue_depth_of_device(dev);
    check_counts(c, 5U, 0U, 0U, 5U);
    CHECK(c.queues == 2U);
    CHECK(c.high_water == 5U);

    // X observed (a binary fence: through every value): the three steps up to X on queue A retire. The fence-less
    // step queued after X and queue B's step stay in flight.
    g::queue_timeline_completed(dev, &h.fence_x, g::kQueueThroughAll);
    c = g::queue_depth_of_device(dev);
    check_counts(c, 5U, 3U, 0U, 2U);

    // Queue A idle: its last step retires; queue B is untouched.
    g::queue_idle_observed(dev, &h.queue_a);
    c = g::queue_depth_of_device(dev);
    check_counts(c, 5U, 4U, 0U, 1U);

    // Device idle: everything left on the device retires.
    g::queue_device_idle_observed(dev);
    c = g::queue_depth_of_device(dev);
    check_counts(c, 5U, 5U, 0U, 0U);
    CHECK(c.unmatched == 0U); // idle waits never count as unmatched completions

    g::queue_forget_device(dev);
}

TEST_CASE("diag10c: two fences on one queue retire each other's earlier steps by queue order",
          "[gpu][queue][pressure][diag10c]")
{
    Handles     h;
    const void* dev = &h.device;
    // DX12 shape: a context's fence X and a present surface's fence Y signal on the same queue A.
    g::queue_step_submitted(dev, &h.queue_a, &h.fence_x, 1U); // seq 1
    g::queue_step_submitted(dev, &h.queue_a, &h.fence_y, 1U); // seq 2
    g::queue_step_submitted(dev, &h.queue_a, &h.fence_x, 2U); // seq 3
    // Y observed through 1 retires seq 1 and 2 (X's value 1 ran before it on the queue); X's value 2 is still queued.
    g::queue_timeline_completed(dev, &h.fence_y, 1U);
    g::QueueDepthCounts c = g::queue_depth_of_device(dev);
    check_counts(c, 3U, 2U, 0U, 1U);
    // X through 1 is now unmatched: its value 1 retired with Y's.
    g::queue_timeline_completed(dev, &h.fence_x, 1U);
    CHECK(g::queue_depth_of_device(dev).unmatched == 1U);
    g::queue_timeline_completed(dev, &h.fence_x, 2U);
    check_counts(g::queue_depth_of_device(dev), 3U, 3U, 0U, 0U);
    g::queue_forget_device(dev);
}

TEST_CASE("diag10c: a timeline that starts again abandons what it had pending", "[gpu][queue][pressure][diag10c]")
{
    Handles     h;
    const void* dev = &h.device;

    // A Vulkan binary fence re-armed before any seam observed its last submission: the first step is abandoned.
    g::queue_step_submitted(dev, &h.queue_a, &h.fence_x, 0U);
    g::queue_step_submitted(dev, &h.queue_a, &h.fence_x, 0U);
    g::QueueDepthCounts c = g::queue_depth_of_device(dev);
    check_counts(c, 2U, 0U, 1U, 1U);

    // A DX12 fence created again at a reused address: values 1 and 2 are pending when value 1 is signalled anew.
    g::queue_step_submitted(dev, &h.queue_b, &h.fence_y, 1U);
    g::queue_step_submitted(dev, &h.queue_b, &h.fence_y, 2U);
    g::queue_step_submitted(dev, &h.queue_b, &h.fence_y, 1U);
    c = g::queue_depth_of_device(dev);
    check_counts(c, 5U, 0U, 3U, 2U);
    // An increasing value on the same fence abandons nothing.
    g::queue_step_submitted(dev, &h.queue_b, &h.fence_y, 2U);
    c = g::queue_depth_of_device(dev);
    check_counts(c, 6U, 0U, 3U, 3U);

    g::queue_forget_device(dev);
}

TEST_CASE("diag10c: a lost device abandons its steps and its depth is unread until it is forgotten",
          "[gpu][queue][pressure][diag10c]")
{
    Handles     h;
    const void* dev   = &h.device;
    const void* other = &h.other_device;
    g::queue_step_submitted(dev, &h.queue_a, &h.fence_x, 1U);
    g::queue_step_submitted(dev, &h.queue_a, &h.fence_x, 2U);
    g::queue_step_submitted(other, &h.queue_b, &h.fence_y, 1U);

    g::queue_device_lost(dev);
    const g::QueueDepthCounts c = g::queue_depth_of_device(dev);
    check_counts(c, 2U, 0U, 2U, 0U);
    CHECK(c.lost);
    const g::QueueDepthReading r = g::queue_depth_reading(c);
    CHECK_FALSE(r.depth.read);
    CHECK(std::strstr(r.reason, "lost") != nullptr);
    CHECK(g::queue_depth_process().lost);

    // The other device is untouched.
    const g::QueueDepthCounts o = g::queue_depth_of_device(other);
    check_counts(o, 1U, 0U, 0U, 1U);
    CHECK_FALSE(o.lost);
    CHECK(g::queue_depth_reading(o).depth.read);

    // A wait after the loss observes nothing: the steps were abandoned, not completed.
    g::queue_timeline_completed(dev, &h.fence_x, 2U);
    CHECK(g::queue_depth_of_device(dev).retired == 0U);

    g::queue_forget_device(dev);
    const g::QueueDepthCounts fresh = g::queue_depth_of_device(dev);
    CHECK_FALSE(fresh.lost);
    check_counts(fresh, 0U, 0U, 0U, 0U);
    g::queue_forget_device(other);
    CHECK(g::queue_depth_of_device(other).submitted == 0U);
}

TEST_CASE("diag10c: forgetting a queue abandons its pending steps and ends the device's counts with its last queue",
          "[gpu][queue][pressure][diag10c]")
{
    Handles     h;
    const void* dev = &h.device;
    g::queue_step_submitted(dev, &h.queue_a, &h.fence_x, 1U);
    g::queue_step_submitted(dev, &h.queue_b, &h.fence_y, 1U);
    g::queue_timeline_completed(dev, &h.fence_y, 1U);

    // Queue A is destroyed with its step still queued: abandoned. Queue B keeps the device's counts alive.
    g::queue_forget_queue(dev, &h.queue_a);
    g::QueueDepthCounts c = g::queue_depth_of_device(dev);
    check_counts(c, 2U, 1U, 1U, 0U);
    CHECK(c.queues == 1U);

    // The last queue goes: the counts end with it, and a later device at this address starts fresh.
    g::queue_forget_queue(dev, &h.queue_b);
    c = g::queue_depth_of_device(dev);
    check_counts(c, 0U, 0U, 0U, 0U);
    CHECK(c.queues == 0U);
    CHECK(c.high_water == 0U);
}

TEST_CASE("diag10c: a step the full table cannot hold is untracked and the depth reads as unread",
          "[gpu][queue][pressure][diag10c]")
{
    Handles                   h;
    const void*               dev           = &h.device;
    const g::QueueDepthCounts process_start = g::queue_depth_process();
    // Room for what the process has pending already plus this device's steps; one more is untracked.
    const crd::u64 room = g::kQueueTrackedSteps - process_start.in_flight;
    for (crd::u64 i = 0U; i < room; ++i)
    {
        g::queue_step_submitted(dev, &h.queue_a, &h.fence_x, i + 1U);
    }
    g::QueueDepthCounts c = g::queue_depth_of_device(dev);
    CHECK(c.in_flight == room);
    CHECK(c.untracked == 0U);
    CHECK(g::queue_depth_reading(c).depth.read);

    g::queue_step_submitted(dev, &h.queue_a, &h.fence_x, room + 1U);
    c = g::queue_depth_of_device(dev);
    CHECK(c.untracked == 1U);
    CHECK(c.submitted == room); // an untracked step is not counted as submitted
    const g::QueueDepthReading r = g::queue_depth_reading(c);
    CHECK_FALSE(r.depth.read);
    CHECK(std::strstr(r.reason, "full") != nullptr);
    CHECK(g::queue_depth_process().untracked == process_start.untracked + 1U);

    // Retiring through the last tracked value empties the table again.
    g::queue_timeline_completed(dev, &h.fence_x, room);
    check_counts(g::queue_depth_of_device(dev), room, room, 0U, 0U);
    g::queue_forget_device(dev);
    CHECK(g::queue_depth_process().in_flight == process_start.in_flight);
}

TEST_CASE("diag10c: the default context reading says queue depth is not tracked", "[gpu][queue][pressure][diag10c]")
{
    const g::QueueDepthReading none;
    CHECK_FALSE(none.depth.read);
    CHECK(std::strlen(none.reason) > 0U);
}
