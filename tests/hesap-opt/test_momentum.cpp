// crd-hesap-opt v7-f — fixed-step momentum (Polyak heavy-ball + Nesterov/FISTA). Validates: (1) the SHARP
// acceleration check — on the κ~1700 1-D Laplacian quadratic, optimally-tuned heavy-ball (rate (√κ−1)/(√κ+1))
// and parameter-free NAG-with-restart both reach the gradient tolerance in ≥ 4× fewer iterations than plain
// fixed-step gradient descent (rate 1−1/κ; same α=1/L) — acceleration is the THEOREM these methods exist for,
// so a wrong μ schedule / restart / velocity update fails this where a "does it converge" check passes;
// (2) Nesterov on a smooth convex NON-quadratic (log-cosh, L=1) incl. the fixed-μ and restart-off branches;
// (3) the determinism moat {1,2,4,8,16} — NON-VACUOUS (κ~1700 ⇒ many iterations) and bit-identical across
// worker counts; (4) boundary n=0 / n=1 (SANITY rule #3).

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

// 1-D discrete Laplacian tridiag(−1, 2, −1): SPD with KNOWN spectrum λ_i = 2−2cos(iπ/(n+1)) ⇒ exact L, κ, and
// the optimal heavy-ball tuning are computable in-test. κ ~ O(n²).
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

// f(x) = Σ ln cosh(x_i − c_i): smooth convex non-quadratic, ∇f_i = tanh(x_i − c_i), L = max sech² = 1 ⇒ α = 1.
// Minimizer x = c with f* = 0 exactly.
class LogCosh final : public opt::Objective<crd::f64>
{
public:
    explicit LogCosh(crd::containers::ConstSpan<crd::f64> c) noexcept
        : Objective<crd::f64>(/*has_gradient=*/true, /*has_hessian_vector=*/false), m_c(c)
    {
    }

    [[nodiscard]] crd::f64 value(crd::containers::ConstSpan<crd::f64> x) const override
    {
        crd::f64 acc = 0.0;
        for (crd::usize i = 0; i < x.size(); ++i)
        {
            acc += std::log(std::cosh(x[i] - m_c[i]));
        }
        return acc;
    }

    [[nodiscard]] crd::usize n() const noexcept override { return m_c.size(); }

    [[nodiscard]] bool gradient(crd::containers::ConstSpan<crd::f64> x,
                                crd::containers::Span<crd::f64> g) const override
    {
        for (crd::usize i = 0; i < x.size(); ++i)
        {
            g[i] = std::tanh(x[i] - m_c[i]);
        }
        return true;
    }

private:
    crd::containers::ConstSpan<crd::f64> m_c;
};

// Shifted sphere Σ (x_i − 3)²: the trivial boundary-test objective (scalar-generic ⇒ forward-AD gradient).
struct ShiftedSphere
{
    template <typename S> S operator()(crd::containers::ConstSpan<S> x) const
    {
        S acc = S(0);
        for (crd::usize i = 0; i < x.size(); ++i)
        {
            const S d = x[i] - S(3);
            acc = acc + d * d;
        }
        return acc;
    }
};
} // namespace

TEST_CASE("v7-f momentum: heavy-ball + NAG accelerate over plain GD on an ill-conditioned quadratic",
          "[hesap][opt][v7]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    const crd::u32 n = 64;
    Csr a = laplacian_1d(&alloc, n);
    sp::SparseLinearOp<crd::f64> op(a);

    // Exact spectrum of tridiag(−1,2,−1): λ_i = 2 − 2cos(iπ/(n+1)).
    const crd::f64 pi = 3.14159265358979323846;
    const crd::f64 lam_min = 2.0 - 2.0 * std::cos(pi / (n + 1.0));
    const crd::f64 lam_max = 2.0 - 2.0 * std::cos(static_cast<crd::f64>(n) * pi / (n + 1.0));
    const crd::f64 kappa = lam_max / lam_min; // ≈ 1712
    const crd::f64 sqk = std::sqrt(kappa);

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
    (void)op.apply({xtrue.data(), n}, {b.data(), n});
    opt::QuadraticObjective<crd::f64> obj(op, {b.data(), n}, &alloc);

    opt::OptOptions<crd::f64> opts;
    opts.grad_tol = 1e-8;
    opts.max_iters = 200000;

    // Plain fixed-step GD = heavy-ball with μ = 0 (also covers the μ=0 edge). Rate 1−1/κ ⇒ ~O(κ·ln) iters.
    opt::MomentumOptions<crd::f64> gd_opts;
    gd_opts.step = 1.0 / lam_max;
    gd_opts.momentum = 0.0;
    const auto gd =
        opt::minimize_momentum<crd::f64>(obj, {x0.data(), n}, opts, &alloc, gd_opts, opt::MomentumVariant::HeavyBall);

    // Heavy-ball at the optimal quadratic tuning: α = 4/(√λmax+√λmin)², μ = ((√κ−1)/(√κ+1))² ⇒ rate (√κ−1)/(√κ+1).
    opt::MomentumOptions<crd::f64> hb_opts;
    const crd::f64 sums = std::sqrt(lam_max) + std::sqrt(lam_min);
    hb_opts.step = 4.0 / (sums * sums);
    hb_opts.momentum = ((sqk - 1.0) / (sqk + 1.0)) * ((sqk - 1.0) / (sqk + 1.0));
    const auto hb =
        opt::minimize_momentum<crd::f64>(obj, {x0.data(), n}, opts, &alloc, hb_opts, opt::MomentumVariant::HeavyBall);

    // NAG: parameter-free FISTA t-sequence + O'Donoghue-Candès gradient restart, same α = 1/L as plain GD.
    opt::MomentumOptions<crd::f64> nag_opts;
    nag_opts.step = 1.0 / lam_max;
    const auto nag =
        opt::minimize_momentum<crd::f64>(obj, {x0.data(), n}, opts, &alloc, nag_opts, opt::MomentumVariant::Nesterov);

    for (const auto* r : {&gd, &hb, &nag})
    {
        REQUIRE(r->status == opt::OptStatus::Success);
        crd::f64 err = 0.0;
        for (crd::u32 i = 0; i < n; ++i)
        {
            err = std::max(err, std::fabs(r->x[i] - xtrue[i]));
        }
        CHECK(err < 1e-5);
        CHECK(r->fn_evals > 0);
        CHECK(r->grad_evals > 0);
    }

    // THE acceleration gate (√κ ≈ 41 ⇒ the true ratio is ~10-40×; 4× is the safe sharp floor).
    CHECK(hb.iterations * 4 < gd.iterations);
    CHECK(nag.iterations * 4 < gd.iterations);
}

TEST_CASE("v7-f momentum: Nesterov converges on a smooth convex non-quadratic (log-cosh)", "[hesap][opt][v7]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    const crd::usize dim = 16;
    crd::containers::Array<crd::f64> c(&alloc);
    crd::containers::Array<crd::f64> x0(&alloc);
    c.resize(dim);
    x0.resize(dim);
    for (crd::usize i = 0; i < dim; ++i)
    {
        c[i] = 0.3 + 0.1 * static_cast<crd::f64>(i);
        x0[i] = 0.0;
    }
    const LogCosh obj({c.data(), dim});

    opt::OptOptions<crd::f64> opts;
    opts.grad_tol = 1e-10;
    opts.max_iters = 5000;

    opt::MomentumOptions<crd::f64> mo;
    mo.step = 1.0; // L = 1 for log-cosh

    // Default path: FISTA sequence + adaptive restart.
    auto r = opt::minimize_momentum<crd::f64>(obj, {x0.data(), dim}, opts, &alloc, mo);
    REQUIRE(r.status == opt::OptStatus::Success);
    crd::f64 err = 0.0;
    for (crd::usize i = 0; i < dim; ++i)
    {
        err = std::max(err, std::fabs(r.x[i] - c[i]));
    }
    CHECK(err < 1e-8);
    CHECK(r.fx < 1e-12); // f* = 0 exactly

    // Branch coverage: fixed μ, and the restart-off path (plain FISTA).
    mo.momentum = 0.9;
    auto r_fixed = opt::minimize_momentum<crd::f64>(obj, {x0.data(), dim}, opts, &alloc, mo);
    CHECK(r_fixed.status == opt::OptStatus::Success);

    mo.momentum = -1.0;
    mo.adaptive_restart = false;
    auto r_norestart = opt::minimize_momentum<crd::f64>(obj, {x0.data(), dim}, opts, &alloc, mo);
    CHECK(r_norestart.status == opt::OptStatus::Success);
}

TEST_CASE("v7-f momentum determinism moat {1,2,4,8,16}", "[hesap][opt][v7][moat]")
{
    const crd::u32 n = 64; // κ ~ 1700 ⇒ NAG runs many iterations (moat non-vacuous)
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
    opts.max_iters = 200000;
    opt::MomentumOptions<crd::f64> mo;
    mo.step = 0.25; // λmax < 4 for the 1-D Laplacian ⇒ 1/4 ≤ 1/L is a valid fixed step

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
            auto r =
                opt::minimize_momentum<crd::f64>(obj, {x0.data(), n}, opts, &alloc, mo, opt::MomentumVariant::Nesterov);

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
                CHECK(ident); // momentum trajectory bit-identical across worker counts
            }
        }
        crd::jobs::shutdown();
    }
}

TEST_CASE("v7-f momentum boundary: n = 0 and n = 1", "[hesap][opt][v7]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    opt::OptOptions<crd::f64> opts;
    opts.grad_tol = 1e-9;
    opts.max_iters = 5000;
    opt::MomentumOptions<crd::f64> mo;
    mo.step = 0.4; // f'' = 2 ⇒ stable for both variants

    // n = 0: immediate Success, no evals.
    auto obj0 = opt::make_objective_from_functor<crd::f64>(ShiftedSphere{}, 0, &alloc);
    auto r0 = opt::minimize_momentum<crd::f64>(obj0, {static_cast<const crd::f64*>(nullptr), 0}, opts, &alloc, mo);
    REQUIRE(r0.status == opt::OptStatus::Success);
    CHECK(r0.converged);
    CHECK(r0.x.size() == 0);
    CHECK(r0.fn_evals == 0);

    // n = 1: both variants find the minimizer x = 3 of (x−3)².
    auto obj1 = opt::make_objective_from_functor<crd::f64>(ShiftedSphere{}, 1, &alloc);
    const crd::f64 start = -5.0;
    for (auto variant : {opt::MomentumVariant::HeavyBall, opt::MomentumVariant::Nesterov})
    {
        auto r1 = opt::minimize_momentum<crd::f64>(obj1, {&start, 1}, opts, &alloc, mo, variant);
        REQUIRE(r1.status == opt::OptStatus::Success);
        CHECK(std::fabs(r1.x[0] - 3.0) < 1e-6);
    }
}
