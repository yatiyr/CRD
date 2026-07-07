#pragma once

// nn_reverse.hpp — Phase 3.1.6 v16-c: reverse-mode VJPs for the tensor / NN op set — the layer that makes a network
// TRAINABLE. Self-contained dense VJP functions (the v15-f pattern): each takes the forward values + the OUTPUT
// adjoint (ḡ) and produces the INPUT adjoints. The matmul VJP `gA = gC·Bᵀ, gB = Aᵀ·gC` IS an einsum with a permuted
// spec (the v16-c principle; production rides the v14 `gemm`/`EinsumPlan`). Composed, they backprop a multi-layer MLP
// in ONE pass — O(1) passes for the whole parameter gradient, where finite differences need O(#params) evaluations.
// Deterministic (crd::math), allocation-free (caller scratch). Gated vs FD AND the v16-a/b scalar tape. ADR-0097.

#include <crd/hesap/autodiff/matrix_jvp.hpp> // reuse gemm / gemm_tn

#include <crd/core/types.hpp>
#include <crd/math/cmath.hpp>
#include <crd/math/deterministic.hpp> // crd::math::deterministic::erf — the exact (erf-form) gelu + its slope

namespace crd::hesap::autodiff::reverse::nn
{
namespace mj = crd::hesap::autodiff::forward::matrix;

// C[m×n] = A[m×k] · Bᵀ  (B stored n×k) — the transposed-right gemm the matmul VJP needs.
inline void gemm_nt(const crd::f64* a, const crd::f64* b, crd::f64* c, int m, int k, int n) noexcept
{
    for (int i = 0; i < m; ++i)
    {
        for (int j = 0; j < n; ++j)
        {
            crd::f64 s = 0.0;
            for (int t = 0; t < k; ++t) { s += a[i * k + t] * b[j * k + t]; }
            c[i * n + j] = s;
        }
    }
}

// ---- matmul  C = A·B  ( A[m×k], B[k×p], C[m×p] ) ------------------------------------------------------------
inline void matmul(const crd::f64* a, const crd::f64* b, crd::f64* c, int m, int k, int p) noexcept
{
    mj::gemm(a, b, c, m, k, p);
}
// VJP: ḡA = ḡC·Bᵀ (m×k) ; ḡB = Aᵀ·ḡC (k×p). Both are einsums with permuted specs.
inline void matmul_vjp(const crd::f64* a, const crd::f64* b, const crd::f64* gc, crd::f64* ga, crd::f64* gb, int m,
                       int k, int p) noexcept
{
    gemm_nt(gc, b, ga, m, p, k);     // ḡA[m×k] = ḡC[m×p]·Bᵀ[p×k]
    mj::gemm_tn(a, gc, gb, k, m, p); // ḡB[k×p] = Aᵀ[k×m]·ḡC[m×p]  (gemm_tn: A stored m×k)
}

// ---- bias add  Y[r×c] = X[r×c] + bias[c]  (broadcast over rows) --------------------------------------------
inline void bias_add(const crd::f64* x, const crd::f64* bias, crd::f64* y, int rows, int cols) noexcept
{
    for (int i = 0; i < rows; ++i)
    {
        for (int j = 0; j < cols; ++j) { y[i * cols + j] = x[i * cols + j] + bias[j]; }
    }
}
// VJP: ḡX = ḡY ; ḡbias[j] = Σ_i ḡY[i,j] (broadcast is transposed by a sum).
inline void bias_add_vjp(const crd::f64* gy, crd::f64* gx, crd::f64* gbias, int rows, int cols) noexcept
{
    for (int j = 0; j < cols; ++j) { gbias[j] = 0.0; }
    for (int i = 0; i < rows; ++i)
    {
        for (int j = 0; j < cols; ++j)
        {
            gx[i * cols + j] = gy[i * cols + j];
            gbias[j] += gy[i * cols + j];
        }
    }
}

// ---- ReLU (elementwise) ------------------------------------------------------------------------------------
inline void relu(const crd::f64* x, crd::f64* y, int n) noexcept
{
    for (int i = 0; i < n; ++i) { y[i] = x[i] > 0.0 ? x[i] : 0.0; }
}
inline void relu_vjp(const crd::f64* x, const crd::f64* gy, crd::f64* gx, int n) noexcept
{
    for (int i = 0; i < n; ++i) { gx[i] = x[i] > 0.0 ? gy[i] : 0.0; }
}

// ---- softmax + cross-entropy loss (row = sample, cols = classes; labels = class index per row) --------------
// Returns mean cross-entropy over the `rows` samples; `probs` (r×c) filled with softmax for the VJP.
[[nodiscard]] inline crd::f64 softmax_cross_entropy(const crd::f64* logits, const int* labels, crd::f64* probs,
                                                    int rows, int cols) noexcept
{
    crd::f64 loss = 0.0;
    for (int i = 0; i < rows; ++i)
    {
        const crd::f64* z = logits + i * cols;
        crd::f64        mx = z[0];
        for (int j = 1; j < cols; ++j) { mx = z[j] > mx ? z[j] : mx; }
        crd::f64 sum = 0.0;
        for (int j = 0; j < cols; ++j)
        {
            const crd::f64 e   = crd::math::exp(z[j] - mx);
            probs[i * cols + j] = e;
            sum += e;
        }
        const crd::f64 inv = 1.0 / sum;
        for (int j = 0; j < cols; ++j) { probs[i * cols + j] *= inv; }
        loss -= crd::math::log(probs[i * cols + labels[i]]);
    }
    return loss / static_cast<crd::f64>(rows);
}
// VJP of the mean loss wrt the logits: ḡlogits = (softmax − onehot(label)) / rows.
inline void softmax_cross_entropy_vjp(const crd::f64* probs, const int* labels, crd::f64* glogits, int rows,
                                      int cols) noexcept
{
    const crd::f64 inv = 1.0 / static_cast<crd::f64>(rows);
    for (int i = 0; i < rows; ++i)
    {
        for (int j = 0; j < cols; ++j) { glogits[i * cols + j] = probs[i * cols + j] * inv; }
        glogits[i * cols + labels[i]] -= inv;
    }
}

// ============================================================================================================
// v16-c coverage — the rest of the v14-m op set, so the whole CNN corpus (conv → relu → pool → flatten → linear
// → softmax-CE) backprops in ONE pass. Same self-contained f64 pattern: forward + VJP, each VJP the transpose of
// its forward, gated vs central FD. crd::math for every transcendental (deterministic + the crush-moat surface).
// ============================================================================================================

// ---- GELU (exact erf form — torch approximate='none') ------------------------------------------------------
// y = 0.5·x·(1 + erf(x/√2)) ; y'(x) = Φ(x) + x·φ(x) = 0.5(1+erf(x/√2)) + x·exp(−x²/2)/√(2π).
inline void gelu(const crd::f64* x, crd::f64* y, int n) noexcept
{
    constexpr crd::f64 inv_sqrt2 = 0.70710678118654752440;
    for (int i = 0; i < n; ++i) { y[i] = 0.5 * x[i] * (1.0 + crd::math::deterministic::erf(x[i] * inv_sqrt2)); }
}
inline void gelu_vjp(const crd::f64* x, const crd::f64* gy, crd::f64* gx, int n) noexcept
{
    constexpr crd::f64 inv_sqrt2   = 0.70710678118654752440;
    constexpr crd::f64 inv_sqrt2pi = 0.39894228040143267794; // 1/√(2π)
    for (int i = 0; i < n; ++i)
    {
        const crd::f64 cdf   = 0.5 * (1.0 + crd::math::deterministic::erf(x[i] * inv_sqrt2));
        const crd::f64 pdf   = inv_sqrt2pi * crd::math::exp(-0.5 * x[i] * x[i]);
        gx[i] = gy[i] * (cdf + x[i] * pdf);
    }
}

// ---- tanh / sigmoid (VJP saves the OUTPUT y, torch convention) ---------------------------------------------
inline void tanh_act(const crd::f64* x, crd::f64* y, int n) noexcept
{
    for (int i = 0; i < n; ++i) { y[i] = crd::math::tanh(x[i]); }
}
inline void tanh_vjp(const crd::f64* y, const crd::f64* gy, crd::f64* gx, int n) noexcept
{
    for (int i = 0; i < n; ++i) { gx[i] = gy[i] * (1.0 - y[i] * y[i]); } // y'(x) = 1 − tanh²
}
inline void sigmoid(const crd::f64* x, crd::f64* y, int n) noexcept
{
    for (int i = 0; i < n; ++i) { y[i] = 1.0 / (1.0 + crd::math::exp(-x[i])); }
}
inline void sigmoid_vjp(const crd::f64* y, const crd::f64* gy, crd::f64* gx, int n) noexcept
{
    for (int i = 0; i < n; ++i) { gx[i] = gy[i] * y[i] * (1.0 - y[i]); } // y'(x) = σ(1−σ)
}

// ---- softmax (row = sample, cols = classes; stable) — the STANDALONE op (the fused CE VJP lives above) ------
inline void softmax(const crd::f64* x, crd::f64* y, int rows, int cols) noexcept
{
    for (int i = 0; i < rows; ++i)
    {
        const crd::f64* z = x + i * cols;
        crd::f64        mx = z[0];
        for (int j = 1; j < cols; ++j) { mx = z[j] > mx ? z[j] : mx; }
        crd::f64 sum = 0.0;
        for (int j = 0; j < cols; ++j)
        {
            const crd::f64 e = crd::math::exp(z[j] - mx);
            y[i * cols + j]  = e;
            sum += e;
        }
        const crd::f64 inv = 1.0 / sum;
        for (int j = 0; j < cols; ++j) { y[i * cols + j] *= inv; }
    }
}
// VJP: per row, ḡx_j = y_j·(ḡy_j − Σ_t y_t·ḡy_t) — the softmax Jacobian (diag(y) − y yᵀ) applied to ḡy.
inline void softmax_vjp(const crd::f64* y, const crd::f64* gy, crd::f64* gx, int rows, int cols) noexcept
{
    for (int i = 0; i < rows; ++i)
    {
        crd::f64 dot = 0.0;
        for (int j = 0; j < cols; ++j) { dot += y[i * cols + j] * gy[i * cols + j]; }
        for (int j = 0; j < cols; ++j) { gx[i * cols + j] = y[i * cols + j] * (gy[i * cols + j] - dot); }
    }
}

// ---- LayerNorm (torch semantics: last-dim d, biased variance, eps INSIDE the sqrt, affine γ/β) -------------
// Forward: μ=mean(x), σ²=var(x), r=1/√(σ²+eps), x̂=(x−μ)·r, y=γ·x̂+β. VJP recomputes μ/r/x̂ from x (self-contained).
inline void layernorm(const crd::f64* x, const crd::f64* gamma, const crd::f64* beta, crd::f64 eps, crd::f64* y,
                      int rows, int d) noexcept
{
    const crd::f64 invd = 1.0 / static_cast<crd::f64>(d);
    for (int i = 0; i < rows; ++i)
    {
        const crd::f64* xr = x + i * d;
        crd::f64        mean = 0.0;
        for (int j = 0; j < d; ++j) { mean += xr[j]; }
        mean *= invd;
        crd::f64 var = 0.0;
        for (int j = 0; j < d; ++j) { const crd::f64 t = xr[j] - mean; var += t * t; }
        var *= invd;
        const crd::f64 rstd = 1.0 / crd::math::sqrt(var + eps);
        for (int j = 0; j < d; ++j) { y[i * d + j] = gamma[j] * ((xr[j] - mean) * rstd) + beta[j]; }
    }
}
// VJP: ḡγ_j = Σ_i ḡy_ij·x̂_ij ; ḡβ_j = Σ_i ḡy_ij ; ḡx = r·(ĝ − mean(ĝ) − x̂·mean(ĝ·x̂)) with ĝ = ḡy·γ.
inline void layernorm_vjp(const crd::f64* x, const crd::f64* gamma, const crd::f64* gy, crd::f64 eps, crd::f64* gx,
                          crd::f64* ggamma, crd::f64* gbeta, int rows, int d) noexcept
{
    const crd::f64 invd = 1.0 / static_cast<crd::f64>(d);
    for (int j = 0; j < d; ++j) { ggamma[j] = 0.0; gbeta[j] = 0.0; }
    for (int i = 0; i < rows; ++i)
    {
        const crd::f64* xr = x + i * d;
        const crd::f64* gr = gy + i * d;
        crd::f64        mean = 0.0;
        for (int j = 0; j < d; ++j) { mean += xr[j]; }
        mean *= invd;
        crd::f64 var = 0.0;
        for (int j = 0; j < d; ++j) { const crd::f64 t = xr[j] - mean; var += t * t; }
        var *= invd;
        const crd::f64 rstd = 1.0 / crd::math::sqrt(var + eps);
        crd::f64 mean_g   = 0.0; // mean_j ĝ_j
        crd::f64 mean_gxh = 0.0; // mean_j ĝ_j·x̂_j
        for (int j = 0; j < d; ++j)
        {
            const crd::f64 xhat = (xr[j] - mean) * rstd;
            const crd::f64 ghat = gr[j] * gamma[j];
            gbeta[j] += gr[j];
            ggamma[j] += gr[j] * xhat;
            mean_g += ghat;
            mean_gxh += ghat * xhat;
        }
        mean_g *= invd;
        mean_gxh *= invd;
        for (int j = 0; j < d; ++j)
        {
            const crd::f64 xhat = (xr[j] - mean) * rstd;
            const crd::f64 ghat = gr[j] * gamma[j];
            gx[i * d + j] = rstd * (ghat - mean_g - xhat * mean_gxh);
        }
    }
}

// ---- pooling (per plane, k×k window, stride s; oh=(h−k)/s+1, ow=(w−k)/s+1) ---------------------------------
// forward max keeps the FIRST maximum (strict `v > m`, matching the v14-m kernel); the VJP routes ḡ to that same
// argmax position. avg spreads ḡ/(k·k). gx is zero-filled then SCATTER-added, so overlapping windows (s<k) sum.
inline void max_pool(const crd::f64* x, crd::f64* y, int planes, int h, int w, int k, int s) noexcept
{
    const int oh = (h - k) / s + 1;
    const int ow = (w - k) / s + 1;
    for (int p = 0; p < planes; ++p)
    {
        const crd::f64* xp = x + static_cast<crd::i64>(p) * h * w;
        crd::f64*       yp = y + static_cast<crd::i64>(p) * oh * ow;
        for (int oi = 0; oi < oh; ++oi)
        {
            for (int oj = 0; oj < ow; ++oj)
            {
                const crd::f64* win = xp + (oi * s) * w + oj * s;
                crd::f64        m   = win[0];
                for (int ki = 0; ki < k; ++ki)
                {
                    for (int kj = 0; kj < k; ++kj)
                    {
                        const crd::f64 v = win[ki * w + kj];
                        m = v > m ? v : m;
                    }
                }
                yp[oi * ow + oj] = m;
            }
        }
    }
}
inline void max_pool_vjp(const crd::f64* x, const crd::f64* gy, crd::f64* gx, int planes, int h, int w, int k,
                         int s) noexcept
{
    const int oh = (h - k) / s + 1;
    const int ow = (w - k) / s + 1;
    for (crd::i64 i = 0; i < static_cast<crd::i64>(planes) * h * w; ++i) { gx[i] = 0.0; }
    for (int p = 0; p < planes; ++p)
    {
        const crd::f64* xp  = x + static_cast<crd::i64>(p) * h * w;
        crd::f64*       gxp = gx + static_cast<crd::i64>(p) * h * w;
        const crd::f64* gyp = gy + static_cast<crd::i64>(p) * oh * ow;
        for (int oi = 0; oi < oh; ++oi)
        {
            for (int oj = 0; oj < ow; ++oj)
            {
                const int base = (oi * s) * w + oj * s; // window top-left offset in the plane
                crd::f64  m    = xp[base];
                int       arg  = base; // first-argmax, same scan order as forward
                for (int ki = 0; ki < k; ++ki)
                {
                    for (int kj = 0; kj < k; ++kj)
                    {
                        const int      off = base + ki * w + kj;
                        const crd::f64 v   = xp[off];
                        if (v > m) { m = v; arg = off; }
                    }
                }
                gxp[arg] += gyp[oi * ow + oj];
            }
        }
    }
}
inline void avg_pool(const crd::f64* x, crd::f64* y, int planes, int h, int w, int k, int s) noexcept
{
    const int      oh  = (h - k) / s + 1;
    const int      ow  = (w - k) / s + 1;
    const crd::f64 inv = 1.0 / static_cast<crd::f64>(k * k);
    for (int p = 0; p < planes; ++p)
    {
        const crd::f64* xp = x + static_cast<crd::i64>(p) * h * w;
        crd::f64*       yp = y + static_cast<crd::i64>(p) * oh * ow;
        for (int oi = 0; oi < oh; ++oi)
        {
            for (int oj = 0; oj < ow; ++oj)
            {
                const crd::f64* win = xp + (oi * s) * w + oj * s;
                crd::f64        acc = 0.0;
                for (int ki = 0; ki < k; ++ki)
                {
                    for (int kj = 0; kj < k; ++kj) { acc += win[ki * w + kj]; }
                }
                yp[oi * ow + oj] = acc * inv;
            }
        }
    }
}
inline void avg_pool_vjp(const crd::f64* gy, crd::f64* gx, int planes, int h, int w, int k, int s) noexcept
{
    const int      oh  = (h - k) / s + 1;
    const int      ow  = (w - k) / s + 1;
    const crd::f64 inv = 1.0 / static_cast<crd::f64>(k * k);
    for (crd::i64 i = 0; i < static_cast<crd::i64>(planes) * h * w; ++i) { gx[i] = 0.0; }
    for (int p = 0; p < planes; ++p)
    {
        crd::f64*       gxp = gx + static_cast<crd::i64>(p) * h * w;
        const crd::f64* gyp = gy + static_cast<crd::i64>(p) * oh * ow;
        for (int oi = 0; oi < oh; ++oi)
        {
            for (int oj = 0; oj < ow; ++oj)
            {
                const crd::f64 g    = gyp[oi * ow + oj] * inv;
                const int      base = (oi * s) * w + oj * s;
                for (int ki = 0; ki < k; ++ki)
                {
                    for (int kj = 0; kj < k; ++kj) { gxp[base + ki * w + kj] += g; }
                }
            }
        }
    }
}

// ---- conv2d (im2col + gemm; the v14-m CNN op) — self-contained f64 ------------------------------------------
// x[C,H,W] (one image), w[OC, C·KH·KW] row-major, bias[OC], y[OC,OH·OW]; oh=(h+2p−kh)/s+1, ow=(w+2p−kw)/s+1.
// col[(c·KH+ki)·KW+kj , oi·OW+oj] = x[c, oi·s+ki−p, oj·s+kj−p] (0 out of bounds) — the r'th row, (oi·OW+oj)'th col.
inline void im2col_one_f64(const crd::f64* x, int chans, int h, int w, int kh, int kw, int pad, int stride, int oh,
                           int ow, crd::f64* col) noexcept
{
    const int ohw = oh * ow;
    int       r   = 0;
    for (int c = 0; c < chans; ++c)
    {
        const crd::f64* plane = x + static_cast<crd::i64>(c) * h * w;
        for (int ki = 0; ki < kh; ++ki)
        {
            for (int kj = 0; kj < kw; ++kj)
            {
                crd::f64* dst = col + static_cast<crd::i64>(r) * ohw;
                ++r;
                for (int oi = 0; oi < oh; ++oi)
                {
                    const int ih = oi * stride + ki - pad;
                    for (int oj = 0; oj < ow; ++oj)
                    {
                        const int iw   = oj * stride + kj - pad;
                        const bool ok  = ih >= 0 && ih < h && iw >= 0 && iw < w;
                        dst[oi * ow + oj] = ok ? plane[static_cast<crd::i64>(ih) * w + iw] : 0.0;
                    }
                }
            }
        }
    }
}
// col2im: the exact transpose of im2col — scatter-add each col entry back to its source pixel (overlaps sum).
inline void col2im_one_f64(const crd::f64* col, int chans, int h, int w, int kh, int kw, int pad, int stride,
                           int oh, int ow, crd::f64* gx) noexcept
{
    const int ohw = oh * ow;
    int       r   = 0;
    for (int c = 0; c < chans; ++c)
    {
        crd::f64* plane = gx + static_cast<crd::i64>(c) * h * w;
        for (int ki = 0; ki < kh; ++ki)
        {
            for (int kj = 0; kj < kw; ++kj)
            {
                const crd::f64* src = col + static_cast<crd::i64>(r) * ohw;
                ++r;
                for (int oi = 0; oi < oh; ++oi)
                {
                    const int ih = oi * stride + ki - pad;
                    if (ih < 0 || ih >= h) { continue; }
                    for (int oj = 0; oj < ow; ++oj)
                    {
                        const int iw = oj * stride + kj - pad;
                        if (iw >= 0 && iw < w) { plane[static_cast<crd::i64>(ih) * w + iw] += src[oi * ow + oj]; }
                    }
                }
            }
        }
    }
}
// Forward conv over a BATCH of n images. col scratch = ckk·ohw floats (one image at a time).
inline void conv2d(const crd::f64* x, const crd::f64* w, const crd::f64* bias, crd::f64* y, int n, int chans,
                   int h, int wi, int oc, int kh, int kw, int pad, int stride, crd::f64* col) noexcept
{
    const int oh  = (h + 2 * pad - kh) / stride + 1;
    const int ow  = (wi + 2 * pad - kw) / stride + 1;
    const int ckk = chans * kh * kw;
    const int ohw = oh * ow;
    for (int img = 0; img < n; ++img)
    {
        const crd::f64* xi = x + static_cast<crd::i64>(img) * chans * h * wi;
        crd::f64*       yi = y + static_cast<crd::i64>(img) * oc * ohw;
        im2col_one_f64(xi, chans, h, wi, kh, kw, pad, stride, oh, ow, col);
        mj::gemm(w, col, yi, oc, ckk, ohw); // yi[OC×OHW] = W[OC×CKK]·col[CKK×OHW]
        for (int o = 0; o < oc; ++o)
        {
            for (int q = 0; q < ohw; ++q) { yi[static_cast<crd::i64>(o) * ohw + q] += bias[o]; }
        }
    }
}
// VJP: ḡW = Σ_img ḡY·colᵀ ; ḡb_o = Σ ḡY[o,·] ; ḡX = col2im(Wᵀ·ḡY). gw/gb accumulate; gx is zero-filled per image.
// scratch: col (ckk·ohw) reused for im2col then gcol; caller passes two ckk·ohw buffers (col, gcol).
inline void conv2d_vjp(const crd::f64* x, const crd::f64* w, const crd::f64* gy, crd::f64* gx, crd::f64* gw,
                       crd::f64* gb, int n, int chans, int h, int wi, int oc, int kh, int kw, int pad, int stride,
                       crd::f64* col, crd::f64* gcol) noexcept
{
    const int oh  = (h + 2 * pad - kh) / stride + 1;
    const int ow  = (wi + 2 * pad - kw) / stride + 1;
    const int ckk = chans * kh * kw;
    const int ohw = oh * ow;
    for (crd::i64 i = 0; i < static_cast<crd::i64>(oc) * ckk; ++i) { gw[i] = 0.0; }
    for (int o = 0; o < oc; ++o) { gb[o] = 0.0; }
    for (int img = 0; img < n; ++img)
    {
        const crd::f64* xi  = x + static_cast<crd::i64>(img) * chans * h * wi;
        const crd::f64* gyi = gy + static_cast<crd::i64>(img) * oc * ohw;
        crd::f64*       gxi = gx + static_cast<crd::i64>(img) * chans * h * wi;
        im2col_one_f64(xi, chans, h, wi, kh, kw, pad, stride, oh, ow, col);
        // ḡW += ḡY[OC×OHW]·colᵀ[OHW×CKK] (col stored CKK×OHW → gemm_nt with B=col, n=CKK, k=OHW)
        for (int o = 0; o < oc; ++o)
        {
            for (int t = 0; t < ckk; ++t)
            {
                crd::f64 sacc = 0.0;
                for (int q = 0; q < ohw; ++q)
                {
                    sacc += gyi[static_cast<crd::i64>(o) * ohw + q] * col[static_cast<crd::i64>(t) * ohw + q];
                }
                gw[static_cast<crd::i64>(o) * ckk + t] += sacc;
            }
        }
        for (int o = 0; o < oc; ++o)
        {
            crd::f64 s = 0.0;
            for (int q = 0; q < ohw; ++q) { s += gyi[static_cast<crd::i64>(o) * ohw + q]; }
            gb[o] += s;
        }
        // ḡcol[CKK×OHW] = Wᵀ[CKK×OC]·ḡY[OC×OHW] (W stored OC×CKK) → col2im → ḡX
        mj::gemm_tn(w, gyi, gcol, ckk, oc, ohw);
        for (crd::i64 i = 0; i < static_cast<crd::i64>(chans) * h * wi; ++i) { gxi[i] = 0.0; }
        col2im_one_f64(gcol, chans, h, wi, kh, kw, pad, stride, oh, ow, gxi);
    }
}

} // namespace crd::hesap::autodiff::reverse::nn
