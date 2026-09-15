// Task-instance identity (crd::jobs::current_task_id()).
//
// Proves "unique task instances, separate from recycled fiber/counter addresses": every
// run()/run_and_wait() call stamps a fresh, non-zero id on its Counter; that id is reachable from
// inside the job via current_task_id(); it is distinct from the recycled Counter ADDRESS (the pool
// reuses slots, the id does not repeat); a whole batch is one task (shared id); a nested run restores
// the parent's id on resume; and an off-fiber caller reads 0. Public API only.

#include <catch2/catch_test_macros.hpp>

#include <crd/jobs/jobs.hpp>
#include <crd/core/types.hpp>

#include <array>
#include <span>
#include <thread>

namespace
{
// Job body: record the id of the task currently running on this fiber into the slot handed via data.
void record_task_id(void* d) noexcept
{
    *static_cast<crd::u64*>(d) = crd::jobs::current_task_id();
}

// Count distinct values in [begin, begin+n) with an O(n^2) sweep -- n is tiny here, no set needed.
template <typename It>
crd::usize distinct_count(It begin, crd::usize n) noexcept
{
    crd::usize d = 0U;
    for (crd::usize i = 0U; i < n; ++i)
    {
        bool seen = false;
        for (crd::usize j = 0U; j < i; ++j)
        {
            if (begin[j] == begin[i])
            {
                seen = true;
                break;
            }
        }
        if (!seen)
            ++d;
    }
    return d;
}
} // namespace

// ---------------------------------------------------------------------------
// Each separate run() gets a unique id; the Counter ADDRESS recycles, the id does not.
// ---------------------------------------------------------------------------
TEST_CASE("diag: task ids are unique across separate runs while counter slots recycle", "[jobs][diag]")
{
    crd::jobs::Config cfg;
    cfg.num_threads = 2U; // background worker so wait() takes the spin path from the main thread
    crd::jobs::init(cfg);

    constexpr crd::usize kN = 64U;
    std::array<crd::u64, kN>              ids{};  // current_task_id() observed inside each run's job
    std::array<crd::jobs::Counter*, kN>   ptrs{}; // the Counter* run() handed back each iteration

    for (crd::usize i = 0U; i < kN; ++i)
    {
        crd::jobs::JobDecl job{};
        job.fn   = &record_task_id;
        job.data = &ids[i];

        crd::jobs::Counter* const c = crd::jobs::run(job);
        ptrs[i] = c;      // captured BEFORE wait(): the slot is live here
        crd::jobs::wait(c); // releases the counter back to the pool -> its slot is now reusable
    }

    for (crd::usize i = 0U; i < kN; ++i)
        CHECK(ids[i] != 0U); // 0 is reserved for "no task"; a running job must never see it

    // The whole point: ids are all distinct (a fresh stamp per run), yet the pool handed back a
    // recycled address at least once (fewer distinct pointers than runs) -- so the id is genuinely
    // independent of the slot address, which is what diagnostics rely on to tell two runs apart.
    CHECK(distinct_count(ids.data(), kN) == kN);
    CHECK(distinct_count(ptrs.data(), kN) < kN);

    crd::jobs::shutdown();
}

// ---------------------------------------------------------------------------
// A batch submitted in one run() is ONE task: every job in it shares the same id.
// (Documents the per-run() contract so it is not "fixed" into per-job later.)
// ---------------------------------------------------------------------------
TEST_CASE("diag: all jobs in one batch share a single task id", "[jobs][diag]")
{
    crd::jobs::Config cfg;
    cfg.num_threads = 2U;
    crd::jobs::init(cfg);

    constexpr crd::usize kBatch = 4U;
    std::array<crd::u64, kBatch> ids{};

    std::array<crd::jobs::JobDecl, kBatch> jobs{};
    for (crd::usize i = 0U; i < kBatch; ++i)
    {
        jobs[i].fn   = &record_task_id;
        jobs[i].data = &ids[i];
    }

    crd::jobs::Counter* const c = crd::jobs::run(std::span<const crd::jobs::JobDecl>(jobs.data(), kBatch));
    crd::jobs::wait(c);

    CHECK(ids[0] != 0U);
    for (crd::usize i = 1U; i < kBatch; ++i)
        CHECK(ids[i] == ids[0]); // one Counter -> one task_id for the whole batch

    crd::jobs::shutdown();
}

// ---------------------------------------------------------------------------
// Nested run_and_wait: the child is its own task; the parent's id reappears after the child completes.
// This is the migration/nesting proof current_task_id()'s comment claims -- no thread-local bookkeeping,
// the id is derived from the running fiber's counter, which travels with the fiber across suspend/resume.
// ---------------------------------------------------------------------------
TEST_CASE("diag: nested run gets a distinct id and the parent id is restored on resume", "[jobs][diag]")
{
    crd::jobs::Config cfg;
    cfg.num_threads = 2U;
    crd::jobs::init(cfg);

    struct RootData
    {
        crd::u64 parent_before = 0U;
        crd::u64 parent_after  = 0U;
        crd::u64 child         = 0U;
    };
    static RootData rd; // static: the job fn is captureless, so it reaches this via data
    rd = RootData{};

    crd::jobs::JobDecl root{};
    root.fn = [](void* d) noexcept
    {
        auto* const data = static_cast<RootData*>(d);
        data->parent_before = crd::jobs::current_task_id();

        crd::jobs::JobDecl child{};
        child.fn   = &record_task_id;
        child.data = &data->child;
        crd::jobs::run_and_wait(child); // suspends this fiber until the child finishes, then resumes

        data->parent_after = crd::jobs::current_task_id();
    };
    root.data = &rd;

    crd::jobs::Counter* const c = crd::jobs::run(root);
    crd::jobs::wait(c);

    CHECK(rd.parent_before != 0U);
    CHECK(rd.child != 0U);
    CHECK(rd.child != rd.parent_before);         // the nested run is a different task instance
    CHECK(rd.parent_after == rd.parent_before);  // parent's identity restored once the child returned

    crd::jobs::shutdown();
}

// ---------------------------------------------------------------------------
// Off-fiber: the main thread, outside any job, reads 0 (the documented "no task" value).
// ---------------------------------------------------------------------------
TEST_CASE("diag: current_task_id is zero when no job is running on this thread", "[jobs][diag]")
{
    crd::jobs::Config cfg;
    cfg.num_threads = 2U;
    crd::jobs::init(cfg);

    CHECK(crd::jobs::current_task_id() == 0U); // main thread is not inside a fiber

    // A run/wait round-trip must not leave a stale id behind on the main thread.
    crd::u64 inside = 0U;
    crd::jobs::JobDecl job{};
    job.fn   = &record_task_id;
    job.data = &inside;
    crd::jobs::Counter* const c = crd::jobs::run(job);
    crd::jobs::wait(c);
    CHECK(inside != 0U);
    CHECK(crd::jobs::current_task_id() == 0U);

    crd::jobs::shutdown();
}

// ---------------------------------------------------------------------------
// Causal parent edge: parent_task_id() is 0 at the top level and the immediate submitter's id when
// nested. Two levels deep discriminates "immediate parent" from "grandparent".
// ---------------------------------------------------------------------------
TEST_CASE("diag: a top-level job has parent_task_id 0", "[jobs][diag]")
{
    crd::jobs::Config cfg;
    cfg.num_threads = 2U;
    crd::jobs::init(cfg);

    crd::u64 parent = 12345U; // sentinel: must be overwritten with 0
    crd::jobs::JobDecl job{};
    job.fn   = [](void* d) noexcept { *static_cast<crd::u64*>(d) = crd::jobs::parent_task_id(); };
    job.data = &parent;
    crd::jobs::run_and_wait(job);
    CHECK(parent == 0U); // submitted from the main thread -> no parent task

    crd::jobs::shutdown();
}

TEST_CASE("diag: parent_task_id records the immediate submitter across two nesting levels", "[jobs][diag]")
{
    crd::jobs::Config cfg;
    cfg.num_threads = 2U;
    crd::jobs::init(cfg);

    struct Chain
    {
        crd::u64 root_id = 0U,  root_parent  = 999U;
        crd::u64 child_id = 0U, child_parent = 999U;
        crd::u64 grand_id = 0U, grand_parent = 999U;
    };
    static Chain ch;
    ch = Chain{};

    crd::jobs::JobDecl root{};
    root.fn = [](void* d) noexcept
    {
        auto* const x = static_cast<Chain*>(d);
        x->root_id     = crd::jobs::current_task_id();
        x->root_parent = crd::jobs::parent_task_id();

        crd::jobs::JobDecl child{};
        child.fn = [](void* dc) noexcept
        {
            auto* const y = static_cast<Chain*>(dc);
            y->child_id     = crd::jobs::current_task_id();
            y->child_parent = crd::jobs::parent_task_id();

            crd::jobs::JobDecl grand{};
            grand.fn = [](void* dg) noexcept
            {
                auto* const z = static_cast<Chain*>(dg);
                z->grand_id     = crd::jobs::current_task_id();
                z->grand_parent = crd::jobs::parent_task_id();
            };
            grand.data = y;
            crd::jobs::run_and_wait(grand);
        };
        child.data = x;
        crd::jobs::run_and_wait(child);
    };
    root.data = &ch;
    crd::jobs::run_and_wait(root);

    CHECK(ch.root_parent == 0U);            // root submitted from the main thread
    CHECK(ch.root_id != 0U);
    CHECK(ch.child_id != 0U);
    CHECK(ch.child_id != ch.root_id);       // distinct task instances
    CHECK(ch.child_parent == ch.root_id);   // child's parent is the root
    CHECK(ch.child_parent != ch.child_id);  // parent id is NOT the child's own id (ordering bug-catch)
    CHECK(ch.grand_id != 0U);
    CHECK(ch.grand_id != ch.child_id);
    CHECK(ch.grand_parent == ch.child_id);  // grandchild's parent is the CHILD (immediate)...
    CHECK(ch.grand_parent != ch.root_id);   // ...not the grandparent
    CHECK(ch.grand_parent != ch.grand_id);

    crd::jobs::shutdown();
}

// ---------------------------------------------------------------------------
// Pooled execution, not a sequential fallback: a batch actually runs across more than one thread. Each
// job records the worker index it ran on. Scheduling is nondeterministic (a slow round could land every
// job on one thread), so the batch is retried a bounded number of times; a correctly pooled scheduler
// spreads within a round or two, while a genuinely single-threaded executor could never satisfy it.
// ---------------------------------------------------------------------------
TEST_CASE("diag: a batch runs across more than one worker thread (pooled, not sequential)", "[jobs][diag]")
{
    crd::jobs::Config cfg;
    cfg.num_threads = 4U; // several background workers so a batch can spread
    crd::jobs::init(cfg);

    constexpr crd::usize kN = 64U;
    bool multithreaded = false;

    for (int round = 0; round < 32 && !multithreaded; ++round)
    {
        std::array<crd::u32, kN>           where{};
        std::array<crd::jobs::JobDecl, kN> jobs{};
        for (crd::usize i = 0U; i < kN; ++i)
        {
            jobs[i].fn = [](void* d) noexcept
            {
                std::this_thread::yield(); // let siblings be picked up by other workers, not all drained here
                *static_cast<crd::u32*>(d) = crd::jobs::worker_index();
            };
            jobs[i].data = &where[i];
        }
        crd::jobs::run_and_wait(std::span<const crd::jobs::JobDecl>(jobs.data(), kN));
        multithreaded = distinct_count(where.data(), kN) > 1U;
    }

    CHECK(multithreaded); // ran on >1 thread -> real pooled execution, not a sequential fallback

    crd::jobs::shutdown();
}
