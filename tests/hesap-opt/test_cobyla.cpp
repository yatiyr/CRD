// v7-p-2 — COBYLA (the faithful NLopt-reference port). Functional gates: (1) Powell's own paper test problem
// (min x0*x1 in the unit disk -> f* = -1/2 on the diagonal); (2) Rosenbrock-in-the-unit-disk against the
// scipy reference solution ALREADY pinned by the v7-n battery -- a CROSS-FAMILY adjudication (a derivative-
// free simplex-model method agreeing with SQP/auglag/IPM on the same instance); (3) active variable bounds;
// (4) bounded Rosenbrock (no general constraints); (5) bit-identical determinism (incl. the reference's
// deterministic LCG); (6) n = 0. The PER-ROUTINE differential harness vs the compiled NLopt oracle is the
// separate v7-p-2 close gate (runtime/examples/cobyla_difftest.cpp, CRD_BUILD_HESAP_VS_COBYLA).

#include <crd/hesap/opt/cobyla.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <limits>

namespace opt = crd::hesap::opt;

namespace
{

class ProductObj final : public opt::Objective<crd::f64>
{
public:
    ProductObj() noexcept : Objective<crd::f64>(false, false) {}
    [[nodiscard]] crd::f64 value(crd::containers::ConstSpan<crd::f64> x) const override { return x[0] * x[1]; }
    [[nodiscard]] crd::usize n() const noexcept override { return 2; }
};

class Rosen2Obj final : public opt::Objective<crd::f64>
{
public:
    Rosen2Obj() noexcept : Objective<crd::f64>(false, false) {}
    [[nodiscard]] crd::f64 value(crd::containers::ConstSpan<crd::f64> x) const override
    {
        const crd::f64 a = 1.0 - x[0];
        const crd::f64 b = x[1] - x[0] * x[0];
        return a * a + 100.0 * b * b;
    }
    [[nodiscard]] crd::usize n() const noexcept override { return 2; }
};

class SumObj final : public opt::Objective<crd::f64>
{
public:
    SumObj() noexcept : Objective<crd::f64>(false, false) {}
    [[nodiscard]] crd::f64 value(crd::containers::ConstSpan<crd::f64> x) const override { return x[0] + x[1]; }
    [[nodiscard]] crd::usize n() const noexcept override { return 2; }
};

// c(x) = 1 - x'x >= 0 (the unit disk), value-only (no Jacobians -- COBYLA never asks).
class DiskCons final : public opt::Constraints<crd::f64>
{
public:
    DiskCons() noexcept : Constraints<crd::f64>(false) {}
    [[nodiscard]] crd::usize num_eq() const noexcept override { return 0; }
    [[nodiscard]] crd::usize num_ineq() const noexcept override { return 1; }
    [[nodiscard]] crd::usize n() const noexcept override { return 2; }
    void eval(crd::containers::ConstSpan<crd::f64> x, crd::containers::Span<crd::f64> ce,
              crd::containers::Span<crd::f64> ci) const override
    {
        (void)ce;
        ci[0] = 1.0 - x[0] * x[0] - x[1] * x[1];
    }
};

} // namespace

TEST_CASE("cobyla: Powell's unit-disk product problem", "[hesap-opt][cobyla]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    // min x0*x1 s.t. 1 - x0^2 - x1^2 >= 0: f* = -1/2 at (1/sqrt2, -1/sqrt2) (or the mirror).
    const ProductObj obj;
    const DiskCons cons;
    const crd::f64 x0[] = {1.0, 1.0};
    opt::CobylaOptions<crd::f64> co;
    co.rhobeg = 0.5;
    co.rhoend = 1e-10;
    const opt::OptResult<crd::f64> r = opt::minimize_cobyla<crd::f64>(obj, &cons, {x0, 2}, {}, {}, &alloc, co);
    REQUIRE(r.status == opt::OptStatus::Success);
    CHECK(std::fabs(r.fx - (-0.5)) < 1e-6);
    CHECK(std::fabs(std::fabs(r.x[0]) - 1.0 / std::sqrt(2.0)) < 1e-5);
    CHECK(std::fabs(std::fabs(r.x[1]) - 1.0 / std::sqrt(2.0)) < 1e-5);
    CHECK(r.x[0] * r.x[1] < 0.0); // opposite signs on the diagonal
}

TEST_CASE("cobyla: Rosenbrock-in-the-unit-disk (cross-family adjudication)", "[hesap-opt][cobyla]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    // The scipy reference instance the v7-n battery pinned for SQP/auglag/IPM: x* ~ (0.78642, 0.61770).
    // A DERIVATIVE-FREE method from a DIFFERENT algorithm family must land on the same point.
    const Rosen2Obj obj;
    const DiskCons cons;
    const crd::f64 x0[] = {0.0, 0.0};
    opt::CobylaOptions<crd::f64> co;
    co.rhobeg = 0.5;
    co.rhoend = 1e-10;
    co.max_evals = 20000;
    const opt::OptResult<crd::f64> r = opt::minimize_cobyla<crd::f64>(obj, &cons, {x0, 2}, {}, {}, &alloc, co);
    REQUIRE(r.status == opt::OptStatus::Success);
    CHECK(std::fabs(r.x[0] - 0.7864151510041092) < 1e-5);
    CHECK(std::fabs(r.x[1] - 0.6176983165954114) < 1e-5);
    // On the boundary: the disk constraint is active.
    CHECK(std::fabs(r.x[0] * r.x[0] + r.x[1] * r.x[1] - 1.0) < 1e-6);
}

TEST_CASE("cobyla: active variable bounds", "[hesap-opt][cobyla]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    // min x0 + x1 with x >= (1, 2): the bounds pin x* = (1, 2).
    const SumObj obj;
    const crd::f64 x0[] = {3.0, 4.0};
    const crd::f64 lo[] = {1.0, 2.0};
    const crd::f64 up[] = {10.0, 10.0};
    opt::CobylaOptions<crd::f64> co;
    co.rhobeg = 0.5;
    co.rhoend = 1e-10;
    const opt::OptResult<crd::f64> r =
        opt::minimize_cobyla<crd::f64>(obj, nullptr, {x0, 2}, {lo, 2}, {up, 2}, &alloc, co);
    REQUIRE(r.status == opt::OptStatus::Success);
    CHECK(std::fabs(r.x[0] - 1.0) < 1e-7);
    CHECK(std::fabs(r.x[1] - 2.0) < 1e-7);
    CHECK(r.x[0] >= 1.0 - 1e-12); // never evaluated outside the box (ENFORCE_BOUNDS)
    CHECK(r.x[1] >= 2.0 - 1e-12);
}

TEST_CASE("cobyla: bounded Rosenbrock (no general constraints)", "[hesap-opt][cobyla]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    const Rosen2Obj obj;
    const crd::f64 x0[] = {-1.2, 1.0};
    const crd::f64 lo[] = {-2.0, -2.0};
    const crd::f64 up[] = {2.0, 2.0};
    opt::CobylaOptions<crd::f64> co;
    co.rhoend = 1e-10;
    co.max_evals = 200000;
    const opt::OptResult<crd::f64> r =
        opt::minimize_cobyla<crd::f64>(obj, nullptr, {x0, 2}, {lo, 2}, {up, 2}, &alloc, co);
    CAPTURE(r.x[0], r.x[1], r.fx, r.fn_evals);
    REQUIRE(r.status == opt::OptStatus::Success);
    CHECK(std::fabs(r.x[0] - 1.0) < 1e-5);
    CHECK(std::fabs(r.x[1] - 1.0) < 1e-5);
}

TEST_CASE("cobyla: bit-identical determinism (run twice)", "[hesap-opt][cobyla][determinism]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    const Rosen2Obj obj;
    const DiskCons cons;
    const crd::f64 x0[] = {0.0, 0.0};
    opt::CobylaOptions<crd::f64> co;
    co.rhoend = 1e-9;
    const opt::OptResult<crd::f64> r1 = opt::minimize_cobyla<crd::f64>(obj, &cons, {x0, 2}, {}, {}, &alloc, co);
    const opt::OptResult<crd::f64> r2 = opt::minimize_cobyla<crd::f64>(obj, &cons, {x0, 2}, {}, {}, &alloc, co);
    REQUIRE(r1.status == r2.status);
    REQUIRE(r1.fn_evals == r2.fn_evals); // incl. the reference's deterministic LCG
    REQUIRE(r1.x[0] == r2.x[0]);         // bit-identical
    REQUIRE(r1.x[1] == r2.x[1]);
    REQUIRE(r1.fx == r2.fx);
}

TEST_CASE("cobyla: n = 0 boundary", "[hesap-opt][cobyla]")
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
    const opt::OptResult<crd::f64> r = opt::minimize_cobyla<crd::f64>(obj, nullptr, {}, {}, {}, &alloc);
    CHECK(r.status == opt::OptStatus::Success);
}
