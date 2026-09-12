#pragma once

// autodiff_fd_functors.hpp — SHARED finite-difference witness functors for the CEIR autodiff device gates (25b-4b, 25c-2). A
// scalar functor `f(x, n) -> scalar` fed to `crd::hesap::autodiff::testing::grad_fd` (central difference) so a device-computed
// gradient is cross-validated against a DIALECT-INDEPENDENT reference. Hoisted here (from the per-backend pipeline TUs) so the two
// backends share ONE definition — the functors are PURE (f64 / templated arithmetic + hesap forward), no backend dependency.
//
// ⛔ MlpLoss's forward is HESAP (nn::matmul + nn::relu on f64), NOT hand-written — a hand-written forward would repeat the device's
// own row-major indexing, so a shared mistake would pass; grad_fd instantiates ONLY f64 (central difference), so a plain-double
// operator is the honest signature. SumGemmAA stays templated (it predates the hesap forward and has no hesap equivalent to call).

#include <crd/hesap/autodiff/nn_reverse.hpp> // MlpLoss forward: nn::matmul + nn::relu

#include <crd/core/types.hpp> // crd::usize

namespace crd::ceir::gpu_test
{
// L(A) = sum(gemm(A, A)) for a SQUARE A[side,side] — the 25b-4b FD witness (both operands are A, so the reverse pass sums two
// operand adjoints). Templated: grad_fd calls it on f64; the analytic ref is hesap matmul_vjp.
struct SumGemmAA
{
    int side = 0;
    template <class S> S operator()(const S* x, int /*n*/) const
    {
        S acc = S(0);
        for (int i = 0; i < side; ++i)
        {
            for (int j = 0; j < side; ++j)
            {
                S c = S(0);
                for (int p = 0; p < side; ++p) { c += x[i * side + p] * x[p * side + j]; }
                acc += c;
            }
        }
        return acc;
    }
};

// L(W) = sum(M ⊙ mlp(xa, W1, W2)) with W = [W1 (d0·d1); W2 (d1·d2)] flattened — the 25c-2 FD witness on grads[0]=dW1, grads[1]=dW2.
// The forward is HESAP (no hand-written MLP — see the header note); grad_fd instantiates only f64, so a plain-double operator.
struct MlpLoss
{
    const double* xa   = nullptr; // input activation [m*d0]
    const double* mask = nullptr; // dLoss mask M [m*d2]
    int           m    = 0;
    int           d0   = 0;
    int           d1   = 0;
    int           d2   = 0;
    static constexpr int kCap = 64; // ⛔ the interior/output scratch cap: m*d1 AND m*d2 must be <= kCap. A caller exceeding it gets a
                                    //    VISIBLY-WRONG 0.0 (the FD cross-check fails HARD) — never a silent stack overflow (the
                                    //    "typed reject, never silent" doctrine, one level up from the baked-32 scar). Widen kCap here.
    double operator()(const double* w, int /*n*/) const
    {
        namespace nnr = crd::hesap::autodiff::reverse::nn;
        if (m * d1 > kCap || m * d2 > kCap) { return 0.0; } // out-of-cap ⇒ deliberately wrong (fails the FD gate loudly)
        const double* w1 = w;                                    // [d0*d1]
        const double* w2 = w + static_cast<crd::usize>(d0 * d1); // [d1*d2]
        double        z1[kCap];
        double        h1[kCap];
        double        z2[kCap];
        nnr::matmul(xa, w1, z1, m, d0, d1); // z1 = xa · W1  [m,d1]
        nnr::relu(z1, h1, m * d1);          // h1 = relu(z1)
        nnr::matmul(h1, w2, z2, m, d1, d2); // z2 = h1 · W2  [m,d2]  (no final activation)
        double acc = 0.0;
        for (int i = 0; i < m * d2; ++i) { acc += mask[i] * z2[i]; } // L = <M, out>
        return acc;
    }
};
} // namespace crd::ceir::gpu_test
