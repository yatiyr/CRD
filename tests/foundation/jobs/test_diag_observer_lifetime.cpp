// Observer-lifetime quiescence contract (crd::jobs::is_quiescent / set_observer).
//
// is_quiescent() is true only when no job is outstanding: the counter pool tracks acquired counters,
// bumped in run() and dropped in wait(), so the predicate is false from the moment run() returns until
// wait() returns -- independent of whether the job has started, finished or parked. set_observer()
// requires quiescence so an observer swap can never split a job's begin/end across two observers.
//
// Covered here: a running job never sees quiescence; the run()/wait() bracket on an initialised pool;
// nested run_and_wait; and a legitimate observer install/clear on a quiescent initialised pool -- the
// perf adapter only ever swaps on an *un*-initialised pool, so that path is otherwise untested. The
// negative control (a mid-flight swap must fatal) cannot abort in-process; it is a harness specimen.

#include <catch2/catch_test_macros.hpp>

#include <crd/jobs/jobs.hpp>
#include <crd/jobs/observer.hpp>
#include <crd/core/types.hpp>

#include <atomic>

namespace
{
void record_quiescent(void* d) noexcept { *static_cast<bool*>(d) = crd::jobs::is_quiescent(); }
void set_flag(void* d) noexcept { static_cast<std::atomic<bool>*>(d)->store(true, std::memory_order_release); }
} // namespace

// ---------------------------------------------------------------------------
// A running job never observes a quiescent system: its own counter is outstanding.
// ---------------------------------------------------------------------------
TEST_CASE("diag: a running job never sees the system quiescent", "[jobs][diag]")
{
    crd::jobs::Config cfg;
    cfg.num_threads = 2U;
    crd::jobs::init(cfg);

    bool inside = true;
    crd::jobs::JobDecl job{};
    job.fn   = &record_quiescent;
    job.data = &inside;
    crd::jobs::run_and_wait(job);
    CHECK_FALSE(inside);

    crd::jobs::shutdown();
}

// ---------------------------------------------------------------------------
// The run()/wait() bracket on an initialised pool: true -> false -> true.
// ---------------------------------------------------------------------------
TEST_CASE("diag: is_quiescent tracks the run/wait bracket", "[jobs][diag]")
{
    crd::jobs::Config cfg;
    cfg.num_threads = 2U;
    crd::jobs::init(cfg);

    CHECK(crd::jobs::is_quiescent()); // initialised, nothing outstanding

    std::atomic<bool> ran{false};
    crd::jobs::JobDecl job{};
    job.fn   = &set_flag;
    job.data = &ran;

    crd::jobs::Counter* const c = crd::jobs::run(job);
    CHECK_FALSE(crd::jobs::is_quiescent()); // a counter is outstanding until wait() releases it
    crd::jobs::wait(c);
    CHECK(ran.load(std::memory_order_acquire));
    CHECK(crd::jobs::is_quiescent()); // back to quiescent

    crd::jobs::shutdown();
}

// ---------------------------------------------------------------------------
// Nested run_and_wait: the parent's counter is outstanding the whole time, so every vantage point
// inside the nest -- and the main thread between run(parent) and wait -- sees non-quiescent.
// ---------------------------------------------------------------------------
TEST_CASE("diag: nested jobs keep the system non-quiescent throughout", "[jobs][diag]")
{
    crd::jobs::Config cfg;
    cfg.num_threads = 2U;
    crd::jobs::init(cfg);

    struct RootData
    {
        bool parent_before = true;
        bool child         = true;
        bool parent_after  = true;
    };
    static RootData rd; // captureless job fn reaches this via data
    rd = RootData{};

    crd::jobs::JobDecl root{};
    root.fn = [](void* d) noexcept
    {
        auto* const data = static_cast<RootData*>(d);
        data->parent_before = crd::jobs::is_quiescent();

        crd::jobs::JobDecl child{};
        child.fn   = &record_quiescent;
        child.data = &data->child;
        crd::jobs::run_and_wait(child);

        data->parent_after = crd::jobs::is_quiescent();
    };
    root.data = &rd;

    crd::jobs::Counter* const c = crd::jobs::run(root);
    const bool main_mid = crd::jobs::is_quiescent(); // root counter outstanding
    crd::jobs::wait(c);
    const bool main_after = crd::jobs::is_quiescent();

    CHECK_FALSE(rd.parent_before);
    CHECK_FALSE(rd.child);
    CHECK_FALSE(rd.parent_after);
    CHECK_FALSE(main_mid);
    CHECK(main_after);

    crd::jobs::shutdown();
}

// ---------------------------------------------------------------------------
// A legitimate observer install/clear on a quiescent initialised pool must NOT trip the fatal, and a
// job may run under it. (The all-null observer is null-checked per callback by the dispatcher.)
// ---------------------------------------------------------------------------
TEST_CASE("diag: installing and clearing an observer while quiescent is allowed", "[jobs][diag]")
{
    crd::jobs::Config cfg;
    cfg.num_threads = 2U;
    crd::jobs::init(cfg);

    crd::jobs::JobObserver obs{}; // all callback pointers null
    crd::jobs::set_observer(&obs); // quiescent (initialised, nothing outstanding) -> no fatal
    CHECK(crd::jobs::current_observer() == &obs);

    std::atomic<bool> ran{false};
    crd::jobs::JobDecl job{};
    job.fn   = &set_flag;
    job.data = &ran;
    crd::jobs::run_and_wait(job);
    CHECK(ran.load(std::memory_order_acquire));

    crd::jobs::set_observer(nullptr); // still quiescent after wait -> no fatal
    CHECK(crd::jobs::current_observer() == nullptr);

    crd::jobs::shutdown();
}
