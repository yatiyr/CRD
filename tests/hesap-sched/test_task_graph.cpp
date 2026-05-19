#include <catch2/catch_test_macros.hpp>

#include <crd/hesap/sched/task_graph.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <atomic>

using crd::hesap::sched::DependencyGraph;
using crd::hesap::sched::TaskGraph;
using crd::hesap::sched::TileAccess;
using crd::hesap::sched::TileDep;
using crd::hesap::sched::TileId;
using crd::hesap::sched::parallel_tiles_for;

namespace
{
// Per-test jobs lifecycle. Each test case init/shutdown matches the
// pattern from tests/jobs/test_jobs.cpp. RAII keeps it tidy.
struct JobsScope
{
    JobsScope() noexcept
    {
        crd::jobs::Config cfg{};
        cfg.num_threads = 4;
        crd::jobs::init(cfg);
    }
    ~JobsScope() noexcept { crd::jobs::shutdown(); }
    JobsScope(const JobsScope&) = delete;
    JobsScope& operator=(const JobsScope&) = delete;
};
} // namespace

TEST_CASE("TileId equality + null", "[hesap][sched][tile]")
{
    REQUIRE(TileId{0} == TileId{0});
    REQUIRE_FALSE(TileId{0} == TileId{1});
    REQUIRE(TileId{7}.idx == 7);
}

TEST_CASE("TaskGraph: every task runs exactly once", "[hesap][sched][task_graph]")
{
    JobsScope js;
    crd::memory::TlsfAllocator alloc(64 * 1024);
    constexpr crd::usize kN = 32;
    std::atomic<int> counters[kN]{};
    TaskGraph g(&alloc);
    for (crd::usize i = 0; i < kN; ++i)
    {
        g.add_task(
            [](void* user_data, crd::u32 task_index)
            {
                auto* arr = static_cast<std::atomic<int>*>(user_data);
                arr[task_index].fetch_add(1, std::memory_order_relaxed);
            },
            counters);
    }
    g.execute(/*num_jobs=*/4);
    for (crd::usize i = 0; i < kN; ++i)
    {
        REQUIRE(counters[i].load() == 1);
    }
}

TEST_CASE("parallel_tiles_for visits each (i,j) tile once", "[hesap][sched][parallel_tiles]")
{
    JobsScope js;
    constexpr crd::u32 kRows = 4;
    constexpr crd::u32 kCols = 6;
    std::atomic<int> grid[kRows * kCols]{};
    parallel_tiles_for(kRows, kCols, /*num_jobs=*/4,
        [&grid](crd::u32 i, crd::u32 j)
        {
            grid[i * kCols + j].fetch_add(1, std::memory_order_relaxed);
        });
    for (crd::u32 i = 0; i < kRows; ++i)
    {
        for (crd::u32 j = 0; j < kCols; ++j)
        {
            REQUIRE(grid[i * kCols + j].load() == 1);
        }
    }
}

TEST_CASE("DependencyGraph: linear chain executes in order", "[hesap][sched][dep_graph]")
{
    JobsScope js;
    crd::memory::TlsfAllocator alloc(64 * 1024);
    DependencyGraph g(&alloc);

    // 4-task linear chain: each writes & subsequent reads tile T_k. Record
    // execution order via an atomic counter; verify task i runs at time i.
    constexpr crd::u32 kN = 4;
    std::atomic<crd::u32> stamp_counter{0};
    std::atomic<crd::u32> task_stamps[kN]{};
    struct Ctx
    {
        std::atomic<crd::u32>* stamp_counter;
        std::atomic<crd::u32>* task_stamps;
    };
    Ctx ctx{&stamp_counter, task_stamps};

    auto fn = [](void* ud, crd::u32 task_idx) {
        auto* c = static_cast<Ctx*>(ud);
        c->task_stamps[task_idx].store(c->stamp_counter->fetch_add(1) + 1U,
                                       std::memory_order_relaxed);
    };
    for (crd::u32 i = 0; i < kN; ++i)
    {
        TileDep deps[2];
        crd::usize ndeps = 0;
        if (i > 0)
        {
            // Read tile T(i-1) — chain dep.
            deps[ndeps++] = {TileId{i - 1}, TileAccess::Read};
        }
        // Write tile T(i).
        deps[ndeps++] = {TileId{i}, TileAccess::Write};
        g.add_task(fn, &ctx, crd::containers::ConstSpan<TileDep>{deps, ndeps});
    }
    g.execute(4U);
    for (crd::u32 i = 0; i < kN; ++i)
    {
        REQUIRE(task_stamps[i].load() == i + 1U);
    }
}

TEST_CASE("DependencyGraph: diamond DAG completes correctly",
          "[hesap][sched][dep_graph]")
{
    JobsScope js;
    crd::memory::TlsfAllocator alloc(64 * 1024);
    DependencyGraph g(&alloc);

    // 0 writes T0
    // 1 reads T0, writes T1
    // 2 reads T0, writes T2
    // 3 reads T1, reads T2, writes T3
    std::atomic<int> run_count{0};
    auto fn = [](void* ud, crd::u32) {
        static_cast<std::atomic<int>*>(ud)->fetch_add(1);
    };
    {
        TileDep deps[] = {{TileId{0}, TileAccess::Write}};
        g.add_task(fn, &run_count, crd::containers::ConstSpan<TileDep>{deps, 1});
    }
    {
        TileDep deps[] = {{TileId{0}, TileAccess::Read}, {TileId{1}, TileAccess::Write}};
        g.add_task(fn, &run_count, crd::containers::ConstSpan<TileDep>{deps, 2});
    }
    {
        TileDep deps[] = {{TileId{0}, TileAccess::Read}, {TileId{2}, TileAccess::Write}};
        g.add_task(fn, &run_count, crd::containers::ConstSpan<TileDep>{deps, 2});
    }
    {
        TileDep deps[] = {{TileId{1}, TileAccess::Read}, {TileId{2}, TileAccess::Read},
                          {TileId{3}, TileAccess::Write}};
        g.add_task(fn, &run_count, crd::containers::ConstSpan<TileDep>{deps, 3});
    }
    g.execute(4U);
    REQUIRE(run_count.load() == 4);
}

TEST_CASE("DependencyGraph: independent tasks run in parallel",
          "[hesap][sched][dep_graph]")
{
    JobsScope js;
    crd::memory::TlsfAllocator alloc(128 * 1024);
    DependencyGraph g(&alloc);
    // 16 independent tasks (each writes a distinct tile). All should be
    // ready in level 0. Verify each runs exactly once.
    constexpr crd::u32 kN = 16;
    std::atomic<int> counters[kN]{};
    auto fn = [](void* ud, crd::u32 idx) {
        static_cast<std::atomic<int>*>(ud)[idx].fetch_add(1);
    };
    for (crd::u32 i = 0; i < kN; ++i)
    {
        TileDep deps[] = {{TileId{i}, TileAccess::Write}};
        g.add_task(fn, counters, crd::containers::ConstSpan<TileDep>{deps, 1});
    }
    g.execute(4U);
    for (crd::u32 i = 0; i < kN; ++i)
    {
        REQUIRE(counters[i].load() == 1);
    }
}

TEST_CASE("TaskGraph: clear resets the queue", "[hesap][sched][task_graph]")
{
    crd::memory::TlsfAllocator alloc(64 * 1024);
    TaskGraph g(&alloc);
    std::atomic<int> counter{0};
    g.add_task(
        [](void* user_data, crd::u32) {
            static_cast<std::atomic<int>*>(user_data)->fetch_add(1, std::memory_order_relaxed);
        },
        &counter);
    REQUIRE(g.size() == 1);
    g.clear();
    REQUIRE(g.empty());
    // No execute — clear+empty test doesn't need jobs.
}
