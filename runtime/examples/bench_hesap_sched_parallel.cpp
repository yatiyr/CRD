// bench_hesap_sched_parallel — the hesap-sched schedulers' crd-jobs board (SIMD-DISPATCH, the folded
// HESAP-JOBS-RETUNE).
//
// One process per thread count: `--threads N` starts crd::jobs on N workers (default: the jobs default) and times one
// job (the control) and N jobs down each arm of detail::SchedPick (production's levers: the cut below which a batch or
// level runs on the calling thread, the ranges per job), round by round in turn so the arms share the host's state; the
// median of the rounds is reported with its speedup over one job. Workloads: a TaskGraph batch of even and of uneven
// tasks (task k costs k mod 16 + 1 units) from 16 to 64K tasks and a DependencyGraph wavefront of R x C tiles, a task
// a fixed chain of multiply-adds on its own output. Every arm is gated bit for bit against the control (each task
// writes only its own output); a miss exits 1.
//   bench_hesap_sched_parallel [--threads N]

#include <crd/hesap/sched/task_graph.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace
{
using crd::f64;
using crd::u32;
using crd::usize;
using crd::hesap::sched::DependencyGraph;
using crd::hesap::sched::TaskGraph;
using crd::hesap::sched::TileAccess;
using crd::hesap::sched::TileDep;
using crd::hesap::sched::TileId;
using crd::hesap::sched::detail::SchedPick;
using Clock = std::chrono::steady_clock;

constexpr int kRounds = 7;
bool g_failed = false;

struct Arm
{
    const char* name;
    SchedPick pick;
};

const Arm kArms[] = {
    {"production", SchedPick{}},
    {"serial-below-64", SchedPick{.jobs_min_tasks = 64}},
    {"serial-below-1K", SchedPick{.jobs_min_tasks = 1024}},
    {"ranges-4-per-job", SchedPick{.chunks_per_job = 4}},
    {"ranges-16-per-job", SchedPick{.chunks_per_job = 16}},
};

// The work: `units` x 64 dependent multiply-adds seeded from the task index (and, in the wavefront, its inputs).
struct Work
{
    f64* out;
    u32 units;   // per task, or the base of the uneven ramp
    bool uneven; // task k costs (k mod 16 + 1) units
    u32 cols;    // the wavefront's width; 0 for a batch
};

f64 spin(f64 x, u32 units)
{
    for (u32 r = 0; r < units * 64U; ++r)
    {
        x = x * 0.999999 + 1e-3;
    }
    return x;
}

void batch_task(void* user_data, u32 k)
{
    const auto* w = static_cast<const Work*>(user_data);
    w->out[k] = spin(static_cast<f64>(k), w->uneven ? w->units * (k % 16U + 1U) : w->units);
}

void wave_task(void* user_data, u32 k)
{
    const auto* w = static_cast<const Work*>(user_data);
    const f64 up = k >= w->cols ? w->out[k - w->cols] : 1.0;
    const f64 left = k % w->cols != 0 ? w->out[k - 1] : 1.0;
    w->out[k] = spin(0.5 * (up + left), w->units);
}

f64 median(f64* times, int n)
{
    std::sort(times, times + n);
    return times[n / 2];
}

f64 seconds(Clock::time_point t0)
{
    return std::chrono::duration<f64>(Clock::now() - t0).count();
}

// Times `run(jobs, pick)` for one job and every arm on `workers`, gating each arm's `out` against one job's.
template <typename Run> void board(const char* label, Run run, f64* out, f64* control, u32 tasks, u32 workers)
{
    constexpr usize arm_count = sizeof(kArms) / sizeof(kArms[0]);
    f64 serial_times[kRounds];
    f64 times[arm_count][kRounds];
    bool live[arm_count];
    for (bool& l : live)
    {
        l = true;
    }
    for (int round = -1; round < kRounds; ++round)
    {
        Clock::time_point t0 = Clock::now();
        run(1U, SchedPick{});
        if (round >= 0)
        {
            serial_times[round] = seconds(t0);
        }
        std::memcpy(control, out, tasks * sizeof(f64));
        for (usize a = 0; a < arm_count; ++a)
        {
            if (!live[a])
            {
                continue;
            }
            std::memset(out, 0, tasks * sizeof(f64));
            t0 = Clock::now();
            run(workers, kArms[a].pick);
            if (round >= 0)
            {
                times[a][round] = seconds(t0);
            }
            if (std::memcmp(out, control, tasks * sizeof(f64)) != 0)
            {
                std::fprintf(stderr, "GATE MISS: %s arm %s: the outputs differ from one job's\n", label, kArms[a].name);
                g_failed = true;
                live[a] = false;
            }
        }
    }
    const f64 serial_s = median(serial_times, kRounds);
    std::printf("%s: one job %.4f ms\n", label, serial_s * 1e3);
    for (usize a = 0; a < arm_count; ++a)
    {
        if (live[a])
        {
            const f64 s = median(times[a], kRounds);
            std::printf("  %-18s %10.4f ms  %5.2fx one job\n", kArms[a].name, s * 1e3, s > 0.0 ? serial_s / s : 0.0);
        }
    }
}
} // namespace

int main(int argc, char** argv)
{
    u32 workers = 0;
    if (argc == 3 && std::strcmp(argv[1], "--threads") == 0)
    {
        workers = static_cast<u32>(std::strtoul(argv[2], nullptr, 10));
    }
    else if (argc != 1)
    {
        std::fprintf(stderr, "usage: bench_hesap_sched_parallel [--threads N]\n");
        return 2;
    }
    crd::jobs::Config cfg;
    if (workers != 0)
    {
        cfg.num_threads = workers;
    }
    crd::jobs::init(cfg);
    const u32 jobs = crd::jobs::num_workers();
    std::printf("[bench_hesap_sched_parallel] the schedulers' levers on %u jobs against one job, median of %d rounds\n",
                jobs, kRounds);
    {
        crd::memory::TlsfAllocator alloc(64U << 20U);
        constexpr u32 max_tasks = 65536;
        crd::containers::Array<f64> out(&alloc);
        crd::containers::Array<f64> control(&alloc);
        out.resize(max_tasks);
        control.resize(max_tasks);
        char label[96];
        const u32 task_counts[] = {16, 256, 4096, 65536};
        for (const bool uneven : {false, true})
        {
            for (const u32 units : {1U, 32U})
            {
                for (const u32 tasks : task_counts)
                {
                    Work w{out.data(), units, uneven, 0};
                    TaskGraph g(&alloc);
                    for (u32 k = 0; k < tasks; ++k)
                    {
                        g.add_task(batch_task, &w);
                    }
                    std::snprintf(label, sizeof(label), "TaskGraph %s units %u tasks %u", uneven ? "uneven" : "even",
                                  units, tasks);
                    board(
                        label, [&g](u32 n, const SchedPick& pick) { g.execute(n, pick); }, out.data(), control.data(),
                        tasks, jobs);
                }
            }
        }
        const u32 sides[] = {8, 32, 128};
        for (const u32 side : sides)
        {
            for (const u32 units : {1U, 32U})
            {
                Work w{out.data(), units, false, side};
                DependencyGraph g(&alloc);
                for (u32 k = 0; k < side * side; ++k)
                {
                    TileDep deps[3];
                    usize count = 0;
                    if (k >= side)
                    {
                        deps[count++] = {TileId{k - side}, TileAccess::Read};
                    }
                    if (k % side != 0)
                    {
                        deps[count++] = {TileId{k - 1}, TileAccess::Read};
                    }
                    deps[count++] = {TileId{k}, TileAccess::Write};
                    g.add_task(wave_task, &w, {deps, count});
                }
                std::snprintf(label, sizeof(label), "DependencyGraph wavefront %u x %u units %u", side, side, units);
                board(
                    label, [&g](u32 n, const SchedPick& pick) { g.execute(n, pick); }, out.data(), control.data(),
                    side * side, jobs);
            }
        }
    }
    crd::jobs::shutdown();
    if (g_failed)
    {
        std::fprintf(stderr, "bench_hesap_sched_parallel: a gate failed\n");
        return 1;
    }
    return 0;
}
