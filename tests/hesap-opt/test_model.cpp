// v7-o — the ALGEBRAIC MODELING LAYER. Gates: (1) WIRING EXACTNESS — the model's unconstrained and
// bounds-only dispatches must be BIT-IDENTICAL to calling the underlying solver directly on the same functor
// (same algorithm + same AD gradients => same trajectory; any divergence is a wiring bug); (2) declarative
// HS14 hits the published optimum through the auto-dispatched SQP AND the auglag override; (3) le-constraints
// and folded variable bounds produce the analytic constrained optimum; (4) an equality-constrained projection
// recovers the ANALYTIC MULTIPLIER through the model's AD-built Jacobians; (5) bit-identical determinism.

#include <crd/hesap/opt/forward_ad.hpp>
#include <crd/hesap/opt/lbfgs.hpp>
#include <crd/hesap/opt/lbfgsb.hpp>
#include <crd/hesap/opt/model.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <limits>

namespace opt = crd::hesap::opt;

namespace
{

// Rosenbrock-2 as a scalar-generic functor (the v7-b DiffFunctor contract).
struct Rosenbrock2
{
    template <typename S> [[nodiscard]] S operator()(crd::containers::ConstSpan<S> x) const
    {
        const S a = S(1.0) - x[0];
        const S b = x[1] - x[0] * x[0];
        return a * a + S(100.0) * b * b;
    }
};

} // namespace

TEST_CASE("model: unconstrained dispatch is bit-identical to direct L-BFGS", "[hesap-opt][model]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    opt::OptOptions<crd::f64> opts;
    opts.grad_tol = 1e-9;
    opts.max_iters = 200;

    opt::Model<crd::f64> model(&alloc);
    (void)model.add_variables(2, /*start=*/-1.2);
    model.set_start(1, 1.0); // the classic (-1.2, 1) start
    model.minimize(Rosenbrock2{});
    const opt::OptResult<crd::f64> rm = model.solve(opts);

    const crd::f64 x0[] = {-1.2, 1.0};
    const auto obj = opt::make_objective_from_functor<crd::f64>(Rosenbrock2{}, 2, &alloc);
    const opt::OptResult<crd::f64> rd = opt::minimize_lbfgs<crd::f64>(obj, {x0, 2}, opts, &alloc);

    REQUIRE(rm.status == opt::OptStatus::Success);
    REQUIRE(rm.status == rd.status);
    REQUIRE(rm.iterations == rd.iterations); // same algorithm, same AD gradients => same trajectory
    REQUIRE(rm.x[0] == rd.x[0]);             // bit-identical
    REQUIRE(rm.x[1] == rd.x[1]);
    CHECK(std::fabs(rm.x[0] - 1.0) < 1e-6);
    CHECK(std::fabs(rm.x[1] - 1.0) < 1e-6);
}

TEST_CASE("model: bounds-only dispatch is bit-identical to direct L-BFGS-B", "[hesap-opt][model]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    opt::OptOptions<crd::f64> opts;
    opts.grad_tol = 1e-9;
    opts.max_iters = 200;

    // The unconstrained minimizer (1,1) lies OUTSIDE the box (x0 <= 0.5) => the bound pins.
    opt::Model<crd::f64> model(&alloc);
    (void)model.add_variable(/*start=*/0.0, /*lower=*/-2.0, /*upper=*/0.5);
    (void)model.add_variable(/*start=*/0.0, /*lower=*/-2.0, /*upper=*/2.0);
    model.minimize(Rosenbrock2{});
    const opt::OptResult<crd::f64> rm = model.solve(opts); // Auto -> Lbfgsb

    const crd::f64 x0[] = {0.0, 0.0};
    const crd::f64 lo[] = {-2.0, -2.0};
    const crd::f64 up[] = {0.5, 2.0};
    const auto obj = opt::make_objective_from_functor<crd::f64>(Rosenbrock2{}, 2, &alloc);
    const opt::OptResult<crd::f64> rd = opt::minimize_lbfgsb<crd::f64>(obj, {x0, 2}, {lo, 2}, {up, 2}, opts, &alloc);

    REQUIRE(rm.status == rd.status);
    REQUIRE(rm.iterations == rd.iterations);
    REQUIRE(rm.x[0] == rd.x[0]);
    REQUIRE(rm.x[1] == rd.x[1]);
    CHECK(std::fabs(rm.x[0] - 0.5) < 1e-8);  // pinned at the upper bound
    CHECK(std::fabs(rm.x[1] - 0.25) < 1e-6); // x1 = x0^2 on the active face
}

TEST_CASE("model: declarative HS14 through SQP and auglag", "[hesap-opt][model]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    // HS14: min (x0-2)^2 + (x1-1)^2  s.t.  x0 - 2 x1 + 1 = 0,  -x0^2/4 - x1^2 + 1 >= 0.
    // Published f* = 9 - 2.875*sqrt(7).
    const crd::f64 fstar = 9.0 - 2.875 * std::sqrt(7.0);
    opt::OptOptions<crd::f64> opts;
    opts.grad_tol = 1e-9;
    opts.max_iters = 200;

    opt::Model<crd::f64> model(&alloc);
    (void)model.add_variables(2, /*start=*/2.0);
    model.minimize(
        [](const auto& x)
        {
            const auto a = x[0] - 2.0;
            const auto b = x[1] - 1.0;
            return a * a + b * b;
        });
    model.subject_to_eq([](const auto& x) { return x[0] - 2.0 * x[1] + 1.0; });
    model.subject_to_ge([](const auto& x) { return -x[0] * x[0] * 0.25 - x[1] * x[1] + 1.0; });

    const opt::OptResult<crd::f64> rs = model.solve(opts); // Auto -> Sqp
    REQUIRE(rs.status == opt::OptStatus::Success);
    CHECK(std::fabs(rs.fx - fstar) < 1e-7);
    REQUIRE(rs.multipliers.size() == 2); // 1 eq + 1 ineq
    CHECK(rs.multipliers[1] > 0.0);      // the nonlinear inequality is ACTIVE at HS14's optimum

    const opt::OptResult<crd::f64> ra = model.solve(opts, opt::ModelMethod::Auglag);
    REQUIRE(ra.status == opt::OptStatus::Success);
    CHECK(std::fabs(ra.fx - fstar) < 1e-6);
}

TEST_CASE("model: le-constraint + folded variable bound (analytic optimum)", "[hesap-opt][model]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    // min (x0-2)^2 + (x1-2)^2  s.t.  x0 + x1 <= 2 (le),  x0 <= 0.5 (variable bound, folded into c_I).
    // On the line x0 + x1 = 2 the unconstrained-along-the-line optimum is x0 = 1 > 0.5 => the bound pins:
    // x* = (0.5, 1.5), f* = 2.25 + 0.25 = 2.5.
    opt::OptOptions<crd::f64> opts;
    opts.grad_tol = 1e-9;
    opts.max_iters = 200;

    opt::Model<crd::f64> model(&alloc);
    (void)model.add_variable(/*start=*/0.0, /*lower=*/-std::numeric_limits<crd::f64>::infinity(),
                             /*upper=*/0.5);
    (void)model.add_variable(/*start=*/0.0);
    model.minimize(
        [](const auto& x)
        {
            const auto a = x[0] - 2.0;
            const auto b = x[1] - 2.0;
            return a * a + b * b;
        });
    model.subject_to_le([](const auto& x) { return x[0] + x[1] - 2.0; });

    const opt::OptResult<crd::f64> r = model.solve(opts); // Auto -> Sqp (bound folded)
    CAPTURE(r.x[0], r.x[1], r.fx, r.iterations, r.kkt_residual);
    REQUIRE(r.status == opt::OptStatus::Success);
    CHECK(std::fabs(r.x[0] - 0.5) < 1e-7);
    CHECK(std::fabs(r.x[1] - 1.5) < 1e-7);
    CHECK(std::fabs(r.fx - 2.5) < 1e-7);
}

TEST_CASE("model: equality projection recovers the analytic multiplier", "[hesap-opt][model]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    // Project p = (2, 2) onto the unit circle: min 0.5*||x - p||^2 s.t. x'x - 1 = 0.
    // x* = p/||p|| = (1/sqrt2, 1/sqrt2). L = f - lambda*c => (x - p) - 2 lambda x = 0 =>
    // x (1 - 2 lambda) = p => |1 - 2 lambda| = ||p|| = 2 sqrt2 (near branch) => lambda* = (1 - 2 sqrt2)/2.
    opt::OptOptions<crd::f64> opts;
    opts.grad_tol = 1e-10;
    opts.max_iters = 200;

    opt::Model<crd::f64> model(&alloc);
    (void)model.add_variables(2, /*start=*/1.0);
    model.set_start(1, 0.5); // off-axis start (avoid the symmetric saddle line)
    model.minimize(
        [](const auto& x)
        {
            const auto a = x[0] - 2.0;
            const auto b = x[1] - 2.0;
            return 0.5 * (a * a + b * b);
        });
    model.subject_to_eq([](const auto& x) { return x[0] * x[0] + x[1] * x[1] - 1.0; });

    const opt::OptResult<crd::f64> r = model.solve(opts);
    REQUIRE(r.status == opt::OptStatus::Success);
    const crd::f64 inv_root2 = 1.0 / std::sqrt(2.0);
    CHECK(std::fabs(r.x[0] - inv_root2) < 1e-7);
    CHECK(std::fabs(r.x[1] - inv_root2) < 1e-7);
    REQUIRE(r.multipliers.size() == 1);
    const crd::f64 lambda_star = (1.0 - 2.0 * std::sqrt(2.0)) / 2.0;
    CHECK(std::fabs(r.multipliers[0] - lambda_star) < 1e-6); // through the model's AD-built Jacobian
}

TEST_CASE("model: bit-identical determinism (solve twice)", "[hesap-opt][model][determinism]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    opt::OptOptions<crd::f64> opts;
    opts.grad_tol = 1e-9;
    opts.max_iters = 200;

    opt::Model<crd::f64> model(&alloc);
    (void)model.add_variables(2, /*start=*/2.0);
    model.minimize(
        [](const auto& x)
        {
            const auto a = x[0] - 2.0;
            const auto b = x[1] - 1.0;
            return a * a + b * b;
        });
    model.subject_to_eq([](const auto& x) { return x[0] - 2.0 * x[1] + 1.0; });
    model.subject_to_ge([](const auto& x) { return -x[0] * x[0] * 0.25 - x[1] * x[1] + 1.0; });

    const opt::OptResult<crd::f64> r1 = model.solve(opts);
    const opt::OptResult<crd::f64> r2 = model.solve(opts);
    REQUIRE(r1.status == r2.status);
    REQUIRE(r1.iterations == r2.iterations);
    REQUIRE(r1.x[0] == r2.x[0]); // bit-identical
    REQUIRE(r1.x[1] == r2.x[1]);
    REQUIRE(r1.fx == r2.fx);
    for (crd::usize i = 0; i < r1.multipliers.size(); ++i)
    {
        REQUIRE(r1.multipliers[i] == r2.multipliers[i]);
    }
}
