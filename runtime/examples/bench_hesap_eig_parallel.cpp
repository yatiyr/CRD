// bench_hesap_eig_parallel — the symmetric eigensolvers' crd-jobs board (SIMD-DISPATCH, the folded HESAP-JOBS-RETUNE).
//
// One process per thread count: `--threads N` starts crd::jobs on N workers (default: the jobs default) and times, at
// each size of a random symmetric tridiagonal (the stage eigvals_sym and eig_sym_mrrr hand their reduction to), the
// one-job eigenvalues and eigenvectors (the controls) and their arms on N jobs, round by round in turn so the arms
// share the host's state; the median of the rounds is reported per size with its speedup over one job.
//   - eigenvalues: tridiag_multisection_arm at the production chunk count (one per job) and at 2, 4 and 8 chunks per
//     job (work stealing across the hybrid cores), gated bit for bit (every chunk descends one bisection tree,
//     ADR-0065 D(dense-eig)-MRRR-multisection-global-tree); dqds, the path below EigvalsPick::multisection_min, one
//     thread, gated by the declared bound |w - w_dqds| <= 4 n eps max(|gl|, |gu|) (test_mrrr_eigenvalues.cpp).
//   - eigenvectors: mrrr_compute_vectors on MrrrPick's production granularity and its gran_max and gran_div arms,
//     gated bit for bit (disjoint columns per segment).
// A miss exits 1. The level line names the selected CPU level (CRD_CPU_LEVEL forces it); both stages are baseline
// scalar code, so the level sweep measures the build and the rest of the process, not a kernel table.
//   bench_hesap_eig_parallel [--threads N]

#include <crd/containers/array.hpp>
#include <crd/core/cpu_features.hpp>
#include <crd/hesap/dense/detail/dqds.hpp>
#include <crd/hesap/dense/detail/eig_arms.hpp>
#include <crd/hesap/dense/detail/mrrr_vectors.hpp>
#include <crd/hesap/dense/detail/sturm_count.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace
{
using crd::f32;
using crd::f64;
using crd::u32;
using crd::usize;
using crd::containers::Array;
using crd::hesap::dense::detail::MrrrPick;
using crd::hesap::dense::detail::MultisectionPick;
using Clock = std::chrono::steady_clock;

constexpr int kRounds = 7;
constexpr f64 kBoundC = 4.0; // the declared bound's c (kEigvalsArmBoundC in test_mrrr_eigenvalues.cpp)
bool g_failed = false;

struct ValuesArm
{
    const char* name;
    u32 chunks_per_job; // 0: the production chunk count (one per job)
};

const ValuesArm kValuesArms[] = {
    {"production", 0},
    {"chunks-2-per-job", 2},
    {"chunks-4-per-job", 4},
    {"chunks-8-per-job", 8},
};

struct VectorsArm
{
    const char* name;
    MrrrPick pick;
};

const VectorsArm kVectorsArms[] = {
    {"production", MrrrPick{}},
    {"gran-max-1", MrrrPick{.gran_max = 1}},
    {"gran-max-16", MrrrPick{.gran_max = 16}},
    {"gran-div-64", MrrrPick{.gran_div = 64}},
    {"gran-div-1024", MrrrPick{.gran_div = 1024}},
};

f64 median(f64* times, int n)
{
    std::sort(times, times + n);
    return times[n / 2];
}

f64 seconds(Clock::time_point t0)
{
    return std::chrono::duration<f64>(Clock::now() - t0).count();
}

template <typename R> Array<R> sized(usize n, crd::memory::IAllocator* alloc)
{
    Array<R> a(alloc);
    a.resize(n);
    return a;
}

template <typename R> void fill_tridiag(R* d, R* e, int n, u32 seed)
{
    u32 s = seed;
    for (int i = 0; i < n; ++i)
    {
        s = s * 1664525U + 1013904223U;
        d[i] = static_cast<R>(2.0 * (static_cast<f64>(s >> 8) / 16777216.0) - 1.0);
    }
    for (int i = 0; i + 1 < n; ++i)
    {
        s = s * 1664525U + 1013904223U;
        e[i] = static_cast<R>(2.0 * (static_cast<f64>(s >> 8) / 16777216.0) - 1.0);
    }
}

// The dqds eigenvalues of (d, e), production's path below multisection_min, with its scratch.
template <typename R> struct Dqds
{
    Array<R> ework;
    Array<R> e2work;
    Array<int> isplit;
    Array<R> zwork;
    Array<R> q;
    Array<R> qe;

    Dqds(usize n, crd::memory::IAllocator* alloc)
        : ework(sized<R>(n, alloc)), e2work(sized<R>(n, alloc)), isplit(sized<int>(n, alloc)),
          zwork(sized<R>(4 * n + 8, alloc)), q(sized<R>(n + 2, alloc)), qe(sized<R>(n + 1, alloc))
    {
    }

    void run(const R* d, const R* e, int n, R* w)
    {
        crd::hesap::dense::detail::tridiag_eigenvalues_dqds<R>(d, e, n, ework.data(), e2work.data(), isplit.data(),
                                                               zwork.data(), q.data(), qe.data(), w,
                                                               static_cast<R>(4) * std::numeric_limits<R>::epsilon());
    }
};

template <typename R> bool same_bits(const Array<R>& x, const Array<R>& y)
{
    return std::memcmp(x.data(), y.data(), x.size() * sizeof(R)) == 0;
}

void report(const char* label, const char* stage, int n, f64* serial_times, int reps)
{
    std::printf("%s %s n %d: one job %.4f ms\n", label, stage, n, median(serial_times, kRounds) / reps * 1e3);
}

void report_arm(const char* name, f64* times, f64* serial_times, int reps)
{
    const f64 s = median(times, kRounds) / reps;
    const f64 serial_s = median(serial_times, kRounds) / reps;
    std::printf("  %-22s %10.4f ms  %5.2fx one job\n", name, s * 1e3, s > 0.0 ? serial_s / s : 0.0);
}

template <typename R>
void values_board(const char* label, const R* d, const R* e, int n, u32 jobs, Dqds<R>& dqds,
                  crd::memory::IAllocator* alloc)
{
    constexpr usize arm_count = sizeof(kValuesArms) / sizeof(kValuesArms[0]);
    const usize nu = static_cast<usize>(n);
    const int reps = static_cast<int>(std::clamp(5e7 / (50.0 * n * n), 1.0, 256.0));
    Array<R> control = sized<R>(nu, alloc);
    Array<R> out = sized<R>(nu, alloc);
    f64 serial_times[kRounds] = {};
    f64 dqds_times[kRounds] = {};
    f64 times[arm_count][kRounds] = {};
    bool live[arm_count] = {};
    std::fill(live, live + arm_count, true);
    bool dqds_live = true;
    R gl = R{0};
    R gu = R{0};
    crd::hesap::dense::detail::gershgorin_bounds(d, e, n, gl, gu);
    const f64 bound = kBoundC * n * static_cast<f64>(std::numeric_limits<R>::epsilon()) *
                      std::max(std::fabs(static_cast<f64>(gl)), std::fabs(static_cast<f64>(gu)));
    for (int round = -1; round < kRounds; ++round) // round -1 warms every path and gates it
    {
        Clock::time_point t0 = Clock::now();
        for (int r = 0; r < reps; ++r)
        {
            crd::hesap::dense::detail::tridiag_multisection_arm<R>(MultisectionPick{.workers = 1}, alloc, d, e, n,
                                                                   control.data());
        }
        if (round >= 0)
        {
            serial_times[round] = seconds(t0);
        }
        for (usize i = 0; i < arm_count; ++i)
        {
            if (!live[i])
            {
                continue;
            }
            const MultisectionPick pick{.chunks = kValuesArms[i].chunks_per_job * jobs, .workers = jobs};
            t0 = Clock::now();
            for (int r = 0; r < reps; ++r)
            {
                crd::hesap::dense::detail::tridiag_multisection_arm<R>(pick, alloc, d, e, n, out.data());
            }
            if (round >= 0)
            {
                times[i][round] = seconds(t0);
            }
            if (round == -1 && !same_bits(out, control))
            {
                std::fprintf(stderr, "GATE MISS: %s eigenvalues n %d arm %s: the values differ from one job's\n", label,
                             n, kValuesArms[i].name);
                g_failed = true;
                live[i] = false;
            }
        }
        if (!dqds_live)
        {
            continue;
        }
        t0 = Clock::now();
        for (int r = 0; r < reps; ++r)
        {
            dqds.run(d, e, n, out.data());
        }
        if (round >= 0)
        {
            dqds_times[round] = seconds(t0);
        }
        if (round == -1)
        {
            f64 worst = 0.0;
            for (usize k = 0; k < nu; ++k)
            {
                worst = std::max(worst, std::fabs(static_cast<f64>(out[k]) - static_cast<f64>(control[k])));
            }
            if (!(worst <= bound))
            {
                std::fprintf(stderr, "GATE MISS: %s eigenvalues n %d dqds: %.3e past the bound %.3e\n", label, n, worst,
                             bound);
                g_failed = true;
                dqds_live = false;
            }
        }
    }
    report(label, "eigenvalues", n, serial_times, reps);
    for (usize i = 0; i < arm_count; ++i)
    {
        if (live[i])
        {
            report_arm(kValuesArms[i].name, times[i], serial_times, reps);
        }
    }
    if (dqds_live)
    {
        report_arm("dqds-one-thread", dqds_times, serial_times, reps);
    }
}

template <typename R>
void vectors_board(const char* label, const R* d, const R* e, int n, u32 jobs, Dqds<R>& dqds,
                   crd::memory::IAllocator* alloc)
{
    constexpr usize arm_count = sizeof(kVectorsArms) / sizeof(kVectorsArms[0]);
    const usize nu = static_cast<usize>(n);
    const int reps = static_cast<int>(std::clamp(5e7 / (40.0 * n * n), 1.0, 256.0));
    Array<R> w = sized<R>(nu, alloc);
    dqds.run(d, e, n, w.data());
    Array<R> control = sized<R>(nu * nu, alloc);
    Array<R> out = sized<R>(nu * nu, alloc);
    f64 serial_times[kRounds] = {};
    f64 times[arm_count][kRounds] = {};
    bool live[arm_count] = {};
    std::fill(live, live + arm_count, true);
    for (int round = -1; round < kRounds; ++round) // round -1 warms every path and gates it
    {
        Clock::time_point t0 = Clock::now();
        for (int r = 0; r < reps; ++r)
        {
            crd::hesap::dense::detail::mrrr_compute_vectors<R>(alloc, n, d, e, w.data(), control.data(), n,
                                                               MrrrPick{.workers = 1});
        }
        if (round >= 0)
        {
            serial_times[round] = seconds(t0);
        }
        for (usize i = 0; i < arm_count; ++i)
        {
            if (!live[i])
            {
                continue;
            }
            MrrrPick pick = kVectorsArms[i].pick;
            pick.workers = jobs;
            t0 = Clock::now();
            for (int r = 0; r < reps; ++r)
            {
                crd::hesap::dense::detail::mrrr_compute_vectors<R>(alloc, n, d, e, w.data(), out.data(), n, pick);
            }
            if (round >= 0)
            {
                times[i][round] = seconds(t0);
            }
            if (round == -1 && !same_bits(out, control))
            {
                std::fprintf(stderr, "GATE MISS: %s eigenvectors n %d arm %s: the vectors differ from one job's\n",
                             label, n, kVectorsArms[i].name);
                g_failed = true;
                live[i] = false;
            }
        }
    }
    report(label, "eigenvectors", n, serial_times, reps);
    for (usize i = 0; i < arm_count; ++i)
    {
        if (live[i])
        {
            report_arm(kVectorsArms[i].name, times[i], serial_times, reps);
        }
    }
}

template <typename R> void run_type(const char* label, u32 jobs)
{
    const int sizes[] = {128, 256, 512, 1024, 2048, 4096, 8192};
    for (const int n : sizes)
    {
        crd::memory::TlsfAllocator alloc(1024ULL * 1024ULL * 1024ULL);
        const usize nu = static_cast<usize>(n);
        Array<R> d = sized<R>(nu, &alloc);
        Array<R> e = sized<R>(nu, &alloc);
        fill_tridiag(d.data(), e.data(), n, 1U + static_cast<u32>(n));
        Dqds<R> dqds(nu, &alloc);
        values_board<R>(label, d.data(), e.data(), n, jobs, dqds, &alloc);
        if (n <= 4096) // the n by n vectors twice: 256 MiB at f64 4096
        {
            vectors_board<R>(label, d.data(), e.data(), n, jobs, dqds, &alloc);
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
        std::fprintf(stderr, "usage: bench_hesap_eig_parallel [--threads N]\n");
        return 2;
    }
    crd::jobs::Config cfg;
    if (workers != 0)
    {
        cfg.num_threads = workers;
    }
    crd::jobs::init(cfg);
    const u32 jobs = crd::jobs::num_workers();
    std::printf("[bench_hesap_eig_parallel] the eigensolvers' levers on %u jobs against one job, median of %d rounds. "
                "selected level %s\n",
                jobs, kRounds, crd::cpu_level_name(crd::cpu_selection().selected));
    run_type<f32>("f32", jobs);
    run_type<f64>("f64", jobs);
    crd::jobs::shutdown();
    if (g_failed)
    {
        std::fprintf(stderr, "bench_hesap_eig_parallel: a gate failed\n");
        return 1;
    }
    return 0;
}
