#pragma once

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/complex.hpp>
#include <crd/hesap/dense/blas1.hpp>
#include <crd/hesap/dense/real_type.hpp>
#include <crd/hesap/dense/vector.hpp>
#include <crd/hesap/iterative/cg.hpp> // detail::krylov_conj / krylov_mag / krylov_real / krylov_smlnum
#include <crd/hesap/iterative/iterative_result.hpp>
#include <crd/hesap/iterative/stopping.hpp>
#include <crd/hesap/sparse/block_linear_op.hpp>
#include <crd/memory/allocator.hpp>

#include <cmath>
#include <limits>

namespace crd::hesap::iterative
{
// -----------------------------------------------------------------------
// Block-CG (multi-RHS) -- classic O'Leary block-CG (Galerkin form, "The block
// conjugate gradient algorithm and related methods", 1980). Phase 3.1.6 v4f-2.
//
// Solves A·X = B for s right-hand sides at once (X, B are n×s), A symmetric/
// Hermitian POSITIVE-DEFINITE. The s columns share one Krylov space ⇒ (a) one
// spmm per step instead of s spmvs — an s× reuse of the A-scan over Eigen's
// per-column CG (Eigen has no block algorithm), the throughput lever on an
// expensive operator; (b) information shared across the block ⇒ often FEWER
// iterations per column than independent CG.
//
// Per step (P NOT orthonormalised — the search block is carried directly):
//   AP   = A·P                       (one spmm)
//   γ    = (PᴴAP)⁻¹ (PᴴR)            (s×s SPD solve, same PᴴAP for both solves)
//   X   += P·γ ;  R -= AP·γ
//   δ    = -(PᴴAP)⁻¹ (Pᴴ A R)        (s×s SPD solve; PᴴAR = (AP)ᴴR)
//   P    = R + P·δ
//
// RANK DEFICIENCY (converged / duplicate columns ⇒ PᴴAP singular): the s×s
// coefficient solve falls back from plain Cholesky to a regularized Cholesky
// (M + εI Tikhonov) that damps the deflated near-zero subspace symmetrically —
// no explicit QR/deflation bookkeeping. This is the breakdown-free path the
// per-step Householder QR used to provide; dropping the QR removes O(n·s²) work
// per step that the regularization replaces only on the (rare) deficient solve.
//
// Determinism: the only parallel step is the operator's block spmm (bit-exact
// across threads, v1e). The s×s block reductions (PᴴAP etc.) and the n×s block
// gemms are computed serially on the calling thread (row-streaming over the n×s
// blocks) ⇒ thread-count-independent; the s×s Cholesky is deterministic. So the
// whole solve is bit-identical across thread counts. Blocks are ROW-MAJOR (n×s,
// stride s) to match spmm.
// -----------------------------------------------------------------------

namespace detail
{
// Solve the s×s Hermitian-positive-definite system M·x = rhs for `nrhs` columns
// (rhs is s×nrhs row-major, overwritten with the solution). In-place Cholesky
// M = LLᴴ + forward/back substitution. M is destroyed. Allocation-free.
template <typename T>
inline bool block_spd_solve(T* m, crd::u32 s, T* rhs, crd::u32 nrhs) noexcept
{
    using R = crd::hesap::dense::RealType<T>;
    const R smin = detail::krylov_smlnum<R>();
    // Cholesky: M = L Lᴴ (lower L overwrites the lower triangle of m).
    for (crd::u32 i = 0; i < s; ++i)
    {
        for (crd::u32 j = 0; j <= i; ++j)
        {
            T sum = m[static_cast<crd::usize>(i) * s + j];
            for (crd::u32 k = 0; k < j; ++k)
            {
                sum = sum - m[static_cast<crd::usize>(i) * s + k] * detail::krylov_conj<T>(m[static_cast<crd::usize>(j) * s + k]);
            }
            if (i == j)
            {
                const R d = detail::krylov_real<T>(sum);
                if (d <= smin)
                {
                    return false; // not SPD (should not happen for orthonormal P, SPD A)
                }
                m[static_cast<crd::usize>(i) * s + j] = T(std::sqrt(d));
            }
            else
            {
                m[static_cast<crd::usize>(i) * s + j] = sum / m[static_cast<crd::usize>(j) * s + j];
            }
        }
    }
    // Solve L y = rhs, then Lᴴ x = y, per RHS column.
    for (crd::u32 c = 0; c < nrhs; ++c)
    {
        for (crd::u32 i = 0; i < s; ++i)
        {
            T sum = rhs[static_cast<crd::usize>(i) * nrhs + c];
            for (crd::u32 k = 0; k < i; ++k)
            {
                sum = sum - m[static_cast<crd::usize>(i) * s + k] * rhs[static_cast<crd::usize>(k) * nrhs + c];
            }
            rhs[static_cast<crd::usize>(i) * nrhs + c] = sum / m[static_cast<crd::usize>(i) * s + i];
        }
        for (crd::u32 ii = 0; ii < s; ++ii)
        {
            const crd::u32 i   = s - 1 - ii;
            T              sum = rhs[static_cast<crd::usize>(i) * nrhs + c];
            for (crd::u32 k = i + 1; k < s; ++k)
            {
                sum = sum - detail::krylov_conj<T>(m[static_cast<crd::usize>(k) * s + i]) * rhs[static_cast<crd::usize>(k) * nrhs + c];
            }
            rhs[static_cast<crd::usize>(i) * nrhs + c] = sum / m[static_cast<crd::usize>(i) * s + i];
        }
    }
    return true;
}

// Regularized solve of the s×s Hermitian semidefinite system M·X = RHS for the
// rank-deficient fallback (D-BCG): a tiny SPD diagonal shift M ← M + ε·I (Tikhonov,
// ε relative to the diagonal scale) makes Cholesky succeed and damps the deflated
// (near-zero) subspace symmetrically — duplicate/converged columns get consistent
// updates. `m` is copied into `scratch` (s×s), shifted, factored; RHS overwritten.
template <typename T>
inline void block_spd_reg_solve(const T* m, crd::u32 s, T* rhs, crd::u32 nrhs, T* scratch) noexcept
{
    using R       = crd::hesap::dense::RealType<T>;
    R       dmax  = R(0);
    for (crd::u32 i = 0; i < s; ++i)
    {
        const R d = detail::krylov_real<T>(m[static_cast<crd::usize>(i) * s + i]);
        dmax      = d > dmax ? d : dmax;
    }
    const R eps = dmax * std::sqrt(std::numeric_limits<R>::epsilon()) + detail::krylov_smlnum<R>();
    for (crd::usize i = 0; i < static_cast<crd::usize>(s) * s; ++i) { scratch[i] = m[i]; }
    for (crd::u32 i = 0; i < s; ++i) { scratch[static_cast<crd::usize>(i) * s + i] = scratch[static_cast<crd::usize>(i) * s + i] + T(eps); }
    (void)block_spd_solve<T>(scratch, s, rhs, nrhs); // M+εI is SPD ⇒ Cholesky succeeds
}

// Solve the GENERAL (non-Hermitian) s×s system M·X = RHS for `nrhs` columns (RHS is
// s×nrhs row-major, overwritten with X). Gaussian elimination with partial pivoting;
// a pivot whose magnitude collapses below ε (relative to the matrix scale) is floored
// to ε so a near-singular M degrades gracefully instead of producing NaN — the
// block-BiCGSTAB analogue of the scalar smlnum breakdown guard (R̃ᴴAP is NOT SPD, so
// the SPD Cholesky path is wrong here). M is destroyed. Allocation-free, deterministic.
template <typename T>
inline void block_lu_solve(T* m, crd::u32 s, T* rhs, crd::u32 nrhs) noexcept
{
    using R    = crd::hesap::dense::RealType<T>;
    R     dmax = R(0);
    for (crd::usize i = 0; i < static_cast<crd::usize>(s) * s; ++i)
    {
        const R mg = detail::krylov_mag<T>(m[i]);
        dmax       = mg > dmax ? mg : dmax;
    }
    const R eps = dmax * std::sqrt(std::numeric_limits<R>::epsilon()) + detail::krylov_smlnum<R>();
    for (crd::u32 c = 0; c < s; ++c)
    {
        crd::u32 piv  = c; // partial pivot: largest-magnitude entry in column c at/below c
        R        best = detail::krylov_mag<T>(m[static_cast<crd::usize>(c) * s + c]);
        for (crd::u32 r = c + 1; r < s; ++r)
        {
            const R mg = detail::krylov_mag<T>(m[static_cast<crd::usize>(r) * s + c]);
            if (mg > best) { best = mg; piv = r; }
        }
        if (piv != c) // swap rows c, piv in m and rhs
        {
            for (crd::u32 j = 0; j < s; ++j)
            {
                const T t = m[static_cast<crd::usize>(piv) * s + j];
                m[static_cast<crd::usize>(piv) * s + j] = m[static_cast<crd::usize>(c) * s + j];
                m[static_cast<crd::usize>(c) * s + j]   = t;
            }
            for (crd::u32 j = 0; j < nrhs; ++j)
            {
                const T t = rhs[static_cast<crd::usize>(piv) * nrhs + j];
                rhs[static_cast<crd::usize>(piv) * nrhs + j] = rhs[static_cast<crd::usize>(c) * nrhs + j];
                rhs[static_cast<crd::usize>(c) * nrhs + j]   = t;
            }
        }
        T pivot = m[static_cast<crd::usize>(c) * s + c];
        if (detail::krylov_mag<T>(pivot) < eps) { pivot = T(eps); } // floor a collapsed pivot (graceful)
        const T inv = T(1) / pivot;
        for (crd::u32 r = c + 1; r < s; ++r)
        {
            const T f = m[static_cast<crd::usize>(r) * s + c] * inv;
            if (detail::krylov_mag<T>(f) == R(0)) { continue; }
            for (crd::u32 j = c; j < s; ++j)
            {
                m[static_cast<crd::usize>(r) * s + j] = m[static_cast<crd::usize>(r) * s + j] - f * m[static_cast<crd::usize>(c) * s + j];
            }
            for (crd::u32 j = 0; j < nrhs; ++j)
            {
                rhs[static_cast<crd::usize>(r) * nrhs + j] = rhs[static_cast<crd::usize>(r) * nrhs + j] - f * rhs[static_cast<crd::usize>(c) * nrhs + j];
            }
        }
    }
    for (crd::u32 cc = 0; cc < s; ++cc) // back-substitution (m now upper-triangular)
    {
        const crd::u32 c = s - 1 - cc;
        T pivot = m[static_cast<crd::usize>(c) * s + c];
        if (detail::krylov_mag<T>(pivot) < eps) { pivot = T(eps); }
        const T inv = T(1) / pivot;
        for (crd::u32 j = 0; j < nrhs; ++j)
        {
            T acc = rhs[static_cast<crd::usize>(c) * nrhs + j];
            for (crd::u32 k = c + 1; k < s; ++k)
            {
                acc = acc - m[static_cast<crd::usize>(c) * s + k] * rhs[static_cast<crd::usize>(k) * nrhs + j];
            }
            rhs[static_cast<crd::usize>(c) * nrhs + j] = acc * inv;
        }
    }
}

// Block reduction G = Bl1ᴴ · Bl2 (s×s), blocks n×s ROW-MAJOR (ld=s). The "tall-
// skinny" product (huge n, tiny K=s): a packed gemm pays packing overhead it can
// never amortise across K=s, so this is a ROW-STREAMING rank-1 accumulation
// instead — each row r contributes G += outer(conj(Bl1[r,:]), Bl2[r,:]). The s×s
// accumulator stays resident in L1 (≤ 4 KB for s ≤ 16); the inner j-loop is a
// contiguous auto-vectorised axpy. Allocation-free. SERIAL ⇒ deterministic
// (fixed row→i→j order; the parallel step is only the block spmm).
template <typename T>
inline void block_gram(const T* bl1, const T* bl2, crd::usize n, crd::u32 s, T* g) noexcept
{
    const crd::usize ss = static_cast<crd::usize>(s) * s;
    for (crd::usize i = 0; i < ss; ++i) { g[i] = T{}; }
    for (crd::usize r = 0; r < n; ++r)
    {
        const T* b1 = bl1 + r * s;
        const T* b2 = bl2 + r * s;
        for (crd::u32 i = 0; i < s; ++i)
        {
            const T  c1 = detail::krylov_conj<T>(b1[i]);
            T*       gi = g + static_cast<crd::usize>(i) * s;
            for (crd::u32 j = 0; j < s; ++j)
            {
                gi[j] = gi[j] + c1 * b2[j]; // contiguous axpy over the gram row
            }
        }
    }
}

// Block axpy-gemm: Out (n×s) += Bl (n×s) · M (s×s)  (sign +1) or -= (sign -1).
// Tall-skinny again (K=s): ROW-STREAMING — each row r does Out[r,:] += Σ_k
// (sign·Bl[r,k])·M[k,:], an s-fold contiguous axpy of M's rows (M resident in L1).
// Accumulates in place (beta=1). Allocation-free. Serial ⇒ deterministic.
template <typename T>
inline void block_gemm_update(const T* bl, const T* m, crd::usize n, crd::u32 s, T* out, int sign) noexcept
{
    using R          = crd::hesap::dense::RealType<T>;
    const T sgn      = T(sign >= 0 ? R(1) : R(-1));
    for (crd::usize r = 0; r < n; ++r)
    {
        const T* bl_r = bl + r * s;
        T*       o    = out + r * s;
        for (crd::u32 k = 0; k < s; ++k)
        {
            const T  a  = bl_r[k] * sgn;
            const T* mk = m + static_cast<crd::usize>(k) * s;
            for (crd::u32 j = 0; j < s; ++j)
            {
                o[j] = o[j] + a * mk[j]; // contiguous axpy of M's k-th row
            }
        }
    }
}

// Thin QR of the n×s ROW-MAJOR block `w`: W = Q·R, Q (orthonormal, overwrites w) and
// the s×s upper-triangular R (row-major, strict-lower zeroed; nullptr ⇒ discard R, the
// pure-orthonormalize path used by block-CG). This is the breakdown-free stabilization
// of block-CG (orthonormal search columns ⇒ PᴴAP / block-Hessenberg stay well-
// conditioned even for cond(A)~1e10, where plain D-BCG loses conjugacy and CholeskyQR's
// cond(W)² gram overflows) AND the block-Arnoldi QR for block-GMRES (R becomes the
// Hessenberg subdiagonal block H_{j+1,j}).
//
// Modified Gram-Schmidt with one reorthogonalisation pass (twice-is-enough). To make MGS
// cache-fast we TRANSPOSE w to column-contiguous scratch `cm` (col j at cm[j*n..)) so the
// projections/updates are the bit-exact SIMD blas1 reductions (dotc / axpy / nrm2 / scal,
// KBN-pairwise ⇒ thread-count-independent), then transpose back. R is captured AS WE GO:
// R[i,j] = Σ_pass ⟨q_i, w_j⟩ (the reorth projection ADDS to R[i,j] — it is the residual
// of pass 0's orthogonalisation, not a replacement), R[j,j] = post-orth column norm. A
// column whose norm collapses (rank deficiency: converged / duplicate RHS, or block-
// Arnoldi happy breakdown) is ZEROED with R[j,j]=0; the zero is absorbed downstream (the
// regularized s×s solve in block-CG / the band-Givens skips a zero subdiagonal in GMRES).
template <typename T>
inline void block_qr(T* w, crd::usize n, crd::u32 s, T* cm, T* r) noexcept
{
    using namespace crd::hesap::dense;
    using R          = RealType<T>;
    const R defl_tol = std::sqrt(std::numeric_limits<R>::epsilon());
    if (r != nullptr)
    {
        for (crd::usize i = 0; i < static_cast<crd::usize>(s) * s; ++i) { r[i] = T{}; }
    }
    for (crd::usize k = 0; k < n; ++k) // transpose row-major → column-contiguous
    {
        for (crd::u32 j = 0; j < s; ++j) { cm[static_cast<crd::usize>(j) * n + k] = w[k * s + j]; }
    }
    for (crd::u32 j = 0; j < s; ++j)
    {
        T* cj = cm + static_cast<crd::usize>(j) * n;
        const crd::containers::Span<T> cjs{cj, n};
        const R                        orig = nrm2<T>(cjs);
        for (int pass = 0; pass < 2; ++pass) // MGS + one reorthogonalisation
        {
            for (crd::u32 i = 0; i < j; ++i)
            {
                const T* ci = cm + static_cast<crd::usize>(i) * n;
                const T  proj = detail::krylov_inner<T>(crd::containers::ConstSpan<T>{ci, n},
                                                        crd::containers::ConstSpan<T>{cj, n}); // ⟨q_i,w_j⟩
                axpy<T>(-proj, crd::containers::ConstSpan<T>{ci, n}, cjs);                      // w_j -= ⟨q_i,w_j⟩ q_i
                if (r != nullptr)
                {
                    r[static_cast<crd::usize>(i) * s + j] = r[static_cast<crd::usize>(i) * s + j] + proj; // accumulate
                }
            }
        }
        const R nrm = nrm2<T>(cjs);
        if (nrm > defl_tol * orig && nrm > detail::krylov_smlnum<R>())
        {
            scal<T>(T(R(1) / nrm), cjs);
            if (r != nullptr) { r[static_cast<crd::usize>(j) * s + j] = T(nrm); }
        }
        else
        {
            for (crd::usize k = 0; k < n; ++k) { cj[k] = T{}; } // rank-deficient / happy-breakdown column ⇒ deflate
            // R[j,j] stays 0
        }
    }
    for (crd::usize k = 0; k < n; ++k) // transpose back column-contiguous → row-major
    {
        for (crd::u32 j = 0; j < s; ++j) { w[k * s + j] = cm[static_cast<crd::usize>(j) * n + k]; }
    }
}

// Pure orthonormalize (discard R) — the block-CG search-block stabilization.
template <typename T>
inline void block_orthonormalize(T* w, crd::usize n, crd::u32 s, T* cm) noexcept
{
    block_qr<T>(w, n, s, cm, nullptr);
}
} // namespace detail

template <typename T>
struct BlockCgWorkspace
{
    crd::usize                   n;
    crd::u32                     s;
    crd::hesap::dense::Vector<T> rblk, pblk, apblk, tblk, zblk, cmblk; // n·s each (zblk/cmblk: precond / QR-transpose scratch)
    crd::containers::Array<T>    ptap, gam, del, mcopy;               // s·s each (block ops are allocation-free row-streaming)

    BlockCgWorkspace(crd::memory::IAllocator* alloc, crd::usize size, crd::u32 nrhs)
        : n(size), s(nrhs), rblk(alloc, size * nrhs), pblk(alloc, size * nrhs), apblk(alloc, size * nrhs),
          tblk(alloc, size * nrhs), zblk(alloc, size * nrhs), cmblk(alloc, size * nrhs), ptap(alloc), gam(alloc),
          del(alloc), mcopy(alloc)
    {
        CRD_ASSERT_MSG(nrhs >= 1, "BlockCgWorkspace: nrhs must be >= 1");
        const crd::usize ss = static_cast<crd::usize>(nrhs) * nrhs;
        ptap.resize(ss);
        gam.resize(ss);
        del.resize(ss);
        mcopy.resize(ss);
    }
};

namespace detail
{
// Unified (preconditioned) block-CG. `m_inv == nullptr` ⇒ plain block-CG (Z aliases
// R). Classic O'Leary Galerkin recurrence with the preconditioner folded in exactly
// where setting M=I (Z≡R) recovers the unpreconditioned loop: P₀ = Z₀, the search-
// block update uses Z (= M⁻¹R) instead of R, while γ keeps the TRUE residual R (the
// Galerkin condition PᴴR_{k+1}=0 is on the real residual). B / X are n×s ROW-MAJOR.
template <typename T>
IterativeResult<crd::hesap::dense::RealType<T>> block_pcg_impl(
    const crd::hesap::sparse::BlockLinearOp<T>&             a,
    const crd::hesap::sparse::BlockLinearOp<T>*             m_inv,
    crd::containers::ConstSpan<T>                           b,
    crd::containers::Span<T>                                x,
    const IterativeOptions<crd::hesap::dense::RealType<T>>& opts,
    BlockCgWorkspace<T>&                                    ws,
    crd::memory::IAllocator*                                result_alloc)
{
    using namespace crd::hesap::dense;
    using R = RealType<T>;

    IterativeResult<R> result(result_alloc);
    const crd::usize   n = a.n_rows();
    const crd::u32     s = ws.s;
    CRD_ASSERT_MSG(a.n_rows() == a.n_cols(), "block_cg: operator must be square");
    CRD_ASSERT_MSG(b.size() == n * s && x.size() == n * s, "block_cg: B/X must be n×s row-major");

    T* R_ = ws.rblk.data();
    T* P  = ws.pblk.data();
    T* AP = ws.apblk.data();
    T* Tw = ws.tblk.data();
    // Z = M⁻¹R (preconditioned residual); aliases R when unpreconditioned.
    T* Z = (m_inv != nullptr) ? ws.zblk.data() : R_;

    // R = B - A·X
    (void)a.apply_block(x, s, crd::containers::Span<T>{AP, n * s}, s, s); // AP = A·X (scratch)
    for (crd::usize i = 0; i < n * s; ++i)
    {
        R_[i] = b[i] - AP[i];
    }
    // Per-column ‖B·,j‖ for the relative-residual test.
    crd::containers::Array<R> bnorm(result_alloc);
    bnorm.resize(s);
    for (crd::u32 j = 0; j < s; ++j)
    {
        R acc = R(0);
        for (crd::usize k = 0; k < n; ++k)
        {
            acc += detail::krylov_mag<T>(b[k * s + j]) * detail::krylov_mag<T>(b[k * s + j]);
        }
        bnorm[j] = std::sqrt(acc) + detail::krylov_smlnum<R>();
    }
    auto all_converged = [&]() -> bool {
        for (crd::u32 j = 0; j < s; ++j)
        {
            R acc = R(0);
            for (crd::usize k = 0; k < n; ++k)
            {
                acc += detail::krylov_mag<T>(R_[k * s + j]) * detail::krylov_mag<T>(R_[k * s + j]);
            }
            if (std::sqrt(acc) > opts.rel_tol * bnorm[j])
            {
                return false;
            }
        }
        return true;
    };

    if (all_converged())
    {
        result.converged = true;
        result.reason    = StopReason::Converged;
        return result;
    }

    // Classic block-CG (D-BCG), Galerkin form, no per-step QR. Rank deficiency
    // (converged / duplicate columns ⇒ PᴴAP singular) is handled on the s×s
    // coefficient matrices: a fast Cholesky, falling back to a regularized Cholesky
    // (M+εI Tikhonov) that damps the deflated near-zero subspace symmetrically.
    const crd::usize ss = static_cast<crd::usize>(s) * s;
    if (m_inv != nullptr)
    {
        (void)m_inv->apply_block(crd::containers::ConstSpan<T>{R_, n * s}, s, crd::containers::Span<T>{Z, n * s}, s, s);
    }
    for (crd::usize i = 0; i < n * s; ++i) { P[i] = Z[i]; } // P₀ = Z₀ ...
    detail::block_orthonormalize<T>(P, n, s, ws.cmblk.data()); // ... orthonormalized (breakdown-free)

    auto solve_block = [&](const T* m, T* rhs) {
        for (crd::usize i = 0; i < ss; ++i) { ws.mcopy[i] = m[i]; }
        if (!detail::block_spd_solve<T>(ws.mcopy.data(), s, rhs, s))
        {
            detail::block_spd_reg_solve<T>(m, s, rhs, s, ws.mcopy.data()); // regularized deflated fallback
        }
    };

    for (crd::usize it = 1; it <= opts.max_iter; ++it)
    {
        (void)a.apply_block(crd::containers::ConstSpan<T>{P, n * s}, s, crd::containers::Span<T>{AP, n * s}, s, s); // AP=A·P
        detail::block_gram<T>(P, AP, n, s, ws.ptap.data()); // PᴴAP (the M for both solves)

        // γ = (PᴴAP)⁺·(PᴴR) ; X += P·γ ; R -= AP·γ.  (Galerkin form; γ uses the TRUE residual R.)
        detail::block_gram<T>(P, R_, n, s, ws.gam.data()); // gam = PᴴR
        solve_block(ws.ptap.data(), ws.gam.data());        // gam = γ
        detail::block_gemm_update<T>(P, ws.gam.data(), n, s, x.data(), +1);
        detail::block_gemm_update<T>(AP, ws.gam.data(), n, s, R_, -1);

        result.iterations = it;
        if (all_converged())
        {
            result.converged           = true;
            result.reason              = StopReason::Converged;
            result.final_residual_norm = R(0);
            return result;
        }

        // Z = M⁻¹R_new ; δ = -(PᴴAP)⁺·(Pᴴ A Z) ; Pᴴ A Z = (AP)ᴴ Z ; P = Z + P·δ.
        if (m_inv != nullptr)
        {
            (void)m_inv->apply_block(crd::containers::ConstSpan<T>{R_, n * s}, s, crd::containers::Span<T>{Z, n * s}, s, s);
        }
        detail::block_gram<T>(AP, Z, n, s, ws.del.data()); // del = (AP)ᴴZ
        solve_block(ws.ptap.data(), ws.del.data());        // del = (PᴴAP)⁺(PᴴAZ)
        for (crd::usize i = 0; i < n * s; ++i) { Tw[i] = Z[i]; }
        detail::block_gemm_update<T>(P, ws.del.data(), n, s, Tw, -1); // Tw = Z - P·del = Z + P·δ (A-conjugate to P)
        detail::block_orthonormalize<T>(Tw, n, s, ws.cmblk.data()); // re-basis ⇒ PᴴAP well-conditioned
        for (crd::usize i = 0; i < n * s; ++i) { P[i] = Tw[i]; }
    }

    result.reason = StopReason::MaxIterations;
    return result;
}
} // namespace detail

// Block-CG: solve A·X = B for s columns. B / X are n×s ROW-MAJOR (stride s). A
// SPD/HPD. Converged when every column's relative residual ‖R·,j‖/‖B·,j‖ ≤ rel_tol.
template <typename T>
IterativeResult<crd::hesap::dense::RealType<T>> block_cg(const crd::hesap::sparse::BlockLinearOp<T>& a,
                                                         crd::containers::ConstSpan<T>               b,
                                                         crd::containers::Span<T>                    x,
                                                         const IterativeOptions<crd::hesap::dense::RealType<T>>& opts,
                                                         BlockCgWorkspace<T>&                        ws,
                                                         crd::memory::IAllocator*                    result_alloc)
{
    return detail::block_pcg_impl<T>(a, nullptr, b, x, opts, ws, result_alloc);
}

// Block-PCG: preconditioned block-CG. `m_inv` applies Z = M⁻¹R as a block op
// (n×s in/out). M must be SPD/HPD for the Galerkin recurrence to stay symmetric.
template <typename T>
IterativeResult<crd::hesap::dense::RealType<T>> block_pcg(const crd::hesap::sparse::BlockLinearOp<T>& a,
                                                          const crd::hesap::sparse::BlockLinearOp<T>& m_inv,
                                                          crd::containers::ConstSpan<T>               b,
                                                          crd::containers::Span<T>                    x,
                                                          const IterativeOptions<crd::hesap::dense::RealType<T>>& opts,
                                                          BlockCgWorkspace<T>&                        ws,
                                                          crd::memory::IAllocator*                    result_alloc)
{
    return detail::block_pcg_impl<T>(a, &m_inv, b, x, opts, ws, result_alloc);
}

} // namespace crd::hesap::iterative
