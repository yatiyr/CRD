// bench_hesap_eigen_lobpcg — v6-close crush bench: Cerid AMG-LOBPCG vs ARPACK shift-invert
// (scripts/eigen_ref_scipy.py) for the smallest eigenpairs of the 3D Laplacian (the fill-in regime).
//
// HONEST scope (advisor-gated): this is AMG-LOBPCG (iterative+multigrid) vs DIRECT shift-invert ARPACK — an
// iterative method beating a DIRECT one in the regime where direct factorization's fill-in is fatal (3D). It is
// NOT a same-class head-to-head (the same-class peer = AMG-preconditioned LOBPCG, e.g. pyamg+scipy, expected
// parity+moat — owed). The matrix is the 3D model-POISSON Laplacian, AMG's BEST case; general/anisotropic FEM
// is a hypothesis, NOT demonstrated here.
//
// Metrics:
//   (1) MEMORY = AMG `operator_complexity()` — the sum of the level-operator nnz (a STANDARD AMG metric); it
//       EXCLUDES prolongation/restriction + smoother data, so it understates the full AMG footprint, but the
//       full footprint is still O(n) and ≪ the SuperLU factor's nnz(L)+nnz(U) the oracle prints. The factor
//       fill grows ~O(n^(4/3)) in 3D; AMG stays O(n). LANGUAGE-INDEPENDENT — the rock-solid metric.
//   (2) WALL-CLOCK = AMG setup + LOBPCG solve vs eigsh-SI's internal splu + ARPACK iterate. BOTH single-
//       threaded: Cerid SERIAL (SparseLinearOp, no jobs); scipy SuperLU/ARPACK effectively serial (if SuperLU
//       uses BLAS threads, that is conservative FOR Cerid). eigsh-SI is splu-dominated ⇒ tol-robust.
//   Accuracy matched by construction (rectangular grid ⇒ non-degenerate; both report identical analytic
//   eigenvalues — cross-check the printed smallest-4). The determinism MOAT (bit-identical {1,2,4,8}) is
//   Cerid's alone — proven in the v6-e-b tests, not re-timed here.
//
// Plain runtime target (no external link; the scipy peer runs separately). Build in win-release for the timing.

#include <crd/containers/array.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/amg/amg.hpp>
#include <crd/hesap/eigen/eigen.hpp>
#include <crd/hesap/sparse/sparse_linear_op.hpp>
#include <crd/hesap/sparse/triplet_builder.hpp>
#include <crd/memory/allocators/growable_tlsf_allocator.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>

namespace
{
using Clock = std::chrono::high_resolution_clock;
namespace sp = crd::hesap::sparse;
namespace eig = crd::hesap::eigen;
using Csr = sp::SparseMatrix<crd::f64, sp::SparseFormat::Csr>;

// 7-point 3D Laplacian on a RECTANGULAR mx*my*mz grid: diag 6, 6 neighbours -1. n = mx*my*mz. Distinct dims
// ⇒ NON-degenerate smallest modes (so this and eigsh resolve identical distinct eigenvalues). Matches
// eigen_ref_scipy.py (same grid (s, s+3, s+7); eigenvalues are permutation-invariant ⇒ index order irrelevant).
Csr laplacian_3d(crd::memory::IAllocator* a, crd::u32 mx, crd::u32 my, crd::u32 mz)
{
    const crd::u32 n = mx * my * mz;
    sp::TripletBuilder<crd::f64> tb(a, n, n);
    auto id = [my, mz](crd::u32 i, crd::u32 j, crd::u32 l) { return (i * my + j) * mz + l; };
    for (crd::u32 i = 0; i < mx; ++i)
    {
        for (crd::u32 j = 0; j < my; ++j)
        {
            for (crd::u32 l = 0; l < mz; ++l)
            {
                const crd::u32 d = id(i, j, l);
                tb.add(d, d, 6.0);
                if (i + 1 < mx)
                {
                    tb.add(d, id(i + 1, j, l), -1.0);
                    tb.add(id(i + 1, j, l), d, -1.0);
                }
                if (j + 1 < my)
                {
                    tb.add(d, id(i, j + 1, l), -1.0);
                    tb.add(id(i, j + 1, l), d, -1.0);
                }
                if (l + 1 < mz)
                {
                    tb.add(d, id(i, j, l + 1), -1.0);
                    tb.add(id(i, j, l + 1), d, -1.0);
                }
            }
        }
    }
    return tb.compress();
}

double secs(Clock::time_point a, Clock::time_point b)
{
    return std::chrono::duration<double>(b - a).count();
}
} // namespace

int main(int argc, char** argv)
{
    crd::u32 ss[] = {16, 20, 24, 28, 32};
    std::printf("%4s %9s %10s %10s %10s %13s %9s  %s\n", "s", "n", "amg_set_s", "lobpcg_s", "total_s",
                "amg_nnz", "nnz/row", "smallest4 (iters,conv)");
    for (crd::u32 mi = 0; mi < (argc > 1 ? static_cast<crd::u32>(argc - 1) : 5U); ++mi)
    {
        const crd::u32 s = (argc > 1) ? static_cast<crd::u32>(std::atoi(argv[mi + 1])) : ss[mi];
        const crd::u32 mx = s, my = s + 3, mz = s + 7; // rectangular ⇒ non-degenerate
        crd::memory::GrowableTlsfAllocator alloc;
        Csr a = laplacian_3d(&alloc, mx, my, mz);
        const crd::u32 n = mx * my * mz;

        const auto t0 = Clock::now();
        crd::hesap::amg::SaAmg<crd::f64> amg(a, &alloc);
        const auto t1 = Clock::now();

        sp::SparseLinearOp<crd::f64> op(a);
        eig::EigenOptions<crd::f64> opts;
        opts.nev = 4;
        opts.which = eig::Which::SmallestAlgebraic;
        opts.tol = 1e-7;
        opts.max_restarts = 500;
        auto r = eig::eigs_sym_lobpcg<crd::f64>(op, opts, &alloc, &amg);
        const auto t2 = Clock::now();

        double sm[4];
        for (crd::u32 c = 0; c < 4; ++c)
        {
            sm[c] = r.values[c].re;
        }
        std::sort(sm, sm + 4);
        const crd::usize anz = amg.operator_complexity();
        std::printf("%4u %9u %10.3f %10.3f %10.3f %13llu %9.1f  [%.5f,%.5f,%.5f,%.5f] (%u,%s)\n", s, n,
                    secs(t0, t1), secs(t1, t2), secs(t0, t2), static_cast<unsigned long long>(anz),
                    static_cast<double>(anz) / n, sm[0], sm[1], sm[2], sm[3], r.iterations,
                    r.converged ? "yes" : "NO");
    }
    return 0;
}
