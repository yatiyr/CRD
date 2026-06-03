#pragma once

#include <crd/containers/array.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/complex.hpp> // crd::hesap::conj / abs / real (the complex variants, v5d-f)
#include <crd/hesap/dense/blas3.hpp> // dense::gemm + MatrixView (the v5d-perf blocked-BLAS-3 front factor)
#include <crd/hesap/dense/real_type.hpp>
#include <crd/math/simd/vec4d.hpp>
#include <crd/math/simd/vec8f.hpp>
#include <crd/memory/allocator.hpp>

#include <type_traits>

namespace crd::hesap::direct
{
// =======================================================================
// v5d-b — the per-front INDEFINITE Bunch-Kaufman kernel (the genuinely-new
// algorithm of the multifrontal LDLᵀ; everything else in v5d is reuse of the
// v5b-3 symmetric symbolic + MfFront + extend_add).
//
// `factor_front_ldlt` is the COL-MAJOR, `npiv`-restricted analog of
// dense::LDLT / LAPACK xSYTRF (UPLO=Lower). It factors the leading `npiv`
// FULLY-SUMMED pivots of an m×m symmetric front with Bunch-Kaufman 1×1/2×2
// partial pivoting and leaves the symmetric Schur complement
// S = A22 - L21·D·L21ᵀ in the trailing (m-npiv)×(m-npiv) lower triangle (the
// contribution block the driver extend-adds to the parent — v5d-c).
//
// Storage: lower triangle, full m×m allocation, COL-MAJOR. Element (row i,
// col j) with i ≥ j = d[j*ld + i]. The trailing update is a SYMMETRIC rank-1
// (1×1 pivot) / rank-2 (2×2 pivot) update touching ONLY the lower triangle —
// the ~½-flop win over a both-triangles form (the MA57-class floor). Col-major
// makes both the source column k and each destination column j contiguous, so
// no scratch packing is needed (the dense row-major form packed buffers for
// SIMD contiguity; here the update reads the still-original column k while
// writing only columns j > k, which are disjoint, then normalizes column k
// last) — hence NO scratch allocator and NO hidden malloc.
//
// Pivot contract (MA57 multifrontal, symmetric indefinite):
//   - pivot *diagonal* choices are restricted to the fully-summed block
//     [k, npiv); a 2×2 partner must also be < npiv.
//   - the colmax / stability search runs over the FULL column [k+1, m) so the
//     L21 growth into the contribution-block rows is accounted for.
//   - if the only stable pivot for a fully-summed variable would land on a
//     non-fully-summed CB row (imax ≥ npiv), or its column is structurally
//     null, the variable is DELAYED: the kernel STOPS and returns the count of
//     pivots eliminated so far (< npiv). It NEVER pivots onto a CB row (that
//     would eliminate a non-fully-summed variable = silent corruption). The
//     driver (v5d-c) / Duff-Reid follow-on relays a delayed pivot to the
//     parent front. (The v5d-b test fronts are diagonally strong, so the
//     restriction never fires; one targeted test locks the delay contract.)
//
// On exit (for the eliminated leading r = return value pivots):
//   d[k,k]                         D's 1×1 block      (block_kinds[k]==1).
//   d[k,k],d[k+1,k],d[k+1,k+1]     D's 2×2 block      (block_kinds[k]==2,
//                                  block_kinds[k+1]==0); L[k+1,k] is implicitly
//                                  0 — that slot stores D[k+1,k].
//   d[i,k] for i>k (not in a 2×2)  L21 multipliers (unit-lower diag implicit).
//   trailing lower triangle        the Schur complement (contribution block).
//   block_kinds[0:r], piv[0:r]     pivot tiling + local swap targets (all < npiv).
//
// MOAT: the BK pivot scan is a deterministic pure function of the front buffer
// — strict-`>` first-max colmax/rowmax + the fixed [0,npiv) restriction ⇒
// identical pivot choices regardless of worker count (fronts are assembled in
// fixed postorder ⇒ identical buffers) ⇒ L,D bit-identical across {1,2,4,8,16}
// workers (proven in v5d-e). The strict-`>` first-max tie-break IS the moat
// invariant — do not relax it.
//
// f32 + f64 (real); Complex32/64 in BOTH modes — `factor_front_ldlt<T, Hermitian>`:
//   Hermitian == false → LDLᵀ (complex-SYMMETRIC, A = Aᵀ, unconjugated, D complex);
//   Hermitian == true  → LDLᴴ (HERMITIAN-indefinite, A = Aᴴ, conjugated, D real-diagonal).
// The two complex algorithms differ ONLY at: the trailing-update second factor
// (conjugate for Hermitian), the D 1×1 (real for Hermitian) + 2×2 inverse, and
// the diagonal pivot magnitude (|Re| for Hermitian). For real T every conjugation
// is a no-op and Hermitian is irrelevant ⇒ <real,false> is byte-identical to the
// v5d-b/c/d/e real path (the moat + reconstruction tests pin this).
// =======================================================================

// |x| magnitude for the Bunch-Kaufman tests → RealType (real: |x|; complex: modulus).
template <typename T> [[nodiscard]] inline dense::RealType<T> ldlt_mag(const T& x) noexcept
{
    if constexpr (dense::is_complex_v<T>)
    {
        return crd::hesap::abs(x);
    }
    else
    {
        return x < T{0} ? -x : x;
    }
}

// Conjugate iff Hermitian-and-complex (LDLᴴ); identity for LDLᵀ and for every real T.
template <bool Hermitian, typename T> [[nodiscard]] inline T ldlt_conjh(const T& x) noexcept
{
    if constexpr (Hermitian && dense::is_complex_v<T>)
    {
        return crd::hesap::conj(x);
    }
    else
    {
        return x;
    }
}

// The diagonal PIVOT value: Hermitian-complex ⇒ force real (discard the numerically-zero imaginary noise of
// a Hermitian diagonal, so the solve's "divide by real d" is exact); LDLᵀ + real ⇒ the value as-is.
template <bool Hermitian, typename T> [[nodiscard]] inline T ldlt_pivd(const T& x) noexcept
{
    if constexpr (Hermitian && dense::is_complex_v<T>)
    {
        return T{crd::hesap::real(x), dense::RealType<T>{0}};
    }
    else
    {
        return x;
    }
}

// The diagonal CANDIDATE magnitude for pivoting: Hermitian-complex ⇒ |Re(x)|; else ldlt_mag.
template <bool Hermitian, typename T> [[nodiscard]] inline dense::RealType<T> ldlt_magd(const T& x) noexcept
{
    if constexpr (Hermitian && dense::is_complex_v<T>)
    {
        const dense::RealType<T> r = crd::hesap::real(x);
        return r < dense::RealType<T>{0} ? -r : r;
    }
    else
    {
        return ldlt_mag<T>(x);
    }
}

// SIMD axpy-negate: dst[p] -= s * src[p] for p in [0, len). f32 / f64 fast paths
// via Vec8f / Vec4d + single-rounded FMA; scalar tail. (Mirror of dense ldlt.cpp.)
template <typename T> inline void ldlt_axpy_negate(T* dst, const T* src, T s, crd::usize len) noexcept
{
    crd::usize p = 0;
    if constexpr (std::is_same_v<T, crd::f64>)
    {
        namespace simd = crd::math::simd;
        const simd::Vec4d neg_s(-s);
        for (; p + 16 <= len; p += 16)
        {
            simd::Vec4d r0 = simd::Vec4d::load(dst + p);
            simd::Vec4d r1 = simd::Vec4d::load(dst + p + 4);
            simd::Vec4d r2 = simd::Vec4d::load(dst + p + 8);
            simd::Vec4d r3 = simd::Vec4d::load(dst + p + 12);
            r0 = simd::fma(neg_s, simd::Vec4d::load(src + p), r0);
            r1 = simd::fma(neg_s, simd::Vec4d::load(src + p + 4), r1);
            r2 = simd::fma(neg_s, simd::Vec4d::load(src + p + 8), r2);
            r3 = simd::fma(neg_s, simd::Vec4d::load(src + p + 12), r3);
            r0.store(dst + p);
            r1.store(dst + p + 4);
            r2.store(dst + p + 8);
            r3.store(dst + p + 12);
        }
        for (; p + 4 <= len; p += 4)
        {
            simd::Vec4d r = simd::Vec4d::load(dst + p);
            r = simd::fma(neg_s, simd::Vec4d::load(src + p), r);
            r.store(dst + p);
        }
    }
    else if constexpr (std::is_same_v<T, crd::f32>)
    {
        namespace simd = crd::math::simd;
        const simd::Vec8f neg_s(-s);
        for (; p + 32 <= len; p += 32)
        {
            simd::Vec8f r0 = simd::Vec8f::load(dst + p);
            simd::Vec8f r1 = simd::Vec8f::load(dst + p + 8);
            simd::Vec8f r2 = simd::Vec8f::load(dst + p + 16);
            simd::Vec8f r3 = simd::Vec8f::load(dst + p + 24);
            r0 = simd::fma(neg_s, simd::Vec8f::load(src + p), r0);
            r1 = simd::fma(neg_s, simd::Vec8f::load(src + p + 8), r1);
            r2 = simd::fma(neg_s, simd::Vec8f::load(src + p + 16), r2);
            r3 = simd::fma(neg_s, simd::Vec8f::load(src + p + 24), r3);
            r0.store(dst + p);
            r1.store(dst + p + 8);
            r2.store(dst + p + 16);
            r3.store(dst + p + 24);
        }
        for (; p + 8 <= len; p += 8)
        {
            simd::Vec8f r = simd::Vec8f::load(dst + p);
            r = simd::fma(neg_s, simd::Vec8f::load(src + p), r);
            r.store(dst + p);
        }
    }
    for (; p < len; ++p)
    {
        dst[p] -= s * src[p];
    }
}

// Symmetric row+column swap k1 ↔ k2 over the COL-MAJOR lower triangle (element
// (i,j) with i ≥ j = d[j*ld + i]). The index pairing is identical to the
// row-major dense form (it is about which (i,j) cells exchange, not the
// storage); only the addressing differs. Assumes k1, k2 < m.
template <typename T> inline void ldlt_swap_sym(T* d, crd::u32 m, crd::u32 ld, crd::u32 k1, crd::u32 k2) noexcept
{
    if (k1 == k2)
    {
        return;
    }
    if (k1 > k2)
    {
        const crd::u32 t = k1;
        k1 = k2;
        k2 = t;
    }
    const crd::usize ldz = static_cast<crd::usize>(ld);
    // el(i,j) with i >= j  (lower triangle, col-major).
    auto idx = [ldz](crd::u32 i, crd::u32 j) -> crd::usize
    { return static_cast<crd::usize>(j) * ldz + static_cast<crd::usize>(i); };
    auto sw = [&](crd::usize a, crd::usize b)
    {
        const T t = d[a];
        d[a] = d[b];
        d[b] = t;
    };
    // Step 1: diagonals.
    sw(idx(k1, k1), idx(k2, k2));
    // Step 2: left of k1 — (k1,j) ↔ (k2,j) for j in [0,k1).
    for (crd::u32 j = 0; j < k1; ++j)
    {
        sw(idx(k1, j), idx(k2, j));
    }
    // Step 3: between — (j,k1) ↔ (k2,j) for j in (k1,k2)  [j>k1, k2>j].
    for (crd::u32 j = k1 + 1; j < k2; ++j)
    {
        sw(idx(j, k1), idx(k2, j));
    }
    // Step 4: below k2 — (i,k1) ↔ (i,k2) for i in (k2,m).
    for (crd::u32 i = k2 + 1; i < m; ++i)
    {
        sw(idx(i, k1), idx(i, k2));
    }
}

// (1 + √17) / 8 — the textbook Bunch-Kaufman pivot threshold (bounds element growth most tightly, delays most
// often). A SMALLER threshold (MA57/MUMPS-style relaxed/threshold pivoting, e.g. 0.1 or 0.01) accepts weaker
// 1×1 pivots ⇒ FAR fewer Duff-Reid delays ⇒ no front blowup on indefinite multifrontal fronts (the v5d-h
// indefinite-perf lever), at the cost of looser element-growth bounds (recover accuracy via the residual gate).
inline constexpr double kBunchKaufmanAlpha = 0.6403882032022075;

// factor_front_ldlt — see the file header. `Hermitian` selects LDLᴴ (conjugated, real D-diagonal) vs LDLᵀ
// (complex-symmetric, unconjugated); for real T it is irrelevant (every conjugation is a no-op). Returns the
// number of successfully eliminated leading pivots (== npiv on full success; < npiv signals a delayed pivot
// relayed to the parent front). `block_kinds` and `piv` must each hold at least `npiv` entries. `pivot_alpha`
// is the BK threshold (default = textbook; lower ⇒ fewer delays, looser growth — see kBunchKaufmanAlpha).
template <typename T, bool Hermitian = false>
[[nodiscard]] inline crd::u32 factor_front_ldlt(T* d, crd::u32 ld, crd::u32 m, crd::u32 npiv,
                                                crd::u8* block_kinds, crd::u32* piv,
                                                double pivot_alpha = kBunchKaufmanAlpha)
{
    using R = dense::RealType<T>;
    const crd::usize ldz = static_cast<crd::usize>(ld);
    auto el = [&](crd::u32 i, crd::u32 j) -> T& { return d[static_cast<crd::usize>(j) * ldz + i]; };
    const R alpha = static_cast<R>(pivot_alpha);

    crd::u32 k = 0;
    while (k < npiv)
    {
        const R absakk = ldlt_magd<Hermitian, T>(el(k, k)); // diagonal candidate (|Re| for Hermitian)

        // colmax over the FULL column [k+1, m) (L21 reaches the CB rows).
        R colmax = R{0};
        crd::u32 imax = k;
        for (crd::u32 i = k + 1; i < m; ++i)
        {
            const R v = ldlt_mag<T>(el(i, k));
            if (v > colmax)
            {
                colmax = v;
                imax = i;
            }
        }

        if (absakk == R{0} && colmax == R{0})
        {
            return k; // structurally null fully-summed column ⇒ delayed pivot.
        }

        crd::u32 kp = k;
        crd::u32 kstep = 1;

        if (absakk >= alpha * colmax)
        {
            kp = k;
            kstep = 1;
        }
        else
        {
            // rowmax over row imax, columns in [k,m) \ {imax}.
            R rowmax = R{0};
            for (crd::u32 j = k; j < imax; ++j)
            {
                const R v = ldlt_mag<T>(el(imax, j));
                if (v > rowmax)
                {
                    rowmax = v;
                }
            }
            for (crd::u32 j = imax + 1; j < m; ++j)
            {
                const R v = ldlt_mag<T>(el(j, imax));
                if (v > rowmax)
                {
                    rowmax = v;
                }
            }

            if (absakk * rowmax >= alpha * colmax * colmax)
            {
                kp = k; // 1×1 pivot at k (no swap).
                kstep = 1;
            }
            else if (imax < npiv && ldlt_magd<Hermitian, T>(el(imax, imax)) >= alpha * rowmax)
            {
                kp = imax; // 1×1 pivot at imax (fully-summed) with swap.
                kstep = 1;
            }
            else if (imax < npiv && k + 1 < npiv)
            {
                kp = imax; // 2×2 pivot (k, imax) — both fully-summed; room for the partner slot.
                kstep = 2;
            }
            else
            {
                // The only stable pivot would land on a CB row (imax ≥ npiv) or there is no
                // fully-summed slot for the 2×2 partner ⇒ delay this fully-summed variable.
                return k;
            }
        }

        if (kstep == 1)
        {
            if (kp != k)
            {
                ldlt_swap_sym<T>(d, m, ld, k, kp);
            }
            piv[k] = kp;
            block_kinds[k] = 1U;

            const T d_kk = ldlt_pivd<Hermitian, T>(el(k, k)); // Hermitian ⇒ force real
            el(k, k) = d_kk;                                  // store the (realified) D[k] for the driver
            if (ldlt_mag<T>(d_kk) == R{0})
            {
                return k; // a zero 1×1 pivot — delay (BK should preclude this; guard anyway).
            }
            const T inv_d = T{1} / d_kk;
            // Rank-1 trailing update (lower triangle, col-outer): A[i,j] -= L[i,k]·D·conjH(L[j,k]). Read the
            // still-original column k (only columns j > k are written), then normalize column k → L21 last.
            for (crd::u32 j = k + 1; j < m; ++j)
            {
                const T s_j = ldlt_conjh<Hermitian, T>(el(j, k)) * inv_d; // conjH(A[j,k])/d ⇒ Hermitian Schur
                if (s_j == T{0})
                {
                    continue;
                }
                ldlt_axpy_negate<T>(&el(j, j), &el(j, k), s_j, m - j);
            }
            for (crd::u32 i = k + 1; i < m; ++i)
            {
                el(i, k) *= inv_d; // L[i,k] = A[i,k]/D[k] (un-conjugated)
            }
            k += 1;
        }
        else // kstep == 2
        {
            if (kp != k + 1)
            {
                ldlt_swap_sym<T>(d, m, ld, k + 1, kp);
            }
            piv[k] = kp;
            piv[k + 1] = kp;
            block_kinds[k] = 2U;
            block_kinds[k + 1] = 0U; // continuation marker.

            const T d11 = ldlt_pivd<Hermitian, T>(el(k, k)); // Hermitian ⇒ real diagonal
            const T d22 = ldlt_pivd<Hermitian, T>(el(k + 1, k + 1));
            el(k, k) = d11;
            el(k + 1, k + 1) = d22;
            const T d21 = el(k + 1, k);
            // det = d11·d22 − conjH(d21)·d21  (Hermitian: d11·d22 − |d21|², real; symmetric: d11·d22 − d21²).
            const T det = d11 * d22 - ldlt_conjh<Hermitian, T>(d21) * d21;
            if (ldlt_mag<T>(det) == R{0})
            {
                return k; // singular 2×2 — delay.
            }
            const T inv_det = T{1} / det;
            // Rank-2 trailing update: S[i,j] -= A[i,K]·inv(D)·conjH(A[j,K]). The "column-j" coefficients use
            // conjH(A[j,k]),conjH(A[j,k+1]) (Hermitian) so the Schur stays Hermitian. Read columns k,k+1
            // (only columns j ≥ k+2 are written), then normalize → L21 last.
            for (crd::u32 j = k + 2; j < m; ++j)
            {
                const T ajk = ldlt_conjh<Hermitian, T>(el(j, k));
                const T ajkp1 = ldlt_conjh<Hermitian, T>(el(j, k + 1));
                const T tfa = (d22 * ajk - ldlt_conjh<Hermitian, T>(d21) * ajkp1) * inv_det;
                const T tfb = (d11 * ajkp1 - d21 * ajk) * inv_det;
                if (tfa != T{0})
                {
                    ldlt_axpy_negate<T>(&el(j, j), &el(j, k), tfa, m - j);
                }
                if (tfb != T{0})
                {
                    ldlt_axpy_negate<T>(&el(j, j), &el(j, k + 1), tfb, m - j);
                }
            }
            // L21 = A21·inv(D) (A21 un-conjugated): L[i,k]=(d22·A[i,k]−d21·A[i,k+1])/det,
            // L[i,k+1]=(d11·A[i,k+1]−conjH(d21)·A[i,k])/det.
            for (crd::u32 i = k + 2; i < m; ++i)
            {
                const T aik = el(i, k);
                const T aikp1 = el(i, k + 1);
                el(i, k) = (d22 * aik - d21 * aikp1) * inv_det;
                el(i, k + 1) = (d11 * aikp1 - ldlt_conjh<Hermitian, T>(d21) * aik) * inv_det;
            }
            k += 2;
        }
    }
    return npiv;
}

// v5d-h/perf — BLOCKED-BLAS-3 Bunch-Kaufman LDLᵀ front factor (the lever to crush MUMPS on big indefinite
// fronts; subsumes the old SPD-only 1×1 path). Right-looking xLASYF-class: factor the npiv fully-summed
// columns in nb-wide panels; each panel does FULL BK (1×1 AND 2×2, with symmetric swaps over the WHOLE front)
// updating only the PANEL columns + the pivots' full L21 foot (BLAS-2, thin), then ONE BLAS-3 lower-triangle
// `dense::gemm` trailing update `S -= L21·(D·L21ᵀ)` per panel. Returns the number of pivots stably eliminated
// (== npiv on success; < npiv ⇒ Duff-Reid DELAYED pivots — the caller relays the rest to the parent).
//
// CORRECTNESS — fully-updated-data invariant: at panel start p0, columns [p0,m) are fully updated by pivots
// [0,p0) (prior panels' BLAS-3 trailing flushes). Within the panel the eager rank-1/2 updates touch the panel
// columns [.,p1) (incl. their full foot), so column k and any candidate imax < p1 are fully updated ⇒ the BK
// stability tests (colmax/rowmax) are EXACT. If the BK pivot candidate lies BEYOND the panel (imax ≥ p1) once
// the panel has progress (k>p0), its row is NOT yet updated ⇒ we FLUSH the panel's BLAS-3 trailing (making all
// of [k,m) fully updated) and RESTART the panel at k (where the candidate is now valid). A 2×2 that would
// straddle the panel end is likewise flushed + restarted. MOAT: deterministic (one worker/front, serial gemm,
// fixed nb) ⇒ bit-identical across worker counts. Real-only (the crush regime); complex keeps the unblocked
// path. `scratch` backs the W panel + the gemm packing. (kLdltBlockBail retained for ABI; never returned now.)
inline constexpr crd::u32 kLdltBlockBail = 0xFFFFFFFEU;

template <typename T>
[[nodiscard]] inline crd::u32 factor_front_ldlt_blocked(T* d, crd::u32 ld, crd::u32 m, crd::u32 npiv,
                                                        crd::u8* block_kinds, crd::u32* piv, crd::u32 nb,
                                                        crd::memory::IAllocator* scratch,
                                                        double pivot_alpha = kBunchKaufmanAlpha)
{
    static_assert(!dense::is_complex_v<T>, "factor_front_ldlt_blocked is real-only (the crush regime)");
    namespace dl = crd::hesap::dense;
    const crd::usize ldz = static_cast<crd::usize>(ld);
    auto el = [&](crd::u32 i, crd::u32 j) -> T& { return d[static_cast<crd::usize>(j) * ldz + i]; };
    auto mag = [](T x) -> T { return x < T{0} ? -x : x; };
    const T alpha = static_cast<T>(pivot_alpha); // BK threshold; lower ⇒ fewer delays (see kBunchKaufmanAlpha)
    crd::containers::Array<T> w(scratch);               // W = D·L21ᵀ panel scratch (col-major)

    // BLAS-3 trailing flush: trailing[b:,b:] -= L21·(D·L21ᵀ) using the ELIMINATED pivots [a, ke), lower
    // triangle only (syrk-equivalent), W = D·L21ᵀ handling 1×1 and 2×2 D blocks. b (the trailing start) is the
    // tentative panel end p1 — NOT ke — because columns [ke, p1) were already updated eagerly by the within-
    // panel rank-1/2 (so flushing them would double-count); the flush only covers the deferred trailing [p1,m).
    auto flush = [&](crd::u32 a, crd::u32 ke, crd::u32 b)
    {
        const crd::u32 tr = m - b;
        const crd::u32 bw = ke - a;
        if (tr == 0 || bw == 0)
        {
            return;
        }
        w.resize(static_cast<crd::usize>(tr) * bw);
        crd::u32 jj = a;
        while (jj < ke)
        {
            const crd::u32 c = jj - a;
            if (block_kinds[jj] == 1U)
            {
                const T dj = el(jj, jj);
                for (crd::u32 i = 0; i < tr; ++i)
                {
                    w[static_cast<crd::usize>(c) * tr + i] = el(b + i, jj) * dj;
                }
                jj += 1;
            }
            else // 2×2 block (jj, jj+1): W[:,c]=L[:,jj]·d11+L[:,jj+1]·d21, W[:,c+1]=L[:,jj]·d21+L[:,jj+1]·d22
            {
                const T d11 = el(jj, jj);
                const T d22 = el(jj + 1, jj + 1);
                const T d21 = el(jj + 1, jj);
                for (crd::u32 i = 0; i < tr; ++i)
                {
                    const T lij = el(b + i, jj);
                    const T lij1 = el(b + i, jj + 1);
                    w[static_cast<crd::usize>(c) * tr + i] = lij * d11 + lij1 * d21;
                    w[static_cast<crd::usize>(c + 1) * tr + i] = lij * d21 + lij1 * d22;
                }
                jj += 2;
            }
        }
        constexpr crd::u32 trail_tile = 192;
        for (crd::u32 cj0 = 0; cj0 < tr; cj0 += trail_tile)
        {
            const crd::u32 cj1 = (cj0 + trail_tile < tr) ? cj0 + trail_tile : tr;
            const crd::u32 rrows = tr - cj0;
            const crd::u32 ccols = cj1 - cj0;
            const dl::MatrixView<const T, dl::Layout::ColMajor> l21(&el(b + cj0, a), rrows, bw, ld);
            const dl::MatrixView<const T, dl::Layout::ColMajor> wv(&w[cj0], ccols, bw, tr);
            dl::MatrixView<T, dl::Layout::ColMajor> c22(&el(b + cj0, b + cj0), rrows, ccols, ld);
            dl::gemm<T, dl::Layout::ColMajor>(T{-1}, l21, wv, T{1}, c22, dl::Trans::None, dl::Trans::Transpose, scratch);
        }
    };

    crd::u32 p0 = 0;
    while (p0 < npiv)
    {
        const crd::u32 p1 = (p0 + nb < npiv) ? p0 + nb : npiv; // tentative panel end
        crd::u32 kk = p0;
        bool restart = false; // a pivot candidate fell beyond the panel ⇒ flush + restart at kk
        while (kk < p1)
        {
            const T absakk = mag(el(kk, kk));
            T colmax = T{0};
            crd::u32 imax = kk;
            for (crd::u32 i = kk + 1; i < m; ++i)
            {
                const T v = mag(el(i, kk));
                if (v > colmax)
                {
                    colmax = v;
                    imax = i;
                }
            }
            if (absakk == T{0} && colmax == T{0})
            {
                flush(p0, kk, p1);
                return kk; // structurally null fully-summed column ⇒ delay
            }
            crd::u32 kp = kk;
            crd::u32 kstep = 1;
            if (absakk >= alpha * colmax)
            {
                kp = kk;
                kstep = 1;
            }
            else
            {
                if (imax >= p1 && kk > p0)
                {
                    flush(p0, kk, p1); // candidate beyond the panel + panel has progress ⇒ flush + restart
                    restart = true;
                    break;
                }
                // imax is fully updated (imax < p1, OR kk == p0 so all of [p0,m) is current) ⇒ rowmax exact.
                T rowmax = T{0};
                for (crd::u32 j = kk; j < imax; ++j)
                {
                    const T v = mag(el(imax, j));
                    if (v > rowmax)
                    {
                        rowmax = v;
                    }
                }
                for (crd::u32 j = imax + 1; j < m; ++j)
                {
                    const T v = mag(el(j, imax));
                    if (v > rowmax)
                    {
                        rowmax = v;
                    }
                }
                if (absakk * rowmax >= alpha * colmax * colmax)
                {
                    kp = kk;
                    kstep = 1;
                }
                else if (imax < npiv && mag(el(imax, imax)) >= alpha * rowmax)
                {
                    kp = imax;
                    kstep = 1;
                }
                else if (imax < npiv && kk + 1 < npiv)
                {
                    kp = imax;
                    kstep = 2;
                }
                else
                {
                    flush(p0, kk, p1);
                    return kk; // stable pivot would land on a CB row / no 2×2 slot ⇒ delay
                }
            }
            if (kstep == 2 && kk + 1 >= p1)
            {
                flush(p0, kk, p1); // the 2×2 would straddle the panel end ⇒ flush + restart at kk
                restart = true;
                break;
            }

            if (kstep == 1)
            {
                if (kp != kk)
                {
                    ldlt_swap_sym<T>(d, m, ld, kk, kp);
                }
                piv[kk] = kp;
                block_kinds[kk] = 1U;
                const T d_kk = el(kk, kk);
                if (d_kk == T{0})
                {
                    flush(p0, kk, p1);
                    return kk;
                }
                const T inv_d = T{1} / d_kk;
                for (crd::u32 j = kk + 1; j < p1; ++j) // rank-1 within the PANEL columns + their foot
                {
                    const T s = el(j, kk) * inv_d;
                    if (s == T{0})
                    {
                        continue;
                    }
                    ldlt_axpy_negate<T>(&el(j, j), &el(j, kk), s, m - j);
                }
                for (crd::u32 i = kk + 1; i < m; ++i)
                {
                    el(i, kk) *= inv_d; // normalize column kk → full L21 (incl. foot, used by the flush)
                }
                kk += 1;
            }
            else // kstep == 2
            {
                if (kp != kk + 1)
                {
                    ldlt_swap_sym<T>(d, m, ld, kk + 1, kp);
                }
                piv[kk] = kp;
                piv[kk + 1] = kp;
                block_kinds[kk] = 2U;
                block_kinds[kk + 1] = 0U;
                const T d11 = el(kk, kk);
                const T d22 = el(kk + 1, kk + 1);
                const T d21 = el(kk + 1, kk);
                const T det = d11 * d22 - d21 * d21;
                if (det == T{0})
                {
                    flush(p0, kk, p1);
                    return kk;
                }
                const T inv_det = T{1} / det;
                for (crd::u32 j = kk + 2; j < p1; ++j) // rank-2 within the PANEL columns + their foot
                {
                    const T ajk = el(j, kk);
                    const T ajkp1 = el(j, kk + 1);
                    const T tfa = (d22 * ajk - d21 * ajkp1) * inv_det;
                    const T tfb = (d11 * ajkp1 - d21 * ajk) * inv_det;
                    if (tfa != T{0})
                    {
                        ldlt_axpy_negate<T>(&el(j, j), &el(j, kk), tfa, m - j);
                    }
                    if (tfb != T{0})
                    {
                        ldlt_axpy_negate<T>(&el(j, j), &el(j, kk + 1), tfb, m - j);
                    }
                }
                for (crd::u32 i = kk + 2; i < m; ++i) // normalize cols kk, kk+1 → full L21
                {
                    const T aik = el(i, kk);
                    const T aikp1 = el(i, kk + 1);
                    el(i, kk) = (d22 * aik - d21 * aikp1) * inv_det;
                    el(i, kk + 1) = (d11 * aikp1 - d21 * aik) * inv_det;
                }
                kk += 2;
            }
        }
        if (!restart)
        {
            flush(p0, kk, p1); // normal panel-end BLAS-3 trailing update for pivots [p0, kk)
        }
        p0 = kk; // advance (restart sets kk == the flush point; progress is guaranteed since flush needs kk>p0)
    }
    return npiv;
}

} // namespace crd::hesap::direct
