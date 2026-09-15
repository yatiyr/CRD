// No-blocking-context contract for crd::jobs::wait().
//
// An unenrolled thread (a raw std::thread, not thread 0 and not a pool worker) may wait() only when
// the pool has a background worker to decrement the counter (num_threads >= 2). On a single-thread
// pool that same call is a guaranteed deadlock, so wait() fatals instead of hanging -- that negative
// path is a subprocess specimen (it aborts), covered separately. This file proves the positive: with
// num_threads >= 2 the unenrolled-thread wait completes.

#include <catch2/catch_test_macros.hpp>

#include <crd/diag/specimen_runner.hpp>
#include <crd/jobs/jobs.hpp>

#include <atomic>
#include <thread>

TEST_CASE("diag: an unenrolled thread can wait on a multi-thread pool", "[jobs][diag]")
{
    crd::jobs::Config cfg;
    cfg.num_threads = 2U; // >= 2: a background worker exists to run the job and decrement the counter
    crd::jobs::init(cfg);

    std::atomic<bool> ran{false};
    std::atomic<bool> returned{false};

    std::thread t(
        [&]()
        {
            crd::jobs::JobDecl job{};
            job.fn = [](void* d) noexcept
            { static_cast<std::atomic<bool>*>(d)->store(true, std::memory_order_release); };
            job.data = &ran;

            crd::jobs::Counter* const c = crd::jobs::run(job);
            crd::jobs::wait(c); // unenrolled thread waiting; a worker decrements, so this must return
            returned.store(true, std::memory_order_release);
        });
    t.join();

    CHECK(ran.load(std::memory_order_acquire));
    CHECK(returned.load(std::memory_order_acquire)); // wait() returned rather than deadlocking

    crd::jobs::shutdown();
}

// ---------------------------------------------------------------------------
// The negative: an unenrolled thread waiting on a SINGLE-thread pool must trip the deadlock fatal.
// It aborts (well, _Exit(42) via the specimen's assert handler), so it runs as a bounded child through
// the DIAG.0 harness. Unlike a clean fatal, a NON-firing check here HANGS on the spin loop, so a low
// timeout is set: a timeout (exit != 42) is the failure signal.
// ---------------------------------------------------------------------------
TEST_CASE("diag harness: an unenrolled wait on a single-thread pool trips the deadlock fatal",
          "[jobs][diag][harness]")
{
    namespace cd   = crd::diag;
    namespace cont = crd::containers;

    cd::Expectation e;
    e.want       = cd::Expectation::Want::CleanExit; // exit_code is the oracle, not the verdict
    e.timeout_ms = 2000U; // the FAILURE path hangs on wait()'s spin loop -> fail in 2s, not the 10s default
    const cd::Outcome o =
        cd::run_specimen(cont::String{CRD_DIAG_UNENROLLED_WAIT_SPECIMEN}, cont::Array<cont::String>{}, e);
    INFO("verdict=" << cd::verdict_name(o.verdict) << " exit=" << o.exit_code << " id=" << o.identity.c_str());

    CHECK(cont::StringView{o.identity} == cont::StringView{"crd-diag-unenrolled-wait-specimen"});
    // 42 = the handler saw the no-blocking-context message and exited; 97 = a different assert fired;
    // anything else (a timeout terminates the child with exit 1) = the check did not fire -> the very
    // hang this contract exists to prevent. Assert the exit code, not the verdict.
    CHECK(o.exit_code == 42);
}
