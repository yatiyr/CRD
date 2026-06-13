// v9-k gates: parameter sensitivities — forward (CVODES augmented system) + adjoint (CVODES ASA). THE gate
// is THREE INDEPENDENT DERIVATIVE ORACLES agreeing: forward-sensitivity S(T), adjoint dg/dp, and
// central-difference FD re-integration. For a functional g = wᵀ·y(T), adjoint dg/dp_j == Σ_i w_i·S[j,i].
// Non-stiff linear-exchange problem (ERK) for the full three-way check; stiff Robertson-with-parameters
// (BDF, the block-diagonal augmented Jacobian) for forward-sensitivity vs FD; determinism.

#include <crd/containers/array.hpp>
#include <crd/hesap/ode/sensitivity.hpp>
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

// Linear two-compartment exchange: y0' = −p0·y0 + p1·y1, y1' = p0·y0 − p1·y1. Non-stiff for moderate p.
class Exchange final : public ode::ParametricOdeFunction<f64>
{
public:
    void rhs(f64, containers::ConstSpan<f64> y, containers::ConstSpan<f64> p, containers::Span<f64> d) const override
    {
        d[0] = -p[0] * y[0] + p[1] * y[1];
        d[1] = p[0] * y[0] - p[1] * y[1];
    }
    void jacobian_y(f64, containers::ConstSpan<f64>, containers::ConstSpan<f64> p, containers::Span<f64> j) const override
    {
        j[0] = -p[0];
        j[1] = p[1];
        j[2] = p[0];
        j[3] = -p[1];
    }
    void dfdp(f64, containers::ConstSpan<f64> y, containers::ConstSpan<f64>, usize jp,
              containers::Span<f64> out) const override
    {
        if (jp == 0)
        {
            out[0] = -y[0];
            out[1] = y[0];
        }
        else
        {
            out[0] = y[1];
            out[1] = -y[1];
        }
    }
    [[nodiscard]] usize dim() const noexcept override { return 2; }
    [[nodiscard]] usize n_params() const noexcept override { return 2; }
};

// Robertson with the rate constants as parameters p = [0.04, 1e4, 3e7].
class RobertsonP final : public ode::ParametricOdeFunction<f64>
{
public:
    void rhs(f64, containers::ConstSpan<f64> y, containers::ConstSpan<f64> p, containers::Span<f64> d) const override
    {
        d[0] = -p[0] * y[0] + p[1] * y[1] * y[2];
        d[1] = p[0] * y[0] - p[1] * y[1] * y[2] - p[2] * y[1] * y[1];
        d[2] = p[2] * y[1] * y[1];
    }
    void jacobian_y(f64, containers::ConstSpan<f64> y, containers::ConstSpan<f64> p, containers::Span<f64> j) const override
    {
        j[0] = -p[0];
        j[1] = p[1] * y[2];
        j[2] = p[1] * y[1];
        j[3] = p[0];
        j[4] = -p[1] * y[2] - 2.0 * p[2] * y[1];
        j[5] = -p[1] * y[1];
        j[6] = 0.0;
        j[7] = 2.0 * p[2] * y[1];
        j[8] = 0.0;
    }
    void dfdp(f64, containers::ConstSpan<f64> y, containers::ConstSpan<f64>, usize jp,
              containers::Span<f64> out) const override
    {
        out[0] = 0.0;
        out[1] = 0.0;
        out[2] = 0.0;
        if (jp == 0)
        {
            out[0] = -y[0];
            out[1] = y[0];
        }
        else if (jp == 1)
        {
            out[0] = y[1] * y[2];
            out[1] = -y[1] * y[2];
        }
        else
        {
            out[1] = -y[1] * y[1];
            out[2] = y[1] * y[1];
        }
    }
    [[nodiscard]] usize dim() const noexcept override { return 3; }
    [[nodiscard]] usize n_params() const noexcept override { return 3; }
};

// FD oracle: ∂y(T)/∂p_j by central difference, re-integrating the state only.
void fd_sensitivity(ode::ParametricOdeFunction<f64>& pfn, f64 t0, f64 t1, containers::ConstSpan<f64> y0,
                    containers::ConstSpan<f64> p, usize jp, f64 delta, bool stiff, containers::Span<f64> dydp,
                    crd::memory::IAllocator* alloc)
{
    const usize n = pfn.dim();
    const usize np = pfn.n_params();
    containers::Array<f64> pp(alloc);
    pp.resize(np);
    auto solve = [&](f64 sign, containers::Array<f64>& yout)
    {
        for (usize k = 0; k < np; ++k)
        {
            pp[k] = p[k];
        }
        pp[jp] += sign * delta;
        ode::detail::ParamFixedFn<f64> f(pfn, containers::ConstSpan<f64>(pp.data(), np));
        yout.resize(n);
        for (usize i = 0; i < n; ++i)
        {
            yout[i] = y0[i];
        }
        ode::OdeOptions<f64> opts;
        opts.rtol = 1e-10;
        opts.atol = 1e-12;
        const ode::OdeResult<f64> r =
            stiff ? ode::integrate_bdf<f64>(f, t0, t1, containers::Span<f64>(yout.data(), n), opts, alloc)
                  : ode::integrate_erk<f64>(f, t0, t1, containers::Span<f64>(yout.data(), n), opts, alloc);
        REQUIRE(r.success);
    };
    containers::Array<f64> yp(alloc);
    containers::Array<f64> ym(alloc);
    solve(+1.0, yp);
    solve(-1.0, ym);
    for (usize i = 0; i < n; ++i)
    {
        dydp[i] = (yp[i] - ym[i]) / (2.0 * delta);
    }
}

} // namespace

TEST_CASE("sensitivity: forward vs adjoint vs FD -- three oracles agree (non-stiff)", "[ode][sensitivity]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    Exchange pfn;
    const f64 p[2] = {0.7, 0.3};
    const f64 y0[2] = {1.0, 0.5};
    const f64 t0 = 0.0;
    const f64 t1 = 2.0;
    const usize n = 2;
    const usize np = 2;

    // Forward sensitivities.
    containers::Array<f64> y(&alloc);
    y.resize(n);
    std::memcpy(y.data(), y0, n * sizeof(f64));
    containers::Array<f64> smat(&alloc);
    smat.resize(n * np);
    for (usize i = 0; i < n * np; ++i)
    {
        smat[i] = 0.0; // dy0/dp = 0
    }
    ode::OdeOptions<f64> opts;
    opts.rtol = 1e-10;
    opts.atol = 1e-12;
    const ode::OdeResult<f64> rf = ode::integrate_forward_sensitivities<f64>(
        pfn, t0, t1, containers::Span<f64>(y.data(), n), containers::Span<f64>(smat.data(), n * np),
        containers::ConstSpan<f64>(p, np), opts, &alloc);
    REQUIRE(rf.success);

    // FD cross-check of the sensitivity block.
    for (usize j = 0; j < np; ++j)
    {
        containers::Array<f64> fd(&alloc);
        fd.resize(n);
        fd_sensitivity(pfn, t0, t1, containers::ConstSpan<f64>(y0, n), containers::ConstSpan<f64>(p, np), j, 1e-6,
                       false, containers::Span<f64>(fd.data(), n), &alloc);
        for (usize i = 0; i < n; ++i)
        {
            INFO("smat[" << j << "," << i << "]=" << smat[j * n + i] << " FD=" << fd[i]);
            CHECK(std::abs(smat[j * n + i] - fd[i]) < 1e-5);
        }
    }

    // Adjoint for g = w'.y(T): dg/dp_j must equal sum_i w_i.smat[j,i].
    const f64 w[2] = {1.0, -2.0};
    containers::Array<f64> grad(&alloc);
    grad.resize(np);
    const ode::OdeResult<f64> ra = ode::integrate_adjoint_sensitivities<f64>(
        pfn, t0, t1, containers::ConstSpan<f64>(y0, n), containers::ConstSpan<f64>(p, np),
        containers::ConstSpan<f64>(w, n), containers::Span<f64>(grad.data(), np), opts, &alloc);
    REQUIRE(ra.success);
    for (usize j = 0; j < np; ++j)
    {
        const f64 from_fwd = w[0] * smat[j * n + 0] + w[1] * smat[j * n + 1];
        INFO("adjoint grad[" << j << "]=" << grad[j] << " from-forward=" << from_fwd);
        CHECK(std::abs(grad[j] - from_fwd) < 1e-6);
    }
}

TEST_CASE("sensitivity: stiff Robertson forward sensitivities vs FD (BDF augmented)", "[ode][sensitivity]")
{
    crd::memory::TlsfAllocator alloc(1U << 24);
    RobertsonP pfn;
    const f64 p[3] = {0.04, 1e4, 3e7};
    const f64 y0[3] = {1.0, 0.0, 0.0};
    const f64 t0 = 0.0;
    const f64 t1 = 10.0;
    const usize n = 3;
    const usize np = 3;

    containers::Array<f64> y(&alloc);
    y.resize(n);
    std::memcpy(y.data(), y0, n * sizeof(f64));
    containers::Array<f64> smat(&alloc);
    smat.resize(n * np);
    for (usize i = 0; i < n * np; ++i)
    {
        smat[i] = 0.0;
    }
    ode::OdeOptions<f64> opts;
    opts.rtol = 1e-9;
    opts.atol = 1e-12;
    const ode::OdeResult<f64> rf = ode::integrate_forward_sensitivities<f64>(
        pfn, t0, t1, containers::Span<f64>(y.data(), n), containers::Span<f64>(smat.data(), n * np),
        containers::ConstSpan<f64>(p, np), opts, &alloc, /*stiff*/ true);
    REQUIRE(rf.success);

    // Relative FD per parameter (rate constants span 0.04 .. 3e7, so the perturbation scales with p).
    for (usize j = 0; j < np; ++j)
    {
        containers::Array<f64> fd(&alloc);
        fd.resize(n);
        const f64 delta = 1e-6 * p[j];
        fd_sensitivity(pfn, t0, t1, containers::ConstSpan<f64>(y0, n), containers::ConstSpan<f64>(p, np), j, delta,
                       true, containers::Span<f64>(fd.data(), n), &alloc);
        for (usize i = 0; i < n; ++i)
        {
            const f64 scale = 1.0 + std::abs(smat[j * n + i]);
            INFO("Robertson smat[" << j << "," << i << "]=" << smat[j * n + i] << " FD=" << fd[i]);
            CHECK(std::abs(smat[j * n + i] - fd[i]) / scale < 1e-4);
        }
    }
}

TEST_CASE("sensitivity: run-twice bit identity (forward + adjoint)", "[ode][sensitivity][determinism]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    Exchange pfn;
    const f64 p[2] = {0.7, 0.3};
    const f64 y0[2] = {1.0, 0.5};
    const f64 w[2] = {1.0, -2.0};

    auto run = [&](containers::Array<f64>& S, containers::Array<f64>& grad)
    {
        containers::Array<f64> y(&alloc);
        y.resize(2);
        std::memcpy(y.data(), y0, 2 * sizeof(f64));
        S.resize(4);
        for (usize i = 0; i < 4; ++i)
        {
            S[i] = 0.0;
        }
        ode::OdeOptions<f64> opts;
        opts.rtol = 1e-9;
        opts.atol = 1e-11;
        (void)ode::integrate_forward_sensitivities<f64>(pfn, 0.0, 2.0, containers::Span<f64>(y.data(), 2),
                                                        containers::Span<f64>(S.data(), 4),
                                                        containers::ConstSpan<f64>(p, 2), opts, &alloc);
        grad.resize(2);
        (void)ode::integrate_adjoint_sensitivities<f64>(pfn, 0.0, 2.0, containers::ConstSpan<f64>(y0, 2),
                                                        containers::ConstSpan<f64>(p, 2),
                                                        containers::ConstSpan<f64>(w, 2),
                                                        containers::Span<f64>(grad.data(), 2), opts, &alloc);
    };
    containers::Array<f64> s1(&alloc);
    containers::Array<f64> g1(&alloc);
    containers::Array<f64> s2(&alloc);
    containers::Array<f64> g2(&alloc);
    run(s1, g1);
    run(s2, g2);
    CHECK(std::memcmp(s1.data(), s2.data(), 4 * sizeof(f64)) == 0);
    CHECK(std::memcmp(g1.data(), g2.data(), 2 * sizeof(f64)) == 0);
}
