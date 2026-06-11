// crd-hesap-opt v7-j — the constrained substrate. Validates: (1) the 4-part KKT residual against ANALYTIC
// KKT points (equality + inequality with active-set complementarity); (2) the dense Bunch-Kaufman KKT solve —
// stationarity/feasibility certificate of the saddle solution, the inertia test NOT over-regularizing
// (indefinite W with PD reduced Hessian ⇒ δ = 0), and the δ-ladder firing on an indefinite reduced Hessian;
// (3) the least-squares multiplier estimate at an analytic KKT point; (4) the ℓ1 merit directional derivative
// against finite differences (positive/negative/violated/inactive constraint rows); (5) the substrate PROVER
// `minimize_sqp_equality`: ONE-step convergence on an equality QP (the sharp Newton-KKT gate), the circle
// problem (nonlinear constraint CURVATURE through add_lagrangian_hessian), and circle projection; (6) the moat
// {1,2,4,8,16} (quartic objective over the parallel-but-bit-exact spmv + a linear constraint); (7) boundaries
// m = 0 (SQP ≡ Newton) and n = 0.

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

// f = ‖x‖² (∇ = 2x, H = 2I) — the analytic-KKT vehicle.
class SphereObjective final : public opt::Objective<crd::f64>
{
public:
    explicit SphereObjective(crd::usize n) noexcept
        : Objective<crd::f64>(true, /*has_hessian_vector=*/false, /*has_hessian=*/true), m_n(n)
    {
    }
    [[nodiscard]] crd::f64 value(crd::containers::ConstSpan<crd::f64> x) const override
    {
        crd::f64 acc = 0.0;
        for (crd::usize i = 0; i < m_n; ++i)
        {
            acc += x[i] * x[i];
        }
        return acc;
    }
    [[nodiscard]] crd::usize n() const noexcept override { return m_n; }
    [[nodiscard]] bool gradient(crd::containers::ConstSpan<crd::f64> x,
                                crd::containers::Span<crd::f64> g) const override
    {
        for (crd::usize i = 0; i < m_n; ++i)
        {
            g[i] = 2.0 * x[i];
        }
        return true;
    }
    [[nodiscard]] bool hessian(crd::containers::ConstSpan<crd::f64>, crd::containers::Span<crd::f64> h) const override
    {
        for (crd::usize k = 0; k < m_n * m_n; ++k)
        {
            h[k] = 0.0;
        }
        for (crd::usize i = 0; i < m_n; ++i)
        {
            h[i * m_n + i] = 2.0;
        }
        return true;
    }

private:
    crd::usize m_n;
};

// Linear constraints c_E = A_E·x − b_E (rows of a fixed row-major matrix), c_I = A_I·x − b_I. No curvature.
class LinearConstraints final : public opt::Constraints<crd::f64>
{
public:
    LinearConstraints(crd::usize n, crd::containers::ConstSpan<crd::f64> ae, crd::containers::ConstSpan<crd::f64> be,
                      crd::containers::ConstSpan<crd::f64> ai, crd::containers::ConstSpan<crd::f64> bi) noexcept
        : Constraints<crd::f64>(/*has_jacobians=*/true), m_n(n), m_ae(ae), m_be(be), m_ai(ai), m_bi(bi)
    {
    }
    [[nodiscard]] crd::usize num_eq() const noexcept override { return m_be.size(); }
    [[nodiscard]] crd::usize num_ineq() const noexcept override { return m_bi.size(); }
    [[nodiscard]] crd::usize n() const noexcept override { return m_n; }
    void eval(crd::containers::ConstSpan<crd::f64> x, crd::containers::Span<crd::f64> ce,
              crd::containers::Span<crd::f64> ci) const override
    {
        for (crd::usize i = 0; i < m_be.size(); ++i)
        {
            crd::f64 acc = -m_be[i];
            for (crd::usize j = 0; j < m_n; ++j)
            {
                acc += m_ae[i * m_n + j] * x[j];
            }
            ce[i] = acc;
        }
        for (crd::usize i = 0; i < m_bi.size(); ++i)
        {
            crd::f64 acc = -m_bi[i];
            for (crd::usize j = 0; j < m_n; ++j)
            {
                acc += m_ai[i * m_n + j] * x[j];
            }
            ci[i] = acc;
        }
    }
    [[nodiscard]] bool jacobians(crd::containers::ConstSpan<crd::f64>, crd::containers::Span<crd::f64> je,
                                 crd::containers::Span<crd::f64> ji) const override
    {
        for (crd::usize k = 0; k < m_be.size() * m_n; ++k)
        {
            je[k] = m_ae[k];
        }
        for (crd::usize k = 0; k < m_bi.size() * m_n; ++k)
        {
            ji[k] = m_ai[k];
        }
        return true;
    }

private:
    crd::usize m_n;
    crd::containers::ConstSpan<crd::f64> m_ae;
    crd::containers::ConstSpan<crd::f64> m_be;
    crd::containers::ConstSpan<crd::f64> m_ai;
    crd::containers::ConstSpan<crd::f64> m_bi;
};

// f = aᵀx (linear; zero Hessian) — paired with the circle constraint below.
class LinearObjective final : public opt::Objective<crd::f64>
{
public:
    explicit LinearObjective(crd::containers::ConstSpan<crd::f64> a) noexcept
        : Objective<crd::f64>(true, false, /*has_hessian=*/true), m_a(a)
    {
    }
    [[nodiscard]] crd::f64 value(crd::containers::ConstSpan<crd::f64> x) const override
    {
        crd::f64 acc = 0.0;
        for (crd::usize i = 0; i < m_a.size(); ++i)
        {
            acc += m_a[i] * x[i];
        }
        return acc;
    }
    [[nodiscard]] crd::usize n() const noexcept override { return m_a.size(); }
    [[nodiscard]] bool gradient(crd::containers::ConstSpan<crd::f64>, crd::containers::Span<crd::f64> g) const override
    {
        for (crd::usize i = 0; i < m_a.size(); ++i)
        {
            g[i] = m_a[i];
        }
        return true;
    }
    [[nodiscard]] bool hessian(crd::containers::ConstSpan<crd::f64>, crd::containers::Span<crd::f64> h) const override
    {
        for (crd::usize k = 0; k < m_a.size() * m_a.size(); ++k)
        {
            h[k] = 0.0;
        }
        return true;
    }

private:
    crd::containers::ConstSpan<crd::f64> m_a;
};

// The unit circle: c_E = x² + y² − 1 = 0. NONLINEAR — contributes curvature −λ·∇²c = −λ·2I through the
// Lagrangian-Hessian hook (the v7-j capability the SQP must consume to converge on a linear objective).
class CircleConstraint final : public opt::Constraints<crd::f64>
{
public:
    CircleConstraint() noexcept : Constraints<crd::f64>(/*has_jacobians=*/true, /*has_lagrangian_hessian=*/true) {}
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

// Circle projection: min (x−2)² + (y−1)² s.t. x² + y² = 1 → x* = (2,1)/√5, λ* = 1 − √5 (NONZERO ⇒ the
// constraint curvature genuinely enters W — the healthy exact-Hessian Newton-SQP regime).
// [HS6 (λ* = 0 ⇒ a curvature-free exact W; ℓ1-Armijo then creeps globally at ~×0.995/iter — MEASURED) is
// DEFERRED to v7-n as a globalization stress gate: N&W Alg 18.3 itself specifies DAMPED BFGS, not exact ∇²L,
// and the production filter/watchdog globalization lands there.]
class ShiftedSphere2 final : public opt::Objective<crd::f64>
{
public:
    ShiftedSphere2() noexcept : Objective<crd::f64>(true, false, /*has_hessian=*/true) {}
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

// Quartic chain over the PARALLEL spmv: f = ½xᵀAx + Σ¼x_i⁴ (A = the parallel Laplacian op) — the moat vehicle
// (value/gradient ride the parallel-but-bit-exact spmv; the dense Hessian A + diag(3x²) is analytic tridiag).
class ParQuartic final : public opt::Objective<crd::f64>
{
public:
    ParQuartic(const crd::hesap::LinearOp<crd::f64>& a, crd::usize n, crd::memory::IAllocator* alloc)
        : Objective<crd::f64>(true, false, /*has_hessian=*/true), m_a(&a), m_n(n), m_ax(alloc)
    {
        m_ax.resize(n);
    }
    [[nodiscard]] crd::f64 value(crd::containers::ConstSpan<crd::f64> x) const override
    {
        (void)m_a->apply(x, {m_ax.data(), m_n}); // parallel spmv
        crd::f64 acc = 0.0;
        for (crd::usize i = 0; i < m_n; ++i)
        {
            acc += 0.5 * x[i] * m_ax[i] + 0.25 * x[i] * x[i] * x[i] * x[i];
        }
        return acc;
    }
    [[nodiscard]] crd::usize n() const noexcept override { return m_n; }
    [[nodiscard]] bool gradient(crd::containers::ConstSpan<crd::f64> x,
                                crd::containers::Span<crd::f64> g) const override
    {
        (void)m_a->apply(x, {m_ax.data(), m_n}); // parallel spmv
        for (crd::usize i = 0; i < m_n; ++i)
        {
            g[i] = m_ax[i] + x[i] * x[i] * x[i];
        }
        return true;
    }
    [[nodiscard]] bool hessian(crd::containers::ConstSpan<crd::f64> x, crd::containers::Span<crd::f64> h) const override
    {
        for (crd::usize k = 0; k < m_n * m_n; ++k)
        {
            h[k] = 0.0;
        }
        for (crd::usize i = 0; i < m_n; ++i) // A = tridiag(−1,2,−1) + diag(3x²)
        {
            h[i * m_n + i] = 2.0 + 3.0 * x[i] * x[i];
            if (i + 1 < m_n)
            {
                h[i * m_n + (i + 1)] = -1.0;
                h[(i + 1) * m_n + i] = -1.0;
            }
        }
        return true;
    }

private:
    const crd::hesap::LinearOp<crd::f64>* m_a;
    crd::usize m_n;
    mutable crd::containers::Array<crd::f64> m_ax;
};

class EmptyConstraints final : public opt::Constraints<crd::f64>
{
public:
    explicit EmptyConstraints(crd::usize n) noexcept : Constraints<crd::f64>(/*has_jacobians=*/true), m_n(n) {}
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

TEST_CASE("v7-j KKT residual: analytic equality + inequality KKT points", "[hesap][opt][v7]")
{
    crd::memory::TlsfAllocator alloc(1U << 20);
    const SphereObjective obj(2); // f = x² + y²

    SECTION("equality x + y = 1: solution (1/2, 1/2), lambda = 1")
    {
        const crd::f64 ae[] = {1.0, 1.0};
        const crd::f64 be[] = {1.0};
        const LinearConstraints cons(2, {ae, 2}, {be, 1}, {static_cast<const crd::f64*>(nullptr), 0},
                                     {static_cast<const crd::f64*>(nullptr), 0});
        const crd::f64 xs[] = {0.5, 0.5};
        const crd::f64 ls[] = {1.0};
        const auto r0 = opt::compute_kkt_residual<crd::f64>(obj, cons, {xs, 2}, {ls, 1},
                                                            {static_cast<const crd::f64*>(nullptr), 0}, &alloc);
        CHECK(r0.max() < 1e-14);
        const crd::f64 xb[] = {1.0, 1.0}; // feasible? 1+1−1 = 1 ≠ 0 ⇒ primal = 1; stationarity = |2−1| = 1
        const auto r1 = opt::compute_kkt_residual<crd::f64>(obj, cons, {xb, 2}, {ls, 1},
                                                            {static_cast<const crd::f64*>(nullptr), 0}, &alloc);
        CHECK(std::fabs(r1.primal - 1.0) < 1e-14);
        CHECK(std::fabs(r1.stationarity - 1.0) < 1e-14);
    }
    SECTION("inequality x >= 1: solution (1, 0), mu = 2; dual + complementarity parts")
    {
        const crd::f64 ai[] = {1.0, 0.0}; // c_I = x − 1 ≥ 0
        const crd::f64 bi[] = {1.0};
        const LinearConstraints cons(2, {static_cast<const crd::f64*>(nullptr), 0},
                                     {static_cast<const crd::f64*>(nullptr), 0}, {ai, 2}, {bi, 1});
        const crd::f64 xs[] = {1.0, 0.0};
        const crd::f64 mus[] = {2.0};
        const auto r0 = opt::compute_kkt_residual<crd::f64>(
            obj, cons, {xs, 2}, {static_cast<const crd::f64*>(nullptr), 0}, {mus, 1}, &alloc);
        CHECK(r0.max() < 1e-14);
        const crd::f64 mneg[] = {-1.0}; // dual violation
        const auto r1 = opt::compute_kkt_residual<crd::f64>(
            obj, cons, {xs, 2}, {static_cast<const crd::f64*>(nullptr), 0}, {mneg, 1}, &alloc);
        CHECK(std::fabs(r1.dual - 1.0) < 1e-14);
        const crd::f64 xoff[] = {2.0, 0.0}; // inactive c_I = 1 with mu = 2 ⇒ complementarity = 2
        const auto r2 = opt::compute_kkt_residual<crd::f64>(
            obj, cons, {xoff, 2}, {static_cast<const crd::f64*>(nullptr), 0}, {mus, 1}, &alloc);
        CHECK(std::fabs(r2.complementarity - 2.0) < 1e-14);
        CHECK(r2.primal < 1e-14); // still feasible
    }
}

TEST_CASE("v7-j dense KKT solve: certificate + the inertia test", "[hesap][opt][v7]")
{
    crd::memory::TlsfAllocator alloc(1U << 20);
    const crd::usize n = 3;
    const crd::usize m = 1;
    const crd::f64 je[] = {1.0, 1.0, 1.0};
    const crd::f64 g[] = {1.0, -2.0, 0.5};
    const crd::f64 c[] = {0.7};
    crd::containers::Array<crd::f64> p(&alloc);
    crd::containers::Array<crd::f64> lam(&alloc);
    p.resize(n);
    lam.resize(m);

    auto check_certificate = [&](const crd::f64* w, crd::f64 delta)
    {
        // (W + δI)p + g − J_Eᵀλ⁺ = 0  and  J_E p + c = 0.
        for (crd::usize i = 0; i < n; ++i)
        {
            crd::f64 acc = g[i] - je[i] * lam[0] + delta * p[i];
            for (crd::usize j = 0; j < n; ++j)
            {
                acc += w[i * n + j] * p[j];
            }
            CHECK(std::fabs(acc) < 1e-10);
        }
        crd::f64 feas = c[0];
        for (crd::usize j = 0; j < n; ++j)
        {
            feas += je[j] * p[j];
        }
        CHECK(std::fabs(feas) < 1e-10);
    };

    SECTION("PD W: plain saddle solve, no regularization")
    {
        const crd::f64 w[] = {1.0, 0.0, 0.0, 0.0, 2.0, 0.0, 0.0, 0.0, 5.0};
        const auto info = opt::solve_kkt_dense<crd::f64>(&alloc, {w, n * n}, {je, m * n}, {g, n}, {c, m}, {p.data(), n},
                                                         {lam.data(), m});
        REQUIRE(info.solved);
        CHECK(info.delta == 0.0);
        CHECK_FALSE(info.inertia_corrected);
        check_certificate(w, 0.0);
    }
    SECTION("indefinite W but PD reduced Hessian: the inertia test must NOT over-regularize")
    {
        // J = e1ᵀ... use je2 = (1,0,0): nullspace = span{e2,e3}; W = diag(−1,2,3) ⇒ ZᵀWZ = diag(2,3) PD.
        const crd::f64 je2[] = {1.0, 0.0, 0.0};
        const crd::f64 w[] = {-1.0, 0.0, 0.0, 0.0, 2.0, 0.0, 0.0, 0.0, 3.0};
        const auto info = opt::solve_kkt_dense<crd::f64>(&alloc, {w, n * n}, {je2, m * n}, {g, n}, {c, m},
                                                         {p.data(), n}, {lam.data(), m});
        REQUIRE(info.solved);
        CHECK(info.delta == 0.0); // (n, m, 0) inertia holds without δ — Sylvester via the nullspace
        // certificate with je2:
        for (crd::usize i = 0; i < n; ++i)
        {
            crd::f64 acc = g[i] - je2[i] * lam[0];
            for (crd::usize j = 0; j < n; ++j)
            {
                acc += w[i * n + j] * p[j];
            }
            CHECK(std::fabs(acc) < 1e-10);
        }
        CHECK(std::fabs(c[0] + p[0]) < 1e-12);
    }
    SECTION("indefinite REDUCED Hessian: the delta ladder fires and still certifies")
    {
        const crd::f64 je2[] = {1.0, 0.0, 0.0};
        const crd::f64 w[] = {1.0, 0.0, 0.0, 0.0, -2.0, 0.0, 0.0, 0.0, 3.0}; // ZᵀWZ = diag(−2,3) indefinite
        const auto info = opt::solve_kkt_dense<crd::f64>(&alloc, {w, n * n}, {je2, m * n}, {g, n}, {c, m},
                                                         {p.data(), n}, {lam.data(), m});
        REQUIRE(info.solved);
        CHECK(info.inertia_corrected);
        CHECK(info.delta > 0.0);
        for (crd::usize i = 0; i < n; ++i) // certificate on the REGULARIZED W
        {
            crd::f64 acc = g[i] - je2[i] * lam[0] + info.delta * p[i];
            for (crd::usize j = 0; j < n; ++j)
            {
                acc += w[i * n + j] * p[j];
            }
            CHECK(std::fabs(acc) < 1e-9);
        }
        CHECK(std::fabs(c[0] + p[0]) < 1e-12);
    }
}

TEST_CASE("v7-j multiplier least-squares estimate at an analytic KKT point", "[hesap][opt][v7]")
{
    crd::memory::TlsfAllocator alloc(1U << 20);
    // min x + y s.t. x² + y² = 1 at (−√½, −√½): J_E = (−√2, −√2), ∇f = (1,1) ⇒ λ = −1/√2.
    const crd::f64 a[] = {1.0, 1.0};
    const LinearObjective obj({a, 2});
    const CircleConstraint cons;
    const crd::f64 s = -std::sqrt(0.5);
    const crd::f64 xs[] = {s, s};
    crd::containers::Array<crd::f64> lam(&alloc);
    lam.resize(1);
    opt::estimate_eq_multipliers<crd::f64>(obj, cons, {xs, 2}, {lam.data(), 1}, &alloc);
    CHECK(std::fabs(lam[0] - (-1.0 / std::sqrt(2.0))) < 1e-12);
}

TEST_CASE("v7-j l1 merit: directional derivative matches finite differences", "[hesap][opt][v7]")
{
    // Hand-built pieces at a kink-free point: ce = (+2, −3), ci = (−2 violated, +4 inactive).
    const crd::usize n = 2;
    const crd::f64 g[] = {1.0, -1.0};
    const crd::f64 p[] = {0.3, 0.7};
    const crd::f64 ce[] = {2.0, -3.0};
    const crd::f64 ci[] = {-2.0, 4.0};
    const crd::f64 je[] = {1.0, 2.0, -1.0, 0.5};
    const crd::f64 ji[] = {2.0, -1.0, 0.0, 1.0};
    const crd::f64 nu = 2.5;

    const crd::f64 d =
        opt::l1_merit_directional<crd::f64>({g, n}, {p, n}, {ce, 2}, {ci, 2}, {je, 2 * n}, {ji, 2 * n}, nu);
    // FD on the exact piecewise-linear model along p (constraints are treated to first order — t small keeps
    // every sign fixed, so the model IS the function here).
    const crd::f64 t = 1e-7;
    auto phi_at = [&](crd::f64 tt) -> crd::f64
    {
        crd::f64 fx = 0.0;
        for (crd::usize j = 0; j < n; ++j)
        {
            fx += g[j] * tt * p[j]; // f linearized (exact for the directional-derivative check)
        }
        crd::f64 pen = 0.0;
        for (crd::usize i = 0; i < 2; ++i)
        {
            crd::f64 cv = ce[i];
            for (crd::usize j = 0; j < n; ++j)
            {
                cv += tt * je[i * n + j] * p[j];
            }
            pen += std::fabs(cv);
        }
        for (crd::usize i = 0; i < 2; ++i)
        {
            crd::f64 cv = ci[i];
            for (crd::usize j = 0; j < n; ++j)
            {
                cv += tt * ji[i * n + j] * p[j];
            }
            if (cv < 0.0)
            {
                pen -= cv;
            }
        }
        return fx + nu * pen;
    };
    const crd::f64 fd = (phi_at(t) - phi_at(0.0)) / t;
    CHECK(std::fabs(d - fd) < 1e-6);
}

TEST_CASE("v7-j SQP equality: one-step convergence on an equality-constrained QP", "[hesap][opt][v7]")
{
    crd::memory::TlsfAllocator alloc(1U << 20);
    const crd::usize n = 3;
    const SphereObjective obj(n);                          // f = ‖x‖², H = 2I (a QP)
    const crd::f64 ae[] = {1.0, 1.0, 1.0, 1.0, -1.0, 0.0}; // x+y+z = 3; x−y = 1
    const crd::f64 be[] = {3.0, 1.0};
    const LinearConstraints cons(n, {ae, 2 * n}, {be, 2}, {static_cast<const crd::f64*>(nullptr), 0},
                                 {static_cast<const crd::f64*>(nullptr), 0});
    crd::containers::Array<crd::f64> x0(&alloc);
    x0.resize(n);
    x0[0] = 5.0;
    x0[1] = -3.0;
    x0[2] = 0.5;

    opt::OptOptions<crd::f64> opts;
    opts.grad_tol = 1e-10;
    opts.max_iters = 20;
    auto r = opt::minimize_sqp_equality<crd::f64>(obj, cons, {x0.data(), n}, opts, &alloc);
    REQUIRE(r.status == opt::OptStatus::Success);
    CHECK(r.iterations <= 1); // Newton-KKT on a QP with linear constraints is EXACT — the sharp gate
    CHECK(r.kkt_residual < 1e-10);
    CHECK(std::fabs(r.x[0] + r.x[1] + r.x[2] - 3.0) < 1e-12); // feasibility
    CHECK(std::fabs(r.x[0] - r.x[1] - 1.0) < 1e-12);
    CHECK(r.multipliers.size() == 2);
}

TEST_CASE("v7-j SQP equality: circle problem (nonlinear constraint curvature)", "[hesap][opt][v7]")
{
    crd::memory::TlsfAllocator alloc(1U << 20);
    // min x + y s.t. x² + y² = 1 → (−√½, −√½), λ = −1/√2. f is LINEAR: all curvature comes from the
    // constraint through add_lagrangian_hessian — the capability this test exists to exercise.
    const crd::f64 a[] = {1.0, 1.0};
    const LinearObjective obj({a, 2});
    const CircleConstraint cons;
    crd::containers::Array<crd::f64> x0(&alloc);
    x0.resize(2);
    x0[0] = -0.5;
    x0[1] = -0.8;

    opt::OptOptions<crd::f64> opts;
    opts.grad_tol = 1e-10;
    opts.max_iters = 50;
    auto r = opt::minimize_sqp_equality<crd::f64>(obj, cons, {x0.data(), 2}, opts, &alloc);
    REQUIRE(r.status == opt::OptStatus::Success);
    const crd::f64 s = -std::sqrt(0.5);
    CHECK(std::fabs(r.x[0] - s) < 1e-9);
    CHECK(std::fabs(r.x[1] - s) < 1e-9);
    CHECK(std::fabs(r.multipliers[0] - (-1.0 / std::sqrt(2.0))) < 1e-9);
    CHECK(r.kkt_residual < 1e-10);
}

TEST_CASE("v7-j SQP equality: circle projection (nonzero-multiplier curvature)", "[hesap][opt][v7]")
{
    crd::memory::TlsfAllocator alloc(1U << 20);
    const ShiftedSphere2 obj;    // min (x−2)² + (y−1)²
    const CircleConstraint cons; // s.t. x² + y² = 1
    crd::containers::Array<crd::f64> x0(&alloc);
    x0.resize(2);
    x0[0] = 0.0; // off the circle, away from the solution
    x0[1] = 1.5;

    opt::OptOptions<crd::f64> opts;
    opts.grad_tol = 1e-10;
    opts.max_iters = 100;
    auto r = opt::minimize_sqp_equality<crd::f64>(obj, cons, {x0.data(), 2}, opts, &alloc);
    REQUIRE(r.status == opt::OptStatus::Success);
    const crd::f64 s5 = std::sqrt(5.0);
    CHECK(std::fabs(r.x[0] - 2.0 / s5) < 1e-9);
    CHECK(std::fabs(r.x[1] - 1.0 / s5) < 1e-9);
    CHECK(std::fabs(r.multipliers[0] - (1.0 - s5)) < 1e-9); // λ* = 1 − √5
    CHECK(r.kkt_residual < 1e-10);
    CHECK(r.iterations < 30); // healthy Newton-SQP territory
}

TEST_CASE("v7-j SQP equality determinism moat {1,2,4,8,16}", "[hesap][opt][v7][moat]")
{
    const crd::usize n = 64;
    crd::memory::TlsfAllocator alloc(1U << 25);
    Csr a = laplacian_1d(&alloc, static_cast<crd::u32>(n));

    crd::containers::Array<crd::f64> ae(&alloc); // Σx_i = 8 (one linear equality)
    crd::containers::Array<crd::f64> be(&alloc);
    crd::containers::Array<crd::f64> x0(&alloc);
    ae.resize(n);
    be.resize(1);
    x0.resize(n);
    for (crd::usize i = 0; i < n; ++i)
    {
        ae[i] = 1.0;
        x0[i] = 0.0;
    }
    be[0] = 8.0;
    const LinearConstraints cons(n, {ae.data(), n}, {be.data(), 1}, {static_cast<const crd::f64*>(nullptr), 0},
                                 {static_cast<const crd::f64*>(nullptr), 0});

    opt::OptOptions<crd::f64> opts;
    opts.grad_tol = 1e-9;
    opts.max_iters = 100;

    crd::containers::Array<crd::f64> x_ref(&alloc);
    crd::f64 lam_ref = 0.0;
    bool have_ref = false;
    crd::usize iters_ref = 0;
    for (crd::u32 nw : {1U, 2U, 4U, 8U, 16U})
    {
        crd::jobs::Config cfg;
        cfg.num_threads = nw;
        crd::jobs::init(cfg);
        {
            sp::ParallelSparseLinearOp<crd::f64> op(a, &alloc, /*parallel_min_stored_bytes=*/0);
            const ParQuartic obj(op, n, &alloc); // value/gradient ride the parallel spmv
            auto r = opt::minimize_sqp_equality<crd::f64>(obj, cons, {x0.data(), n}, opts, &alloc);

            REQUIRE(r.status == opt::OptStatus::Success);
            REQUIRE(r.iterations > 1); // the quartic needs several SQP iterations ⇒ moat not vacuous
            if (!have_ref)
            {
                x_ref.resize(r.x.size());
                for (crd::usize i = 0; i < r.x.size(); ++i)
                {
                    x_ref[i] = r.x[i];
                }
                lam_ref = r.multipliers[0];
                iters_ref = r.iterations;
                have_ref = true;
            }
            else
            {
                bool ident = (r.iterations == iters_ref) && (r.multipliers[0] == lam_ref);
                for (crd::usize i = 0; i < r.x.size() && ident; ++i)
                {
                    ident = (r.x[i] == x_ref[i]);
                }
                CHECK(ident); // primal AND dual trajectory bit-identical across worker counts
            }
        }
        crd::jobs::shutdown();
    }
}

TEST_CASE("v7-j SQP equality boundaries: m = 0 (SQP = Newton) and n = 0", "[hesap][opt][v7]")
{
    crd::memory::TlsfAllocator alloc(1U << 20);
    {
        const crd::usize n = 3;
        const SphereObjective obj(n);
        const EmptyConstraints cons(n);
        crd::containers::Array<crd::f64> x0(&alloc);
        x0.resize(n);
        x0[0] = 2.0;
        x0[1] = -1.0;
        x0[2] = 0.7;
        opt::OptOptions<crd::f64> opts;
        opts.grad_tol = 1e-10;
        opts.max_iters = 10;
        auto r = opt::minimize_sqp_equality<crd::f64>(obj, cons, {x0.data(), n}, opts, &alloc);
        REQUIRE(r.status == opt::OptStatus::Success);
        CHECK(r.iterations <= 1); // unconstrained Newton on a quadratic
        for (crd::usize i = 0; i < n; ++i)
        {
            CHECK(std::fabs(r.x[i]) < 1e-12);
        }
        CHECK(r.multipliers.size() == 0);
    }
    {
        const SphereObjective obj(0);
        const EmptyConstraints cons(0);
        opt::OptOptions<crd::f64> opts;
        auto r =
            opt::minimize_sqp_equality<crd::f64>(obj, cons, {static_cast<const crd::f64*>(nullptr), 0}, opts, &alloc);
        REQUIRE(r.status == opt::OptStatus::Success);
        CHECK(r.x.size() == 0);
    }
}
