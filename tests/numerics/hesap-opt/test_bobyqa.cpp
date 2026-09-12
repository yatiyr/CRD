// v7-p-4 — BOBYQA (the faithful NLopt-reference port, bounds native). Functional gates: (1) the bounded
// quadratic-exactness property (full npt, optimum interior); (2) an ACTIVE-BOUND optimum (the unconstrained
// minimizer outside the box -> pinned exactly); (3) Rosenbrock in a box from the classic start; (4) bit-
// identical determinism; (5) n = 0. The differential harness vs the compiled oracle is the close gate
// (runtime/examples/bobyqa_difftest.cpp).

#include <crd/hesap/opt/bobyqa.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>
#include <cmath>

namespace opt = crd::hesap::opt;

namespace
{

class ShiftQuad final : public opt::Objective<crd::f64>
{
public:
    explicit ShiftQuad(crd::containers::ConstSpan<crd::f64> c) noexcept : Objective<crd::f64>(false, false), m_c(c) {}
    [[nodiscard]] crd::f64 value(crd::containers::ConstSpan<crd::f64> x) const override
    {
        crd::f64 acc = 0.0;
        for (crd::usize i = 0; i < m_c.size(); ++i)
        {
            const crd::f64 d = x[i] - m_c[i];
            acc += (1.0 + static_cast<crd::f64>(i)) * d * d;
        }
        return acc;
    }
    [[nodiscard]] crd::usize n() const noexcept override { return m_c.size(); }

private:
    crd::containers::ConstSpan<crd::f64> m_c;
};

class Rosen2 final : public opt::Objective<crd::f64>
{
public:
    Rosen2() noexcept : Objective<crd::f64>(false, false) {}
    [[nodiscard]] crd::f64 value(crd::containers::ConstSpan<crd::f64> x) const override
    {
        const crd::f64 a = 1.0 - x[0];
        const crd::f64 b = x[1] - x[0] * x[0];
        return a * a + 100.0 * b * b;
    }
    [[nodiscard]] crd::usize n() const noexcept override { return 2; }
};

} // namespace

TEST_CASE("bobyqa: interior quadratic at full npt", "[hesap-opt][bobyqa]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    const crd::f64 c[] = {1.5, -0.5, 2.0};
    const crd::containers::ConstSpan<crd::f64> cs{c, 3};
    const ShiftQuad obj(cs);
    const crd::f64 x0[] = {0.0, 0.0, 0.0};
    const crd::f64 lo[] = {-5.0, -5.0, -5.0};
    const crd::f64 up[] = {5.0, 5.0, 5.0};
    opt::BobyqaOptions<crd::f64> bo;
    bo.rhobeg = 0.5;
    bo.rhoend = 1e-10;
    bo.npt = 10; // (n+1)(n+2)/2 for n = 3
    const opt::OptResult<crd::f64> r = opt::minimize_bobyqa<crd::f64>(obj, {x0, 3}, {lo, 3}, {up, 3}, &alloc, bo);
    REQUIRE(r.status == opt::OptStatus::Success);
    for (crd::usize j = 0; j < 3; ++j)
    {
        CHECK(std::fabs(r.x[j] - c[j]) < 1e-7);
    }
    CHECK(r.fx < 1e-12);
}

TEST_CASE("bobyqa: active-bound optimum pinned exactly", "[hesap-opt][bobyqa]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    // The unconstrained minimizer (1.5, -0.5) lies OUTSIDE the box x0 <= 1: the bound pins x0 = 1.
    const crd::f64 c[] = {1.5, -0.5};
    const crd::containers::ConstSpan<crd::f64> cs{c, 2};
    const ShiftQuad obj(cs);
    const crd::f64 x0[] = {0.0, 0.0};
    const crd::f64 lo[] = {-2.0, -2.0};
    const crd::f64 up[] = {1.0, 2.0};
    opt::BobyqaOptions<crd::f64> bo;
    bo.rhobeg = 0.25;
    bo.rhoend = 1e-10;
    const opt::OptResult<crd::f64> r = opt::minimize_bobyqa<crd::f64>(obj, {x0, 2}, {lo, 2}, {up, 2}, &alloc, bo);
    REQUIRE(r.status == opt::OptStatus::Success);
    CHECK(r.x[0] == 1.0); // landed EXACTLY on the bound (the SL/SU exact-landing machinery)
    CHECK(std::fabs(r.x[1] - (-0.5)) < 1e-7);
}

TEST_CASE("bobyqa: Rosenbrock in a box", "[hesap-opt][bobyqa]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    const Rosen2 obj;
    const crd::f64 x0[] = {-1.2, 1.0};
    const crd::f64 lo[] = {-2.0, -2.0};
    const crd::f64 up[] = {2.0, 2.0};
    opt::BobyqaOptions<crd::f64> bo;
    bo.rhobeg = 0.5;
    bo.rhoend = 1e-10;
    bo.max_evals = 20000;
    const opt::OptResult<crd::f64> r = opt::minimize_bobyqa<crd::f64>(obj, {x0, 2}, {lo, 2}, {up, 2}, &alloc, bo);
    REQUIRE(r.status == opt::OptStatus::Success);
    CHECK(std::fabs(r.x[0] - 1.0) < 1e-6);
    CHECK(std::fabs(r.x[1] - 1.0) < 1e-6);
}

TEST_CASE("bobyqa: bit-identical determinism (run twice)", "[hesap-opt][bobyqa][determinism]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    const Rosen2 obj;
    const crd::f64 x0[] = {-1.2, 1.0};
    const crd::f64 lo[] = {-2.0, -2.0};
    const crd::f64 up[] = {2.0, 2.0};
    opt::BobyqaOptions<crd::f64> bo;
    bo.rhoend = 1e-9;
    const opt::OptResult<crd::f64> r1 = opt::minimize_bobyqa<crd::f64>(obj, {x0, 2}, {lo, 2}, {up, 2}, &alloc, bo);
    const opt::OptResult<crd::f64> r2 = opt::minimize_bobyqa<crd::f64>(obj, {x0, 2}, {lo, 2}, {up, 2}, &alloc, bo);
    REQUIRE(r1.status == r2.status);
    REQUIRE(r1.fn_evals == r2.fn_evals);
    REQUIRE(r1.x[0] == r2.x[0]); // bit-identical
    REQUIRE(r1.x[1] == r2.x[1]);
    REQUIRE(r1.fx == r2.fx);
}

TEST_CASE("bobyqa: n = 0 boundary", "[hesap-opt][bobyqa]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    class ZeroObj final : public opt::Objective<crd::f64>
    {
    public:
        ZeroObj() noexcept : Objective<crd::f64>(false, false) {}
        [[nodiscard]] crd::f64 value(crd::containers::ConstSpan<crd::f64>) const override { return 0.0; }
        [[nodiscard]] crd::usize n() const noexcept override { return 0; }
    };
    const ZeroObj obj;
    const opt::OptResult<crd::f64> r = opt::minimize_bobyqa<crd::f64>(obj, {}, {}, {}, &alloc);
    CHECK(r.status == opt::OptStatus::Success);
}
