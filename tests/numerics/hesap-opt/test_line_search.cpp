// crd-hesap-opt v7-c — line searches. Validates: (1) WolfeLineSearch (strong + weak) returns a step at which the
// Armijo AND curvature conditions actually hold (recomputed from a fresh eval), with grad_at_new_valid=true;
// (2) MoreThuenteLineSearch finds a strong-Wolfe step on a Moré-Thuente benchmark function (the cubic-interpolation
// path) using no more evals than the bisection-zoom — correctness only (no gold-standard claim for a bare line
// search; that's measured at v7-d vs liblbfgs); (3) gradient descent with a Wolfe line search converges; (4) the
// line-search determinism moat {1,2,4,8,16} — NON-VACUOUS: ok + the Wolfe conditions hold + zoom was genuinely
// entered (evals>1) + bit-identical α/x across worker counts (the composition v7-d L-BFGS rests on).

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

// Moré-Thuente 1994 test function 1: φ(t) = −t/(t²+β); φ'(t) = −(β−t²)/(t²+β)². Minimizer at t=√β. The classic
// line-search interpolation benchmark.
class MtFunc1 final : public opt::Objective<crd::f64>
{
public:
    explicit MtFunc1(crd::f64 beta) : opt::Objective<crd::f64>(/*has_gradient=*/true, /*has_hessian_vector=*/false),
                                      m_beta(beta) {}
    [[nodiscard]] crd::f64 value(crd::containers::ConstSpan<crd::f64> x) const override
    {
        const crd::f64 t = x[0];
        return -t / (t * t + m_beta);
    }
    [[nodiscard]] crd::usize n() const noexcept override { return 1; }
    [[nodiscard]] bool gradient(crd::containers::ConstSpan<crd::f64> x, crd::containers::Span<crd::f64> g) const override
    {
        const crd::f64 t = x[0];
        const crd::f64 d = t * t + m_beta;
        g[0] = -(m_beta - t * t) / (d * d);
        return true;
    }

private:
    crd::f64 m_beta;
};

// Moré-Thuente 1994 test function 2: φ(t) = (t+β)⁵ − 2(t+β)⁴, β=0.004; φ'(t) = 5(t+β)⁴ − 8(t+β)³. Minimizer at
// t = 1.6 − β = 1.596. The long near-flat region near t=0 (φ'(0) ≈ −5e-7) forces the stage-1 / modified-function
// machinery that the convex fn 1 likely skips — the branch the advisor flagged as untested.
class MtFunc2 final : public opt::Objective<crd::f64>
{
public:
    explicit MtFunc2(crd::f64 beta) : opt::Objective<crd::f64>(/*has_gradient=*/true, /*has_hessian_vector=*/false),
                                      m_beta(beta) {}
    [[nodiscard]] crd::f64 value(crd::containers::ConstSpan<crd::f64> x) const override
    {
        const crd::f64 u = x[0] + m_beta;
        return u * u * u * u * (u - 2.0);
    }
    [[nodiscard]] crd::usize n() const noexcept override { return 1; }
    [[nodiscard]] bool gradient(crd::containers::ConstSpan<crd::f64> x, crd::containers::Span<crd::f64> g) const override
    {
        const crd::f64 u = x[0] + m_beta;
        g[0] = 5.0 * u * u * u * u - 8.0 * u * u * u;
        return true;
    }

private:
    crd::f64 m_beta;
};

// A shifted 1-D quadratic φ(t) = (t−c)² — convex, so overshooting the minimizer gives a HIGHER value, forcing
// dcstep case 1 (higher function value) + bracketing.
class Quad1D final : public opt::Objective<crd::f64>
{
public:
    explicit Quad1D(crd::f64 c) : opt::Objective<crd::f64>(/*has_gradient=*/true, /*has_hessian_vector=*/false),
                                 m_c(c) {}
    [[nodiscard]] crd::f64 value(crd::containers::ConstSpan<crd::f64> x) const override
    {
        const crd::f64 d = x[0] - m_c;
        return d * d;
    }
    [[nodiscard]] crd::usize n() const noexcept override { return 1; }
    [[nodiscard]] bool gradient(crd::containers::ConstSpan<crd::f64> x, crd::containers::Span<crd::f64> g) const override
    {
        g[0] = 2.0 * (x[0] - m_c);
        return true;
    }

private:
    crd::f64 m_c;
};

// Recompute the Wolfe conditions at x + alpha·p from a fresh objective eval.
struct WolfeAt
{
    crd::f64 phi_a;
    crd::f64 dphi_a;
};
WolfeAt eval_at(const opt::Objective<crd::f64>& obj, crd::containers::ConstSpan<crd::f64> x,
                crd::containers::ConstSpan<crd::f64> p, crd::f64 alpha, crd::memory::IAllocator* a)
{
    namespace dn = crd::hesap::dense;
    const crd::usize n = x.size();
    crd::containers::Array<crd::f64> xa(a);
    crd::containers::Array<crd::f64> ga(a);
    xa.resize(n);
    ga.resize(n);
    for (crd::usize i = 0; i < n; ++i)
    {
        xa[i] = x[i] + alpha * p[i];
    }
    const crd::f64 phi = obj.value({xa.data(), n});
    (void)obj.gradient({xa.data(), n}, {ga.data(), n});
    return {phi, dn::dot<crd::f64>({ga.data(), n}, {p.data(), n})};
}
} // namespace

TEST_CASE("v7-c WolfeLineSearch returns a step where the Wolfe conditions hold", "[hesap][opt][v7]")
{
    namespace dn = crd::hesap::dense;
    crd::memory::TlsfAllocator alloc(1U << 22);
    const crd::u32 n = 12;
    Csr a = well_conditioned_spd(&alloc, n);
    sp::SparseLinearOp<crd::f64> op(a);

    crd::containers::Array<crd::f64> b(&alloc);
    crd::containers::Array<crd::f64> x(&alloc);
    crd::containers::Array<crd::f64> g(&alloc);
    crd::containers::Array<crd::f64> p(&alloc);
    crd::containers::Array<crd::f64> x_out(&alloc);
    crd::containers::Array<crd::f64> g_out(&alloc);
    b.resize(n);
    x.resize(n);
    g.resize(n);
    p.resize(n);
    x_out.resize(n);
    g_out.resize(n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        b[i] = 0.4 * static_cast<crd::f64>(i) - 1.0;
        x[i] = 0.0;
    }
    opt::QuadraticObjective<crd::f64> obj(op, {b.data(), n}, &alloc);
    const crd::f64 fx = obj.value({x.data(), n});
    REQUIRE(obj.gradient({x.data(), n}, {g.data(), n}));
    for (crd::u32 i = 0; i < n; ++i)
    {
        p[i] = -g[i]; // steepest descent
    }
    const crd::f64 dphi0 = dn::dot<crd::f64>({g.data(), n}, {p.data(), n});
    REQUIRE(dphi0 < 0.0);

    const crd::f64 c1 = 1e-4;
    const crd::f64 c2 = 0.9;

    SECTION("strong Wolfe")
    {
        opt::WolfeLineSearch<crd::f64> ls(c1, c2, /*strong=*/true, 50);
        auto r = ls.search(obj, {x.data(), n}, fx, {g.data(), n}, {p.data(), n}, 1.0, {x_out.data(), n},
                           {g_out.data(), n});
        REQUIRE(r.ok);
        REQUIRE(r.grad_at_new_valid);
        const auto w = eval_at(obj, {x.data(), n}, {p.data(), n}, r.alpha, &alloc);
        CHECK(w.phi_a <= fx + c1 * r.alpha * dphi0);     // Armijo
        CHECK(std::fabs(w.dphi_a) <= c2 * std::fabs(dphi0)); // strong curvature
        // g_out returned by the search matches a fresh gradient at the accepted point.
        CHECK(std::fabs(dn::dot<crd::f64>({g_out.data(), n}, {p.data(), n}) - w.dphi_a) < 1e-12);
    }
    SECTION("weak Wolfe")
    {
        opt::WolfeLineSearch<crd::f64> ls(c1, c2, /*strong=*/false, 50);
        auto r = ls.search(obj, {x.data(), n}, fx, {g.data(), n}, {p.data(), n}, 1.0, {x_out.data(), n},
                           {g_out.data(), n});
        REQUIRE(r.ok);
        const auto w = eval_at(obj, {x.data(), n}, {p.data(), n}, r.alpha, &alloc);
        CHECK(w.phi_a <= fx + c1 * r.alpha * dphi0); // Armijo
        CHECK(w.dphi_a >= c2 * dphi0);               // weak curvature (one-sided)
    }
}

TEST_CASE("v7-c MoreThuenteLineSearch finds a strong-Wolfe step on a benchmark function", "[hesap][opt][v7]")
{
    crd::memory::TlsfAllocator alloc(1U << 20);
    MtFunc1 obj(2.0); // minimizer at t=√2

    crd::containers::Array<crd::f64> x(&alloc);
    crd::containers::Array<crd::f64> g(&alloc);
    crd::containers::Array<crd::f64> p(&alloc);
    crd::containers::Array<crd::f64> x_out(&alloc);
    crd::containers::Array<crd::f64> g_out(&alloc);
    x.resize(1);
    g.resize(1);
    p.resize(1);
    x_out.resize(1);
    g_out.resize(1);
    x[0] = 0.0;
    p[0] = 1.0; // search along +t
    const crd::f64 fx = obj.value({x.data(), 1});
    REQUIRE(obj.gradient({x.data(), 1}, {g.data(), 1}));
    const crd::f64 dphi0 = g[0] * p[0];
    REQUIRE(dphi0 < 0.0);

    const crd::f64 c1 = 1e-4;
    const crd::f64 c2 = 0.1;
    opt::MoreThuenteLineSearch<crd::f64> mt(c1, c2, 64);
    auto r = mt.search(obj, {x.data(), 1}, fx, {g.data(), 1}, {p.data(), 1}, 1.0, {x_out.data(), 1},
                       {g_out.data(), 1});
    REQUIRE(r.ok);
    REQUIRE(r.grad_at_new_valid);
    const auto w = eval_at(obj, {x.data(), 1}, {p.data(), 1}, r.alpha, &alloc);
    CHECK(w.phi_a <= fx + c1 * r.alpha * dphi0);          // Armijo
    CHECK(std::fabs(w.dphi_a) <= c2 * std::fabs(dphi0));  // strong curvature
    CHECK(r.evals > 1);                                    // interpolation genuinely exercised

    // Correctness vs bisection: More-Thuente should use no more evals than the bisection-zoom on this smooth 1-D φ.
    opt::WolfeLineSearch<crd::f64> wls(c1, c2, /*strong=*/true, 64);
    auto rw = wls.search(obj, {x.data(), 1}, fx, {g.data(), 1}, {p.data(), 1}, 1.0, {x_out.data(), 1},
                         {g_out.data(), 1});
    REQUIRE(rw.ok);
    CHECK(r.evals <= rw.evals);
}

TEST_CASE("v7-c MoreThuente fires the stage-1 / modified-function path (near-flat fn 2)", "[hesap][opt][v7]")
{
    crd::memory::TlsfAllocator alloc(1U << 20);
    MtFunc2 obj(0.004); // minimizer at t = 1.596; near-flat near t=0

    crd::containers::Array<crd::f64> x(&alloc);
    crd::containers::Array<crd::f64> g(&alloc);
    crd::containers::Array<crd::f64> p(&alloc);
    crd::containers::Array<crd::f64> x_out(&alloc);
    crd::containers::Array<crd::f64> g_out(&alloc);
    x.resize(1);
    g.resize(1);
    p.resize(1);
    x_out.resize(1);
    g_out.resize(1);
    x[0] = 0.0;
    p[0] = 1.0;
    const crd::f64 fx = obj.value({x.data(), 1});
    REQUIRE(obj.gradient({x.data(), 1}, {g.data(), 1}));
    const crd::f64 dphi0 = g[0] * p[0];
    REQUIRE(dphi0 < 0.0);

    const crd::f64 c1 = 1e-4;
    const crd::f64 c2 = 0.1;
    opt::MoreThuenteLineSearch<crd::f64> mt(c1, c2, 64);
    auto r = mt.search(obj, {x.data(), 1}, fx, {g.data(), 1}, {p.data(), 1}, 1.0, {x_out.data(), 1},
                       {g_out.data(), 1});
    REQUIRE(r.ok);
    const auto w = eval_at(obj, {x.data(), 1}, {p.data(), 1}, r.alpha, &alloc);
    CHECK(w.phi_a <= fx + c1 * r.alpha * dphi0);         // Armijo
    CHECK(std::fabs(w.dphi_a) <= c2 * std::fabs(dphi0)); // strong curvature
    CHECK(std::fabs(r.alpha - 1.596) < 0.3);             // near the known minimizer
    CHECK(r.evals < 32);                                 // no eval blow-up
}

TEST_CASE("v7-c MoreThuente brackets on overshoot (convex fn, alpha0 past the minimizer)", "[hesap][opt][v7]")
{
    crd::memory::TlsfAllocator alloc(1U << 20);
    Quad1D obj(3.0); // φ(t) = (t−3)², minimizer at t = 3

    crd::containers::Array<crd::f64> x(&alloc);
    crd::containers::Array<crd::f64> g(&alloc);
    crd::containers::Array<crd::f64> p(&alloc);
    crd::containers::Array<crd::f64> x_out(&alloc);
    crd::containers::Array<crd::f64> g_out(&alloc);
    x.resize(1);
    g.resize(1);
    p.resize(1);
    x_out.resize(1);
    g_out.resize(1);
    x[0] = 0.0;
    p[0] = 1.0;
    const crd::f64 fx = obj.value({x.data(), 1});
    REQUIRE(obj.gradient({x.data(), 1}, {g.data(), 1}));
    const crd::f64 dphi0 = g[0] * p[0]; // = −6
    REQUIRE(dphi0 < 0.0);

    const crd::f64 c1 = 1e-4;
    const crd::f64 c2 = 0.1;
    opt::MoreThuenteLineSearch<crd::f64> mt(c1, c2, 64);
    // alpha0 = 8 overshoots the minimizer (3) ⇒ φ(8)=25 > φ(0)=9 ⇒ dcstep case 1 (higher value) + immediate bracket;
    // curvature |φ'(8)|=10 ≫ 0.6 forces interpolation (multiple evals).
    auto r = mt.search(obj, {x.data(), 1}, fx, {g.data(), 1}, {p.data(), 1}, 8.0, {x_out.data(), 1},
                       {g_out.data(), 1});
    REQUIRE(r.ok);
    CHECK(r.evals > 1);
    const auto w = eval_at(obj, {x.data(), 1}, {p.data(), 1}, r.alpha, &alloc);
    CHECK(w.phi_a <= fx + c1 * r.alpha * dphi0);
    CHECK(std::fabs(w.dphi_a) <= c2 * std::fabs(dphi0));
    CHECK(std::fabs(r.alpha - 3.0) < 0.3); // strong Wolfe with c2=0.1 ⇒ |α−3| ≤ 0.3
}

TEST_CASE("v7-c MoreThuente determinism moat {1,2,4,8,16} (parallel objective)", "[hesap][opt][v7][moat]")
{
    namespace dn = crd::hesap::dense;
    const crd::u32 n = 32;
    crd::memory::TlsfAllocator alloc(1U << 24);
    Csr a = well_conditioned_spd(&alloc, n);
    sp::SparseLinearOp<crd::f64> serial_op(a);

    crd::containers::Array<crd::f64> b(&alloc);
    crd::containers::Array<crd::f64> x(&alloc);
    crd::containers::Array<crd::f64> g(&alloc);
    crd::containers::Array<crd::f64> p(&alloc);
    b.resize(n);
    x.resize(n);
    g.resize(n);
    p.resize(n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        b[i] = 0.25 * static_cast<crd::f64>(i) - 0.4;
        x[i] = 0.0;
    }
    crd::f64 fx0 = 0.0;
    {
        opt::QuadraticObjective<crd::f64> o(serial_op, {b.data(), n}, &alloc);
        fx0 = o.value({x.data(), n});
        (void)o.gradient({x.data(), n}, {g.data(), n});
    }
    for (crd::u32 i = 0; i < n; ++i)
    {
        p[i] = -g[i];
    }
    const crd::f64 dphi0 = dn::dot<crd::f64>({g.data(), n}, {p.data(), n});
    const crd::f64 c1 = 1e-4;
    const crd::f64 c2 = 0.9;

    crd::f64 alpha_ref = 0.0;
    crd::containers::Array<crd::f64> x_ref(&alloc);
    bool have_ref = false;
    for (crd::u32 nw : {1U, 2U, 4U, 8U, 16U})
    {
        crd::jobs::Config cfg;
        cfg.num_threads = nw;
        crd::jobs::init(cfg);
        {
            sp::ParallelSparseLinearOp<crd::f64> op(a, &alloc, /*parallel_min_stored_bytes=*/0);
            opt::QuadraticObjective<crd::f64> obj(op, {b.data(), n}, &alloc);
            opt::MoreThuenteLineSearch<crd::f64> mt(c1, c2, 64);

            crd::containers::Array<crd::f64> x_out(&alloc);
            crd::containers::Array<crd::f64> g_out(&alloc);
            x_out.resize(n);
            g_out.resize(n);
            auto r = mt.search(obj, {x.data(), n}, fx0, {g.data(), n}, {p.data(), n}, 6.0, {x_out.data(), n},
                               {g_out.data(), n});
            REQUIRE(r.ok);
            CHECK(r.evals > 1); // interpolation/bracketing genuinely exercised
            const auto w = eval_at(obj, {x.data(), n}, {p.data(), n}, r.alpha, &alloc);
            CHECK(w.phi_a <= fx0 + c1 * r.alpha * dphi0);
            CHECK(std::fabs(w.dphi_a) <= c2 * std::fabs(dphi0));
            if (!have_ref)
            {
                alpha_ref = r.alpha;
                x_ref.resize(n);
                for (crd::u32 i = 0; i < n; ++i)
                {
                    x_ref[i] = x_out[i];
                }
                have_ref = true;
            }
            else
            {
                bool ident = (r.alpha == alpha_ref);
                for (crd::u32 i = 0; i < n && ident; ++i)
                {
                    ident = (x_out[i] == x_ref[i]);
                }
                CHECK(ident); // α and the accepted point bit-identical across worker counts (branchy dcstep code)
            }
        }
        crd::jobs::shutdown();
    }
}

TEST_CASE("v7-c gradient descent with a Wolfe line search converges", "[hesap][opt][v7]")
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
    // 1e-6, not 1e-8: steepest descent + a curvature-enforcing line search drives |g| toward the point where the
    // Armijo test (φ(α)−φ(0) ≈ −½|g|²) drops below the ε·|φ₀| rounding floor and no step satisfies it. Tight
    // convergence is L-BFGS's job (v7-d); this is an integration smoke that the optimizer drives a Wolfe search.
    opts.grad_tol = 1e-6;
    opts.max_iters = 2000;
    opt::WolfeLineSearch<crd::f64> wolfe;
    auto r = opt::minimize_gradient_descent<crd::f64>(obj, {x0.data(), n}, opts, &alloc, &wolfe);

    REQUIRE(r.status == opt::OptStatus::Success);
    crd::f64 err = 0.0;
    for (crd::u32 i = 0; i < n; ++i)
    {
        err = std::max(err, std::fabs(r.x[i] - xtrue[i]));
    }
    CHECK(err < 1e-4);
}

TEST_CASE("v7-c line-search determinism moat {1,2,4,8,16} (parallel objective, zoom entered)",
          "[hesap][opt][v7][moat]")
{
    namespace dn = crd::hesap::dense;
    const crd::u32 n = 32;
    crd::memory::TlsfAllocator alloc(1U << 24);
    Csr a = well_conditioned_spd(&alloc, n);
    sp::SparseLinearOp<crd::f64> serial_op(a);

    crd::containers::Array<crd::f64> b(&alloc);
    crd::containers::Array<crd::f64> x(&alloc);
    crd::containers::Array<crd::f64> g(&alloc);
    crd::containers::Array<crd::f64> p(&alloc);
    b.resize(n);
    x.resize(n);
    g.resize(n);
    p.resize(n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        b[i] = 0.3 * static_cast<crd::f64>(i) - 0.5;
        x[i] = 0.0;
    }
    {
        opt::QuadraticObjective<crd::f64> obj0(serial_op, {b.data(), n}, &alloc);
        (void)obj0.gradient({x.data(), n}, {g.data(), n});
    }
    for (crd::u32 i = 0; i < n; ++i)
    {
        p[i] = -g[i];
    }
    const crd::f64 fx0 = [&] {
        opt::QuadraticObjective<crd::f64> o(serial_op, {b.data(), n}, &alloc);
        return o.value({x.data(), n});
    }();
    const crd::f64 dphi0 = dn::dot<crd::f64>({g.data(), n}, {p.data(), n});
    const crd::f64 c1 = 1e-4;
    const crd::f64 c2 = 0.9;

    crd::f64 alpha_ref = 0.0;
    crd::containers::Array<crd::f64> x_ref(&alloc);
    bool have_ref = false;
    for (crd::u32 nw : {1U, 2U, 4U, 8U, 16U})
    {
        crd::jobs::Config cfg;
        cfg.num_threads = nw;
        crd::jobs::init(cfg);
        {
            sp::ParallelSparseLinearOp<crd::f64> op(a, &alloc, /*parallel_min_stored_bytes=*/0);
            opt::QuadraticObjective<crd::f64> obj(op, {b.data(), n}, &alloc);
            opt::WolfeLineSearch<crd::f64> ls(c1, c2, /*strong=*/true, 60);

            crd::containers::Array<crd::f64> x_out(&alloc);
            crd::containers::Array<crd::f64> g_out(&alloc);
            x_out.resize(n);
            g_out.resize(n);
            // alpha0 = 8 overshoots the (≈1) minimizer ⇒ bracketing fails Armijo ⇒ zoom is genuinely entered.
            auto r = ls.search(obj, {x.data(), n}, fx0, {g.data(), n}, {p.data(), n}, 8.0, {x_out.data(), n},
                               {g_out.data(), n});
            REQUIRE(r.ok);
            CHECK(r.evals > 1); // zoom (or multi-step bracketing) genuinely exercised — not a one-shot accept

            // The Wolfe conditions actually hold at the returned step.
            const auto w = eval_at(obj, {x.data(), n}, {p.data(), n}, r.alpha, &alloc);
            CHECK(w.phi_a <= fx0 + c1 * r.alpha * dphi0);
            CHECK(std::fabs(w.dphi_a) <= c2 * std::fabs(dphi0));

            if (!have_ref)
            {
                alpha_ref = r.alpha;
                x_ref.resize(n);
                for (crd::u32 i = 0; i < n; ++i)
                {
                    x_ref[i] = x_out[i];
                }
                have_ref = true;
            }
            else
            {
                bool ident = (r.alpha == alpha_ref);
                for (crd::u32 i = 0; i < n && ident; ++i)
                {
                    ident = (x_out[i] == x_ref[i]);
                }
                CHECK(ident); // α and the accepted point bit-identical across worker counts
            }
        }
        crd::jobs::shutdown();
    }
}
