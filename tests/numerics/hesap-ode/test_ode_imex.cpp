// v9-i gates: IMEX additive Runge-Kutta (ARK3/4/5, Kennedy-Carpenter, extracted from ARKODE v6.4.1).
// PER-PART order slopes certify each tableau (explicit half, implicit half, AND a genuine split) — the
// d4-sign lesson: a transcription error in either half drops the order. The implicit halves use a LINEAR
// f_I so simplified Newton is exact in one step and never contaminates the order measurement; the explicit
// half carries the nonlinearity. Plus: L-stability of the stiff implicit mode, advection-diffusion MOL
// (split == monolithic + the IMEX step-count advantage), run-twice determinism, FD-vs-analytic J_I.

#include <crd/containers/array.hpp>
#include <crd/hesap/ode/erk.hpp>
#include <crd/hesap/ode/imex.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstring>

using crd::f64;
using crd::usize;
namespace ode = crd::hesap::ode;
namespace containers = crd::containers;

namespace
{

// y' = -2t·y²  (exact y = 1/(1+t²), y(1) = 1/2), split with a LINEAR implicit shift f_I = lam·y so the
// stage Newton is exact in a single iteration. lam = 0 ⇒ purely explicit (f_I ≡ 0). Otherwise a genuine
// IMEX split whose two halves sum to the true (nonlinear, non-autonomous) RHS.
class ImexShift final : public ode::OdeFunction<f64>
{
public:
    explicit ImexShift(f64 lam) : m_lam(lam)
    {
        set_has_imex_split(true);
        set_has_implicit_jacobian(true);
    }
    void rhs(f64 t, containers::ConstSpan<f64> y, containers::Span<f64> d) const override
    {
        d[0] = -2.0 * t * y[0] * y[0];
    }
    [[nodiscard]] bool rhs_explicit(f64 t, containers::ConstSpan<f64> y, containers::Span<f64> d) const override
    {
        d[0] = -2.0 * t * y[0] * y[0] - m_lam * y[0];
        return true;
    }
    [[nodiscard]] bool rhs_implicit(f64, containers::ConstSpan<f64> y, containers::Span<f64> d) const override
    {
        d[0] = m_lam * y[0];
        return true;
    }
    [[nodiscard]] bool jacobian_implicit(f64, containers::ConstSpan<f64>, containers::Span<f64> j) const override
    {
        j[0] = m_lam;
        return true;
    }
    [[nodiscard]] usize dim() const noexcept override { return 1; }

private:
    f64 m_lam;
};

// Pure implicit linear decay y' = lam·y (f_E ≡ 0, exact e^{lam·t}) — certifies the ESDIRK half in isolation.
class LinDecay final : public ode::OdeFunction<f64>
{
public:
    explicit LinDecay(f64 lam) : m_lam(lam)
    {
        set_has_imex_split(true);
        set_has_implicit_jacobian(true);
    }
    void rhs(f64, containers::ConstSpan<f64> y, containers::Span<f64> d) const override { d[0] = m_lam * y[0]; }
    [[nodiscard]] bool rhs_explicit(f64, containers::ConstSpan<f64>, containers::Span<f64> d) const override
    {
        d[0] = 0.0;
        return true;
    }
    [[nodiscard]] bool rhs_implicit(f64, containers::ConstSpan<f64> y, containers::Span<f64> d) const override
    {
        d[0] = m_lam * y[0];
        return true;
    }
    [[nodiscard]] bool jacobian_implicit(f64, containers::ConstSpan<f64>, containers::Span<f64> j) const override
    {
        j[0] = m_lam;
        return true;
    }
    [[nodiscard]] usize dim() const noexcept override { return 1; }

private:
    f64 m_lam;
};

f64 fixed_h_error(ode::ImexMethod m, ode::OdeFunction<f64>& f, f64 y0, f64 t1, f64 exact, f64 h,
                  crd::memory::IAllocator* alloc)
{
    containers::Array<f64> y(alloc);
    y.resize(1);
    y[0] = y0;
    ode::OdeOptions<f64> opts;
    opts.rtol = 1e10; // never reject ⇒ fixed step h
    opts.atol = 1e10;
    opts.h0 = h;
    opts.hmax = h;
    const ode::OdeResult<f64> r = ode::integrate_imex<f64>(f, 0.0, t1, containers::Span<f64>(y.data(), 1), opts, alloc, m);
    REQUIRE(r.success);
    return std::abs(y[0] - exact);
}

f64 slope(ode::ImexMethod m, ode::OdeFunction<f64>& f, f64 y0, f64 t1, f64 exact, f64 h1, f64 h2,
          crd::memory::IAllocator* alloc)
{
    const f64 e1 = fixed_h_error(m, f, y0, t1, exact, h1, alloc);
    const f64 e2 = fixed_h_error(m, f, y0, t1, exact, h2, alloc);
    return std::log2(e1 / e2);
}

struct OrderBand
{
    ode::ImexMethod method;
    int q;
    f64 lo;
    f64 hi;
};

} // namespace

TEST_CASE("imex: per-part empirical order slopes (explicit / implicit / split)", "[ode][imex]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    const f64 exp1 = std::exp(-2.0); // LinDecay(lam=-2) exact at t=1

    const OrderBand bands[3] = {
        {ode::ImexMethod::Ark3, 3, 2.5, 3.7},
        {ode::ImexMethod::Ark4, 4, 3.4, 4.8},
        {ode::ImexMethod::Ark5, 5, 4.2, 5.9},
    };

    for (const OrderBand& bd : bands)
    {
        // EXPLICIT half (lam = 0 ⇒ f_I ≡ 0): the ARK reduces to its explicit tableau.
        ImexShift fe(0.0);
        const f64 p_e = slope(bd.method, fe, 1.0, 1.0, 0.5, 1.0 / 16.0, 1.0 / 32.0, &alloc);
        INFO("method q=" << bd.q << " EXPLICIT p_hat=" << p_e);
        CHECK(p_e > bd.lo);
        CHECK(p_e < bd.hi);

        // IMPLICIT half (linear decay, f_E ≡ 0): the ESDIRK tableau in isolation.
        LinDecay fi(-2.0);
        const f64 p_i = slope(bd.method, fi, 1.0, 1.0, exp1, 1.0 / 16.0, 1.0 / 32.0, &alloc);
        INFO("method q=" << bd.q << " IMPLICIT p_hat=" << p_i);
        CHECK(p_i > bd.lo);
        CHECK(p_i < bd.hi);

        // GENUINE split (lam = -3 ⇒ both halves nonzero, summing to the true RHS).
        ImexShift fs(-3.0);
        const f64 p_s = slope(bd.method, fs, 1.0, 1.0, 0.5, 1.0 / 16.0, 1.0 / 32.0, &alloc);
        INFO("method q=" << bd.q << " SPLIT p_hat=" << p_s);
        CHECK(p_s > bd.lo);
        CHECK(p_s < bd.hi);
    }
}

namespace
{

// Slow explicit mode + stiff implicit mode: y0' = -y0 (f_E), y1' = -1e4·y1 (f_I). L-stability ⇒ y1 → 0.
class SlowStiff final : public ode::OdeFunction<f64>
{
public:
    SlowStiff()
    {
        set_has_imex_split(true);
        set_has_implicit_jacobian(true);
    }
    void rhs(f64, containers::ConstSpan<f64> y, containers::Span<f64> d) const override
    {
        d[0] = -y[0];
        d[1] = -1e4 * y[1];
    }
    [[nodiscard]] bool rhs_explicit(f64, containers::ConstSpan<f64> y, containers::Span<f64> d) const override
    {
        d[0] = -y[0];
        d[1] = 0.0;
        return true;
    }
    [[nodiscard]] bool rhs_implicit(f64, containers::ConstSpan<f64> y, containers::Span<f64> d) const override
    {
        d[0] = 0.0;
        d[1] = -1e4 * y[1];
        return true;
    }
    [[nodiscard]] bool jacobian_implicit(f64, containers::ConstSpan<f64>, containers::Span<f64> j) const override
    {
        j[0] = 0.0;
        j[1] = 0.0;
        j[2] = 0.0;
        j[3] = -1e4;
        return true;
    }
    [[nodiscard]] usize dim() const noexcept override { return 2; }
};

// 1D periodic advection-diffusion MOL: u_t = -c·u_x (explicit, central diff) + ν·u_xx (implicit). The
// diffusion eigenvalues ~ -4ν/dx² make the system stiff; IMEX treats only diffusion implicitly. A constant
// implicit Jacobian (the diffusion stencil). `analytic` toggles the analytic-J_I capability (else FD).
class AdvDiff final : public ode::OdeFunction<f64>
{
public:
    AdvDiff(usize nx, f64 c, f64 nu, bool analytic) : m_n(nx), m_c(c), m_nu(nu), m_analytic(analytic)
    {
        m_dx = (2.0 * 3.141592653589793) / static_cast<f64>(nx);
        set_has_imex_split(true);
        set_has_implicit_jacobian(analytic);
    }
    void rhs(f64, containers::ConstSpan<f64> y, containers::Span<f64> d) const override
    {
        for (usize i = 0; i < m_n; ++i)
        {
            d[i] = adv(y, i) + dif(y, i);
        }
    }
    [[nodiscard]] bool rhs_explicit(f64, containers::ConstSpan<f64> y, containers::Span<f64> d) const override
    {
        for (usize i = 0; i < m_n; ++i)
        {
            d[i] = adv(y, i);
        }
        return true;
    }
    [[nodiscard]] bool rhs_implicit(f64, containers::ConstSpan<f64> y, containers::Span<f64> d) const override
    {
        for (usize i = 0; i < m_n; ++i)
        {
            d[i] = dif(y, i);
        }
        return true;
    }
    [[nodiscard]] bool jacobian_implicit(f64, containers::ConstSpan<f64>, containers::Span<f64> j) const override
    {
        if (!m_analytic)
        {
            return false;
        }
        for (usize i = 0; i < m_n * m_n; ++i)
        {
            j[i] = 0.0;
        }
        const f64 coef = m_nu / (m_dx * m_dx);
        for (usize i = 0; i < m_n; ++i)
        {
            j[i * m_n + i] = -2.0 * coef;
            j[i * m_n + ((i + 1) % m_n)] += coef;
            j[i * m_n + ((i + m_n - 1) % m_n)] += coef;
        }
        return true;
    }
    [[nodiscard]] usize dim() const noexcept override { return m_n; }

private:
    [[nodiscard]] f64 adv(containers::ConstSpan<f64> y, usize i) const
    {
        const f64 ip = y[(i + 1) % m_n];
        const f64 im = y[(i + m_n - 1) % m_n];
        return -m_c * (ip - im) / (2.0 * m_dx);
    }
    [[nodiscard]] f64 dif(containers::ConstSpan<f64> y, usize i) const
    {
        const f64 ip = y[(i + 1) % m_n];
        const f64 im = y[(i + m_n - 1) % m_n];
        return m_nu * (ip - 2.0 * y[i] + im) / (m_dx * m_dx);
    }
    usize m_n;
    f64 m_c;
    f64 m_nu;
    f64 m_dx;
    bool m_analytic;
};

} // namespace

TEST_CASE("imex: L-stable stiff implicit mode decays cleanly", "[ode][imex]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    SlowStiff f;
    for (int mi = 0; mi < 3; ++mi)
    {
        containers::Array<f64> y(&alloc);
        y.resize(2);
        y[0] = 1.0;
        y[1] = 1.0;
        ode::OdeOptions<f64> opts;
        opts.rtol = 1e-8;
        opts.atol = 1e-11;
        const ode::OdeResult<f64> r = ode::integrate_imex<f64>(
            f, 0.0, 2.0, containers::Span<f64>(y.data(), 2), opts, &alloc, static_cast<ode::ImexMethod>(mi));
        REQUIRE(r.success);
        INFO("method " << mi);
        CHECK(std::abs(y[0] - std::exp(-2.0)) < 1e-6);
        CHECK(std::abs(y[1]) < 1e-10); // stiff mode killed (L-stability)
    }
}

TEST_CASE("imex: advection-diffusion MOL == monolithic + the IMEX step advantage", "[ode][imex][crush]")
{
    crd::memory::TlsfAllocator alloc(1U << 24);
    const usize nx = 64;
    AdvDiff f(nx, 1.0, 1.0, /*analytic*/ true);

    containers::Array<f64> u0(&alloc);
    u0.resize(nx);
    const f64 dx = (2.0 * 3.141592653589793) / static_cast<f64>(nx);
    for (usize i = 0; i < nx; ++i)
    {
        u0[i] = std::sin(static_cast<f64>(i) * dx);
    }

    // IMEX ARK4 (diffusion implicit ⇒ not stiffness-limited).
    containers::Array<f64> ui(&alloc);
    ui.resize(nx);
    std::memcpy(ui.data(), u0.data(), nx * sizeof(f64));
    ode::OdeOptions<f64> oi;
    oi.rtol = 1e-7;
    oi.atol = 1e-9;
    const ode::OdeResult<f64> ri =
        ode::integrate_imex<f64>(f, 0.0, 1.0, containers::Span<f64>(ui.data(), nx), oi, &alloc, ode::ImexMethod::Ark4);
    REQUIRE(ri.success);

    // Monolithic RK45 on the SAME combined RHS (the explicit reference), tight tol.
    containers::Array<f64> ue(&alloc);
    ue.resize(nx);
    std::memcpy(ue.data(), u0.data(), nx * sizeof(f64));
    ode::OdeOptions<f64> oe;
    oe.rtol = 1e-9;
    oe.atol = 1e-11;
    const ode::OdeResult<f64> re = ode::integrate_erk<f64>(f, 0.0, 1.0, containers::Span<f64>(ue.data(), nx), oe, &alloc,
                                                           ode::ErkMethod::Rk45);
    REQUIRE(re.success);

    f64 maxdiff = 0.0;
    for (usize i = 0; i < nx; ++i)
    {
        maxdiff = std::max(maxdiff, std::abs(ui[i] - ue[i]));
    }
    INFO("maxdiff=" << maxdiff << "  ARK4 naccept=" << ri.work.naccept << "  RK45 naccept=" << re.work.naccept);
    CHECK(maxdiff < 1e-5);
    // The diffusion stiffness throttles explicit RK; IMEX is not stiffness-limited ⇒ far fewer steps.
    CHECK(ri.work.naccept * 3 < re.work.naccept);
}

TEST_CASE("imex: run-twice bit identity + FD-vs-analytic J_I", "[ode][imex][determinism]")
{
    crd::memory::TlsfAllocator alloc(1U << 24);
    const usize nx = 48;

    auto run = [&](bool analytic, containers::Array<f64>& out) -> ode::OdeResult<f64>
    {
        AdvDiff f(nx, 0.7, 0.5, analytic);
        out.resize(nx);
        const f64 dx = (2.0 * 3.141592653589793) / static_cast<f64>(nx);
        for (usize i = 0; i < nx; ++i)
        {
            out[i] = std::sin(static_cast<f64>(i) * dx);
        }
        ode::OdeOptions<f64> opts;
        opts.rtol = 1e-7;
        opts.atol = 1e-9;
        return ode::integrate_imex<f64>(f, 0.0, 1.0, containers::Span<f64>(out.data(), nx), opts, &alloc,
                                        ode::ImexMethod::Ark4);
    };

    containers::Array<f64> a(&alloc);
    containers::Array<f64> b(&alloc);
    const ode::OdeResult<f64> r1 = run(true, a);
    const ode::OdeResult<f64> r2 = run(true, b);
    REQUIRE(r1.success);
    CHECK(std::memcmp(a.data(), b.data(), nx * sizeof(f64)) == 0);
    CHECK(r1.work.nfev == r2.work.nfev);
    CHECK(r1.work.nlu == r2.work.nlu);

    containers::Array<f64> c(&alloc);
    const ode::OdeResult<f64> r3 = run(false, c); // FD implicit Jacobian
    REQUIRE(r3.success);
    f64 maxdiff = 0.0;
    for (usize i = 0; i < nx; ++i)
    {
        maxdiff = std::max(maxdiff, std::abs(a[i] - c[i]));
    }
    INFO("FD-vs-analytic maxdiff=" << maxdiff);
    CHECK(maxdiff < 1e-5);
}
