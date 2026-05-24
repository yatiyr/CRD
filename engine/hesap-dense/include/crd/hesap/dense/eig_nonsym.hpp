#pragma once

#include <crd/containers/array.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/complex.hpp>
#include <crd/hesap/dense/matrix.hpp>
#include <crd/hesap/dense/real_type.hpp>
#include <crd/hesap/dense/vector.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::hesap::dense
{
// -----------------------------------------------------------------------
// Phase 3.1.6 v3d — non-symmetric eigensolver substrate.
//
// Pipeline (v3d-1 real Schur, v3d-2 eigenvectors):
//   balance (dgebal)  →  Hessenberg (dgehrd)  →  Francis double-shift QR
//   with Aggressive Early Deflation (dhseqr/dlaqr0) → real Schur  →
//   eigenvectors (dtrevc) + 3-stage back-transform.
//
// v3d-1a ships the REDUCTION: Hessenberg reduction Qᵀ·A·Q = H (upper
// Hessenberg) + explicit Q formation, reusing the shared `make_householder`
// substrate. Real f32/f64 (complex Schur lands in v3d-2). The Francis QR
// (v3d-1b) and AED (v3d-1c, the hard-gate) consume H. Lower layer: raw
// scalars (ADR-0078 §5).
// -----------------------------------------------------------------------

// =======================================================================
// balance — LAPACK dgebal (JOB='B'): isolate eigenvalues by a similarity
// PERMUTATION (push rows/cols that already isolate an eigenvalue to the
// corners) + reduce the norm of the active block by a radix-2 diagonal
// SCALING similarity. In place. On exit:
//   - `ilo`/`ihi` (0-based, inclusive) bound the block still needing the QR
//     iteration; indices outside carry isolated eigenvalues on the diagonal,
//   - `scale[i]` (REAL — `RealType<T>`) holds the permutation/scale info (LAPACK
//     convention: for i<ilo or i>ihi the permutation index, for ilo<=i<=ihi the
//     scale factor).
// Reduces rounding error in the downstream Schur step. Real f32/f64 AND complex
// c32/c64 (v3d-2c-2, zgebal): for complex `T` the same permutation + radix-2
// scaling on |·| magnitudes; the scale array is real either way. `if constexpr`.
// =======================================================================
template <typename T>
void balance(Matrix<T>& a, crd::containers::Array<RealType<T>>& scale, crd::usize& ilo,
             crd::usize& ihi);

// =======================================================================
// hessenberg — reduce the active block A[ilo..ihi, ilo..ihi] (n×n A, RowMajor)
// to UPPER HESSENBERG form in place by Householder reflectors (LAPACK dgehd2,
// unblocked). On exit:
//   - the upper Hessenberg H occupies the upper triangle + first subdiagonal,
//   - reflector i (annihilating A[i+2..ihi, i]) is stored in A[i+2..ihi][i],
//     with implicit v[i+1] = 1; A[i+1][i] holds the subdiagonal of H,
//   - tau[i] (i in [ilo, ihi-1)) holds the reflector scalars; tau has length n
//     (entries outside [ilo, ihi-1) are 0).
// Rows/cols outside [ilo, ihi] are left untouched (they carry isolated
// eigenvalues after balancing). For an un-balanced matrix pass ilo=0, ihi=n-1.
//
// Real f32/f64 AND complex c32/c64 (v3d-2c-1, zgehd2): for complex `T`, the
// reduction is a UNITARY similarity Qᴴ·A·Q = H carried on the two-real-array
// SIMD path internally (`make_householder_complex`); the subdiagonal of H is
// real, `tau` is complex. Dispatch is compile-time (`if constexpr`).
// =======================================================================
template <typename T>
void hessenberg(Matrix<T>& a, crd::usize ilo, crd::usize ihi, crd::containers::Array<T>& tau);

// =======================================================================
// form_hessenberg_q — materialize the orthogonal Q (n×n) of the Hessenberg
// reduction (LAPACK dorghr), from the reflectors stored in `a_packed` (the
// post-`hessenberg` matrix) + `tau`. Q = H_ilo · H_{ilo+1} · … · H_{ihi-2},
// identity outside the [ilo, ihi] block. So A = Q · H · Qᵀ. Complex c32/c64
// (v3d-2c-1, zunghr): Q is UNITARY and A = Q · H · Qᴴ.
// =======================================================================
template <typename T>
[[nodiscard]] Matrix<T> form_hessenberg_q(crd::memory::IAllocator* alloc, const Matrix<T>& a_packed,
                                          crd::usize ilo, crd::usize ihi,
                                          const crd::containers::Array<T>& tau);

// -----------------------------------------------------------------------
// Phase 3.1.6 v3d-1b — real Schur form via Francis double-shift QR.
// -----------------------------------------------------------------------

template <typename T>
struct RealSchur
{
    Matrix<T> t;                    // quasi-triangular real Schur form (1×1 + 2×2 blocks)
    Matrix<T> z;                    // orthogonal Schur vectors: input H = Z·T·Zᵀ (empty if !vectors)
    crd::containers::Array<T> wr;   // eigenvalue real parts (length n)
    crd::containers::Array<T> wi;   // eigenvalue imaginary parts (±pair for 2×2 blocks)
    bool converged = false;

    explicit RealSchur(crd::memory::IAllocator* alloc) noexcept : t(alloc), z(alloc), wr(alloc), wi(alloc)
    {
    }
    RealSchur(RealSchur&&) noexcept = default;
    RealSchur& operator=(RealSchur&&) noexcept = default;
    RealSchur(const RealSchur&) = delete;
    RealSchur& operator=(const RealSchur&) = delete;
};

// =======================================================================
// real_schur — Francis implicit double-shift QR (LAPACK dlahqr) on an UPPER
// HESSENBERG matrix `h_in` (n×n), reducing the active block [ilo, ihi] to real
// Schur form T (quasi-upper-triangular: real eigenvalues on the diagonal,
// complex-conjugate pairs as standardized 2×2 blocks via `dlanv2`). With
// `vectors`, accumulates the orthogonal Z so that `h_in = Z·T·Zᵀ`. Eigenvalues
// are returned in `wr`/`wi`. Deterministic (fixed Wilkinson + exceptional-shift
// schedule, capped iters; D(non-sym)-1). Real f32/f64 (complex Schur is v3d-2).
// =======================================================================
template <typename T>
[[nodiscard]] RealSchur<T> real_schur(crd::memory::IAllocator* alloc, const Matrix<T>& h_in,
                                      crd::usize ilo, crd::usize ihi, bool vectors);

// =======================================================================
// v3d-1c-1 — reorder_schur (LAPACK dtrexc): move the diagonal block starting at
// position `ifst` of a real Schur form `t` (quasi-upper-triangular, n×n) to
// position `ilst`, by a sequence of adjacent 1×1/2×2 block swaps (`dlaexc`,
// each solving a small Sylvester system `dlasy2`). The orthogonal `z` (Schur
// vectors) is updated in step so the decomposition `A = z·t·zᵀ` is preserved
// with eigenvalues permuted. Positions are 0-based; if `ifst`/`ilst` land inside
// a 2×2 block they snap to its top. Returns false if a swap was rejected
// (too ill-conditioned). The AED-deflation prerequisite (v3d-1c-2). Real f32/f64.
// =======================================================================
template <typename T>
bool reorder_schur(Matrix<T>& t, Matrix<T>& z, crd::usize ifst, crd::usize ilst);

// =======================================================================
// v3d-1c-2 — aed_deflate (LAPACK dlaqr2): one Aggressive Early Deflation pass
// on the active upper-Hessenberg block [ktop, kbot] of `h` (n×n, 0-based
// inclusive). Takes a trailing deflation window of size `nw`, computes its
// real Schur form (via `real_schur`), and tests each eigenvalue's spike tip
// `|S·V(1,j)|` (S = the subdiagonal coupling above the window): negligible ⇒
// deflate, else `reorder_schur` moves it up out of the way. Converged
// eigenvalues are split off; the spike is reflected + the leading block
// re-Hessenbergized, and `h` (+ `z` over [iloz,ihiz] if `wantz`; full T slabs
// if `wantt`) is updated globally so `h` stays similar. `wr`/`wi` receive the
// window eigenvalues. Returns {ns = #undeflatable (shifts), nd = #deflated}.
// Real f32/f64. The AED step the multishift driver (v3d-1c-3) consumes.
// =======================================================================
template <typename T>
struct AedResult
{
    crd::usize ns = 0;  // number of undeflatable eigenvalues (available as shifts)
    crd::usize nd = 0;  // number of converged eigenvalues deflated this pass
};

template <typename T>
[[nodiscard]] AedResult<T> aed_deflate(crd::memory::IAllocator* alloc, Matrix<T>& h, crd::usize ktop,
                                       crd::usize kbot, crd::usize nw, Matrix<T>& z, bool wantz,
                                       crd::usize iloz, crd::usize ihiz, bool wantt,
                                       crd::containers::Array<T>& wr, crd::containers::Array<T>& wi);

// =======================================================================
// v3d-1c-3 — schur_aed (LAPACK dlaqr0-class): real Schur form of an upper-
// Hessenberg `h_in` over [ilo, ihi] driven by **Aggressive Early Deflation**.
// The driver loops: deflate converged trailing eigenvalues via `aed_deflate`,
// then run double-shift QR sweeps using the undeflated AED eigenvalues as
// shifts; blocks below a crossover (NMIN≈75) are finished by `real_schur`
// (dlahqr). AED converges a whole window per Schur instead of one eigenvalue
// per O(n) sweeps, so the total QR-sweep count drops vs pure double-shift —
// the lever that beats Eigen RealSchur + LAPACK at scale. `sweeps` (out)
// reports the QR-sweep count for the hard-gate measurement. Real f32/f64.
// =======================================================================
template <typename T>
[[nodiscard]] RealSchur<T> schur_aed(crd::memory::IAllocator* alloc, const Matrix<T>& h_in,
                                     crd::usize ilo, crd::usize ihi, bool vectors,
                                     crd::usize* sweeps = nullptr);

// -----------------------------------------------------------------------
// Phase 3.1.6 v3d-2c-2 — complex Schur form via single-shift QR (zlahqr).
//
// A complex (Hermitian-NOT-assumed) upper-Hessenberg `h_in` (n×n) over the
// active block [ilo, ihi] is reduced to UPPER-TRIANGULAR Schur form T by a
// unitary similarity: `h_in = Z·T·Zᴴ`. Complex eigenvalues sit directly on the
// diagonal of T (no 2×2 blocks, no `dlanv2` — that is the structural
// simplification over the real `real_schur`). Single Wilkinson-shift implicit
// QR with complex Givens bulge-chase + Ahues-Tisseur deflation + an exceptional
// shift schedule (D(non-sym)-6). With `vectors`, accumulates Z. c32/c64.
// -----------------------------------------------------------------------
template <typename T>  // T = Complex<f32|f64>
struct ComplexSchur
{
    Matrix<T> t;                  // upper-triangular Schur form
    Matrix<T> z;                  // unitary Schur vectors: h_in = Z·T·Zᴴ (empty if !vectors)
    crd::containers::Array<T> w;  // eigenvalues (the diagonal of T), length n
    bool converged = false;

    explicit ComplexSchur(crd::memory::IAllocator* alloc) noexcept : t(alloc), z(alloc), w(alloc) {}
    ComplexSchur(ComplexSchur&&) noexcept = default;
    ComplexSchur& operator=(ComplexSchur&&) noexcept = default;
    ComplexSchur(const ComplexSchur&) = delete;
    ComplexSchur& operator=(const ComplexSchur&) = delete;
};

template <typename T>
[[nodiscard]] ComplexSchur<T> complex_schur(crd::memory::IAllocator* alloc, const Matrix<T>& h_in,
                                            crd::usize ilo, crd::usize ihi, bool vectors);

// =======================================================================
// v3d-2c-2b-1 — reorder_complex_schur (LAPACK ztrexc): move the diagonal
// eigenvalue at position `ifst` of an UPPER-TRIANGULAR complex Schur form `t`
// (n×n) to position `ilst`, by a sequence of adjacent 1×1 swaps. Each swap of
// adjacent diagonal entries t(k,k), t(k+1,k+1) is a single complex Givens
// (`zlartg(t(k,k+1), t(k+1,k+1)−t(k,k))`) applied as a unitary similarity
// G·T·Gᴴ to rows/cols (k,k+1) of T + columns (k,k+1) of the unitary `z`, so the
// decomposition `A = z·t·zᴴ` is preserved with eigenvalues permuted. Positions
// are 0-based. The complex analog of `reorder_schur` — with NO 2×2 blocks
// (complex eigenvalues sit directly on the diagonal) there is no `dlaexc`/
// `dlasy2`, so every swap is an exact 1×1 rotation with no rejection path; the
// `bool` return is `true` (API symmetry with the real reorder for the AED
// deflation caller, v3d-2c-2b-2). c32/c64.
// =======================================================================
template <typename T>  // T = Complex<f32|f64>
bool reorder_complex_schur(Matrix<T>& t, Matrix<T>& z, crd::usize ifst, crd::usize ilst);

// =======================================================================
// v3d-2c-2b-2 — complex_aed_deflate (LAPACK zlaqr2/zlaqr3): one Aggressive
// Early Deflation pass on the active upper-Hessenberg block [ktop, kbot] of `h`
// (n×n complex, 0-based inclusive). Takes a trailing deflation window of size
// `nw`, computes its complex Schur form (via `complex_schur`), and tests each
// eigenvalue's spike tip `|s|·|V(1,j)|` (s = the subdiagonal coupling above the
// window): negligible ⇒ deflate, else `reorder_complex_schur` moves it up. The
// spike is reflected (`make_householder_complex`) + the leading block
// re-Hessenbergized (`hessenberg<Complex>`), and `h` (+ `z` over [iloz,ihiz] if
// `wantz`; full T slabs if `wantt`) is updated globally via complex `gemm` slabs
// so `h` stays unitarily similar. `w` receives the window eigenvalues (the
// diagonal of the window Schur form). Returns {ns = #undeflatable shifts, nd =
// #deflated}. The complex analog of `aed_deflate` — with NO 2×2 blocks the
// deflation test, the eigenvalue restore (diagonal of T) and the reorder are all
// 1×1 (no `dlanv2`, no bulge branch). The complex multishift driver (v3d-2c-2b-3)
// consumes this. c32/c64.
// =======================================================================
template <typename T>  // T = Complex<f32|f64>
[[nodiscard]] AedResult<T> complex_aed_deflate(crd::memory::IAllocator* alloc, Matrix<T>& h,
                                               crd::usize ktop, crd::usize kbot, crd::usize nw,
                                               Matrix<T>& z, bool wantz, crd::usize iloz,
                                               crd::usize ihiz, bool wantt,
                                               crd::containers::Array<T>& w);

// =======================================================================
// v3d-2c-2b-3 — complex_schur_aed (LAPACK zlaqr0-class): upper-triangular
// complex Schur form of an upper-Hessenberg `h_in` over [ilo, ihi] driven by
// **Aggressive Early Deflation**. The driver loops: deflate converged trailing
// eigenvalues via `complex_aed_deflate`, then run a small-bulge multishift QR
// sweep (`zlaqr5`-class, complex: accumulate per-window reflectors into U then
// BLAS-3 `gemm` slab far-updates) using the undeflated AED eigenvalues as
// shifts; blocks below a crossover (NMIN) are finished by `complex_schur`
// (zlahqr). AED converges a whole window per inner Schur instead of one
// eigenvalue per O(n) sweep, so the total QR-sweep count drops vs single-shift
// `complex_schur` — the lever that crushes it at scale (the complex external
// refs Eigen `ComplexSchur` + LAPACK `zhseqr` access-violate at n≥256, so the
// scale gate is against our own single-shift baseline). `sweeps` (out) reports
// the multishift-sweep count. c32/c64. h_in = Z·T·Zᴴ.
// =======================================================================
template <typename T>  // T = Complex<f32|f64>
[[nodiscard]] ComplexSchur<T> complex_schur_aed(crd::memory::IAllocator* alloc, const Matrix<T>& h_in,
                                                crd::usize ilo, crd::usize ihi, bool vectors,
                                                crd::usize* sweeps = nullptr);

// -----------------------------------------------------------------------
// Phase 3.1.6 v3d-2b — public non-symmetric eigensolver (real input).
//
//   A·vₖ = λₖ·vₖ ,  A real n×n general.
//
// Pipeline: balance (dgebal) → Hessenberg (dgehd2) + form Q (dorghr) →
// real Schur via AED multishift (schur_aed: A_bal = (Q·Z)·T·(Q·Z)ᵀ) →
// right eigenvectors of T (dtrevc) → 3-stage back-transform
//   V = D⁻¹·P · Q · Z · V_schur
// undoing Schur (Z·), Hessenberg (Q·) and balance (dgebak: row scale over
// [ilo,ihi] + isolating permutation at the corners). All three stages are
// real-linear, so the dtrevc real-packed re/im columns survive them and the
// complex eigenvectors are assembled only at the end.
//
// Real A has real eigenvalues (real eigenvectors) and complex-conjugate
// eigenvalue PAIRS (conjugate eigenvector pairs) — `values`/`vectors` are
// therefore complex even for a real `T`. Eigenpairs are returned in Schur
// order (the diagonal order of T from `schur_aed` — deterministic;
// D(non-sym)-5), NOT sorted (complex spectra have no natural total order).
//
// Eigenvector normalization (D(non-sym)-4, matches LAPACK dgeev + Eigen
// EigenSolver): each column scaled to Euclidean norm 1, then phase-rotated so
// the lowest-index largest-magnitude component is real and positive (the
// conjugate column carries the conjugated phase). NOTE: this supersedes the
// v3d-2 plan-row wording "max-abs = 1" — 2-norm=1 is the LAPACK convention and
// keeps the head-to-head gate vs Eigen/LAPACK clean.
// -----------------------------------------------------------------------
template <typename T>
struct EigNonsym
{
    Vector<Complex<RealType<T>>> values;   // eigenvalues (Schur order)
    Matrix<Complex<RealType<T>>> vectors;  // column k = eigenvector for values[k]

    explicit EigNonsym(crd::memory::IAllocator* alloc) noexcept : values(alloc), vectors(alloc) {}
    EigNonsym(crd::memory::IAllocator* alloc, crd::usize n) : values(alloc, n), vectors(alloc, n, n) {}

    EigNonsym(EigNonsym&&) noexcept = default;
    EigNonsym& operator=(EigNonsym&&) noexcept = default;
    EigNonsym(const EigNonsym&) = delete;
    EigNonsym& operator=(const EigNonsym&) = delete;
};

// =======================================================================
// eig — full non-symmetric eigendecomposition of a general matrix `a` (not
// modified; cloned into a working buffer internally). Scratch + outputs use
// `alloc`. Real f32/f64 (v3d-2b) AND complex c32/c64 (v3d-2c-3, zgeev path):
// dispatched compile-time via `if constexpr`. Complex input → upper-triangular
// complex Schur (`complex_schur_aed`) + `ztrevc` + unitary back-transform;
// every eigenpair is a single complex column (no real-packed/conjugate-pair
// layout). Both paths: eigenpairs in Schur order (D(non-sym)-5), each
// eigenvector ‖·‖₂=1 with the largest-magnitude component real-positive
// (D(non-sym)-4).
// =======================================================================
template <typename T>
[[nodiscard]] EigNonsym<T> eig(crd::memory::IAllocator* alloc, const Matrix<T>& a);

namespace detail
{
// =======================================================================
// v3d-1c-4 (multishift train) — exposed for the M1/M2 equivalence gate.
//
// `multishift_sweep` runs one small-bulge multi-shift QR sweep (LAPACK
// `dlaqr5`, KACC22=1 = accumulate-into-U then BLAS-3 gemm slab updates) on the
// active Hessenberg block [ktop, kbot] (0-based inclusive) of `hd` (n×n,
// row-major), with `nshifts` shifts (even; conjugate pairs in `sr`/`si`). With
// `nshifts == 2` it is one Francis double-shift sweep — verified by the
// implicit-Q characterization (similarity + orthogonality + Hessenberg +
// first-column ∝ shift polynomial). Real f32/f64.
// =======================================================================
template <typename T>
void multishift_sweep(crd::memory::IAllocator* alloc, crd::usize n, crd::usize ktop, crd::usize kbot,
                      const T* sr, const T* si, crd::usize nshifts, T* hd, crd::usize ld,
                      crd::usize iloz, crd::usize ihiz, T* zd, crd::usize zld, bool wantz);

// =======================================================================
// v3d-2c-2b-3 — exposed for the complex implicit-Q isolation gate. One complex
// small-bulge multishift QR sweep (`zlaqr5`) on the active block [ktop, kbot] of
// `hd` (n×n complex, row-major) with `nshifts` complex shifts (paired into
// nshifts/2 bulges). One sweep is a unitary similarity, so on Z=I the result
// satisfies Z·H'·Zᴴ == H_orig regardless of shifts. T = Complex<f32|f64>.
// =======================================================================
template <typename T>
void complex_multishift_sweep(crd::memory::IAllocator* alloc, crd::usize n, crd::usize ktop,
                              crd::usize kbot, const T* shifts, crd::usize nshifts, T* hd,
                              crd::usize ld, crd::usize iloz, crd::usize ihiz, T* zd, crd::usize zld,
                              bool wantz);

// =======================================================================
// v3d-2a — exposed for the dlaln2 isolation gate. Solve (ca·A − w·D)·X = scale·B
// (transposed if `ltrans`) for the na×na (na∈{1,2}) X with overflow scaling +
// the `smin` singular-value floor. `a2`/`b2`/`x2` are row-major 2×2 buffers
// (only na×na / na×nw used; for nw=2 the imag part is column 1). Writes
// `scale`/`xnorm`/`info` (info=1 ⇒ the coefficient was perturbed). Real f32/f64.
// =======================================================================
template <typename T>
void lin_solve_2x2(bool ltrans, int na, int nw, T smin, T ca, const T* a2, T d1, T d2, const T* b2,
                   T wr, T wi, T* x2, T& scale, T& xnorm, int& info);

// =======================================================================
// v3d-2a — right eigenvectors of a real quasi-upper-triangular Schur form `t`
// (n×n), real-packed (LAPACK `dtrevc`, SIDE='R', HOWMNY='A', NOT back-
// transformed): real eigenvalue at diagonal k → real eigenvector in column k;
// complex 2×2 block at (k-1,k) → the +imag eigenvalue's vector = col(k-1) +
// i·col(k) (conjugate for the −imag). Each column normalized ‖·‖∞ = 1. The
// v3d-2b back-transform + public complex `eig` consume this. Real f32/f64.
// =======================================================================
template <typename T>
[[nodiscard]] Matrix<T> schur_right_eigvecs(crd::memory::IAllocator* alloc, const Matrix<T>& t);
} // namespace detail

} // namespace crd::hesap::dense
