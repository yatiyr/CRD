// bench_hesap_amg_parallel — SaAmg's crd-jobs board (SIMD-DISPATCH, the folded HESAP-JOBS-RETUNE).
//
// One process per thread count: `--threads N` starts crd::jobs on N workers (default: the jobs default). For each
// generated problem (2D and 3D Poisson from 64K to 1M rows, a 2D convection-diffusion on the Petrov-Galerkin K-cycle
// path, 2D Poisson under Ruge-Stuben), f32 and f64, rounds interleave the arms and report the median:
//   - the finest level's build phases on one job (strength, aggregation, tentative prolongator, power iteration, the
//     smoothing product A T, the whole smoothed prolongator, the transpose, the Galerkin product R (A P)) with each
//     phase's share of their sum, so every kept-serial phase is on evidence; the three products also on N jobs;
//   - the whole build at the default pick (serial, the control) and each detail::AmgPick arm on N jobs, gated bit for
//     bit on every level's A, P, R and 1/diag(A) and the coarsest LU;
//   - one apply (the problem's cycle) at the default pick and on the CSR products' arm, gated bit for bit.
// A miss exits 1. The level line names the selected CPU level (CRD_CPU_LEVEL forces it); the products are baseline
// scalar code, so the level sweep measures the build and the rest of the process, not a kernel table.
//   bench_hesap_amg_parallel [--threads N]

#include <crd/containers/array.hpp>
#include <crd/core/cpu_features.hpp>
#include <crd/hesap/amg/aggregation.hpp>
#include <crd/hesap/amg/amg.hpp>
#include <crd/hesap/amg/detail/amg_arms.hpp>
#include <crd/hesap/amg/prolongator.hpp>
#include <crd/hesap/amg/strength.hpp>
#include <crd/hesap/sparse/convert.hpp>
#include <crd/hesap/sparse/spgemm_parallel.hpp>
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
using crd::hesap::amg::SaAmg;
using crd::hesap::amg::detail::AmgPick;
using Clock = std::chrono::steady_clock;
template <typename T> using Csr = crd::hesap::sparse::SparseMatrix<T, crd::hesap::sparse::SparseFormat::Csr>;

constexpr int kRounds = 5;
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

// ---- problems ----

enum class Shape : crd::u8
{
    Poisson2d, // 5-point, n = g^2
    Poisson3d, // 7-point, n = g^3
    ConvDiff2d // anisotropic upwind convection-diffusion with reaction, nonsymmetric
};

struct Problem
{
    const char* name;
    Shape shape;
    u32 g;
    bool ruge_stuben;
};

const Problem kProblems[] = {
    {"poisson2d-256", Shape::Poisson2d, 256, false},   {"poisson2d-512", Shape::Poisson2d, 512, false},
    {"poisson2d-1024", Shape::Poisson2d, 1024, false}, {"poisson3d-40", Shape::Poisson3d, 40, false},
    {"poisson3d-64", Shape::Poisson3d, 64, false},     {"poisson3d-100", Shape::Poisson3d, 100, false},
    {"convdiff2d-512", Shape::ConvDiff2d, 512, false}, {"poisson2d-512-rs", Shape::Poisson2d, 512, true},
};

template <typename T> Csr<T> build_problem(const Problem& pb, crd::memory::IAllocator* alloc)
{
    const u32 g = pb.g;
    const u32 g2 = pb.shape == Shape::Poisson3d ? g * g : 1;
    const u32 n = pb.shape == Shape::Poisson3d ? g * g * g : g * g;
    const f64 eps = pb.shape == Shape::ConvDiff2d ? 0.1 : 1.0;
    const f64 beta = pb.shape == Shape::ConvDiff2d ? 0.3 : 0.0;
    const f64 react = pb.shape == Shape::ConvDiff2d ? 0.01 : 0.0;
    crd::hesap::sparse::TripletBuilder<T> b(alloc, n, n);
    for (u32 r = 0; r < n; ++r)
    {
        const u32 j = r % g;
        const u32 i = (r / g) % g;
        f64 diag = pb.shape == Shape::Poisson3d ? 6.0 : 2.0 * eps + 2.0 + beta + react;
        if (j > 0)
        {
            b.add(r, r - 1, static_cast<T>(-eps - beta)); // west, upwind
        }
        if (j + 1 < g)
        {
            b.add(r, r + 1, static_cast<T>(-eps));
        }
        if (i > 0)
        {
            b.add(r, r - g, static_cast<T>(-1.0));
        }
        if (i + 1 < g)
        {
            b.add(r, r + g, static_cast<T>(-1.0));
        }
        if (pb.shape == Shape::Poisson3d)
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
        b.add(r, r, static_cast<T>(diag));
    }
    return b.compress();
}

template <typename T> typename SaAmg<T>::Options options_of(const Problem& pb)
{
    typename SaAmg<T>::Options o;
    if (pb.ruge_stuben)
    {
        o.coarsening = SaAmg<T>::Coarsening::RugeStuben;
    }
    if (pb.shape == Shape::ConvDiff2d)
    {
        o.cycle = SaAmg<T>::Cycle::K;
    }
    return o;
}

// ---- gates ----

template <typename U> bool same_bytes(const U* x, const U* y, usize n)
{
    return n == 0 || std::memcmp(x, y, n * sizeof(U)) == 0;
}

template <typename T> bool same_csr(const Csr<T>& x, const Csr<T>& y)
{
    return x.rows() == y.rows() && x.cols() == y.cols() && x.nnz() == y.nnz() &&
           same_bytes(x.pattern().outer_ptr.data(), y.pattern().outer_ptr.data(), x.pattern().outer_ptr.size()) &&
           same_bytes(x.pattern().inner_idx.data(), y.pattern().inner_idx.data(), x.nnz()) &&
           same_bytes(x.values().values.data(), y.values().values.data(), x.nnz());
}

template <typename T> bool same_hierarchy(const SaAmg<T>& x, const SaAmg<T>& y)
{
    if (x.num_levels() != y.num_levels() || x.coarse_size() != y.coarse_size())
    {
        return false;
    }
    for (u32 k = 0; k + 1 < x.num_levels(); ++k)
    {
        const auto& lx = x.level(k);
        const auto& ly = y.level(k);
        if (!same_csr(lx.a, ly.a) || !same_csr(lx.p, ly.p) || !same_csr(lx.r, ly.r) ||
            !same_bytes(lx.dinv.data(), ly.dinv.data(), lx.dinv.size()))
        {
            return false;
        }
    }
    const usize nc = x.coarse_size();
    return same_bytes(x.coarse_lu().packed().data(), y.coarse_lu().packed().data(), nc * nc);
}

// ---- the finest level's phases ----

enum Phase : u32
{
    kStrength,
    kAggregate,
    kTentative,
    kPower,
    kSmoothProduct,
    kProlongator,
    kTranspose,
    kGalerkin,
    kPhaseCount
};

const char* const kPhaseNames[kPhaseCount] = {"strength",    "aggregation", "tentative P", "power iteration",
                                              "A T product", "smoothed P",  "transpose",   "R (A P) product"};

template <typename T>
void phase_board(const char* label, const Problem& pb, const Csr<T>& a, u32 jobs, crd::memory::IAllocator* alloc)
{
    using R = crd::hesap::dense::RealType<T>;
    const AmgPick on_jobs{.spgemm = {.parallel_min_flop = 0, .workers = jobs}, .spmv_min_nnz = 0, .spmv_workers = jobs};
    const typename SaAmg<T>::Options opts{};
    const u32 n = a.rows();
    Array<T> dinv(alloc);
    dinv.resize(n);
    for (u32 i = 0; i < n; ++i)
    {
        for (u32 q = a.pattern().outer_ptr[i]; q < a.pattern().outer_ptr[i + 1]; ++q)
        {
            if (a.pattern().inner_idx[q] == i)
            {
                dinv[i] = T(R(1)) / a.values().values[q];
            }
        }
    }
    f64 serial[kPhaseCount][kRounds] = {};
    f64 parallel[kPhaseCount][kRounds] = {};
    const auto timed = [](f64& slot, auto&& fn)
    {
        const Clock::time_point t0 = Clock::now();
        fn();
        slot = seconds(t0);
    };
    for (int round = 0; round < kRounds; ++round)
    {
        u32 n_agg = 0;
        Csr<T> s(alloc);
        Array<u32> agg(alloc);
        Csr<T> tent(alloc);
        Csr<T> p(alloc);
        R rho = R(0);
        timed(serial[kStrength][round], [&] { s = crd::hesap::amg::strength_matrix<T>(a, opts.theta, alloc); });
        timed(serial[kAggregate][round], [&] { agg = crd::hesap::amg::aggregate<T>(s, n_agg, alloc); });
        timed(serial[kTentative][round], [&] { tent = crd::hesap::amg::tentative_prolongator<T>(agg, n_agg, alloc); });
        timed(serial[kPower][round], [&] { rho = crd::hesap::amg::estimate_drinv_a_radius<T>(a, dinv, alloc); });
        timed(parallel[kPower][round],
              [&] { (void)crd::hesap::amg::estimate_drinv_a_radius<T>(a, dinv, alloc, on_jobs); });
        timed(serial[kSmoothProduct][round], [&] { (void)crd::hesap::sparse::spgemm<T>(a, tent, alloc); });
        timed(parallel[kSmoothProduct][round],
              [&] { (void)crd::hesap::sparse::spgemm_arm<T>(on_jobs.spgemm, a, tent, alloc); });
        const R omega = rho > R(0) ? R(static_cast<R>(4.0 / 3.0)) / rho : R(static_cast<R>(2.0 / 3.0));
        timed(serial[kProlongator][round],
              [&] { p = crd::hesap::amg::smoothed_prolongator<T>(a, tent, dinv, omega, alloc); });
        Csr<T> r(alloc);
        timed(serial[kTranspose][round], [&] { r = crd::hesap::sparse::transpose<T>(p, alloc); });
        timed(serial[kGalerkin][round],
              [&]
              {
                  const Csr<T> ap = crd::hesap::sparse::spgemm<T>(a, p, alloc);
                  (void)crd::hesap::sparse::spgemm<T>(r, ap, alloc);
              });
        timed(parallel[kGalerkin][round],
              [&]
              {
                  const Csr<T> ap = crd::hesap::sparse::spgemm_arm<T>(on_jobs.spgemm, a, p, alloc);
                  (void)crd::hesap::sparse::spgemm_arm<T>(on_jobs.spgemm, r, ap, alloc);
              });
    }
    f64 total = 0.0;
    f64 med[kPhaseCount] = {};
    for (u32 ph = 0; ph < kPhaseCount; ++ph)
    {
        med[ph] = median(serial[ph], kRounds);
        total += ph == kSmoothProduct ? 0.0 : med[ph]; // A T is inside the smoothed prolongator
    }
    std::printf("%s %s n %u: the finest level's phases, one job (share of their sum) | %u jobs\n", label, pb.name, n,
                jobs);
    for (u32 ph = 0; ph < kPhaseCount; ++ph)
    {
        std::printf("  %-18s %10.3f ms (%5.1f%%)", kPhaseNames[ph], med[ph] * 1e3,
                    ph == kSmoothProduct ? 0.0 : 100.0 * med[ph] / total);
        if (ph == kPower || ph == kSmoothProduct || ph == kGalerkin)
        {
            const f64 par = median(parallel[ph], kRounds);
            std::printf(" | %10.3f ms %5.2fx", par * 1e3, par > 0.0 ? med[ph] / par : 0.0);
        }
        std::printf("\n");
    }
}

// ---- the whole build and the apply ----

struct BuildArm
{
    const char* name;
    AmgPick pick; // a workers field of 0 takes the board's N jobs
};

const BuildArm kBuildArms[] = {
    {"spgemm-on-jobs", {.spgemm = {.parallel_min_flop = 0}}},
    {"spmv-on-jobs", {.spmv_min_nnz = 0}},
    {"both-on-jobs", {.spgemm = {.parallel_min_flop = 0}, .spmv_min_nnz = 0}},
    {"finest-only", {.spgemm = {.parallel_min_flop = 1ULL << 20}, .spmv_min_nnz = usize{1} << 16}},
};

template <typename T> void apply_once(const SaAmg<T>& m, const Array<T>& r, Array<T>& z)
{
    const u32 n = static_cast<u32>(m.n_rows());
    (void)m.apply(crd::containers::ConstSpan<T>{r.data(), n}, crd::containers::Span<T>{z.data(), n});
}

template <typename T>
void build_board(const char* label, const Problem& pb, const Csr<T>& a, u32 jobs, crd::memory::IAllocator* alloc)
{
    constexpr usize arm_count = sizeof(kBuildArms) / sizeof(kBuildArms[0]);
    const typename SaAmg<T>::Options opts = options_of<T>(pb);
    const u32 n = a.rows();
    const SaAmg<T> control(a, alloc, opts);
    Array<T> r(alloc);
    Array<T> zc(alloc);
    Array<T> z(alloc);
    r.resize(n);
    zc.resize(n);
    z.resize(n);
    for (u32 i = 0; i < n; ++i)
    {
        r[i] = static_cast<T>(1.0 + 0.25 * static_cast<f64>(i % 5));
    }
    apply_once(control, r, zc);
    f64 serial_times[kRounds] = {};
    f64 times[arm_count][kRounds] = {};
    f64 apply_serial[kRounds] = {};
    f64 apply_par[kRounds] = {};
    bool live[arm_count] = {};
    std::fill(live, live + arm_count, true);
    bool apply_live = true;
    const SaAmg<T> apply_arm(a, alloc, opts, AmgPick{.spmv_min_nnz = 0, .spmv_workers = jobs});
    for (int round = -1; round < kRounds; ++round) // round -1 warms every path and gates it
    {
        Clock::time_point t0 = Clock::now();
        {
            const SaAmg<T> m(a, alloc, opts);
            if (round >= 0)
            {
                serial_times[round] = seconds(t0);
            }
        }
        for (usize i = 0; i < arm_count; ++i)
        {
            if (!live[i])
            {
                continue;
            }
            AmgPick pick = kBuildArms[i].pick;
            pick.spgemm.workers = jobs;
            pick.spmv_workers = jobs;
            t0 = Clock::now();
            const SaAmg<T> m(a, alloc, opts, pick);
            if (round >= 0)
            {
                times[i][round] = seconds(t0);
            }
            if (round == -1 && !same_hierarchy(control, m))
            {
                std::fprintf(stderr, "GATE MISS: %s %s build arm %s: the hierarchy differs from one job's\n", label,
                             pb.name, kBuildArms[i].name);
                g_failed = true;
                live[i] = false;
            }
        }
        t0 = Clock::now();
        apply_once(control, r, z);
        if (round >= 0)
        {
            apply_serial[round] = seconds(t0);
        }
        t0 = Clock::now();
        apply_once(apply_arm, r, z);
        if (round >= 0)
        {
            apply_par[round] = seconds(t0);
        }
        if (round == -1 && apply_live && !same_bytes(z.data(), zc.data(), n))
        {
            std::fprintf(stderr, "GATE MISS: %s %s apply on %u jobs: z differs from one job's\n", label, pb.name, jobs);
            g_failed = true;
            apply_live = false;
        }
    }
    const f64 base = median(serial_times, kRounds);
    std::printf("%s %s n %u: %u levels, build one job %.3f ms\n", label, pb.name, n, control.num_levels(), base * 1e3);
    for (usize i = 0; i < arm_count; ++i)
    {
        if (live[i])
        {
            const f64 s = median(times[i], kRounds);
            std::printf("  build %-16s %10.3f ms  %5.2fx one job\n", kBuildArms[i].name, s * 1e3,
                        s > 0.0 ? base / s : 0.0);
        }
    }
    const f64 ab = median(apply_serial, kRounds);
    const f64 ap = median(apply_par, kRounds);
    std::printf("  apply one job      %10.3f ms\n", ab * 1e3);
    if (apply_live)
    {
        std::printf("  apply spmv-on-jobs %10.3f ms  %5.2fx one job\n", ap * 1e3, ap > 0.0 ? ab / ap : 0.0);
    }
}

template <typename T> void run_type(const char* label, u32 jobs)
{
    for (const Problem& pb : kProblems)
    {
        crd::memory::GrowableTlsfAllocator alloc;
        const Csr<T> a = build_problem<T>(pb, &alloc);
        if (!pb.ruge_stuben)
        {
            phase_board<T>(label, pb, a, jobs, &alloc);
        }
        build_board<T>(label, pb, a, jobs, &alloc);
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
        std::fprintf(stderr, "usage: bench_hesap_amg_parallel [--threads N]\n");
        return 2;
    }
    crd::jobs::Config cfg;
    if (workers != 0)
    {
        cfg.num_threads = workers;
    }
    crd::jobs::init(cfg);
    const u32 jobs = crd::jobs::num_workers();
    std::printf("[bench_hesap_amg_parallel] SaAmg's levers on %u jobs against one job, median of %d rounds. "
                "selected level %s\n",
                jobs, kRounds, crd::cpu_level_name(crd::cpu_selection().selected));
    run_type<f32>("f32", jobs);
    run_type<f64>("f64", jobs);
    crd::jobs::shutdown();
    if (g_failed)
    {
        std::fprintf(stderr, "bench_hesap_amg_parallel: a gate failed\n");
        return 1;
    }
    return 0;
}
