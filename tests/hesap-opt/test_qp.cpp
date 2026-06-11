// crd-hesap-opt v7-k — QP: OSQP-class ADMM · Mehrotra IPM · Goldfarb-Idnani dual active-set, all on the
// canonical two-sided form with the shared OSQP-sign duals. Validates: (1) analytic instances — the box
// projection (x* = clamp, duals = the residual with the right SIGNS) and an equality-only QP checked against
// the v7-j KKT solve; (2) the FULL KKT certificate (stationarity / primal feasibility / dual signs /
// complementarity) on EVERY solver output; (3) the CROSS-ADJUDICATION gate — three INDEPENDENT algorithms
// agreeing to tight tolerance on a Philox-generated family of strictly convex QPs (feasible by construction;
// mixed equality/two-sided/one-sided rows) — an agreement that cannot co-occur with an algorithmic bug in any
// one of them; (4) certified infeasibility: primal (contradictory rows: ADMM certificate + GI dual-unbounded)
// and dual (unbounded objective: ADMM certificate); (5) determinism — adaptive-ρ ADMM bit-identical across
// runs and across jobs worker counts (serial solver; the contract the sparse backend will inherit);
// (6) boundaries m = 0 (unconstrained: all three reduce to −P⁻¹q) and n = 0.

#include <crd/hesap/opt/opt.hpp>
#include <crd/hesap/stats/philox.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <limits>

namespace opt = crd::hesap::opt;
namespace st = crd::hesap::stats;

namespace
{
constexpr crd::f64 kInf = std::numeric_limits<crd::f64>::infinity();

// The full KKT certificate for min ½xᵀPx + qᵀx s.t. l ≤ Ax ≤ u with OSQP-sign duals.
void check_qp_kkt(const opt::QpProblem<crd::f64>& prob, const opt::QpResult<crd::f64>& r, crd::f64 tol)
{
    const crd::usize n = prob.n;
    const crd::usize m = prob.m;
    // Stationarity: Px + q + Aᵀy = 0.
    for (crd::usize j = 0; j < n; ++j)
    {
        crd::f64 acc = prob.q[j];
        for (crd::usize k = 0; k < n; ++k)
        {
            acc += prob.p[j * n + k] * r.x[k];
        }
        for (crd::usize i = 0; i < m; ++i)
        {
            acc += prob.a[i * n + j] * r.y[i];
        }
        CHECK(std::fabs(acc) < tol);
    }
    for (crd::usize i = 0; i < m; ++i)
    {
        crd::f64 ax = 0.0;
        for (crd::usize j = 0; j < n; ++j)
        {
            ax += prob.a[i * n + j] * r.x[j];
        }
        CHECK(ax >= prob.l[i] - tol); // primal feasibility
        CHECK(ax <= prob.u[i] + tol);
        if (prob.l[i] != prob.u[i]) // complementarity as the PRODUCT (the right form for interior points,
        {                           // whose iterates satisfy |y|·dist ≈ μ with both factors individually small)
            if (r.y[i] > tol)
            {
                CHECK(std::fabs(r.y[i] * (ax - prob.u[i])) < 100.0 * tol); // y > 0 pairs with the u side
            }
            else if (r.y[i] < -tol)
            {
                CHECK(std::fabs(r.y[i] * (ax - prob.l[i])) < 100.0 * tol); // y < 0 pairs with the l side
            }
        }
    }
}
} // namespace

TEST_CASE("v7-k QP: box projection (analytic solution + dual signs), all three solvers", "[hesap][opt][v7]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    const crd::usize n = 3;
    // min ½‖x − c‖² ⇒ P = I, q = −c; box −1 ≤ x_i ≤ 1 via A = I.
    const crd::f64 p[] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    const crd::f64 q[] = {-2.0, 3.0, -0.5}; // c = (2, −3, 0.5)
    const crd::f64 a[] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    const crd::f64 l[] = {-1, -1, -1};
    const crd::f64 u[] = {1, 1, 1};
    const opt::QpProblem<crd::f64> prob{{p, 9}, {q, 3}, {a, 9}, {l, 3}, {u, 3}, n, 3};
    const crd::f64 xstar[] = {1.0, -1.0, 0.5};
    const crd::f64 ystar[] = {1.0, -2.0, 0.0}; // y = c − x*: active-upper positive, active-lower negative

    auto verify = [&](const opt::QpResult<crd::f64>& r, crd::f64 tol)
    {
        REQUIRE(r.status == opt::QpStatus::Solved);
        for (crd::usize i = 0; i < n; ++i)
        {
            CHECK(std::fabs(r.x[i] - xstar[i]) < tol);
            CHECK(std::fabs(r.y[i] - ystar[i]) < tol);
        }
        check_qp_kkt(prob, r, tol);
    };
    verify(opt::solve_qp_admm<crd::f64>(prob, {}, &alloc), 1e-7);
    verify(opt::solve_qp_mehrotra<crd::f64>(prob, {}, &alloc), 1e-7);
    verify(opt::solve_qp_goldfarb_idnani<crd::f64>(prob, &alloc), 1e-10);
}

TEST_CASE("v7-k QP: equality-constrained QP matches the v7-j KKT solve, all three solvers", "[hesap][opt][v7]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    const crd::usize n = 3;
    const crd::f64 p[] = {2, 0, 0, 0, 2, 0, 0, 0, 2}; // ‖x‖² ⇒ P = 2I
    const crd::f64 q[] = {0, 0, 0};
    const crd::f64 a[] = {1, 1, 1, 1, -1, 0}; // x+y+z = 3; x−y = 1 (both equality rows)
    const crd::f64 l[] = {3, 1};
    const crd::f64 u[] = {3, 1};
    const opt::QpProblem<crd::f64> prob{{p, 9}, {q, 3}, {a, 6}, {l, 2}, {u, 2}, n, 2};

    // Reference via the v7-j saddle solve (from x0 = 0: p_step = x*).
    crd::containers::Array<crd::f64> xr(&alloc);
    crd::containers::Array<crd::f64> lam(&alloc);
    xr.resize(n);
    lam.resize(2);
    const crd::f64 c[] = {-3, -1}; // A_E x + c = 0
    const auto ks =
        opt::solve_kkt_dense<crd::f64>(&alloc, {p, 9}, {a, 6}, {q, 3}, {c, 2}, {xr.data(), n}, {lam.data(), 2});
    REQUIRE(ks.solved);

    auto verify = [&](const opt::QpResult<crd::f64>& r, crd::f64 tol)
    {
        REQUIRE(r.status == opt::QpStatus::Solved);
        for (crd::usize i = 0; i < n; ++i)
        {
            CHECK(std::fabs(r.x[i] - xr[i]) < tol);
        }
        CHECK(std::fabs(r.x[0] + r.x[1] + r.x[2] - 3.0) < tol);
        check_qp_kkt(prob, r, tol);
    };
    verify(opt::solve_qp_admm<crd::f64>(prob, {}, &alloc), 1e-7);
    verify(opt::solve_qp_mehrotra<crd::f64>(prob, {}, &alloc), 1e-7);
    verify(opt::solve_qp_goldfarb_idnani<crd::f64>(prob, &alloc), 1e-9);
}

TEST_CASE("v7-k QP cross-adjudication: three independent algorithms agree on a Philox family", "[hesap][opt][v7]")
{
    crd::memory::TlsfAllocator alloc(1U << 24);
    const crd::usize n = 8;
    const crd::usize m = 12;
    st::PhiloxRng rng(/*seed=*/20240610);
    auto uni = [&]() -> crd::f64
    {
        return 2.0 * rng.next_f64() - 1.0;
    };

    for (int inst = 0; inst < 10; ++inst)
    {
        // P = BᵀB + I (PD); q random; A random; bounds around a random x0 ⇒ FEASIBLE BY CONSTRUCTION.
        crd::containers::Array<crd::f64> b(&alloc);
        crd::containers::Array<crd::f64> p(&alloc);
        crd::containers::Array<crd::f64> q(&alloc);
        crd::containers::Array<crd::f64> a(&alloc);
        crd::containers::Array<crd::f64> l(&alloc);
        crd::containers::Array<crd::f64> u(&alloc);
        crd::containers::Array<crd::f64> x0(&alloc);
        b.resize(n * n);
        p.resize(n * n);
        q.resize(n);
        a.resize(m * n);
        l.resize(m);
        u.resize(m);
        x0.resize(n);
        for (crd::usize k = 0; k < n * n; ++k)
        {
            b[k] = uni();
        }
        for (crd::usize i = 0; i < n; ++i)
        {
            for (crd::usize j = 0; j < n; ++j)
            {
                crd::f64 acc = i == j ? 1.0 : 0.0;
                for (crd::usize k = 0; k < n; ++k)
                {
                    acc += b[k * n + i] * b[k * n + j];
                }
                p[i * n + j] = acc;
            }
            q[i] = uni();
            x0[i] = uni();
        }
        for (crd::usize k = 0; k < m * n; ++k)
        {
            a[k] = uni();
        }
        for (crd::usize i = 0; i < m; ++i)
        {
            crd::f64 ax0 = 0.0;
            for (crd::usize j = 0; j < n; ++j)
            {
                ax0 += a[i * n + j] * x0[j];
            }
            if (i < 2) // two equality rows
            {
                l[i] = ax0;
                u[i] = ax0;
            }
            else if (i == 2) // one upper-only row
            {
                l[i] = -kInf;
                u[i] = ax0 + 0.1 + rng.next_f64();
            }
            else if (i == 3) // one lower-only row
            {
                l[i] = ax0 - 0.1 - rng.next_f64();
                u[i] = kInf;
            }
            else // two-sided
            {
                l[i] = ax0 - 0.1 - rng.next_f64();
                u[i] = ax0 + 0.1 + rng.next_f64();
            }
        }
        const opt::QpProblem<crd::f64> prob{
            {p.data(), n * n}, {q.data(), n}, {a.data(), m * n}, {l.data(), m}, {u.data(), m}, n, m};

        const auto r_gi = opt::solve_qp_goldfarb_idnani<crd::f64>(prob, &alloc);
        const auto r_ipm = opt::solve_qp_mehrotra<crd::f64>(prob, {}, &alloc);
        const auto r_admm = opt::solve_qp_admm<crd::f64>(prob, {}, &alloc);
        CHECK(r_gi.dual_res < 1e-7); // per-solver certificates (the expansion prints the value on failure)
        CHECK(r_gi.primal_res < 1e-7);
        CHECK(r_ipm.dual_res < 1e-6);
        CHECK(r_ipm.primal_res < 1e-6);
        CHECK(r_admm.dual_res < 1e-5);
        CHECK(r_admm.primal_res < 1e-5);
        REQUIRE(r_gi.status == opt::QpStatus::Solved);
        REQUIRE(r_ipm.status == opt::QpStatus::Solved);
        REQUIRE(r_admm.status == opt::QpStatus::Solved);
        check_qp_kkt(prob, r_gi, 1e-7);
        check_qp_kkt(prob, r_ipm, 1e-6);
        check_qp_kkt(prob, r_admm, 1e-5);
        for (crd::usize j = 0; j < n; ++j) // three independent algorithms, one answer
        {
            CHECK(std::fabs(r_gi.x[j] - r_ipm.x[j]) < 1e-5);
            CHECK(std::fabs(r_gi.x[j] - r_admm.x[j]) < 1e-5);
        }
        CHECK(std::fabs(r_gi.obj - r_ipm.obj) < 1e-6);
        CHECK(std::fabs(r_gi.obj - r_admm.obj) < 1e-4); // first-order + polish: obj error ~ ∇f·‖Δx‖ ≈ 1e-5
    }
}

// KEPT as a permanent property test (SANITY #3): this scanner found the GI J-transpose bug at n = 2 — a bug
// invisible to every diagonal-P test and to the n=8 family only via aggregate disagreement. Tiny instances
// localize; 400 of them run in milliseconds.
TEST_CASE("v7-k GI property scan: 400 tiny instances vs the IPM", "[hesap][opt][v7]")
{
    crd::memory::TlsfAllocator alloc(1U << 24);
    const crd::usize n = 2;
    const crd::usize m = 3;
    st::PhiloxRng rng(99);
    auto uni = [&]() -> crd::f64
    {
        return 2.0 * rng.next_f64() - 1.0;
    };
    for (int inst = 0; inst < 400; ++inst)
    {
        crd::containers::Array<crd::f64> b(&alloc);
        crd::containers::Array<crd::f64> p(&alloc);
        crd::containers::Array<crd::f64> q(&alloc);
        crd::containers::Array<crd::f64> a(&alloc);
        crd::containers::Array<crd::f64> l(&alloc);
        crd::containers::Array<crd::f64> u(&alloc);
        crd::containers::Array<crd::f64> x0(&alloc);
        b.resize(n * n);
        p.resize(n * n);
        q.resize(n);
        a.resize(m * n);
        l.resize(m);
        u.resize(m);
        x0.resize(n);
        for (crd::usize k = 0; k < n * n; ++k)
        {
            b[k] = uni();
        }
        for (crd::usize i = 0; i < n; ++i)
        {
            for (crd::usize j = 0; j < n; ++j)
            {
                crd::f64 acc = i == j ? 1.0 : 0.0;
                for (crd::usize k = 0; k < n; ++k)
                {
                    acc += b[k * n + i] * b[k * n + j];
                }
                p[i * n + j] = acc;
            }
            q[i] = uni();
            x0[i] = uni();
        }
        for (crd::usize k = 0; k < m * n; ++k)
        {
            a[k] = uni();
        }
        for (crd::usize i = 0; i < m; ++i)
        {
            crd::f64 ax0 = 0.0;
            for (crd::usize j = 0; j < n; ++j)
            {
                ax0 += a[i * n + j] * x0[j];
            }
            if (i == 0)
            {
                l[i] = ax0;
                u[i] = ax0;
            }
            else
            {
                l[i] = ax0 - 0.05 - 0.2 * rng.next_f64();
                u[i] = ax0 + 0.05 + 0.2 * rng.next_f64();
            }
        }
        const opt::QpProblem<crd::f64> prob{
            {p.data(), n * n}, {q.data(), n}, {a.data(), m * n}, {l.data(), m}, {u.data(), m}, n, m};
        const auto r_gi = opt::solve_qp_goldfarb_idnani<crd::f64>(prob, &alloc);
        const auto r_ipm = opt::solve_qp_mehrotra<crd::f64>(prob, {}, &alloc);
        if (r_gi.status == opt::QpStatus::Solved && r_ipm.status == opt::QpStatus::Solved)
        {
            crd::f64 dx = 0.0;
            for (crd::usize j = 0; j < n; ++j)
            {
                dx = std::max(dx, std::fabs(r_gi.x[j] - r_ipm.x[j]));
            }
            if (dx > 1e-5 || r_gi.dual_res > 1e-7)
            {
                UNSCOPED_INFO("inst=" << inst << " dx=" << dx << " gi.dres=" << r_gi.dual_res);
                UNSCOPED_INFO("P=[" << p[0] << "," << p[1] << ";" << p[2] << "," << p[3] << "] q=[" << q[0] << ","
                                    << q[1] << "]");
                UNSCOPED_INFO("A row0=[" << a[0] << "," << a[1] << "] l=" << l[0] << " u=" << u[0]);
                UNSCOPED_INFO("A row1=[" << a[2] << "," << a[3] << "] l=" << l[1] << " u=" << u[1]);
                UNSCOPED_INFO("A row2=[" << a[4] << "," << a[5] << "] l=" << l[2] << " u=" << u[2]);
                UNSCOPED_INFO("gi.x=[" << r_gi.x[0] << "," << r_gi.x[1] << "] gi.y=[" << r_gi.y[0] << "," << r_gi.y[1]
                                       << "," << r_gi.y[2] << "]");
                UNSCOPED_INFO("ipm.x=[" << r_ipm.x[0] << "," << r_ipm.x[1] << "] ipm.y=[" << r_ipm.y[0] << ","
                                        << r_ipm.y[1] << "," << r_ipm.y[2] << "]");
                FAIL("first GI mismatch found");
            }
        }
    }
    SUCCEED();
}

TEST_CASE("v7-k QP: certified infeasibility", "[hesap][opt][v7]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    {
        // PRIMAL infeasible: x ≥ 1 and x ≤ 0 on the same scalar variable.
        const crd::f64 p[] = {1.0};
        const crd::f64 q[] = {0.0};
        const crd::f64 a[] = {1.0, 1.0};
        const crd::f64 l[] = {1.0, -kInf};
        const crd::f64 u[] = {kInf, 0.0};
        const opt::QpProblem<crd::f64> prob{{p, 1}, {q, 1}, {a, 2}, {l, 2}, {u, 2}, 1, 2};
        const auto r_admm = opt::solve_qp_admm<crd::f64>(prob, {}, &alloc);
        CHECK(r_admm.status == opt::QpStatus::PrimalInfeasible);
        const auto r_gi = opt::solve_qp_goldfarb_idnani<crd::f64>(prob, &alloc);
        CHECK(r_gi.status == opt::QpStatus::PrimalInfeasible);
        const auto r_ipm = opt::solve_qp_mehrotra<crd::f64>(prob, {}, &alloc);
        CHECK(r_ipm.status != opt::QpStatus::Solved); // honest scope: the IPM detects only non-convergence
    }
    {
        // DUAL infeasible (unbounded below): P = 0, q = −1, unconstrained.
        const crd::f64 p[] = {0.0};
        const crd::f64 q[] = {-1.0};
        const opt::QpProblem<crd::f64> prob{{p, 1},
                                            {q, 1},
                                            {static_cast<const crd::f64*>(nullptr), 0},
                                            {static_cast<const crd::f64*>(nullptr), 0},
                                            {static_cast<const crd::f64*>(nullptr), 0},
                                            1,
                                            0};
        const auto r_admm = opt::solve_qp_admm<crd::f64>(prob, {}, &alloc);
        CHECK(r_admm.status == opt::QpStatus::DualInfeasible);
    }
}

TEST_CASE("v7-k QP determinism: adaptive-rho ADMM bit-identical across runs + worker counts", "[hesap][opt][v7][moat]")
{
    crd::memory::TlsfAllocator alloc(1U << 24);
    const crd::usize n = 6;
    const crd::usize m = 8;
    st::PhiloxRng rng(/*seed=*/7);
    auto uni = [&]() -> crd::f64
    {
        return 2.0 * rng.next_f64() - 1.0;
    };
    crd::containers::Array<crd::f64> b(&alloc);
    crd::containers::Array<crd::f64> p(&alloc);
    crd::containers::Array<crd::f64> q(&alloc);
    crd::containers::Array<crd::f64> a(&alloc);
    crd::containers::Array<crd::f64> l(&alloc);
    crd::containers::Array<crd::f64> u(&alloc);
    b.resize(n * n);
    p.resize(n * n);
    q.resize(n);
    a.resize(m * n);
    l.resize(m);
    u.resize(m);
    for (crd::usize k = 0; k < n * n; ++k)
    {
        b[k] = uni();
    }
    for (crd::usize i = 0; i < n; ++i)
    {
        for (crd::usize j = 0; j < n; ++j)
        {
            crd::f64 acc = i == j ? 0.5 : 0.0;
            for (crd::usize k = 0; k < n; ++k)
            {
                acc += b[k * n + i] * b[k * n + j];
            }
            p[i * n + j] = acc;
        }
        q[i] = uni();
    }
    for (crd::usize k = 0; k < m * n; ++k)
    {
        a[k] = uni();
    }
    for (crd::usize i = 0; i < m; ++i)
    {
        l[i] = -0.5 - rng.next_f64();
        u[i] = 0.5 + rng.next_f64();
    }
    const opt::QpProblem<crd::f64> prob{
        {p.data(), n * n}, {q.data(), n}, {a.data(), m * n}, {l.data(), m}, {u.data(), m}, n, m};

    opt::QpAdmmOptions<crd::f64> aopts; // adaptive ρ ON — the rule is deterministic by design
    const auto r1 = opt::solve_qp_admm<crd::f64>(prob, aopts, &alloc);
    const auto r2 = opt::solve_qp_admm<crd::f64>(prob, aopts, &alloc);
    REQUIRE(r1.status == opt::QpStatus::Solved);
    REQUIRE(r1.iterations == r2.iterations);
    for (crd::usize j = 0; j < n; ++j)
    {
        CHECK(r1.x[j] == r2.x[j]); // bit-identical, adaptive ρ included
    }

    // Worker-count loop: the dense QP solvers are SERIAL (the contract the sparse backend inherits) — the
    // trajectory must not depend on the jobs configuration at all.
    for (crd::u32 nw : {1U, 2U, 4U, 8U, 16U})
    {
        crd::jobs::Config cfg;
        cfg.num_threads = nw;
        crd::jobs::init(cfg);
        {
            const auto r = opt::solve_qp_admm<crd::f64>(prob, aopts, &alloc);
            bool ident = r.iterations == r1.iterations;
            for (crd::usize j = 0; j < n && ident; ++j)
            {
                ident = (r.x[j] == r1.x[j]);
            }
            CHECK(ident);
        }
        crd::jobs::shutdown();
    }
}

TEST_CASE("v7-k QP boundaries: m = 0 (unconstrained) and n = 0", "[hesap][opt][v7]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    {
        const crd::f64 p[] = {2, 0, 0, 2}; // x* = −P⁻¹q = (1, 2)
        const crd::f64 q[] = {-2, -4};
        const opt::QpProblem<crd::f64> prob{{p, 4},
                                            {q, 2},
                                            {static_cast<const crd::f64*>(nullptr), 0},
                                            {static_cast<const crd::f64*>(nullptr), 0},
                                            {static_cast<const crd::f64*>(nullptr), 0},
                                            2,
                                            0};
        const auto ra = opt::solve_qp_admm<crd::f64>(prob, {}, &alloc);
        const auto rm = opt::solve_qp_mehrotra<crd::f64>(prob, {}, &alloc);
        const auto rg = opt::solve_qp_goldfarb_idnani<crd::f64>(prob, &alloc);
        REQUIRE(ra.status == opt::QpStatus::Solved);
        REQUIRE(rm.status == opt::QpStatus::Solved);
        REQUIRE(rg.status == opt::QpStatus::Solved);
        CHECK(std::fabs(ra.x[0] - 1.0) < 1e-6);
        CHECK(std::fabs(rm.x[1] - 2.0) < 1e-7);
        CHECK(std::fabs(rg.x[0] - 1.0) < 1e-12);
        CHECK(std::fabs(rg.x[1] - 2.0) < 1e-12);
    }
    {
        const opt::QpProblem<crd::f64> prob{}; // n = m = 0
        const auto ra = opt::solve_qp_admm<crd::f64>(prob, {}, &alloc);
        const auto rg = opt::solve_qp_goldfarb_idnani<crd::f64>(prob, &alloc);
        CHECK(ra.status == opt::QpStatus::Solved);
        CHECK(rg.status == opt::QpStatus::Solved);
    }
}
