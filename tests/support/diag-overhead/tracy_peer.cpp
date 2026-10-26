// crd-diag-overhead-tracy -- the peer's out-of-line half (DIAG.11b clause (8); see overhead.hpp, the tracy mode).
//
// Tracy's lifetime and connection, and Tracy's counterparts of the two Cerid observers that are callbacks: a zone per
// job through crd-jobs' JobObserver (Cerid: the profiler's jobs adapter, a region per job) and a zone per dispatched
// CEIR op through the plan's RunHooks (Cerid: the profiling bridge's hooks, a dispatch count and self time per op
// class). Both use Tracy's C API, which takes a static source location and returns the open zone's context: the
// callbacks cannot hold a C++ scope across begin and end. Compiled only into crd-diag-overhead-tracy.

#include "overhead.hpp"

#include <crd/jobs/observer.hpp>

#include <tracy/TracyC.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

namespace diag_overhead::tracy_peer
{

namespace
{

using crd::u32;
using crd::u64;
using crd::u8;

// ---- jobs -------------------------------------------------------------------------------------------------------

// Per worker (crd-jobs' thread index): the open job's zone and the counts, each written only by its own worker, so
// the counts need no read-modify-write; read after the pool has joined its workers.
struct alignas(64) JobSlot
{
    TracyCZoneCtx    zone{};
    std::atomic<u64> zoned{0U};
    std::atomic<u64> unmatched{0U};
    bool             open = false;
    char             pad[64U - sizeof(TracyCZoneCtx) - (2U * sizeof(std::atomic<u64>)) - sizeof(bool)]{}; // one line
};
static_assert(sizeof(JobSlot) == 64U, "a worker's slot is one cache line");

JobSlot g_job_slots[256]; // NOLINT(modernize-avoid-c-arrays): indexed by the observer's u8 thread index

const ___tracy_source_location_data kJobLocation{"job", "crd::jobs job", __FILE__, static_cast<uint32_t>(__LINE__),
                                                 0U};

void bump(std::atomic<u64>& count) noexcept
{
    count.store(count.load(std::memory_order_relaxed) + 1U, std::memory_order_relaxed);
}

void on_job_begin(crd::jobs::FiberHandle /*fiber*/, u8 thread_index, u8 /*priority*/, u8 /*stack_tier*/) noexcept
{
    JobSlot& slot = g_job_slots[thread_index];
    if (slot.open) // a job began inside another on this thread: the outer zone is closed first, and counted
    {
        ___tracy_emit_zone_end(slot.zone);
        bump(slot.unmatched);
    }
    slot.zone = ___tracy_emit_zone_begin(&kJobLocation, 1);
    slot.open = true;
    if (slot.zone.active != 0)
    {
        bump(slot.zoned);
    }
}

void on_job_end(crd::jobs::FiberHandle /*fiber*/, u8 thread_index) noexcept
{
    JobSlot& slot = g_job_slots[thread_index];
    if (!slot.open)
    {
        bump(slot.unmatched);
        return;
    }
    ___tracy_emit_zone_end(slot.zone);
    slot.open = false;
}

const crd::jobs::JobObserver kJobObserver{&on_job_begin, &on_job_end, nullptr, nullptr};

// ---- CEIR ops ---------------------------------------------------------------------------------------------------

constexpr u32 kOpClasses = 32U;  // >= plan::Op's count, as the bridge's PlanProfile::kMaxOps
constexpr u32 kOpDepth   = 256U; // the bridge's parent-pause stack

// The op classes' zone names and source locations, filled once by start(); Tracy keeps the pointers.
char                          g_op_names[kOpClasses][16]{}; // NOLINT(modernize-avoid-c-arrays)
___tracy_source_location_data g_op_locations[kOpClasses]{}; // NOLINT(modernize-avoid-c-arrays)

struct OpState
{
    TracyCZoneCtx open[kOpDepth]{}; // NOLINT(modernize-avoid-c-arrays)
    u32           depth     = 0U;
    u64           zoned     = 0U;
    u64           unmatched = 0U;
};

OpState g_ops;

void op_pre(u8 op, void* user)
{
    auto* const s = static_cast<OpState*>(user);
    if (s->depth < kOpDepth)
    {
        const TracyCZoneCtx zone = ___tracy_emit_zone_begin(&g_op_locations[op < kOpClasses ? op : kOpClasses - 1U], 1);
        s->open[s->depth]        = zone;
        if (zone.active != 0)
        {
            ++s->zoned;
        }
    }
    else
    {
        ++s->unmatched;
    }
    ++s->depth;
}

void op_post(u8 /*op*/, void* user)
{
    auto* const s = static_cast<OpState*>(user);
    if (s->depth == 0U)
    {
        ++s->unmatched;
        return;
    }
    --s->depth;
    if (s->depth < kOpDepth)
    {
        ___tracy_emit_zone_end(s->open[s->depth]);
    }
}

} // namespace

void start() noexcept
{
    for (u32 i = 0U; i < kOpClasses; ++i)
    {
        (void)std::snprintf(g_op_names[i], sizeof(g_op_names[i]), "ceir.op.%u", i);
        g_op_locations[i] = ___tracy_source_location_data{g_op_names[i], "crd::ceir::plan::run", __FILE__,
                                                          static_cast<uint32_t>(__LINE__), 0U};
    }
    tracy::StartupProfiler();
}

void stop() noexcept
{
    tracy::ShutdownProfiler();
}

bool connected() noexcept
{
    return TracyIsConnected;
}

bool wait_connected(u32 ms) noexcept
{
    for (u32 waited = 0U; waited < ms; ++waited)
    {
        if (TracyIsConnected)
        {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return TracyIsConnected;
}

void install_jobs_observer() noexcept
{
    crd::jobs::set_observer(&kJobObserver);
}

void uninstall_jobs_observer() noexcept
{
    crd::jobs::set_observer(nullptr);
}

u64 jobs_zoned() noexcept
{
    u64 total = 0U;
    for (const JobSlot& slot : g_job_slots)
    {
        total += slot.zoned.load(std::memory_order_relaxed);
    }
    return total;
}

u64 jobs_unmatched() noexcept
{
    u64 total = 0U;
    for (const JobSlot& slot : g_job_slots)
    {
        total += slot.unmatched.load(std::memory_order_relaxed) + (slot.open ? 1U : 0U);
    }
    return total;
}

crd::ceir::plan::RunHooks op_zone_hooks() noexcept
{
    return crd::ceir::plan::RunHooks{&op_pre, &op_post, &g_ops};
}

u64 ops_zoned() noexcept
{
    return g_ops.zoned;
}

u64 ops_unmatched() noexcept
{
    return g_ops.unmatched + g_ops.depth;
}

} // namespace diag_overhead::tracy_peer
