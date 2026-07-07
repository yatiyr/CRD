// test_ode_adjoint.cpp — Phase 3.1.6 v16-f: revolve checkpointing + the DTO/CTO ODE-adjoint honesty split.
// Gates: (1) the revolve schedule is VALID (every step reversed exactly once in decreasing order, working state at the
// step's input, ≤ snaps checkpoints) and GW-OPTIMAL (recompute count == the DP minimum); (2) the DTO gradient (AD
// through the discrete RK4) ≡ central FD of the discrete loss — EXACT; (3) revolve-checkpointed DTO == store-all DTO,
// BIT-IDENTICAL, at O(snaps) memory; (4) the CTO continuous adjoint is a valid approximation (close to DTO) but NOT
// exact — DTO matches FD far tighter than CTO does (the honesty split).

#include <crd/hesap/autodiff/ode_adjoint.hpp>

#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>

namespace rev = crd::hesap::autodiff::reverse;
using crd::f64;
using Catch::Matchers::WithinAbs;

namespace
{
// nonlinear parametric RHS (2 states, 2 params) — scalar-generic (f64 forward / Var taped):
//   x0' = θ0·x1 − 0.5·x0² ;  x1' = −θ1·x0 + 0.3·sin(x1)
struct OdeF
{
    template <class T>
    void operator()(f64 /*t*/, const T* x, const T* theta, T* dx, int /*d*/, int /*np*/) const
    {
        using crd::math::sin;
        dx[0] = theta[0] * x[1] - 0.5 * x[0] * x[0];
        dx[1] = -theta[1] * x[0] + 0.3 * sin(x[1]);
    }
};
} // namespace

TEST_CASE("v16-f: revolve schedule is valid + GW-optimal", "[autodiff][reverse][revolve]")
{
    const int cases[][2] = {{1, 1}, {2, 1}, {7, 2}, {20, 3}, {50, 4}, {100, 5}};
    for (const auto& c : cases)
    {
        const int nt = c[0];
        const int snaps = c[1];
        INFO("T=" << nt << " snaps=" << snaps);
        // caller-owned DP tables
        static crd::i64 costbuf[101 * 6];
        static int      argbuf[101 * 6];
        rev::RevolvePlan plan(nt, snaps, costbuf, argbuf);

        int  pos = 0;
        int  maxslot = -1;
        int  recompute = 0;
        int  expect_rev = nt - 1;
        int  slot_step[16] = {};
        bool valid = true;
        rev::revolve(
            plan, nt, snaps,
            [&](int from, int to) { if (pos != from) { valid = false; } recompute += (to - from); pos = to; },
            [&](int slot) { slot_step[slot] = pos; if (slot > maxslot) { maxslot = slot; } },
            [&](int slot) { pos = slot_step[slot]; },
            [&](int step) { if (step != expect_rev || pos != step) { valid = false; } --expect_rev; });
        CHECK(valid);                                        // reverse order T−1..0, state at each step, advances from pos
        CHECK(expect_rev == -1);                             // every step reversed exactly once
        CHECK(maxslot < snaps);                              // ≤ snaps checkpoints
        CHECK(static_cast<crd::i64>(recompute) == plan.recompute(nt, snaps)); // GW-optimal recompute
    }
    // O(log T) memory: 5 checkpoints reverse 100 steps with bounded, small recompute.
    static crd::i64 cb[101 * 6];
    static int      ab[101 * 6];
    rev::RevolvePlan p(100, 5, cb, ab);
    CHECK(p.recompute(100, 5) < 100 * 6); // far below the O(T²) of naive 1-checkpoint recompute
}

TEST_CASE("v16-f: DTO gradient (AD through RK4) == central FD; revolve == store-all bit-identical",
          "[autodiff][reverse][ode]")
{
    constexpr int              d = 2;
    constexpr int              np = 2;
    constexpr int              nsteps = 20;
    const f64                  h = 0.05;
    crd::memory::TlsfAllocator alloc(8 << 20);
    rev::Tape                  tape(&alloc);
    rev::Var                   vscr[7 * d + np];
    f64                        x0[d] = {1.0, 0.5};
    f64                        theta[np] = {0.8, 1.2};
    f64                        loss_grad[d] = {1.0, 0.5};

    f64 x_all[(nsteps + 1) * d];
    f64 fscr[5 * d];
    f64 xbar[d];
    f64 xbar_next[d];
    f64 dto_xbar0[d];
    f64 dto_tbar[np];
    rev::dto_gradient(OdeF{}, x0, theta, d, np, nsteps, h, loss_grad, dto_xbar0, dto_tbar, x_all, fscr, xbar, xbar_next,
                      tape, vscr);

    // central FD of the discrete loss L = loss_grad·x_T
    auto fwd_loss = [&](const f64* xx0, const f64* th) -> f64
    {
        f64 x[d];
        f64 sc[5 * d];
        f64 xn[d];
        for (int i = 0; i < d; ++i) { x[i] = xx0[i]; }
        for (int k = 0; k < nsteps; ++k) { rev::rk4_step<f64>(OdeF{}, x, th, k * h, h, xn, d, np, sc); for (int i = 0; i < d; ++i) { x[i] = xn[i]; } }
        f64 loss = 0.0;
        for (int i = 0; i < d; ++i) { loss += loss_grad[i] * x[i]; }
        return loss;
    };
    const f64 hh = 1e-6;
    for (int j = 0; j < np; ++j)
    {
        f64 th[np] = {theta[0], theta[1]};
        th[j]      = theta[j] + hh;
        const f64 fp = fwd_loss(x0, th);
        th[j]        = theta[j] - hh;
        const f64 fm = fwd_loss(x0, th);
        CHECK_THAT(dto_tbar[j], WithinAbs((fp - fm) / (2.0 * hh), 1e-6));
    }
    for (int i = 0; i < d; ++i)
    {
        f64 xx[d] = {x0[0], x0[1]};
        xx[i]     = x0[i] + hh;
        const f64 fp = fwd_loss(xx, theta);
        xx[i]        = x0[i] - hh;
        const f64 fm = fwd_loss(xx, theta);
        CHECK_THAT(dto_xbar0[i], WithinAbs((fp - fm) / (2.0 * hh), 1e-6));
    }

    // revolve-checkpointed DTO == store-all DTO, BIT-IDENTICAL, for several snaps counts
    for (int snaps : {2, 3, 5})
    {
        static crd::i64 cb[(nsteps + 1) * 6];
        static int      ab[(nsteps + 1) * 6];
        rev::RevolvePlan plan(nsteps, snaps, cb, ab);
        f64 ckpt[5 * d];
        f64 work[d];
        f64 xnext[d];
        f64 rxbar[d];
        f64 rxbar_next[d];
        f64 rfscr[5 * d];
        f64 rev_xbar0[d];
        f64 rev_tbar[np];
        rev::dto_gradient_revolve(OdeF{}, x0, theta, d, np, nsteps, h, loss_grad, rev_xbar0, rev_tbar, snaps, plan, ckpt,
                                  work, xnext, rxbar, rxbar_next, rfscr, tape, vscr);
        for (int i = 0; i < d; ++i) { CHECK(rev_xbar0[i] == dto_xbar0[i]); }
        for (int j = 0; j < np; ++j) { CHECK(rev_tbar[j] == dto_tbar[j]); }
    }
}

TEST_CASE("v16-f: CTO continuous adjoint is a valid approximation; DTO is the EXACT one (honesty split)",
          "[autodiff][reverse][ode]")
{
    constexpr int              d = 2;
    constexpr int              np = 2;
    constexpr int              nsteps = 40;
    const f64                  h = 0.025; // fine step: RK4 accurate, CTO close to DTO
    crd::memory::TlsfAllocator alloc(8 << 20);
    rev::Tape                  tape(&alloc);
    rev::Var                   vscr[7 * d + np];
    f64                        x0[d] = {1.0, 0.5};
    f64                        theta[np] = {0.8, 1.2};
    f64                        loss_grad[d] = {1.0, 0.5};

    f64 x_all[(nsteps + 1) * d];
    f64 fscr[5 * d];
    f64 xbar[d];
    f64 xbar_next[d];
    f64 dto_xbar0[d];
    f64 dto_tbar[np];
    rev::dto_gradient(OdeF{}, x0, theta, d, np, nsteps, h, loss_grad, dto_xbar0, dto_tbar, x_all, fscr, xbar, xbar_next,
                      tape, vscr);

    f64 x_all2[(nsteps + 1) * d];
    f64 fscr2[5 * d];
    f64 lam[d];
    f64 xi[d];
    f64 gth[np];
    f64 cto_xbar0[d];
    f64 cto_tbar[np];
    rev::cto_gradient(OdeF{}, x0, theta, d, np, nsteps, h, loss_grad, cto_xbar0, cto_tbar, x_all2, fscr2, lam, xi, gth,
                      tape, vscr);

    // FD reference (the truth for the DISCRETE solve)
    auto fwd_loss = [&](const f64* th) -> f64
    {
        f64 x[d];
        f64 sc[5 * d];
        f64 xn[d];
        for (int i = 0; i < d; ++i) { x[i] = x0[i]; }
        for (int k = 0; k < nsteps; ++k) { rev::rk4_step<f64>(OdeF{}, x, th, k * h, h, xn, d, np, sc); for (int i = 0; i < d; ++i) { x[i] = xn[i]; } }
        f64 loss = 0.0;
        for (int i = 0; i < d; ++i) { loss += loss_grad[i] * x[i]; }
        return loss;
    };
    const f64 hh = 1e-6;
    for (int j = 0; j < np; ++j)
    {
        f64 th[np] = {theta[0], theta[1]};
        th[j]      = theta[j] + hh;
        const f64 fp = fwd_loss(th);
        th[j]        = theta[j] - hh;
        const f64 fm = fwd_loss(th);
        const f64 fd = (fp - fm) / (2.0 * hh);
        CHECK_THAT(dto_tbar[j], WithinAbs(fd, 1e-6)); // DTO is EXACT (consistent with the discrete forward)
        CHECK_THAT(cto_tbar[j], WithinAbs(fd, 5e-3)); // CTO is only APPROXIMATE (a valid continuous adjoint)
    }
    for (int i = 0; i < d; ++i) { CHECK_THAT(cto_xbar0[i], WithinAbs(dto_xbar0[i], 5e-3)); }
}
