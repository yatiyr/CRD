// bench_hesap_operator_parallel — the sparse LinearOps' crd-jobs board (SIMD-DISPATCH, the folded HESAP-JOBS-RETUNE).
//
// One process per thread count: `--threads N` starts crd::jobs on N workers (default: the jobs default). For each
// generated matrix (2D and 3D Laplacians and a uniform 16-per-row pattern, sized from a few KiB through the L2 and the
// L3 to DRAM), f32 and f64, rounds interleave the arms and report the median:
//   - one apply of each op (ParallelSparseLinearOp's A x, ParallelSpmvLeastSquaresOp's A x and A^H y on a tall A,
//     ParallelSpmmLinearOp's A X at s 4 and 8) on the serial kernel, on 2, 4, ... and N forced jobs and at production's
//     pick, with each op's measure against the cut (sparse_resident_bytes()), so the crossover is on evidence;
//   - a fixed-iteration CG, LSQR and block CG on the serial op, on N jobs and at production's pick, with the solve's
//     time outside the operator (the kept-serial Krylov reductions and updates).
// Every arm's output and every solve's iterations and solution are gated bit for bit against the serial op; a miss
// exits 1. The level line names the selected CPU level (CRD_CPU_LEVEL forces it), which picks the SELL bodies.
//   bench_hesap_operator_parallel [--threads N]

#include <crd/containers/array.hpp>
#include <crd/core/cpu_features.hpp>
#include <crd/hesap/iterative/block_cg.hpp>
#include <crd/hesap/iterative/cg.hpp>
#include <crd/hesap/iterative/lsqr.hpp>
#include <crd/hesap/sparse/block_linear_op.hpp>
#include <crd/hesap/sparse/detail/operator_arms.hpp>
#include <crd/hesap/sparse/parallel_sparse_linear_op.hpp>
#include <crd/hesap/sparse/triplet_builder.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/memory/allocators/growable_tlsf_allocator.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace
{
using crd::f32;
using crd::f64;
using crd::u32;
using crd::u64;
using crd::usize;
using crd::containers::Array;
using crd::hesap::sparse::ParallelSparseLinearOp;
using crd::hesap::sparse::ParallelSpmmLinearOp;
using crd::hesap::sparse::ParallelSpmvLeastSquaresOp;
using crd::hesap::sparse::detail::OperatorPick;
using Clock = std::chrono::steady_clock;
template <typename T> using Csr = crd::hesap::sparse::SparseMatrix<T, crd::hesap::sparse::SparseFormat::Csr>;
template <typename T> using Vec = crd::hesap::dense::Vector<T>;

constexpr int kRounds = 7;
constexpr usize kApplies = 20; // applies per timed sample
constexpr usize kIters = 40;   // iterations of each fixed-iteration solve
constexpr usize kNever = ~usize{0};
bool g_failed = false;

f64 median(f64* times, int n)
{
    std::sort(times, times + n);
    return times[n / 2];
}

f64 seconds(Clock::time_point t0)
{
    return std::chrono::duration<f64>(Clock::now() - t0).count();
}

template <typename U> bool same_bytes(const U* x, const U* y, usize n)
{
    return n == 0 || std::memcmp(x, y, n * sizeof(U)) == 0;
}

// ---- matrices ----

enum class Shape : crd::u8
{
    Laplace2d, // 5-point, n = g^2
    Laplace3d, // 7-point, n = g^3
    Uniform16  // 16 random columns per row, n = g
};

struct Problem
{
    const char* name;
    Shape shape;
    u32 g;
};

const Problem kProblems[] = {
    {"laplace2d-16", Shape::Laplace2d, 16},       {"laplace2d-64", Shape::Laplace2d, 64},
    {"laplace2d-128", Shape::Laplace2d, 128},     {"laplace2d-256", Shape::Laplace2d, 256},
    {"laplace2d-512", Shape::Laplace2d, 512},     {"laplace2d-1024", Shape::Laplace2d, 1024},
    {"laplace3d-16", Shape::Laplace3d, 16},       {"laplace3d-32", Shape::Laplace3d, 32},
    {"laplace3d-64", Shape::Laplace3d, 64},       {"laplace3d-100", Shape::Laplace3d, 100},
    {"uniform16-2k", Shape::Uniform16, 2048},     {"uniform16-32k", Shape::Uniform16, 32768},
    {"uniform16-256k", Shape::Uniform16, 262144},
};

// The square SPD matrix of the problem, or (tall) its rows stacked over a diagonal of the first half: a full-column
// rank least-squares A of 1.5 n rows.
template <typename T> Csr<T> build_problem(const Problem& pb, bool tall, crd::memory::IAllocator* alloc)
{
    const u32 g = pb.g;
    u32 n = g;
    if (pb.shape == Shape::Laplace2d)
    {
        n = g * g;
    }
    else if (pb.shape == Shape::Laplace3d)
    {
        n = g * g * g;
    }
    const u32 rows = tall ? n + n / 2 : n;
    crd::hesap::sparse::TripletBuilder<T> b(alloc, rows, n);
    u64 s = 0x9E3779B97F4A7C15ULL ^ n;
    for (u32 r = 0; r < n; ++r)
    {
        if (pb.shape == Shape::Uniform16)
        {
            for (u32 k = 0; k < 16; ++k)
            {
                s = s * 6364136223846793005ULL + 1442695040888963407ULL;
                const u32 c = static_cast<u32>(s >> 33) % n;
                b.add(r, c, static_cast<T>(-1.0 / 32.0));
                b.add(c, r, static_cast<T>(-1.0 / 32.0)); // symmetric, diagonally dominant with the diagonal below
            }
            b.add(r, r, static_cast<T>(2.0));
            continue;
        }
        const u32 gx = g;
        const u32 g2 = g * g;
        const u32 j = r % gx;
        const u32 i = (r / gx) % gx;
        b.add(r, r, static_cast<T>(pb.shape == Shape::Laplace3d ? 6.0 : 4.0));
        if (j > 0)
        {
            b.add(r, r - 1, static_cast<T>(-1.0));
        }
        if (j + 1 < gx)
        {
            b.add(r, r + 1, static_cast<T>(-1.0));
        }
        if (i > 0)
        {
            b.add(r, r - gx, static_cast<T>(-1.0));
        }
        if (i + 1 < gx)
        {
            b.add(r, r + gx, static_cast<T>(-1.0));
        }
        if (pb.shape == Shape::Laplace3d)
        {
            const u32 k = r / g2;
            if (k > 0)
            {
                b.add(r, r - g2, static_cast<T>(-1.0));
            }
            if (k + 1 < g)
            {
                b.add(r, r + g2, static_cast<T>(-1.0));
            }
        }
    }
    for (u32 r = n; r < rows; ++r)
    {
        b.add(r, r - n, static_cast<T>(1.0));
    }
    return b.compress();
}

template <typename T> Vec<T> ramp(crd::memory::IAllocator* alloc, usize n)
{
    Vec<T> v(alloc, n);
    for (usize i = 0; i < n; ++i)
    {
        v(i) = static_cast<T>(1.0 + 0.125 * static_cast<f64>(i % 9));
    }
    return v;
}

// The forced job counts: 2, 4, ... below N, then N.
u32 job_arms(u32 jobs, u32* out)
{
    u32 count = 0;
    for (u32 j = 2; j < jobs; j *= 2)
    {
        out[count++] = j;
    }
    out[count++] = jobs;
    return count;
}

constexpr u32 kMaxArms = 16;

// One op's apply board: `apply(op, out)` runs kApplies applies into out; arms: serial, each forced job count,
// production.
template <typename T, typename Op, typename Make, typename Apply>
void apply_board(const char* label, const char* what, const char* pb, usize measure, usize out_n, u32 jobs,
                 const Make& make, const Apply& apply, crd::memory::IAllocator* alloc)
{
    u32 counts[kMaxArms] = {};
    const u32 forced = job_arms(jobs, counts);
    const u32 arms = forced + 2; // serial first, production last
    Array<T> want(alloc);
    Array<T> got(alloc);
    want.resize(out_n);
    got.resize(out_n);
    f64 times[kMaxArms + 2][kRounds] = {};
    bool live[kMaxArms + 2] = {};
    std::fill(live, live + arms, true);
    bool production_parallel = false;
    for (int round = -1; round < kRounds; ++round) // round -1 warms every arm and gates it
    {
        for (u32 a = 0; a < arms; ++a)
        {
            if (!live[a])
            {
                continue;
            }
            OperatorPick pick{}; // production, the last arm
            if (a == 0)
            {
                pick.parallel_min_bytes = kNever;
            }
            else if (a + 1 < arms)
            {
                pick = OperatorPick{.parallel_min_bytes = 0, .workers = counts[a - 1]};
            }
            const Op op = make(pick);
            if (a + 1 == arms)
            {
                production_parallel = op.is_parallel();
            }
            Array<T>& out = a == 0 ? want : got;
            const Clock::time_point t0 = Clock::now();
            apply(op, out);
            if (round >= 0)
            {
                times[a][round] = seconds(t0) / static_cast<f64>(kApplies);
            }
            if (round == -1 && a > 0 && !same_bytes(want.data(), got.data(), out_n))
            {
                std::fprintf(stderr, "GATE MISS: %s %s %s arm %u: the output differs from the serial op's\n", label, pb,
                             what, a);
                g_failed = true;
                live[a] = false;
            }
        }
    }
    const f64 base = median(times[0], kRounds);
    std::printf("%s %s %s: measure %zu B, cut %zu B (production %s)\n  serial          %10.3f us\n", label, pb, what,
                measure, crd::hesap::sparse::sparse_resident_bytes(), production_parallel ? "parallel" : "serial",
                base * 1e6);
    for (u32 a = 1; a < arms; ++a)
    {
        if (!live[a])
        {
            continue;
        }
        const f64 t = median(times[a], kRounds);
        if (a + 1 == arms)
        {
            std::printf("  production      %10.3f us  %5.2fx serial\n", t * 1e6, t > 0.0 ? base / t : 0.0);
        }
        else
        {
            std::printf("  %3u jobs        %10.3f us  %5.2fx serial\n", counts[a - 1], t * 1e6,
                        t > 0.0 ? base / t : 0.0);
        }
    }
}

// One fixed-iteration solve on the serial op, on N jobs and at production's pick: `solve(op, x)` returns the
// iterations; `apply_time(op)` is one apply's seconds, so the time outside the operator is the rest.
template <typename T, typename Op, typename Make, typename Solve, typename ApplyTime>
void solve_board(const char* label, const char* what, const char* pb, usize x_n, u32 jobs, const Make& make,
                 const Solve& solve, const ApplyTime& apply_time, crd::memory::IAllocator* alloc)
{
    const OperatorPick picks[3] = {OperatorPick{.parallel_min_bytes = kNever},
                                   OperatorPick{.parallel_min_bytes = 0, .workers = jobs}, OperatorPick{}};
    const char* names[3] = {"serial", "N jobs", "production"};
    Vec<T> want(alloc, x_n);
    Vec<T> got(alloc, x_n);
    f64 times[3][kRounds] = {};
    f64 outside[3][kRounds] = {};
    bool live[3] = {true, true, true};
    usize iters_want = 0;
    for (int round = -1; round < kRounds; ++round)
    {
        for (u32 a = 0; a < 3; ++a)
        {
            if (!live[a])
            {
                continue;
            }
            const Op op = make(picks[a]);
            Vec<T>& x = a == 0 ? want : got;
            for (usize i = 0; i < x_n; ++i)
            {
                x(i) = T(0);
            }
            const f64 per_apply = apply_time(op);
            const Clock::time_point t0 = Clock::now();
            const usize iters = solve(op, x);
            const f64 t = seconds(t0);
            if (round >= 0)
            {
                times[a][round] = t;
                outside[a][round] = t - per_apply * static_cast<f64>(iters);
            }
            if (round == -1)
            {
                if (a == 0)
                {
                    iters_want = iters;
                }
                else if (iters != iters_want || !same_bytes(want.data(), got.data(), x_n))
                {
                    std::fprintf(stderr, "GATE MISS: %s %s %s %s: the solve differs from the serial op's\n", label, pb,
                                 what, names[a]);
                    g_failed = true;
                    live[a] = false;
                }
            }
        }
    }
    std::printf("%s %s %s, %zu iterations\n", label, pb, what, iters_want);
    const f64 base = median(times[0], kRounds);
    for (u32 a = 0; a < 3; ++a)
    {
        if (live[a])
        {
            const f64 t = median(times[a], kRounds);
            const f64 o = median(outside[a], kRounds);
            std::printf("  %-10s %10.3f ms  %5.2fx serial, outside the operator %10.3f ms (%4.1f%%)\n", names[a],
                        t * 1e3, t > 0.0 ? base / t : 0.0, o * 1e3, t > 0.0 ? 100.0 * o / t : 0.0);
        }
    }
}

template <typename T> f64 time_apply(const crd::hesap::LinearOp<T>& op, Vec<T>& x, Vec<T>& y)
{
    const Clock::time_point t0 = Clock::now();
    for (usize k = 0; k < kApplies; ++k)
    {
        (void)op.apply(x.span(), y.span());
    }
    return seconds(t0) / static_cast<f64>(kApplies);
}

template <typename T> void run_problem(const char* label, const Problem& pb, u32 jobs)
{
    using R = crd::hesap::dense::RealType<T>;
    crd::memory::GrowableTlsfAllocator alloc;
    const Csr<T> a = build_problem<T>(pb, false, &alloc);
    const Csr<T> tall = build_problem<T>(pb, true, &alloc);
    const u32 n = a.rows();
    const u32 m = tall.rows();
    Vec<T> x = ramp<T>(&alloc, n);
    Vec<T> xm = ramp<T>(&alloc, m);
    Vec<T> y(&alloc, m);
    Vec<T> yn(&alloc, n);
    crd::hesap::iterative::IterativeOptions<R> opts;
    opts.rel_tol = R(0);
    opts.max_iter = kIters;

    // The square op.
    {
        const usize measure = crd::hesap::sparse::to_sell<T>(a, &alloc).stored_bytes();
        auto make = [&](const OperatorPick& p)
        {
            return ParallelSparseLinearOp<T>(a, &alloc, p);
        };
        auto apply = [&](const ParallelSparseLinearOp<T>& op, Array<T>& out)
        {
            for (usize k = 0; k < kApplies; ++k)
            {
                (void)op.apply(x.span(), crd::containers::Span<T>{out.data(), n});
            }
        };
        apply_board<T, ParallelSparseLinearOp<T>>(label, "A x", pb.name, measure, n, jobs, make, apply, &alloc);
        Vec<T> b = ramp<T>(&alloc, n);
        crd::hesap::iterative::KrylovWorkspace<T> ws(&alloc, n);
        auto solve = [&](const ParallelSparseLinearOp<T>& op, Vec<T>& sol)
        {
            return crd::hesap::iterative::cg<T>(op, b.span(), sol.span(), opts, ws, &alloc).iterations;
        };
        auto at = [&](const ParallelSparseLinearOp<T>& op)
        {
            return time_apply<T>(op, x, yn);
        };
        solve_board<T, ParallelSparseLinearOp<T>>(label, "CG", pb.name, n, jobs, make, solve, at, &alloc);
    }
    // The least-squares op on the tall A: A x and A^H y, then LSQR.
    {
        const usize sa = crd::hesap::sparse::to_sell<T>(tall, &alloc).stored_bytes();
        const usize sh =
            crd::hesap::sparse::to_sell<T>(crd::hesap::sparse::transpose<T>(tall, &alloc), &alloc).stored_bytes();
        const usize measure = sa > sh ? sa : sh;
        auto make = [&](const OperatorPick& p)
        {
            return ParallelSpmvLeastSquaresOp<T>(tall, &alloc, p);
        };
        auto apply = [&](const ParallelSpmvLeastSquaresOp<T>& op, Array<T>& out)
        {
            for (usize k = 0; k < kApplies; ++k)
            {
                (void)op.apply(x.span(), crd::containers::Span<T>{out.data(), m});
                (void)op.apply_adjoint(xm.span(), crd::containers::Span<T>{out.data() + m, n});
            }
        };
        apply_board<T, ParallelSpmvLeastSquaresOp<T>>(label, "A x + A^H y", pb.name, measure, usize{m} + n, jobs, make,
                                                      apply, &alloc);
        Vec<T> b = ramp<T>(&alloc, m);
        crd::hesap::iterative::LsqrWorkspace<T> ws(&alloc, m, n);
        auto solve = [&](const ParallelSpmvLeastSquaresOp<T>& op, Vec<T>& sol)
        {
            return crd::hesap::iterative::lsqr<T>(op, b.span(), sol.span(), opts, ws, &alloc).iterations;
        };
        auto at = [&](const ParallelSpmvLeastSquaresOp<T>& op)
        {
            const Clock::time_point t0 = Clock::now();
            for (usize k = 0; k < kApplies; ++k)
            {
                (void)op.apply(x.span(), y.span());
                (void)op.apply_adjoint(xm.span(), yn.span());
            }
            return seconds(t0) / static_cast<f64>(kApplies);
        };
        solve_board<T, ParallelSpmvLeastSquaresOp<T>>(label, "LSQR", pb.name, n, jobs, make, solve, at, &alloc);
    }
    // The block op at s 4 and 8: A X, then block CG.
    for (const u32 s : {4U, 8U})
    {
        const usize ns = static_cast<usize>(n) * s;
        Vec<T> xb = ramp<T>(&alloc, ns);
        Vec<T> yb(&alloc, ns);
        char what[32];
        std::snprintf(what, sizeof(what), "A X, s %u", s);
        auto make = [&](const OperatorPick& p)
        {
            return ParallelSpmmLinearOp<T>(a, p);
        };
        auto apply = [&](const ParallelSpmmLinearOp<T>& op, Array<T>& out)
        {
            for (usize k = 0; k < kApplies; ++k)
            {
                (void)op.apply_block(xb.span(), s, crd::containers::Span<T>{out.data(), ns}, s, s);
            }
        };
        apply_board<T, ParallelSpmmLinearOp<T>>(label, what, pb.name, a.nnz() * sizeof(T), ns, jobs, make, apply,
                                                &alloc);
        crd::hesap::iterative::BlockCgWorkspace<T> ws(&alloc, n, s);
        auto solve = [&](const ParallelSpmmLinearOp<T>& op, Vec<T>& sol)
        {
            return crd::hesap::iterative::block_cg<T>(op, xb.span(), sol.span(), opts, ws, &alloc).iterations;
        };
        auto at = [&](const ParallelSpmmLinearOp<T>& op)
        {
            const Clock::time_point t0 = Clock::now();
            for (usize k = 0; k < kApplies; ++k)
            {
                (void)op.apply_block(xb.span(), s, yb.span(), s, s);
            }
            return seconds(t0) / static_cast<f64>(kApplies);
        };
        std::snprintf(what, sizeof(what), "block CG, s %u", s);
        solve_board<T, ParallelSpmmLinearOp<T>>(label, what, pb.name, ns, jobs, make, solve, at, &alloc);
    }
}

template <typename T> void run_type(const char* label, u32 jobs)
{
    for (const Problem& pb : kProblems)
    {
        run_problem<T>(label, pb, jobs);
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
        std::fprintf(stderr, "usage: bench_hesap_operator_parallel [--threads N]\n");
        return 2;
    }
    crd::jobs::Config cfg;
    if (workers != 0)
    {
        cfg.num_threads = workers;
    }
    crd::jobs::init(cfg);
    const u32 jobs = crd::jobs::num_workers();
    std::printf("[bench_hesap_operator_parallel] the sparse LinearOps' picks on up to %u jobs against the serial op, "
                "median of %d rounds. selected level %s\n",
                jobs, kRounds, crd::cpu_level_name(crd::cpu_selection().selected));
    run_type<f32>("f32", jobs);
    run_type<f64>("f64", jobs);
    crd::jobs::shutdown();
    if (g_failed)
    {
        std::fprintf(stderr, "bench_hesap_operator_parallel: a gate failed\n");
        return 1;
    }
    return 0;
}
