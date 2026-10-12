#include <crd/gpu/queue_pressure.hpp>

#include <mutex>

namespace crd::gpu
{
namespace
{
struct Step
{
    const void* device   = nullptr;
    const void* queue    = nullptr;
    const void* timeline = nullptr;
    crd::u64    value    = 0U;
    crd::u64    seq      = 0U; // process-wide submission order
    bool        used     = false;
};

struct QueueSlot
{
    const void* device = nullptr;
    const void* queue  = nullptr;
    bool        used   = false;
};

struct DeviceSlot
{
    const void*      device = nullptr;
    QueueDepthCounts counts{};
    bool             used = false;
};

struct Tracker
{
    std::mutex       mutex;
    Step             steps[kQueueTrackedSteps]{};
    QueueSlot        queues[kQueueTrackedQueues]{};
    DeviceSlot       devices[kQueueTrackedDevices]{};
    QueueDepthCounts process{};
    crd::u64         next_seq = 0U;
};

Tracker& tracker() noexcept
{
    static Tracker t; // function-local: constructed on first use, valid during static destruction of later users
    return t;
}

DeviceSlot* find_device(Tracker& t, const void* device) noexcept
{
    for (DeviceSlot& d : t.devices)
    {
        if (d.used && d.device == device)
        {
            return &d;
        }
    }
    return nullptr;
}

DeviceSlot* device_slot(Tracker& t, const void* device) noexcept
{
    DeviceSlot* found = find_device(t, device);
    if (found != nullptr)
    {
        return found;
    }
    for (DeviceSlot& d : t.devices)
    {
        if (!d.used)
        {
            d = DeviceSlot{};
            d.device = device;
            d.used   = true;
            return &d;
        }
    }
    return nullptr;
}

QueueSlot* find_queue(Tracker& t, const void* device, const void* queue) noexcept
{
    for (QueueSlot& q : t.queues)
    {
        if (q.used && q.device == device && q.queue == queue)
        {
            return &q;
        }
    }
    return nullptr;
}

bool ensure_queue(Tracker& t, DeviceSlot& d, const void* queue) noexcept
{
    if (find_queue(t, d.device, queue) != nullptr)
    {
        return true;
    }
    for (QueueSlot& q : t.queues)
    {
        if (!q.used)
        {
            q.device = d.device;
            q.queue  = queue;
            q.used   = true;
            ++d.counts.queues;
            return true;
        }
    }
    return false;
}

void raise(crd::u64& high_water, crd::u64 value) noexcept
{
    if (value > high_water)
    {
        high_water = value;
    }
}

// End one pending step, as retired (observed complete) or abandoned.
void end_step(Tracker& t, Step& s, bool retired) noexcept
{
    DeviceSlot* d = find_device(t, s.device);
    if (d != nullptr && d->counts.in_flight > 0U)
    {
        --d->counts.in_flight;
        if (retired)
        {
            ++d->counts.retired;
        }
        else
        {
            ++d->counts.abandoned;
        }
    }
    if (t.process.in_flight > 0U)
    {
        --t.process.in_flight;
    }
    if (retired)
    {
        ++t.process.retired;
    }
    else
    {
        ++t.process.abandoned;
    }
    s = Step{};
}

// Retire every pending step of (`device`, `queue`) submitted no later than `through_seq`.
void retire_queue_through(Tracker& t, const void* device, const void* queue, crd::u64 through_seq) noexcept
{
    for (Step& s : t.steps)
    {
        if (s.used && s.device == device && s.queue == queue && s.seq <= through_seq)
        {
            end_step(t, s, true);
        }
    }
}

void abandon_where(Tracker& t, const void* device, const void* queue, bool any_queue) noexcept
{
    for (Step& s : t.steps)
    {
        if (s.used && s.device == device && (any_queue || s.queue == queue))
        {
            end_step(t, s, false);
        }
    }
}

void release_device_slot(Tracker& t, const void* device) noexcept
{
    for (QueueSlot& q : t.queues)
    {
        if (q.used && q.device == device)
        {
            q = QueueSlot{};
        }
    }
    DeviceSlot* d = find_device(t, device);
    if (d != nullptr)
    {
        *d = DeviceSlot{};
    }
}
} // namespace

QueueDepthCounts queue_depth_process() noexcept
{
    Tracker&              t = tracker();
    const std::lock_guard lock(t.mutex);
    QueueDepthCounts      out = t.process;
    out.queues                = 0U;
    for (const QueueSlot& q : t.queues)
    {
        if (q.used)
        {
            ++out.queues;
        }
    }
    out.lost = false;
    for (const DeviceSlot& d : t.devices)
    {
        if (d.used && d.counts.lost)
        {
            out.lost = true;
        }
    }
    return out;
}

QueueDepthCounts queue_depth_of_device(const void* device) noexcept
{
    Tracker&              t = tracker();
    const std::lock_guard lock(t.mutex);
    const DeviceSlot*     d = find_device(t, device);
    return d != nullptr ? d->counts : QueueDepthCounts{};
}

QueueDepthReading queue_depth_reading(const QueueDepthCounts& counts) noexcept
{
    QueueDepthReading r;
    r.counts                   = counts;
    r.depth.bounded            = false;
    r.depth.in_use             = counts.in_flight;
    r.depth.high_water         = counts.high_water;
    r.depth.high_water_tracked = true;
    if (counts.lost)
    {
        r.reason = "the device was lost: its pending submissions were abandoned, not completed";
        return r;
    }
    if (counts.untracked != 0U)
    {
        r.reason = "the queue tracker was full: untracked submissions make in flight under-read";
        return r;
    }
    r.depth.read = true;
    r.reason     = "";
    return r;
}

void queue_step_submitted(const void* device, const void* queue, const void* timeline, crd::u64 value) noexcept
{
    Tracker&              t = tracker();
    const std::lock_guard lock(t.mutex);
    DeviceSlot*           d = device_slot(t, device);
    if (d == nullptr)
    {
        ++t.process.untracked;
        return;
    }
    if (!ensure_queue(t, *d, queue))
    {
        ++d->counts.untracked;
        ++t.process.untracked;
        return;
    }
    if (timeline != nullptr)
    {
        // A timeline that starts again (a value at or below one still pending) abandons what it had pending.
        for (Step& s : t.steps)
        {
            if (s.used && s.device == device && s.timeline == timeline && s.value >= value)
            {
                end_step(t, s, false);
            }
        }
    }
    Step* free_step = nullptr;
    for (Step& s : t.steps)
    {
        if (!s.used)
        {
            free_step = &s;
            break;
        }
    }
    if (free_step == nullptr)
    {
        ++d->counts.untracked;
        ++t.process.untracked;
        return;
    }
    free_step->device   = device;
    free_step->queue    = queue;
    free_step->timeline = timeline;
    free_step->value    = value;
    free_step->seq      = ++t.next_seq;
    free_step->used     = true;
    ++d->counts.submitted;
    ++d->counts.in_flight;
    raise(d->counts.high_water, d->counts.in_flight);
    ++t.process.submitted;
    ++t.process.in_flight;
    raise(t.process.high_water, t.process.in_flight);
}

void queue_timeline_completed(const void* device, const void* timeline, crd::u64 value) noexcept
{
    Tracker&              t = tracker();
    const std::lock_guard lock(t.mutex);
    bool                  matched = false;
    // The latest matching step on each queue retires everything before it on that queue. Repeat until no matching
    // step is left: each pass retires at least the one it found.
    while (true)
    {
        const Step* latest = nullptr;
        for (const Step& s : t.steps)
        {
            if (s.used && s.device == device && s.timeline == timeline && s.value <= value
                && (latest == nullptr || s.seq > latest->seq))
            {
                latest = &s;
            }
        }
        if (latest == nullptr)
        {
            break;
        }
        matched = true;
        const void*    queue = latest->queue;
        const crd::u64 seq   = latest->seq;
        retire_queue_through(t, device, queue, seq);
    }
    if (!matched)
    {
        DeviceSlot* d = find_device(t, device);
        if (d != nullptr)
        {
            ++d->counts.unmatched;
        }
        ++t.process.unmatched;
    }
}

void queue_idle_observed(const void* device, const void* queue) noexcept
{
    Tracker&              t = tracker();
    const std::lock_guard lock(t.mutex);
    retire_queue_through(t, device, queue, kQueueThroughAll);
}

void queue_device_idle_observed(const void* device) noexcept
{
    Tracker&              t = tracker();
    const std::lock_guard lock(t.mutex);
    for (Step& s : t.steps)
    {
        if (s.used && s.device == device)
        {
            end_step(t, s, true);
        }
    }
}

void queue_device_lost(const void* device) noexcept
{
    Tracker&              t = tracker();
    const std::lock_guard lock(t.mutex);
    abandon_where(t, device, nullptr, true);
    DeviceSlot* d = device_slot(t, device);
    if (d != nullptr)
    {
        d->counts.lost = true;
    }
}

void queue_forget_queue(const void* device, const void* queue) noexcept
{
    Tracker&              t = tracker();
    const std::lock_guard lock(t.mutex);
    abandon_where(t, device, queue, false);
    DeviceSlot* d = find_device(t, device);
    QueueSlot*  q = find_queue(t, device, queue);
    if (q != nullptr)
    {
        *q = QueueSlot{};
        if (d != nullptr && d->counts.queues > 0U)
        {
            --d->counts.queues;
        }
    }
    if (d != nullptr && d->counts.queues == 0U)
    {
        release_device_slot(t, device); // the device's last live queue: its counts end with it
    }
}

void queue_forget_device(const void* device) noexcept
{
    Tracker&              t = tracker();
    const std::lock_guard lock(t.mutex);
    abandon_where(t, device, nullptr, true);
    release_device_slot(t, device);
}

} // namespace crd::gpu
