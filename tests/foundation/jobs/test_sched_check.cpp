// DIAG.4b controlled-interleaving smoke test: with the gate ON and an oracle installed, the yield points
// on the counter publication/reclamation path actually fire. Entirely gated -- in the default build
// (gate off) this file compiles to nothing; it runs only in crd-jobs-schedcheck-tests, which links the
// gate-ON twin library crd-jobs-schedcheck. The oracle here just counts calls; the recording / seeded /
// replay / driver oracles are the following steps.

#include "../../../engine/foundation/jobs/src/sched_check.hpp"

#if CRD_JOBS_SCHED_CHECK

#include <catch2/catch_test_macros.hpp>

#include "sched_driver.hpp"
#include "sched_minimize.hpp"
#include "sched_record.hpp"
#include <crd/jobs/jobs.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

namespace
{
std::atomic<int> g_point_hits{0}; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables) test counter

void count_point(void* /*user*/, const char* /*tag*/) noexcept
{
    g_point_hits.fetch_add(1, std::memory_order_relaxed);
}

std::atomic<bool> g_child_ran{false}; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

// --- f-2b (the FOUND test) state. Job bodies are captureless (JobDecl::fn is a plain function pointer),
//     so the gate atom and the slot handoff live at file scope. ---
std::atomic<bool>                g_go{false};           // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)
std::atomic<crd::jobs::Counter*> g_child_counter{nullptr}; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

void found_noop(void* /*data*/) noexcept {} // empty body: the re-acquire loop only needs the Counter*

void found_child(void* /*data*/) noexcept
{
    // Gated so its completion decrement lands AFTER the scheduler's ABA load saw value==1 -- the decrement
    // then owns the resume (the scheduler does not cancel), which is what the forced interleaving needs.
    while (!g_go.load(std::memory_order_acquire))
        std::this_thread::yield();
}

void found_root(void* /*data*/) noexcept
{
    crd::jobs::JobDecl child{};
    child.fn = &found_child;
    g_child_counter.store(crd::jobs::run(child), std::memory_order_release);
    crd::jobs::wait(g_child_counter.load(std::memory_order_acquire)); // parks this fiber on the counter path
}

// RAII: the break switch is cleared even if an assertion throws, and regardless of Catch2's case order.
struct BreakGuard
{
    BreakGuard() noexcept { crd::jobs::detail::sched_check_break_handshake(true); }
    ~BreakGuard() { crd::jobs::detail::sched_check_break_handshake(false); }
    BreakGuard(const BreakGuard&)            = delete;
    BreakGuard& operator=(const BreakGuard&) = delete;
};

struct ExposeResult
{
    bool                     arrived    = false; // the scheduler reached fp.finalizing
    bool                     reacquired = false; // the freed child slot was popped again by a fresh run()
    bool                     deadlocked = false; // the driver's safety net tripped
    bool                     found      = false; // the detector reported the reclamation this run
    std::vector<const char*> observed;           // the driver's tag trace
};

// One run of the forced reclamation choreography, driving `script`, under whatever handshake mode is
// currently set (the CALLER owns the BreakGuard). Resets the violation counter and statics; inits and
// shuts down its own pool. This is the single source of the choreography -- case 4, case 5's seed, its
// oracle and its replay all go through here so none of them can drift.
ExposeResult expose_with(const std::vector<const char*>& script)
{
    g_go.store(false, std::memory_order_relaxed);
    g_child_counter.store(nullptr, std::memory_order_relaxed);
    crd::jobs::detail::sched_check_reset_violations();

    crd::jobs::test::SchedDriver drv(script);
    drv.install();

    crd::jobs::Config cfg;
    cfg.num_threads = 3U; // one worker is held at fp.finalizing; peers must resume the fiber and run noops
    crd::jobs::init(cfg);

    crd::jobs::JobDecl root{};
    root.fn                          = &found_root;
    crd::jobs::Counter* const c_root = crd::jobs::run(root);

    ExposeResult r;
    // Wait until the scheduler ARRIVED at fp.finalizing (its ABA load already saw value==1, child still
    // gated). Only then release the child, so its decrement -- not the scheduler -- owns the resume.
    r.arrived = drv.await_arrived("fp.finalizing", std::chrono::milliseconds(5000));
    g_go.store(true, std::memory_order_release);

    // Broken variant: the fiber unwinds without the handshake, the root job finishes and releases the
    // child slot; main's wait returns once c_root hits zero (child slot already free by then).
    crd::jobs::wait(c_root);

    crd::jobs::Counter* const        target = g_child_counter.load(std::memory_order_acquire);
    std::vector<crd::jobs::Counter*> noops;
    for (crd::u32 i = 0U; i < cfg.max_counters; ++i)
    {
        crd::jobs::JobDecl noop{};
        noop.fn                     = &found_noop;
        crd::jobs::Counter* const c = crd::jobs::run(noop);
        noops.push_back(c);
        if (c == target)
        {
            r.reacquired = true;
            break;
        }
    }

    drv.mark("test.reacquired"); // release the scheduler; it now runs the detector against the recycled slot

    for (crd::jobs::Counter* c : noops)
        crd::jobs::wait(c); // balance every acquire so shutdown()'s m_acquired==0 assert holds

    crd::jobs::test::SchedDriver::uninstall();
    crd::jobs::shutdown();

    r.deadlocked = drv.deadlocked();
    r.found      = crd::jobs::detail::sched_check_violations() > 0U;
    r.observed   = drv.observed();
    return r;
}

// --- (h) multicore-stress pillar (the case below): a free-running (no driver) PERTURBED stress. It counts
//     reach-proving tags, records a per-worker last tag, and applies a seeded ~1/4 xorshift-yield jitter at
//     each yield point to widen the real-hardware interleavings between the counter path's atomic steps.
//     This perturbation surfaced the zero-drain / release recycle race in counter_decrement (a fast-path or
//     ABA-cancelled waiter released a counter while the zero-decrement was between its fetch_sub and its
//     waiters.exchange, so a concurrent acquire recycled the slot and the stale exchange then stole the next
//     generation's waiter -- a premature wake that returned run_and_wait with its child unrun and cascaded
//     into a decrement past zero). It is fixed by Counter::drained gating release() on the drain; with the
//     fix this stress is silent. The state is module-global so the captureless assert platform handler the
//     case installs can read it too. ---
struct StressState
{
    static constexpr crd::u32 kMaxWorkers = 16U;

    std::atomic<unsigned>    rng {0x9e3779b9U}; // seeded xorshift jitter; a stress aid, not a replayable artifact
    std::atomic<long>        published {0};
    std::atomic<long>        dec_claim {0};
    std::atomic<long>        wait_resumed {0};
    std::atomic<int>         children {0};
    std::atomic<int>         phase {0};    // 0=setup 1=running 2=shutdown
    std::atomic<int>         launched {0};
    // last tag seen on each worker; a fixed lock-free table indexed by worker index is exactly the point.
    std::atomic<const char*> last_tag[kMaxWorkers]; // NOLINT(cppcoreguidelines-avoid-c-arrays)

    void reset() noexcept
    {
        rng.store(0x9e3779b9U, std::memory_order_relaxed);
        published.store(0, std::memory_order_relaxed);
        dec_claim.store(0, std::memory_order_relaxed);
        wait_resumed.store(0, std::memory_order_relaxed);
        children.store(0, std::memory_order_relaxed);
        phase.store(0, std::memory_order_relaxed);
        launched.store(0, std::memory_order_relaxed);
        for (crd::u32 i = 0U; i < kMaxWorkers; ++i)
            last_tag[i].store(nullptr, std::memory_order_relaxed);
    }

    void on_point(const char* tag) noexcept
    {
        if (std::strcmp(tag, "fp.published") == 0)
            published.fetch_add(1, std::memory_order_relaxed);
        else if (std::strcmp(tag, "dec.claim") == 0)
            dec_claim.fetch_add(1, std::memory_order_relaxed);
        else if (std::strcmp(tag, "wait.resumed") == 0)
            wait_resumed.fetch_add(1, std::memory_order_relaxed);

        const crd::u32 w = crd::jobs::worker_index();
        if (w < kMaxWorkers)
            last_tag[w].store(tag, std::memory_order_relaxed);

        // Seeded jitter: yield at ~1/4 of points to widen the windows between the counter path's atomic steps.
        unsigned x = rng.load(std::memory_order_relaxed);
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        rng.store(x, std::memory_order_relaxed);
        if ((x & 3U) == 0U)
            std::this_thread::yield();
    }

    static void on_point_cb(void* user, const char* tag) noexcept
    {
        static_cast<StressState*>(user)->on_point(tag);
    }
};

// The single perturbed-stress instance. Module-global so both the watchdog thread and the captureless assert
// platform handler (which cannot capture) can read the diagnostic state on a hang / firing assert.
StressState g_stress; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

// Dump the stress diagnostic state; shared by the watchdog and the assert handler. Touches only atomics and
// stderr, so it is usable from either a watchdog thread or an assert-path callback.
void dump_stress_state(const char* prefix) noexcept
{
    std::fprintf(stderr,
                 "\n%s: phase=%d quiescent=%d launched=%d published=%ld dec_claim=%ld wait_resumed=%ld "
                 "children=%d violations=%llu\n",
                 prefix, g_stress.phase.load(std::memory_order_relaxed),
                 crd::jobs::is_quiescent() ? 1 : 0, g_stress.launched.load(std::memory_order_relaxed),
                 g_stress.published.load(std::memory_order_relaxed),
                 g_stress.dec_claim.load(std::memory_order_relaxed),
                 g_stress.wait_resumed.load(std::memory_order_relaxed),
                 g_stress.children.load(std::memory_order_relaxed),
                 static_cast<unsigned long long>(crd::jobs::detail::sched_check_violations()));
    for (crd::u32 w = 0U; w < StressState::kMaxWorkers; ++w)
    {
        const char* const t = g_stress.last_tag[w].load(std::memory_order_relaxed);
        if (t != nullptr)
            std::fprintf(stderr, "  worker[%u] last=%s\n", w, t);
    }
    std::fflush(stderr);
}

void stress_child(void* /*data*/) noexcept
{
    std::this_thread::yield(); // make the parent usually reach counter_wait before value hits 0 (park, not
    std::this_thread::yield(); // fast-path), so the run stresses the real publish/finalize/resume handshake
    g_stress.children.fetch_add(1, std::memory_order_relaxed);
}

void stress_root(void* /*data*/) noexcept
{
    crd::jobs::JobDecl child{};
    child.fn = &stress_child;
    crd::jobs::run_and_wait(child); // parks this fiber on the child's counter
}

// From an observed tag trace, the subsequence of tags that appear in `script`, in trace order (used to
// assert a script was replayed in its scripted order).
std::vector<const char*> scripted_subsequence(const std::vector<const char*>& observed,
                                              const std::vector<const char*>& script)
{
    std::vector<const char*> seen;
    for (const char* observed_tag : observed)
    {
        for (const char* scripted : script)
        {
            if (std::strcmp(observed_tag, scripted) == 0)
            {
                seen.push_back(observed_tag);
                break;
            }
        }
    }
    return seen;
}
} // namespace

TEST_CASE("schedcheck: yield points fire under the gate with an oracle installed", "[jobs][schedcheck][diag]")
{
    g_point_hits.store(0, std::memory_order_relaxed);
    g_child_ran.store(false, std::memory_order_relaxed);

    crd::jobs::detail::SchedOracle oracle{};
    oracle.on_point = &count_point;
    crd::jobs::detail::set_sched_oracle(&oracle);

    crd::jobs::Config cfg;
    cfg.num_threads = 2U;
    crd::jobs::init(cfg);

    // A nested run_and_wait parks the root fiber on the counter path -> exercises the publish/finalize/
    // resume points, and the child's completion decrement exercises the drain points.
    crd::jobs::JobDecl root{};
    root.fn = [](void*) noexcept
    {
        crd::jobs::JobDecl child{};
        child.fn = [](void*) noexcept { g_child_ran.store(true, std::memory_order_release); };
        crd::jobs::run_and_wait(child);
    };
    crd::jobs::Counter* const c = crd::jobs::run(root);
    crd::jobs::wait(c);
    CHECK(g_child_ran.load(std::memory_order_acquire));

    crd::jobs::detail::set_sched_oracle(nullptr);
    crd::jobs::shutdown();

    CHECK(g_point_hits.load(std::memory_order_relaxed) > 0); // the counter-path yield points fired
}

TEST_CASE("schedcheck: the recording oracle captures the counter-path trace and its ordering",
          "[jobs][schedcheck][diag]")
{
    crd::jobs::test::SchedRecorder rec;
    rec.install();

    crd::jobs::Config cfg;
    cfg.num_threads = 2U; // one background worker: the root parks (child cannot run until it does)
    crd::jobs::init(cfg);

    static std::atomic<bool> child_ran{false};
    child_ran.store(false, std::memory_order_relaxed);

    crd::jobs::JobDecl root{};
    root.fn = [](void*) noexcept
    {
        crd::jobs::JobDecl child{};
        child.fn = [](void*) noexcept { child_ran.store(true, std::memory_order_release); };
        crd::jobs::run_and_wait(child); // parks the root fiber on the counter path
    };
    crd::jobs::Counter* const c = crd::jobs::run(root);
    crd::jobs::wait(c);
    CHECK(child_ran.load(std::memory_order_acquire));

    crd::jobs::test::SchedRecorder::uninstall();
    crd::jobs::shutdown(); // no worker can write to the recorder after this

    const auto& ev = rec.events();
    auto first_index = [&](const char* tag) -> long
    {
        for (crd::usize i = 0U; i < ev.size(); ++i)
        {
            if (std::strcmp(ev[i].tag, tag) == 0)
                return static_cast<long>(i);
        }
        return -1;
    };
    auto contains = [&](const char* tag) { return first_index(tag) >= 0; };

    CHECK_FALSE(rec.overflowed());
    // The root fiber parked, so the publication points are present; the child's completion drained.
    CHECK(contains("fp.published"));
    CHECK(contains("fp.finalizing"));
    CHECK(contains("dec.zero"));
    CHECK(contains("wait.resumed"));
    // The trace already encodes the invariant the (f) broken variant violates: publish precedes finalize,
    // and the fiber is not observed resuming until after finalize (the park_finalized handshake).
    CHECK(first_index("fp.published") < first_index("fp.finalizing"));
    CHECK(first_index("fp.finalizing") < first_index("wait.resumed"));
    // dec.claim is present only when the decrement (not counter_finish_park's ABA re-check) won the claim,
    // so it is not asserted unconditionally.
}

TEST_CASE("schedcheck: the driver replays a scripted order without deadlock", "[jobs][schedcheck][diag]")
{
    // Robust script: the three always-present, single-occurrence park points in their natural order. The
    // driver imposes it (parking a point that arrives before its predecessor is consumed) and must not
    // deadlock -- num_threads >= 3 keeps a peer runnable while one worker is held at a point.
    const std::vector<const char*> script = {"fp.published", "fp.finalizing", "wait.resumed"};
    crd::jobs::test::SchedDriver drv(script);
    crd::jobs::detail::sched_check_reset_violations();
    drv.install();

    crd::jobs::Config cfg;
    cfg.num_threads = 3U;
    crd::jobs::init(cfg);

    static std::atomic<bool> child_ran2{false};
    child_ran2.store(false, std::memory_order_relaxed);

    crd::jobs::JobDecl root{};
    root.fn = [](void*) noexcept
    {
        crd::jobs::JobDecl child{};
        child.fn = [](void*) noexcept { child_ran2.store(true, std::memory_order_release); };
        crd::jobs::run_and_wait(child);
    };
    crd::jobs::Counter* const c = crd::jobs::run(root);
    crd::jobs::wait(c);
    CHECK(child_ran2.load(std::memory_order_acquire));

    crd::jobs::test::SchedDriver::uninstall();
    crd::jobs::shutdown();

    CHECK_FALSE(drv.deadlocked()); // the safety net was not tripped

    // Zero false positives: the repaired algorithm, even forced through this interleaving, never lets a
    // parked slot be recycled under counter_finish_park -- so the gate-only detector reports nothing.
    // (A variant that drops the park_finalized handshake is what makes this count go positive: next step.)
    CHECK(crd::jobs::detail::sched_check_violations() == 0U);

    // The scripted tags appear in the observed trace in exactly the scripted order (replay reproduced it).
    std::vector<const char*> seen;
    for (const char* observed_tag : drv.observed())
    {
        for (const char* scripted : script)
        {
            if (std::strcmp(observed_tag, scripted) == 0)
            {
                seen.push_back(observed_tag);
                break;
            }
        }
    }
    REQUIRE(seen.size() == script.size());
    for (std::size_t i = 0U; i < script.size(); ++i)
    {
        CHECK(std::strcmp(seen[i], script[i]) == 0);
    }
}

TEST_CASE("schedcheck: the broken handshake variant is FOUND and replayed from its script",
          "[jobs][schedcheck][diag]")
{
    // The exposing interleaving: the scheduler publishes the park (fp.published); the gated child's later
    // completion resumes the parked fiber (wait.resumed); with the handshake dropped the fiber unwinds and
    // its slot is released, so the TEST thread re-acquires that very slot (test.reacquired) BEFORE the
    // scheduler finishes touching it (fp.finalizing) -- the reclamation the park_finalized handshake exists
    // to forbid. The synthetic test.reacquired tag holds the scheduler at fp.finalizing until the re-acquire
    // has happened (otherwise fp.finalizing would fire the instant wait.resumed was consumed).
    const std::vector<const char*> script = {"fp.published", "wait.resumed", "test.reacquired",
                                             "fp.finalizing"};

    const BreakGuard   guard; // handshake dropped for this case (reset in the dtor even on a thrown assertion)
    const ExposeResult r = expose_with(script);

    CHECK(r.arrived);
    CHECK(r.reacquired);                // the just-released child slot came back (free-list order held)
    CHECK_FALSE(r.deadlocked);          // the safety net was not tripped
    CHECK(r.found);                     // FOUND: the detector caught the reclamation

    // REPLAYED: the scripted tags appear in the observed trace in exactly the scripted order.
    const std::vector<const char*> seen = scripted_subsequence(r.observed, script);
    CHECK(seen.size() == script.size());
    if (seen.size() == script.size())
    {
        for (std::size_t i = 0U; i < script.size(); ++i)
        {
            CHECK(std::strcmp(seen[i], script[i]) == 0);
        }
    }
}

TEST_CASE("schedcheck: the failing script minimizes to its essential tags (delta-minimization)",
          "[jobs][schedcheck][diag]")
{
    const BreakGuard guard; // every oracle run below is the broken variant

    // Every expose_with must achieve arrived + reacquired regardless of the driven script -- only the
    // detector's verdict may vary. A subset that broke the choreography would corrupt the oracle, so track
    // it across ALL oracle invocations and assert at the end it never happened (never a silent "not found").
    bool choreography_ok = true;

    // All-K-runs-must-find: a subset that only SOMETIMES finds (a race) is not a reproduction. (For this
    // failure every subset the greedy minimizer visits is in fact deterministic; K is margin.)
    constexpr int kRuns = 3;
    auto          reproduces = [&](const std::vector<const char*>& s) -> bool
    {
        for (int i = 0; i < kRuns; ++i)
        {
            const ExposeResult r = expose_with(s);
            if (!r.arrived || !r.reacquired)
                choreography_ok = false;
            if (r.deadlocked || !r.found)
                return false;
        }
        return true;
    };

    // Seed = the RECORDED failing log, projected to distinct tags in first-occurrence order -- the actual
    // "failure sequence" to minimize, not the hand-written script. (It carries the dec.zero/dec.claim drain
    // points too, which minimization must discover are inessential.)
    const ExposeResult seed_run = expose_with(
        std::vector<const char*>{"fp.published", "wait.resumed", "test.reacquired", "fp.finalizing"});
    std::vector<const char*> seed;
    for (const char* tag : seed_run.observed)
    {
        bool dup = false;
        for (const char* s : seed)
        {
            if (std::strcmp(s, tag) == 0)
            {
                dup = true;
                break;
            }
        }
        if (!dup)
            seed.push_back(tag);
    }
    CHECK(seed.size() >= 4U); // at least fp.published, wait.resumed, test.reacquired, fp.finalizing

    // Precondition: minimizing a non-failing input yields garbage, so assert the seed reproduces first.
    REQUIRE(reproduces(seed));

    const std::vector<const char*> minimal = crd::jobs::test::minimize_failure(seed, reproduces);

    // The reclamation needs exactly one thing: hold fp.finalizing until the test has re-acquired the slot.
    // So the minimal failing sequence is precisely {test.reacquired, fp.finalizing}; nothing else matters.
    auto has = [&](const std::vector<const char*>& v, const char* t)
    {
        for (const char* m : v)
        {
            if (std::strcmp(m, t) == 0)
                return true;
        }
        return false;
    };
    CHECK(minimal.size() == 2U);
    CHECK(has(minimal, "test.reacquired"));
    CHECK(has(minimal, "fp.finalizing"));

    // 1-minimality: dropping either essential tag no longer reproduces (both are deterministic not-found --
    // [fp.finalizing] alone is released immediately; [test.reacquired] alone never holds the scheduler).
    CHECK_FALSE(reproduces(std::vector<const char*>{"fp.finalizing"}));
    CHECK_FALSE(reproduces(std::vector<const char*>{"test.reacquired"}));

    // Replayed from the minimized log: it still finds, without deadlock, in the minimized order.
    const ExposeResult replay = expose_with(minimal);
    CHECK(replay.found);
    CHECK_FALSE(replay.deadlocked);
    const std::vector<const char*> seen = scripted_subsequence(replay.observed, minimal);
    CHECK(seen.size() == minimal.size());
    if (seen.size() == minimal.size())
    {
        for (std::size_t i = 0U; i < minimal.size(); ++i)
        {
            CHECK(std::strcmp(seen[i], minimal[i]) == 0);
        }
    }

    CHECK(choreography_ok); // no subset silently broke the choreography (which would mask as "not found")
}

TEST_CASE("schedcheck: perturbed multicore park/reclaim stress -- the detector stays silent",
          "[jobs][schedcheck][diag]")
{
    // The HARDWARE regime of the DIAG.4b acceptance (distinct from the logical-replay driver in cases 4/5
    // and the weak-memory GenMC model): thousands of UNCONTROLLED parks under real thread timings, NO driver
    // imposing an order, plus a seeded ~1/4 yield jitter at each yield point (see StressState) to widen the
    // interleavings. It proves the production publish/finalize/resume handshake is reached at scale on this
    // hardware and the detector reports zero false positives across all those parks. This is also the guard
    // for the zero-drain/release recycle race the jitter surfaced (now fixed by Counter::drained): before the
    // fix it underflowed a counter (~6% of runs) and stale-drained (~8%); with the fix it is silent.
    //
    // A firing CRD_ASSERT in this binary would otherwise pop a modal MessageBoxA and block at ~0 CPU
    // (indistinguishable from a deadlock), so the case installs an assert platform handler that dumps the
    // stress state and hard-exits 42, and a 20 s watchdog that dumps and _Exit(3)s a genuine hang -- both
    // bounded, diagnosable failures rather than a wedge. (It does NOT claim to find the reclamation bug on
    // demand -- a free-running search only sometimes hits the exact window; the model + driver exist for the
    // deterministic proof.)
    REQUIRE_FALSE(crd::jobs::detail::sched_check_handshake_broken()); // repaired only: broken here = real UAF
    crd::jobs::detail::sched_check_reset_violations();
    g_stress.reset();

    // Route asserts to a bounded hard-exit (never a modal dialog); restored after the run.
    crd::AssertPlatformHandler const prev_assert_handler = crd::get_assert_platform_handler();
    crd::set_assert_platform_handler(
        [](const char* msg) -> int
        {
            std::fprintf(stderr, "\nSTRESS ASSERT: %s\n", msg);
            dump_stress_state("STRESS ASSERT-STATE");
            std::_Exit(42);
        });

    std::atomic<bool> done{false};
    std::thread       watchdog(
        [&done]
        {
            for (int i = 0; i < 200; ++i) // 200 * 100 ms = 20 s
            {
                if (done.load(std::memory_order_acquire))
                    return;
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            if (done.load(std::memory_order_acquire))
                return;
            dump_stress_state("STRESS HANG");
            std::_Exit(3); // hard-exit so a hang is a bounded, diagnosable failure rather than a wedge
        });

    crd::jobs::detail::SchedOracle oracle{};
    oracle.on_point = &StressState::on_point_cb;
    oracle.user     = &g_stress;
    crd::jobs::detail::set_sched_oracle(&oracle);

    crd::jobs::Config cfg;
    cfg.num_threads = 4U;
    crd::jobs::init(cfg);

    constexpr int kRoots = 2000;
    constexpr int kBatch = 32; // <= small_fiber_count/2 and << max_counters, so a batch never exhausts pools
    g_stress.phase.store(1, std::memory_order_relaxed); // running
    int launched = 0;
    while (launched < kRoots)
    {
        const int                        n = (kRoots - launched < kBatch) ? (kRoots - launched) : kBatch;
        std::vector<crd::jobs::Counter*> cs;
        cs.reserve(static_cast<std::size_t>(n));
        for (int i = 0; i < n; ++i)
        {
            crd::jobs::JobDecl root{};
            root.fn = &stress_root;
            cs.push_back(crd::jobs::run(root));
        }
        for (crd::jobs::Counter* c : cs)
            crd::jobs::wait(c);
        launched += n;
        g_stress.launched.store(launched, std::memory_order_relaxed);
    }

    g_stress.phase.store(2, std::memory_order_relaxed); // shutdown
    crd::jobs::detail::set_sched_oracle(nullptr);
    crd::jobs::shutdown();

    done.store(true, std::memory_order_release);
    watchdog.join();
    crd::set_assert_platform_handler(prev_assert_handler);

    CHECK(g_stress.children.load(std::memory_order_relaxed) == kRoots); // every child ran (no lost work)
    CHECK(g_stress.published.load(std::memory_order_relaxed) > 0);      // the park path was reached
    CHECK(g_stress.wait_resumed.load(std::memory_order_relaxed) > 0);   // fibers actually parked and resumed
    // dec.claim fires only when the decrement drained a still-Pending waiter -- i.e. a genuine park+resume
    // where the decrement (not the scheduler's ABA cancel) owned the resume. It cannot fast-path away, so
    // it is the strong proof the handshake ran for real, and its presence means timing variance exercised
    // both resume paths across the run.
    CHECK(g_stress.dec_claim.load(std::memory_order_relaxed) > 0);
    // The zero-false-positive result for the hardware regime: across thousands of uncontrolled parks the
    // gate-only detector never fired on the repaired algorithm.
    CHECK(crd::jobs::detail::sched_check_violations() == 0U);
}

#endif
