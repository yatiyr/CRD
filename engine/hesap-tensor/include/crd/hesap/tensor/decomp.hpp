#pragma once
// ---------------------------------------------------------------------------
// crd-hesap-tensor — v14-j: tensor decompositions I (DENSE scope).
//
//   - cp_als        — CP/PARAFAC via alternating least squares: rank-R factors
//                     [I_n x R] + weights (Kolda-Bader column normalization),
//                     deterministic init (SVD of the unfoldings, or keyed
//                     PhiloxRng draws), fit tracking, bounded iteration
//                     (max_iters + tol => NotConverged, never spins).
//                     The tensor enters ONLY through the MTTKRP seam (below):
//                     `cp_als_generic` is the core; the dense entry point wires
//                     `DenseMttkrp`; the v14-i sparse integrator wires its own
//                     functor against the same contract.
//   - hosvd / hooi  — Tucker: truncated multilinear SVD (mode-n unfolding ->
//                     hesap-dense SVD -> leading left singular vectors; core =
//                     X x_n U_n^T), and bounded HOOI ALS on top of it.
//   - hosvd_rand /  — the randomized variants: per-mode Halko-Martinsson-Tropp
//     hooi_rand       range finder (gaussian sketch + oversampling + power
//                     iterations + Householder QR) with EVERY sketch entry a
//                     pure function of (seed, stream, index) over the engine's
//                     Philox4x32 counter RNG => DETERMINISTIC-RANDOMIZED: the
//                     same seed is bit-identical at any worker count (gated).
//
// REUSE (SANITY #8): unfoldings ride the v14-d HPTT-class permute machinery
// (`permute_copy`); all matrix products ride hesap-dense `gemm`; the exact
// factor kernel rides hesap-dense `eig_sym` (the Gram trick — D(v14j)-4); the
// randomized QR rides hesap-dense Householder
// `factor_qr/apply_q`; the R x R normal-equations solves ride this module's
// batched scalar kernels (`chol_scalar_one`, Jacobi `svd_scalar_sweeps` as the
// pseudo-inverse fallback); randomness is `crd::hesap::stats::PhiloxRng` keyed
// draws (never sequential global state).
//
// Error contract (the v13 pillars): noexcept, status-not-exception. The module
// TensorStatus enum has no NotConverged member and existing files are frozen,
// so this header defines DecompStatus (superset semantics; `to_decomp` maps).
//
// Determinism: all orchestration is serial; the only worker-count-sensitive
// callees (`permute_copy` MT tiles) are bit-identical at any worker count by
// construction, and every random draw is a keyed counter function => every
// entry point here is bit-identical at any worker count (the {1..16} gate).
//
// Divergence notes (house rule):
//   D(v14j)-1: CP column normalization is the 2-norm EVERY iteration (Kolda-
//              Bader's ttb switches to the max-norm after sweep 1). Model-
//              equivalent reparametrization; fit parity vs tensorly proven in
//              scripts/v14j_decomp_oracle.py before this port.
//   D(v14j)-2: Tucker rec_error uses tensorly's norm identity
//              sqrt(max(||X||^2 - ||core||^2, 0)) / ||X|| — it carries a
//              ~sqrt(eps) floor near exact recovery (cancellation); exact-
//              recovery gates measure the true reconstruction instead.
//   D(v14j)-3: the randomized sketch is Irwin-Hall (sum of 12 uniforms - 6)
//              over Philox — the engine's sketch-gaussian precedent
//              (dense::counter_gaussian), not a transcendental Box-Muller.
//   D(v14j)-4: leading left singular vectors of the (short-fat) unfoldings are
//              computed via the GRAM trick — G = A A^T (one gemm pass) +
//              eig_sym — the TuckerMPI-standard dense-Tucker kernel, NOT a
//              bidiagonal SVD of the full unfolding (measured 2026-07-05:
//              64x4096 exact svd 117 ms vs gram+eig < 1 ms — the entire
//              tensorly wall-clock gap was this kernel). Directions with
//              sigma <= sqrt(eps)*sigma_max lose angle accuracy (kappa^2), but
//              they carry <= sqrt(eps)*sigma_max reconstruction mass —
//              immaterial to truncation; the frozen oracle gates re-verified
//              green on the switched kernel (incl. the 1e-12 exact-recovery
//              rows).
// ---------------------------------------------------------------------------
#include "batched.hpp"  // chol_scalar_one / svd_scalar_sweeps / svd_finalize_one (R x R solves)
#include "permute.hpp"  // HPTT-class unfoldings
#include "tensor.hpp"

#include <crd/containers/array.hpp>
#include <crd/hesap/dense/blas3.hpp>
#include <crd/hesap/dense/eig_sym.hpp>
#include <crd/hesap/dense/qr.hpp>
#include <crd/hesap/stats/philox.hpp>
#include <crd/memory/allocator.hpp>

#include <cmath>  // std::sqrt, std::fma (single-rounded chains — the codelet rule)

namespace crd::hesap::tensor
{

// TensorStatus + NotConverged (bounded-iteration pillar). tensor.hpp is frozen
// this slice, so the decomposition surface carries its own status enum.
enum class DecompStatus : crd::u8
{
    Ok,
    BadInput,      // bad rank / bad mode count / bad output sizes
    ShapeMismatch, // factor/core shapes do not match the input tensor
    AllocFailed,   // IAllocator returned null
    NotConverged,  // iteration budget exhausted before |delta rec_error| < tol
    Unsupported,
};

[[nodiscard]] inline const char* to_string(DecompStatus status) noexcept
{
    switch (status)
    {
    case DecompStatus::Ok:
        return "Ok";
    case DecompStatus::BadInput:
        return "BadInput";
    case DecompStatus::ShapeMismatch:
        return "ShapeMismatch";
    case DecompStatus::AllocFailed:
        return "AllocFailed";
    case DecompStatus::NotConverged:
        return "NotConverged";
    case DecompStatus::Unsupported:
        return "Unsupported";
    }
    return "?";
}

[[nodiscard]] inline DecompStatus to_decomp(TensorStatus st) noexcept
{
    switch (st)
    {
    case TensorStatus::Ok:
        return DecompStatus::Ok;
    case TensorStatus::ShapeMismatch:
        return DecompStatus::ShapeMismatch;
    case TensorStatus::AllocFailed:
        return DecompStatus::AllocFailed;
    case TensorStatus::BadInput:
    case TensorStatus::RankOverflow:
    case TensorStatus::NotContiguous:
        return DecompStatus::BadInput;
    case TensorStatus::Unsupported:
        return DecompStatus::Unsupported;
    }
    return DecompStatus::Unsupported;
}

namespace decompdetail
{

// ---- keyed counter draws (the deterministic-randomized primitive) ----------
// Pure functions of (seed, stream, idx) over Philox4x32 — the value at any
// position exists independently of who computes it, in what order, on how many
// workers (the counter-based moat). Streams used by this header:
//   stream = mode                  — CP random init (uniform [0,1), tensorly's class)
//   stream = kSketchStream + mode  — randomized range-finder sketches (gaussian)
inline constexpr crd::u64 kSketchStream = 0x524E4453ULL; // 'RNDS'

template <typename T>
[[nodiscard]] inline T philox_uniform(crd::u64 seed, crd::u64 stream, crd::u64 idx) noexcept
{
    crd::hesap::stats::PhiloxRng rng(seed, stream);
    rng.jump_to_block(idx);
    return static_cast<T>(rng.next_f64());
}

template <typename T>
[[nodiscard]] inline T philox_gaussian(crd::u64 seed, crd::u64 stream, crd::u64 idx) noexcept
{
    // Irwin-Hall 12 (D(v14j)-3): 12 f64 uniforms = 6 Philox blocks per sample.
    crd::hesap::stats::PhiloxRng rng(seed, stream);
    rng.jump_to_block(idx * 6ULL);
    crd::f64 acc = 0.0;
    for (crd::u32 t = 0; t < 12U; ++t)
    {
        acc += rng.next_f64();
    }
    return static_cast<T>(acc - 6.0);
}

// Batched sketch fill: dst[idx] == philox_gaussian(seed, stream, idx) BIT-
// EXACTLY for every idx (gated) — sample idx consumes u64 lanes [12*idx,
// 12*idx+12) of the (seed, stream) Philox stream either way — but rides
// PhiloxRng::fill's AVX2 8-block path (the per-sample scalar draw measured as
// the dominant randomized-tier cost). The keyed-counter moat is unchanged:
// still a pure function of (seed, stream).
template <typename T>
inline void philox_gaussian_fill(crd::u64 seed, crd::u64 stream, T* dst, crd::u64 count) noexcept
{
    crd::hesap::stats::PhiloxRng rng(seed, stream);
    constexpr crd::u64 chunk = 256U; // samples per fill: 24 KB stack buffer
    crd::u64 buf[chunk * 12U];
    crd::u64 done = 0;
    while (done < count)
    {
        const crd::u64 n = count - done < chunk ? count - done : chunk;
        rng.fill({buf, static_cast<crd::usize>(n * 12U)});
        for (crd::u64 s = 0; s < n; ++s)
        {
            crd::f64 acc = 0.0;
            for (crd::u32 t = 0; t < 12U; ++t)
            {
                // next_f64's exact mapping: (u64 >> 11) / 2^53
                acc += static_cast<crd::f64>(buf[s * 12U + t] >> 11) * (1.0 / 9007199254740992.0);
            }
            dst[done + s] = static_cast<T>(acc - 6.0);
        }
        done += n;
    }
}

// ---- unfolding orders -------------------------------------------------------
// Mode-n unfolding (tensorly/C-order): X_(n)[i_n, j] with j the row-major flat
// index of the REMAINING modes in ascending order (last mode fastest) — i.e.
// permute order [n, 0, .., n-1, n+1, .., N-1] then view as [I_n, prod rest].
inline void unfold_order(crd::u32 mode, crd::u32 ndim, crd::u32* order) noexcept
{
    order[0] = mode;
    crd::u32 o = 1;
    for (crd::u32 d = 0; d < ndim; ++d)
    {
        if (d != mode)
        {
            order[o++] = d;
        }
    }
}

// Inverse of unfold_order: out dim d reads permuted dim inv[d].
inline void unfold_inverse_order(crd::u32 mode, crd::u32 ndim, crd::u32* inv) noexcept
{
    inv[mode] = 0;
    crd::u32 o = 1;
    for (crd::u32 d = 0; d < ndim; ++d)
    {
        if (d != mode)
        {
            inv[d] = o++;
        }
    }
}

template <typename T>
[[nodiscard]] inline TensorStatus unfold_copy(TensorView<const T> src, crd::u32 mode, Tensor<T>& dst) noexcept
{
    crd::u32 order[kMaxRank];
    unfold_order(mode, src.rank(), order);
    return permute_copy<T>(src, {order, src.rank()}, dst);
}

// ---- small dense helpers ----------------------------------------------------

template <typename T>
[[nodiscard]] inline crd::hesap::dense::MatrixView<const T, crd::hesap::dense::Layout::RowMajor>
mat_cview(const T* data, crd::u64 rows, crd::u64 cols) noexcept
{
    return {data, static_cast<crd::usize>(rows), static_cast<crd::usize>(cols), static_cast<crd::usize>(cols)};
}

template <typename T>
[[nodiscard]] inline crd::hesap::dense::MatrixView<T, crd::hesap::dense::Layout::RowMajor>
mat_view(T* data, crd::u64 rows, crd::u64 cols) noexcept
{
    return {data, static_cast<crd::usize>(rows), static_cast<crd::usize>(cols), static_cast<crd::usize>(cols)};
}

// g[r x r] = f^T f for row-major f [rows x r] (plain fma loops; r is small).
template <typename T>
inline void gram(const T* f, crd::u64 rows, crd::u64 r, T* g) noexcept
{
    for (crd::u64 a = 0; a < r; ++a)
    {
        for (crd::u64 b = a; b < r; ++b)
        {
            T s = T(0);
            for (crd::u64 i = 0; i < rows; ++i)
            {
                s = std::fma(f[i * r + a], f[i * r + b], s);
            }
            g[a * r + b] = s;
            g[b * r + a] = s;
        }
    }
}

// Solve X V = M row-wise for symmetric V (destroying vwork): X written into
// `dst` [rows x r], M read from `src` (src == dst allowed only when the caller
// does not need M afterwards — cp_als keeps them separate for the fit terms).
// Cholesky first; a non-SPD V (degenerate factors) falls back to the Jacobi-SVD
// pseudo-inverse with cutoff r*eps*sigma_max — bounded, never spins.
// scratch layout: ju[r*r] | jv[r*r] | sig[r] | tmp[2r]
template <typename T>
inline void solve_rows_sym(const T* src, T* dst, crd::u64 rows, T* vwork, crd::u64 r, T* scratch) noexcept
{
    crd::i32 info = 0;
    T* ju = scratch;
    T* jv = ju + r * r;
    T* sig = jv + r * r;
    T* tmp = sig + r;
    for (crd::u64 e = 0; e < r * r; ++e)
    {
        ju[e] = vwork[e]; // preserve V for the fallback; chol destroys its copy
    }
    batcheddetail::chol_scalar_one(vwork, r, &info);
    if (info == 0)
    {
        const T* l = vwork;
        for (crd::u64 i = 0; i < rows; ++i)
        {
            const T* m = src + i * r;
            T* y = tmp;
            for (crd::u64 a = 0; a < r; ++a) // forward: L y = m
            {
                T s = m[a];
                for (crd::u64 p = 0; p < a; ++p)
                {
                    s = std::fma(-l[a * r + p], y[p], s);
                }
                y[a] = s / l[a * r + a];
            }
            T* x = dst + i * r;
            for (crd::u64 aa = r; aa-- > 0U;) // backward: L^T x = y
            {
                T s = y[aa];
                for (crd::u64 p = aa + 1U; p < r; ++p)
                {
                    s = std::fma(-l[p * r + aa], x[p], s);
                }
                x[aa] = s / l[aa * r + aa];
            }
        }
        return;
    }
    // pseudo-inverse fallback: V = U diag(sig) W^T (one-sided Jacobi; V symmetric)
    crd::i32 jinfo = 0;
    batcheddetail::svd_scalar_sweeps(ju, jv, r, 64U, &jinfo);
    batcheddetail::svd_finalize_one(ju, jv, sig, r);
    const T eps = std::is_same_v<T, crd::f32> ? T(1.1920929e-07) : T(2.220446049250313e-16);
    const T cutoff = static_cast<T>(r) * eps * sig[0];
    for (crd::u64 t = 0; t < r; ++t)
    {
        sig[t] = sig[t] > cutoff ? T(1) / sig[t] : T(0);
    }
    // row x: out = m * pinv, pinv = W diag(sig+) U^T  =>  tmp_t = (m . W_col_t) sig_t; out_a = sum_t tmp_t U[a][t]
    for (crd::u64 i = 0; i < rows; ++i)
    {
        const T* m = src + i * r;
        for (crd::u64 t = 0; t < r; ++t)
        {
            T s = T(0);
            for (crd::u64 a = 0; a < r; ++a)
            {
                s = std::fma(m[a], jv[a * r + t], s);
            }
            tmp[t] = s * sig[t];
        }
        T* x = dst + i * r;
        for (crd::u64 a = 0; a < r; ++a)
        {
            T s = T(0);
            for (crd::u64 t = 0; t < r; ++t)
            {
                s = std::fma(tmp[t], ju[a * r + t], s);
            }
            x[a] = s;
        }
    }
}

// ---- leading left singular vectors of a row-major [rows x cols] matrix ------
// The GRAM kernel (D(v14j)-4, TuckerMPI-standard): G = A A^T (one gemm pass
// over A; exactly symmetric — (i,j) and (j,i) sum identical products in the
// same k order), then hesap-dense eig_sym (values ASCENDING => leading vectors
// are the LAST columns, emitted in descending-eigenvalue order). dst = leading
// `r` left singular vectors, row-major [rows x r]; r <= min(rows, cols)
// validated by the callers.
template <typename T>
inline void svd_leading_columns(crd::memory::IAllocator* alloc, const T* a, crd::u64 rows, crd::u64 cols,
                                crd::u64 r, T* dst) noexcept
{
    namespace hd = crd::hesap::dense;
    hd::Symmetric<T> g(alloc, static_cast<crd::usize>(rows));
    hd::gemm<T, hd::Layout::RowMajor>(T(1), mat_cview(a, rows, cols), mat_cview(a, rows, cols), T(0),
                                      mat_view(g.data(), rows, rows), hd::Trans::None, hd::Trans::Transpose,
                                      alloc);
    const hd::EigSym<T> e = hd::eig_sym<T>(alloc, g);
    for (crd::u64 i = 0; i < rows; ++i)
    {
        for (crd::u64 j = 0; j < r; ++j)
        {
            dst[i * r + j] = e.vectors.at(static_cast<crd::usize>(i), static_cast<crd::usize>(rows - 1U - j));
        }
    }
}

// Explicit thin Q [rows x ell] of a row-major [rows x ell] input via
// Householder factor_qr + apply_q on unit vectors (elite path, no Gram-Schmidt).
template <typename T>
inline void qr_explicit_q(crd::memory::IAllocator* alloc, const T* y, crd::u64 rows, crd::u64 ell,
                          T* q_out, T* colbuf) noexcept
{
    namespace hd = crd::hesap::dense;
    hd::QR<T> qr(alloc, static_cast<crd::usize>(rows), static_cast<crd::usize>(ell));
    for (crd::u64 e = 0; e < rows * ell; ++e)
    {
        qr.packed().data()[e] = y[e];
    }
    hd::factor_qr<T, hd::Layout::RowMajor>(qr);
    for (crd::u64 j = 0; j < ell; ++j)
    {
        for (crd::u64 i = 0; i < rows; ++i)
        {
            colbuf[i] = T(0);
        }
        colbuf[j] = T(1);
        hd::apply_q<T, hd::Layout::RowMajor>(qr, {colbuf, static_cast<crd::usize>(rows)});
        for (crd::u64 i = 0; i < rows; ++i)
        {
            q_out[i * ell + j] = colbuf[i];
        }
    }
}

// Randomized tier: HMT 2011 range finder with a Philox-keyed gaussian sketch
// (stream = kSketchStream + mode => independent per-mode sketches from one
// seed) + `power` subspace iterations + Rayleigh-Ritz rotation. dst = leading
// `r` approximate left singular vectors, row-major [rows x r]. Deterministic-
// randomized by construction. Two shapes of the same algorithm:
//   rows <= kRsvdGramRows — the GRAM-OPERATOR form (the Tucker unfolding
//     regime, rows << cols): G = A A^T once (one pass), power iterations are
//     rows x rows products with SMALL re-orthonormalizations, and the Ritz
//     block is H = Q^T G Q — no tall QR, no second pass over A. (Measured
//     2026-07-05: the classic form's [cols x ell] Householder QRs were 2 ms
//     each on 4096x24 — the dominant randomized-tier cost.)
//   rows >  kRsvdGramRows — the classic HMT form (G would be rows^2): tall
//     re-orthonormalized power passes over A, Ritz block from B = Q^T A.
// Both power-suppress sub-(sigma_max*eps^{1/(2q+1)}) directions identically;
// the Gram form shares D(v14j)-4's accuracy story (gated).
inline constexpr crd::u64 kRsvdGramRows = 256U;

template <typename T>
[[nodiscard]] inline DecompStatus rsvd_leading_columns(crd::memory::IAllocator* alloc, const T* a, crd::u64 rows,
                                                       crd::u64 cols, crd::u64 r, crd::u64 oversample,
                                                       crd::u32 power, crd::u64 seed, crd::u64 stream,
                                                       T* dst) noexcept
{
    namespace hd = crd::hesap::dense;
    const crd::u64 minmn = rows < cols ? rows : cols;
    crd::u64 ell = r + oversample;
    if (ell > minmn)
    {
        ell = minmn;
    }
    if (rows <= kRsvdGramRows)
    {
        // ---- Gram-operator form -------------------------------------------
        crd::containers::Array<T> work(alloc);
        // om[cols*ell] | y[rows*ell] | q[rows*ell] | col[rows] | h[ell*ell]
        work.resize(static_cast<crd::usize>(cols * ell + 2U * rows * ell + rows + ell * ell), T(0));
        T* om = work.data();
        T* y = om + cols * ell;
        T* q = y + rows * ell;
        T* col = q + rows * ell;
        T* h = col + rows;
        hd::Symmetric<T> g(alloc, static_cast<crd::usize>(rows));
        hd::gemm<T, hd::Layout::RowMajor>(T(1), mat_cview(a, rows, cols), mat_cview(a, rows, cols), T(0),
                                          mat_view(g.data(), rows, rows), hd::Trans::None, hd::Trans::Transpose,
                                          alloc);
        philox_gaussian_fill<T>(seed, stream, om, cols * ell); // keyed sketch (bit-equal to per-idx draws)
        hd::gemm<T, hd::Layout::RowMajor>(T(1), mat_cview(a, rows, cols), mat_cview(om, cols, ell), T(0),
                                          mat_view(y, rows, ell), hd::Trans::None, hd::Trans::None, alloc);
        qr_explicit_q(alloc, y, rows, ell, q, col);
        for (crd::u32 it = 0; it < power; ++it) // (A A^T)^q on the small side
        {
            hd::gemm<T, hd::Layout::RowMajor>(T(1), mat_cview(g.data(), rows, rows), mat_cview(q, rows, ell),
                                              T(0), mat_view(y, rows, ell), hd::Trans::None, hd::Trans::None,
                                              alloc);
            qr_explicit_q(alloc, y, rows, ell, q, col);
        }
        // Ritz block H = Q^T G Q (ell x ell), symmetrized against the two-gemm
        // rounding skew, then rotate back: dst = Q * top-r eigenvectors.
        hd::gemm<T, hd::Layout::RowMajor>(T(1), mat_cview(g.data(), rows, rows), mat_cview(q, rows, ell), T(0),
                                          mat_view(y, rows, ell), hd::Trans::None, hd::Trans::None, alloc);
        hd::gemm<T, hd::Layout::RowMajor>(T(1), mat_cview(q, rows, ell), mat_cview(y, rows, ell), T(0),
                                          mat_view(h, ell, ell), hd::Trans::Transpose, hd::Trans::None, alloc);
        hd::Symmetric<T> hs(alloc, static_cast<crd::usize>(ell));
        for (crd::u64 i = 0; i < ell; ++i)
        {
            for (crd::u64 j = 0; j < ell; ++j)
            {
                hs.data()[i * ell + j] = (h[i * ell + j] + h[j * ell + i]) * T(0.5);
            }
        }
        const hd::EigSym<T> e = hd::eig_sym<T>(alloc, hs); // values ascending
        for (crd::u64 i = 0; i < rows; ++i)
        {
            for (crd::u64 j = 0; j < r; ++j)
            {
                T acc = T(0);
                for (crd::u64 t = 0; t < ell; ++t)
                {
                    acc = std::fma(q[i * ell + t],
                                   e.vectors.at(static_cast<crd::usize>(t), static_cast<crd::usize>(ell - 1U - j)),
                                   acc);
                }
                dst[i * r + j] = acc;
            }
        }
        return DecompStatus::Ok;
    }
    // ---- classic HMT form (tall rows: G is rows^2 — never form it) ---------
    crd::containers::Array<T> work(alloc);
    // om[cols*ell] | y[rows*ell] | q[rows*ell] | z[cols*ell] | col[max(rows,cols)] | b[ell*cols]
    const crd::u64 colmax = rows > cols ? rows : cols;
    work.resize(static_cast<crd::usize>(2U * cols * ell + 2U * rows * ell + colmax + ell * cols), T(0));
    T* om = work.data();
    T* y = om + cols * ell;
    T* q = y + rows * ell;
    T* z = q + rows * ell;
    T* col = z + cols * ell;
    T* b = col + colmax;
    philox_gaussian_fill<T>(seed, stream, om, cols * ell); // keyed sketch (bit-equal to per-idx draws)
    hd::gemm<T, hd::Layout::RowMajor>(T(1), mat_cview(a, rows, cols), mat_cview(om, cols, ell), T(0),
                                      mat_view(y, rows, ell), hd::Trans::None, hd::Trans::None, alloc);
    qr_explicit_q(alloc, y, rows, ell, q, col);
    for (crd::u32 it = 0; it < power; ++it)
    {
        hd::gemm<T, hd::Layout::RowMajor>(T(1), mat_cview(a, rows, cols), mat_cview(q, rows, ell), T(0),
                                          mat_view(om, cols, ell), hd::Trans::Transpose, hd::Trans::None, alloc);
        qr_explicit_q(alloc, om, cols, ell, z, col);
        hd::gemm<T, hd::Layout::RowMajor>(T(1), mat_cview(a, rows, cols), mat_cview(z, cols, ell), T(0),
                                          mat_view(y, rows, ell), hd::Trans::None, hd::Trans::None, alloc);
        qr_explicit_q(alloc, y, rows, ell, q, col);
    }
    hd::gemm<T, hd::Layout::RowMajor>(T(1), mat_cview(q, rows, ell), mat_cview(a, rows, cols), T(0),
                                      mat_view(b, ell, cols), hd::Trans::Transpose, hd::Trans::None, alloc);
    // leading left singular vectors of B via the same Gram kernel (D(v14j)-4)
    hd::Symmetric<T> g(alloc, static_cast<crd::usize>(ell));
    hd::gemm<T, hd::Layout::RowMajor>(T(1), mat_cview(b, ell, cols), mat_cview(b, ell, cols), T(0),
                                      mat_view(g.data(), ell, ell), hd::Trans::None, hd::Trans::Transpose, alloc);
    const hd::EigSym<T> e = hd::eig_sym<T>(alloc, g); // values ascending
    // dst = Q * (top-r eigenvectors, descending)
    for (crd::u64 i = 0; i < rows; ++i)
    {
        for (crd::u64 j = 0; j < r; ++j)
        {
            T acc = T(0);
            for (crd::u64 t = 0; t < ell; ++t)
            {
                acc = std::fma(q[i * ell + t],
                               e.vectors.at(static_cast<crd::usize>(t), static_cast<crd::usize>(ell - 1U - j)),
                               acc);
            }
            dst[i * r + j] = acc;
        }
    }
    return DecompStatus::Ok;
}

// ---- mode-n tensor-times-matrix ---------------------------------------------
// trans == true : out = src x_mode U^T  (U [rows_u x cols_u], src extent rows_u
//                 at `mode` -> out extent cols_u)  — the projection direction.
// trans == false: out = src x_mode U    (src extent cols_u -> out rows_u) — the
//                 reconstruction direction.
// Pipeline: unfold (HPTT permute) -> gemm -> fold back (inverse permute), with
// the mode-0-contiguous fast path skipping both permutes.
template <typename T>
[[nodiscard]] inline TensorStatus ttm(TensorView<const T> src, const T* u, crd::u64 rows_u, crd::u64 cols_u,
                                      crd::u32 mode, bool trans, Tensor<T>& unfold_buf, Tensor<T>& prod_buf,
                                      Tensor<T>& out, crd::memory::IAllocator* alloc) noexcept
{
    namespace hd = crd::hesap::dense;
    const crd::u32 nd = src.rank();
    if (mode >= nd)
    {
        return TensorStatus::BadInput;
    }
    const crd::u64 in_extent = trans ? rows_u : cols_u;
    const crd::u64 out_extent = trans ? cols_u : rows_u;
    if (src.shape(mode) != in_extent)
    {
        return TensorStatus::ShapeMismatch;
    }
    crd::u64 p = 1;
    for (crd::u32 d = 0; d < nd; ++d)
    {
        if (d != mode)
        {
            p *= src.shape(d);
        }
    }
    const hd::Trans ta = trans ? hd::Trans::Transpose : hd::Trans::None;
    if (mode == 0U && src.is_contiguous())
    {
        // fast path: the mode-0 unfolding IS the storage; gemm straight into out
        crd::u64 oshape[kMaxRank];
        oshape[0] = out_extent;
        for (crd::u32 d = 1; d < nd; ++d)
        {
            oshape[d] = src.shape(d);
        }
        const TensorStatus st = out.resize({oshape, nd});
        if (st != TensorStatus::Ok)
        {
            return st;
        }
        hd::gemm<T, hd::Layout::RowMajor>(T(1), mat_cview(u, rows_u, cols_u), mat_cview(src.data(), in_extent, p),
                                          T(0), mat_view(out.data(), out_extent, p), ta, hd::Trans::None, alloc);
        return TensorStatus::Ok;
    }
    TensorStatus st = unfold_copy(src, mode, unfold_buf);
    if (st != TensorStatus::Ok)
    {
        return st;
    }
    crd::u64 pshape[kMaxRank];
    pshape[0] = out_extent;
    {
        crd::u32 o = 1;
        for (crd::u32 d = 0; d < nd; ++d)
        {
            if (d != mode)
            {
                pshape[o++] = src.shape(d);
            }
        }
    }
    st = prod_buf.resize({pshape, nd});
    if (st != TensorStatus::Ok)
    {
        return st;
    }
    hd::gemm<T, hd::Layout::RowMajor>(T(1), mat_cview(u, rows_u, cols_u),
                                      mat_cview(unfold_buf.data(), in_extent, p), T(0),
                                      mat_view(prod_buf.data(), out_extent, p), ta, hd::Trans::None, alloc);
    crd::u32 inv[kMaxRank];
    unfold_inverse_order(mode, nd, inv);
    return permute_copy<T>(TensorView<const T>(prod_buf.view()), {inv, nd}, out);
}

// Project src on every mode except `skip` (pass ndim or larger for none):
// out = src x_k U_k^T over k != skip, ascending (tensorly's order).
template <typename T>
[[nodiscard]] inline TensorStatus project_all(TensorView<const T> src, crd::containers::ConstSpan<Tensor<T>> factors,
                                              crd::containers::ConstSpan<crd::u64> ranks, crd::u32 skip,
                                              Tensor<T>& out, Tensor<T>& ping, Tensor<T>& unfold_buf,
                                              Tensor<T>& prod_buf, crd::memory::IAllocator* alloc) noexcept
{
    const crd::u32 nd = src.rank();
    Tensor<T>* bufs[2] = {&out, &ping};
    crd::u32 cur = 0;
    bool first = true;
    for (crd::u32 k = 0; k < nd; ++k)
    {
        if (k == skip)
        {
            continue;
        }
        TensorView<const T> in = first ? src : TensorView<const T>(bufs[1U - cur]->view());
        const TensorStatus st = ttm<T>(in, factors[k].data(), src.shape(k), ranks[k], k, true, unfold_buf,
                                       prod_buf, *bufs[cur], alloc);
        if (st != TensorStatus::Ok)
        {
            return st;
        }
        first = false;
        cur = 1U - cur;
    }
    if (first) // nothing applied (ndim == 1 with skip == 0): plain copy
    {
        crd::u32 order[kMaxRank];
        for (crd::u32 d = 0; d < nd; ++d)
        {
            order[d] = d;
        }
        return permute_copy<T>(src, {order, nd}, out);
    }
    // last write went into bufs[1 - cur] (cur was flipped after the write)
    if (cur == 0U) // result landed in `ping`: move it into `out`
    {
        out = static_cast<Tensor<T>&&>(ping);
    }
    return TensorStatus::Ok;
}

template <typename T>
[[nodiscard]] inline T norm_sq(const T* p, crd::u64 n) noexcept
{
    T s = T(0);
    for (crd::u64 i = 0; i < n; ++i)
    {
        s = std::fma(p[i], p[i], s);
    }
    return s;
}

} // namespace decompdetail

// =============================================================================
// CP-ALS
// =============================================================================

enum class CpInit : crd::u8
{
    Svd,    // leading left singular vectors of each mode unfolding (deterministic;
            // columns past min(I_n, P_n) padded with keyed Philox uniforms)
    Random, // keyed Philox uniforms [0,1) (tensorly's random-init class)
};

template <typename T>
struct CpOptions
{
    crd::u32 max_iters = 100U;
    T tol = T(1e-8); // |delta rec_error| stop rule (tensorly's); tol <= 0 => fixed budget, always Ok
    CpInit init = CpInit::Svd;
    crd::u64 seed = 0xCE41DDECULL;
    crd::containers::Span<T> fit_history{}; // optional: fit written per iteration while capacity lasts
};

template <typename T>
struct CpInfo
{
    crd::u32 iters = 0;
    T fit = T(0);       // 1 - rec_error
    T rec_error = T(0); // ||X - Xhat||_F / ||X||_F (via the gram identity)
    bool converged = false;
};

// -----------------------------------------------------------------------------
// THE MTTKRP SEAM (the v14-i sparse wiring point). CP-ALS touches the tensor
// ONLY through this functor contract:
//
//   TensorStatus operator()(crd::u32 mode, crd::containers::ConstSpan<Tensor<T>> factors,
//                           crd::u64 rank, crd::containers::Span<T> out) noexcept
//
//   out (size I_mode * rank, row-major [I_mode x rank]) must receive
//   M = X_(mode) * KhatriRao(factors[k], k != mode, ascending, last fastest)
//   — the C-order convention (tensorly's). `factors[k]` are the CURRENT
//   iterates, [I_k x rank] row-major, unit-norm columns.
//
// The sparse variant implements the same operator over its COO/CSF storage and
// calls cp_als_generic below; nothing else in the algorithm changes.
// -----------------------------------------------------------------------------
template <typename T>
class DenseMttkrp
{
public:
    // `x` is BORROWED for the duration of the cp_als call only (Span lifetime
    // discipline — never store this object beyond the call).
    DenseMttkrp(TensorView<const T> x, crd::memory::IAllocator* alloc) noexcept
        : m_x(x), m_alloc(alloc), m_kr(alloc)
    {
        for (crd::u32 d = 0; d < kMaxRank; ++d)
        {
            m_unfold[d] = Tensor<T>(alloc);
            m_built[d] = false;
        }
    }

    // Materialize (once) and expose the mode-n unfolding [I_mode x P] — hoisted
    // across ALS iterations (the wall-clock lever vs per-iteration unfolds).
    [[nodiscard]] TensorStatus ensure_unfold(crd::u32 mode) noexcept
    {
        if (m_built[mode])
        {
            return TensorStatus::Ok;
        }
        const TensorStatus st = decompdetail::unfold_copy(m_x, mode, m_unfold[mode]);
        if (st == TensorStatus::Ok)
        {
            m_built[mode] = true;
        }
        return st;
    }

    [[nodiscard]] const Tensor<T>& unfolding(crd::u32 mode) const noexcept { return m_unfold[mode]; }

    [[nodiscard]] TensorStatus operator()(crd::u32 mode, crd::containers::ConstSpan<Tensor<T>> factors,
                                          crd::u64 rank, crd::containers::Span<T> out) noexcept
    {
        namespace hd = crd::hesap::dense;
        const crd::u32 nd = m_x.rank();
        TensorStatus st = ensure_unfold(mode);
        if (st != TensorStatus::Ok)
        {
            return st;
        }
        const crd::u64 rows = m_x.shape(mode);
        const crd::u64 p = m_unfold[mode].size() / (rows > 0U ? rows : 1U);
        if (out.size() != rows * rank)
        {
            return TensorStatus::BadInput;
        }
        // Khatri-Rao of the other factors, built back-to-front in ONE buffer:
        // the last included mode varies fastest (C-order match with the unfold).
        const crd::u64 kshape[2] = {p, rank};
        st = m_kr.resize({kshape, 2U});
        if (st != TensorStatus::Ok)
        {
            return st;
        }
        T* kr = m_kr.data();
        crd::u64 cur_rows = 0;
        bool first = true;
        for (crd::u32 k = nd; k-- > 0U;)
        {
            if (k == mode)
            {
                continue;
            }
            const T* fk = factors[k].data();
            const crd::u64 ik = m_x.shape(k);
            if (first)
            {
                for (crd::u64 e = 0; e < ik * rank; ++e)
                {
                    kr[e] = fk[e];
                }
                cur_rows = ik;
                first = false;
                continue;
            }
            for (crd::u64 i = ik; i-- > 1U;) // descending: block 0 stays readable until last
            {
                T* dstrow = kr + i * cur_rows * rank;
                for (crd::u64 j = 0; j < cur_rows; ++j)
                {
                    for (crd::u64 r = 0; r < rank; ++r)
                    {
                        dstrow[j * rank + r] = fk[i * rank + r] * kr[j * rank + r];
                    }
                }
            }
            for (crd::u64 j = 0; j < cur_rows; ++j) // i == 0: in-place scale
            {
                for (crd::u64 r = 0; r < rank; ++r)
                {
                    kr[j * rank + r] *= fk[r];
                }
            }
            cur_rows *= ik;
        }
        hd::gemm<T, hd::Layout::RowMajor>(T(1), decompdetail::mat_cview(m_unfold[mode].data(), rows, p),
                                          decompdetail::mat_cview(kr, p, rank), T(0),
                                          decompdetail::mat_view(out.data(), rows, rank), hd::Trans::None,
                                          hd::Trans::None, m_alloc); // allocator propagation, no hidden scratch
        return TensorStatus::Ok;
    }

private:
    TensorView<const T> m_x;
    crd::memory::IAllocator* m_alloc = nullptr;
    Tensor<T> m_unfold[kMaxRank];
    Tensor<T> m_kr;
    bool m_built[kMaxRank] = {};
};

// -----------------------------------------------------------------------------
// cp_als_generic — the ALS core over the MTTKRP seam. `factors` must arrive
// sized [shape[n] x rank] row-major and INITIALIZED (the dense wrapper below
// does SVD/Philox init; the sparse integrator brings its own). weights size ==
// rank. Bounded: runs at most opts.max_iters sweeps; with tol > 0 returns
// NotConverged when the budget ends before |delta rec_error| < tol.
// -----------------------------------------------------------------------------
template <typename T, typename Mttkrp>
[[nodiscard]] DecompStatus cp_als_generic(crd::containers::ConstSpan<crd::u64> shape, T x_norm_sq, Mttkrp& mttkrp,
                                          crd::u64 rank, crd::containers::Span<Tensor<T>> factors,
                                          crd::containers::Span<T> weights, CpInfo<T>& info,
                                          crd::memory::IAllocator* alloc, const CpOptions<T>& opts) noexcept
{
    using namespace decompdetail;
    const crd::u32 nd = static_cast<crd::u32>(shape.size());
    info = CpInfo<T>{};
    if (nd < 2U || nd > kMaxRank || rank == 0U || factors.size() != nd || weights.size() != rank ||
        opts.max_iters == 0U || alloc == nullptr)
    {
        return DecompStatus::BadInput;
    }
    crd::u64 imax = 0;
    for (crd::u32 n = 0; n < nd; ++n)
    {
        if (shape[n] == 0U)
        {
            return DecompStatus::BadInput;
        }
        if (factors[n].rank() != 2U || factors[n].shape(0) != shape[n] || factors[n].shape(1) != rank)
        {
            return DecompStatus::ShapeMismatch;
        }
        if (shape[n] > imax)
        {
            imax = shape[n];
        }
    }
    if (!(x_norm_sq > T(0))) // all-zeros input: exact zero model, done
    {
        for (crd::u64 r = 0; r < rank; ++r)
        {
            weights[r] = T(0);
        }
        info.fit = T(1);
        info.rec_error = T(0);
        info.converged = true;
        info.iters = 0;
        return DecompStatus::Ok;
    }
    crd::containers::Array<T> mbuf(alloc);      // MTTKRP result [imax x rank]
    crd::containers::Array<T> fbuf(alloc);      // unnormalized solve output [imax x rank]
    crd::containers::Array<T> grams(alloc);     // per-mode gramians [nd][rank x rank]
    crd::containers::Array<T> vbuf(alloc);      // hadamard V
    crd::containers::Array<T> vwork(alloc);     // V working copy (destroyed by the solver)
    crd::containers::Array<T> gtmp(alloc);      // last-mode unnormalized gram (fit identity)
    crd::containers::Array<T> jscratch(alloc);  // solver scratch: 2r^2 + 3r
    mbuf.resize(static_cast<crd::usize>(imax * rank));
    fbuf.resize(static_cast<crd::usize>(imax * rank));
    grams.resize(static_cast<crd::usize>(nd * rank * rank));
    vbuf.resize(static_cast<crd::usize>(rank * rank));
    vwork.resize(static_cast<crd::usize>(rank * rank));
    gtmp.resize(static_cast<crd::usize>(rank * rank));
    jscratch.resize(static_cast<crd::usize>(2U * rank * rank + 3U * rank));
    for (crd::u32 n = 0; n < nd; ++n)
    {
        gram(factors[n].data(), shape[n], rank, grams.data() + static_cast<crd::usize>(n) * rank * rank);
    }
    const T norm_x = std::sqrt(x_norm_sq);
    T prev_rec = T(-1);
    T rec = T(0);
    for (crd::u32 iter = 0; iter < opts.max_iters; ++iter)
    {
        T iprod = T(0);
        T model_sq = T(0);
        for (crd::u32 n = 0; n < nd; ++n)
        {
            const crd::u64 rows = shape[n];
            const TensorStatus mst = mttkrp(n, {factors.data(), nd}, rank, {mbuf.data(), rows * rank});
            if (mst != TensorStatus::Ok)
            {
                return to_decomp(mst);
            }
            for (crd::u64 e = 0; e < rank * rank; ++e) // V = hadamard of the other gramians
            {
                T v = T(1);
                for (crd::u32 k = 0; k < nd; ++k)
                {
                    if (k != n)
                    {
                        v *= grams[static_cast<crd::usize>(k) * rank * rank + e];
                    }
                }
                vbuf[e] = v;
                vwork[e] = v;
            }
            solve_rows_sym(mbuf.data(), fbuf.data(), rows, vwork.data(), rank, jscratch.data());
            if (n == nd - 1U) // fit identity terms on the unnormalized last factor
            {
                for (crd::u64 e = 0; e < rows * rank; ++e)
                {
                    iprod = std::fma(mbuf[e], fbuf[e], iprod);
                }
                gram(fbuf.data(), rows, rank, gtmp.data());
                for (crd::u64 e = 0; e < rank * rank; ++e)
                {
                    model_sq = std::fma(vbuf[e], gtmp[e], model_sq);
                }
            }
            T* f = factors[n].data(); // normalize columns -> weights (D(v14j)-1)
            for (crd::u64 r = 0; r < rank; ++r)
            {
                T ss = T(0);
                for (crd::u64 i = 0; i < rows; ++i)
                {
                    ss = std::fma(fbuf[i * rank + r], fbuf[i * rank + r], ss);
                }
                const T lam = std::sqrt(ss);
                weights[r] = lam;
                const T inv = lam > T(0) ? T(1) / lam : T(1);
                for (crd::u64 i = 0; i < rows; ++i)
                {
                    f[i * rank + r] = fbuf[i * rank + r] * inv;
                }
            }
            gram(f, rows, rank, grams.data() + static_cast<crd::usize>(n) * rank * rank);
        }
        T e2 = x_norm_sq - T(2) * iprod + model_sq;
        if (e2 < T(0))
        {
            e2 = T(0);
        }
        rec = std::sqrt(e2) / norm_x;
        info.iters = iter + 1U;
        info.rec_error = rec;
        info.fit = T(1) - rec;
        if (static_cast<crd::u64>(iter) < opts.fit_history.size())
        {
            opts.fit_history[iter] = info.fit;
        }
        if (opts.tol > T(0) && iter > 0U)
        {
            const T d = prev_rec > rec ? prev_rec - rec : rec - prev_rec;
            if (d < opts.tol)
            {
                info.converged = true;
                return DecompStatus::Ok;
            }
        }
        prev_rec = rec;
    }
    info.converged = opts.tol <= T(0); // fixed-budget mode is a completed contract
    return info.converged ? DecompStatus::Ok : DecompStatus::NotConverged;
}

// -----------------------------------------------------------------------------
// cp_als — the dense entry point: hoisted unfoldings (DenseMttkrp), init per
// opts (SVD of unfoldings / keyed Philox), then cp_als_generic. `factors` are
// resized here; weights.size() == rank.
// -----------------------------------------------------------------------------
template <typename T>
[[nodiscard]] DecompStatus cp_als(TensorView<const T> x, crd::u64 rank, crd::containers::Span<Tensor<T>> factors,
                                  crd::containers::Span<T> weights, CpInfo<T>& info, crd::memory::IAllocator* alloc,
                                  const CpOptions<T>& opts = {}) noexcept
{
    using namespace decompdetail;
    const crd::u32 nd = x.rank();
    info = CpInfo<T>{};
    if (nd < 2U || rank == 0U || factors.size() != nd || weights.size() != rank || alloc == nullptr)
    {
        return DecompStatus::BadInput;
    }
    for (crd::u32 n = 0; n < nd; ++n)
    {
        if (x.shape(n) == 0U)
        {
            return DecompStatus::BadInput;
        }
        const crd::u64 fshape[2] = {x.shape(n), rank};
        const TensorStatus st = factors[n].resize({fshape, 2U});
        if (st != TensorStatus::Ok)
        {
            return to_decomp(st);
        }
    }
    DenseMttkrp<T> mttkrp(x, alloc);
    // ||X||^2 from the (hoisted) mode-0 unfolding — contiguous scan
    {
        const TensorStatus st = mttkrp.ensure_unfold(0U);
        if (st != TensorStatus::Ok)
        {
            return to_decomp(st);
        }
    }
    const T x_norm_sq = norm_sq(mttkrp.unfolding(0U).data(), mttkrp.unfolding(0U).size());
    for (crd::u32 n = 0; n < nd; ++n) // init
    {
        T* f = factors[n].data();
        const crd::u64 rows = x.shape(n);
        if (opts.init == CpInit::Random)
        {
            for (crd::u64 e = 0; e < rows * rank; ++e)
            {
                f[e] = philox_uniform<T>(opts.seed, n, e);
            }
            continue;
        }
        const TensorStatus st = mttkrp.ensure_unfold(n);
        if (st != TensorStatus::Ok)
        {
            return to_decomp(st);
        }
        const crd::u64 p = mttkrp.unfolding(n).size() / rows;
        const crd::u64 minmn = rows < p ? rows : p;
        const crd::u64 rsvd_cols = rank < minmn ? rank : minmn;
        if (x_norm_sq > T(0))
        {
            crd::containers::Array<T> ubuf(alloc);
            ubuf.resize(static_cast<crd::usize>(rows * rsvd_cols));
            svd_leading_columns(alloc, mttkrp.unfolding(n).data(), rows, p, rsvd_cols, ubuf.data());
            for (crd::u64 i = 0; i < rows; ++i)
            {
                for (crd::u64 r = 0; r < rsvd_cols; ++r)
                {
                    f[i * rank + r] = ubuf[i * rsvd_cols + r];
                }
            }
        }
        else
        {
            for (crd::u64 i = 0; i < rows; ++i) // zero tensor: identity-like columns
            {
                for (crd::u64 r = 0; r < rsvd_cols; ++r)
                {
                    f[i * rank + r] = i == r ? T(1) : T(0);
                }
            }
        }
        for (crd::u64 i = 0; i < rows; ++i) // pad columns past min(I_n, P_n) with keyed draws
        {
            for (crd::u64 r = rsvd_cols; r < rank; ++r)
            {
                f[i * rank + r] = philox_uniform<T>(opts.seed, n, i * rank + r);
            }
        }
    }
    return cp_als_generic<T, DenseMttkrp<T>>({x.shape().data(), nd}, x_norm_sq, mttkrp, rank, factors, weights,
                                             info, alloc, opts);
}

// cp_reconstruct — materialize Xhat = sum_r w_r a_r(0) o ... o a_r(N-1) into
// `out` (resized to the factor shapes). One KR build + one gemm: the mode-0
// unfolding of the model IS the row-major storage.
template <typename T>
[[nodiscard]] DecompStatus cp_reconstruct(crd::containers::ConstSpan<T> weights,
                                          crd::containers::ConstSpan<Tensor<T>> factors, Tensor<T>& out,
                                          crd::memory::IAllocator* alloc) noexcept
{
    using namespace decompdetail;
    namespace hd = crd::hesap::dense;
    const crd::u32 nd = static_cast<crd::u32>(factors.size());
    if (nd < 2U || nd > kMaxRank || alloc == nullptr)
    {
        return DecompStatus::BadInput;
    }
    const crd::u64 rank = weights.size();
    crd::u64 shape[kMaxRank];
    crd::u64 p = 1;
    for (crd::u32 n = 0; n < nd; ++n)
    {
        if (factors[n].rank() != 2U || factors[n].shape(1) != rank || rank == 0U)
        {
            return DecompStatus::ShapeMismatch;
        }
        shape[n] = factors[n].shape(0);
        if (n > 0U)
        {
            p *= shape[n];
        }
    }
    TensorStatus st = out.resize({shape, nd});
    if (st != TensorStatus::Ok)
    {
        return to_decomp(st);
    }
    Tensor<T> kr(alloc);
    const crd::u64 kshape[2] = {p, rank};
    st = kr.resize({kshape, 2U});
    if (st != TensorStatus::Ok)
    {
        return to_decomp(st);
    }
    T* k = kr.data();
    crd::u64 cur_rows = 0;
    bool first = true;
    for (crd::u32 m = nd; m-- > 1U;) // KR of modes 1..N-1, last fastest
    {
        const T* fm = factors[m].data();
        const crd::u64 im = shape[m];
        if (first)
        {
            for (crd::u64 e = 0; e < im * rank; ++e)
            {
                k[e] = fm[e];
            }
            cur_rows = im;
            first = false;
            continue;
        }
        for (crd::u64 i = im; i-- > 1U;)
        {
            T* dstrow = k + i * cur_rows * rank;
            for (crd::u64 j = 0; j < cur_rows; ++j)
            {
                for (crd::u64 r = 0; r < rank; ++r)
                {
                    dstrow[j * rank + r] = fm[i * rank + r] * k[j * rank + r];
                }
            }
        }
        for (crd::u64 j = 0; j < cur_rows; ++j)
        {
            for (crd::u64 r = 0; r < rank; ++r)
            {
                k[j * rank + r] *= fm[r];
            }
        }
        cur_rows *= im;
    }
    Tensor<T> aw(alloc); // factor 0 with the weights folded in
    const crd::u64 ashape[2] = {shape[0], rank};
    st = aw.resize({ashape, 2U});
    if (st != TensorStatus::Ok)
    {
        return to_decomp(st);
    }
    const T* f0 = factors[0].data();
    for (crd::u64 i = 0; i < shape[0]; ++i)
    {
        for (crd::u64 r = 0; r < rank; ++r)
        {
            aw.data()[i * rank + r] = f0[i * rank + r] * weights[r];
        }
    }
    hd::gemm<T, hd::Layout::RowMajor>(T(1), mat_cview(aw.data(), shape[0], rank), mat_cview(k, p, rank), T(0),
                                      mat_view(out.data(), shape[0], p), hd::Trans::None, hd::Trans::Transpose, alloc);
    return DecompStatus::Ok;
}

// =============================================================================
// Tucker: HOSVD / HOOI (+ the deterministic-randomized variants)
// =============================================================================

template <typename T>
struct TuckerOptions
{
    crd::u32 max_iters = 50U;
    T tol = T(1e-10); // |delta rec_error| stop rule; tol <= 0 => fixed budget, always Ok
};

struct RandOptions
{
    crd::u64 oversample = 8U;
    crd::u32 power_iters = 2U;
    crd::u64 seed = 0xCE41DDECULL;
};

template <typename T>
struct TuckerInfo
{
    crd::u32 iters = 0;
    T fit = T(0);
    T rec_error = T(0); // the norm identity (D(v14j)-2)
    bool converged = false;
};

namespace decompdetail
{

// Shared HOSVD factor pass: per mode, leading R_n left singular vectors of the
// mode-n unfolding — exact (`rand == false`) or the Philox-keyed randomized
// range finder (`rand == true`, stream = kSketchStream + mode).
template <typename T>
[[nodiscard]] inline DecompStatus hosvd_factors(TensorView<const T> x, crd::containers::ConstSpan<crd::u64> ranks,
                                                crd::containers::Span<Tensor<T>> factors,
                                                crd::memory::IAllocator* alloc, bool rand,
                                                const RandOptions& ropts) noexcept
{
    const crd::u32 nd = x.rank();
    Tensor<T> ubuf(alloc);
    for (crd::u32 n = 0; n < nd; ++n)
    {
        const crd::u64 rows = x.shape(n);
        const crd::u64 p = x.size() / rows;
        const crd::u64 minmn = rows < p ? rows : p;
        if (ranks[n] == 0U || ranks[n] > minmn)
        {
            return DecompStatus::BadInput;
        }
        const crd::u64 fshape[2] = {rows, ranks[n]};
        TensorStatus st = factors[n].resize({fshape, 2U});
        if (st != TensorStatus::Ok)
        {
            return to_decomp(st);
        }
        st = unfold_copy(x, n, ubuf);
        if (st != TensorStatus::Ok)
        {
            return to_decomp(st);
        }
        if (rand)
        {
            const DecompStatus ds =
                rsvd_leading_columns(alloc, ubuf.data(), rows, p, ranks[n], ropts.oversample, ropts.power_iters,
                                     ropts.seed, kSketchStream + n, factors[n].data());
            if (ds != DecompStatus::Ok)
            {
                return ds;
            }
        }
        else
        {
            svd_leading_columns(alloc, ubuf.data(), rows, p, ranks[n], factors[n].data());
        }
    }
    return DecompStatus::Ok;
}

// Zero-input guard shared by every Tucker entry: identity-like factors, zero
// core, exact fit (dense SVD machinery is undefined on an all-zero unfolding).
template <typename T>
[[nodiscard]] inline DecompStatus tucker_zero_input(TensorView<const T> x,
                                                    crd::containers::ConstSpan<crd::u64> ranks,
                                                    crd::containers::Span<Tensor<T>> factors, Tensor<T>& core,
                                                    TuckerInfo<T>* info) noexcept
{
    const crd::u32 nd = x.rank();
    crd::u64 cshape[kMaxRank];
    for (crd::u32 n = 0; n < nd; ++n)
    {
        const crd::u64 rows = x.shape(n);
        const crd::u64 p = x.size() / (rows > 0U ? rows : 1U);
        const crd::u64 minmn = rows < p ? rows : p;
        if (ranks[n] == 0U || ranks[n] > minmn)
        {
            return DecompStatus::BadInput;
        }
        const crd::u64 fshape[2] = {rows, ranks[n]};
        const TensorStatus st = factors[n].resize({fshape, 2U});
        if (st != TensorStatus::Ok)
        {
            return to_decomp(st);
        }
        T* f = factors[n].data();
        for (crd::u64 i = 0; i < rows; ++i)
        {
            for (crd::u64 r = 0; r < ranks[n]; ++r)
            {
                f[i * ranks[n] + r] = i == r ? T(1) : T(0);
            }
        }
        cshape[n] = ranks[n];
    }
    const TensorStatus st = core.resize({cshape, nd});
    if (st != TensorStatus::Ok)
    {
        return to_decomp(st);
    }
    core.zero();
    if (info != nullptr)
    {
        info->iters = 0;
        info->rec_error = T(0);
        info->fit = T(1);
        info->converged = true;
    }
    return DecompStatus::Ok;
}

template <typename T>
[[nodiscard]] inline bool tucker_validate(TensorView<const T> x, crd::containers::ConstSpan<crd::u64> ranks,
                                          crd::containers::Span<Tensor<T>> factors,
                                          crd::memory::IAllocator* alloc) noexcept
{
    const crd::u32 nd = x.rank();
    if (nd < 2U || nd > kMaxRank || ranks.size() != nd || factors.size() != nd || alloc == nullptr)
    {
        return false;
    }
    for (crd::u32 n = 0; n < nd; ++n)
    {
        if (x.shape(n) == 0U)
        {
            return false;
        }
    }
    return true;
}

// Shared HOSVD driver (exact or randomized factor pass) + core + identity error.
template <typename T>
[[nodiscard]] inline DecompStatus hosvd_impl(TensorView<const T> x, crd::containers::ConstSpan<crd::u64> ranks,
                                             crd::containers::Span<Tensor<T>> factors, Tensor<T>& core,
                                             crd::memory::IAllocator* alloc, bool rand, const RandOptions& ropts,
                                             TuckerInfo<T>* info) noexcept
{
    if (!tucker_validate(x, ranks, factors, alloc))
    {
        return DecompStatus::BadInput;
    }
    Tensor<T> probe(alloc); // contiguous copy doubles as the ||X||^2 scan
    {
        crd::u32 order[kMaxRank];
        for (crd::u32 d = 0; d < x.rank(); ++d)
        {
            order[d] = d;
        }
        const TensorStatus st = permute_copy<T>(x, {order, x.rank()}, probe);
        if (st != TensorStatus::Ok)
        {
            return to_decomp(st);
        }
    }
    const T x_norm_sq = norm_sq(probe.data(), probe.size());
    if (!(x_norm_sq > T(0)))
    {
        return tucker_zero_input(x, ranks, factors, core, info);
    }
    const DecompStatus ds = hosvd_factors(TensorView<const T>(probe.view()), ranks, factors, alloc, rand, ropts);
    if (ds != DecompStatus::Ok)
    {
        return ds;
    }
    Tensor<T> ping(alloc);
    Tensor<T> ubuf(alloc);
    Tensor<T> pbuf(alloc);
    const TensorStatus st = project_all(TensorView<const T>(probe.view()), {factors.data(), factors.size()},
                                        ranks, x.rank(), core, ping, ubuf, pbuf, alloc);
    if (st != TensorStatus::Ok)
    {
        return to_decomp(st);
    }
    if (info != nullptr)
    {
        T e2 = x_norm_sq - norm_sq(core.data(), core.size());
        if (e2 < T(0))
        {
            e2 = T(0);
        }
        info->rec_error = std::sqrt(e2) / std::sqrt(x_norm_sq);
        info->fit = T(1) - info->rec_error;
        info->iters = 0;
        info->converged = true;
    }
    return DecompStatus::Ok;
}

// Shared HOOI driver: HOSVD init, then bounded ALS on the Tucker core
// (tensorly's sweep order + error formula), exact or randomized mode updates.
template <typename T>
[[nodiscard]] inline DecompStatus hooi_impl(TensorView<const T> x, crd::containers::ConstSpan<crd::u64> ranks,
                                            crd::containers::Span<Tensor<T>> factors, Tensor<T>& core,
                                            TuckerInfo<T>& info, crd::memory::IAllocator* alloc, bool rand,
                                            const RandOptions& ropts, const TuckerOptions<T>& opts) noexcept
{
    info = TuckerInfo<T>{};
    if (!tucker_validate(x, ranks, factors, alloc) || opts.max_iters == 0U)
    {
        return DecompStatus::BadInput;
    }
    const crd::u32 nd = x.rank();
    Tensor<T> probe(alloc);
    {
        crd::u32 order[kMaxRank];
        for (crd::u32 d = 0; d < nd; ++d)
        {
            order[d] = d;
        }
        const TensorStatus st = permute_copy<T>(x, {order, nd}, probe);
        if (st != TensorStatus::Ok)
        {
            return to_decomp(st);
        }
    }
    const T x_norm_sq = norm_sq(probe.data(), probe.size());
    if (!(x_norm_sq > T(0)))
    {
        return tucker_zero_input(x, ranks, factors, core, &info);
    }
    // HOOI mode-update feasibility: R_n <= prod_{k != n} R_k (the projected
    // unfolding is [I_n x prod R_k]) on top of the HOSVD bound.
    for (crd::u32 n = 0; n < nd; ++n)
    {
        crd::u64 pr = 1;
        for (crd::u32 k = 0; k < nd; ++k)
        {
            if (k != n)
            {
                pr *= ranks[k];
            }
        }
        if (ranks[n] > pr)
        {
            return DecompStatus::BadInput;
        }
    }
    DecompStatus ds = hosvd_factors(TensorView<const T>(probe.view()), ranks, factors, alloc, rand, ropts);
    if (ds != DecompStatus::Ok)
    {
        return ds;
    }
    Tensor<T> proj(alloc);
    Tensor<T> ping(alloc);
    Tensor<T> ubuf(alloc);
    Tensor<T> pbuf(alloc);
    const T norm_x = std::sqrt(x_norm_sq);
    T prev_rec = T(-1);
    for (crd::u32 iter = 0; iter < opts.max_iters; ++iter)
    {
        for (crd::u32 n = 0; n < nd; ++n)
        {
            TensorStatus st = project_all(TensorView<const T>(probe.view()), {factors.data(), factors.size()},
                                          ranks, n, proj, ping, ubuf, pbuf, alloc);
            if (st != TensorStatus::Ok)
            {
                return to_decomp(st);
            }
            st = unfold_copy(TensorView<const T>(proj.view()), n, ubuf);
            if (st != TensorStatus::Ok)
            {
                return to_decomp(st);
            }
            const crd::u64 rows = x.shape(n);
            const crd::u64 p = ubuf.size() / rows;
            if (rand)
            {
                ds = rsvd_leading_columns(alloc, ubuf.data(), rows, p, ranks[n], ropts.oversample,
                                          ropts.power_iters, ropts.seed, kSketchStream + n, factors[n].data());
                if (ds != DecompStatus::Ok)
                {
                    return ds;
                }
            }
            else
            {
                svd_leading_columns(alloc, ubuf.data(), rows, p, ranks[n], factors[n].data());
            }
        }
        // core from the last projection (skip = nd-1) + its fresh factor —
        // identical to a fresh full projection (orthogonal projections commute)
        const crd::u32 last = nd - 1U;
        const TensorStatus st = ttm(TensorView<const T>(proj.view()), factors[last].data(), x.shape(last),
                                    ranks[last], last, true, ubuf, pbuf, core, alloc);
        if (st != TensorStatus::Ok)
        {
            return to_decomp(st);
        }
        T e2 = x_norm_sq - norm_sq(core.data(), core.size());
        if (e2 < T(0))
        {
            e2 = T(0);
        }
        const T rec = std::sqrt(e2) / norm_x;
        info.iters = iter + 1U;
        info.rec_error = rec;
        info.fit = T(1) - rec;
        if (opts.tol > T(0) && iter > 0U)
        {
            const T d = prev_rec > rec ? prev_rec - rec : rec - prev_rec;
            if (d < opts.tol)
            {
                info.converged = true;
                return DecompStatus::Ok;
            }
        }
        prev_rec = rec;
    }
    info.converged = opts.tol <= T(0);
    return info.converged ? DecompStatus::Ok : DecompStatus::NotConverged;
}

} // namespace decompdetail

// hosvd — truncated multilinear SVD: factors[n] = leading ranks[n] left
// singular vectors of the mode-n unfolding (hesap-dense SVD), core =
// X x_0 U_0^T x_1 U_1^T ... Requires 1 <= ranks[n] <= min(I_n, prod I_k).
template <typename T>
[[nodiscard]] DecompStatus hosvd(TensorView<const T> x, crd::containers::ConstSpan<crd::u64> ranks,
                                 crd::containers::Span<Tensor<T>> factors, Tensor<T>& core,
                                 crd::memory::IAllocator* alloc, TuckerInfo<T>* info = nullptr) noexcept
{
    return decompdetail::hosvd_impl(x, ranks, factors, core, alloc, false, RandOptions{}, info);
}

// hooi — Tucker via higher-order orthogonal iteration: HOSVD init, then per
// mode U_n = leading singular vectors of the all-but-n projection; bounded.
template <typename T>
[[nodiscard]] DecompStatus hooi(TensorView<const T> x, crd::containers::ConstSpan<crd::u64> ranks,
                                crd::containers::Span<Tensor<T>> factors, Tensor<T>& core, TuckerInfo<T>& info,
                                crd::memory::IAllocator* alloc, const TuckerOptions<T>& opts = {}) noexcept
{
    return decompdetail::hooi_impl(x, ranks, factors, core, info, alloc, false, RandOptions{}, opts);
}

// hosvd_rand / hooi_rand — the rSVD-based variants: every mode update rides the
// Philox-keyed randomized range finder. Same seed => bit-identical results at
// ANY worker count (deterministic-randomized; gated).
template <typename T>
[[nodiscard]] DecompStatus hosvd_rand(TensorView<const T> x, crd::containers::ConstSpan<crd::u64> ranks,
                                      crd::containers::Span<Tensor<T>> factors, Tensor<T>& core,
                                      crd::memory::IAllocator* alloc, const RandOptions& ropts = {},
                                      TuckerInfo<T>* info = nullptr) noexcept
{
    return decompdetail::hosvd_impl(x, ranks, factors, core, alloc, true, ropts, info);
}

template <typename T>
[[nodiscard]] DecompStatus hooi_rand(TensorView<const T> x, crd::containers::ConstSpan<crd::u64> ranks,
                                     crd::containers::Span<Tensor<T>> factors, Tensor<T>& core,
                                     TuckerInfo<T>& info, crd::memory::IAllocator* alloc,
                                     const RandOptions& ropts = {}, const TuckerOptions<T>& opts = {}) noexcept
{
    return decompdetail::hooi_impl(x, ranks, factors, core, info, alloc, true, ropts, opts);
}

// tucker_reconstruct — materialize Xhat = core x_0 U_0 x_1 U_1 ... into `out`.
template <typename T>
[[nodiscard]] DecompStatus tucker_reconstruct(const Tensor<T>& core, crd::containers::ConstSpan<Tensor<T>> factors,
                                              Tensor<T>& out, crd::memory::IAllocator* alloc) noexcept
{
    using namespace decompdetail;
    const crd::u32 nd = core.rank();
    if (nd < 2U || nd > kMaxRank || factors.size() != nd || alloc == nullptr)
    {
        return DecompStatus::BadInput;
    }
    for (crd::u32 n = 0; n < nd; ++n)
    {
        if (factors[n].rank() != 2U || factors[n].shape(1) != core.shape(n))
        {
            return DecompStatus::ShapeMismatch;
        }
    }
    Tensor<T> ping(alloc);
    Tensor<T> ubuf(alloc);
    Tensor<T> pbuf(alloc);
    Tensor<T> cur(alloc);
    {
        crd::u32 order[kMaxRank];
        for (crd::u32 d = 0; d < nd; ++d)
        {
            order[d] = d;
        }
        const TensorStatus st = permute_copy<T>(TensorView<const T>(core.view()), {order, nd}, cur);
        if (st != TensorStatus::Ok)
        {
            return to_decomp(st);
        }
    }
    for (crd::u32 n = 0; n < nd; ++n)
    {
        const TensorStatus st = ttm(TensorView<const T>(cur.view()), factors[n].data(), factors[n].shape(0),
                                    core.shape(n), n, false, ubuf, pbuf, ping, alloc);
        if (st != TensorStatus::Ok)
        {
            return to_decomp(st);
        }
        cur = static_cast<Tensor<T>&&>(ping);
        ping = Tensor<T>(alloc);
    }
    out = static_cast<Tensor<T>&&>(cur);
    return DecompStatus::Ok;
}

} // namespace crd::hesap::tensor
