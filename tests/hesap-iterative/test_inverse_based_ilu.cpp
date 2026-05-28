#include <crd/containers/span.hpp>
#include <crd/hesap/complex.hpp>
#include <crd/hesap/dense/vector.hpp>
#include <crd/hesap/iterative/bicgstab.hpp>
#include <crd/hesap/preconditioners/inverse_based_ilu.hpp>
#include <crd/hesap/sparse/parallel_sparse_linear_op.hpp>
#include <crd/hesap/sparse/sparse_linear_op.hpp>
#include <crd/hesap/sparse/triplet_builder.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>
#include <cmath>

using namespace crd::hesap::iterative;
using namespace crd::hesap::sparse;
using crd::hesap::preconditioners::InverseBasedIlu;
namespace dense = crd::hesap::dense;

namespace
{
// A well-scaled, diagonally-dominant nonsymmetric tridiagonal: every pivot is
// accepted (the inverse factor never grows past kappa).
template <typename T> SparseMatrix<T, SparseFormat::Csr> diag_dominant(crd::memory::IAllocator* a, crd::u32 n)
{
    TripletBuilder<T> b(a, n, n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        b.add(i, i, T(4));
        if (i > 0)
        {
            b.add(i, i - 1, T(static_cast<crd::f64>(-1.0)));
        }
        if (i + 1 < n)
        {
            b.add(i, i + 1, T(static_cast<crd::f64>(-0.7)));
        }
    }
    return b.compress();
}

// 2D anisotropic 5-point stencil on a gridN×gridN mesh (n=gridN²): weak x-coupling
// (eps), strong y-coupling (1). Inverse-based pivoting SEMI-COARSENS this ⇒ a
// genuine multilevel hierarchy (>2 levels) — the AMG-like behaviour. eps≈1 ⇒
// isotropic Laplacian (coarsens in one step). Small eps ⇒ deep recursion.
template <typename T>
SparseMatrix<T, SparseFormat::Csr> aniso2d(crd::memory::IAllocator* a, crd::u32 gridN, crd::f64 eps, crd::f64 beta)
{
    const crd::u32 n = gridN * gridN;
    TripletBuilder<T> b(a, n, n);
    auto idx = [gridN](crd::u32 i, crd::u32 j)
    {
        return i * gridN + j;
    };
    for (crd::u32 i = 0; i < gridN; ++i)
    {
        for (crd::u32 j = 0; j < gridN; ++j)
        {
            const crd::u32 r = idx(i, j);
            b.add(r, r, T(static_cast<crd::f64>(2.0 * eps + 2.0)));
            if (j > 0)
            {
                b.add(r, idx(i, j - 1), T((-eps - beta)));
            } // west
            if (j + 1 < gridN)
            {
                b.add(r, idx(i, j + 1), T((-eps + beta)));
            } // east
            if (i > 0)
            {
                b.add(r, idx(i - 1, j), T(static_cast<crd::f64>(-1.0)));
            }
            if (i + 1 < gridN)
            {
                b.add(r, idx(i + 1, j), T(static_cast<crd::f64>(-1.0)));
            }
        }
    }
    return b.compress();
}

template <typename T>
double true_resid(SparseLinearOp<T>& op, const dense::Vector<T>& b, const dense::Vector<T>& x,
                  crd::memory::IAllocator* alloc, crd::u32 n)
{
    dense::Vector<T> ax(alloc, n);
    (void)op.apply(x.span(), ax.span());
    double nb = 0;
    double nr = 0;
    for (crd::u32 i = 0; i < n; ++i)
    {
        const auto bi = b(i);
        nb += double(bi) * bi;
        const auto d = b(i) - ax(i);
        nr += double(d) * d;
    }
    return std::sqrt(nr / nb);
}
} // namespace

TEST_CASE("InverseBasedIlu accepts every pivot on a diagonally dominant matrix and solves",
          "[hesap-iterative][inverse_based_ilu]")
{
    crd::jobs::init();
    {
        crd::memory::TlsfAllocator alloc{64U << 20};
        const crd::u32 n = 400;
        auto a = diag_dominant<crd::f64>(&alloc, n);
        SparseLinearOp<crd::f64> op(a);
        InverseBasedIlu<crd::f64> m(a, &alloc, 5.0, 1e-3);
        // diagonally dominant ⇒ inverse factors stay tiny ⇒ no deferral, one level.
        REQUIRE(m.num_deferred() == 0);
        REQUIRE(m.num_levels() == 1);
        dense::Vector<crd::f64> b(&alloc, n);
        dense::Vector<crd::f64> x(&alloc, n);
        for (crd::u32 i = 0; i < n; ++i)
        {
            b(i) = 1.0 + 0.01 * static_cast<crd::f64>(i % 7);
        }
        IterativeOptions<crd::f64> opts;
        opts.rel_tol = 1e-10;
        opts.max_iter = 2000;
        BicgstabWorkspace<crd::f64> ws(&alloc, n);
        auto res = bicgstab<crd::f64>(op, &m, b.span(), x.span(), opts, ws, &alloc);
        REQUIRE(res.converged);
        REQUIRE(true_resid<crd::f64>(op, b, x, &alloc, n) < 1e-8);
    }
    crd::jobs::shutdown();
}

TEST_CASE("InverseBasedIlu defers pivots when kappa is tight and still solves (the defer path)",
          "[hesap-iterative][inverse_based_ilu]")
{
    crd::jobs::init();
    {
        crd::memory::TlsfAllocator alloc{64U << 20};
        const crd::u32 n = 400;
        auto a = diag_dominant<crd::f64>(&alloc, n);
        SparseLinearOp<crd::f64> op(a);
        // a tight kappa forces a fraction of pivots to be rejected to the Schur leaf.
        // reorder=false: this exercises the natural-order defer mechanic (exact counts).
        InverseBasedIlu<crd::f64> m(a, &alloc, 1.05, 1e-3, 0U, 50U, 64U, crd::hesap::preconditioners::Mc64Mode::None,
                                    0.0, /*reorder=*/false);
        REQUIRE(m.num_deferred() > 0); // the defer path actually fires
        REQUIRE(m.num_levels() >= 2);  // recursion / dense base engaged
        REQUIRE(m.num_accepted() + m.num_deferred() == n);
        dense::Vector<crd::f64> b(&alloc, n);
        dense::Vector<crd::f64> x(&alloc, n);
        for (crd::u32 i = 0; i < n; ++i)
        {
            b(i) = 1.0;
        }
        IterativeOptions<crd::f64> opts;
        opts.rel_tol = 1e-9;
        opts.max_iter = 4000;
        BicgstabWorkspace<crd::f64> ws(&alloc, n);
        auto res = bicgstab<crd::f64>(op, &m, b.span(), x.span(), opts, ws, &alloc);
        REQUIRE(res.converged);
        REQUIRE(true_resid<crd::f64>(op, b, x, &alloc, n) < 1e-7);
    }
    crd::jobs::shutdown();
}

TEST_CASE("InverseBasedIlu recurses into a multilevel hierarchy on an anisotropic 2D stencil (v4j-2b)",
          "[hesap-iterative][inverse_based_ilu]")
{
    crd::jobs::init();
    {
        crd::memory::TlsfAllocator alloc{256U << 20};
        const crd::u32 g = 50; // 50x50 grid → n=2500
        auto a = aniso2d<crd::f64>(&alloc, g, 1e-2, 0.3);
        const crd::u32 n = a.rows();
        SparseLinearOp<crd::f64> op(a);
        // anisotropy + tight κ ⇒ semi-coarsening defers a chunk at SEVERAL levels:
        // a genuine multilevel hierarchy (>2 levels), not just B + one leaf.
        // reorder=false: the v4j-2b natural-order multilevel regression (reorder deepens further).
        InverseBasedIlu<crd::f64> m(a, &alloc, 1.1, 1e-2, 0U, 50U, 64U, crd::hesap::preconditioners::Mc64Mode::None,
                                    0.0, /*reorder=*/false);
        REQUIRE(m.num_levels() > 2);
        REQUIRE(m.num_deferred() > 0);
        dense::Vector<crd::f64> b(&alloc, n);
        dense::Vector<crd::f64> x(&alloc, n);
        for (crd::u32 i = 0; i < n; ++i)
        {
            b(i) = 1.0;
        }
        IterativeOptions<crd::f64> opts;
        opts.rel_tol = 1e-9;
        opts.max_iter = 2000;
        BicgstabWorkspace<crd::f64> ws(&alloc, n);
        auto res = bicgstab<crd::f64>(op, &m, b.span(), x.span(), opts, ws, &alloc);
        REQUIRE(res.converged);
        REQUIRE(true_resid<crd::f64>(op, b, x, &alloc, n) < 1e-7);
    }
    crd::jobs::shutdown();
}

TEST_CASE("InverseBasedIlu is bit-exact over serial vs parallel spmv (determinism moat)",
          "[hesap-iterative][inverse_based_ilu][determinism]")
{
    crd::jobs::init();
    {
        crd::memory::TlsfAllocator alloc{96U << 20};
        const crd::u32 n = 500;
        auto a = diag_dominant<crd::f64>(&alloc, n);
        ParallelSparseLinearOp<crd::f64> serial_op(a, &alloc, ~crd::usize{0});
        ParallelSparseLinearOp<crd::f64> parallel_op(a, &alloc, 0);
        dense::Vector<crd::f64> b(&alloc, n);
        for (crd::u32 i = 0; i < n; ++i)
        {
            b(i) = 1.0 + 0.001 * static_cast<crd::f64>(i % 11);
        }
        auto solve = [&](const crd::hesap::LinearOp<crd::f64>& opx, dense::Vector<crd::f64>& x)
        {
            InverseBasedIlu<crd::f64> m(a, &alloc, 3.0, 1e-3);
            IterativeOptions<crd::f64> opts;
            opts.rel_tol = 1e-10;
            opts.max_iter = 4000;
            BicgstabWorkspace<crd::f64> ws(&alloc, n);
            return bicgstab<crd::f64>(opx, &m, b.span(), x.span(), opts, ws, &alloc);
        };
        dense::Vector<crd::f64> xs(&alloc, n);
        dense::Vector<crd::f64> xp(&alloc, n);
        auto rs = solve(serial_op, xs);
        auto rp = solve(parallel_op, xp);
        REQUIRE(rs.iterations == rp.iterations);
        for (crd::u32 i = 0; i < n; ++i)
        {
            REQUIRE(xs(i) == xp(i));
        }
    }
    crd::jobs::shutdown();
}

namespace
{
using C = crd::hesap::Complex<crd::f64>;
// Complex diagonally-dominant nonsymmetric tridiagonal with IMAGINARY off-diagonals (so Aᴴ ≠ Aᵀ
// — the adjoint test genuinely exercises conjugation, not just transposition).
SparseMatrix<C, SparseFormat::Csr> complex_tridiag(crd::memory::IAllocator* a, crd::u32 n)
{
    TripletBuilder<C> b(a, n, n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        b.add(i, i, C{4.0, 0.5});
        if (i > 0)
        {
            b.add(i, i - 1, C{-1.0, 0.3});
        }
        if (i + 1 < n)
        {
            b.add(i, i + 1, C{-0.7, -0.2});
        }
    }
    return b.compress();
}
} // namespace

TEST_CASE("InverseBasedIlu solves a complex non-Hermitian system (complex completeness)",
          "[hesap-iterative][inverse_based_ilu][complex]")
{
    crd::jobs::init();
    {
        crd::memory::TlsfAllocator alloc{64U << 20};
        const crd::u32 n = 300;
        auto a = complex_tridiag(&alloc, n);
        SparseLinearOp<C> op(a);
        InverseBasedIlu<C> m(a, &alloc, 5.0, 1e-3);
        dense::Vector<C> b(&alloc, n);
        dense::Vector<C> x(&alloc, n);
        for (crd::u32 i = 0; i < n; ++i)
        {
            b(i) = C{1.0, 0.1 * static_cast<crd::f64>(i % 5)};
        }
        IterativeOptions<crd::f64> opts;
        opts.rel_tol = 1e-9;
        opts.max_iter = 2000;
        BicgstabWorkspace<C> ws(&alloc, n);
        auto res = bicgstab<C>(op, &m, b.span(), x.span(), opts, ws, &alloc);
        REQUIRE(res.converged);
        dense::Vector<C> ax(&alloc, n);
        (void)op.apply(x.span(), ax.span());
        double nb = 0;
        double nr = 0;
        for (crd::u32 i = 0; i < n; ++i)
        {
            nb += double(b(i).re) * b(i).re + double(b(i).im) * b(i).im;
            const C d = b(i) - ax(i);
            nr += double(d.re) * d.re + double(d.im) * d.im;
        }
        REQUIRE(std::sqrt(nr / nb) < 1e-7);
    }
    crd::jobs::shutdown();
}

TEST_CASE("InverseBasedIlu per-level AMD reorder: solves + deepens + adjoint identity holds (v4j-3a)",
          "[hesap-iterative][inverse_based_ilu][reorder]")
{
    crd::jobs::init();
    {
        crd::memory::TlsfAllocator alloc{256U << 20};
        const crd::u32 g = 50; // 50x50 anisotropic conv-diff, n=2500
        auto a = aniso2d<crd::f64>(&alloc, g, 1e-2, 0.3);
        const crd::u32 n = a.rows();
        SparseLinearOp<crd::f64> op(a);
        // reorder=true (the last ctor arg): per-level AMD reorder ⇒ deeper hierarchy.
        InverseBasedIlu<crd::f64> m(a, &alloc, 5.0, 1e-2, 0U, 50U, 64U, crd::hesap::preconditioners::Mc64Mode::None,
                                    0.0, /*reorder=*/true);
        InverseBasedIlu<crd::f64> m_nat(a, &alloc, 5.0, 1e-2, 0U, 50U, 64U, crd::hesap::preconditioners::Mc64Mode::None,
                                        0.0,
                                        /*reorder=*/false); // natural order baseline (explicit since default is now ON)
        REQUIRE(m.num_levels() >= m_nat.num_levels());      // reorder keeps the Schur hard ⇒ at least as deep
        dense::Vector<crd::f64> b(&alloc, n);
        dense::Vector<crd::f64> x(&alloc, n);
        for (crd::u32 i = 0; i < n; ++i)
        {
            b(i) = 1.0;
        }
        IterativeOptions<crd::f64> opts;
        opts.rel_tol = 1e-9;
        opts.max_iter = 2000;
        BicgstabWorkspace<crd::f64> ws(&alloc, n);
        auto res = bicgstab<crd::f64>(op, &m, b.span(), x.span(), opts, ws, &alloc);
        REQUIRE(res.converged);
        REQUIRE(true_resid<crd::f64>(op, b, x, &alloc, n) < 1e-7);
    }
    crd::jobs::shutdown();
}

TEST_CASE("InverseBasedIlu reorder: apply_adjoint is the true conjugate-transpose (adjoint identity)",
          "[hesap-iterative][inverse_based_ilu][reorder][complex]")
{
    crd::jobs::init();
    {
        crd::memory::TlsfAllocator alloc{64U << 20};
        const crd::u32 n = 200;
        auto a = complex_tridiag(&alloc, n);
        InverseBasedIlu<C> m(a, &alloc, 1.1, 1e-2, 0U, 50U, 64U, crd::hesap::preconditioners::Mc64Mode::None, 0.0,
                             /*reorder=*/true);
        dense::Vector<C> x(&alloc, n);
        dense::Vector<C> y(&alloc, n);
        dense::Vector<C> u(&alloc, n);
        dense::Vector<C> v(&alloc, n);
        for (crd::u32 i = 0; i < n; ++i)
        {
            x(i) = C{1.0 + 0.01 * static_cast<crd::f64>(i % 7), -0.02 * static_cast<crd::f64>(i % 5)};
            y(i) = C{0.5 - 0.03 * static_cast<crd::f64>(i % 4), 0.04 * static_cast<crd::f64>(i % 6)};
        }
        (void)m.apply(x.span(), u.span());
        (void)m.apply_adjoint(y.span(), v.span());
        C lhs{0.0, 0.0};
        C rhs{0.0, 0.0};
        for (crd::u32 i = 0; i < n; ++i)
        {
            lhs = lhs + C{y(i).re, -y(i).im} * u(i);
            rhs = rhs + C{v(i).re, -v(i).im} * x(i);
        }
        const double err = std::sqrt((lhs.re - rhs.re) * (lhs.re - rhs.re) + (lhs.im - rhs.im) * (lhs.im - rhs.im));
        const double mag = std::sqrt(lhs.re * lhs.re + lhs.im * lhs.im);
        REQUIRE(err <= 1e-9 * (mag + 1.0)); // adjoint identity holds through the symmetric reorder
    }
    crd::jobs::shutdown();
}

TEST_CASE("InverseBasedIlu apply_adjoint is the true conjugate-transpose of apply (the adjoint identity)",
          "[hesap-iterative][inverse_based_ilu][complex]")
{
    crd::jobs::init();
    {
        crd::memory::TlsfAllocator alloc{64U << 20};
        const crd::u32 n = 200;
        auto a = complex_tridiag(&alloc, n);
        // tight κ ⇒ deferral ⇒ the full structure (Lᴱ/Uꜰ conj-transposes + DenseLuLeaf adjoint) is exercised.
        // reorder=false: the natural-order adjoint path (the reorder adjoint path has its own case above).
        InverseBasedIlu<C> m(a, &alloc, 1.1, 1e-2, 0U, 50U, 64U, crd::hesap::preconditioners::Mc64Mode::None, 0.0,
                             /*reorder=*/false);
        REQUIRE(m.num_deferred() > 0);
        dense::Vector<C> x(&alloc, n);
        dense::Vector<C> y(&alloc, n);
        dense::Vector<C> u(&alloc, n);
        dense::Vector<C> v(&alloc, n);
        for (crd::u32 i = 0; i < n; ++i)
        {
            x(i) = C{1.0 + 0.01 * static_cast<crd::f64>(i % 7), -0.02 * static_cast<crd::f64>(i % 5)};
            y(i) = C{0.5 - 0.03 * static_cast<crd::f64>(i % 4), 0.04 * static_cast<crd::f64>(i % 6)};
        }
        (void)m.apply(x.span(), u.span());         // u = M⁻¹ x
        (void)m.apply_adjoint(y.span(), v.span()); // v = M⁻ᴴ y
        // ⟨y, M⁻¹x⟩ = ⟨M⁻ᴴy, x⟩  ⇔  Σ conj(y)·u  ==  Σ conj(v)·x
        C lhs{0.0, 0.0};
        C rhs{0.0, 0.0};
        for (crd::u32 i = 0; i < n; ++i)
        {
            lhs = lhs + C{y(i).re, -y(i).im} * u(i);
            rhs = rhs + C{v(i).re, -v(i).im} * x(i);
        }
        const double err = std::sqrt((lhs.re - rhs.re) * (lhs.re - rhs.re) + (lhs.im - rhs.im) * (lhs.im - rhs.im));
        const double mag = std::sqrt(lhs.re * lhs.re + lhs.im * lhs.im);
        REQUIRE(err <= 1e-9 * (mag + 1.0)); // adjoint identity holds to round-off
    }
    crd::jobs::shutdown();
}

TEST_CASE("DenseLuLeaf gracefully degrades on a singular Schur leaf (v4z robustness: never asserts)",
          "[hesap-iterative][inverse_based_ilu][robustness]")
{
    crd::jobs::init();
    {
        crd::memory::TlsfAllocator alloc{4U << 20};
        // A structurally singular 4x4 (row 1 is entirely zero) -- exactly the kind of
        // deferred coarsest block a non-PDE matrix (e.g. the power-circuit gemat11) drives
        // the multilevel ILU into. Before v4z this asserted in solve_lu ("factor is
        // singular"); now the dense leaf shifts the diagonal by a relative pivot floor and
        // refactors, so the preconditioner stays applicable (degrades, never aborts).
        const crd::u32 n = 4;
        TripletBuilder<crd::f64> tb(&alloc, n, n);
        tb.add(0, 0, 2.0);
        // row 1: no entries (zero row) => the factor is singular
        tb.add(2, 2, 3.0);
        tb.add(3, 3, 4.0);
        tb.add(0, 3, 0.5);
        auto s = tb.compress();
        crd::hesap::preconditioners::DenseLuLeaf<crd::f64> leaf(s, &alloc);
        dense::Vector<crd::f64> r(&alloc, n);
        dense::Vector<crd::f64> z(&alloc, n);
        dense::Vector<crd::f64> za(&alloc, n);
        for (crd::u32 i = 0; i < n; ++i)
        {
            r(i) = 1.0;
        }
        REQUIRE(leaf.apply(r.span(), z.span()));          // does not assert / crash
        REQUIRE(leaf.apply_adjoint(r.span(), za.span())); // adjoint path likewise degrades
        for (crd::u32 i = 0; i < n; ++i)
        {
            REQUIRE(std::isfinite(z(i)));
            REQUIRE(std::isfinite(za(i)));
        }

        // f32 too: the relative pivot floor √ε·max|diag| is larger in single precision,
        // so confirm the same degrade path stays finite (no over-perturb to NaN/Inf).
        TripletBuilder<crd::f32> tb32(&alloc, n, n);
        tb32.add(0, 0, 2.0F);
        tb32.add(2, 2, 3.0F);
        tb32.add(3, 3, 4.0F);
        tb32.add(0, 3, 0.5F);
        auto s32 = tb32.compress();
        crd::hesap::preconditioners::DenseLuLeaf<crd::f32> leaf32(s32, &alloc);
        dense::Vector<crd::f32> r32(&alloc, n);
        dense::Vector<crd::f32> z32(&alloc, n);
        for (crd::u32 i = 0; i < n; ++i)
        {
            r32(i) = 1.0F;
        }
        REQUIRE(leaf32.apply(r32.span(), z32.span()));
        for (crd::u32 i = 0; i < n; ++i)
        {
            REQUIRE(std::isfinite(z32(i)));
        }
    }
    crd::jobs::shutdown();
}
