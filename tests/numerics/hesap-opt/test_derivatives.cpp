// crd-hesap-opt v7-b — derivatives. Validates: (1) the Dual<T> forward-mode AD scalar (algebra + transcendental
// chain rules, exact to rounding); (2) forward_ad_gradient against closed-form gradients (polynomial Rosenbrock +
// a transcendental functor); (3) the FunctorObjective adapter (value real-path + AD gradient + capability flags);
// (4) the scale-relative finite-difference gradient (forward + central) against the analytic gradient on a
// deliberately badly-scaled point; (5) the gradient_check harness — passes a correct analytic gradient and FLAGS a
// deliberately-wrong one; (6) the FD-over-parallel determinism moat {1,2,4,8,16} (the composition v7-d's L-BFGS
// moat rests on: an FD gradient over a bit-exact parallel objective is itself bit-identical across worker counts).

#include <crd/hesap/opt/opt.hpp>
#include <crd/hesap/sparse/parallel_sparse_linear_op.hpp>
#include <crd/hesap/sparse/sparse_linear_op.hpp>
#include <crd/hesap/sparse/triplet_builder.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>

namespace opt = crd::hesap::opt;
namespace sp = crd::hesap::sparse;

namespace
{
using Csr = sp::SparseMatrix<crd::f64, sp::SparseFormat::Csr>;
using D = opt::Dual<crd::f64>;

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

// Scalar-generic functors (run on both crd::f64 and Dual<crd::f64>).
struct Rosenbrock2D
{
    template <typename S>
    S operator()(crd::containers::ConstSpan<S> x) const
    {
        const S a = S(1) - x[0];
        const S b = x[1] - x[0] * x[0];
        return a * a + S(100) * (b * b);
    }
};

struct TransFunctor // f(x) = sin(x0)·exp(x1)
{
    template <typename S>
    S operator()(crd::containers::ConstSpan<S> x) const
    {
        using std::exp;
        using std::sin;
        return sin(x[0]) * exp(x[1]);
    }
};

// An objective with a CORRECT value (½‖x‖²) but a deliberately WRONG analytic gradient (2x instead of x) — the
// gradient_check harness must catch it.
class WrongGradObjective final : public opt::Objective<crd::f64>
{
public:
    explicit WrongGradObjective(crd::usize n)
        : opt::Objective<crd::f64>(/*has_gradient=*/true, /*has_hessian_vector=*/false), m_n(n)
    {
    }

    [[nodiscard]] crd::f64 value(crd::containers::ConstSpan<crd::f64> x) const override
    {
        crd::f64 s = 0.0;
        for (crd::usize i = 0; i < x.size(); ++i)
        {
            s += 0.5 * x[i] * x[i];
        }
        return s;
    }
    [[nodiscard]] crd::usize n() const noexcept override { return m_n; }
    [[nodiscard]] bool gradient(crd::containers::ConstSpan<crd::f64> x,
                                crd::containers::Span<crd::f64> g) const override
    {
        for (crd::usize i = 0; i < x.size(); ++i)
        {
            g[i] = 2.0 * x[i]; // WRONG (true ∇ = x)
        }
        return true;
    }

private:
    crd::usize m_n;
};

using Catch::Matchers::WithinRel;
} // namespace

TEST_CASE("v7-b Dual forward-mode AD: algebra + transcendental chain rules", "[hesap][opt][v7]")
{
    // f(x) = x³ at x=2 ⇒ f=8, f'=3x²=12.
    const D x{2.0, 1.0};
    const D x3 = x * x * x;
    CHECK_THAT(x3.v, WithinRel(8.0, 1e-14));
    CHECK_THAT(x3.d, WithinRel(12.0, 1e-14));

    // Transcendentals: value = std fn, derivative = chain rule.
    CHECK_THAT(opt::sin(D{0.5, 1.0}).d, WithinRel(std::cos(0.5), 1e-13));
    CHECK_THAT(opt::cos(D{0.5, 1.0}).d, WithinRel(-std::sin(0.5), 1e-13));
    CHECK_THAT(opt::exp(D{1.0, 1.0}).d, WithinRel(std::exp(1.0), 1e-13));
    CHECK_THAT(opt::log(D{2.0, 1.0}).d, WithinRel(0.5, 1e-13));            // 1/x
    CHECK_THAT(opt::sqrt(D{4.0, 1.0}).d, WithinRel(0.25, 1e-13));         // 1/(2√x)
    CHECK_THAT(opt::tan(D{0.3, 1.0}).d, WithinRel(1.0 / (std::cos(0.3) * std::cos(0.3)), 1e-12));
    CHECK_THAT(opt::tanh(D{0.4, 1.0}).d, WithinRel(1.0 - std::tanh(0.4) * std::tanh(0.4), 1e-12));
    CHECK_THAT(opt::pow(D{3.0, 1.0}, 2.0).d, WithinRel(6.0, 1e-13));      // p·x^(p-1) = 2·3
    CHECK_THAT(opt::abs(D{-2.0, 1.0}).d, WithinRel(-1.0, 1e-14));         // sign(x)·x'

    // Quotient rule with the numerator seeded: d/da (a/b) = 1/b.
    const D q = D{6.0, 1.0} / D{2.0, 0.0};
    CHECK_THAT(q.v, WithinRel(3.0, 1e-14));
    CHECK_THAT(q.d, WithinRel(0.5, 1e-14));
}

TEST_CASE("v7-b forward-AD gradient matches closed-form", "[hesap][opt][v7]")
{
    crd::memory::TlsfAllocator alloc(1U << 20);

    // Rosenbrock at the classic (-1.2, 1.0).
    crd::containers::Array<crd::f64> x(&alloc);
    crd::containers::Array<crd::f64> g(&alloc);
    x.resize(2);
    g.resize(2);
    x[0] = -1.2;
    x[1] = 1.0;
    const crd::f64 fval = opt::forward_ad_gradient<crd::f64>(Rosenbrock2D{}, {x.data(), 2}, {g.data(), 2}, &alloc);

    const crd::f64 a = 1.0 - x[0];
    const crd::f64 d = x[1] - x[0] * x[0];
    CHECK_THAT(fval, WithinRel(a * a + 100.0 * d * d, 1e-12));
    CHECK_THAT(g[0], WithinRel(-2.0 * a - 400.0 * x[0] * d, 1e-10)); // ∂f/∂x
    CHECK_THAT(g[1], WithinRel(200.0 * d, 1e-10));                   // ∂f/∂y

    // Transcendental f = sin(x0)·exp(x1) at (0.3, 0.7).
    x[0] = 0.3;
    x[1] = 0.7;
    const crd::f64 tval = opt::forward_ad_gradient<crd::f64>(TransFunctor{}, {x.data(), 2}, {g.data(), 2}, &alloc);
    CHECK_THAT(tval, WithinRel(std::sin(0.3) * std::exp(0.7), 1e-13));
    CHECK_THAT(g[0], WithinRel(std::cos(0.3) * std::exp(0.7), 1e-12));
    CHECK_THAT(g[1], WithinRel(std::sin(0.3) * std::exp(0.7), 1e-12));
}

TEST_CASE("v7-b FunctorObjective adapter: real value + AD gradient + capabilities", "[hesap][opt][v7]")
{
    crd::memory::TlsfAllocator alloc(1U << 20);
    auto obj = opt::make_objective_from_functor<crd::f64>(Rosenbrock2D{}, 2, &alloc);

    REQUIRE(obj.n() == 2);
    REQUIRE(obj.has_gradient());
    REQUIRE_FALSE(obj.has_hessian_vector());

    crd::containers::Array<crd::f64> x(&alloc);
    crd::containers::Array<crd::f64> g(&alloc);
    x.resize(2);
    g.resize(2);
    x[0] = 0.5;
    x[1] = -0.25;

    const crd::f64 a = 1.0 - x[0];
    const crd::f64 d = x[1] - x[0] * x[0];
    CHECK_THAT(obj.value({x.data(), 2}), WithinRel(a * a + 100.0 * d * d, 1e-12));

    REQUIRE(obj.gradient({x.data(), 2}, {g.data(), 2}));
    CHECK_THAT(g[0], WithinRel(-2.0 * a - 400.0 * x[0] * d, 1e-10));
    CHECK_THAT(g[1], WithinRel(200.0 * d, 1e-10));
}

TEST_CASE("v7-b finite-difference gradient (forward/central) matches analytic on a scaled point",
          "[hesap][opt][v7]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    const crd::u32 n = 12;
    Csr mat = well_conditioned_spd(&alloc, n);
    sp::SparseLinearOp<crd::f64> op(mat);

    crd::containers::Array<crd::f64> b(&alloc);
    crd::containers::Array<crd::f64> x(&alloc);
    b.resize(n);
    x.resize(n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        b[i] = 0.3 * static_cast<crd::f64>(i) - 1.0;
        // Deliberately badly-scaled x (0.01 … 100) — a FIXED absolute FD step would be wrong here.
        x[i] = std::pow(10.0, static_cast<crd::f64>(static_cast<int>(i % 5) - 2));
    }
    opt::QuadraticObjective<crd::f64> obj(op, {b.data(), n}, &alloc);

    crd::containers::Array<crd::f64> ga(&alloc);
    crd::containers::Array<crd::f64> gf(&alloc);
    crd::containers::Array<crd::f64> gc(&alloc);
    ga.resize(n);
    gf.resize(n);
    gc.resize(n);
    REQUIRE(obj.gradient({x.data(), n}, {ga.data(), n})); // analytic A·x − b
    opt::finite_difference_gradient<crd::f64>(obj, {x.data(), n}, {gf.data(), n}, opt::FdMode::Forward, &alloc);
    opt::finite_difference_gradient<crd::f64>(obj, {x.data(), n}, {gc.data(), n}, opt::FdMode::Central, &alloc);

    // HONEST tolerances: FD agreement is bounded by the GLOBAL function-value roundoff floor (~ε·|f|/h), which
    // this deliberately badly-scaled + coupled point (|f|~1e4) stresses — not just the per-component step. The
    // scale-relative step is what keeps it even this good; a fixed absolute step would be far worse on the 0.01
    // and 100 components. (For a quadratic, central truncation is zero — f'''=0 — so central is roundoff-only.)
    for (crd::u32 i = 0; i < n; ++i)
    {
        const crd::f64 denom = std::max(std::fabs(ga[i]), 1.0);
        CHECK(std::fabs(gf[i] - ga[i]) / denom < 1e-4);  // forward O(√ε), degraded by the global |f| floor
        CHECK(std::fabs(gc[i] - ga[i]) / denom < 1e-6);  // central O(ε^(2/3)), roundoff-only on a quadratic
    }
}

TEST_CASE("v7-b gradient_check passes a correct gradient and flags a wrong one", "[hesap][opt][v7]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    const crd::u32 n = 10;

    crd::containers::Array<crd::f64> x(&alloc);
    x.resize(n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        x[i] = 1.0 + 0.2 * static_cast<crd::f64>(i);
    }

    // Correct analytic gradient (QuadraticObjective: ∇ = A·x − b).
    Csr mat = well_conditioned_spd(&alloc, n);
    sp::SparseLinearOp<crd::f64> op(mat);
    crd::containers::Array<crd::f64> b(&alloc);
    b.resize(n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        b[i] = 0.5 * static_cast<crd::f64>(i);
    }
    opt::QuadraticObjective<crd::f64> good(op, {b.data(), n}, &alloc);
    const auto rgood = opt::gradient_check<crd::f64>(good, {x.data(), n}, &alloc);
    CHECK(rgood.max_rel_err < 1e-5); // a correct gradient sits at the FD noise floor

    // Deliberately wrong gradient (2x vs the true x).
    WrongGradObjective bad(n);
    const auto rbad = opt::gradient_check<crd::f64>(bad, {x.data(), n}, &alloc);
    CHECK(rbad.max_rel_err > 1e-2); // caught
}

TEST_CASE("v7-b finite-difference gradient determinism moat {1,2,4,8,16} (parallel objective)",
          "[hesap][opt][v7][moat]")
{
    const crd::u32 n = 32;
    crd::memory::TlsfAllocator alloc(1U << 24);
    Csr mat = well_conditioned_spd(&alloc, n);

    crd::containers::Array<crd::f64> b(&alloc);
    crd::containers::Array<crd::f64> x(&alloc);
    b.resize(n);
    x.resize(n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        b[i] = 0.2 * static_cast<crd::f64>(i) - 0.5;
        x[i] = 1.0 + 0.05 * static_cast<crd::f64>(i);
    }

    crd::containers::Array<crd::f64> g_ref(&alloc);
    bool have_ref = false;
    for (crd::u32 nw : {1U, 2U, 4U, 8U, 16U})
    {
        crd::jobs::Config cfg;
        cfg.num_threads = nw;
        crd::jobs::init(cfg);
        {
            // value() runs A·x bit-exact across worker counts ⇒ the FD gradient over it is bit-identical too.
            sp::ParallelSparseLinearOp<crd::f64> op(mat, &alloc, /*parallel_min_stored_bytes=*/0);
            opt::QuadraticObjective<crd::f64> inner(op, {b.data(), n}, &alloc);
            opt::FiniteDiffObjective<crd::f64> fd(inner, &alloc, opt::FdMode::Central);

            crd::containers::Array<crd::f64> g(&alloc);
            g.resize(n);
            REQUIRE(fd.gradient({x.data(), n}, {g.data(), n}));
            if (!have_ref)
            {
                g_ref.resize(n);
                for (crd::u32 i = 0; i < n; ++i)
                {
                    g_ref[i] = g[i];
                }
                have_ref = true;
            }
            else
            {
                bool ident = true;
                for (crd::u32 i = 0; i < n && ident; ++i)
                {
                    ident = (g[i] == g_ref[i]);
                }
                CHECK(ident); // FD gradient bit-identical across worker counts
            }
        }
        crd::jobs::shutdown();
    }
}
