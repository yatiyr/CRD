// test_nn_reverse.cpp — Phase 3.1.6 v16-c: the tensor/NN VJP layer makes a 2-layer MLP trainable. The gate is the
// torch-style numerical gradcheck: EVERY parameter gradient from ONE backward pass (matmul/bias/relu/softmax-CE VJPs
// composed) matches a central finite difference. Backprop yields all params' gradients in one pass; FD needs 2·#params
// forward evals — the reason networks are trainable by reverse mode.

#include <crd/hesap/autodiff/nn_reverse.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>

namespace nn = crd::hesap::autodiff::reverse::nn;
using crd::f64;
using Catch::Matchers::WithinAbs;

namespace
{
// NOLINTBEGIN(readability-identifier-naming) -- standard ML notation (B=batch, D=in-dim, H=hidden, C=classes; and
// X/W/Z/A/P activations + gWeight gradients below) is deliberately kept over lower_case for math readability.
constexpr int B = 4;
constexpr int D = 3;
constexpr int H = 5;
constexpr int C = 2;
f64           gX_in[B * D];
int           gLabels[B];
// NOLINTEND(readability-identifier-naming)

void init_inputs()
{
    for (int i = 0; i < B * D; ++i) { gX_in[i] = 0.4 * std::sin(1.0 + i * 1.3); }
    for (int i = 0; i < B; ++i) { gLabels[i] = i % C; }
}

// forward: loss = CE(softmax(relu(X·W1+b1)·W2+b2), labels)
f64 forward(const f64* w1, const f64* b1, const f64* w2, const f64* b2)
{
    f64 z1[B * H];
    f64 a1[B * H];
    f64 z2[B * C];
    f64 probs[B * C];
    nn::matmul(gX_in, w1, z1, B, D, H);
    nn::bias_add(z1, b1, z1, B, H);
    nn::relu(z1, a1, B * H);
    nn::matmul(a1, w2, z2, B, H, C);
    nn::bias_add(z2, b2, z2, B, C);
    return nn::softmax_cross_entropy(z2, gLabels, probs, B, C);
}

// backward: all parameter gradients in ONE pass.
void backward(const f64* w1, const f64* b1, const f64* w2, const f64* b2, f64* gw1, f64* gb1, f64* gw2, f64* gb2)
{
    f64 z1[B * H];
    f64 a1[B * H];
    f64 z2[B * C];
    f64 probs[B * C];
    nn::matmul(gX_in, w1, z1, B, D, H);
    nn::bias_add(z1, b1, z1, B, H);
    nn::relu(z1, a1, B * H);
    nn::matmul(a1, w2, z2, B, H, C);
    nn::bias_add(z2, b2, z2, B, C);
    (void)nn::softmax_cross_entropy(z2, gLabels, probs, B, C); // only `probs` needed for the VJP

    f64 gz2[B * C];
    f64 gpre2[B * C];
    f64 ga1[B * H];
    f64 gz1[B * H];
    f64 gpre1[B * H];
    f64 gx[B * D];
    nn::softmax_cross_entropy_vjp(probs, gLabels, gz2, B, C);
    nn::bias_add_vjp(gz2, gpre2, gb2, B, C);
    nn::matmul_vjp(a1, w2, gpre2, ga1, gw2, B, H, C);
    nn::relu_vjp(z1, ga1, gz1, B * H);
    nn::bias_add_vjp(gz1, gpre1, gb1, B, H);
    nn::matmul_vjp(gX_in, w1, gpre1, gx, gw1, B, D, H);
}

// central-FD gradcheck of one parameter array against its analytic gradient.
void gradcheck(f64* p, int np, const f64* gan, const f64* w1, const f64* b1, const f64* w2, const f64* b2)
{
    const f64 h = 1e-6;
    for (int i = 0; i < np; ++i)
    {
        const f64 saved = p[i];
        p[i]            = saved + h;
        const f64 fp    = forward(w1, b1, w2, b2);
        p[i]            = saved - h;
        const f64 fm    = forward(w1, b1, w2, b2);
        p[i]            = saved;
        CHECK_THAT(gan[i], WithinAbs((fp - fm) / (2.0 * h), 1e-5));
    }
}
} // namespace

TEST_CASE("v16-c: MLP parameter gradients (one backward pass) == numerical gradcheck", "[autodiff][reverse][nn]")
{
    init_inputs();
    f64 w1[D * H];
    f64 b1[H];
    f64 w2[H * C];
    f64 b2[C];
    for (int i = 0; i < D * H; ++i) { w1[i] = 0.3 * std::cos(1.0 + i); }
    for (int i = 0; i < H; ++i) { b1[i] = 0.1 * (i - 2); }
    for (int i = 0; i < H * C; ++i) { w2[i] = 0.25 * std::sin(0.5 + i); }
    for (int i = 0; i < C; ++i) { b2[i] = 0.05 * i; }

    f64 gw1[D * H];
    f64 gb1[H];
    f64 gw2[H * C];
    f64 gb2[C];
    backward(w1, b1, w2, b2, gw1, gb1, gw2, gb2);

    // torch-style gradcheck: every parameter gradient matches central FD.
    gradcheck(w1, D * H, gw1, w1, b1, w2, b2);
    gradcheck(b1, H, gb1, w1, b1, w2, b2);
    gradcheck(w2, H * C, gw2, w1, b1, w2, b2);
    gradcheck(b2, C, gb2, w1, b1, w2, b2);
}

TEST_CASE("v16-c: matmul VJP == einsum-with-permuted-spec, and is deterministic", "[autodiff][reverse][nn]")
{
    // C = A·B ; ḡA = ḡC·Bᵀ ; ḡB = Aᵀ·ḡC — check against a direct index computation + run-to-run bit-identity.
    constexpr int m = 3;
    constexpr int k = 4;
    constexpr int p = 2;
    f64           a[m * k];
    f64           b[k * p];
    f64           gc[m * p];
    for (int i = 0; i < m * k; ++i) { a[i] = 0.2 + 0.1 * i; }
    for (int i = 0; i < k * p; ++i) { b[i] = -0.3 + 0.15 * i; }
    for (int i = 0; i < m * p; ++i) { gc[i] = 0.5 - 0.2 * i; }
    f64 ga[m * k];
    f64 gb[k * p];
    f64 ga2[m * k];
    f64 gb2[k * p];
    nn::matmul_vjp(a, b, gc, ga, gb, m, k, p);
    nn::matmul_vjp(a, b, gc, ga2, gb2, m, k, p);
    // ḡA[i,t] = Σ_j ḡC[i,j]·B[t,j]
    for (int i = 0; i < m; ++i)
    {
        for (int t = 0; t < k; ++t)
        {
            f64 s = 0.0;
            for (int j = 0; j < p; ++j) { s += gc[i * p + j] * b[t * p + j]; }
            CHECK_THAT(ga[i * k + t], WithinAbs(s, 1e-12));
            CHECK(ga[i * k + t] == ga2[i * k + t]); // deterministic
        }
    }
    // ḡB[t,j] = Σ_i A[i,t]·ḡC[i,j]
    for (int t = 0; t < k; ++t)
    {
        for (int j = 0; j < p; ++j)
        {
            f64 s = 0.0;
            for (int i = 0; i < m; ++i) { s += a[i * k + t] * gc[i * p + j]; }
            CHECK_THAT(gb[t * p + j], WithinAbs(s, 1e-12));
            CHECK(gb[t * p + j] == gb2[t * p + j]);
        }
    }
}

// ============================================================================================================
// v16-c coverage gates — every remaining v14-m VJP is the transpose of its forward, so ONE backward pass yields
// the parameter gradients that a central finite difference reproduces (torch-gradcheck protocol). The headline is
// the full CNN: conv → relu → maxpool → linear → softmax-CE backprops ALL params in one pass.
// ============================================================================================================
namespace
{
// Central-FD gradcheck: perturb each x[i], compare the analytic gradient gan[i] to (loss(+h)−loss(−h))/2h.
template <class Loss>
void fd_gradcheck(f64* x, int n, const f64* gan, Loss loss, f64 tol = 1e-5, f64 hstep = 1e-6)
{
    for (int i = 0; i < n; ++i)
    {
        const f64 saved = x[i];
        x[i]            = saved + hstep;
        const f64 fp    = loss();
        x[i]            = saved - hstep;
        const f64 fm    = loss();
        x[i]            = saved;
        CHECK_THAT(gan[i], WithinAbs((fp - fm) / (2.0 * hstep), tol));
    }
}
} // namespace

TEST_CASE("v16-c: elementwise activation VJPs (gelu/tanh/sigmoid/softmax) == FD", "[autodiff][reverse][nn]")
{
    constexpr int n = 7;
    f64           x[n];
    f64           c[n];
    for (int i = 0; i < n; ++i)
    {
        x[i] = 0.6 * std::sin(0.7 + 1.1 * i);
        c[i] = 0.4 * std::cos(0.3 + 0.9 * i); // the upstream adjoint ḡy
    }
    SECTION("gelu")
    {
        f64 gx[n];
        nn::gelu_vjp(x, c, gx, n);
        auto loss = [&] { f64 y[n]; nn::gelu(x, y, n); f64 s = 0; for (int i = 0; i < n; ++i) { s += c[i] * y[i]; } return s; };
        fd_gradcheck(x, n, gx, loss);
    }
    SECTION("tanh")
    {
        f64 y[n];
        f64 gx[n];
        nn::tanh_act(x, y, n);
        nn::tanh_vjp(y, c, gx, n);
        auto loss = [&] { f64 yy[n]; nn::tanh_act(x, yy, n); f64 s = 0; for (int i = 0; i < n; ++i) { s += c[i] * yy[i]; } return s; };
        fd_gradcheck(x, n, gx, loss);
    }
    SECTION("sigmoid")
    {
        f64 y[n];
        f64 gx[n];
        nn::sigmoid(x, y, n);
        nn::sigmoid_vjp(y, c, gx, n);
        auto loss = [&] { f64 yy[n]; nn::sigmoid(x, yy, n); f64 s = 0; for (int i = 0; i < n; ++i) { s += c[i] * yy[i]; } return s; };
        fd_gradcheck(x, n, gx, loss);
    }
    SECTION("softmax (row-wise)")
    {
        constexpr int rows = 3;
        constexpr int cols = 4;
        f64           xs[rows * cols];
        f64           cs[rows * cols];
        for (int i = 0; i < rows * cols; ++i)
        {
            xs[i] = 0.5 * std::sin(0.2 + 0.8 * i);
            cs[i] = 0.3 * std::cos(1.1 + 0.6 * i);
        }
        f64 y[rows * cols];
        f64 gx[rows * cols];
        nn::softmax(xs, y, rows, cols);
        nn::softmax_vjp(y, cs, gx, rows, cols);
        auto loss = [&] { f64 yy[rows * cols]; nn::softmax(xs, yy, rows, cols); f64 s = 0; for (int i = 0; i < rows * cols; ++i) { s += cs[i] * yy[i]; } return s; };
        fd_gradcheck(xs, rows * cols, gx, loss);
    }
}

TEST_CASE("v16-c: LayerNorm VJP (x, gamma, beta) == FD", "[autodiff][reverse][nn]")
{
    constexpr int rows = 3;
    constexpr int d = 5;
    const f64     eps = 1e-5;
    f64           x[rows * d];
    f64           gamma[d];
    f64           beta[d];
    f64           c[rows * d];
    for (int i = 0; i < rows * d; ++i)
    {
        x[i] = 0.7 * std::sin(0.4 + 1.3 * i);
        c[i] = 0.35 * std::cos(0.9 + 0.7 * i);
    }
    for (int j = 0; j < d; ++j)
    {
        gamma[j] = 0.8 + 0.2 * std::sin(1.0 + j);
        beta[j]  = 0.1 * (j - 2);
    }
    f64 gx[rows * d];
    f64 ggamma[d];
    f64 gbeta[d];
    nn::layernorm_vjp(x, gamma, c, eps, gx, ggamma, gbeta, rows, d);
    auto loss = [&] { f64 y[rows * d]; nn::layernorm(x, gamma, beta, eps, y, rows, d); f64 s = 0; for (int i = 0; i < rows * d; ++i) { s += c[i] * y[i]; } return s; };
    fd_gradcheck(x, rows * d, gx, loss);
    fd_gradcheck(gamma, d, ggamma, loss);
    fd_gradcheck(beta, d, gbeta, loss);
}

TEST_CASE("v16-c: pooling VJPs (max/avg) == FD", "[autodiff][reverse][nn]")
{
    constexpr int planes = 2;
    constexpr int h = 4;
    constexpr int w = 4;
    constexpr int k = 2;
    constexpr int s = 2;
    const int     oh = (h - k) / s + 1;
    const int     ow = (w - k) / s + 1;
    f64           x[planes * h * w];
    f64           c[planes * oh * ow];
    for (int i = 0; i < planes * h * w; ++i) { x[i] = std::sin(0.3 + 0.37 * i); } // distinct → no pooling ties
    for (int i = 0; i < planes * oh * ow; ++i) { c[i] = 0.4 * std::cos(0.6 + 0.9 * i); }
    SECTION("max_pool")
    {
        f64 gx[planes * h * w];
        nn::max_pool_vjp(x, c, gx, planes, h, w, k, s);
        auto loss = [&] { f64 y[planes * oh * ow]; nn::max_pool(x, y, planes, h, w, k, s); f64 sm = 0; for (int i = 0; i < planes * oh * ow; ++i) { sm += c[i] * y[i]; } return sm; };
        fd_gradcheck(x, planes * h * w, gx, loss);
    }
    SECTION("avg_pool")
    {
        f64 gx[planes * h * w];
        nn::avg_pool_vjp(c, gx, planes, h, w, k, s);
        auto loss = [&] { f64 y[planes * oh * ow]; nn::avg_pool(x, y, planes, h, w, k, s); f64 sm = 0; for (int i = 0; i < planes * oh * ow; ++i) { sm += c[i] * y[i]; } return sm; };
        fd_gradcheck(x, planes * h * w, gx, loss);
    }
}

// NOLINTBEGIN(readability-identifier-naming) -- standard ML notation (X/W/Z/A/P activations, convW/convB weights, gWeight
// gradients, OC/N/Cls dims) is deliberately kept over lower_case for math readability in these CNN backprop tests.
TEST_CASE("v16-c: conv2d VJP (x, w, bias) == FD", "[autodiff][reverse][nn]")
{
    constexpr int n = 2;
    constexpr int chans = 2;
    constexpr int hin = 4;
    constexpr int win = 4;
    constexpr int OC = 3;
    constexpr int kh = 3;
    constexpr int kw = 3;
    constexpr int pad = 1;
    constexpr int stride = 1;
    const int     oh = (hin + 2 * pad - kh) / stride + 1;
    const int     ow = (win + 2 * pad - kw) / stride + 1;
    const int     ckk = chans * kh * kw;
    const int     ohw = oh * ow;
    f64           x[n * chans * hin * win];
    f64           w[OC * chans * kh * kw];
    f64           bias[OC];
    f64           c[n * OC * ohw];
    for (int i = 0; i < n * chans * hin * win; ++i) { x[i] = 0.5 * std::sin(0.2 + 0.6 * i); }
    for (int i = 0; i < OC * chans * kh * kw; ++i) { w[i] = 0.3 * std::cos(0.5 + 0.4 * i); }
    for (int i = 0; i < OC; ++i) { bias[i] = 0.05 * (i - 1); }
    for (int i = 0; i < n * OC * ohw; ++i) { c[i] = 0.25 * std::sin(1.0 + 0.3 * i); }
    f64 col[ckk * ohw];
    f64 gcol[ckk * ohw];
    f64 gx[n * chans * hin * win];
    f64 gw[OC * chans * kh * kw];
    f64 gb[OC];
    nn::conv2d_vjp(x, w, c, gx, gw, gb, n, chans, hin, win, OC, kh, kw, pad, stride, col, gcol);
    auto loss = [&] { f64 y[n * OC * ohw]; f64 col2[ckk * ohw]; nn::conv2d(x, w, bias, y, n, chans, hin, win, OC, kh, kw, pad, stride, col2); f64 sm = 0; for (int i = 0; i < n * OC * ohw; ++i) { sm += c[i] * y[i]; } return sm; };
    fd_gradcheck(x, n * chans * hin * win, gx, loss);
    fd_gradcheck(w, OC * chans * kh * kw, gw, loss);
    fd_gradcheck(bias, OC, gb, loss);
}

TEST_CASE("v16-c: full CNN (conv->relu->maxpool->linear->softmax-CE) backprops ALL params in ONE pass == FD",
          "[autodiff][reverse][nn]")
{
    constexpr int N = 2;
    constexpr int chans = 2;
    constexpr int hin = 4;
    constexpr int win = 4;
    constexpr int OC = 3;
    constexpr int kh = 3;
    constexpr int kw = 3;
    constexpr int pad = 1;
    constexpr int stride = 1;
    constexpr int oh = 4;
    constexpr int ow = 4;                 // conv output (3x3 s1 p1 keeps size)
    constexpr int k = 2;
    constexpr int s = 2;                   // 2x2/s2 maxpool
    constexpr int oh2 = (oh - k) / s + 1;
    constexpr int ow2 = (ow - k) / s + 1;
    constexpr int flat = OC * oh2 * ow2;          // 12 — the flattened pooled feature
    constexpr int Cls  = 2;                        // classes

    f64 X[N * chans * hin * win];
    f64 convW[OC * chans * kh * kw];
    f64 convB[OC];
    f64 W2[flat * Cls];
    f64 b2[Cls];
    int labels[N];
    for (int i = 0; i < N * chans * hin * win; ++i) { X[i] = 0.5 * std::sin(0.4 + 0.7 * i); }
    for (int i = 0; i < OC * chans * kh * kw; ++i) { convW[i] = 0.3 * std::cos(0.6 + 0.5 * i); }
    for (int i = 0; i < OC; ++i) { convB[i] = 0.1 * (i - 1); }
    for (int i = 0; i < flat * Cls; ++i) { W2[i] = 0.2 * std::sin(0.3 + 0.4 * i); }
    for (int i = 0; i < Cls; ++i) { b2[i] = 0.05 * i; }
    for (int i = 0; i < N; ++i) { labels[i] = i % Cls; }

    // forward → scalar CE loss (recomputed inside FD; all intermediates are call-local stack scratch)
    auto forward = [&]() -> f64
    {
        f64 col[(chans * kh * kw) * (oh * ow)];
        f64 convY[N * OC * oh * ow];
        f64 A[N * OC * oh * ow];
        f64 P[N * OC * oh2 * ow2];
        f64 Z2[N * Cls];
        f64 probs[N * Cls];
        nn::conv2d(X, convW, convB, convY, N, chans, hin, win, OC, kh, kw, pad, stride, col);
        nn::relu(convY, A, N * OC * oh * ow);
        nn::max_pool(A, P, N * OC, oh, ow, k, s);         // P is [N, flat] contiguous (planes = N*OC)
        nn::matmul(P, W2, Z2, N, flat, Cls);
        nn::bias_add(Z2, b2, Z2, N, Cls);
        return nn::softmax_cross_entropy(Z2, labels, probs, N, Cls);
    };

    // backward: every parameter gradient in ONE pass.
    f64 gConvW[OC * chans * kh * kw];
    f64 gConvB[OC];
    f64 gW2[flat * Cls];
    f64 gb2[Cls];
    {
        f64 col[(chans * kh * kw) * (oh * ow)];
        f64 gcol[(chans * kh * kw) * (oh * ow)];
        f64 convY[N * OC * oh * ow];
        f64 A[N * OC * oh * ow];
        f64 P[N * OC * oh2 * ow2];
        f64 Z2[N * Cls];
        f64 probs[N * Cls];
        nn::conv2d(X, convW, convB, convY, N, chans, hin, win, OC, kh, kw, pad, stride, col);
        nn::relu(convY, A, N * OC * oh * ow);
        nn::max_pool(A, P, N * OC, oh, ow, k, s);
        nn::matmul(P, W2, Z2, N, flat, Cls);
        nn::bias_add(Z2, b2, Z2, N, Cls);
        (void)nn::softmax_cross_entropy(Z2, labels, probs, N, Cls);

        f64 gZ2[N * Cls];
        f64 gpre2[N * Cls];
        f64 gP[N * OC * oh2 * ow2];
        f64 gA[N * OC * oh * ow];
        f64 gConvY[N * OC * oh * ow];
        f64 gX[N * chans * hin * win];
        nn::softmax_cross_entropy_vjp(probs, labels, gZ2, N, Cls);
        nn::bias_add_vjp(gZ2, gpre2, gb2, N, Cls);
        nn::matmul_vjp(P, W2, gpre2, gP, gW2, N, flat, Cls);   // gP[N×flat] = ḡZ2·W2ᵀ ; gW2 = Pᵀ·ḡZ2
        nn::max_pool_vjp(A, gP, gA, N * OC, oh, ow, k, s);     // gP viewed as [N*OC, oh2*ow2] — same buffer
        nn::relu_vjp(convY, gA, gConvY, N * OC * oh * ow);
        nn::conv2d_vjp(X, convW, gConvY, gX, gConvW, gConvB, N, chans, hin, win, OC, kh, kw, pad, stride, col, gcol);
    }

    // torch-style gradcheck: every parameter's one-pass gradient matches central FD.
    fd_gradcheck(convW, OC * C * kh * kw, gConvW, forward);
    fd_gradcheck(convB, OC, gConvB, forward);
    fd_gradcheck(W2, flat * Cls, gW2, forward);
    fd_gradcheck(b2, Cls, gb2, forward);
}
// NOLINTEND(readability-identifier-naming)
