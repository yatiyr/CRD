// v7-p-1 — the DIRECT-SEARCH trio: Nelder-Mead (scipy semantics) + Powell conjugate directions (Brent 1-D) +
// pattern search (GPS / OrthoMADS-Philox). Gates: (1) all FOUR variants agree with the analytic minimum of a
// shifted quadratic; (2) Nelder-Mead + Powell solve Rosenbrock-2 (the curved-valley smooth gate; pattern
// search's domain is nonsmooth, not curved valleys — honest scoping); (3) the NONSMOOTH gate — an l1 objective
// where gradient methods are INAPPLICABLE, solved by Nelder-Mead and both pattern-search polls; (4) Powell's
// finite-termination-flavored gate on a CROSS-COUPLED quadratic (the conjugacy property: few sweeps, far fewer
// evals than the dimension-naive bound); (5) bit-identical determinism incl. the OrthoMADS Philox stream;
// (6) n = 1 and n = 0 boundaries.

#include <crd/hesap/opt/nelder_mead.hpp>
#include <crd/hesap/opt/pattern_search.hpp>
#include <crd/hesap/opt/powell.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>
#include <cmath>

namespace opt = crd::hesap::opt;

namespace
{

// f = sum (x_i - c_i)^2 with a cross-coupling term option: + kappa * (x_0 - c_0) * (x_1 - c_1) for n >= 2.
class ShiftedQuad final : public opt::Objective<crd::f64>
{
public:
    ShiftedQuad(crd::containers::ConstSpan<crd::f64> c, crd::f64 kappa = 0.0) noexcept
        : Objective<crd::f64>(false, false), m_c(c), m_kappa(kappa)
    {
    }
    [[nodiscard]] crd::f64 value(crd::containers::ConstSpan<crd::f64> x) const override
    {
        crd::f64 acc = 0.0;
        for (crd::usize i = 0; i < m_c.size(); ++i)
        {
            const crd::f64 d = x[i] - m_c[i];
            acc += d * d;
        }
        if (m_kappa != 0.0 && m_c.size() >= 2)
        {
            acc += m_kappa * (x[0] - m_c[0]) * (x[1] - m_c[1]);
        }
        return acc;
    }
    [[nodiscard]] crd::usize n() const noexcept override { return m_c.size(); }

private:
    crd::containers::ConstSpan<crd::f64> m_c;
    crd::f64 m_kappa;
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

// Nonsmooth: f = sum |x_i - c_i| — no gradient exists at the solution; the direct-search home turf.
class L1Shift final : public opt::Objective<crd::f64>
{
public:
    explicit L1Shift(crd::containers::ConstSpan<crd::f64> c) noexcept : Objective<crd::f64>(false, false), m_c(c) {}
    [[nodiscard]] crd::f64 value(crd::containers::ConstSpan<crd::f64> x) const override
    {
        crd::f64 acc = 0.0;
        for (crd::usize i = 0; i < m_c.size(); ++i)
        {
            acc += std::fabs(x[i] - m_c[i]);
        }
        return acc;
    }
    [[nodiscard]] crd::usize n() const noexcept override { return m_c.size(); }

private:
    crd::containers::ConstSpan<crd::f64> m_c;
};

} // namespace

TEST_CASE("direct search: all variants hit the analytic quadratic minimum", "[hesap-opt][direct-search]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    const crd::f64 c[] = {1.5, -0.5, 2.0};
    const crd::containers::ConstSpan<crd::f64> cs{c, 3};
    const ShiftedQuad obj(cs);
    const crd::f64 x0[] = {0.0, 0.0, 0.0};
    opt::OptOptions<crd::f64> opts;
    opts.max_iters = 2000;

    SECTION("Nelder-Mead")
    {
        opt::NelderMeadOptions<crd::f64> nm;
        nm.max_fun = 10000;
        const opt::OptResult<crd::f64> r = opt::minimize_nelder_mead<crd::f64>(obj, {x0, 3}, opts, &alloc, nm);
        REQUIRE(r.status == opt::OptStatus::Success);
        for (crd::usize j = 0; j < 3; ++j)
        {
            CHECK(std::fabs(r.x[j] - c[j]) < 1e-5);
        }
    }
    SECTION("Nelder-Mead adaptive (Gao-Han)")
    {
        opt::NelderMeadOptions<crd::f64> nm;
        nm.adaptive = true;
        nm.max_fun = 10000;
        const opt::OptResult<crd::f64> r = opt::minimize_nelder_mead<crd::f64>(obj, {x0, 3}, opts, &alloc, nm);
        REQUIRE(r.status == opt::OptStatus::Success);
        for (crd::usize j = 0; j < 3; ++j)
        {
            CHECK(std::fabs(r.x[j] - c[j]) < 1e-5);
        }
    }
    SECTION("Powell")
    {
        const opt::OptResult<crd::f64> r = opt::minimize_powell<crd::f64>(obj, {x0, 3}, opts, &alloc);
        REQUIRE(r.status == opt::OptStatus::Success);
        for (crd::usize j = 0; j < 3; ++j)
        {
            CHECK(std::fabs(r.x[j] - c[j]) < 1e-6);
        }
    }
    SECTION("pattern search GPS")
    {
        const opt::OptResult<crd::f64> r = opt::minimize_pattern_search<crd::f64>(obj, {x0, 3}, opts, &alloc);
        REQUIRE(r.status == opt::OptStatus::Success);
        for (crd::usize j = 0; j < 3; ++j)
        {
            CHECK(std::fabs(r.x[j] - c[j]) < 1e-5);
        }
    }
    SECTION("pattern search OrthoMADS")
    {
        opt::PatternSearchOptions<crd::f64> ps;
        ps.poll = opt::PatternPoll::OrthoMads;
        const opt::OptResult<crd::f64> r = opt::minimize_pattern_search<crd::f64>(obj, {x0, 3}, opts, &alloc, ps);
        REQUIRE(r.status == opt::OptStatus::Success);
        for (crd::usize j = 0; j < 3; ++j)
        {
            CHECK(std::fabs(r.x[j] - c[j]) < 1e-5);
        }
    }
}

TEST_CASE("direct search: Nelder-Mead and Powell solve Rosenbrock-2", "[hesap-opt][direct-search]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    const Rosen2 obj;
    const crd::f64 x0[] = {-1.2, 1.0};
    opt::OptOptions<crd::f64> opts;
    opts.max_iters = 5000;

    opt::NelderMeadOptions<crd::f64> nm;
    nm.max_fun = 20000;
    const opt::OptResult<crd::f64> rn = opt::minimize_nelder_mead<crd::f64>(obj, {x0, 2}, opts, &alloc, nm);
    REQUIRE(rn.status == opt::OptStatus::Success);
    CHECK(std::fabs(rn.x[0] - 1.0) < 1e-4);
    CHECK(std::fabs(rn.x[1] - 1.0) < 1e-4);

    const opt::OptResult<crd::f64> rp = opt::minimize_powell<crd::f64>(obj, {x0, 2}, opts, &alloc);
    REQUIRE(rp.status == opt::OptStatus::Success);
    CHECK(std::fabs(rp.x[0] - 1.0) < 1e-5);
    CHECK(std::fabs(rp.x[1] - 1.0) < 1e-5);
}

TEST_CASE("direct search: the nonsmooth l1 gate (gradient methods inapplicable)", "[hesap-opt][direct-search]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    const crd::f64 c[] = {0.7, -1.3};
    const crd::containers::ConstSpan<crd::f64> cs{c, 2};
    const L1Shift obj(cs);
    const crd::f64 x0[] = {3.0, 2.0};
    opt::OptOptions<crd::f64> opts;
    opts.max_iters = 5000;

    SECTION("Nelder-Mead")
    {
        opt::NelderMeadOptions<crd::f64> nm;
        nm.max_fun = 20000;
        const opt::OptResult<crd::f64> r = opt::minimize_nelder_mead<crd::f64>(obj, {x0, 2}, opts, &alloc, nm);
        REQUIRE(r.status == opt::OptStatus::Success);
        CHECK(std::fabs(r.x[0] - c[0]) < 1e-5);
        CHECK(std::fabs(r.x[1] - c[1]) < 1e-5);
    }
    SECTION("pattern search GPS (the kink-aligned poll)")
    {
        const opt::OptResult<crd::f64> r = opt::minimize_pattern_search<crd::f64>(obj, {x0, 2}, opts, &alloc);
        REQUIRE(r.status == opt::OptStatus::Success);
        CHECK(std::fabs(r.x[0] - c[0]) < 1e-6);
        CHECK(std::fabs(r.x[1] - c[1]) < 1e-6);
    }
    SECTION("pattern search OrthoMADS")
    {
        opt::PatternSearchOptions<crd::f64> ps;
        ps.poll = opt::PatternPoll::OrthoMads;
        const opt::OptResult<crd::f64> r = opt::minimize_pattern_search<crd::f64>(obj, {x0, 2}, opts, &alloc, ps);
        REQUIRE(r.status == opt::OptStatus::Success);
        CHECK(std::fabs(r.x[0] - c[0]) < 1e-5);
        CHECK(std::fabs(r.x[1] - c[1]) < 1e-5);
    }
}

TEST_CASE("direct search: Powell conjugacy on a cross-coupled quadratic", "[hesap-opt][direct-search]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    // kappa = 1.5 couples x0/x1 strongly; coordinate descent alone zigzags, Powell's replacement rule builds
    // the conjugate direction and finishes in a handful of sweeps.
    const crd::f64 c[] = {2.0, -1.0};
    const crd::containers::ConstSpan<crd::f64> cs{c, 2};
    const ShiftedQuad obj(cs, /*kappa=*/1.5);
    const crd::f64 x0[] = {10.0, 10.0};
    opt::OptOptions<crd::f64> opts;
    opts.max_iters = 100;

    const opt::OptResult<crd::f64> r = opt::minimize_powell<crd::f64>(obj, {x0, 2}, opts, &alloc);
    REQUIRE(r.status == opt::OptStatus::Success);
    CHECK(std::fabs(r.x[0] - c[0]) < 1e-6);
    CHECK(std::fabs(r.x[1] - c[1]) < 1e-6);
    CHECK(r.iterations <= 12); // conjugacy: a handful of sweeps, not a zigzag crawl
}

TEST_CASE("direct search: bit-identical determinism", "[hesap-opt][direct-search][determinism]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    const crd::f64 c[] = {1.5, -0.5, 2.0};
    const crd::containers::ConstSpan<crd::f64> cs{c, 3};
    const ShiftedQuad obj(cs);
    const crd::f64 x0[] = {0.0, 0.0, 0.0};
    opt::OptOptions<crd::f64> opts;
    opts.max_iters = 2000;

    const opt::OptResult<crd::f64> n1 = opt::minimize_nelder_mead<crd::f64>(obj, {x0, 3}, opts, &alloc);
    const opt::OptResult<crd::f64> n2 = opt::minimize_nelder_mead<crd::f64>(obj, {x0, 3}, opts, &alloc);
    const opt::OptResult<crd::f64> p1 = opt::minimize_powell<crd::f64>(obj, {x0, 3}, opts, &alloc);
    const opt::OptResult<crd::f64> p2 = opt::minimize_powell<crd::f64>(obj, {x0, 3}, opts, &alloc);
    opt::PatternSearchOptions<crd::f64> ps;
    ps.poll = opt::PatternPoll::OrthoMads;
    ps.seed = 0xC0FFEEULL;
    const opt::OptResult<crd::f64> m1 = opt::minimize_pattern_search<crd::f64>(obj, {x0, 3}, opts, &alloc, ps);
    const opt::OptResult<crd::f64> m2 = opt::minimize_pattern_search<crd::f64>(obj, {x0, 3}, opts, &alloc, ps);
    for (crd::usize j = 0; j < 3; ++j)
    {
        REQUIRE(n1.x[j] == n2.x[j]); // bit-identical
        REQUIRE(p1.x[j] == p2.x[j]);
        REQUIRE(m1.x[j] == m2.x[j]); // incl. the Philox-keyed OrthoMADS stream
    }
    REQUIRE(n1.fn_evals == n2.fn_evals);
    REQUIRE(p1.fn_evals == p2.fn_evals);
    REQUIRE(m1.fn_evals == m2.fn_evals);

    // A DIFFERENT OrthoMADS seed still converges (the poll set is fresh randomness; the answer is not).
    ps.seed = 0xBEEFULL;
    const opt::OptResult<crd::f64> m3 = opt::minimize_pattern_search<crd::f64>(obj, {x0, 3}, opts, &alloc, ps);
    REQUIRE(m3.status == opt::OptStatus::Success);
    for (crd::usize j = 0; j < 3; ++j)
    {
        CHECK(std::fabs(m3.x[j] - c[j]) < 1e-5);
    }
}

TEST_CASE("direct search: n = 1 and n = 0 boundaries", "[hesap-opt][direct-search]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    const crd::f64 c1[] = {3.25};
    const crd::containers::ConstSpan<crd::f64> cs1{c1, 1};
    const ShiftedQuad obj1(cs1);
    const crd::f64 x01[] = {-5.0};
    opt::OptOptions<crd::f64> opts;
    opts.max_iters = 2000;

    const opt::OptResult<crd::f64> rn = opt::minimize_nelder_mead<crd::f64>(obj1, {x01, 1}, opts, &alloc);
    REQUIRE(rn.status == opt::OptStatus::Success);
    CHECK(std::fabs(rn.x[0] - 3.25) < 1e-6);
    const opt::OptResult<crd::f64> rp = opt::minimize_powell<crd::f64>(obj1, {x01, 1}, opts, &alloc);
    REQUIRE(rp.status == opt::OptStatus::Success);
    CHECK(std::fabs(rp.x[0] - 3.25) < 1e-6);
    const opt::OptResult<crd::f64> rs = opt::minimize_pattern_search<crd::f64>(obj1, {x01, 1}, opts, &alloc);
    REQUIRE(rs.status == opt::OptStatus::Success);
    CHECK(std::fabs(rs.x[0] - 3.25) < 1e-5);

    const crd::containers::ConstSpan<crd::f64> empty{};
    const ShiftedQuad obj0(empty);
    const opt::OptResult<crd::f64> r0 = opt::minimize_nelder_mead<crd::f64>(obj0, empty, opts, &alloc);
    CHECK(r0.status == opt::OptStatus::Success);
}
