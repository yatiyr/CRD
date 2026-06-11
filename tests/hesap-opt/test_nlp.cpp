// crd-hesap-opt v7-n — NLP: damped-BFGS SQP (+ augmented Lagrangian + filter IPM as they land). Validates:
// (1) **HS6 — the stress gate DEFERRED from v7-j**: λ* = 0 leaves the exact-∇²L SQP curvature-free and the ℓ1
// merit creeps (~×0.995/iter, measured there); the N&W-prescribed damped BFGS must converge it FAST from the
// classic start; (2) HS14 (mixed equality + active nonlinear inequality, known f*); (3) Rosenbrock-in-a-disk
// (the scipy reference instance — known boundary solution, μ > 0); (4) the circle projection (analytic x*, λ*)
// — cross-checks the v7-j Newton-SQP result through a different algorithm; (5) determinism (bit-identical
// run-twice + worker counts); (6) boundaries m = 0 (SQP ≡ BFGS) and n = 0.

#include <crd/hesap/opt/opt.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>
#include <cmath>

namespace opt = crd::hesap::opt;

namespace
{
// min (1−x₁)² s.t. 10(x₂ − x₁²) = 0 — HS6. Solution (1, 1), λ* = 0.
class Hs6Obj final : public opt::Objective<crd::f64>
{
public:
    Hs6Obj() noexcept : Objective<crd::f64>(true, false, /*has_hessian=*/true) {}
    [[nodiscard]] crd::f64 value(crd::containers::ConstSpan<crd::f64> x) const override
    {
        const crd::f64 a = 1.0 - x[0];
        return a * a;
    }
    [[nodiscard]] crd::usize n() const noexcept override { return 2; }
    [[nodiscard]] bool gradient(crd::containers::ConstSpan<crd::f64> x,
                                crd::containers::Span<crd::f64> g) const override
    {
        g[0] = -2.0 * (1.0 - x[0]);
        g[1] = 0.0;
        return true;
    }
    [[nodiscard]] bool hessian(crd::containers::ConstSpan<crd::f64>, crd::containers::Span<crd::f64> h) const override
    {
        h[0] = 2.0;
        h[1] = 0.0;
        h[2] = 0.0;
        h[3] = 0.0;
        return true;
    }
};
class Hs6Cons final : public opt::Constraints<crd::f64>
{
public:
    Hs6Cons() noexcept : Constraints<crd::f64>(/*has_jacobians=*/true, /*has_lagrangian_hessian=*/true) {}
    [[nodiscard]] crd::usize num_eq() const noexcept override { return 1; }
    [[nodiscard]] crd::usize num_ineq() const noexcept override { return 0; }
    [[nodiscard]] crd::usize n() const noexcept override { return 2; }
    void eval(crd::containers::ConstSpan<crd::f64> x, crd::containers::Span<crd::f64> ce,
              crd::containers::Span<crd::f64>) const override
    {
        ce[0] = 10.0 * (x[1] - x[0] * x[0]);
    }
    [[nodiscard]] bool jacobians(crd::containers::ConstSpan<crd::f64> x, crd::containers::Span<crd::f64> je,
                                 crd::containers::Span<crd::f64>) const override
    {
        je[0] = -20.0 * x[0];
        je[1] = 10.0;
        return true;
    }
    [[nodiscard]] bool add_lagrangian_hessian(crd::containers::ConstSpan<crd::f64>,
                                              crd::containers::ConstSpan<crd::f64> lambda,
                                              crd::containers::ConstSpan<crd::f64>,
                                              crd::containers::Span<crd::f64> h) const override
    {
        h[0] -= lambda[0] * (-20.0); // −λ·∇²c, ∇²c = [[−20,0],[0,0]]
        return true;
    }
};

// HS14: min (x₁−2)² + (x₂−1)² s.t. x₁ − 2x₂ + 1 = 0 (eq), −x₁²/4 − x₂² + 1 ≥ 0 (ineq).
// f* = 9 − 2.875·√7 ≈ 1.3934649806; x* = ((√7−1)/2, (√7+1)/4).
class Hs14Obj final : public opt::Objective<crd::f64>
{
public:
    Hs14Obj() noexcept : Objective<crd::f64>(true, false, /*has_hessian=*/true) {}
    [[nodiscard]] crd::f64 value(crd::containers::ConstSpan<crd::f64> x) const override
    {
        const crd::f64 a = x[0] - 2.0;
        const crd::f64 b = x[1] - 1.0;
        return a * a + b * b;
    }
    [[nodiscard]] crd::usize n() const noexcept override { return 2; }
    [[nodiscard]] bool gradient(crd::containers::ConstSpan<crd::f64> x,
                                crd::containers::Span<crd::f64> g) const override
    {
        g[0] = 2.0 * (x[0] - 2.0);
        g[1] = 2.0 * (x[1] - 1.0);
        return true;
    }
    [[nodiscard]] bool hessian(crd::containers::ConstSpan<crd::f64>, crd::containers::Span<crd::f64> h) const override
    {
        h[0] = 2.0;
        h[1] = 0.0;
        h[2] = 0.0;
        h[3] = 2.0;
        return true;
    }
};
class Hs14Cons final : public opt::Constraints<crd::f64>
{
public:
    Hs14Cons() noexcept : Constraints<crd::f64>(/*has_jacobians=*/true, /*has_lagrangian_hessian=*/true) {}
    [[nodiscard]] crd::usize num_eq() const noexcept override { return 1; }
    [[nodiscard]] crd::usize num_ineq() const noexcept override { return 1; }
    [[nodiscard]] crd::usize n() const noexcept override { return 2; }
    void eval(crd::containers::ConstSpan<crd::f64> x, crd::containers::Span<crd::f64> ce,
              crd::containers::Span<crd::f64> ci) const override
    {
        ce[0] = x[0] - 2.0 * x[1] + 1.0;
        ci[0] = -0.25 * x[0] * x[0] - x[1] * x[1] + 1.0;
    }
    [[nodiscard]] bool jacobians(crd::containers::ConstSpan<crd::f64> x, crd::containers::Span<crd::f64> je,
                                 crd::containers::Span<crd::f64> ji) const override
    {
        je[0] = 1.0;
        je[1] = -2.0;
        ji[0] = -0.5 * x[0];
        ji[1] = -2.0 * x[1];
        return true;
    }
    [[nodiscard]] bool add_lagrangian_hessian(crd::containers::ConstSpan<crd::f64>,
                                              crd::containers::ConstSpan<crd::f64>,
                                              crd::containers::ConstSpan<crd::f64> mu,
                                              crd::containers::Span<crd::f64> h) const override
    {
        h[0] -= mu[0] * (-0.5); // −μ·∇²c_I, ∇²c_I = [[−0.5,0],[0,−2]] (the equality row is linear)
        h[3] -= mu[0] * (-2.0);
        return true;
    }
};

// Rosenbrock in the unit disk: min 100(y−x²)² + (1−x)² s.t. 1 − x² − y² ≥ 0. Known boundary solution
// x* ≈ (0.7864151510, 0.6176983165) (the scipy reference instance), constraint ACTIVE, μ* > 0.
class RosenObj final : public opt::Objective<crd::f64>
{
public:
    RosenObj() noexcept : Objective<crd::f64>(true, false, /*has_hessian=*/true) {}
    [[nodiscard]] crd::f64 value(crd::containers::ConstSpan<crd::f64> x) const override
    {
        const crd::f64 a = 1.0 - x[0];
        const crd::f64 b = x[1] - x[0] * x[0];
        return a * a + 100.0 * b * b;
    }
    [[nodiscard]] crd::usize n() const noexcept override { return 2; }
    [[nodiscard]] bool gradient(crd::containers::ConstSpan<crd::f64> x,
                                crd::containers::Span<crd::f64> g) const override
    {
        const crd::f64 b = x[1] - x[0] * x[0];
        g[0] = -2.0 * (1.0 - x[0]) - 400.0 * x[0] * b;
        g[1] = 200.0 * b;
        return true;
    }
    [[nodiscard]] bool hessian(crd::containers::ConstSpan<crd::f64> x, crd::containers::Span<crd::f64> h) const override
    {
        h[0] = 2.0 + 1200.0 * x[0] * x[0] - 400.0 * x[1];
        h[1] = -400.0 * x[0];
        h[2] = -400.0 * x[0];
        h[3] = 200.0;
        return true;
    }
};
class DiskCons final : public opt::Constraints<crd::f64>
{
public:
    DiskCons() noexcept : Constraints<crd::f64>(/*has_jacobians=*/true, /*has_lagrangian_hessian=*/true) {}
    [[nodiscard]] crd::usize num_eq() const noexcept override { return 0; }
    [[nodiscard]] crd::usize num_ineq() const noexcept override { return 1; }
    [[nodiscard]] crd::usize n() const noexcept override { return 2; }
    void eval(crd::containers::ConstSpan<crd::f64> x, crd::containers::Span<crd::f64>,
              crd::containers::Span<crd::f64> ci) const override
    {
        ci[0] = 1.0 - x[0] * x[0] - x[1] * x[1];
    }
    [[nodiscard]] bool jacobians(crd::containers::ConstSpan<crd::f64> x, crd::containers::Span<crd::f64>,
                                 crd::containers::Span<crd::f64> ji) const override
    {
        ji[0] = -2.0 * x[0];
        ji[1] = -2.0 * x[1];
        return true;
    }
    [[nodiscard]] bool add_lagrangian_hessian(crd::containers::ConstSpan<crd::f64>,
                                              crd::containers::ConstSpan<crd::f64>,
                                              crd::containers::ConstSpan<crd::f64> mu,
                                              crd::containers::Span<crd::f64> h) const override
    {
        h[0] -= mu[0] * (-2.0); // −μ·∇²c_I, ∇²c_I = −2I
        h[3] -= mu[0] * (-2.0);
        return true;
    }
};

// min (x−2)² + (y−1)² s.t. x² + y² = 1 — the v7-j circle projection: x* = (2,1)/√5, λ* = 1 − √5.
class Proj2Obj final : public opt::Objective<crd::f64>
{
public:
    Proj2Obj() noexcept : Objective<crd::f64>(true, false, /*has_hessian=*/true) {}
    [[nodiscard]] crd::f64 value(crd::containers::ConstSpan<crd::f64> x) const override
    {
        const crd::f64 a = x[0] - 2.0;
        const crd::f64 b = x[1] - 1.0;
        return a * a + b * b;
    }
    [[nodiscard]] crd::usize n() const noexcept override { return 2; }
    [[nodiscard]] bool gradient(crd::containers::ConstSpan<crd::f64> x,
                                crd::containers::Span<crd::f64> g) const override
    {
        g[0] = 2.0 * (x[0] - 2.0);
        g[1] = 2.0 * (x[1] - 1.0);
        return true;
    }
    [[nodiscard]] bool hessian(crd::containers::ConstSpan<crd::f64>, crd::containers::Span<crd::f64> h) const override
    {
        h[0] = 2.0;
        h[1] = 0.0;
        h[2] = 0.0;
        h[3] = 2.0;
        return true;
    }
};
class CircleCons final : public opt::Constraints<crd::f64>
{
public:
    CircleCons() noexcept : Constraints<crd::f64>(/*has_jacobians=*/true, /*has_lagrangian_hessian=*/true) {}
    [[nodiscard]] crd::usize num_eq() const noexcept override { return 1; }
    [[nodiscard]] crd::usize num_ineq() const noexcept override { return 0; }
    [[nodiscard]] crd::usize n() const noexcept override { return 2; }
    void eval(crd::containers::ConstSpan<crd::f64> x, crd::containers::Span<crd::f64> ce,
              crd::containers::Span<crd::f64>) const override
    {
        ce[0] = x[0] * x[0] + x[1] * x[1] - 1.0;
    }
    [[nodiscard]] bool jacobians(crd::containers::ConstSpan<crd::f64> x, crd::containers::Span<crd::f64> je,
                                 crd::containers::Span<crd::f64>) const override
    {
        je[0] = 2.0 * x[0];
        je[1] = 2.0 * x[1];
        return true;
    }
    [[nodiscard]] bool add_lagrangian_hessian(crd::containers::ConstSpan<crd::f64>,
                                              crd::containers::ConstSpan<crd::f64> lambda,
                                              crd::containers::ConstSpan<crd::f64>,
                                              crd::containers::Span<crd::f64> h) const override
    {
        h[0] -= lambda[0] * 2.0; // −λ·∇²c, ∇²c = 2I
        h[3] -= lambda[0] * 2.0;
        return true;
    }
};

// f = ‖x − c‖² with no constraints — the m = 0 boundary vehicle.
class ShiftObj final : public opt::Objective<crd::f64>
{
public:
    explicit ShiftObj(crd::containers::ConstSpan<crd::f64> c) noexcept : Objective<crd::f64>(true, false), m_c(c) {}
    [[nodiscard]] crd::f64 value(crd::containers::ConstSpan<crd::f64> x) const override
    {
        crd::f64 acc = 0.0;
        for (crd::usize i = 0; i < m_c.size(); ++i)
        {
            const crd::f64 d = x[i] - m_c[i];
            acc += d * d;
        }
        return acc;
    }
    [[nodiscard]] crd::usize n() const noexcept override { return m_c.size(); }
    [[nodiscard]] bool gradient(crd::containers::ConstSpan<crd::f64> x,
                                crd::containers::Span<crd::f64> g) const override
    {
        for (crd::usize i = 0; i < m_c.size(); ++i)
        {
            g[i] = 2.0 * (x[i] - m_c[i]);
        }
        return true;
    }

private:
    crd::containers::ConstSpan<crd::f64> m_c;
};
class NoCons final : public opt::Constraints<crd::f64>
{
public:
    explicit NoCons(crd::usize n) noexcept : Constraints<crd::f64>(/*has_jacobians=*/true), m_n(n) {}
    [[nodiscard]] crd::usize num_eq() const noexcept override { return 0; }
    [[nodiscard]] crd::usize num_ineq() const noexcept override { return 0; }
    [[nodiscard]] crd::usize n() const noexcept override { return m_n; }
    void eval(crd::containers::ConstSpan<crd::f64>, crd::containers::Span<crd::f64>,
              crd::containers::Span<crd::f64>) const override
    {
    }
    [[nodiscard]] bool jacobians(crd::containers::ConstSpan<crd::f64>, crd::containers::Span<crd::f64>,
                                 crd::containers::Span<crd::f64>) const override
    {
        return true;
    }

private:
    crd::usize m_n;
};
} // namespace

TEST_CASE("v7-n SQP: HS6 (the deferred lambda-zero stress gate) converges fast", "[hesap][opt][v7]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    const Hs6Obj obj;
    const Hs6Cons cons;
    crd::containers::Array<crd::f64> x0(&alloc);
    x0.resize(2);
    x0[0] = -1.2; // the classic start that creeps under exact-Hessian l1-SQP (v7-j, measured)
    x0[1] = 1.0;
    opt::OptOptions<crd::f64> opts;
    opts.grad_tol = 1e-8;
    opts.max_iters = 100;
    auto r = opt::minimize_sqp<crd::f64>(obj, cons, {x0.data(), 2}, opts, &alloc);
    REQUIRE(r.status == opt::OptStatus::Success);
    CHECK(std::fabs(r.x[0] - 1.0) < 1e-6);
    CHECK(std::fabs(r.x[1] - 1.0) < 1e-6);
    CHECK(r.fx < 1e-12);
    CHECK(r.iterations < 60); // the damped-BFGS cure: fast, not the measured ~×0.995/iter creep
    CHECK(r.kkt_residual < 1e-8);
}

TEST_CASE("v7-n SQP: HS14 (equality + active nonlinear inequality)", "[hesap][opt][v7]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    const Hs14Obj obj;
    const Hs14Cons cons;
    crd::containers::Array<crd::f64> x0(&alloc);
    x0.resize(2);
    x0[0] = 2.0; // the standard HS14 start
    x0[1] = 2.0;
    opt::OptOptions<crd::f64> opts;
    opts.grad_tol = 1e-9;
    opts.max_iters = 100;
    auto r = opt::minimize_sqp<crd::f64>(obj, cons, {x0.data(), 2}, opts, &alloc);
    REQUIRE(r.status == opt::OptStatus::Success);
    const crd::f64 s7 = std::sqrt(7.0);
    CHECK(std::fabs(r.x[0] - 0.5 * (s7 - 1.0)) < 1e-7);
    CHECK(std::fabs(r.x[1] - 0.25 * (s7 + 1.0)) < 1e-7);
    CHECK(std::fabs(r.fx - (9.0 - 2.875 * s7)) < 1e-9); // the published f*
    CHECK(r.multipliers[1] >= 0.0);                     // the inequality multiplier sign
    CHECK(r.kkt_residual < 1e-8);
}

TEST_CASE("v7-n SQP: Rosenbrock in the unit disk (the scipy reference instance)", "[hesap][opt][v7]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    const RosenObj obj;
    const DiskCons cons;
    crd::containers::Array<crd::f64> x0(&alloc);
    x0.resize(2);
    x0[0] = 0.0;
    x0[1] = 0.0;
    opt::OptOptions<crd::f64> opts;
    opts.grad_tol = 1e-8;
    opts.max_iters = 200;
    auto r = opt::minimize_sqp<crd::f64>(obj, cons, {x0.data(), 2}, opts, &alloc);
    REQUIRE(r.status == opt::OptStatus::Success);
    CHECK(std::fabs(r.x[0] - 0.7864151510) < 1e-6);
    CHECK(std::fabs(r.x[1] - 0.6176983165) < 1e-6);
    CHECK(std::fabs(r.x[0] * r.x[0] + r.x[1] * r.x[1] - 1.0) < 1e-8); // active at the boundary
    CHECK(r.multipliers[0] > 0.0);                                    // with a strictly positive multiplier
    CHECK(r.kkt_residual < 1e-8);
}

TEST_CASE("v7-n SQP: circle projection matches the v7-j Newton-SQP analytics", "[hesap][opt][v7]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    const Proj2Obj obj;
    const CircleCons cons;
    crd::containers::Array<crd::f64> x0(&alloc);
    x0.resize(2);
    x0[0] = 0.0;
    x0[1] = 1.5;
    opt::OptOptions<crd::f64> opts;
    opts.grad_tol = 1e-9;
    opts.max_iters = 100;
    auto r = opt::minimize_sqp<crd::f64>(obj, cons, {x0.data(), 2}, opts, &alloc);
    REQUIRE(r.status == opt::OptStatus::Success);
    const crd::f64 s5 = std::sqrt(5.0);
    CHECK(std::fabs(r.x[0] - 2.0 / s5) < 1e-8);
    CHECK(std::fabs(r.x[1] - 1.0 / s5) < 1e-8);
    CHECK(std::fabs(r.multipliers[0] - (1.0 - s5)) < 1e-7); // λ* = 1 − √5
}

TEST_CASE("v7-n SQP determinism: bit-identical across runs + worker counts", "[hesap][opt][v7][moat]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    const RosenObj obj;
    const DiskCons cons;
    crd::containers::Array<crd::f64> x0(&alloc);
    x0.resize(2);
    x0[0] = 0.0;
    x0[1] = 0.0;
    opt::OptOptions<crd::f64> opts;
    opts.grad_tol = 1e-8;
    opts.max_iters = 200;
    const auto r1 = opt::minimize_sqp<crd::f64>(obj, cons, {x0.data(), 2}, opts, &alloc);
    const auto r2 = opt::minimize_sqp<crd::f64>(obj, cons, {x0.data(), 2}, opts, &alloc);
    REQUIRE(r1.status == opt::OptStatus::Success);
    REQUIRE(r1.iterations == r2.iterations);
    CHECK(r1.x[0] == r2.x[0]);
    CHECK(r1.x[1] == r2.x[1]);
    CHECK(r1.multipliers[0] == r2.multipliers[0]);
    for (crd::u32 nw : {1U, 2U, 4U, 8U})
    {
        crd::jobs::Config cfg;
        cfg.num_threads = nw;
        crd::jobs::init(cfg);
        {
            const auto r = opt::minimize_sqp<crd::f64>(obj, cons, {x0.data(), 2}, opts, &alloc);
            CHECK(r.x[0] == r1.x[0]);
            CHECK(r.x[1] == r1.x[1]);
        }
        crd::jobs::shutdown();
    }
}

TEST_CASE("v7-n augmented Lagrangian: the same battery through a different method", "[hesap][opt][v7]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    opt::OptOptions<crd::f64> opts;
    opts.grad_tol = 1e-7; // first-order inner solves: one decade looser than the SQP gates
    opts.max_iters = 50;  // outer iterations

    {
        const Hs6Obj obj; // the λ* = 0 problem — auglag has no exact-Hessian curvature trap at all
        const Hs6Cons cons;
        crd::containers::Array<crd::f64> x0(&alloc);
        x0.resize(2);
        x0[0] = -1.2;
        x0[1] = 1.0;
        auto r = opt::minimize_auglag<crd::f64>(obj, cons, {x0.data(), 2}, opts, &alloc);
        REQUIRE(r.status == opt::OptStatus::Success);
        CHECK(std::fabs(r.x[0] - 1.0) < 1e-5);
        CHECK(std::fabs(r.x[1] - 1.0) < 1e-5);
        CHECK(r.kkt_residual < 1e-7);
    }
    {
        const Hs14Obj obj;
        const Hs14Cons cons;
        crd::containers::Array<crd::f64> x0(&alloc);
        x0.resize(2);
        x0[0] = 2.0;
        x0[1] = 2.0;
        auto r = opt::minimize_auglag<crd::f64>(obj, cons, {x0.data(), 2}, opts, &alloc);
        REQUIRE(r.status == opt::OptStatus::Success);
        const crd::f64 s7 = std::sqrt(7.0);
        CHECK(std::fabs(r.fx - (9.0 - 2.875 * s7)) < 1e-6);
        CHECK(r.multipliers[1] >= 0.0);
        CHECK(r.kkt_residual < 1e-7);
    }
    {
        const RosenObj obj;
        const DiskCons cons;
        crd::containers::Array<crd::f64> x0(&alloc);
        x0.resize(2);
        x0[0] = 0.0;
        x0[1] = 0.0;
        auto r = opt::minimize_auglag<crd::f64>(obj, cons, {x0.data(), 2}, opts, &alloc);
        REQUIRE(r.status == opt::OptStatus::Success);
        CHECK(std::fabs(r.x[0] - 0.7864151510) < 1e-5);
        CHECK(std::fabs(r.x[1] - 0.6176983165) < 1e-5);
        CHECK(r.multipliers[0] > 0.0);
    }
    {
        const Proj2Obj obj;
        const CircleCons cons;
        crd::containers::Array<crd::f64> x0(&alloc);
        x0.resize(2);
        x0[0] = 0.0;
        x0[1] = 1.5;
        auto r = opt::minimize_auglag<crd::f64>(obj, cons, {x0.data(), 2}, opts, &alloc);
        REQUIRE(r.status == opt::OptStatus::Success);
        const crd::f64 s5 = std::sqrt(5.0);
        CHECK(std::fabs(r.x[0] - 2.0 / s5) < 1e-5);
        CHECK(std::fabs(r.x[1] - 1.0 / s5) < 1e-5);
        CHECK(std::fabs(r.multipliers[0] - (1.0 - s5)) < 1e-4); // first-order multiplier estimates
    }
    {
        // Determinism: bit-identical run-twice (outer + L-BFGS inner are serial recurrences).
        const Hs14Obj obj;
        const Hs14Cons cons;
        crd::containers::Array<crd::f64> x0(&alloc);
        x0.resize(2);
        x0[0] = 2.0;
        x0[1] = 2.0;
        const auto a = opt::minimize_auglag<crd::f64>(obj, cons, {x0.data(), 2}, opts, &alloc);
        const auto b = opt::minimize_auglag<crd::f64>(obj, cons, {x0.data(), 2}, opts, &alloc);
        CHECK(a.x[0] == b.x[0]);
        CHECK(a.x[1] == b.x[1]);
        CHECK(a.iterations == b.iterations);
    }
}

TEST_CASE("v7-n-2 filter interior point: the battery through the third method", "[hesap][opt][v7]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    opt::OptOptions<crd::f64> opts;
    opts.grad_tol = 1e-8; // the NLP error E_0
    opts.max_iters = 300; // total Newton iterations across all barrier problems

    {
        const Hs6Obj obj; // mi = 0: the degenerate-barrier (equality-only) path
        const Hs6Cons cons;
        crd::containers::Array<crd::f64> x0(&alloc);
        x0.resize(2);
        x0[0] = -1.2;
        x0[1] = 1.0;
        auto r = opt::minimize_interior_point<crd::f64>(obj, cons, {x0.data(), 2}, opts, &alloc);
        REQUIRE(r.status == opt::OptStatus::Success);
        CHECK(std::fabs(r.x[0] - 1.0) < 1e-6);
        CHECK(std::fabs(r.x[1] - 1.0) < 1e-6);
        CHECK(r.kkt_residual < 1e-8);
    }
    {
        const Hs14Obj obj;
        const Hs14Cons cons;
        crd::containers::Array<crd::f64> x0(&alloc);
        x0.resize(2);
        x0[0] = 2.0;
        x0[1] = 2.0;
        auto r = opt::minimize_interior_point<crd::f64>(obj, cons, {x0.data(), 2}, opts, &alloc);
        REQUIRE(r.status == opt::OptStatus::Success);
        const crd::f64 s7 = std::sqrt(7.0);
        CHECK(std::fabs(r.fx - (9.0 - 2.875 * s7)) < 1e-7);
        CHECK(r.multipliers[1] >= 0.0); // z ≥ 0 by the fraction-to-boundary + κ_Σ clip
        CHECK(r.kkt_residual < 1e-8);
    }
    {
        const RosenObj obj;
        const DiskCons cons;
        crd::containers::Array<crd::f64> x0(&alloc);
        x0.resize(2);
        x0[0] = 0.0;
        x0[1] = 0.0;
        auto r = opt::minimize_interior_point<crd::f64>(obj, cons, {x0.data(), 2}, opts, &alloc);
        REQUIRE(r.status == opt::OptStatus::Success);
        CHECK(std::fabs(r.x[0] - 0.7864151510) < 1e-6);
        CHECK(std::fabs(r.x[1] - 0.6176983165) < 1e-6);
        CHECK(r.multipliers[0] > 0.0); // the boundary multiplier
        CHECK(r.kkt_residual < 1e-8);
    }
    {
        const Proj2Obj obj;
        const CircleCons cons;
        crd::containers::Array<crd::f64> x0(&alloc);
        x0.resize(2);
        x0[0] = 0.0;
        x0[1] = 1.5;
        auto r = opt::minimize_interior_point<crd::f64>(obj, cons, {x0.data(), 2}, opts, &alloc);
        REQUIRE(r.status == opt::OptStatus::Success);
        const crd::f64 s5 = std::sqrt(5.0);
        CHECK(std::fabs(r.x[0] - 2.0 / s5) < 1e-7);
        CHECK(std::fabs(r.x[1] - 1.0 / s5) < 1e-7);
        CHECK(std::fabs(r.multipliers[0] - (1.0 - s5)) < 1e-6); // λ* = 1 − √5 via a FIFTH algorithm
    }
    {
        // Determinism: bit-identical run-twice (filter + μ schedule + FTB are all fixed-order arithmetic).
        const RosenObj obj;
        const DiskCons cons;
        crd::containers::Array<crd::f64> x0(&alloc);
        x0.resize(2);
        x0[0] = 0.0;
        x0[1] = 0.0;
        const auto a = opt::minimize_interior_point<crd::f64>(obj, cons, {x0.data(), 2}, opts, &alloc);
        const auto b = opt::minimize_interior_point<crd::f64>(obj, cons, {x0.data(), 2}, opts, &alloc);
        CHECK(a.x[0] == b.x[0]);
        CHECK(a.x[1] == b.x[1]);
        CHECK(a.iterations == b.iterations);
    }
}

TEST_CASE("v7-n SQP boundaries: m = 0 (SQP = BFGS) and n = 0", "[hesap][opt][v7]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    {
        const crd::f64 c[] = {1.0, -2.0, 0.5};
        const ShiftObj obj({c, 3});
        const NoCons cons(3);
        crd::containers::Array<crd::f64> x0(&alloc);
        x0.resize(3);
        for (crd::usize i = 0; i < 3; ++i)
        {
            x0[i] = 0.0;
        }
        opt::OptOptions<crd::f64> opts;
        opts.grad_tol = 1e-9;
        opts.max_iters = 100;
        auto r = opt::minimize_sqp<crd::f64>(obj, cons, {x0.data(), 3}, opts, &alloc);
        REQUIRE(r.status == opt::OptStatus::Success);
        for (crd::usize i = 0; i < 3; ++i)
        {
            CHECK(std::fabs(r.x[i] - c[i]) < 1e-7);
        }
        CHECK(r.multipliers.size() == 0);
    }
    {
        const ShiftObj obj({static_cast<const crd::f64*>(nullptr), 0});
        const NoCons cons(0);
        opt::OptOptions<crd::f64> opts;
        auto r = opt::minimize_sqp<crd::f64>(obj, cons, {static_cast<const crd::f64*>(nullptr), 0}, opts, &alloc);
        REQUIRE(r.status == opt::OptStatus::Success);
    }
}
