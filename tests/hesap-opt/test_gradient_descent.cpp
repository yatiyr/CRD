// crd-hesap-opt v7-a — substrate + the first end-to-end optimizer (steepest descent). Validates: (1) the
// QuadraticObjective value/gradient/Hessian-vector against hand-computed values; (2) gradient descent converges
// to the minimizer x* = A^-1 b of a WELL-CONDITIONED SPD quadratic (kappa ~ 1.5, so it genuinely reaches the
// gradient tolerance well under max_iters) — asserting status==Success, NOT just that it ran (a maxed-out run
// would pass a bit-identity moat vacuously); (3) run-twice bit-identity (determinism by construction);
// (4) the {1,2,4,8,16} determinism moat over a FORCED-parallel objective eval — bit-identical x AND Success on
// every worker count.

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

// Well-conditioned SPD tridiagonal A = tridiag(-0.1, 1, -0.1): eigenvalues in [0.8, 1.2], kappa ~ 1.5 ⇒ steepest
// descent converges in a handful of iterations (the advisor's "converge by construction" substrate test).
Csr well_conditioned_spd(crd::memory::IAllocator* a, crd::u32 n)
{
    sp::TripletBuilder<crd::f64> tb(a, n, n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        tb.add(i, i, 1.0);
        if (i + 1 < n)
        {
            tb.add(i, i + 1, -0.1);
            tb.add(i + 1, i, -0.1);
        }
    }
    return tb.compress();
}
} // namespace

TEST_CASE("v7-a QuadraticObjective value/gradient/Hessian-vector are correct", "[hesap][opt][v7]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    const crd::u32 n = 12;
    Csr a = well_conditioned_spd(&alloc, n);
    sp::SparseLinearOp<crd::f64> op(a);

    crd::containers::Array<crd::f64> xtrue(&alloc);
    crd::containers::Array<crd::f64> b(&alloc);
    xtrue.resize(n);
    b.resize(n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        xtrue[i] = 1.0 + 0.1 * static_cast<crd::f64>(i);
    }
    (void)op.apply({xtrue.data(), n}, {b.data(), n}); // b = A·xtrue ⇒ x* = xtrue

    opt::QuadraticObjective<crd::f64> obj(op, {b.data(), n}, &alloc);
    REQUIRE(obj.n() == n);
    REQUIRE(obj.has_gradient());
    REQUIRE(obj.has_hessian_vector());

    // ∇f(x*) = A·x* − b = 0.
    crd::containers::Array<crd::f64> g(&alloc);
    g.resize(n);
    REQUIRE(obj.gradient({xtrue.data(), n}, {g.data(), n}));
    crd::f64 gnorm = 0.0;
    for (crd::u32 i = 0; i < n; ++i)
    {
        gnorm = std::max(gnorm, std::fabs(g[i]));
    }
    CHECK(gnorm < 1e-12);

    // f(x*) = ½ x*ᵀA x* − bᵀx* = −½ bᵀx* (since A x* = b).
    crd::f64 bx = 0.0;
    for (crd::u32 i = 0; i < n; ++i)
    {
        bx += b[i] * xtrue[i];
    }
    CHECK(std::fabs(obj.value({xtrue.data(), n}) - (-0.5 * bx)) < 1e-10);

    // Hessian-vector = A·v (Hessian of a quadratic is A).
    crd::containers::Array<crd::f64> v(&alloc);
    crd::containers::Array<crd::f64> hv(&alloc);
    crd::containers::Array<crd::f64> av(&alloc);
    v.resize(n);
    hv.resize(n);
    av.resize(n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        v[i] = static_cast<crd::f64>((i % 3) + 1);
    }
    REQUIRE(obj.hessian_vector({xtrue.data(), n}, {v.data(), n}, {hv.data(), n}));
    (void)op.apply({v.data(), n}, {av.data(), n});
    for (crd::u32 i = 0; i < n; ++i)
    {
        CHECK(std::fabs(hv[i] - av[i]) < 1e-12);
    }
}

TEST_CASE("v7-a gradient descent converges to the quadratic minimizer A^-1 b", "[hesap][opt][v7]")
{
    crd::memory::TlsfAllocator alloc(1U << 23);
    const crd::u32 n = 20;
    Csr a = well_conditioned_spd(&alloc, n);
    sp::SparseLinearOp<crd::f64> op(a);

    crd::containers::Array<crd::f64> xtrue(&alloc);
    crd::containers::Array<crd::f64> b(&alloc);
    crd::containers::Array<crd::f64> x0(&alloc);
    xtrue.resize(n);
    b.resize(n);
    x0.resize(n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        xtrue[i] = 1.0 + 0.1 * static_cast<crd::f64>(i);
        x0[i] = 0.0;
    }
    (void)op.apply({xtrue.data(), n}, {b.data(), n});

    opt::QuadraticObjective<crd::f64> obj(op, {b.data(), n}, &alloc);
    opt::OptOptions<crd::f64> opts;
    opts.grad_tol = 1e-8;
    opts.max_iters = 2000;
    auto r = opt::minimize_gradient_descent<crd::f64>(obj, {x0.data(), n}, opts, &alloc);

    REQUIRE(r.status == opt::OptStatus::Success); // genuinely converged, not maxed out
    REQUIRE(r.converged);
    crd::f64 err = 0.0;
    for (crd::u32 i = 0; i < n; ++i)
    {
        err = std::max(err, std::fabs(r.x[i] - xtrue[i]));
    }
    CHECK(err < 1e-6); // recovered x* = A^-1 b = xtrue

    // run-twice bit-identity (determinism by construction).
    auto r2 = opt::minimize_gradient_descent<crd::f64>(obj, {x0.data(), n}, opts, &alloc);
    REQUIRE(r2.x.size() == r.x.size());
    bool ident = (r2.iterations == r.iterations);
    for (crd::u32 i = 0; i < n && ident; ++i)
    {
        ident = (r2.x[i] == r.x[i]);
    }
    CHECK(ident);
}

TEST_CASE("v7-a gradient descent determinism moat {1,2,4,8,16} (forced-parallel objective eval)",
          "[hesap][opt][v7][moat]")
{
    const crd::u32 n = 32;
    crd::memory::TlsfAllocator alloc(1U << 24);
    Csr a = well_conditioned_spd(&alloc, n);
    sp::SparseLinearOp<crd::f64> serial_op(a);

    crd::containers::Array<crd::f64> xtrue(&alloc);
    crd::containers::Array<crd::f64> b(&alloc);
    crd::containers::Array<crd::f64> x0(&alloc);
    xtrue.resize(n);
    b.resize(n);
    x0.resize(n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        xtrue[i] = 1.0 + 0.05 * static_cast<crd::f64>(i);
        x0[i] = 0.0;
    }
    (void)serial_op.apply({xtrue.data(), n}, {b.data(), n});

    opt::OptOptions<crd::f64> opts;
    opts.grad_tol = 1e-8;
    opts.max_iters = 2000;

    crd::containers::Array<crd::f64> x_ref(&alloc);
    bool have_ref = false;
    crd::usize iters_ref = 0;
    for (crd::u32 nw : {1U, 2U, 4U, 8U, 16U})
    {
        crd::jobs::Config cfg;
        cfg.num_threads = nw;
        crd::jobs::init(cfg);
        {
            // Forced-parallel objective eval (A·x bit-exact across worker counts) ⇒ bit-identical trajectory.
            sp::ParallelSparseLinearOp<crd::f64> op(a, &alloc, /*parallel_min_stored_bytes=*/0);
            opt::QuadraticObjective<crd::f64> obj(op, {b.data(), n}, &alloc);
            auto r = opt::minimize_gradient_descent<crd::f64>(obj, {x0.data(), n}, opts, &alloc);
            REQUIRE(r.status == opt::OptStatus::Success); // a maxed-out run would make the moat vacuous
            if (!have_ref)
            {
                x_ref.resize(r.x.size());
                for (crd::u32 i = 0; i < r.x.size(); ++i)
                {
                    x_ref[i] = r.x[i];
                }
                iters_ref = r.iterations;
                have_ref = true;
            }
            else
            {
                bool ident = (r.iterations == iters_ref) && (r.x.size() == x_ref.size());
                for (crd::u32 i = 0; i < r.x.size() && ident; ++i)
                {
                    ident = (r.x[i] == x_ref[i]);
                }
                CHECK(ident); // optimization trajectory bit-identical across worker counts
            }
        }
        crd::jobs::shutdown();
    }
}
