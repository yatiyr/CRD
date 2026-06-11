// v7-l — LINEAR PROGRAMMING: the bounded-variable revised simplex + the Mehrotra-at-P=0 interior point.
// Gates: analytic vertex solutions + EXACT dual recovery on a rows-only LP (q + Aᵀy = 0 checked directly) +
// the Beale degenerate/cycling example + certified infeasibility/unboundedness + a Philox-driven
// cross-adjudication scan (two INDEPENDENT algorithms must agree on the optimum) + bit-identical determinism.

#include <crd/hesap/opt/lp.hpp>
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

TEST_CASE("lp: analytic vertex LP solved by both members", "[hesap-opt][lp]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    // min -x - 2y  s.t.  x + y <= 4, x <= 3, y <= 2 (as rows), x >= 0, y >= 0 (variable bounds).
    // Optimum (2, 2), obj = -6.
    const crd::f64 c[] = {-1.0, -2.0};
    const crd::f64 a[] = {1.0, 1.0, 1.0, 0.0, 0.0, 1.0};
    const crd::f64 l[] = {-kInf, -kInf, -kInf};
    const crd::f64 u[] = {4.0, 3.0, 2.0};
    const crd::f64 xlo[] = {0.0, 0.0};
    const crd::f64 xup[] = {kInf, kInf};
    const opt::LpProblem<crd::f64> prob{{c, 2}, {a, 6}, {l, 3}, {u, 3}, {xlo, 2}, {xup, 2}, 2, 3};

    const opt::LpResult<crd::f64> rs = opt::solve_lp_simplex(prob, &alloc);
    REQUIRE(rs.status == opt::QpStatus::Solved);
    CHECK(std::fabs(rs.x[0] - 2.0) < 1e-9);
    CHECK(std::fabs(rs.x[1] - 2.0) < 1e-9);
    CHECK(std::fabs(rs.obj - (-6.0)) < 1e-9);

    const opt::LpResult<crd::f64> ri = opt::solve_lp_mehrotra(prob, &alloc);
    REQUIRE(ri.status == opt::QpStatus::Solved);
    CHECK(std::fabs(ri.x[0] - 2.0) < 1e-6);
    CHECK(std::fabs(ri.x[1] - 2.0) < 1e-6);
    CHECK(std::fabs(ri.obj - (-6.0)) < 1e-6);
}

TEST_CASE("lp: exact dual recovery on a rows-only LP (OSQP sign)", "[hesap-opt][lp]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    // Same LP with the variable bounds ALSO as rows (so stationarity q + A^T y = 0 holds exactly with no
    // bound duals). Rows: x+y<=4, x<=3, y<=2, x>=0, y>=0. Active at (2,2): rows 0 and 2.
    // q + A^T y = 0 with complementarity => y = (1, 0, 1, 0, 0).
    const crd::f64 c[] = {-1.0, -2.0};
    const crd::f64 a[] = {1.0, 1.0, 1.0, 0.0, 0.0, 1.0, 1.0, 0.0, 0.0, 1.0};
    const crd::f64 l[] = {-kInf, -kInf, -kInf, 0.0, 0.0};
    const crd::f64 u[] = {4.0, 3.0, 2.0, kInf, kInf};
    const opt::LpProblem<crd::f64> prob{{c, 2}, {a, 10}, {l, 5}, {u, 5}, {}, {}, 2, 5};
    const crd::f64 ystar[] = {1.0, 0.0, 1.0, 0.0, 0.0};

    const opt::LpResult<crd::f64> rs = opt::solve_lp_simplex(prob, &alloc);
    REQUIRE(rs.status == opt::QpStatus::Solved);
    CHECK(std::fabs(rs.obj - (-6.0)) < 1e-9);
    for (crd::usize i = 0; i < 5; ++i)
    {
        CHECK(std::fabs(rs.y[i] - ystar[i]) < 1e-8);
    }
    // The certificate directly: q + A^T y = 0.
    for (crd::usize j = 0; j < 2; ++j)
    {
        crd::f64 station = c[j];
        for (crd::usize i = 0; i < 5; ++i)
        {
            station += a[i * 2 + j] * rs.y[i];
        }
        CHECK(std::fabs(station) < 1e-8);
    }

    const opt::LpResult<crd::f64> ri = opt::solve_lp_mehrotra(prob, &alloc);
    REQUIRE(ri.status == opt::QpStatus::Solved);
    for (crd::usize i = 0; i < 5; ++i)
    {
        CHECK(std::fabs(ri.y[i] - ystar[i]) < 1e-5);
    }
}

TEST_CASE("lp: equality rows and a free variable", "[hesap-opt][lp]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    // min y  s.t.  x + y = 3 (equality row), y >= 0, x FREE. Optimum y = 0, x = 3, obj = 0.
    const crd::f64 c[] = {0.0, 1.0};
    const crd::f64 a[] = {1.0, 1.0};
    const crd::f64 l[] = {3.0};
    const crd::f64 u[] = {3.0};
    const crd::f64 xlo[] = {-kInf, 0.0};
    const crd::f64 xup[] = {kInf, kInf};
    const opt::LpProblem<crd::f64> prob{{c, 2}, {a, 2}, {l, 1}, {u, 1}, {xlo, 2}, {xup, 2}, 2, 1};

    const opt::LpResult<crd::f64> rs = opt::solve_lp_simplex(prob, &alloc);
    REQUIRE(rs.status == opt::QpStatus::Solved);
    CHECK(std::fabs(rs.x[0] - 3.0) < 1e-9);
    CHECK(std::fabs(rs.x[1]) < 1e-9);
    CHECK(std::fabs(rs.x[0] + rs.x[1] - 3.0) < 1e-9);

    const opt::LpResult<crd::f64> ri = opt::solve_lp_mehrotra(prob, &alloc);
    REQUIRE(ri.status == opt::QpStatus::Solved);
    CHECK(std::fabs(ri.x[1]) < 1e-6);
}

TEST_CASE("lp: bound flips (box optimum, slack row)", "[hesap-opt][lp]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    // min -x1 - x2  s.t.  x1 + x2 <= 10, 0 <= x <= 3. Optimum at the box corner (3, 3); the row stays slack,
    // so the simplex path is pure bound flips. obj = -6, row dual 0.
    const crd::f64 c[] = {-1.0, -1.0};
    const crd::f64 a[] = {1.0, 1.0};
    const crd::f64 l[] = {-kInf};
    const crd::f64 u[] = {10.0};
    const crd::f64 xlo[] = {0.0, 0.0};
    const crd::f64 xup[] = {3.0, 3.0};
    const opt::LpProblem<crd::f64> prob{{c, 2}, {a, 2}, {l, 1}, {u, 1}, {xlo, 2}, {xup, 2}, 2, 1};

    const opt::LpResult<crd::f64> rs = opt::solve_lp_simplex(prob, &alloc);
    REQUIRE(rs.status == opt::QpStatus::Solved);
    CHECK(std::fabs(rs.x[0] - 3.0) < 1e-9);
    CHECK(std::fabs(rs.x[1] - 3.0) < 1e-9);
    CHECK(std::fabs(rs.y[0]) < 1e-9); // slack row carries no dual
}

TEST_CASE("lp: Beale's degenerate example (anti-cycling)", "[hesap-opt][lp]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    // The classic cycling LP (Beale 1955): min -3/4 x1 + 150 x2 - 1/50 x3 + 6 x4
    //   s.t. 1/4 x1 - 60 x2 - 1/25 x3 + 9 x4 <= 0
    //        1/2 x1 - 90 x2 - 1/50 x3 + 3 x4 <= 0
    //        x3 <= 1,   x >= 0.
    // Optimum obj = -1/20 at x = (1/25, 0, 1, 0). Dantzig with naive tie-breaks cycles forever on this
    // instance; the Bland fallback must terminate at the right vertex.
    const crd::f64 c[] = {-0.75, 150.0, -0.02, 6.0};
    const crd::f64 a[] = {0.25, -60.0, -0.04, 9.0, 0.5, -90.0, -0.02, 3.0, 0.0, 0.0, 1.0, 0.0};
    const crd::f64 l[] = {-kInf, -kInf, -kInf};
    const crd::f64 u[] = {0.0, 0.0, 1.0};
    const crd::f64 xlo[] = {0.0, 0.0, 0.0, 0.0};
    const crd::f64 xup[] = {kInf, kInf, kInf, kInf};
    const opt::LpProblem<crd::f64> prob{{c, 4}, {a, 12}, {l, 3}, {u, 3}, {xlo, 4}, {xup, 4}, 4, 3};

    const opt::LpResult<crd::f64> rs = opt::solve_lp_simplex(prob, &alloc);
    REQUIRE(rs.status == opt::QpStatus::Solved);
    CHECK(std::fabs(rs.obj - (-0.05)) < 1e-9);
    CHECK(std::fabs(rs.x[0] - 0.04) < 1e-9);
    CHECK(std::fabs(rs.x[2] - 1.0) < 1e-9);
}

TEST_CASE("lp: certified infeasibility and unboundedness", "[hesap-opt][lp]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    SECTION("infeasible rows (x >= 2 and x <= 1) -> Phase I certificate")
    {
        const crd::f64 c[] = {1.0};
        const crd::f64 a[] = {1.0, 1.0};
        const crd::f64 l[] = {2.0, -kInf};
        const crd::f64 u[] = {kInf, 1.0};
        const opt::LpProblem<crd::f64> prob{{c, 1}, {a, 2}, {l, 2}, {u, 2}, {}, {}, 1, 2};
        const opt::LpResult<crd::f64> rs = opt::solve_lp_simplex(prob, &alloc);
        CHECK(rs.status == opt::QpStatus::PrimalInfeasible);
        const opt::LpResult<crd::f64> ri = opt::solve_lp_mehrotra(prob, &alloc);
        CHECK(ri.status != opt::QpStatus::Solved); // the IPM must not claim success
    }
    SECTION("unbounded ray (min -x, x >= 1, no upper bound) -> DualInfeasible")
    {
        const crd::f64 c[] = {-1.0};
        const crd::f64 a[] = {1.0};
        const crd::f64 l[] = {1.0};
        const crd::f64 u[] = {kInf};
        const opt::LpProblem<crd::f64> prob{{c, 1}, {a, 1}, {l, 1}, {u, 1}, {}, {}, 1, 1};
        const opt::LpResult<crd::f64> rs = opt::solve_lp_simplex(prob, &alloc);
        CHECK(rs.status == opt::QpStatus::DualInfeasible);
    }
}

TEST_CASE("lp: simplex vs interior point cross-adjudication (Philox scan)", "[hesap-opt][lp]")
{
    crd::memory::TlsfAllocator alloc(1U << 24);
    // Two INDEPENDENT algorithms (combinatorial vertex pivoting vs the smooth predictor-corrector) must agree
    // on the optimal VALUE of every random boxed-feasible instance. Boxed variables guarantee boundedness;
    // row bounds are placed around A*x_ref with positive slack so feasibility is guaranteed by construction.
    st::PhiloxRng rng(/*seed=*/0x17CEU, /*stream=*/0U);
    constexpr crd::usize n = 4;
    constexpr crd::usize m = 6;
    crd::usize agree = 0;
    constexpr crd::usize instances = 60;
    for (crd::usize inst = 0; inst < instances; ++inst)
    {
        crd::f64 c[n];
        crd::f64 a[m * n];
        crd::f64 l[m];
        crd::f64 u[m];
        crd::f64 xlo[n];
        crd::f64 xup[n];
        crd::f64 xref[n];
        for (crd::usize j = 0; j < n; ++j)
        {
            c[j] = rng.next_f64() * 4.0 - 2.0;
            xlo[j] = -2.0;
            xup[j] = 3.0;
            xref[j] = xlo[j] + rng.next_f64() * (xup[j] - xlo[j]);
        }
        for (crd::usize i = 0; i < m; ++i)
        {
            crd::f64 ax = 0.0;
            for (crd::usize j = 0; j < n; ++j)
            {
                a[i * n + j] = rng.next_f64() * 2.0 - 1.0;
                ax += a[i * n + j] * xref[j];
            }
            l[i] = ax - (0.2 + rng.next_f64() * 2.0);
            u[i] = ax + (0.2 + rng.next_f64() * 2.0);
        }
        const opt::LpProblem<crd::f64> prob{{c, n}, {a, m * n}, {l, m}, {u, m}, {xlo, n}, {xup, n}, n, m};
        const opt::LpResult<crd::f64> rs = opt::solve_lp_simplex(prob, &alloc);
        const opt::LpResult<crd::f64> ri = opt::solve_lp_mehrotra(prob, &alloc);
        REQUIRE(rs.status == opt::QpStatus::Solved); // feasible + bounded by construction
        if (ri.status != opt::QpStatus::Solved)
        {
            continue; // the IPM may hit its iteration cap on a near-degenerate instance; the simplex is gated
        }
        const crd::f64 scale = 1.0 + std::fabs(rs.obj);
        REQUIRE(std::fabs(rs.obj - ri.obj) / scale < 1e-5);
        // The simplex vertex must satisfy every row and box to tolerance.
        for (crd::usize i = 0; i < m; ++i)
        {
            crd::f64 ax = 0.0;
            for (crd::usize j = 0; j < n; ++j)
            {
                ax += a[i * n + j] * rs.x[j];
            }
            REQUIRE(ax > l[i] - 1e-7);
            REQUIRE(ax < u[i] + 1e-7);
        }
        ++agree;
    }
    CHECK(agree >= instances * 9 / 10); // the IPM resolves >= 90% of the scan
}

TEST_CASE("lp: bit-identical determinism (run twice)", "[hesap-opt][lp][determinism]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    const crd::f64 c[] = {-1.0, -2.0, 0.5};
    const crd::f64 a[] = {1.0, 1.0, 1.0, 1.0, -1.0, 0.0};
    const crd::f64 l[] = {-kInf, -1.0};
    const crd::f64 u[] = {5.0, 2.0};
    const crd::f64 xlo[] = {0.0, 0.0, 0.0};
    const crd::f64 xup[] = {4.0, 4.0, 4.0};
    const opt::LpProblem<crd::f64> prob{{c, 3}, {a, 6}, {l, 2}, {u, 2}, {xlo, 3}, {xup, 3}, 3, 2};

    const opt::LpResult<crd::f64> s1 = opt::solve_lp_simplex(prob, &alloc);
    const opt::LpResult<crd::f64> s2 = opt::solve_lp_simplex(prob, &alloc);
    REQUIRE(s1.status == opt::QpStatus::Solved);
    REQUIRE(s1.status == s2.status);
    REQUIRE(s1.iterations == s2.iterations);
    for (crd::usize j = 0; j < 3; ++j)
    {
        REQUIRE(s1.x[j] == s2.x[j]); // bit-identical
    }
    for (crd::usize i = 0; i < 2; ++i)
    {
        REQUIRE(s1.y[i] == s2.y[i]);
    }

    const opt::LpResult<crd::f64> i1 = opt::solve_lp_mehrotra(prob, &alloc);
    const opt::LpResult<crd::f64> i2 = opt::solve_lp_mehrotra(prob, &alloc);
    REQUIRE(i1.status == i2.status);
    for (crd::usize j = 0; j < 3; ++j)
    {
        REQUIRE(i1.x[j] == i2.x[j]);
    }
}
