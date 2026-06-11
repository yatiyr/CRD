// v7-q — the GLOBAL/metaheuristic family over the Philox stream. Gates: (1) CMA-ES (Hansen-faithful) on the
// sphere (sigma-collapse convergence to tight accuracy) and on Rosenbrock-5 (the rugged-valley global gate
// pycma is famous for); (2) differential evolution finds the Rastrigin-4 GLOBAL minimum (the multimodal
// torture test, deterministic given the seed); (3) PSO on the sphere; (4) simulated annealing + basin-hopping
// escape the WRONG WELL of a double-well (gradient-blind global moves); (5) multi-start as the baseline on
// Rastrigin-2; (6) bit-identical run-twice determinism for the Philox-driven members; (7) n = 0 edges.
// Plus the NormalSampler moment check lives in tests/hesap-stats.

#include <crd/hesap/opt/cmaes.hpp>
#include <crd/hesap/opt/global_search.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>
#include <cmath>

namespace opt = crd::hesap::opt;

namespace
{

class SphereObj final : public opt::Objective<crd::f64>
{
public:
    explicit SphereObj(crd::usize n) noexcept : Objective<crd::f64>(false, false), m_n(n) {}
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

private:
    crd::usize m_n;
};

class RosenN final : public opt::Objective<crd::f64>
{
public:
    explicit RosenN(crd::usize n) noexcept : Objective<crd::f64>(false, false), m_n(n) {}
    [[nodiscard]] crd::f64 value(crd::containers::ConstSpan<crd::f64> x) const override
    {
        crd::f64 acc = 0.0;
        for (crd::usize i = 0; i + 1 < m_n; ++i)
        {
            const crd::f64 a = 1.0 - x[i];
            const crd::f64 b = x[i + 1] - x[i] * x[i];
            acc += a * a + 100.0 * b * b;
        }
        return acc;
    }
    [[nodiscard]] crd::usize n() const noexcept override { return m_n; }

private:
    crd::usize m_n;
};

// Rastrigin: f = 10n + sum(x^2 - 10 cos(2 pi x)); global minimum 0 at the origin, ~10^n local minima.
class RastriginObj final : public opt::Objective<crd::f64>
{
public:
    explicit RastriginObj(crd::usize n) noexcept : Objective<crd::f64>(false, false), m_n(n) {}
    [[nodiscard]] crd::f64 value(crd::containers::ConstSpan<crd::f64> x) const override
    {
        crd::f64 acc = 10.0 * static_cast<crd::f64>(m_n);
        for (crd::usize i = 0; i < m_n; ++i)
        {
            acc += x[i] * x[i] - 10.0 * std::cos(6.283185307179586 * x[i]);
        }
        return acc;
    }
    [[nodiscard]] crd::usize n() const noexcept override { return m_n; }

private:
    crd::usize m_n;
};

// Asymmetric double well: the LOCAL minimum near x = (-1, ...) is shallower than the GLOBAL near x = (1.5, ...).
class DoubleWell final : public opt::Objective<crd::f64>
{
public:
    DoubleWell() noexcept : Objective<crd::f64>(false, false) {}
    [[nodiscard]] crd::f64 value(crd::containers::ConstSpan<crd::f64> x) const override
    {
        crd::f64 acc = 0.0;
        for (crd::usize i = 0; i < 2; ++i)
        {
            const crd::f64 t = x[i];
            acc += 0.5 * (t + 1.0) * (t + 1.0) * (t - 1.5) * (t - 1.5) - 0.8 * t; // tilted: right well wins
        }
        return acc;
    }
    [[nodiscard]] crd::usize n() const noexcept override { return 2; }
};

} // namespace

TEST_CASE("cmaes: sphere collapses to the optimum", "[hesap-opt][global][cmaes]")
{
    crd::memory::TlsfAllocator alloc(1U << 23);
    const SphereObj obj(8);
    crd::f64 x0[8];
    for (crd::usize i = 0; i < 8; ++i)
    {
        x0[i] = 2.0;
    }
    opt::CmaesOptions<crd::f64> co;
    co.sigma0 = 1.0;
    const opt::OptResult<crd::f64> r = opt::minimize_cmaes<crd::f64>(obj, {x0, 8}, &alloc, co);
    REQUIRE(r.status == opt::OptStatus::Success);
    CHECK(r.fx < 1e-10);
}

TEST_CASE("cmaes: Rosenbrock-5 from a bad start", "[hesap-opt][global][cmaes]")
{
    crd::memory::TlsfAllocator alloc(1U << 23);
    const RosenN obj(5);
    crd::f64 x0[5];
    for (crd::usize i = 0; i < 5; ++i)
    {
        x0[i] = -2.0;
    }
    opt::CmaesOptions<crd::f64> co;
    co.sigma0 = 0.5;
    co.max_evals = 200000;
    const opt::OptResult<crd::f64> r = opt::minimize_cmaes<crd::f64>(obj, {x0, 5}, &alloc, co);
    REQUIRE(r.status == opt::OptStatus::Success);
    for (crd::usize i = 0; i < 5; ++i)
    {
        CHECK(std::fabs(r.x[i] - 1.0) < 1e-4);
    }
}

TEST_CASE("differential evolution: the Rastrigin-4 global minimum", "[hesap-opt][global][de]")
{
    crd::memory::TlsfAllocator alloc(1U << 23);
    const RastriginObj obj(4);
    const crd::f64 lo[] = {-5.12, -5.12, -5.12, -5.12};
    const crd::f64 up[] = {5.12, 5.12, 5.12, 5.12};
    opt::DeOptions<crd::f64> de;
    de.tol = 1e-8;
    de.max_gens = 2000;
    const opt::OptResult<crd::f64> r =
        opt::minimize_differential_evolution<crd::f64>(obj, {lo, 4}, {up, 4}, &alloc, de);
    REQUIRE(r.status == opt::OptStatus::Success);
    CHECK(r.fx < 1e-4); // the global basin, through ~10^4 local minima
    for (crd::usize i = 0; i < 4; ++i)
    {
        CHECK(std::fabs(r.x[i]) < 1e-2);
    }
}

TEST_CASE("pso: sphere", "[hesap-opt][global][pso]")
{
    crd::memory::TlsfAllocator alloc(1U << 23);
    const SphereObj obj(4);
    const crd::f64 lo[] = {-5.0, -5.0, -5.0, -5.0};
    const crd::f64 up[] = {5.0, 5.0, 5.0, 5.0};
    const opt::OptResult<crd::f64> r = opt::minimize_pso<crd::f64>(obj, {lo, 4}, {up, 4}, &alloc);
    CHECK(r.fx < 1e-6);
}

TEST_CASE("simulated annealing + basin hopping escape the wrong well", "[hesap-opt][global]")
{
    crd::memory::TlsfAllocator alloc(1U << 23);
    const DoubleWell obj;
    const crd::f64 x0[] = {-1.0, -1.0}; // the SHALLOW well
    const crd::f64 lo[] = {-3.0, -3.0};
    const crd::f64 up[] = {3.0, 3.0};

    const opt::OptResult<crd::f64> rs =
        opt::minimize_simulated_annealing<crd::f64>(obj, {x0, 2}, {lo, 2}, {up, 2}, &alloc);
    CHECK(rs.x[0] > 1.0); // crossed to the global well
    CHECK(rs.x[1] > 1.0);

    opt::BasinHoppingOptions<crd::f64> bh;
    bh.step = 2.0; // the wells sit ~2.5 apart — the stepsize knob is how scipy users bridge such barriers
    const opt::OptResult<crd::f64> rb = opt::minimize_basin_hopping<crd::f64>(obj, {x0, 2}, &alloc, bh);
    CHECK(rb.x[0] > 1.0);
    CHECK(rb.x[1] > 1.0);
}

TEST_CASE("basin hopping LbfgsFd local mode: scipy's default minimizer, fewer evals, deterministic",
          "[hesap-opt][global][determinism]")
{
    crd::memory::TlsfAllocator alloc(1U << 23);
    const DoubleWell obj;
    const crd::f64 x0[] = {-1.0, -1.0}; // the SHALLOW well
    opt::BasinHoppingOptions<crd::f64> bh;
    bh.step = 2.0;
    bh.local_minimizer = opt::BasinHoppingLocal::LbfgsFd;
    const opt::OptResult<crd::f64> r1 = opt::minimize_basin_hopping<crd::f64>(obj, {x0, 2}, &alloc, bh);
    CHECK(r1.x[0] > 1.0); // crossed to the global well
    CHECK(r1.x[1] > 1.0);
    CHECK(r1.fn_evals > 0); // the counter includes every FD probe (scipy nfev semantics)

    // Same seed ⇒ bit-identical trajectory; and the gradient path should land well under the NM eval bill.
    const opt::OptResult<crd::f64> r2 = opt::minimize_basin_hopping<crd::f64>(obj, {x0, 2}, &alloc, bh);
    REQUIRE(r1.fn_evals == r2.fn_evals);
    REQUIRE(r1.fx == r2.fx);
    REQUIRE(r1.x[0] == r2.x[0]);
    REQUIRE(r1.x[1] == r2.x[1]);

    opt::BasinHoppingOptions<crd::f64> bh_nm;
    bh_nm.step = 2.0;
    const opt::OptResult<crd::f64> rn = opt::minimize_basin_hopping<crd::f64>(obj, {x0, 2}, &alloc, bh_nm);
    CHECK(r1.fn_evals < rn.fn_evals);
}

TEST_CASE("multi-start: Rastrigin-2 baseline", "[hesap-opt][global]")
{
    crd::memory::TlsfAllocator alloc(1U << 23);
    const RastriginObj obj(2);
    const crd::f64 lo[] = {-5.12, -5.12};
    const crd::f64 up[] = {5.12, 5.12};
    opt::MultiStartOptions<crd::f64> ms;
    ms.starts = 60;
    const opt::OptResult<crd::f64> r = opt::minimize_multi_start<crd::f64>(obj, {lo, 2}, {up, 2}, &alloc, ms);
    CHECK(r.fx < 1e-6); // enough draws over [-5.12, 5.12]^2 land in the central basin
}

TEST_CASE("global: bit-identical determinism (run twice)", "[hesap-opt][global][determinism]")
{
    crd::memory::TlsfAllocator alloc(1U << 23);
    const RosenN obj(3);
    const crd::f64 x0[] = {-1.0, 2.0, 0.5};
    opt::CmaesOptions<crd::f64> co;
    co.max_evals = 5000;
    const opt::OptResult<crd::f64> c1 = opt::minimize_cmaes<crd::f64>(obj, {x0, 3}, &alloc, co);
    const opt::OptResult<crd::f64> c2 = opt::minimize_cmaes<crd::f64>(obj, {x0, 3}, &alloc, co);
    REQUIRE(c1.fn_evals == c2.fn_evals);
    REQUIRE(c1.fx == c2.fx); // bit-identical through the Philox normal stream + eig_sym
    for (crd::usize i = 0; i < 3; ++i)
    {
        REQUIRE(c1.x[i] == c2.x[i]);
    }

    const RastriginObj robj(2);
    const crd::f64 lo[] = {-5.12, -5.12};
    const crd::f64 up[] = {5.12, 5.12};
    opt::DeOptions<crd::f64> de;
    de.max_gens = 100;
    const opt::OptResult<crd::f64> d1 =
        opt::minimize_differential_evolution<crd::f64>(robj, {lo, 2}, {up, 2}, &alloc, de);
    const opt::OptResult<crd::f64> d2 =
        opt::minimize_differential_evolution<crd::f64>(robj, {lo, 2}, {up, 2}, &alloc, de);
    REQUIRE(d1.fn_evals == d2.fn_evals);
    REQUIRE(d1.fx == d2.fx);
    REQUIRE(d1.x[0] == d2.x[0]);
    REQUIRE(d1.x[1] == d2.x[1]);
}

TEST_CASE("global: n = 0 edges", "[hesap-opt][global]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    const SphereObj obj(0);
    const opt::OptResult<crd::f64> r1 = opt::minimize_cmaes<crd::f64>(obj, {}, &alloc);
    CHECK(r1.status == opt::OptStatus::Success);
    const opt::OptResult<crd::f64> r2 = opt::minimize_differential_evolution<crd::f64>(obj, {}, {}, &alloc);
    CHECK(r2.status == opt::OptStatus::Success);
}
