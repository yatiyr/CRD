// v9-l gates: higher-index DAE. (1) STRUCTURAL index analysis (Pryce Σ-method, the Pantelides equivalent):
// the Cartesian pendulum is index 3, a semi-explicit DAE index 1, the integrator chain index 2, a pure ODE
// index 0 — the canonical hand-checkable indices. (2) INDEX REDUCTION: the index-3 constrained pendulum,
// reduced to index 1 via the acceleration constraint + λ-elimination, integrates as a plain ODE with the
// position constraint AND total energy conserved (where the raw index-3 form is not directly integrable).

#include <crd/containers/array.hpp>
#include <crd/hesap/ode/dae.hpp>
#include <crd/hesap/ode/dae_structural.hpp>
#include <crd/hesap/ode/erk.hpp>
#include <crd/hesap/ode/solution.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstring>

using crd::f64;
using crd::i32;
using crd::usize;
namespace ode = crd::hesap::ode;
namespace containers = crd::containers;

TEST_CASE("dae structural: Pryce Sigma-method indices (pendulum 3, index-2, index-1, ODE 0)", "[ode][dae]")
{
    crd::memory::TlsfAllocator alloc(1U << 20);

    // Cartesian pendulum (2nd-order form): eq0 x''+λx=0, eq1 y''+λy+g=0, eq2 x²+y²−L²=0. Vars [x,y,λ].
    {
        ode::StructuralDae dae(&alloc);
        dae.resize(3);
        dae.set(0, 0, 2); // x'' in eq0
        dae.set(0, 2, 0); // λ in eq0
        dae.set(1, 1, 2); // y'' in eq1
        dae.set(1, 2, 0); // λ in eq1
        dae.set(2, 0, 0); // x in eq2
        dae.set(2, 1, 0); // y in eq2
        const ode::StructuralResult r = ode::structural_index(dae, &alloc);
        REQUIRE(r.ok);
        INFO("pendulum index=" << r.index << " c=[" << r.c[0] << "," << r.c[1] << "," << r.c[2] << "]");
        CHECK(r.index == 3);
    }
    // Semi-explicit index-1: eq0 x'−f(x,z)=0, eq1 g(x,z)=0. Vars [x,z].
    {
        ode::StructuralDae dae(&alloc);
        dae.resize(2);
        dae.set(0, 0, 1);
        dae.set(0, 1, 0);
        dae.set(1, 0, 0);
        dae.set(1, 1, 0);
        const ode::StructuralResult r = ode::structural_index(dae, &alloc);
        REQUIRE(r.ok);
        CHECK(r.index == 1);
    }
    // Index-2 chain: eq0 x'−z=0, eq1 x−t=0 (z absent in eq1). Vars [x,z].
    {
        ode::StructuralDae dae(&alloc);
        dae.resize(2);
        dae.set(0, 0, 1);
        dae.set(0, 1, 0);
        dae.set(1, 0, 0);
        const ode::StructuralResult r = ode::structural_index(dae, &alloc);
        REQUIRE(r.ok);
        CHECK(r.index == 2);
    }
    // Pure ODE: eq0 x'−f(x)=0. Var [x].
    {
        ode::StructuralDae dae(&alloc);
        dae.resize(1);
        dae.set(0, 0, 1);
        const ode::StructuralResult r = ode::structural_index(dae, &alloc);
        REQUIRE(r.ok);
        CHECK(r.index == 0);
    }
    // Structurally singular (no finite transversal): one variable appears in no equation.
    {
        ode::StructuralDae dae(&alloc);
        dae.resize(2);
        dae.set(0, 0, 1); // only var 0 used; var 1 absent everywhere
        dae.set(1, 0, 0);
        const ode::StructuralResult r = ode::structural_index(dae, &alloc);
        CHECK(!r.ok);
    }
}

namespace
{

// Cartesian pendulum: M=I, f=[0,−g], constraint c=x²+y²−L², G=[2x,2y], γ=−Ġ·v=−2(vx²+vy²).
class Pendulum final : public ode::ConstrainedMechanicalSystem<f64>
{
public:
    Pendulum(f64 g, f64 L) : m_g(g), m_l(L) {}
    [[nodiscard]] usize n_coords() const noexcept override { return 2; }
    [[nodiscard]] usize n_constraints() const noexcept override { return 1; }
    void mass(f64, containers::ConstSpan<f64>, containers::Span<f64> m) const override
    {
        m[0] = 1.0;
        m[1] = 0.0;
        m[2] = 0.0;
        m[3] = 1.0;
    }
    void force(f64, containers::ConstSpan<f64>, containers::ConstSpan<f64>, containers::Span<f64> f) const override
    {
        f[0] = 0.0;
        f[1] = -m_g;
    }
    void constraint_jacobian(f64, containers::ConstSpan<f64> q, containers::Span<f64> g) const override
    {
        g[0] = 2.0 * q[0];
        g[1] = 2.0 * q[1];
    }
    void gamma(f64, containers::ConstSpan<f64>, containers::ConstSpan<f64> v, containers::Span<f64> gam) const override
    {
        gam[0] = -2.0 * (v[0] * v[0] + v[1] * v[1]);
    }
    void constraint(f64, containers::ConstSpan<f64> q, containers::Span<f64> c) const override
    {
        c[0] = q[0] * q[0] + q[1] * q[1] - m_l * m_l;
    }

private:
    f64 m_g;
    f64 m_l;
};

} // namespace

TEST_CASE("dae reduction: index-3 pendulum -> index-1 ODE keeps constraint + energy", "[ode][dae]")
{
    crd::memory::TlsfAllocator alloc(1U << 24);
    const f64 g = 9.81;
    const f64 rod_len = 1.0;
    Pendulum sys(g, rod_len);
    ode::IndexReducedMechanicalOde<f64> reduced(&alloc, sys);

    // Consistent ICs: horizontal release from rest. c = rod_len^2 - rod_len^2 = 0; G.v = 0.
    containers::Array<f64> yv(&alloc);
    yv.resize(4);
    yv[0] = rod_len; // x
    yv[1] = 0.0; // y
    yv[2] = 0.0; // vx
    yv[3] = 0.0; // vy

    ode::OdeOptions<f64> opts;
    opts.rtol = 1e-10;
    opts.atol = 1e-12;
    ode::OdeSolution<f64> sol(&alloc);
    const ode::OdeResult<f64> r = ode::integrate_erk<f64>(reduced, 0.0, 3.0, containers::Span<f64>(yv.data(), 4), opts,
                                                          &alloc, ode::ErkMethod::Rk45, &sol);
    REQUIRE(r.success);

    f64 max_c = 0.0;
    f64 max_de = 0.0;
    for (usize k = 0; k < sol.num_nodes(); ++k)
    {
        const containers::ConstSpan<f64> y = sol.y_node(k);
        const f64 c = y[0] * y[0] + y[1] * y[1] - rod_len * rod_len; // position constraint
        const f64 e = 0.5 * (y[2] * y[2] + y[3] * y[3]) + g * y[1];  // total energy (E(0) = 0)
        max_c = std::max(max_c, std::abs(c));
        max_de = std::max(max_de, std::abs(e));
    }
    INFO("nodes=" << sol.num_nodes() << " max|constraint|=" << max_c << " max|energy|=" << max_de);
    CHECK(max_c < 1e-6);  // the index-1 reduction holds the position constraint
    CHECK(max_de < 1e-5); // energy conserved (E == 0 from horizontal release)

    // The pendulum actually swung (not a degenerate static solution).
    CHECK(yv[1] < -0.1);
}

TEST_CASE("dae reduction: run-twice bit identity", "[ode][dae][determinism]")
{
    crd::memory::TlsfAllocator alloc(1U << 24);
    Pendulum sys(9.81, 1.0);

    auto run = [&](containers::Array<f64>& out)
    {
        ode::IndexReducedMechanicalOde<f64> reduced(&alloc, sys);
        out.resize(4);
        out[0] = 1.0;
        out[1] = 0.0;
        out[2] = 0.0;
        out[3] = 0.0;
        ode::OdeOptions<f64> opts;
        opts.rtol = 1e-9;
        opts.atol = 1e-11;
        (void)ode::integrate_erk<f64>(reduced, 0.0, 2.0, containers::Span<f64>(out.data(), 4), opts, &alloc,
                                      ode::ErkMethod::Rk45);
    };
    containers::Array<f64> a(&alloc);
    containers::Array<f64> b(&alloc);
    run(a);
    run(b);
    CHECK(std::memcmp(a.data(), b.data(), 4 * sizeof(f64)) == 0);
}
