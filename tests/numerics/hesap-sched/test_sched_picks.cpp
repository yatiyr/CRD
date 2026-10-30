// The schedulers' crd-jobs picks (task_graph.hpp, detail::SchedPick): every cut, chunk count and job count runs each
// task exactly once, keeps every DependencyGraph edge (a task starts after each predecessor has finished), computes the
// same tiles, and runs in order on the calling thread when the pool is down.

#include <crd/hesap/sched/task_graph.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <atomic>
#include <catch2/catch_test_macros.hpp>

using crd::u32;
using crd::u64;
using crd::hesap::sched::DependencyGraph;
using crd::hesap::sched::TaskGraph;
using crd::hesap::sched::TileAccess;
using crd::hesap::sched::TileDep;
using crd::hesap::sched::TileId;
using crd::hesap::sched::detail::SchedPick;

namespace
{
constexpr u32 kJobCounts[] = {0, 1, 2, 4, 8, 16, 32};
constexpr u32 kNever = ~u32{0};
constexpr u32 kMaxTasks = 500;
constexpr u32 kGuard = 4;

const SchedPick kPicks[] = {
    {},
    {.jobs_min_tasks = kNever},
    {.jobs_min_tasks = 16},
    {.chunks_per_job = 4},
    {.chunks_per_job = 0},
    {.jobs_min_tasks = 8, .chunks_per_job = 64},
};

struct JobsScope
{
    JobsScope() noexcept
    {
        crd::jobs::Config cfg{};
        cfg.num_threads = 8;
        crd::jobs::init(cfg);
    }
    ~JobsScope() noexcept { crd::jobs::shutdown(); }
    JobsScope(const JobsScope&) = delete;
    JobsScope& operator=(const JobsScope&) = delete;
};

// Each task k adds one run, stamps its start and writes k * k + 1; the slots past the tasks stay untouched.
struct Slots
{
    std::atomic<u32> runs[kMaxTasks + kGuard];
    std::atomic<u32> stamp[kMaxTasks + kGuard];
    std::atomic<u32> clock{0};
    u64 out[kMaxTasks + kGuard];

    void reset() noexcept
    {
        for (u32 k = 0; k < kMaxTasks + kGuard; ++k)
        {
            runs[k].store(0);
            stamp[k].store(0);
            out[k] = 0;
        }
        clock.store(0);
    }

    void run(u32 k) noexcept
    {
        runs[k].fetch_add(1);
        stamp[k].store(clock.fetch_add(1) + 1U);
        out[k] = u64{k} * k + 1U;
    }

    // Every task ran once and wrote its value; with `in_order`, task k ran k-th.
    [[nodiscard]] bool each_once(u32 tasks, bool in_order) const noexcept
    {
        for (u32 k = 0; k < kMaxTasks + kGuard; ++k)
        {
            const bool live = k < tasks;
            if (runs[k].load() != (live ? 1U : 0U) || out[k] != (live ? u64{k} * k + 1U : 0U))
            {
                return false;
            }
            if (live && in_order && stamp[k].load() != k + 1U)
            {
                return false;
            }
        }
        return true;
    }
};

void run_task(void* user_data, u32 k)
{
    static_cast<Slots*>(user_data)->run(k);
}

// A wavefront of R x C tiles, v(i, j) = v(i - 1, j) + v(i, j - 1) (1 off the edge), then one snapshot per tile and one
// write after it: task k < T is wave tile k, T <= k < 2T the snapshot of tile k - T, 2T <= k the write of tile k - 2T.
// The snapshot reads the tile and writes its own; the write read-writes the tile, so it waits for the snapshot (WAR).
constexpr u32 kRows = 9;
constexpr u32 kCols = 7;
constexpr u32 kTiles = kRows * kCols;

struct Wave
{
    u64 value[kTiles];
    u64 snap[kTiles];
    std::atomic<u32> start[3 * kTiles];
    std::atomic<u32> end[3 * kTiles];
    std::atomic<u32> clock{0};

    void reset() noexcept
    {
        for (u32 t = 0; t < kTiles; ++t)
        {
            value[t] = 0;
            snap[t] = 0;
        }
        for (u32 k = 0; k < 3 * kTiles; ++k)
        {
            start[k].store(0);
            end[k].store(0);
        }
        clock.store(0);
    }
};

void run_wave(void* user_data, u32 k)
{
    auto* w = static_cast<Wave*>(user_data);
    w->start[k].store(w->clock.fetch_add(1) + 1U);
    if (k < kTiles)
    {
        const u32 i = k / kCols;
        const u32 j = k % kCols;
        w->value[k] = (i > 0 ? w->value[k - kCols] : 1U) + (j > 0 ? w->value[k - 1] : 1U);
    }
    else if (k < 2 * kTiles)
    {
        w->snap[k - kTiles] = w->value[k - kTiles];
    }
    else
    {
        w->value[k - 2 * kTiles] += 1000U;
    }
    w->end[k].store(w->clock.fetch_add(1) + 1U);
}

void build_wave(DependencyGraph& g, Wave& w)
{
    for (u32 k = 0; k < kTiles; ++k)
    {
        TileDep deps[3];
        crd::usize count = 0;
        if (k >= kCols)
        {
            deps[count++] = {TileId{k - kCols}, TileAccess::Read};
        }
        if (k % kCols != 0)
        {
            deps[count++] = {TileId{k - 1}, TileAccess::Read};
        }
        deps[count++] = {TileId{k}, TileAccess::Write};
        g.add_task(run_wave, &w, {deps, count});
    }
    for (u32 t = 0; t < kTiles; ++t)
    {
        const TileDep deps[] = {{TileId{t}, TileAccess::Read}, {TileId{kTiles + t}, TileAccess::Write}};
        g.add_task(run_wave, &w, {deps, 2});
    }
    for (u32 t = 0; t < kTiles; ++t)
    {
        const TileDep deps[] = {{TileId{t}, TileAccess::ReadWrite}};
        g.add_task(run_wave, &w, {deps, 1});
    }
}

// Every edge held (each predecessor ended before its task started) and the tiles are the serial walk's.
bool wave_holds(const Wave& w)
{
    u64 want[kTiles];
    for (u32 k = 0; k < kTiles; ++k)
    {
        const u32 i = k / kCols;
        const u32 j = k % kCols;
        want[k] = (i > 0 ? want[k - kCols] : 1U) + (j > 0 ? want[k - 1] : 1U);
    }
    const auto before = [&w](u32 pred, u32 task)
    {
        return w.end[pred].load() != 0 && w.end[pred].load() < w.start[task].load();
    };
    for (u32 k = 0; k < kTiles; ++k)
    {
        if ((k >= kCols && !before(k - kCols, k)) || (k % kCols != 0 && !before(k - 1, k)))
        {
            return false;
        }
        if (!before(k, kTiles + k) || !before(kTiles + k, 2 * kTiles + k))
        {
            return false;
        }
        if (w.snap[k] != want[k] || w.value[k] != want[k] + 1000U)
        {
            return false;
        }
    }
    return true;
}
} // namespace

TEST_CASE("sched picks: TaskGraph and parallel_tiles_for run every task once at every cut, chunk and job count",
          "[hesap][sched][jobs-picks]")
{
    JobsScope js;
    crd::memory::TlsfAllocator alloc(256 * 1024);
    Slots slot_storage;
    Slots* slots = &slot_storage;
    const u32 task_counts[] = {0, 1, 3, 37, kMaxTasks};
    for (const u32 tasks : task_counts)
    {
        TaskGraph g(&alloc);
        for (u32 k = 0; k < tasks; ++k)
        {
            g.add_task(run_task, slots);
        }
        for (const SchedPick& pick : kPicks)
        {
            for (const u32 jobs : kJobCounts)
            {
                INFO("tasks " << tasks << " jobs " << jobs << " jobs_min_tasks " << pick.jobs_min_tasks
                              << " chunks_per_job " << pick.chunks_per_job);
                slots->reset();
                g.execute(jobs, pick);
                CHECK(slots->each_once(tasks, false));
            }
        }
    }
    const u32 shapes[][2] = {{0, 5}, {1, 1}, {3, 7}, {17, 13}};
    for (const auto& shape : shapes)
    {
        const u32 rows = shape[0];
        const u32 cols = shape[1];
        for (const SchedPick& pick : kPicks)
        {
            for (const u32 jobs : kJobCounts)
            {
                INFO("tiles " << rows << " x " << cols << " jobs " << jobs << " jobs_min_tasks " << pick.jobs_min_tasks
                              << " chunks_per_job " << pick.chunks_per_job);
                slots->reset();
                crd::hesap::sched::parallel_tiles_for(
                    rows, cols, jobs, [slots, cols](u32 i, u32 j) { slots->run(i * cols + j); }, pick);
                CHECK(slots->each_once(rows * cols, false));
            }
        }
    }
}

TEST_CASE("sched picks: DependencyGraph keeps every edge and computes the same tiles at every cut, chunk and job count",
          "[hesap][sched][jobs-picks]")
{
    JobsScope js;
    crd::memory::TlsfAllocator alloc(1024 * 1024);
    Wave w;
    DependencyGraph g(&alloc);
    build_wave(g, w);
    for (const SchedPick& pick : kPicks)
    {
        for (const u32 jobs : kJobCounts)
        {
            INFO("jobs " << jobs << " jobs_min_tasks " << pick.jobs_min_tasks << " chunks_per_job "
                         << pick.chunks_per_job);
            w.reset();
            g.execute(jobs, pick);
            CHECK(wave_holds(w));
        }
    }
}

TEST_CASE("sched picks: a pool that is down runs every scheduler in order on the calling thread",
          "[hesap][sched][jobs-picks]")
{
    REQUIRE(crd::jobs::num_workers() == 0U);
    crd::memory::TlsfAllocator alloc(1024 * 1024);
    Slots slot_storage;
    Slots* slots = &slot_storage;
    TaskGraph tg(&alloc);
    for (u32 k = 0; k < 37; ++k)
    {
        tg.add_task(run_task, slots);
    }
    Wave w;
    DependencyGraph dg(&alloc);
    build_wave(dg, w);
    for (const SchedPick& pick : kPicks)
    {
        slots->reset();
        tg.execute(16, pick);
        CHECK(slots->each_once(37, true));
        slots->reset();
        crd::hesap::sched::parallel_tiles_for(5, 7, 16, [slots](u32 i, u32 j) { slots->run(i * 7 + j); }, pick);
        CHECK(slots->each_once(35, true));
        w.reset();
        dg.execute(16, pick);
        CHECK(wave_holds(w));
    }
}
