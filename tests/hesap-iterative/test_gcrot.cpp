#include <catch2/catch_test_macros.hpp>

#include <crd/containers/span.hpp>
#include <crd/hesap/complex.hpp>
#include <crd/hesap/dense/blas1.hpp>
#include <crd/hesap/dense/vector.hpp>
#include <crd/hesap/iterative/gcrot.hpp>
#include <crd/hesap/iterative/gmres.hpp>
#include <crd/hesap/preconditioners/jacobi.hpp>
#include <crd/hesap/sparse/parallel_sparse_linear_op.hpp>
#include <crd/hesap/sparse/triplet_builder.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

using namespace crd::hesap::iterative;
using namespace crd::hesap::sparse;
using crd::hesap::Complex;
using crd::hesap::preconditioners::JacobiPreconditioner;
namespace dense = crd::hesap::dense;

namespace
{
template <typename T>
SparseMatrix<T, SparseFormat::Csr> random_nonsym(crd::memory::IAllocator* a, crd::u32 n, crd::u64 seed)
{
    crd::u64 state = seed;
    auto     next  = [&state]() -> double {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<double>(state >> 11) / static_cast<double>(1ULL << 53);
    };
    TripletBuilder<T> b(a, n, n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        if constexpr (dense::is_complex_v<T>) { b.add(i, i, T{5.0, 0.5}); }
        else { b.add(i, i, T(5)); }
        for (crd::u32 off = 0; off < 4; ++off)
        {
            const crd::u32 j = static_cast<crd::u32>(next() * n);
            if (j == i) { continue; }
            const double re = next() - 0.5;
            if constexpr (dense::is_complex_v<T>) { b.add(i, j, T{re, next() - 0.5}); }
            else { b.add(i, j, static_cast<T>(re)); }
        }
    }
    return b.compress();
}

// Nonsymmetric tridiagonal with a CLUSTER of a few small, well-separated
// eigenvalues (first `nsmall` diagonal entries ≈ 0.1, the rest ≈ 10) + asymmetric
// off-diagonals. Small-m restarted GMRES stagnates re-discovering the small modes
// each restart; GCROT deflates them into the recycle space ⇒ far fewer iterations.
// This is the textbook deflation win and a robust iters-saved gate.
template <typename T>
SparseMatrix<T, SparseFormat::Csr> small_eig_cluster(crd::memory::IAllocator* a, crd::u32 n, crd::u32 nsmall)
{
    TripletBuilder<T> b(a, n, n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        b.add(i, i, (i < nsmall) ? static_cast<T>(0.1) : T(10));
        if (i + 1 < n) { b.add(i, i + 1, T(-1)); }
        if (i > 0) { b.add(i, i - 1, static_cast<T>(-0.7)); }
    }
    return b.compress();
}

// small_eig_cluster + a uniform diagonal shift α (A_i = A_base + α·I): a parametric
// sequence sharing spectral structure — the recycle space built on one A_i deflates
// the next (the de Sturler cross-solve payoff).
template <typename T>
SparseMatrix<T, SparseFormat::Csr> cluster_shifted(crd::memory::IAllocator* a, crd::u32 n, crd::u32 nsmall, double shift)
{
    TripletBuilder<T> b(a, n, n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        b.add(i, i, ((i < nsmall) ? static_cast<T>(0.1) : T(10)) + static_cast<T>(shift));
        if (i + 1 < n) { b.add(i, i + 1, T(-1)); }
        if (i > 0) { b.add(i, i - 1, static_cast<T>(-0.7)); }
    }
    return b.compress();
}

template <typename T>
crd::hesap::dense::RealType<T> true_residual(const crd::hesap::LinearOp<T>& op, crd::containers::ConstSpan<T> x,
                                             crd::containers::ConstSpan<T> b, crd::memory::IAllocator* a)
{
    const crd::usize n = op.n_rows();
    dense::Vector<T> ax(a, n);
    (void)op.apply(x, ax.span());
    dense::Vector<T> r(a, n);
    for (crd::usize i = 0; i < n; ++i) { r(i) = b[i] - ax(i); }
    return dense::nrm2<T>(r.span()) / dense::nrm2<T>(b);
}
} // namespace

TEST_CASE("gcrot_build_d constructs D from the R-transpose-inverse B-transpose solve", "[hesap-iterative][gcrot][unit]")
{
    // m-stride = 2; Rsq = [[2,1],[0,3]] (upper-tri); B = [[4,5],[6,7]] (ncu=2 × kcols=2).
    const crd::f64 rmat[4] = {2, 1, 0, 3};
    const crd::f64 bmat[4] = {4, 5, 6, 7};
    crd::f64       x[4]    = {0, 0, 0, 0};
    crd::f64       d[4]    = {0, 0, 0, 0};
    crd::hesap::iterative::detail::gcrot_build_d<crd::f64>(rmat, bmat, /*m=*/2, /*ncu=*/2, /*kcols=*/2, x, d);
    // Hand-solved: X=[[2,3],[1,4/3]] ⇒ D=Xᵀ=[[2,1],[3,4/3]] (row-major d[i*kcols+j]).
    REQUIRE(d[0] == 2.0);
    REQUIRE(d[1] == 1.0);
    REQUIRE(d[2] == 3.0);
    REQUIRE(std::abs(d[3] - 4.0 / 3.0) < 1e-15);
}

TEST_CASE("GCROT(m,k) 'smallest' truncation converges in <= iters of 'oldest'", "[hesap-iterative][gcrot]")
{
    crd::jobs::init();
    {
        crd::memory::TlsfAllocator alloc{96U << 20};
        const crd::u32             n = 240;
        auto                       a = small_eig_cluster<crd::f64>(&alloc, n, /*nsmall=*/6);
        ParallelSparseLinearOp<crd::f64> op(a, &alloc);
        dense::Vector<crd::f64>    b(&alloc, n);
        b.fill(1.0);
        IterativeOptions<crd::f64> opts;
        opts.rel_tol  = 1e-10;
        opts.max_iter = 8000;

        dense::Vector<crd::f64>  xo(&alloc, n);
        GcrotWorkspace<crd::f64> wo(&alloc, n, /*inner=*/8, /*recycle=*/8);
        wo.truncate = GcrotTruncate::Oldest;
        auto ro = gcrot<crd::f64>(op, b.span(), xo.span(), opts, wo, &alloc);

        dense::Vector<crd::f64>  xs(&alloc, n);
        GcrotWorkspace<crd::f64> wsm(&alloc, n, /*inner=*/8, /*recycle=*/8);
        wsm.truncate = GcrotTruncate::Smallest;
        auto rs = gcrot<crd::f64>(op, b.span(), xs.span(), opts, wsm, &alloc);

        REQUIRE(ro.converged);
        REQUIRE(rs.converged);
        REQUIRE(true_residual<crd::f64>(op, xs.span(), b.span(), &alloc) < 1e-7);
        // SVD-optimal selection keeps the best k-1 recycle directions ⇒ never worse.
        REQUIRE(rs.iterations <= ro.iterations);
    }
    crd::jobs::shutdown();
}

TEST_CASE("GCROT(m,k) solves a nonsymmetric system (f64)", "[hesap-iterative][gcrot]")
{
    crd::jobs::init();
    {
        crd::memory::TlsfAllocator alloc{64U << 20};
        const crd::u32             n = 150;
        auto                       a = random_nonsym<crd::f64>(&alloc, n, /*seed=*/0x6C607ULL);
        ParallelSparseLinearOp<crd::f64> op(a, &alloc);
        dense::Vector<crd::f64>    b(&alloc, n);
        b.fill(1.0);
        dense::Vector<crd::f64>    x(&alloc, n);
        IterativeOptions<crd::f64> opts;
        opts.rel_tol  = 1e-12;
        opts.max_iter = 2000;
        GcrotWorkspace<crd::f64> ws(&alloc, n, /*inner=*/20, /*recycle=*/10);

        auto res = gcrot<crd::f64>(op, b.span(), x.span(), opts, ws, &alloc);
        REQUIRE(res.converged);
        REQUIRE(true_residual<crd::f64>(op, x.span(), b.span(), &alloc) < 1e-8);
    }
    crd::jobs::shutdown();
}

TEST_CASE("GCROT(m,k) recycling beats restarted GMRES(m) in iterations", "[hesap-iterative][gcrot]")
{
    crd::jobs::init();
    {
        crd::memory::TlsfAllocator alloc{64U << 20};
        const crd::u32             n = 240;
        auto                       a = small_eig_cluster<crd::f64>(&alloc, n, /*nsmall=*/6);
        ParallelSparseLinearOp<crd::f64> op(a, &alloc);
        dense::Vector<crd::f64>    b(&alloc, n);
        b.fill(1.0);
        const crd::usize           inner = 8;

        IterativeOptions<crd::f64> opts;
        opts.rel_tol  = 1e-10;
        opts.max_iter = 8000;

        dense::Vector<crd::f64> xg(&alloc, n);
        GmresWorkspace<crd::f64> wg(&alloc, n, inner);
        auto                     rg = gmres<crd::f64>(op, b.span(), xg.span(), opts, wg, &alloc);

        dense::Vector<crd::f64> xc(&alloc, n);
        GcrotWorkspace<crd::f64> wc(&alloc, n, inner, /*recycle=*/inner);
        auto                     rc = gcrot<crd::f64>(op, b.span(), xc.span(), opts, wc, &alloc);

        REQUIRE(rg.converged);
        REQUIRE(rc.converged);
        REQUIRE(true_residual<crd::f64>(op, xc.span(), b.span(), &alloc) < 1e-7);
        // Recycling the slow modes across restart cycles ⇒ strictly fewer iterations
        // than plain restarted GMRES(m) at the same inner dimension.
        REQUIRE(rc.iterations < rg.iterations);
    }
    crd::jobs::shutdown();
}

TEST_CASE("GCROT(m,k) with Jacobi right preconditioner (f64)", "[hesap-iterative][gcrot][precond]")
{
    crd::jobs::init();
    {
        crd::memory::TlsfAllocator alloc{64U << 20};
        const crd::u32             n = 150;
        auto                       a = random_nonsym<crd::f64>(&alloc, n, /*seed=*/0xBEEF77ULL);
        ParallelSparseLinearOp<crd::f64> op(a, &alloc);
        JacobiPreconditioner<crd::f64>   m(a, &alloc);
        dense::Vector<crd::f64>    b(&alloc, n);
        b.fill(1.0);
        dense::Vector<crd::f64>    x(&alloc, n);
        IterativeOptions<crd::f64> opts;
        opts.rel_tol  = 1e-12;
        opts.max_iter = 2000;
        GcrotWorkspace<crd::f64> ws(&alloc, n, /*inner=*/20, /*recycle=*/10);

        auto res = gcrot<crd::f64>(op, &m, b.span(), x.span(), opts, ws, &alloc);
        REQUIRE(res.converged);
        REQUIRE(true_residual<crd::f64>(op, x.span(), b.span(), &alloc) < 1e-8);
    }
    crd::jobs::shutdown();
}

TEST_CASE("GCROT(m,k) solves a complex nonsymmetric system (c64)", "[hesap-iterative][gcrot][complex]")
{
    crd::jobs::init();
    {
        crd::memory::TlsfAllocator alloc{64U << 20};
        using C        = Complex<crd::f64>;
        const crd::u32 n = 120;
        auto           a = random_nonsym<C>(&alloc, n, /*seed=*/0xC0FFEE3ULL);
        ParallelSparseLinearOp<C> op(a, &alloc);
        dense::Vector<C> b(&alloc, n);
        b.fill(C{1.0, 0.0});
        dense::Vector<C> x(&alloc, n);
        IterativeOptions<crd::f64> opts;
        opts.rel_tol  = 1e-12;
        opts.max_iter = 2000;
        GcrotWorkspace<C> ws(&alloc, n, /*inner=*/20, /*recycle=*/10);

        auto res = gcrot<C>(op, b.span(), x.span(), opts, ws, &alloc);
        REQUIRE(res.converged);
        REQUIRE(true_residual<C>(op, x.span(), b.span(), &alloc) < 1e-7);
    }
    crd::jobs::shutdown();
}

TEST_CASE("GCROT cross-solve recycling is correct across DIFFERENT operators (C rebuilt)",
          "[hesap-iterative][gcrot][recycle]")
{
    crd::jobs::init();
    {
        crd::memory::TlsfAllocator alloc{96U << 20};
        const crd::u32             n = 200;
        // Fill the recycle space on A1, then REUSE it on a different operator A2 (same
        // shape). The stored c = A1·u is stale for A2 — gcrot_recycled must rebuild
        // C = A2·U on entry, or the projection silently corrupts the A2 solve.
        auto a1 = cluster_shifted<crd::f64>(&alloc, n, /*nsmall=*/6, /*shift=*/0.0);
        auto a2 = cluster_shifted<crd::f64>(&alloc, n, /*nsmall=*/6, /*shift=*/3.0);
        ParallelSparseLinearOp<crd::f64> op1(a1, &alloc);
        ParallelSparseLinearOp<crd::f64> op2(a2, &alloc);
        dense::Vector<crd::f64>    b(&alloc, n);
        b.fill(1.0);
        IterativeOptions<crd::f64> opts;
        opts.rel_tol  = 1e-10;
        opts.max_iter = 8000;

        RecycleSpace<crd::f64>   rs(&alloc, n, /*recycle=*/8);
        GcrotWorkspace<crd::f64> ws(&alloc, n, /*inner=*/8, /*recycle=*/8);

        dense::Vector<crd::f64> x1(&alloc, n);
        auto r1 = gcrot_recycled<crd::f64>(op1, b.span(), x1.span(), opts, ws, rs, &alloc);
        REQUIRE(r1.converged);
        REQUIRE(rs.dimension() > 0); // recycle space populated by the first solve

        dense::Vector<crd::f64> x2(&alloc, n);
        auto r2 = gcrot_recycled<crd::f64>(op2, b.span(), x2.span(), opts, ws, rs, &alloc);
        REQUIRE(r2.converged);
        // The A2 solution must be CORRECT despite the recycle space being filled on A1.
        REQUIRE(true_residual<crd::f64>(op2, x2.span(), b.span(), &alloc) < 1e-8);
    }
    crd::jobs::shutdown();
}

TEST_CASE("GCROT cross-solve recycling saves iterations across a shift sequence",
          "[hesap-iterative][gcrot][recycle]")
{
    crd::jobs::init();
    {
        crd::memory::TlsfAllocator alloc{128U << 20};
        const crd::u32             n      = 240;
        const crd::u32             nsmall = 6;
        const double               shifts[6] = {0.0, 0.05, 0.1, 0.15, 0.2, 0.25};
        dense::Vector<crd::f64>    b(&alloc, n);
        b.fill(1.0);
        IterativeOptions<crd::f64> opts;
        opts.rel_tol  = 1e-10;
        opts.max_iter = 8000;

        // Fresh GMRES(m) for every system in the sequence (no memory between solves).
        crd::usize gmres_total = 0;
        for (double s : shifts)
        {
            auto                       a = cluster_shifted<crd::f64>(&alloc, n, nsmall, s);
            ParallelSparseLinearOp<crd::f64> op(a, &alloc);
            dense::Vector<crd::f64>    x(&alloc, n);
            GmresWorkspace<crd::f64>   wg(&alloc, n, /*restart=*/8);
            auto                       r = gmres<crd::f64>(op, b.span(), x.span(), opts, wg, &alloc);
            REQUIRE(r.converged);
            gmres_total += r.iterations;
        }

        // GCROT with a PERSISTENT recycle space carried across the whole sequence.
        crd::usize               gcrot_total = 0;
        RecycleSpace<crd::f64>   rs(&alloc, n, /*recycle=*/8);
        GcrotWorkspace<crd::f64> wc(&alloc, n, /*inner=*/8, /*recycle=*/8);
        for (double s : shifts)
        {
            auto                       a = cluster_shifted<crd::f64>(&alloc, n, nsmall, s);
            ParallelSparseLinearOp<crd::f64> op(a, &alloc);
            dense::Vector<crd::f64>    x(&alloc, n);
            auto                       r = gcrot_recycled<crd::f64>(op, b.span(), x.span(), opts, wc, rs, &alloc);
            REQUIRE(r.converged);
            gcrot_total += r.iterations;
        }

        // Solve 1 ties (empty recycle space); solves 2..N ride the accumulated
        // deflation space ⇒ the sequence TOTAL is decisively fewer iterations.
        REQUIRE(gcrot_total < gmres_total * 7 / 10);
    }
    crd::jobs::shutdown();
}

TEST_CASE("GCROT(m,k) is bit-exact over serial vs parallel spmv (determinism moat)",
          "[hesap-iterative][gcrot][determinism]")
{
    crd::jobs::init();
    {
        crd::memory::TlsfAllocator alloc{128U << 20};
        const crd::u32             n = 300;
        auto                       a = random_nonsym<crd::f64>(&alloc, n, /*seed=*/0x9CC9A7ULL);
        ParallelSparseLinearOp<crd::f64> serial_op(a, &alloc, /*parallel_min_stored_bytes=*/~crd::usize{0});
        ParallelSparseLinearOp<crd::f64> parallel_op(a, &alloc, /*parallel_min_stored_bytes=*/0);
        REQUIRE_FALSE(serial_op.is_parallel());
        REQUIRE(parallel_op.is_parallel());
        dense::Vector<crd::f64> b(&alloc, n);
        b.fill(1.0);

        auto solve = [&](const crd::hesap::LinearOp<crd::f64>& op, dense::Vector<crd::f64>& x) {
            IterativeOptions<crd::f64> opts;
            opts.rel_tol          = 1e-12;
            opts.max_iter         = 800;
            opts.record_residuals = true;
            GcrotWorkspace<crd::f64> ws(&alloc, n, /*inner=*/20, /*recycle=*/10);
            return gcrot<crd::f64>(op, b.span(), x.span(), opts, ws, &alloc);
        };
        dense::Vector<crd::f64> xs(&alloc, n);
        dense::Vector<crd::f64> xp(&alloc, n);
        auto                    rs = solve(serial_op, xs);
        auto                    rp = solve(parallel_op, xp);
        REQUIRE(rs.iterations == rp.iterations);
        REQUIRE(rs.residual_history.size() == rp.residual_history.size());
        for (crd::usize i = 0; i < rs.residual_history.size(); ++i)
        {
            REQUIRE(rs.residual_history[i] == rp.residual_history[i]);
        }
        for (crd::u32 i = 0; i < n; ++i) { REQUIRE(xs(i) == xp(i)); }
    }
    crd::jobs::shutdown();
}
