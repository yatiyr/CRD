// test_neural_ode.cpp -- Phase 3.1.6 v16-k: NEURAL ODE. The RHS is a tiny MLP f_theta; training fits the flow map of a
// true damped-spiral ODE by minimising Sigma_k ||ODE_theta(x0_k -> T) - xT_k||^2, with the parameter gradient by the
// v16-f DISCRETIZE-THEN-OPTIMIZE adjoint (AD through the RK4 integrator) and a FIXED-ORDER batch fold (v16-i moat).
// Gate: training reduces the loss, and the whole run replays BIT-FOR-BIT (deterministic-training moat on a real ML
// task -- torch/torchdiffeq cannot). ADR-0097.

#include <crd/hesap/autodiff/ode_adjoint.hpp>

#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cmath>

namespace rev = crd::hesap::autodiff::reverse;
using crd::f64;

namespace
{
constexpr int kHid = 8;               // hidden width
constexpr int kDim   = 2;               // state dim
constexpr int kNp  = kHid * kDim + kHid + kDim * kHid + kDim; // 42 params

// neural-ODE RHS: dx = W2 * tanh(W1 x + b1) + b2. theta = [W1(hxd), b1(h), W2(dxh), b2(d)] flattened.
struct NnRhs
{
    template <class T>
    void operator()(f64 /*t*/, const T* x, const T* th, T* dx, int d, int /*np*/) const
    {
        using crd::math::tanh; // ADL picks rev::tanh for Var, crd::math::tanh for f64
        const T* w1 = th;
        const T* b1 = th + kHid * d;
        const T* w2 = th + kHid * d + kHid;
        const T* b2 = th + kHid * d + kHid + d * kHid;
        T        hid[kHid];
        for (int j = 0; j < kHid; ++j)
        {
            T s = b1[j];
            for (int i = 0; i < d; ++i) { s = s + w1[j * d + i] * x[i]; }
            hid[j] = tanh(s);
        }
        for (int i = 0; i < d; ++i)
        {
            T s = b2[i];
            for (int j = 0; j < kHid; ++j) { s = s + w2[i * kHid + j] * hid[j]; }
            dx[i] = s;
        }
    }
};

// true damped spiral dx/dt = A x, A = [[-0.1,-2],[2,-0.1]].
struct TrueRhs
{
    void operator()(f64 /*t*/, const f64* x, const f64* /*th*/, f64* dx, int /*d*/, int /*np*/) const
    {
        dx[0] = -0.1 * x[0] - 1.0 * x[1];
        dx[1] = 1.0 * x[0] - 0.1 * x[1];
    }
};
} // namespace

TEST_CASE("v16-k: neural ODE trains via the DTO adjoint, reduces loss, replays bit-for-bit", "[autodiff][neuralode]")
{
    constexpr int              nb = 12; // batch of trajectories
    constexpr int              nt = 20; // RK4 steps
    const f64                  h  = 0.05;
    crd::memory::TlsfAllocator alloc(32 << 20);
    rev::Tape                  tape(&alloc);

    // data: x0_k -> xT_k under the true ODE
    f64 x0[nb * kDim];
    f64 xt[nb * kDim];
    for (int k = 0; k < nb; ++k)
    {
        f64 x[kDim] = {std::sin(1.0 + k), std::cos(0.5 + k)};
        x0[k * kDim + 0] = x[0];
        x0[k * kDim + 1] = x[1];
        f64 sc[5 * kDim];
        f64 xn[kDim];
        for (int s = 0; s < nt; ++s) { rev::rk4_step<f64>(TrueRhs{}, x, static_cast<const f64*>(nullptr), s * h, h, xn, kDim, 0, sc); x[0] = xn[0]; x[1] = xn[1]; }
        xt[k * kDim + 0] = x[0];
        xt[k * kDim + 1] = x[1];
    }

    // one full training run (SGD over the DTO batch gradient), returns the loss trace end + final weights.
    rev::Var vscr[7 * kDim + kNp];
    const auto run = [&](f64* theta_out, f64* loss_first, f64* loss_last)
    {
        f64 theta[kNp];
        for (int i = 0; i < kNp; ++i) { theta[i] = 0.2 * std::sin(0.3 + i); } // deterministic init
        for (int epoch = 0; epoch < 150; ++epoch)
        {
            f64 grad[kNp] = {};
            f64 loss      = 0.0;
            for (int k = 0; k < nb; ++k) // FIXED sample order -> deterministic fold
            {
                f64 xpred[kDim] = {x0[k * kDim + 0], x0[k * kDim + 1]};
                f64 sc[5 * kDim];
                f64 xn[kDim];
                for (int s = 0; s < nt; ++s) { rev::rk4_step<f64>(NnRhs{}, xpred, theta, s * h, h, xn, kDim, kNp, sc); xpred[0] = xn[0]; xpred[1] = xn[1]; }
                f64 lg[kDim];
                for (int i = 0; i < kDim; ++i) { const f64 e = xpred[i] - xt[k * kDim + i]; lg[i] = 2.0 * e; loss += e * e; }
                f64 xall[(nt + 1) * kDim];
                f64 fscr[5 * kDim];
                f64 xb[kDim];
                f64 xbn[kDim];
                f64 xbar0[kDim];
                f64 tbar[kNp];
                rev::dto_gradient(NnRhs{}, x0 + k * kDim, theta, kDim, kNp, nt, h, lg, xbar0, tbar, xall, fscr, xb, xbn, tape, vscr);
                for (int i = 0; i < kNp; ++i) { grad[i] += tbar[i]; }
            }
            if (epoch == 0) { *loss_first = loss; }
            *loss_last = loss;
            for (int i = 0; i < kNp; ++i) { theta[i] -= 0.05 * grad[i] / static_cast<f64>(nb); }
        }
        for (int i = 0; i < kNp; ++i) { theta_out[i] = theta[i]; }
    };

    f64 w1[kNp];
    f64 w2[kNp];
    f64 lf1 = 0.0;
    f64 ll1 = 0.0;
    f64 lf2 = 0.0;
    f64 ll2 = 0.0;
    run(w1, &lf1, &ll1);
    run(w2, &lf2, &ll2);
    CHECK(ll1 < 0.5 * lf1);                       // training more than halved the fit loss
    CHECK(ll1 < lf1);                             // (and it strictly decreased)
    for (int i = 0; i < kNp; ++i) { CHECK(w1[i] == w2[i]); } // BIT-identical replay (the moat)
    CHECK(lf1 == lf2);
    CHECK(ll1 == ll2);
}
