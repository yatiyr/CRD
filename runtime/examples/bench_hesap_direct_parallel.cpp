// bench_hesap_direct_parallel — the sparse direct factors' crd-jobs board (SIMD-DISPATCH, the folded
// HESAP-JOBS-RETUNE).
//
// One process per thread count: `--threads N` starts crd::jobs on N workers (default: the jobs default) and times,
// per matrix and type, the supernodal Cholesky's numeric refactor on one job (the control) and factorize_arm on N
// jobs down each arm of detail::SupernodalPick, then the production factor's solve on 1 and 16 right-hand sides on
// one job and down solve_arm's arms, round by round in turn so the arms share the host's state; the median of the
// rounds is reported with its speedup over one job. The scheduling arms are gated bit for bit against the control
// (the lower trapezoid of every panel, and x); the blocking arms (the cdiv's scalar cut and panels, the solve's
// batch width) by the normwise backward error of a solve, |b - A x| <= 4 n eps (|A| |x| + |b|), the bound the arm
// tests declare. A miss exits 1. Then the two LU factors' numeric factor, one job against factorize_arm on N jobs
// down each arm of detail::SupernodalLuPick and MultifrontalLuPick: the scheduling arms gated bit for bit (L and
// U), the blocking arms (the inline foot update, the panel widths) by the same bound on the raw factor's solve
// (apply_inverse, no refinement); the LU solves run on the calling thread (kept serial), so they are not timed
// here. The matrices are generated: 3D and 2D Laplacians under AMD and a bordered block matrix whose dense border
// is one fat root front, and for the LUs unsymmetric convection-diffusion stencils under AMD. The level line names
// the direct and dense kernels' level (CRD_CPU_LEVEL forces it).
//   bench_hesap_direct_parallel [--threads N]

#include <crd/containers/array.hpp>
#include <crd/core/cpu_features.hpp>
#include <crd/hesap/dense/dense_dispatch.hpp>
#include <crd/hesap/direct/detail/direct_picks.hpp>
#include <crd/hesap/direct/direct_dispatch.hpp>
#include <crd/hesap/direct/multifrontal_lu.hpp>
#include <crd/hesap/direct/supernodal_cholesky.hpp>
#include <crd/hesap/direct/supernodal_lu.hpp>
#include <crd/hesap/ordering/amd.hpp>
#include <crd/hesap/sparse/triplet_builder.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/memory/allocators/growable_tlsf_allocator.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <limits>

namespace
{
using crd::f32;
using crd::f64;
using crd::u32;
using crd::u64;
using crd::usize;
using crd::hesap::direct::MultifrontalLU;
using crd::hesap::direct::SupernodalCholesky;
using crd::hesap::direct::SupernodalLU;
using crd::hesap::direct::detail::MultifrontalLuPick;
using crd::hesap::direct::detail::SupernodalLuPick;
using crd::hesap::direct::detail::SupernodalPick;
namespace sp = crd::hesap::sparse;
using Clock = std::chrono::steady_clock;
template <typename T> using Csr = sp::SparseMatrix<T, sp::SparseFormat::Csr>;

constexpr int kRounds = 5;
constexpr u32 kNeverWidth = 0xFFFFFFFFU;
constexpr u64 kNeverWork = ~u64{0};
constexpr usize kManyRhs = 16;
bool g_failed = false;

struct Arm
{
    const char* name;
    SupernodalPick pick;
    bool same_bits; // a scheduling arm: gated bit for bit, otherwise by the backward-error bound
};

const Arm kFactorArms[] = {
    {"production", {}, true},
    {"node-parallel-all", {.node_min_cols = 0}, true},
    {"node-parallel-never", {.node_min_cols = kNeverWidth}, true},
    {"node-split-all", {.node_min_cols = 0, .gemm_min_flop = 0}, true},
    {"node-split-never", {.node_min_cols = 0, .gemm_min_flop = kNeverWork}, true},
    {"gemm-split-16M", {.gemm_min_flop = 16ULL << 20}, true},
    {"syrk-all", {.cmod_syrk_min = 0}, true},
    {"syrk-never", {.cmod_syrk_min = kNeverWidth}, true},
    {"cdiv-blocked-all", {.cdiv_scalar_max = 0}, false},
    {"cdiv-scalar-to-64", {.cdiv_scalar_max = 64}, false},
    {"cdiv-block-128", {.cdiv_block = 128}, false},
    {"cdiv-block-512", {.cdiv_block = 512}, false},
    {"cdiv-inner-32", {.cdiv_inner = 32}, false},
    {"cdiv-inner-128", {.cdiv_inner = 128}, false},
};

// The arm with on_jobs forces the solve onto the bench's jobs; the others run their pick as written.
struct SolveArm
{
    const char* name;
    SupernodalPick pick;
    bool same_bits;
    bool on_jobs;
};

const SolveArm kSolveArms[] = {
    {"production", {}, true, false},
    {"forced-jobs", {}, true, true},
    {"gate-off-cap-8", {.solve_min_work = 0}, true, false},
    {"gate-off-uncapped", {.solve_min_work = 0, .solve_max_workers = kNeverWidth}, true, false},
    {"batch-all", {.solve_batch_min_nc = 0}, false, false},
    {"batch-never", {.solve_batch_min_nc = kNeverWidth}, false, false},
};

template <typename Pick> struct LuArm
{
    const char* name;
    Pick pick;
    bool same_bits;
};

const LuArm<SupernodalLuPick> kSupernodalLuArms[] = {
    {"production", {}, true},
    {"foot-gemm-all", {.cmod_gemm_min_flop = 0}, false},
    {"foot-inline-all", {.cmod_gemm_min_flop = kNeverWork}, false},
    {"foot-inline-to-4M", {.cmod_gemm_min_flop = 4ULL << 20}, false},
    {"diag-block-32", {.diag_block = 32}, false},
    {"diag-block-96", {.diag_block = 96}, false},
};

const LuArm<MultifrontalLuPick> kMultifrontalLuArms[] = {
    {"production", {}, true},
    {"walk-serial", {.small_fill = kNeverWork}, true},
    {"walk-levels-all", {.small_fill = 0}, true},
    {"front-parallel-all", {.front_par_min = 1}, true},
    {"front-parallel-never", {.front_par_min = kNeverWidth}, true},
    {"front-parallel-from-2", {.front_par_min = 2}, true},
    {"zero-fill-never", {.zero_fill_min = kNeverWidth}, true},
    {"extend-add-all", {.extend_add_min = 0}, true},
    {"extend-add-never", {.extend_add_min = kNeverWidth}, true},
    {"lookahead-off", {.front = {.lookahead = false}}, true},
    {"trsm-split-all", {.front = {.trsm_min = 0}}, true},
    {"trsm-split-never", {.front = {.trsm_min = kNeverWidth}}, true},
    {"schur-split-all", {.front = {.schur_min_mnk = 0}}, true},
    {"schur-split-4M", {.front = {.schur_min_mnk = 4ULL << 20}}, true},
    {"panel-64", {.front = {.panel = 64}}, false},
    {"panel-192", {.front = {.panel = 192}}, false},
    {"panel-256", {.front = {.panel = 256}}, false},
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

// The 7-point (3D, nz > 1) or 5-point Laplacian on an nx x ny x nz grid, diagonal 2 d + 1 for d dimensions.
Csr<f64> laplacian(crd::memory::IAllocator* a, u32 nx, u32 ny, u32 nz)
{
    const u32 n = nx * ny * nz;
    sp::TripletBuilder<f64> tb(a, n, n);
    const f64 diag = nz > 1 ? 7.0 : 5.0;
    for (u32 z = 0; z < nz; ++z)
    {
        for (u32 y = 0; y < ny; ++y)
        {
            for (u32 x = 0; x < nx; ++x)
            {
                const u32 v = (z * ny + y) * nx + x;
                tb.add(v, v, diag);
                if (x > 0)
                {
                    tb.add(v, v - 1, -1.0);
                    tb.add(v - 1, v, -1.0);
                }
                if (y > 0)
                {
                    tb.add(v, v - nx, -1.0);
                    tb.add(v - nx, v, -1.0);
                }
                if (z > 0)
                {
                    tb.add(v, v - nx * ny, -1.0);
                    tb.add(v - nx * ny, v, -1.0);
                }
            }
        }
    }
    return tb.compress();
}

// The 7-point (3D, nz > 1) or 5-point convection-diffusion stencil on an nx x ny x nz grid: each axis' forward
// neighbour -1.3, its backward one -0.7, the diagonal 2 d + 1 for d dimensions, so it is diagonally dominant and
// unsymmetric in value on a symmetric pattern.
Csr<f64> convection(crd::memory::IAllocator* a, u32 nx, u32 ny, u32 nz)
{
    const u32 n = nx * ny * nz;
    sp::TripletBuilder<f64> tb(a, n, n);
    const u32 strides[] = {1U, nx, nx * ny};
    const u32 extents[] = {nx, ny, nz};
    for (u32 v = 0; v < n; ++v)
    {
        tb.add(v, v, nz > 1 ? 7.0 : 5.0);
        u32 rest = v;
        for (u32 d = 0; d < 3; ++d)
        {
            const u32 at = rest % extents[d];
            rest /= extents[d];
            if (at + 1 < extents[d])
            {
                tb.add(v, v + strides[d], -1.3);
                tb.add(v + strides[d], v, -0.7);
            }
        }
    }
    return tb.compress();
}

// nb dense bs x bs blocks and a dense nbord-wide border coupled to every block: the border is one fat root front
// that receives every block's update (the node-parallel schedule's matrix).
Csr<f64> bordered(crd::memory::IAllocator* a, u32 nb, u32 bs, u32 nbord)
{
    const u32 n = nb * bs + nbord;
    const u32 b0 = nb * bs;
    sp::TripletBuilder<f64> tb(a, n, n);
    for (u32 b = 0; b < nb; ++b)
    {
        for (u32 i = 0; i < bs; ++i)
        {
            for (u32 j = 0; j < bs; ++j)
            {
                tb.add(b * bs + i, b * bs + j, i == j ? static_cast<f64>(n + 1) : 1.0);
            }
        }
    }
    for (u32 i = b0; i < n; ++i)
    {
        for (u32 j = b0; j < n; ++j)
        {
            tb.add(i, j, i == j ? static_cast<f64>(n + 1) : 1.0);
        }
        for (u32 j = 0; j < b0; ++j)
        {
            tb.add(i, j, 1.0);
            tb.add(j, i, 1.0);
        }
    }
    return tb.compress();
}

// PAP^T under AMD, in T.
template <typename T> Csr<T> ordered(const Csr<f64>& m, crd::memory::IAllocator* a)
{
    const auto perm = crd::hesap::ordering::amd_order(m.pattern(), a);
    const auto& pat = m.pattern();
    const auto& vals = m.values().values;
    sp::TripletBuilder<T> tb(a, m.rows(), m.rows());
    for (u32 i = 0; i < m.rows(); ++i)
    {
        for (u32 q = pat.outer_ptr[i]; q < pat.outer_ptr[i + 1]; ++q)
        {
            tb.add(perm.inv_perm[i], perm.inv_perm[pat.inner_idx[q]], static_cast<T>(vals[q]));
        }
    }
    return tb.compress();
}

template <typename T> void apply(const Csr<T>& a, const T* x, T* b)
{
    const auto& pat = a.pattern();
    const auto& vals = a.values().values;
    for (u32 i = 0; i < a.rows(); ++i)
    {
        T acc{};
        for (u32 q = pat.outer_ptr[i]; q < pat.outer_ptr[i + 1]; ++q)
        {
            acc += vals[q] * x[pat.inner_idx[q]];
        }
        b[i] = acc;
    }
}

// Every column of x within |b - A x| <= 4 n eps (|A| |x| + |b|) in the infinity norm.
template <typename T>
bool within_bound(const Csr<T>& a, const T* x, const T* b, usize nrhs, crd::memory::IAllocator* alloc)
{
    const u32 n = a.rows();
    const auto& pat = a.pattern();
    const auto& vals = a.values().values;
    f64 an = 0.0;
    for (u32 i = 0; i < n; ++i)
    {
        f64 row = 0.0;
        for (u32 q = pat.outer_ptr[i]; q < pat.outer_ptr[i + 1]; ++q)
        {
            row += std::fabs(static_cast<f64>(vals[q]));
        }
        an = std::max(an, row);
    }
    const f64 tol = 4.0 * static_cast<f64>(n) * static_cast<f64>(std::numeric_limits<T>::epsilon());
    crd::containers::Array<T> ax(alloc);
    ax.resize(n);
    for (usize c = 0; c < nrhs; ++c)
    {
        const T* xc = x + c * n;
        const T* bc = b + c * n;
        apply(a, xc, ax.data());
        f64 r = 0.0;
        f64 xn = 0.0;
        f64 bn = 0.0;
        for (u32 i = 0; i < n; ++i)
        {
            r = std::max(r, std::fabs(static_cast<f64>(bc[i]) - static_cast<f64>(ax[i])));
            xn = std::max(xn, std::fabs(static_cast<f64>(xc[i])));
            bn = std::max(bn, std::fabs(static_cast<f64>(bc[i])));
        }
        if (!(r <= tol * (an * xn + bn)))
        {
            return false;
        }
    }
    return true;
}

// info() and the lower trapezoid of every panel, bit for bit.
template <typename T> bool same_factor(const SupernodalCholesky<T>& x, const SupernodalCholesky<T>& y)
{
    if (x.info() != y.info() || x.panels().size() != y.panels().size())
    {
        return false;
    }
    const auto& sym = x.symbolic();
    const auto off = x.panel_offsets();
    for (u32 s = 0; s < sym.nsuper; ++s)
    {
        const u32 nr = sym.srowp[s + 1] - sym.srowp[s];
        const u32 nc = sym.scol[s + 1] - sym.scol[s];
        for (u32 c = 0; c < nc; ++c)
        {
            const usize at = off[s] + static_cast<usize>(c) * nr + c;
            if (std::memcmp(x.panels().data() + at, y.panels().data() + at, (nr - c) * sizeof(T)) != 0)
            {
                return false;
            }
        }
    }
    return true;
}

// The numeric refactor's seconds (the symbolic analysis is done once, untimed).
template <typename T>
f64 time_factor(const SupernodalPick& pick, const Csr<T>& a, u32 jobs, SupernodalCholesky<T>& f, int reps)
{
    const auto& vals = a.values().values;
    const Clock::time_point t0 = Clock::now();
    for (int r = 0; r < reps; ++r)
    {
        f.factorize_arm(pick, a.pattern(), {vals.data(), vals.size()}, crd::hesap::direct::kSupernodeRelax, jobs,
                        /*reuse_symbolic=*/true);
    }
    return seconds(t0);
}

template <typename T>
void board_factor(const char* label, const char* matrix, const Csr<T>& a, u32 workers, crd::memory::IAllocator* alloc)
{
    constexpr usize arm_count = std::size(kFactorArms);
    const u32 n = a.rows();
    const auto& vals = a.values().values;
    crd::containers::Array<T> xtrue(alloc);
    crd::containers::Array<T> b(alloc);
    crd::containers::Array<T> x(alloc);
    xtrue.resize(n);
    b.resize(n);
    x.resize(n);
    for (u32 i = 0; i < n; ++i)
    {
        xtrue[i] = static_cast<T>(1.0 + 0.01 * static_cast<f64>(i % 37));
    }
    apply(a, xtrue.data(), b.data());
    SupernodalCholesky<T> control(alloc);
    control.factorize_arm({}, a.pattern(), {vals.data(), vals.size()}, crd::hesap::direct::kSupernodeRelax, 1);
    crd::containers::Array<SupernodalCholesky<T>> outs(alloc);
    outs.reserve(arm_count);
    for (usize i = 0; i < arm_count; ++i)
    {
        outs.emplace_back(alloc).factorize_arm(kFactorArms[i].pick, a.pattern(), {vals.data(), vals.size()},
                                               crd::hesap::direct::kSupernodeRelax, workers);
    }
    const f64 once = time_factor<T>({}, a, 1, control, 1);
    const int reps = static_cast<int>(std::clamp(0.2 / std::max(once, 1e-6), 1.0, 64.0));
    f64 serial_times[kRounds] = {};
    f64 times[arm_count][kRounds] = {};
    bool live[arm_count] = {};
    std::fill(live, live + arm_count, true);
    for (int round = -1; round < kRounds; ++round) // round -1 warms every path and gates it
    {
        const f64 s = time_factor<T>({}, a, 1, control, reps);
        if (round >= 0)
        {
            serial_times[round] = s;
        }
        for (usize i = 0; i < arm_count; ++i)
        {
            if (!live[i])
            {
                continue;
            }
            const f64 t = time_factor<T>(kFactorArms[i].pick, a, workers, outs[i], reps);
            if (round >= 0)
            {
                times[i][round] = t;
                continue;
            }
            bool ok = same_factor(outs[i], control);
            if (!kFactorArms[i].same_bits)
            {
                std::memcpy(x.data(), b.data(), n * sizeof(T));
                ok = outs[i].info() == 0 && outs[i].solve({x.data(), n}) &&
                     within_bound(a, x.data(), b.data(), 1, alloc);
            }
            if (!ok)
            {
                std::fprintf(stderr, "GATE MISS: %s %s factor arm %s: %s\n", label, matrix, kFactorArms[i].name,
                             kFactorArms[i].same_bits ? "the factor differs from one job's"
                                                      : "past the backward-error bound");
                g_failed = true;
                live[i] = false;
            }
        }
    }
    std::memcpy(x.data(), b.data(), n * sizeof(T));
    if (!(control.info() == 0 && control.solve({x.data(), n}) && within_bound(a, x.data(), b.data(), 1, alloc)))
    {
        std::fprintf(stderr, "GATE MISS: %s %s: one job's factor is past the backward-error bound\n", label, matrix);
        g_failed = true;
    }
    const f64 serial_s = median(serial_times, kRounds) / reps;
    std::printf("%s %s n %u lnz %llu: one-job refactor %.3f ms\n", label, matrix, n,
                static_cast<unsigned long long>(control.factor_nnz()), serial_s * 1e3);
    for (usize i = 0; i < arm_count; ++i)
    {
        if (live[i])
        {
            const f64 s = median(times[i], kRounds) / reps;
            std::printf("  factor %-22s %10.3f ms  %5.2fx one job\n", kFactorArms[i].name, s * 1e3,
                        s > 0.0 ? serial_s / s : 0.0);
        }
    }
}

template <typename T>
void board_solve(const char* label, const char* matrix, const Csr<T>& a, usize nrhs, u32 workers,
                 crd::memory::IAllocator* alloc)
{
    constexpr usize arm_count = std::size(kSolveArms);
    const u32 n = a.rows();
    const usize len = static_cast<usize>(n) * nrhs;
    const auto& vals = a.values().values;
    SupernodalCholesky<T> f(alloc);
    f.factorize_arm({}, a.pattern(), {vals.data(), vals.size()}, crd::hesap::direct::kSupernodeRelax, workers);
    crd::containers::Array<T> xtrue(alloc);
    crd::containers::Array<T> b(alloc);
    crd::containers::Array<T> ref(alloc);
    crd::containers::Array<T> x(alloc);
    xtrue.resize(len);
    b.resize(len);
    ref.resize(len);
    x.resize(len);
    for (usize c = 0; c < nrhs; ++c)
    {
        for (u32 i = 0; i < n; ++i)
        {
            xtrue[c * n + i] = static_cast<T>(1.0 + static_cast<f64>(c) + 0.01 * static_cast<f64>(i % 41));
        }
        apply(a, xtrue.data() + c * n, b.data() + c * n);
    }
    auto time_solve = [&](const SupernodalPick& pick, crd::containers::Array<T>& out, int reps)
    {
        f64 t = 0.0;
        for (int r = 0; r < reps; ++r)
        {
            std::memcpy(out.data(), b.data(), len * sizeof(T));
            const Clock::time_point t0 = Clock::now();
            if (!f.solve_arm(pick, {out.data(), len}, nrhs))
            {
                g_failed = true;
            }
            t += seconds(t0);
        }
        return t;
    };
    const SupernodalPick serial{.solve_workers = 1};
    const f64 once = time_solve(serial, ref, 1);
    const int reps = static_cast<int>(std::clamp(0.1 / std::max(once, 1e-6), 1.0, 256.0));
    f64 serial_times[kRounds] = {};
    f64 times[arm_count][kRounds] = {};
    bool live[arm_count] = {};
    std::fill(live, live + arm_count, true);
    for (int round = -1; round < kRounds; ++round)
    {
        const f64 s = time_solve(serial, ref, reps);
        if (round >= 0)
        {
            serial_times[round] = s;
        }
        for (usize i = 0; i < arm_count; ++i)
        {
            if (!live[i])
            {
                continue;
            }
            SupernodalPick pick = kSolveArms[i].pick;
            if (kSolveArms[i].on_jobs)
            {
                pick.solve_workers = workers;
            }
            const f64 t = time_solve(pick, x, reps);
            if (round >= 0)
            {
                times[i][round] = t;
                continue;
            }
            const bool ok = kSolveArms[i].same_bits ? std::memcmp(x.data(), ref.data(), len * sizeof(T)) == 0
                                                    : within_bound(a, x.data(), b.data(), nrhs, alloc);
            if (!ok)
            {
                std::fprintf(stderr, "GATE MISS: %s %s solve x%zu arm %s: %s\n", label, matrix, nrhs,
                             kSolveArms[i].name,
                             kSolveArms[i].same_bits ? "x differs from one job's" : "past the backward-error bound");
                g_failed = true;
                live[i] = false;
            }
        }
    }
    if (!within_bound(a, ref.data(), b.data(), nrhs, alloc))
    {
        std::fprintf(stderr, "GATE MISS: %s %s solve x%zu: one job's x is past the backward-error bound\n", label,
                     matrix, nrhs);
        g_failed = true;
    }
    const f64 serial_s = median(serial_times, kRounds) / reps;
    std::printf("%s %s solve x%zu: one job %.3f ms\n", label, matrix, nrhs, serial_s * 1e3);
    for (usize i = 0; i < arm_count; ++i)
    {
        if (live[i])
        {
            const f64 s = median(times[i], kRounds) / reps;
            std::printf("  solve  %-22s %10.3f ms  %5.2fx one job\n", kSolveArms[i].name, s * 1e3,
                        s > 0.0 ? serial_s / s : 0.0);
        }
    }
}

// info(), the fill, L and U bit for bit.
template <typename F> bool same_lu(const F& x, const F& y)
{
    auto same = [](auto p, auto q)
    {
        return p.size() == q.size() && std::memcmp(p.data(), q.data(), p.size() * sizeof(*p.data())) == 0;
    };
    return x.info() == y.info() && x.l_nnz() == y.l_nnz() && x.u_nnz() == y.u_nnz() &&
           same(x.l_values(), y.l_values()) && same(x.u_values(), y.u_values());
}

// The raw factor's solve of A x = b (apply_inverse: no refinement hides a worse factor) within the bound.
template <typename F, typename T>
bool lu_within_bound(const F& f, const Csr<T>& a, const T* b, crd::containers::Array<T>& x,
                     crd::memory::IAllocator* alloc)
{
    const u32 n = a.rows();
    std::memcpy(x.data(), b, n * sizeof(T));
    f.apply_inverse({x.data(), n}, 1);
    return f.info() == 0 && within_bound(a, x.data(), b, 1, alloc);
}

template <typename T, typename F, typename Pick>
f64 time_lu(const Pick& pick, const Csr<T>& a, u32 jobs, F& f, int reps)
{
    const Clock::time_point t0 = Clock::now();
    for (int r = 0; r < reps; ++r)
    {
        f.factorize_arm(pick, a, jobs);
    }
    return seconds(t0);
}

// One LU factor's arms, as board_factor's: round -1 warms every path and gates it, the median of the rounds after.
template <typename T, typename F, typename Pick, usize N>
void board_lu(const char* label, const char* factor, const char* matrix, const Csr<T>& a, const LuArm<Pick> (&arms)[N],
              u32 workers, crd::memory::IAllocator* alloc)
{
    const u32 n = a.rows();
    crd::containers::Array<T> xtrue(alloc);
    crd::containers::Array<T> b(alloc);
    crd::containers::Array<T> x(alloc);
    xtrue.resize(n);
    b.resize(n);
    x.resize(n);
    for (u32 i = 0; i < n; ++i)
    {
        xtrue[i] = static_cast<T>(1.0 + 0.01 * static_cast<f64>(i % 37));
    }
    apply(a, xtrue.data(), b.data());
    F control(alloc);
    const f64 once = time_lu<T>(Pick{}, a, 1, control, 1);
    const int reps = static_cast<int>(std::clamp(0.2 / std::max(once, 1e-6), 1.0, 64.0));
    crd::containers::Array<F> outs(alloc);
    outs.reserve(N);
    for (usize i = 0; i < N; ++i)
    {
        outs.emplace_back(alloc);
    }
    f64 serial_times[kRounds] = {};
    f64 times[N][kRounds] = {};
    bool live[N] = {};
    std::fill(live, live + N, true);
    for (int round = -1; round < kRounds; ++round)
    {
        const f64 s = time_lu<T>(Pick{}, a, 1, control, reps);
        if (round >= 0)
        {
            serial_times[round] = s;
        }
        for (usize i = 0; i < N; ++i)
        {
            if (!live[i])
            {
                continue;
            }
            const f64 t = time_lu<T>(arms[i].pick, a, workers, outs[i], reps);
            if (round >= 0)
            {
                times[i][round] = t;
                continue;
            }
            const bool ok =
                arms[i].same_bits ? same_lu(outs[i], control) : lu_within_bound(outs[i], a, b.data(), x, alloc);
            if (!ok)
            {
                std::fprintf(stderr, "GATE MISS: %s %s %s arm %s: %s\n", label, factor, matrix, arms[i].name,
                             arms[i].same_bits ? "the factor differs from one job's" : "past the backward-error bound");
                g_failed = true;
                live[i] = false;
            }
        }
    }
    if (!lu_within_bound(control, a, b.data(), x, alloc))
    {
        std::fprintf(stderr, "GATE MISS: %s %s %s: one job's factor is past the backward-error bound\n", label, factor,
                     matrix);
        g_failed = true;
    }
    const f64 serial_s = median(serial_times, kRounds) / reps;
    std::printf("%s %s %s n %u fill %llu: one-job factor %.3f ms\n", label, factor, matrix, n,
                static_cast<unsigned long long>(control.factor_nnz()), serial_s * 1e3);
    for (usize i = 0; i < N; ++i)
    {
        if (live[i])
        {
            const f64 sec = median(times[i], kRounds) / reps;
            std::printf("  factor %-22s %10.3f ms  %5.2fx one job\n", arms[i].name, sec * 1e3,
                        sec > 0.0 ? serial_s / sec : 0.0);
        }
    }
}

template <typename T> void run_type(const char* label, u32 workers)
{
    struct Matrix
    {
        const char* name;
        u32 nx;
        u32 ny;
        u32 nz;
        bool border;
    };
    const Matrix matrices[] = {
        {"laplace3d-16", 16, 16, 16, false},       {"laplace3d-24", 24, 24, 24, false},
        {"laplace3d-32", 32, 32, 32, false},       {"laplace2d-256", 256, 256, 1, false},
        {"bordered-48x64+640", 48, 64, 640, true},
    };
    for (const Matrix& m : matrices)
    {
        crd::memory::GrowableTlsfAllocator alloc;
        const Csr<f64> base = m.border ? bordered(&alloc, m.nx, m.ny, m.nz) : laplacian(&alloc, m.nx, m.ny, m.nz);
        const Csr<T> a = ordered<T>(base, &alloc);
        board_factor<T>(label, m.name, a, workers, &alloc);
        board_solve<T>(label, m.name, a, 1, workers, &alloc);
        board_solve<T>(label, m.name, a, kManyRhs, workers, &alloc);
    }
    struct Stencil
    {
        const char* name;
        u32 nx;
        u32 ny;
        u32 nz;
    };
    const Stencil stencils[] = {
        {"convection3d-16", 16, 16, 16},
        {"convection3d-24", 24, 24, 24},
        {"convection2d-256", 256, 256, 1},
    };
    for (const Stencil& m : stencils)
    {
        crd::memory::GrowableTlsfAllocator alloc;
        const Csr<T> a = ordered<T>(convection(&alloc, m.nx, m.ny, m.nz), &alloc);
        board_lu<T, SupernodalLU<T>>(label, "supernodal-lu", m.name, a, kSupernodalLuArms, workers, &alloc);
        board_lu<T, MultifrontalLU<T>>(label, "multifrontal-lu", m.name, a, kMultifrontalLuArms, workers, &alloc);
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
        std::fprintf(stderr, "usage: bench_hesap_direct_parallel [--threads N]\n");
        return 2;
    }
    crd::jobs::Config cfg;
    if (workers != 0)
    {
        cfg.num_threads = workers;
    }
    crd::jobs::init(cfg);
    const u32 jobs = crd::jobs::num_workers();
    std::printf("[bench_hesap_direct_parallel] the sparse direct factors' levers on %u jobs against one job, median of "
                "%d rounds. hesap-direct level %s, hesap-dense level %s (selected %s)\n",
                jobs, kRounds, crd::cpu_level_name(crd::hesap::direct::direct_kernels().level),
                crd::cpu_level_name(crd::hesap::dense::dense_kernels().level),
                crd::cpu_level_name(crd::cpu_selection().selected));
    run_type<f32>("f32", jobs);
    run_type<f64>("f64", jobs);
    crd::jobs::shutdown();
    if (g_failed)
    {
        std::fprintf(stderr, "bench_hesap_direct_parallel: a gate failed\n");
        return 1;
    }
    return 0;
}
