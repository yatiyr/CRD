// v7-m — CONIC PROGRAMMING (SCS-class operator splitting). Gates: (1) LP-as-conic reproduces the v7-l
// analytic LP including the EXACT duals (the SCS dual convention coincides with the OSQP sign on nonneg
// rows); (2) an analytic SOCP (linear objective over a norm ball: x* = p - r*c/||c||, y* = (||c||, c) checked
// against the closed form); (3) an analytic 2x2 SDP (min x s.t. [[x,1],[1,x]] PSD -> x* = 1, dual
// Y* = [[.5,-.5],[-.5,.5]], complementarity YS = 0); (4) mixed Zero+Nonneg cones vs the LP simplex;
// (5) certified primal/dual infeasibility; (6) a Philox cross-adjudication scan vs the v7-l simplex
// (THIRD independent algorithm family on the same instances); (7) bit-identical determinism.

#include <crd/hesap/opt/conic.hpp>
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

TEST_CASE("conic: LP-as-conic reproduces the analytic LP and its duals", "[hesap-opt][conic]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    // min -x - 2y s.t. x+y<=4, x<=3, y<=2, x>=0, y>=0 — all as nonneg rows (s = b - Ax >= 0).
    // Optimum (2,2), obj -6, y* = (1, 0, 1, 0, 0) (same as the v7-l dual gate).
    const crd::f64 c[] = {-1.0, -2.0};
    const crd::f64 a[] = {1.0, 1.0, 1.0, 0.0, 0.0, 1.0, -1.0, 0.0, 0.0, -1.0};
    const crd::f64 b[] = {4.0, 3.0, 2.0, 0.0, 0.0};
    const opt::ConeDesc cones[] = {{opt::ConeType::Nonneg, 5}};
    const opt::ConicProblem<crd::f64> prob{{c, 2}, {a, 10}, {b, 5}, {cones, 1}, 2, 5};

    const opt::ConicResult<crd::f64> r = opt::solve_conic_admm(prob, &alloc);
    REQUIRE(r.status == opt::QpStatus::Solved);
    CHECK(std::fabs(r.x[0] - 2.0) < 1e-5);
    CHECK(std::fabs(r.x[1] - 2.0) < 1e-5);
    CHECK(std::fabs(r.obj - (-6.0)) < 1e-5);
    const crd::f64 ystar[] = {1.0, 0.0, 1.0, 0.0, 0.0};
    for (crd::usize i = 0; i < 5; ++i)
    {
        CHECK(std::fabs(r.y[i] - ystar[i]) < 1e-5);
    }
    // The conic KKT directly: A^T y + c = 0, s in K, y in K*, s^T y ~ 0.
    crd::f64 comp = 0.0;
    for (crd::usize i = 0; i < 5; ++i)
    {
        CHECK(r.s[i] > -1e-6);
        CHECK(r.y[i] > -1e-6);
        comp += r.s[i] * r.y[i];
    }
    CHECK(std::fabs(comp) < 1e-4);
    CHECK(r.primal_res < 1e-5);
    CHECK(r.dual_res < 1e-5);
    CHECK(r.gap < 1e-4);
}

TEST_CASE("conic: analytic SOCP (linear objective over a norm ball)", "[hesap-opt][conic]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    // min c^T x s.t. ||x - p|| <= r with p = (1, 2), r = 0.5, c = (1, 1).
    // Closed form: x* = p - r*c/||c||, obj* = c^T p - r*||c||, y* = (||c||, c) = (sqrt2, 1, 1).
    // SCS form: s = b - Ax = (r, x - p) in SOC(3): A = [0 0; -1 0; 0 -1], b = (0.5, -1, -2).
    const crd::f64 c[] = {1.0, 1.0};
    const crd::f64 a[] = {0.0, 0.0, -1.0, 0.0, 0.0, -1.0};
    const crd::f64 b[] = {0.5, -1.0, -2.0};
    const opt::ConeDesc cones[] = {{opt::ConeType::Soc, 3}};
    const opt::ConicProblem<crd::f64> prob{{c, 2}, {a, 6}, {b, 3}, {cones, 1}, 2, 3};

    const opt::ConicResult<crd::f64> r = opt::solve_conic_admm(prob, &alloc);
    REQUIRE(r.status == opt::QpStatus::Solved);
    const crd::f64 root2 = std::sqrt(2.0);
    const crd::f64 xstar0 = 1.0 - 0.5 / root2;
    const crd::f64 xstar1 = 2.0 - 0.5 / root2;
    CHECK(std::fabs(r.x[0] - xstar0) < 1e-5);
    CHECK(std::fabs(r.x[1] - xstar1) < 1e-5);
    CHECK(std::fabs(r.obj - (3.0 - 0.5 * root2)) < 1e-5);
    CHECK(std::fabs(r.y[0] - root2) < 1e-4);
    CHECK(std::fabs(r.y[1] - 1.0) < 1e-4);
    CHECK(std::fabs(r.y[2] - 1.0) < 1e-4);
    // s* = (r, x* - p) sits ON the cone boundary: s0 = ||s_1:2||.
    const crd::f64 sn = std::sqrt(r.s[1] * r.s[1] + r.s[2] * r.s[2]);
    CHECK(std::fabs(r.s[0] - sn) < 1e-5);
}

TEST_CASE("conic: analytic 2x2 SDP", "[hesap-opt][conic]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    // min x s.t. M(x) = [[x, 1], [1, x]] PSD  ->  x* = 1 (det = x^2 - 1 >= 0, x >= 0).
    // Full-matrix vectorization: s = vec(M) = b - A x with A = (-1, 0, 0, -1)^T, b = (0, 1, 1, 0).
    // Dual: Y* = [[.5, -.5], [-.5, .5]] (PSD, trace-compl YS = 0, A^T y + c = 0).
    const crd::f64 c[] = {1.0};
    const crd::f64 a[] = {-1.0, 0.0, 0.0, -1.0};
    const crd::f64 b[] = {0.0, 1.0, 1.0, 0.0};
    const opt::ConeDesc cones[] = {{opt::ConeType::Psd, 2}};
    const opt::ConicProblem<crd::f64> prob{{c, 1}, {a, 4}, {b, 4}, {cones, 1}, 1, 4};

    const opt::ConicResult<crd::f64> r = opt::solve_conic_admm(prob, &alloc);
    REQUIRE(r.status == opt::QpStatus::Solved);
    CHECK(std::fabs(r.x[0] - 1.0) < 1e-5);
    CHECK(std::fabs(r.obj - 1.0) < 1e-5);
    CHECK(std::fabs(r.y[0] - 0.5) < 1e-4);
    CHECK(std::fabs(r.y[3] - 0.5) < 1e-4);
    CHECK(std::fabs(r.y[1] + 0.5) < 1e-4); // off-diagonals -1/2
    CHECK(std::fabs(r.y[2] + 0.5) < 1e-4);
    // s* = vec([[1,1],[1,1]]) — the PSD boundary (eigenvalues 0 and 2).
    for (crd::usize i = 0; i < 4; ++i)
    {
        CHECK(std::fabs(r.s[i] - 1.0) < 1e-5);
    }
}

TEST_CASE("conic: mixed Zero + Nonneg cones vs the LP simplex", "[hesap-opt][conic]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    // min x + 2y s.t. x + y = 1 (Zero row), x >= 0, y >= 0 (Nonneg rows). Optimum (1, 0), obj 1.
    const crd::f64 c[] = {1.0, 2.0};
    const crd::f64 a[] = {1.0, 1.0, -1.0, 0.0, 0.0, -1.0};
    const crd::f64 b[] = {1.0, 0.0, 0.0};
    const opt::ConeDesc cones[] = {{opt::ConeType::Zero, 1}, {opt::ConeType::Nonneg, 2}};
    const opt::ConicProblem<crd::f64> prob{{c, 2}, {a, 6}, {b, 3}, {cones, 2}, 2, 3};

    const opt::ConicResult<crd::f64> r = opt::solve_conic_admm(prob, &alloc);
    REQUIRE(r.status == opt::QpStatus::Solved);
    CHECK(std::fabs(r.x[0] - 1.0) < 1e-5);
    CHECK(std::fabs(r.x[1]) < 1e-5);
    CHECK(std::fabs(r.s[0]) < 1e-6); // the Zero block is pinned exactly by the projection

    // The same LP through the v7-l simplex must agree.
    const crd::f64 al[] = {1.0, 1.0};
    const crd::f64 ll[] = {1.0};
    const crd::f64 ul[] = {1.0};
    const crd::f64 xlo[] = {0.0, 0.0};
    const crd::f64 xup[] = {kInf, kInf};
    const opt::LpProblem<crd::f64> lp{{c, 2}, {al, 2}, {ll, 1}, {ul, 1}, {xlo, 2}, {xup, 2}, 2, 1};
    const opt::LpResult<crd::f64> rs = opt::solve_lp_simplex(lp, &alloc);
    REQUIRE(rs.status == opt::QpStatus::Solved);
    CHECK(std::fabs(r.obj - rs.obj) < 1e-5);
}

TEST_CASE("conic: certified primal and dual infeasibility", "[hesap-opt][conic]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    SECTION("primal infeasible (x <= 1 and x >= 2 as nonneg rows)")
    {
        const crd::f64 c[] = {1.0};
        const crd::f64 a[] = {1.0, -1.0};
        const crd::f64 b[] = {1.0, -2.0};
        const opt::ConeDesc cones[] = {{opt::ConeType::Nonneg, 2}};
        const opt::ConicProblem<crd::f64> prob{{c, 1}, {a, 2}, {b, 2}, {cones, 1}, 1, 2};
        const opt::ConicResult<crd::f64> r = opt::solve_conic_admm(prob, &alloc);
        CHECK(r.status == opt::QpStatus::PrimalInfeasible);
    }
    SECTION("dual infeasible (min -x, x >= 1: an unbounded ray)")
    {
        const crd::f64 c[] = {-1.0};
        const crd::f64 a[] = {-1.0};
        const crd::f64 b[] = {-1.0};
        const opt::ConeDesc cones[] = {{opt::ConeType::Nonneg, 1}};
        const opt::ConicProblem<crd::f64> prob{{c, 1}, {a, 1}, {b, 1}, {cones, 1}, 1, 1};
        const opt::ConicResult<crd::f64> r = opt::solve_conic_admm(prob, &alloc);
        CHECK(r.status == opt::QpStatus::DualInfeasible);
    }
}

TEST_CASE("conic: cross-adjudication vs the LP simplex (Philox scan)", "[hesap-opt][conic]")
{
    crd::memory::TlsfAllocator alloc(1U << 24);
    // The THIRD independent algorithm family (operator splitting) against the combinatorial simplex on
    // random boxed-feasible LPs in conic form (rows l <= Ax <= u become the nonneg blocks
    // [u - Ax; Ax - l] >= 0, the box becomes [xup - x; x - xlo] >= 0).
    st::PhiloxRng rng(/*seed=*/0xC0DEU, /*stream=*/0U);
    constexpr crd::usize n = 3;
    constexpr crd::usize m = 4;
    constexpr crd::usize mc = 2 * m + 2 * n; // conic rows
    crd::usize solved = 0;
    constexpr crd::usize instances = 25;
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
        // Conic assembly: rows [A; -A; I; -I], b = (u, -l, xup, -xlo), one nonneg block.
        crd::f64 ac[mc * n];
        crd::f64 bc[mc];
        for (crd::usize i = 0; i < m; ++i)
        {
            for (crd::usize j = 0; j < n; ++j)
            {
                ac[i * n + j] = a[i * n + j];
                ac[(m + i) * n + j] = -a[i * n + j];
            }
            bc[i] = u[i];
            bc[m + i] = -l[i];
        }
        for (crd::usize j = 0; j < n; ++j)
        {
            for (crd::usize k = 0; k < n; ++k)
            {
                ac[(2 * m + j) * n + k] = j == k ? 1.0 : 0.0;
                ac[(2 * m + n + j) * n + k] = j == k ? -1.0 : 0.0;
            }
            bc[2 * m + j] = xup[j];
            bc[2 * m + n + j] = -xlo[j];
        }
        const opt::ConeDesc cones[] = {{opt::ConeType::Nonneg, mc}};
        const opt::ConicProblem<crd::f64> cprob{{c, n}, {ac, mc * n}, {bc, mc}, {cones, 1}, n, mc};
        const opt::LpProblem<crd::f64> lp{{c, n}, {a, m * n}, {l, m}, {u, m}, {xlo, n}, {xup, n}, n, m};

        const opt::LpResult<crd::f64> rs = opt::solve_lp_simplex(lp, &alloc);
        REQUIRE(rs.status == opt::QpStatus::Solved);
        const opt::ConicResult<crd::f64> rc = opt::solve_conic_admm(cprob, &alloc);
        if (rc.status != opt::QpStatus::Solved)
        {
            continue; // counted below; the simplex stays the hard gate
        }
        const crd::f64 scale = 1.0 + std::fabs(rs.obj);
        REQUIRE(std::fabs(rs.obj - rc.obj) / scale < 1e-4);
        ++solved;
    }
    CHECK(solved >= instances * 9 / 10);
}

TEST_CASE("conic: bit-identical determinism (run twice)", "[hesap-opt][conic][determinism]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    const crd::f64 c[] = {1.0, 1.0};
    const crd::f64 a[] = {0.0, 0.0, -1.0, 0.0, 0.0, -1.0};
    const crd::f64 b[] = {0.5, -1.0, -2.0};
    const opt::ConeDesc cones[] = {{opt::ConeType::Soc, 3}};
    const opt::ConicProblem<crd::f64> prob{{c, 2}, {a, 6}, {b, 3}, {cones, 1}, 2, 3};

    const opt::ConicResult<crd::f64> r1 = opt::solve_conic_admm(prob, &alloc);
    const opt::ConicResult<crd::f64> r2 = opt::solve_conic_admm(prob, &alloc);
    REQUIRE(r1.status == r2.status);
    REQUIRE(r1.iterations == r2.iterations);
    for (crd::usize j = 0; j < 2; ++j)
    {
        REQUIRE(r1.x[j] == r2.x[j]); // bit-identical
    }
    for (crd::usize i = 0; i < 3; ++i)
    {
        REQUIRE(r1.y[i] == r2.y[i]);
        REQUIRE(r1.s[i] == r2.s[i]);
    }
}
