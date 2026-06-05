// bench_hesap_mixed_internal — v5f-c INTERNAL crush gate: Cerid f32-factor + f64-IR vs Cerid f64-full.
//
// The unimpeachable claim (same code, same matrix, matched final accuracy): factor-in-f32 (~2x the dense-
// front kernel rate) + f64 iterative refinement recovers f64 accuracy at a fraction of the f64-full factor
// cost. Reports FACTOR-ONLY and END-TO-END (factor+solve) speedup — the IR adds solve cost, so end-to-end is
// the honest user-felt number — plus residual (must match f64-full) and IR iteration count (convergence is
// the load-bearing assumption: f32-IR reaches f64 only when kappa(A)*u_f <~ 1).
//
// No external peer — runs locally (plain runtime target, build in win-release). The MUMPS-single+IR crush
// peer is the WSL follow-on. crd conventions throughout; raw double only in the residual oracle.

#include <crd/containers/array.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/direct/mixed_refine.hpp>
#include <crd/hesap/direct/multifrontal_ldlt.hpp>
#include <crd/hesap/direct/multifrontal_lu.hpp>
#include <crd/hesap/direct/supernodal_cholesky.hpp>
#include <crd/hesap/ordering/ordering.hpp>
#include <crd/hesap/sparse/sparse.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/memory/allocators/growable_tlsf_allocator.hpp>

#include <chrono>
#include <cmath>
#include <cstdio>

namespace
{
using Clock = std::chrono::high_resolution_clock;
namespace sp = crd::hesap::sparse;
namespace ord = crd::hesap::ordering;
namespace dir = crd::hesap::direct;

crd::memory::GrowableTlsfAllocator g_alloc;

struct Trip
{
    crd::u32 r;
    crd::u32 c;
    crd::f64 v;
};

// 3D 7-point Laplacian on a k*k*k grid. SPD (diag 6) for shift=0; indefinite (diag 6-shift, shift>0 reaches
// negative eigenvalues) for the LDLt domain. Big near-root fronts (~k^2) => the within-front gemm where the
// f32 kernel doubles throughput.
void make_laplacian_3d(crd::u32 k, crd::f64 shift, crd::u32& n, crd::containers::Array<Trip>& t)
{
    n = k * k * k;
    auto id = [k](crd::u32 i, crd::u32 j, crd::u32 l) { return (i * k + j) * k + l; };
    for (crd::u32 i = 0; i < k; ++i)
    {
        for (crd::u32 j = 0; j < k; ++j)
        {
            for (crd::u32 l = 0; l < k; ++l)
            {
                const crd::u32 d = id(i, j, l);
                t.push_back({d, d, 6.0 - shift});
                if (i + 1 < k)
                {
                    t.push_back({d, id(i + 1, j, l), -1.0});
                    t.push_back({id(i + 1, j, l), d, -1.0});
                }
                if (j + 1 < k)
                {
                    t.push_back({d, id(i, j + 1, l), -1.0});
                    t.push_back({id(i, j + 1, l), d, -1.0});
                }
                if (l + 1 < k)
                {
                    t.push_back({d, id(i, j, l + 1), -1.0});
                    t.push_back({id(i, j, l + 1), d, -1.0});
                }
            }
        }
    }
}

// Apply an AMD fill-reducing permutation to the triplets (so every factorization sees the SAME small fill —
// the kernel, not the ordering, is what we measure).
void amd_permute(crd::u32 n, crd::containers::Array<Trip>& t)
{
    // Build a CSC pattern for amd_order.
    sp::TripletBuilder<crd::f64> tb(&g_alloc, n, n);
    for (crd::usize e = 0; e < t.size(); ++e)
    {
        tb.add(t[e].r, t[e].c, t[e].v);
    }
    auto a0 = sp::to_csc<crd::f64>(tb.compress(), &g_alloc);
    auto perm = ord::amd_order(a0.pattern(), &g_alloc);
    for (crd::usize e = 0; e < t.size(); ++e)
    {
        t[e].r = perm.inv_perm[t[e].r];
        t[e].c = perm.inv_perm[t[e].c];
    }
}

sp::SparseMatrix<crd::f64, sp::SparseFormat::Csr> build_csr(crd::u32 n, const crd::containers::Array<Trip>& t)
{
    sp::TripletBuilder<crd::f64> tb(&g_alloc, n, n);
    for (crd::usize e = 0; e < t.size(); ++e)
    {
        tb.add(t[e].r, t[e].c, t[e].v);
    }
    return tb.compress();
}

sp::SparseMatrix<crd::f64, sp::SparseFormat::Csc> build_csc(crd::u32 n, const crd::containers::Array<Trip>& t)
{
    sp::TripletBuilder<crd::f64> tb(&g_alloc, n, n);
    for (crd::usize e = 0; e < t.size(); ++e)
    {
        tb.add(t[e].r, t[e].c, t[e].v);
    }
    return sp::to_csc<crd::f64>(tb.compress(), &g_alloc);
}

// Relative residual ||A*x - b||_2 / ||b||_2 from the triplets (factorization-independent oracle).
crd::f64 rel_resid(crd::u32 n, const crd::containers::Array<Trip>& t, const crd::containers::Array<crd::f64>& x,
                   const crd::containers::Array<crd::f64>& b)
{
    crd::containers::Array<crd::f64> ax(&g_alloc);
    ax.resize(n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        ax[i] = 0.0;
    }
    for (crd::usize e = 0; e < t.size(); ++e)
    {
        ax[t[e].r] += t[e].v * x[t[e].c];
    }
    crd::f64 num = 0.0;
    crd::f64 den = 0.0;
    for (crd::u32 i = 0; i < n; ++i)
    {
        const crd::f64 d = ax[i] - b[i];
        num += d * d;
        den += b[i] * b[i];
    }
    return std::sqrt(num) / (std::sqrt(den) + 1e-300);
}

double ms_since(const Clock::time_point& t0)
{
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

// One family row: factor f64-full + solve, then factor-in-f32 + IR-solve; report the two timings + residuals.
// FactorFull(): -> f64 factorization (info()/solve()); FactorMixed(): -> the IterativeRefinedSolve wrapper.
template <typename FactorFull, typename FactorMixed>
void run_family(const char* label, crd::u32 n, const crd::containers::Array<Trip>& t, crd::u32 reps,
                FactorFull factor_full, FactorMixed factor_mixed)
{
    crd::containers::Array<crd::f64> b(&g_alloc);
    b.resize(n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        b[i] = std::sin(0.37 * static_cast<double>(i) + 0.1);
    }
    crd::containers::Array<crd::f64> x(&g_alloc);
    x.resize(n);

    double full_fac = 1e30;
    double full_sol = 1e30;
    crd::f64 full_res = -1.0;
    for (crd::u32 r = 0; r < reps; ++r)
    {
        const auto f0 = Clock::now();
        auto f = factor_full();
        const double fac = ms_since(f0);
        if (f.info() != 0)
        {
            std::printf("  %-9s n=%-6u  f64-full FACTOR FAILED (info=%zu)\n", label, n,
                        static_cast<size_t>(f.info()));
            return;
        }
        for (crd::u32 i = 0; i < n; ++i)
        {
            x[i] = b[i];
        }
        const auto s0 = Clock::now();
        const bool ok = f.solve({x.data(), n});
        const double sol = ms_since(s0);
        if (!ok)
        {
            std::printf("  %-9s n=%-6u  f64-full SOLVE FAILED\n", label, n);
            return;
        }
        full_fac = std::min(full_fac, fac);
        full_sol = std::min(full_sol, sol);
        full_res = rel_resid(n, t, x, b);
    }

    double mix_fac = 1e30;
    double mix_sol = 1e30;
    crd::f64 mix_res = -1.0;
    crd::u32 mix_iters = 0;
    for (crd::u32 r = 0; r < reps; ++r)
    {
        const auto f0 = Clock::now();
        auto f = factor_mixed();
        const double fac = ms_since(f0);
        if (f.info() != 0)
        {
            std::printf("  %-9s n=%-6u  f32-IR FACTOR FAILED (f32 singular)\n", label, n);
            return;
        }
        for (crd::u32 i = 0; i < n; ++i)
        {
            x[i] = b[i];
        }
        const auto s0 = Clock::now();
        const bool ok = f.solve({x.data(), n});
        const double sol = ms_since(s0);
        if (!ok)
        {
            std::printf("  %-9s n=%-6u  f32-IR did NOT converge (kappa*u_f too large) — flagged, no result\n",
                        label, n);
            return;
        }
        mix_fac = std::min(mix_fac, fac);
        mix_sol = std::min(mix_sol, sol);
        mix_res = rel_resid(n, t, x, b);
        mix_iters = f.last_iters();
    }

    const double fac_speedup = full_fac / mix_fac;
    const double e2e_speedup = (full_fac + full_sol) / (mix_fac + mix_sol);
    std::printf("  %-9s n=%-6u | f64-full fac %8.1f ms sol %7.1f ms res %.1e | f32-IR fac %8.1f ms sol %7.1f ms "
                "res %.1e iters %u | FACTOR %.2fx END-TO-END %.2fx\n",
                label, n, full_fac, full_sol, full_res, mix_fac, mix_sol, mix_res, mix_iters, fac_speedup,
                e2e_speedup);
}

void run_size(crd::u32 k, crd::u32 reps)
{
    // --- SPD Cholesky (shift 0) ---
    {
        crd::u32 n = 0;
        crd::containers::Array<Trip> t(&g_alloc);
        make_laplacian_3d(k, 0.0, n, t);
        amd_permute(n, t);
        auto a_csc = build_csc(n, t);
        run_family(
            "Cholesky", n, t, reps,
            [&]() {
                return dir::factor_supernodal_cholesky<crd::f64>(
                    a_csc.pattern(), {a_csc.values().values.data(), a_csc.values().values.size()}, &g_alloc);
            },
            [&]() { return dir::factor_mixed_cholesky(a_csc, &g_alloc); });
    }
    // --- LU (the SPD Laplacian fed to the general multifrontal LU — the unsym kernel) ---
    {
        crd::u32 n = 0;
        crd::containers::Array<Trip> t(&g_alloc);
        make_laplacian_3d(k, 0.0, n, t);
        amd_permute(n, t);
        auto a_csr = build_csr(n, t);
        run_family(
            "LU", n, t, reps, [&]() { return dir::factor_multifrontal_lu<crd::f64>(a_csr, &g_alloc); },
            [&]() { return dir::factor_mixed_lu(a_csr, &g_alloc); });
    }
    // --- LDLt (indefinite: 3D Laplacian shifted so some eigenvalues go negative) ---
    {
        crd::u32 n = 0;
        crd::containers::Array<Trip> t(&g_alloc);
        make_laplacian_3d(k, 3.0, n, t); // shift 3 -> indefinite (the v5d-h indefinite-3D domain)
        amd_permute(n, t);
        auto a_csc = build_csc(n, t);
        run_family(
            "LDLt", n, t, reps, [&]() { return dir::factor_multifrontal_ldlt<crd::f64>(a_csc, &g_alloc); },
            [&]() { return dir::factor_mixed_ldlt(a_csc, &g_alloc); });
    }
}
} // namespace

int main()
{
    crd::jobs::init(); // within-front gemm parallelism (both f64-full and f32-IR use it equally)
    std::printf("=== v5f-c mixed-precision INTERNAL crush gate (f32-factor + f64-IR vs f64-full) ===\n");
    std::printf("3D Laplacian (AMD-ordered), best-of-reps. Speedup>1 = f32-IR faster AT MATCHED f64 accuracy.\n\n");
    constexpr crd::u32 reps = 2;
    for (crd::u32 k : {16U, 24U, 32U, 40U}) // n = 4096, 13824, 32768, 64000 — the gemm-dominated crush sizes
    {
        run_size(k, reps);
        std::printf("\n");
    }
    crd::jobs::shutdown();
    return 0;
}
