// ---------------------------------------------------------------------------
// crd-perf -- crd-jobs adapter implementation (Detour D-003 v0c).
//
// Hook strategy:
//   - on_job_begin   parks (FiberHandle -> BeginToken) in a fixed-size open
//                    addressed table; emits push_region. The BeginToken stays
//                    parked until on_job_end pops it.
//   - on_job_end     looks up the BeginToken, calls pop_region (which writes
//                    a Sample to whichever thread is currently running --
//                    if the job migrated, begin_thread != end_thread).
//   - on_fiber_yield bumps a stat and emits no Sample. The future v0c2 may
//                    emit a "split" Sample here; for v0c we keep one paired
//                    Sample per job, which is the locked wire format.
//   - on_fiber_resume sets current_fiber_id so subsequent nested scopes are
//                     tagged with the fiber identity.
//
// FiberHandle -> BeginToken table:
//
//   Cerid fibers are bucketed by stack size (Small: 128 max, Medium: 64,
//   Large: 16) -- total 208 at default config. We keep a fixed-size table
//   with 512 slots (next-pow2 of 208 * 1.5 for a comfortable load factor)
//   and open addressing (linear probe) on the pointer hash. The table is
//   sized at install time; resize is not supported -- the engine's fiber
//   count is bounded at startup.
// ---------------------------------------------------------------------------

#include <crd/perf/jobs_adapter.hpp>

#include <crd/core/assert.hpp>
#include <crd/jobs/observer.hpp>
#include <crd/perf/profiler.hpp>
#include <crd/perf/sample.hpp>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <new>

namespace crd::perf
{

#if CRD_PERF_ENABLED

namespace
{

constexpr crd::u32 kTokenTableSize = 512U;   // power of two; mask = size-1
constexpr crd::u32 kTokenTableMask = kTokenTableSize - 1U;

struct TokenSlot
{
    std::atomic<crd::jobs::FiberHandle> key{nullptr};
    BeginToken                          token{};
};

TokenSlot g_tokens[kTokenTableSize]{};
std::mutex g_tokens_mutex; // protects insert / erase; lookup is lock-free

// One interned NameId for every job region. v0c keeps it static -- a per-job
// label can be threaded in a future slice when JobDecl carries a name string.
NameId g_job_name{};

// Observer instance; pointer to this is registered with crd::jobs.
crd::jobs::JobObserver g_observer{};

bool g_installed = false;
std::mutex g_install_mutex;

// Internal atomic mirror of JobsAdapterStats. The observer callbacks fire
// concurrently on every worker thread (a parallel_for's jobs end in
// parallel), so the counters must be atomic -- a plain `++` is a data race
// (lost updates surfaced as `jobs_ended == 7` for an 8-job batch under
// linux-gcc -O2). Relaxed ordering is correct: these are pure tallies with
// no happens-before relationship to any other read.
struct AtomicStats
{
    std::atomic<crd::u64> jobs_begun{0};
    std::atomic<crd::u64> jobs_ended{0};
    std::atomic<crd::u64> fibers_yielded{0};
    std::atomic<crd::u64> fibers_resumed{0};
    std::atomic<crd::u64> missing_tokens{0};

    void reset() noexcept
    {
        jobs_begun.store(0, std::memory_order_relaxed);
        jobs_ended.store(0, std::memory_order_relaxed);
        fibers_yielded.store(0, std::memory_order_relaxed);
        fibers_resumed.store(0, std::memory_order_relaxed);
        missing_tokens.store(0, std::memory_order_relaxed);
    }
};

AtomicStats g_stats{};

// FNV-1a-style mix of a 64-bit pointer to a u32 hash. Adequate distribution
// for ~200 live fibers in a 512-slot table.
[[nodiscard]] crd::u32 hash_handle(crd::jobs::FiberHandle h) noexcept
{
    auto v = static_cast<crd::u64>(reinterpret_cast<std::uintptr_t>(h));
    v ^= v >> 32U;
    v *= 0x9E3779B97F4A7C15ULL;
    v ^= v >> 32U;
    return static_cast<crd::u32>(v);
}

void park_token(crd::jobs::FiberHandle h, BeginToken t) noexcept
{
    std::lock_guard<std::mutex> lock(g_tokens_mutex);
    crd::u32 idx = hash_handle(h) & kTokenTableMask;
    for (crd::u32 step = 0U; step < kTokenTableSize; ++step)
    {
        auto& slot = g_tokens[idx];
        crd::jobs::FiberHandle expected = nullptr;
        if (slot.key.compare_exchange_strong(expected, h, std::memory_order_acq_rel,
                                             std::memory_order_relaxed))
        {
            slot.token = t;
            return;
        }
        idx = (idx + 1U) & kTokenTableMask;
    }
    CRD_ASSERT_MSG(false, "crd-perf: job-adapter token table saturated; bump kTokenTableSize");
}

[[nodiscard]] bool take_token(crd::jobs::FiberHandle h, BeginToken& out) noexcept
{
    std::lock_guard<std::mutex> lock(g_tokens_mutex);
    crd::u32 idx = hash_handle(h) & kTokenTableMask;
    for (crd::u32 step = 0U; step < kTokenTableSize; ++step)
    {
        auto& slot = g_tokens[idx];
        if (slot.key.load(std::memory_order_acquire) == h)
        {
            out = slot.token;
            slot.key.store(nullptr, std::memory_order_release);
            return true;
        }
        idx = (idx + 1U) & kTokenTableMask;
    }
    return false;
}

// ---- Observer callbacks --------------------------------------------------

void cb_on_job_begin(crd::jobs::FiberHandle fiber, crd::u8 /*thread_index*/,
                     crd::u8 /*priority*/, crd::u8 /*stack_tier*/) noexcept
{
    // The OS thread running the job may not be registered with the
    // profiler yet (jobs spawns worker threads independently). Register
    // lazily; idempotent.
    register_thread("job-worker");
    set_current_fiber_id(static_cast<crd::u32>(reinterpret_cast<std::uintptr_t>(fiber)));

    BeginToken tok = push_region(g_job_name, Category::Job);
    park_token(fiber, tok);
    g_stats.jobs_begun.fetch_add(1, std::memory_order_relaxed);
}

void cb_on_job_end(crd::jobs::FiberHandle fiber, crd::u8 /*thread_index*/) noexcept
{
    BeginToken tok{};
    if (take_token(fiber, tok))
    {
        pop_region(g_job_name, tok, Category::Job);
        g_stats.jobs_ended.fetch_add(1, std::memory_order_relaxed);
    }
    else
    {
        g_stats.missing_tokens.fetch_add(1, std::memory_order_relaxed);
    }
    set_current_fiber_id(0U); // clear fiber tag on the thread; next non-job scope is OS-thread context
}

void cb_on_fiber_yield(crd::jobs::FiberHandle /*fiber*/, crd::u8 /*thread_index*/) noexcept
{
    g_stats.fibers_yielded.fetch_add(1, std::memory_order_relaxed);
    // v0c keeps a single paired Sample per job (begin -> end across the
    // yield is captured by Sample.begin_thread vs end_thread). A future
    // v0g UI enhancement could emit a split marker here.
}

void cb_on_fiber_resume(crd::jobs::FiberHandle fiber, crd::u8 /*thread_index*/) noexcept
{
    register_thread("job-worker"); // lazy register on resume thread too
    set_current_fiber_id(static_cast<crd::u32>(reinterpret_cast<std::uintptr_t>(fiber)));
    g_stats.fibers_resumed.fetch_add(1, std::memory_order_relaxed);
}

} // namespace

void install_jobs_adapter() noexcept
{
    std::lock_guard<std::mutex> lock(g_install_mutex);
    if (g_installed)
    {
        return;
    }
    // Intern the canonical "job" region name. If the profiler isn't yet
    // initialised, intern_name returns kInvalidNameId and the push_region
    // calls will no-op (push returns a zero token; pop drops the Sample).
    g_job_name = intern_name("job");

    g_observer.on_job_begin    = &cb_on_job_begin;
    g_observer.on_job_end      = &cb_on_job_end;
    g_observer.on_fiber_yield  = &cb_on_fiber_yield;
    g_observer.on_fiber_resume = &cb_on_fiber_resume;

    // Reset stats so each off->on transition starts from zero. Idempotent
    // re-install (the early-return above) keeps the running totals.
    g_stats.reset();

    crd::jobs::set_observer(&g_observer);
    g_installed = true;
}

void uninstall_jobs_adapter() noexcept
{
    std::lock_guard<std::mutex> lock(g_install_mutex);
    if (!g_installed)
    {
        return;
    }
    crd::jobs::set_observer(nullptr);
    g_observer = crd::jobs::JobObserver{};

    // Drain any parked tokens (jobs that began but for which on_job_end
    // never fired -- shouldn't happen under normal shutdown, but pay it
    // off so a reinstall starts clean).
    {
        std::lock_guard<std::mutex> lock_t(g_tokens_mutex);
        for (auto& slot : g_tokens)
        {
            slot.key.store(nullptr, std::memory_order_release);
        }
    }
    // Reset stats so a subsequent test or workload reads a clean baseline
    // even when it never re-installs the adapter.
    g_stats.reset();
    g_installed = false;
}

[[nodiscard]] bool jobs_adapter_installed() noexcept { return g_installed; }

[[nodiscard]] JobsAdapterStats jobs_adapter_stats() noexcept
{
    JobsAdapterStats out{};
    out.jobs_begun     = g_stats.jobs_begun.load(std::memory_order_relaxed);
    out.jobs_ended     = g_stats.jobs_ended.load(std::memory_order_relaxed);
    out.fibers_yielded = g_stats.fibers_yielded.load(std::memory_order_relaxed);
    out.fibers_resumed = g_stats.fibers_resumed.load(std::memory_order_relaxed);
    out.missing_tokens = g_stats.missing_tokens.load(std::memory_order_relaxed);
    return out;
}

#else // CRD_PERF_ENABLED == 0

void install_jobs_adapter() noexcept {}
void uninstall_jobs_adapter() noexcept {}
[[nodiscard]] bool jobs_adapter_installed() noexcept { return false; }
[[nodiscard]] JobsAdapterStats jobs_adapter_stats() noexcept { return JobsAdapterStats{}; }

#endif

} // namespace crd::perf
