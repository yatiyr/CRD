// crd-hesap-opt v7-f — nonlinear conjugate gradient (FR/PR+/HS/DY). Validates: (1) the SHARP per-variant
// correctness check — on an SPD quadratic with an EXACT line search, nonlinear CG reduces to LINEAR CG and so
// terminates in ≤ n iterations (finite termination holds ONLY under an exact line search — Nocedal & Wright; with
// the inexact default it merely drifts past n, which would falsely look buggy); (2) each variant converges on
// Rosenbrock-N with the default strong-Wolfe search, in an L-BFGS-class iteration count (cheap insurance against a
// β/restart bug the quadratic test passes trivially); (3) the determinism moat {1,2,4,8,16} — NON-VACUOUS (the
// κ~1700 problem runs many iterations) and bit-identical across worker counts.

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

// 1-D discrete Laplacian tridiag(−1, 2, −1): SPD, κ ~ O(n²). Reused for the quadratic + moat tests.
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

// Extended Rosenbrock: Σ [(1−x_i)² + 100(x_{i+1}−x_i²)²]; minimizer = all ones, f* = 0. Scalar-generic ⇒ exact
// forward-mode-AD gradient (v7-b).
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

// EXACT line search for a QUADRATIC objective (the finite-termination check needs it — nonlinear CG = linear CG
// only under an exact line search). For a quadratic f, ∇f(x+αp) = g + α·Ap, so Ap = ∇f(x+1·p) − g (one probe
// gradient) and the exact minimizer along p is α* = −(g·p)/(pᵀAp). Test-only (valid solely for quadratics).
template <typename T>
class ExactQuadraticLineSearch final : public opt::LineSearch<T>
{
public:
    [[nodiscard]] opt::LineSearchResult<T> search(const opt::Objective<T>& obj, crd::containers::ConstSpan<T> x, T fx,
                                                  crd::containers::ConstSpan<T> g, crd::containers::ConstSpan<T> p,
                                                  T /*alpha0*/, crd::containers::Span<T> x_out,
                                                  crd::containers::Span<T> g_out) const override
    {
        const crd::usize n = x.size();
        opt::LineSearchResult<T> r;
        T dphi0 = static_cast<T>(0);
        for (crd::usize i = 0; i < n; ++i)
        {
            dphi0 += g[i] * p[i];
        }
        for (crd::usize i = 0; i < n; ++i) // probe at x + 1·p
        {
            x_out[i] = x[i] + p[i];
        }
        (void)obj.gradient(x_out, g_out); // = g + Ap
        T pap = static_cast<T>(0);
        for (crd::usize i = 0; i < n; ++i)
        {
            pap += (g_out[i] - g[i]) * p[i]; // pᵀAp
        }
        if (!(pap > static_cast<T>(0)))
        {
            r.fx_new = fx;
            r.ok = false; // non-convex along p (won't happen for an SPD quadratic)
            return r;
        }
        const T alpha = -dphi0 / pap;
        for (crd::usize i = 0; i < n; ++i)
        {
            x_out[i] = x[i] + alpha * p[i];
        }
        r.fx_new = obj.value(x_out);
        (void)obj.gradient(x_out, g_out); // ∇f at the accepted point (cost contract)
        r.alpha = alpha;
        r.ok = true;
        r.grad_at_new_valid = true;
        r.evals = 2;
        r.grad_evals = 2;
        return r;
    }
};
} // namespace

TEST_CASE("v7-f nonlinear CG: finite termination on an SPD quadratic (exact line search)", "[hesap][opt][v7]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    const crd::u32 n = 8; // small ⇒ round-off cannot nudge the ≤ n termination over
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
        xtrue[i] = 1.0 + 0.3 * static_cast<crd::f64>(i);
        x0[i] = 0.0;
    }
    (void)op.apply({xtrue.data(), n}, {b.data(), n});

    opt::QuadraticObjective<crd::f64> obj(op, {b.data(), n}, &alloc);
    const ExactQuadraticLineSearch<crd::f64> exact_ls;
    opt::OptOptions<crd::f64> opts;
    opts.grad_tol = 1e-8;
    opts.max_iters = 200;

    for (auto variant : {opt::CgVariant::FletcherReeves, opt::CgVariant::PolakRibierePlus,
                         opt::CgVariant::HestenesStiefel, opt::CgVariant::DaiYuan})
    {
        auto r = opt::minimize_nonlinear_cg<crd::f64>(obj, {x0.data(), n}, opts, &alloc, &exact_ls, variant);
        REQUIRE(r.status == opt::OptStatus::Success);
        CHECK(r.iterations <= n); // linear-CG finite termination — the sharp per-variant correctness gold-check
        crd::f64 err = 0.0;
        for (crd::u32 i = 0; i < n; ++i)
        {
            err = std::max(err, std::fabs(r.x[i] - xtrue[i]));
        }
        CHECK(err < 1e-6);
    }
}

TEST_CASE("v7-f nonlinear CG: each variant converges on Rosenbrock-N", "[hesap][opt][v7]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    const crd::usize dim = 8;
    auto obj = opt::make_objective_from_functor<crd::f64>(RosenbrockN{}, dim, &alloc);

    crd::containers::Array<crd::f64> x0(&alloc);
    x0.resize(dim);
    for (crd::usize i = 0; i < dim; ++i)
    {
        x0[i] = (i % 2 == 0) ? -1.2 : 1.0; // the classic hard start
    }

    opt::OptOptions<crd::f64> opts;
    opts.grad_tol = 1e-6;
    opts.max_iters = 2000;

    for (auto variant : {opt::CgVariant::FletcherReeves, opt::CgVariant::PolakRibierePlus,
                         opt::CgVariant::HestenesStiefel, opt::CgVariant::DaiYuan})
    {
        auto r = opt::minimize_nonlinear_cg<crd::f64>(obj, {x0.data(), dim}, opts, &alloc, nullptr, variant);
        REQUIRE(r.status == opt::OptStatus::Success);
        crd::f64 err = 0.0;
        for (crd::usize i = 0; i < dim; ++i)
        {
            err = std::max(err, std::fabs(r.x[i] - 1.0));
        }
        CHECK(err < 1e-4);
        CHECK(r.fx < 1e-8);
        CHECK(r.fn_evals > 0); // eval-count instrumentation populated (the v7-z scipy-CG eval-parity metric)
        CHECK(r.grad_evals > 0);
        CHECK(r.iterations < 1000); // sanity: a β/restart bug would blow this past the L-BFGS-class count
    }
}

TEST_CASE("v7-f nonlinear CG determinism moat {1,2,4,8,16}", "[hesap][opt][v7][moat]")
{
    const crd::u32 n = 64; // κ ~ 1700 ⇒ CG runs many iterations (moat non-vacuous)
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
            sp::ParallelSparseLinearOp<crd::f64> op(a, &alloc, /*parallel_min_stored_bytes=*/0);
            opt::QuadraticObjective<crd::f64> obj(op, {b.data(), n}, &alloc);
            auto r = opt::minimize_nonlinear_cg<crd::f64>(obj, {x0.data(), n}, opts, &alloc, nullptr,
                                                          opt::CgVariant::PolakRibierePlus);

            REQUIRE(r.status == opt::OptStatus::Success);
            REQUIRE(r.iterations > 4); // genuinely iterative ⇒ moat not vacuous
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
                CHECK(ident); // CG trajectory bit-identical across worker counts
            }
        }
        crd::jobs::shutdown();
    }
}
