#include <catch2/catch_test_macros.hpp>

#include "../../engine/jobs/src/scheduler.hpp"
#include <crd/core/types.hpp>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

using crd::jobs::detail::Scheduler;
using crd::jobs::JobDecl;
using crd::jobs::Priority;
using crd::jobs::StackSize;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Build a simple job that sets *data to true.
static JobDecl make_flag_job(bool* flag, Priority prio = Priority::Normal,
                             crd::i32 pin = -1)
{
    JobDecl j;
    j.fn       = [](void* d) { *static_cast<bool*>(d) = true; };
    j.data     = flag;
    j.priority = prio;
    j.pin_thread = pin;
    return j;
}

// Build a job that atomically increments an int.
static JobDecl make_count_job(std::atomic<int>* counter, Priority prio = Priority::Normal,
                              crd::i32 pin = -1)
{
    JobDecl j;
    j.fn = [](void* d)
    {
        static_cast<std::atomic<int>*>(d)->fetch_add(1, std::memory_order_relaxed);
    };
    j.data       = counter;
    j.priority   = prio;
    j.pin_thread = pin;
    return j;
}

// ---------------------------------------------------------------------------
// 1. Init and shutdown
// ---------------------------------------------------------------------------

TEST_CASE("scheduler: init and shutdown", "[jobs][scheduler]")
{
    Scheduler sched;
    REQUIRE(sched.init({1u, 256u, 16u}));
    CHECK(sched.is_initialized());
    CHECK(sched.num_threads() == 1u);

    sched.shutdown();
    CHECK_FALSE(sched.is_initialized());

    // Re-init after shutdown is legal.
    REQUIRE(sched.init({2u, 64u, 32u}));
    CHECK(sched.num_threads() == 2u);
    sched.shutdown();
}

// ---------------------------------------------------------------------------
// 2. num_threads reflects the config value exactly
// ---------------------------------------------------------------------------

TEST_CASE("scheduler: num_threads reflects config", "[jobs][scheduler]")
{
    Scheduler s1;
    REQUIRE(s1.init({1u, 256u, 16u}));
    CHECK(s1.num_threads() == 1u);

    Scheduler s4;
    REQUIRE(s4.init({4u, 256u, 16u}));
    CHECK(s4.num_threads() == 4u);
}

// ---------------------------------------------------------------------------
// 3. Single High-priority job injected and executed
// ---------------------------------------------------------------------------

TEST_CASE("scheduler: single High job via injection", "[jobs][scheduler]")
{
    Scheduler sched;
    REQUIRE(sched.init({1u, 256u, 16u}));

    bool ran = false;
    sched.push(make_flag_job(&ran, Priority::High));

    REQUIRE(sched.execute_one(0u));
    CHECK(ran);
    CHECK_FALSE(sched.execute_one(0u)); // queue now empty
}

// ---------------------------------------------------------------------------
// 4. Single Normal-priority job injected and executed
// ---------------------------------------------------------------------------

TEST_CASE("scheduler: single Normal job via injection", "[jobs][scheduler]")
{
    Scheduler sched;
    REQUIRE(sched.init({1u, 256u, 16u}));

    bool ran = false;
    sched.push(make_flag_job(&ran, Priority::Normal));

    REQUIRE(sched.execute_one(0u));
    CHECK(ran);
}

// ---------------------------------------------------------------------------
// 5. Single Low-priority job injected and executed
// ---------------------------------------------------------------------------

TEST_CASE("scheduler: single Low job via injection", "[jobs][scheduler]")
{
    Scheduler sched;
    REQUIRE(sched.init({1u, 256u, 16u}));

    bool ran = false;
    sched.push(make_flag_job(&ran, Priority::Low));

    REQUIRE(sched.execute_one(0u));
    CHECK(ran);
}

// ---------------------------------------------------------------------------
// 6. Priority drain order: High before Normal before Low
//
// Jobs are pushed into separate injection queues. execute_one() checks High
// first regardless of push order, so High always runs before Normal before Low.
// ---------------------------------------------------------------------------

TEST_CASE("scheduler: drain order High before Normal before Low", "[jobs][scheduler]")
{
    Scheduler sched;
    REQUIRE(sched.init({1u, 256u, 32u}));

    // Track execution sequence via a shared atomic counter.
    struct SeqData
    {
        std::atomic<int> counter{0};
        int high_seq{0};
        int normal_seq{0};
        int low_seq{0};
    };
    SeqData data;

    // Push in reverse priority order (Low, Normal, High) to prove that drain
    // order is determined by the priority queues, not push order.
    {
        JobDecl low;
        low.fn = [](void* d)
        {
            auto& sd = *static_cast<SeqData*>(d);
            sd.low_seq = sd.counter.fetch_add(1, std::memory_order_relaxed) + 1;
        };
        low.data     = &data;
        low.priority = Priority::Low;
        sched.push(low);
    }
    {
        JobDecl normal;
        normal.fn = [](void* d)
        {
            auto& sd = *static_cast<SeqData*>(d);
            sd.normal_seq = sd.counter.fetch_add(1, std::memory_order_relaxed) + 1;
        };
        normal.data     = &data;
        normal.priority = Priority::Normal;
        sched.push(normal);
    }
    {
        JobDecl high;
        high.fn = [](void* d)
        {
            auto& sd = *static_cast<SeqData*>(d);
            sd.high_seq = sd.counter.fetch_add(1, std::memory_order_relaxed) + 1;
        };
        high.data     = &data;
        high.priority = Priority::High;
        sched.push(high);
    }

    REQUIRE(sched.execute_one(0u)); // must pick High
    REQUIRE(sched.execute_one(0u)); // must pick Normal
    REQUIRE(sched.execute_one(0u)); // must pick Low
    CHECK_FALSE(sched.execute_one(0u));

    CHECK(data.high_seq == 1);
    CHECK(data.normal_seq == 2);
    CHECK(data.low_seq == 3);
}

// ---------------------------------------------------------------------------
// 7. Same-priority: injection queue drained before local deque
//
// Both injection and local hold a Normal job. Injection must be picked first
// per the drain order: injection → local → steal.
// ---------------------------------------------------------------------------

TEST_CASE("scheduler: injection checked before local for same priority", "[jobs][scheduler]")
{
    Scheduler sched;
    REQUIRE(sched.init({1u, 256u, 16u}));

    // Use separate bool flags to identify which job ran first.
    bool injection_ran = false;
    bool local_ran     = false;

    sched.push(make_flag_job(&injection_ran, Priority::Normal)); // → injection queue
    sched.push_local(0u, make_flag_job(&local_ran, Priority::Normal)); // → local deque

    // First execute_one must drain the injection queue.
    REQUIRE(sched.execute_one(0u));
    CHECK(injection_ran);
    CHECK_FALSE(local_ran);

    // Second execute_one drains the local deque.
    REQUIRE(sched.execute_one(0u));
    CHECK(local_ran);

    CHECK_FALSE(sched.execute_one(0u));
}

// ---------------------------------------------------------------------------
// 8. push_local: job executes on owning thread via local deque
// ---------------------------------------------------------------------------

TEST_CASE("scheduler: push_local executes on owning thread", "[jobs][scheduler]")
{
    Scheduler sched;
    REQUIRE(sched.init({1u, 256u, 16u}));

    bool ran = false;
    sched.push_local(0u, make_flag_job(&ran, Priority::Normal));

    REQUIRE(sched.execute_one(0u));
    CHECK(ran);
}

// ---------------------------------------------------------------------------
// 9. Local deque drains in LIFO order (Chase-Lev pop is last-in-first-out)
// ---------------------------------------------------------------------------

TEST_CASE("scheduler: local deque drains LIFO", "[jobs][scheduler]")
{
    Scheduler sched;
    REQUIRE(sched.init({1u, 256u, 16u}));

    // Each job records its value into executed_values.
    // Values pushed: 1, 2, 3 — expected LIFO pop order: 3, 2, 1.
    struct PushData
    {
        int                  value{0};
        std::vector<int>*    result{nullptr};
    };

    std::vector<int> executed_values;
    PushData pd[3];
    for (int i = 0; i < 3; ++i)
    {
        pd[i].value  = i + 1;        // NOLINT(cppcoreguidelines-pro-bounds-constant-array-index)
        pd[i].result = &executed_values; // NOLINT(cppcoreguidelines-pro-bounds-constant-array-index)

        JobDecl j;
        j.fn = [](void* d)
        {
            auto* data = static_cast<PushData*>(d);
            data->result->push_back(data->value);
        };
        j.data     = &pd[i]; // NOLINT(cppcoreguidelines-pro-bounds-constant-array-index)
        j.priority = Priority::Normal;
        sched.push_local(0u, j);
    }

    REQUIRE(sched.execute_one(0u));
    REQUIRE(sched.execute_one(0u));
    REQUIRE(sched.execute_one(0u));
    CHECK_FALSE(sched.execute_one(0u));

    REQUIRE(executed_values.size() == 3u);
    CHECK(executed_values.at(0) == 3); // last-in
    CHECK(executed_values.at(1) == 2);
    CHECK(executed_values.at(2) == 1); // first-in
}

// ---------------------------------------------------------------------------
// 10. Pinned job executes on the target thread and not before
// ---------------------------------------------------------------------------

TEST_CASE("scheduler: pinned job executes on target thread", "[jobs][scheduler]")
{
    Scheduler sched;
    REQUIRE(sched.init({2u, 256u, 16u}));

    bool ran = false;
    sched.push(make_flag_job(&ran, Priority::Normal, /*pin=*/0));

    // Thread 1 must not consume thread 0's pinned job.
    CHECK_FALSE(sched.execute_one(1u));
    CHECK_FALSE(ran);

    // Thread 0 consumes its own pinned slot.
    REQUIRE(sched.execute_one(0u));
    CHECK(ran);
}

// ---------------------------------------------------------------------------
// 11. Pinned job: slot cleared after execution — second execute_one finds nothing
// ---------------------------------------------------------------------------

TEST_CASE("scheduler: pinned slot cleared after execution", "[jobs][scheduler]")
{
    Scheduler sched;
    REQUIRE(sched.init({1u, 256u, 16u}));

    bool ran = false;
    sched.push(make_flag_job(&ran, Priority::Normal, /*pin=*/0));

    REQUIRE(sched.execute_one(0u));
    CHECK(ran);

    // Slot is now empty; second call finds no work.
    CHECK_FALSE(sched.execute_one(0u));
}

// ---------------------------------------------------------------------------
// 12. Work stealing: thread 1 steals from thread 0's local deque
// ---------------------------------------------------------------------------

TEST_CASE("scheduler: work stealing across threads", "[jobs][scheduler]")
{
    Scheduler sched;
    REQUIRE(sched.init({2u, 256u, 16u}));

    // Push to thread 0's local Normal deque.
    bool ran = false;
    sched.push_local(0u, make_flag_job(&ran, Priority::Normal));

    // Thread 1 has nothing in its own queues or the injection queues.
    // It should steal from thread 0's Normal local deque.
    REQUIRE(sched.execute_one(1u));
    CHECK(ran);
}

// ---------------------------------------------------------------------------
// 13. High-priority steal takes precedence over Normal steal
//
// Thread 0 has one High job and one Normal job in its local deques.
// Thread 1 drains: High should be stolen before Normal.
// ---------------------------------------------------------------------------

TEST_CASE("scheduler: steal High before Normal", "[jobs][scheduler]")
{
    Scheduler sched;
    REQUIRE(sched.init({2u, 256u, 16u}));

    struct SeqData
    {
        std::atomic<int> counter{0};
        int high_seq{0};
        int normal_seq{0};
    };
    SeqData data;

    {
        JobDecl h;
        h.fn = [](void* d)
        {
            auto& sd = *static_cast<SeqData*>(d);
            sd.high_seq = sd.counter.fetch_add(1, std::memory_order_relaxed) + 1;
        };
        h.data     = &data;
        h.priority = Priority::High;
        sched.push_local(0u, h);
    }
    {
        JobDecl n;
        n.fn = [](void* d)
        {
            auto& sd = *static_cast<SeqData*>(d);
            sd.normal_seq = sd.counter.fetch_add(1, std::memory_order_relaxed) + 1;
        };
        n.data     = &data;
        n.priority = Priority::Normal;
        sched.push_local(0u, n);
    }

    // Thread 1 steals: High steal happens before Normal steal.
    REQUIRE(sched.execute_one(1u)); // steals High from thread 0
    REQUIRE(sched.execute_one(1u)); // steals Normal from thread 0
    CHECK_FALSE(sched.execute_one(1u));

    CHECK(data.high_seq == 1);
    CHECK(data.normal_seq == 2);
}

// ---------------------------------------------------------------------------
// 14. Semaphore count: N pushes allow N immediate wait_for_work() returns
//
// This is the timing-independent semaphore correctness test. push() posts the
// semaphore once; wait_for_work() acquires once. N pushes → N non-blocking acquires.
// ---------------------------------------------------------------------------

TEST_CASE("scheduler: push increments semaphore for wait_for_work", "[jobs][scheduler]")
{
    Scheduler sched;
    REQUIRE(sched.init({1u, 256u, 32u}));

    static constexpr int kCount = 5;
    std::atomic<int> dummy{0};

    for (int i = 0; i < kCount; ++i)
        sched.push(make_count_job(&dummy, Priority::Normal));

    // Each wait_for_work() should return immediately (semaphore count = kCount).
    for (int i = 0; i < kCount; ++i)
        sched.wait_for_work();

    // Jobs are still in the injection queue — drain them.
    for (int i = 0; i < kCount; ++i)
        REQUIRE(sched.execute_one(0u));

    CHECK(dummy.load() == kCount);
    CHECK_FALSE(sched.execute_one(0u));
}

// ---------------------------------------------------------------------------
// 15. Push wakes a thread blocked in wait_for_work
// ---------------------------------------------------------------------------

TEST_CASE("scheduler: push wakes blocked wait_for_work", "[jobs][scheduler]")
{
    Scheduler sched;
    REQUIRE(sched.init({1u, 256u, 16u}));

    std::atomic<bool> woke{false};

    std::thread waiter([&sched, &woke]()
    {
        sched.wait_for_work(); // blocks until semaphore > 0
        woke.store(true, std::memory_order_release);
    });

    // Brief sleep to give the waiter thread time to start and block.
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    // push() posts the semaphore — waiter must wake.
    std::atomic<int> dummy{0};
    sched.push(make_count_job(&dummy, Priority::Normal));

    waiter.join(); // must complete promptly now that push happened
    CHECK(woke.load(std::memory_order_acquire));

    // Drain the queued job.
    REQUIRE(sched.execute_one(0u));
    CHECK(dummy.load() == 1);
}

// ---------------------------------------------------------------------------
// 16. Concurrent multi-thread stress: 4 workers drain 4 000 mixed-priority jobs
//
// All jobs are pushed to injection queues before workers start.
// Workers compete for jobs via injection dequeue and steal.
// Every job must run exactly once.
// ---------------------------------------------------------------------------

TEST_CASE("scheduler: concurrent multi-thread stress", "[jobs][scheduler][stress]")
{
    static constexpr crd::u32 kNumWorkers  = 4u;
    static constexpr int      kTotalJobs   = 4000;

    Scheduler sched;
    REQUIRE(sched.init({kNumWorkers, 256u, 4096u}));

    std::atomic<int> executed{0};

    // Push all jobs before starting workers.
    for (int i = 0; i < kTotalJobs; ++i)
    {
        const Priority prio = (i % 3 == 0)
                            ? Priority::High
                            : (i % 3 == 1) ? Priority::Normal : Priority::Low;
        sched.push(make_count_job(&executed, prio));
    }

    // Workers loop until all kTotalJobs have been run.
    std::vector<std::thread> workers;
    workers.reserve(kNumWorkers);
    for (crd::u32 t = 0u; t < kNumWorkers; ++t)
    {
        workers.emplace_back([&sched, &executed, t]()
        {
            while (executed.load(std::memory_order_acquire) < kTotalJobs)
            {
                if (!sched.execute_one(t))
                    std::this_thread::yield();
            }
        });
    }

    for (auto& w : workers)
        w.join();

    CHECK(executed.load() == kTotalJobs);
}
