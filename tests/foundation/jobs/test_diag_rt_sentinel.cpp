// DIAG.4c real-time sentinels (the acceptance's fifth distinct report: forbidden blocking/allocation). A
// declared real-time scope (RtScope) must not allocate or block; inside one, the crd allocators report an
// Allocation violation and jobs::wait() reports a Blocking violation -- a diagnostic, never a behaviour change.
// The scope flag is thread-local, so it works off any fiber (the real audio-thread case). These checks are
// CRD_ENABLE_ASSERTS-only; the tests run win-debug where that is on.
#include <crd/core/rt_sentinel.hpp>
#include <crd/memory/allocator.hpp>

#include <crd/jobs/job_decl.hpp>
#include <crd/jobs/jobs.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <thread>

namespace
{
// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables)
std::atomic<int>        g_rt_alloc_fires{0};
std::atomic<int>        g_rt_block_fires{0};
std::atomic<crd::usize> g_rt_last_bytes{0};
std::atomic<int>        g_hang_fires{0};
std::atomic<int>        g_starv_fires{0};
std::atomic<int>        g_livelock_fires{0};
std::atomic<bool>       g_child_done{false};
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables)

void record_rt(const crd::RtViolation& v, void* /*user*/) noexcept
{
    if (v.kind == crd::RtViolationKind::Allocation)
    {
        g_rt_alloc_fires.fetch_add(1, std::memory_order_relaxed);
        g_rt_last_bytes.store(v.bytes, std::memory_order_relaxed);
    }
    else if (v.kind == crd::RtViolationKind::Blocking)
    {
        g_rt_block_fires.fetch_add(1, std::memory_order_relaxed);
    }
}

void count_hang(const crd::jobs::HangReport& /*r*/, void* /*user*/) noexcept
{
    g_hang_fires.fetch_add(1, std::memory_order_relaxed);
}
void count_starv(const crd::jobs::StarvationReport& /*r*/, void* /*user*/) noexcept
{
    g_starv_fires.fetch_add(1, std::memory_order_relaxed);
}
void count_livelock(const crd::jobs::LivelockReport& /*r*/, void* /*user*/) noexcept
{
    g_livelock_fires.fetch_add(1, std::memory_order_relaxed);
}

void flag_child(void* /*data*/) noexcept { g_child_done.store(true, std::memory_order_release); }

void reset_rt() noexcept
{
    g_rt_alloc_fires.store(0, std::memory_order_relaxed);
    g_rt_block_fires.store(0, std::memory_order_relaxed);
    g_rt_last_bytes.store(0, std::memory_order_relaxed);
}

// Allocate then immediately free `n` bytes via the default allocator. The allocation is what a sentinel flags;
// deallocation is not instrumented, so it never adds a spurious fire.
void alloc_free(crd::usize n) noexcept
{
    void* const p = crd::memory::default_allocator()->allocate(n);
    crd::memory::default_allocator()->deallocate(p);
}
} // namespace

TEST_CASE("rt sentinel: allocation inside an RtScope is reported (off-fiber, main thread)", "[jobs][diag][rt]")
{
    reset_rt();
    crd::set_rt_violation_handler(&record_rt, nullptr);

    {
        crd::RtScope rt;
        alloc_free(64U);
    }

    CHECK(g_rt_alloc_fires.load(std::memory_order_relaxed) == 1);
    CHECK(g_rt_last_bytes.load(std::memory_order_relaxed) == 64U); // the requested size is reported
    CHECK(g_rt_block_fires.load(std::memory_order_relaxed) == 0);

    crd::set_rt_violation_handler(nullptr, nullptr);
}

TEST_CASE("rt sentinel: nested scopes stay active until the outermost exits", "[jobs][diag][rt]")
{
    reset_rt();
    crd::set_rt_violation_handler(&record_rt, nullptr);

    {
        crd::RtScope outer;
        {
            crd::RtScope inner;
            alloc_free(16U);
        } // inner exits -- still inside outer
        CHECK(g_rt_alloc_fires.load(std::memory_order_relaxed) == 1);
        alloc_free(16U); // still monitored
        CHECK(g_rt_alloc_fires.load(std::memory_order_relaxed) == 2);
    }
    alloc_free(16U); // outside every scope -> silent
    CHECK(g_rt_alloc_fires.load(std::memory_order_relaxed) == 2);

    crd::set_rt_violation_handler(nullptr, nullptr);
}

TEST_CASE("rt sentinel: allocation outside any scope is silent", "[jobs][diag][rt]")
{
    reset_rt();
    crd::set_rt_violation_handler(&record_rt, nullptr);

    alloc_free(128U); // no RtScope active

    CHECK(g_rt_alloc_fires.load(std::memory_order_relaxed) == 0);

    crd::set_rt_violation_handler(nullptr, nullptr);
}

TEST_CASE("rt sentinel: the scope is thread-local -- fires on a raw off-fiber thread", "[jobs][diag][rt]")
{
    reset_rt();
    crd::set_rt_violation_handler(&record_rt, nullptr);

    // A raw std::thread (not thread 0, not a pool worker) -- the real audio-callback case. The thread-local
    // depth must work here with no job system involved at all.
    std::thread t(
        []() noexcept
        {
            crd::RtScope rt;
            alloc_free(32U);
        });
    t.join();

    CHECK(g_rt_alloc_fires.load(std::memory_order_relaxed) == 1);
    CHECK(g_rt_last_bytes.load(std::memory_order_relaxed) == 32U);

    crd::set_rt_violation_handler(nullptr, nullptr);
}

TEST_CASE("rt sentinel: jobs::wait inside an RtScope is reported as Blocking", "[jobs][diag][rt]")
{
    reset_rt();
    g_child_done.store(false, std::memory_order_relaxed);
    crd::set_rt_violation_handler(&record_rt, nullptr);

    crd::jobs::Config cfg;
    cfg.num_threads = 2U; // a background worker runs the child so main need not pump it while in scope
    crd::jobs::init(cfg);

    crd::jobs::JobDecl child{};
    child.fn                     = &flag_child;
    crd::jobs::Counter* const cc = crd::jobs::run(child);

    // Wait for the child WITHOUT jobs::wait() (a plain flag poll) so it completes on the worker first; then the
    // wait() below hits its fast path (counter already 0) -- it still reports Blocking at entry, and does not
    // pump a job onto the main thread inside the scope.
    while (!g_child_done.load(std::memory_order_acquire))
        std::this_thread::yield();

    {
        crd::RtScope rt;
        crd::jobs::wait(cc);
    }

    CHECK(g_rt_block_fires.load(std::memory_order_relaxed) == 1); // the blocking call was reported
    CHECK(g_rt_alloc_fires.load(std::memory_order_relaxed) == 0); // nothing allocated in the scope

    crd::jobs::shutdown();
    crd::set_rt_violation_handler(nullptr, nullptr);
}

TEST_CASE("rt sentinel: a violation is distinct -- hang/starvation/livelock stay silent", "[jobs][diag][rt]")
{
    reset_rt();
    g_hang_fires.store(0, std::memory_order_relaxed);
    g_starv_fires.store(0, std::memory_order_relaxed);
    g_livelock_fires.store(0, std::memory_order_relaxed);
    crd::set_rt_violation_handler(&record_rt, nullptr);
    crd::jobs::set_hang_handler(&count_hang, nullptr);
    crd::jobs::set_starvation_handler(&count_starv, nullptr);
    crd::jobs::set_livelock_handler(&count_livelock, nullptr);

    crd::jobs::Config cfg;
    cfg.num_threads             = 2U;
    cfg.hang_watchdog_period_ms = 20U; // watchdog live: the other detectors could fire if anything were wrong
    crd::jobs::init(cfg);

    {
        crd::RtScope rt;
        alloc_free(48U);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(120)); // several watchdog windows on an idle pool

    CHECK(g_rt_alloc_fires.load(std::memory_order_relaxed) >= 1); // the RT violation was reported...
    CHECK(g_hang_fires.load(std::memory_order_relaxed) == 0);     // ...and only that one
    CHECK(g_starv_fires.load(std::memory_order_relaxed) == 0);
    CHECK(g_livelock_fires.load(std::memory_order_relaxed) == 0);

    crd::jobs::shutdown();
    crd::set_rt_violation_handler(nullptr, nullptr);
    crd::jobs::set_hang_handler(nullptr, nullptr);
    crd::jobs::set_starvation_handler(nullptr, nullptr);
    crd::jobs::set_livelock_handler(nullptr, nullptr);
}
