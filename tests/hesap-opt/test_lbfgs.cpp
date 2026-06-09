// crd-hesap-opt v7-d — quasi-Newton (L-BFGS + dense BFGS + SR1). Validates: (1) L-BFGS converges on Rosenbrock-N
// (the classic nonlinear test; gradient via v7-b forward-AD) and on an ill-conditioned quadratic (superlinear —
// far fewer iters than steepest descent would need); (2) dense BFGS + SR1 converge; (3) eval-count instrumentation
// is populated (the L-BFGS verdict metric vs liblbfgs); (4) the determinism moat {1,2,4,8,16} — NON-VACUOUS: the
// problem runs MORE than m iterations so the (s,y) ring buffer fills and WRAPS (the two-loop is genuinely
// exercised), and x*/iterations are bit-identical across worker counts.

#include <crd/hesap/opt/opt.hpp>
#include <crd/hesap/sparse/parallel_sparse_linear_op.hpp>
#include <crd/hesap/sparse/sparse_linear_op.hpp>
#include <crd/hesap/sparse/triplet_builder.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>
#include <cmath>

namespace opt = crd::hesap::opt;
namespace sp = crd::hesap::sparse;

namespace
{
using Csr = sp::SparseMatrix<crd::f64, sp::SparseFormat::Csr>;

// 1-D discrete Laplacian tridiag(−1, 2, −1): SPD, κ ~ O(n²) (n=64 ⇒ κ ~ 1700) — ill-conditioned enough that
// L-BFGS needs many iterations (the ring buffer wraps) while staying a clean SPD quadratic with known minimizer.
Csr laplacian_1d(crd::memory::IAllocator* a, crd::u32 n)
{
    sp::TripletBuilder<crd::f64> tb(a, n, n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        tb.add(i, i, 2.0);
        if (i + 1 < n)
        {
            tb.add(i, i + 1, -1.0);
            tb.add(i + 1, i, -1.0);
        }
    }
    return tb.compress();
}

// Extended Rosenbrock: Σ [(1−x_i)² + 100(x_{i+1}−x_i²)²]; minimizer = all ones, f* = 0. Scalar-generic ⇒ its
// gradient is exact forward-mode AD (v7-b).
struct RosenbrockN
{
    template <typename S>
    S operator()(crd::containers::ConstSpan<S> x) const
    {
        S acc = S(0);
        for (crd::usize i = 0; i + 1 < x.size(); ++i)
        {
            const S a = S(1) - x[i];
            const S b = x[i + 1] - x[i] * x[i];
            acc = acc + a * a + S(100) * (b * b);
        }
        return acc;
    }
};
} // namespace

TEST_CASE("v7-d L-BFGS converges on Rosenbrock-N", "[hesap][opt][v7]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    const crd::usize dim = 10;
    auto obj = opt::make_objective_from_functor<crd::f64>(RosenbrockN{}, dim, &alloc);

    crd::containers::Array<crd::f64> x0(&alloc);
    x0.resize(dim);
    for (crd::usize i = 0; i < dim; ++i)
    {
        x0[i] = (i % 2 == 0) ? -1.2 : 1.0; // the classic hard start
    }

    opt::OptOptions<crd::f64> opts;
    opts.grad_tol = 1e-6;
    opts.max_iters = 500;
    auto r = opt::minimize_lbfgs<crd::f64>(obj, {x0.data(), dim}, opts, &alloc, nullptr, /*memory=*/8);

    REQUIRE(r.status == opt::OptStatus::Success);
    crd::f64 err = 0.0;
    for (crd::usize i = 0; i < dim; ++i)
    {
        err = std::max(err, std::fabs(r.x[i] - 1.0));
    }
    CHECK(err < 1e-4);
    CHECK(r.fx < 1e-8);
    CHECK(r.fn_evals > 0);   // eval-count instrumentation populated (the verdict metric vs liblbfgs)
    CHECK(r.grad_evals > 0);
}

TEST_CASE("v7-d L-BFGS converges on an ill-conditioned quadratic (superlinear)", "[hesap][opt][v7]")
{
    crd::memory::TlsfAllocator alloc(1U << 23);
    const crd::u32 n = 64;
    Csr a = laplacian_1d(&alloc, n);
    sp::SparseLinearOp<crd::f64> op(a);

    crd::containers::Array<crd::f64> xtrue(&alloc);
    crd::containers::Array<crd::f64> b(&alloc);
    crd::containers::Array<crd::f64> x0(&alloc);
    xtrue.resize(n);
    b.resize(n);
    x0.resize(n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        xtrue[i] = 1.0 + 0.01 * static_cast<crd::f64>(i);
        x0[i] = 0.0;
    }
    (void)op.apply({xtrue.data(), n}, {b.data(), n});

    opt::QuadraticObjective<crd::f64> obj(op, {b.data(), n}, &alloc);
    opt::OptOptions<crd::f64> opts;
    opts.grad_tol = 1e-7;
    opts.max_iters = 1000;
    auto r = opt::minimize_lbfgs<crd::f64>(obj, {x0.data(), n}, opts, &alloc, nullptr, /*memory=*/8);

    REQUIRE(r.status == opt::OptStatus::Success);
    crd::f64 err = 0.0;
    for (crd::u32 i = 0; i < n; ++i)
    {
        err = std::max(err, std::fabs(r.x[i] - xtrue[i]));
    }
    CHECK(err < 1e-4);
    CHECK(r.iterations < 200); // far fewer than steepest descent (~√κ·log) would need on κ~1700
}

TEST_CASE("v7-d dense BFGS and SR1 converge on Rosenbrock", "[hesap][opt][v7]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    const crd::usize dim = 6;
    auto obj = opt::make_objective_from_functor<crd::f64>(RosenbrockN{}, dim, &alloc);

    crd::containers::Array<crd::f64> x0(&alloc);
    x0.resize(dim);
    for (crd::usize i = 0; i < dim; ++i)
    {
        x0[i] = (i % 2 == 0) ? -1.2 : 1.0;
    }

    opt::OptOptions<crd::f64> opts;
    opts.grad_tol = 1e-6;
    opts.max_iters = 1000;

    SECTION("BFGS")
    {
        auto r = opt::minimize_bfgs<crd::f64>(obj, {x0.data(), dim}, opts, &alloc);
        REQUIRE(r.status == opt::OptStatus::Success);
        crd::f64 err = 0.0;
        for (crd::usize i = 0; i < dim; ++i)
        {
            err = std::max(err, std::fabs(r.x[i] - 1.0));
        }
        CHECK(err < 1e-4);
    }
    SECTION("SR1")
    {
        auto r = opt::minimize_sr1<crd::f64>(obj, {x0.data(), dim}, opts, &alloc);
        REQUIRE(r.status == opt::OptStatus::Success);
        crd::f64 err = 0.0;
        for (crd::usize i = 0; i < dim; ++i)
        {
            err = std::max(err, std::fabs(r.x[i] - 1.0));
        }
        CHECK(err < 1e-4);
    }
}

TEST_CASE("v7-d L-BFGS determinism moat {1,2,4,8,16} (ring buffer wraps)", "[hesap][opt][v7][moat]")
{
    const crd::u32 n = 64;
    crd::memory::TlsfAllocator alloc(1U << 25);
    Csr a = laplacian_1d(&alloc, n);
    sp::SparseLinearOp<crd::f64> serial_op(a);

    crd::containers::Array<crd::f64> xtrue(&alloc);
    crd::containers::Array<crd::f64> b(&alloc);
    crd::containers::Array<crd::f64> x0(&alloc);
    xtrue.resize(n);
    b.resize(n);
    x0.resize(n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        xtrue[i] = 1.0 + 0.02 * static_cast<crd::f64>(i);
        x0[i] = 0.0;
    }
    (void)serial_op.apply({xtrue.data(), n}, {b.data(), n});

    opt::OptOptions<crd::f64> opts;
    opts.grad_tol = 1e-7;
    opts.max_iters = 1000;
    const crd::usize m = 5; // small ⇒ the κ~1700 problem runs >m iterations ⇒ the ring buffer wraps

    crd::containers::Array<crd::f64> x_ref(&alloc);
    bool have_ref = false;
    crd::usize iters_ref = 0;
    for (crd::u32 nw : {1U, 2U, 4U, 8U, 16U})
    {
        crd::jobs::Config cfg;
        cfg.num_threads = nw;
        crd::jobs::init(cfg);
        {
            sp::ParallelSparseLinearOp<crd::f64> op(a, &alloc, /*parallel_min_stored_bytes=*/0);
            opt::QuadraticObjective<crd::f64> obj(op, {b.data(), n}, &alloc);
            auto r = opt::minimize_lbfgs<crd::f64>(obj, {x0.data(), n}, opts, &alloc, nullptr, m);

            REQUIRE(r.status == opt::OptStatus::Success);
            REQUIRE(r.iterations > m); // the (s,y) ring buffer genuinely fills and wraps — moat not vacuous
            if (!have_ref)
            {
                x_ref.resize(r.x.size());
                for (crd::usize i = 0; i < r.x.size(); ++i)
                {
                    x_ref[i] = r.x[i];
                }
                iters_ref = r.iterations;
                have_ref = true;
            }
            else
            {
                bool ident = (r.iterations == iters_ref) && (r.x.size() == x_ref.size());
                for (crd::usize i = 0; i < r.x.size() && ident; ++i)
                {
                    ident = (r.x[i] == x_ref[i]);
                }
                CHECK(ident); // L-BFGS trajectory bit-identical across worker counts
            }
        }
        crd::jobs::shutdown();
    }
}
