// v7-r — MIP branch and bound over the v7-l simplex. Gates: (1) a classic integer LP whose LP relaxation is
// FRACTIONAL (branching must do real work; the known integral optimum is checked); (2) a 0/1 knapsack with
// the brute-force optimum; (3) a Philox cross-adjudication scan: random small BINARY problems solved by B&B
// vs EXHAUSTIVE enumeration (the absolute oracle); (4) mixed integer/continuous variables; (5) certified
// infeasibility; (6) bit-identical determinism.

#include <crd/hesap/opt/mip.hpp>
#include <crd/hesap/stats/philox.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <limits>

namespace opt = crd::hesap::opt;
namespace st = crd::hesap::stats;

namespace
{
constexpr crd::f64 kInf = std::numeric_limits<crd::f64>::infinity();
} // namespace

TEST_CASE("mip: fractional relaxation forced integral", "[hesap-opt][mip]")
{
    crd::memory::TlsfAllocator alloc(1U << 24);
    // max x + y s.t. 2x + 3y <= 12, 6x + 5y <= 30, x,y >= 0 integer  (min form: c = (-1, -1)).
    // LP relaxation optimum is fractional (x = 15/4, y = 3/2); the integer optimum is x + y = 5
    // (e.g. (3, 2): 6+6=12 ok, 18+10=28 ok).
    const crd::f64 c[] = {-1.0, -1.0};
    const crd::f64 a[] = {2.0, 3.0, 6.0, 5.0};
    const crd::f64 l[] = {-kInf, -kInf};
    const crd::f64 u[] = {12.0, 30.0};
    const crd::f64 xlo[] = {0.0, 0.0};
    const crd::f64 xup[] = {10.0, 10.0};
    const opt::LpProblem<crd::f64> prob{{c, 2}, {a, 4}, {l, 2}, {u, 2}, {xlo, 2}, {xup, 2}, 2, 2};
    const bool mask[] = {true, true};

    const opt::MipResult<crd::f64> r = opt::solve_mip_branch_and_bound<crd::f64>(prob, {mask, 2}, &alloc);
    REQUIRE(r.status == opt::QpStatus::Solved);
    CHECK(std::fabs(r.obj - (-5.0)) < 1e-9);
    CHECK(std::fabs(r.x[0] - std::floor(r.x[0] + 0.5)) < 1e-9); // exactly integral
    CHECK(std::fabs(r.x[1] - std::floor(r.x[1] + 0.5)) < 1e-9);
}

TEST_CASE("mip: 0/1 knapsack vs the brute-force optimum", "[hesap-opt][mip]")
{
    crd::memory::TlsfAllocator alloc(1U << 24);
    // max v.x s.t. w.x <= W, x in {0,1}^6.
    const crd::f64 v[] = {10.0, 13.0, 7.0, 8.0, 12.0, 5.0};
    const crd::f64 w[] = {5.0, 7.0, 3.0, 4.0, 6.0, 2.0};
    const crd::f64 wcap = 14.0;
    // Brute force (the absolute oracle).
    crd::f64 best = 0.0;
    for (crd::u32 m = 0; m < 64; ++m)
    {
        crd::f64 tv = 0.0;
        crd::f64 tw = 0.0;
        for (crd::u32 i = 0; i < 6; ++i)
        {
            if ((m >> i) & 1U)
            {
                tv += v[i];
                tw += w[i];
            }
        }
        if (tw <= wcap && tv > best)
        {
            best = tv;
        }
    }
    crd::f64 c[6];
    for (crd::usize i = 0; i < 6; ++i)
    {
        c[i] = -v[i];
    }
    const crd::f64 l[] = {-kInf};
    const crd::f64 u[] = {wcap};
    const crd::f64 xlo[] = {0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
    const crd::f64 xup[] = {1.0, 1.0, 1.0, 1.0, 1.0, 1.0};
    const opt::LpProblem<crd::f64> prob{{c, 6}, {w, 6}, {l, 1}, {u, 1}, {xlo, 6}, {xup, 6}, 6, 1};
    const bool mask[] = {true, true, true, true, true, true};

    const opt::MipResult<crd::f64> r = opt::solve_mip_branch_and_bound<crd::f64>(prob, {mask, 6}, &alloc);
    REQUIRE(r.status == opt::QpStatus::Solved);
    CHECK(std::fabs(-r.obj - best) < 1e-9);
}

TEST_CASE("mip: random binary problems vs exhaustive enumeration (Philox scan)", "[hesap-opt][mip]")
{
    crd::memory::TlsfAllocator alloc(1U << 24);
    st::PhiloxRng rng(/*seed=*/0x317U, /*stream=*/0U);
    constexpr crd::usize n = 8;
    constexpr crd::usize m = 3;
    for (crd::usize inst = 0; inst < 25; ++inst)
    {
        crd::f64 c[n];
        crd::f64 a[m * n];
        crd::f64 l[m];
        crd::f64 u[m];
        for (crd::usize j = 0; j < n; ++j)
        {
            c[j] = rng.next_f64() * 4.0 - 2.0;
        }
        for (crd::usize i = 0; i < m; ++i)
        {
            crd::f64 row_sum = 0.0;
            for (crd::usize j = 0; j < n; ++j)
            {
                a[i * n + j] = rng.next_f64(); // nonnegative rows: u in (0, sum) keeps it feasible (x = 0 ok)
                row_sum += a[i * n + j];
            }
            l[i] = -kInf;
            u[i] = 0.2 * row_sum + rng.next_f64() * 0.6 * row_sum;
        }
        // Exhaustive oracle over 2^8.
        crd::f64 best = kInf;
        for (crd::u32 msk = 0; msk < (1U << n); ++msk)
        {
            bool ok = true;
            for (crd::usize i = 0; i < m && ok; ++i)
            {
                crd::f64 ax = 0.0;
                for (crd::usize j = 0; j < n; ++j)
                {
                    ax += ((msk >> j) & 1U) ? a[i * n + j] : 0.0;
                }
                ok = ax <= u[i] + 1e-12;
            }
            if (ok)
            {
                crd::f64 obj = 0.0;
                for (crd::usize j = 0; j < n; ++j)
                {
                    obj += ((msk >> j) & 1U) ? c[j] : 0.0;
                }
                best = obj < best ? obj : best;
            }
        }
        crd::f64 xlo[n];
        crd::f64 xup[n];
        bool mask[n];
        for (crd::usize j = 0; j < n; ++j)
        {
            xlo[j] = 0.0;
            xup[j] = 1.0;
            mask[j] = true;
        }
        const opt::LpProblem<crd::f64> prob{{c, n}, {a, m * n}, {l, m}, {u, m}, {xlo, n}, {xup, n}, n, m};
        // Root-relaxation cross-check (simplex vs IPM) — the MIP oracle caught a regime the LP scan missed.
        const opt::LpResult<crd::f64> rs = opt::solve_lp_simplex<crd::f64>(prob, &alloc);
        const opt::LpResult<crd::f64> ri = opt::solve_lp_mehrotra<crd::f64>(prob, &alloc);
        CAPTURE(inst, rs.obj, ri.obj);
        if (ri.status == opt::QpStatus::Solved)
        {
            REQUIRE(std::fabs(rs.obj - ri.obj) / (1.0 + std::fabs(rs.obj)) < 1e-5);
        }
        const opt::MipResult<crd::f64> r = opt::solve_mip_branch_and_bound<crd::f64>(prob, {mask, n}, &alloc);
        CAPTURE(r.obj, best, r.nodes);
        REQUIRE(r.status == opt::QpStatus::Solved);
        REQUIRE(std::fabs(r.obj - best) < 1e-7); // matches the absolute oracle
    }
}

TEST_CASE("mip: mixed integer/continuous", "[hesap-opt][mip]")
{
    crd::memory::TlsfAllocator alloc(1U << 24);
    // min -x - 0.6 y s.t. x + y <= 3.5, x integer in [0, 5], y continuous in [0, 5].
    // Optimum: x = 3 (integer), y = 0.5 (fills the slack) -> obj = -3.3.
    const crd::f64 c[] = {-1.0, -0.6};
    const crd::f64 a[] = {1.0, 1.0};
    const crd::f64 l[] = {-kInf};
    const crd::f64 u[] = {3.5};
    const crd::f64 xlo[] = {0.0, 0.0};
    const crd::f64 xup[] = {5.0, 5.0};
    const opt::LpProblem<crd::f64> prob{{c, 2}, {a, 2}, {l, 1}, {u, 1}, {xlo, 2}, {xup, 2}, 2, 1};
    const bool mask[] = {true, false};

    const opt::MipResult<crd::f64> r = opt::solve_mip_branch_and_bound<crd::f64>(prob, {mask, 2}, &alloc);
    REQUIRE(r.status == opt::QpStatus::Solved);
    CHECK(std::fabs(r.x[0] - 3.0) < 1e-9);
    CHECK(std::fabs(r.x[1] - 0.5) < 1e-7);
    CHECK(std::fabs(r.obj - (-3.3)) < 1e-7);
}

TEST_CASE("mip: certified infeasibility", "[hesap-opt][mip]")
{
    crd::memory::TlsfAllocator alloc(1U << 24);
    // 2x = 1 with x integer in [0, 3]: the LP relaxation is feasible (x = 0.5) but no integer point exists.
    const crd::f64 c[] = {1.0};
    const crd::f64 a[] = {2.0};
    const crd::f64 l[] = {1.0};
    const crd::f64 u[] = {1.0};
    const crd::f64 xlo[] = {0.0};
    const crd::f64 xup[] = {3.0};
    const opt::LpProblem<crd::f64> prob{{c, 1}, {a, 1}, {l, 1}, {u, 1}, {xlo, 1}, {xup, 1}, 1, 1};
    const bool mask[] = {true};
    const opt::MipResult<crd::f64> r = opt::solve_mip_branch_and_bound<crd::f64>(prob, {mask, 1}, &alloc);
    CHECK(r.status == opt::QpStatus::PrimalInfeasible);
    CHECK(!r.has_incumbent);
}

TEST_CASE("mip: bit-identical determinism (run twice)", "[hesap-opt][mip][determinism]")
{
    crd::memory::TlsfAllocator alloc(1U << 24);
    const crd::f64 c[] = {-1.0, -1.0};
    const crd::f64 a[] = {2.0, 3.0, 6.0, 5.0};
    const crd::f64 l[] = {-kInf, -kInf};
    const crd::f64 u[] = {12.0, 30.0};
    const crd::f64 xlo[] = {0.0, 0.0};
    const crd::f64 xup[] = {10.0, 10.0};
    const opt::LpProblem<crd::f64> prob{{c, 2}, {a, 4}, {l, 2}, {u, 2}, {xlo, 2}, {xup, 2}, 2, 2};
    const bool mask[] = {true, true};
    const opt::MipResult<crd::f64> r1 = opt::solve_mip_branch_and_bound<crd::f64>(prob, {mask, 2}, &alloc);
    const opt::MipResult<crd::f64> r2 = opt::solve_mip_branch_and_bound<crd::f64>(prob, {mask, 2}, &alloc);
    REQUIRE(r1.status == r2.status);
    REQUIRE(r1.nodes == r2.nodes);
    REQUIRE(r1.obj == r2.obj); // bit-identical
    REQUIRE(r1.x[0] == r2.x[0]);
    REQUIRE(r1.x[1] == r2.x[1]);
}
