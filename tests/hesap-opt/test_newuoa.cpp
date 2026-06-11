// v7-p-3 — NEWUOA (the faithful NLopt-reference port, classic unconstrained scope). Functional gates:
// (1) the QUADRATIC-EXACTNESS property — with npt = (n+1)(n+2)/2 the model interpolates a quadratic exactly,
// so a shifted quadratic is solved essentially to rhoend in few evaluations; (2) Rosenbrock-2 from the
// classic start (the curved-valley gate where quadratic models shine vs COBYLA's linear ones);
// (3) a 4-D sphere at the default npt = 2n+1; (4) bit-identical determinism; (5) n = 0.
// The differential harness vs the compiled oracle is the close gate (runtime/examples/newuoa_difftest.cpp).

#include <crd/hesap/opt/newuoa.hpp>
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
            acc += (1.0 + static_cast<crd::f64>(i)) * d * d; // distinct curvatures
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

TEST_CASE("newuoa: quadratic exactness at full npt", "[hesap-opt][newuoa]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    const crd::f64 c[] = {1.5, -0.5, 2.0};
    const crd::containers::ConstSpan<crd::f64> cs{c, 3};
    const ShiftQuad obj(cs);
    const crd::f64 x0[] = {0.0, 0.0, 0.0};
    opt::NewuoaOptions<crd::f64> no;
    no.rhobeg = 0.5;
    no.rhoend = 1e-10;
    no.npt = 10; // (n+1)(n+2)/2 for n = 3: the model is EXACT on a quadratic
    const opt::OptResult<crd::f64> r = opt::minimize_newuoa<crd::f64>(obj, {x0, 3}, &alloc, no);
    REQUIRE(r.status == opt::OptStatus::Success);
    for (crd::usize j = 0; j < 3; ++j)
    {
        CHECK(std::fabs(r.x[j] - c[j]) < 1e-7);
    }
    CHECK(r.fx < 1e-13);
}

TEST_CASE("newuoa: Rosenbrock-2 from the classic start", "[hesap-opt][newuoa]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    const Rosen2 obj;
    const crd::f64 x0[] = {-1.2, 1.0};
    opt::NewuoaOptions<crd::f64> no;
    no.rhobeg = 0.5;
    no.rhoend = 1e-10;
    no.max_evals = 20000;
    const opt::OptResult<crd::f64> r = opt::minimize_newuoa<crd::f64>(obj, {x0, 2}, &alloc, no);
    REQUIRE(r.status == opt::OptStatus::Success);
    CHECK(std::fabs(r.x[0] - 1.0) < 1e-6);
    CHECK(std::fabs(r.x[1] - 1.0) < 1e-6);
}

TEST_CASE("newuoa: 4-D sphere at the default npt = 2n+1", "[hesap-opt][newuoa]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    const crd::f64 c[] = {0.3, -1.2, 0.8, 2.1};
    const crd::containers::ConstSpan<crd::f64> cs{c, 4};
    const ShiftQuad obj(cs);
    const crd::f64 x0[] = {0.0, 0.0, 0.0, 0.0};
    opt::NewuoaOptions<crd::f64> no;
    no.rhoend = 1e-9;
    const opt::OptResult<crd::f64> r = opt::minimize_newuoa<crd::f64>(obj, {x0, 4}, &alloc, no);
    REQUIRE(r.status == opt::OptStatus::Success);
    for (crd::usize j = 0; j < 4; ++j)
    {
        CHECK(std::fabs(r.x[j] - c[j]) < 1e-6);
    }
}

TEST_CASE("newuoa: bit-identical determinism (run twice)", "[hesap-opt][newuoa][determinism]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    const Rosen2 obj;
    const crd::f64 x0[] = {-1.2, 1.0};
    opt::NewuoaOptions<crd::f64> no;
    no.rhoend = 1e-9;
    const opt::OptResult<crd::f64> r1 = opt::minimize_newuoa<crd::f64>(obj, {x0, 2}, &alloc, no);
    const opt::OptResult<crd::f64> r2 = opt::minimize_newuoa<crd::f64>(obj, {x0, 2}, &alloc, no);
    REQUIRE(r1.status == r2.status);
    REQUIRE(r1.fn_evals == r2.fn_evals);
    REQUIRE(r1.x[0] == r2.x[0]); // bit-identical
    REQUIRE(r1.x[1] == r2.x[1]);
    REQUIRE(r1.fx == r2.fx);
}

TEST_CASE("newuoa: n = 0 boundary", "[hesap-opt][newuoa]")
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
    const opt::OptResult<crd::f64> r = opt::minimize_newuoa<crd::f64>(obj, {}, &alloc);
    CHECK(r.status == opt::OptStatus::Success);
}
