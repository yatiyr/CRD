#include <crd/hesap/dense/ldlt.hpp>

#include <crd/containers/array.hpp>
#include <crd/core/assert.hpp>
#include <crd/math/simd/vec4d.hpp>
#include <crd/math/simd/vec8f.hpp>

#include <cmath>
#include <type_traits>

namespace crd::hesap::dense
{
namespace
{
// Bunch-Kaufman constant. ALPHA = (1 + sqrt(17)) / 8 ≈ 0.6403882032022075.
template <typename T>
inline T bk_alpha() noexcept
{
    return static_cast<T>(0.6403882032022075);
}

template <typename T>
inline T abs_value(T x) noexcept
{
    return x < T{0} ? -x : x;
}

// Symmetric row+column swap: exchange row k1 with k2 AND column k1 with k2
// in the lower triangle of a packed n×n matrix. Off-diagonal lower-triangle
// entries (j < min(k1, k2)) swap as a row pair; entries between the swap
// indices need extra care (the (max(k1,k2), j) entry corresponds to
// position (j, max) in the upper triangle, which we don't store).
//
// Standard implementation (assume k1 < k2):
//   1. Swap A[k1, k1] ↔ A[k2, k2] (diagonals).
//   2. Swap A[k1, j] ↔ A[k2, j] for j in [0, k1).
//   3. For j in (k1, k2): swap A[j, k1] ↔ A[k2, j] (off-diagonal pair).
//   4. Swap A[i, k1] ↔ A[i, k2] for i in (k2, n).
template <typename T>
void swap_sym(T* data, crd::usize n, crd::usize ld, crd::usize k1, crd::usize k2)
{
    if (k1 == k2)
    {
        return;
    }
    if (k1 > k2)
    {
        const crd::usize tmp = k1;
        k1 = k2;
        k2 = tmp;
    }
    // Step 1: diagonals.
    T tmp = data[k1 * ld + k1];
    data[k1 * ld + k1] = data[k2 * ld + k2];
    data[k2 * ld + k2] = tmp;
    // Step 2: left of k1.
    for (crd::usize j = 0; j < k1; ++j)
    {
        T t = data[k1 * ld + j];
        data[k1 * ld + j] = data[k2 * ld + j];
        data[k2 * ld + j] = t;
    }
    // Step 3: between k1 and k2 — the (j, k1) entry pairs with (k2, j).
    for (crd::usize j = k1 + 1; j < k2; ++j)
    {
        T t = data[j * ld + k1];
        data[j * ld + k1] = data[k2 * ld + j];
        data[k2 * ld + j] = t;
    }
    // Step 4: below k2.
    for (crd::usize i = k2 + 1; i < n; ++i)
    {
        T t = data[i * ld + k1];
        data[i * ld + k1] = data[i * ld + k2];
        data[i * ld + k2] = t;
    }
}

// SIMD axpy-negate: row[p] -= s * col[p] for p in [0, len). f32 / f64
// fast paths via Vec8f / Vec4d + FMA; scalar tail.
template <typename T>
inline void axpy_negate(T* row, const T* col, T s, crd::usize len) noexcept
{
    crd::usize p = 0;
    if constexpr (std::is_same_v<T, crd::f64>)
    {
        namespace simd = crd::math::simd;
        const simd::Vec4d neg_s_v(-s);
        for (; p + 16 <= len; p += 16)
        {
            simd::Vec4d r0 = simd::Vec4d::load(row + p);
            simd::Vec4d r1 = simd::Vec4d::load(row + p + 4);
            simd::Vec4d r2 = simd::Vec4d::load(row + p + 8);
            simd::Vec4d r3 = simd::Vec4d::load(row + p + 12);
            const simd::Vec4d c0 = simd::Vec4d::load(col + p);
            const simd::Vec4d c1 = simd::Vec4d::load(col + p + 4);
            const simd::Vec4d c2 = simd::Vec4d::load(col + p + 8);
            const simd::Vec4d c3 = simd::Vec4d::load(col + p + 12);
            r0 = simd::fma(neg_s_v, c0, r0);
            r1 = simd::fma(neg_s_v, c1, r1);
            r2 = simd::fma(neg_s_v, c2, r2);
            r3 = simd::fma(neg_s_v, c3, r3);
            r0.store(row + p);
            r1.store(row + p + 4);
            r2.store(row + p + 8);
            r3.store(row + p + 12);
        }
        for (; p + 4 <= len; p += 4)
        {
            simd::Vec4d r = simd::Vec4d::load(row + p);
            const simd::Vec4d c = simd::Vec4d::load(col + p);
            r = simd::fma(neg_s_v, c, r);
            r.store(row + p);
        }
    }
    else if constexpr (std::is_same_v<T, crd::f32>)
    {
        namespace simd = crd::math::simd;
        const simd::Vec8f neg_s_v(-s);
        for (; p + 32 <= len; p += 32)
        {
            simd::Vec8f r0 = simd::Vec8f::load(row + p);
            simd::Vec8f r1 = simd::Vec8f::load(row + p + 8);
            simd::Vec8f r2 = simd::Vec8f::load(row + p + 16);
            simd::Vec8f r3 = simd::Vec8f::load(row + p + 24);
            const simd::Vec8f c0 = simd::Vec8f::load(col + p);
            const simd::Vec8f c1 = simd::Vec8f::load(col + p + 8);
            const simd::Vec8f c2 = simd::Vec8f::load(col + p + 16);
            const simd::Vec8f c3 = simd::Vec8f::load(col + p + 24);
            r0 = simd::fma(neg_s_v, c0, r0);
            r1 = simd::fma(neg_s_v, c1, r1);
            r2 = simd::fma(neg_s_v, c2, r2);
            r3 = simd::fma(neg_s_v, c3, r3);
            r0.store(row + p);
            r1.store(row + p + 8);
            r2.store(row + p + 16);
            r3.store(row + p + 24);
        }
        for (; p + 8 <= len; p += 8)
        {
            simd::Vec8f r = simd::Vec8f::load(row + p);
            const simd::Vec8f c = simd::Vec8f::load(col + p);
            r = simd::fma(neg_s_v, c, r);
            r.store(row + p);
        }
    }
    for (; p < len; ++p)
    {
        row[p] -= s * col[p];
    }
}

} // namespace

template <typename T, Layout L>
void factor_ldlt(LDLT<T, L>& ldlt)
{
    static_assert(L == Layout::RowMajor, "factor_ldlt currently supports RowMajor only");

    Matrix<T, L>& packed = ldlt.packed();
    Permutation& perm = ldlt.permutation();
    const crd::usize n = packed.rows();
    CRD_ASSERT_MSG(packed.cols() == n, "factor_ldlt: matrix must be square");
    CRD_ASSERT_MSG(perm.n() == n, "factor_ldlt: permutation size mismatch");

    ldlt.set_info(0);
    perm.set_identity();

    if (n == 0)
    {
        return;
    }

    T* data = packed.data();
    const crd::usize ld = packed.ld();
    auto& block_kinds = ldlt.block_kinds();
    for (crd::usize i = 0; i < n; ++i)
    {
        block_kinds[i] = 0U;
    }

    const T alpha = bk_alpha<T>();

    crd::usize k = 0;
    while (k < n)
    {
        // === Pivot selection ===
        const T absakk = abs_value<T>(data[k * ld + k]);

        T colmax = T{0};
        crd::usize imax = k;
        for (crd::usize i = k + 1; i < n; ++i)
        {
            const T v = abs_value<T>(data[i * ld + k]);
            if (v > colmax)
            {
                colmax = v;
                imax = i;
            }
        }

        if (absakk == T{0} && colmax == T{0})
        {
            ldlt.set_info(k + 1);
            return;
        }

        crd::usize kp = k;
        crd::usize kstep = 1;

        if (absakk >= alpha * colmax)
        {
            // 1×1 pivot at (k, k); no swap.
            kp = k;
            kstep = 1;
        }
        else
        {
            // Find rowmax = max over off-diagonal entries in row imax,
            // considering lower-triangle access: for j in [k, imax) use
            // data[imax * ld + j]; for j in (imax, n) use data[j * ld + imax].
            T rowmax = T{0};
            for (crd::usize j = k; j < imax; ++j)
            {
                const T v = abs_value<T>(data[imax * ld + j]);
                if (v > rowmax)
                {
                    rowmax = v;
                }
            }
            for (crd::usize j = imax + 1; j < n; ++j)
            {
                const T v = abs_value<T>(data[j * ld + imax]);
                if (v > rowmax)
                {
                    rowmax = v;
                }
            }

            if (absakk * rowmax >= alpha * colmax * colmax)
            {
                // 1×1 pivot at (k, k); no swap.
                kp = k;
                kstep = 1;
            }
            else if (abs_value<T>(data[imax * ld + imax]) >= alpha * rowmax)
            {
                // 1×1 pivot at (imax, imax); swap k and imax.
                kp = imax;
                kstep = 1;
            }
            else
            {
                // 2×2 pivot; swap k+1 and imax (if needed).
                kp = imax;
                kstep = 2;
            }
        }

        // === Apply swap ===
        if (kstep == 1)
        {
            if (kp != k)
            {
                swap_sym<T>(data, n, ld, k, kp);
            }
            perm.pivot_at(k) = kp;
            block_kinds[k] = 1U;

            // === 1×1 factor step ===
            // D[k, k] = A[k, k]; L[i, k] = A[i, k] / D[k, k] for i > k.
            // Trailing update: A[i, j] -= L[i, k] * L[j, k] * D[k, k]
            //                          = A[i, k] (post-norm) * A[j, k] (pre-norm).
            // Save A[i, k] pre-normalization in scratch, then normalize, then
            // update.
            const T d_kk = data[k * ld + k];
            if (d_kk == T{0})
            {
                ldlt.set_info(k + 1);
                return;
            }
            const T inv_d = T{1} / d_kk;

            // SIMD-tightened trailing update via row-restructured pattern.
            // Trailing update is: A[i, j] -= A[i, k] * A[j, k] / d  for i, j in
            // [k+1, n), i >= j. Pack column k below diag into a contiguous
            // buffer so the inner loop (over panel columns) becomes a
            // CONTIGUOUS row-segment update.
            const crd::usize tail = n - k - 1;
            if (tail > 0)
            {
                crd::containers::Array<T> col_k_buf(packed.allocator());
                col_k_buf.resize(tail);
                for (crd::usize i = 0; i < tail; ++i)
                {
                    col_k_buf[i] = data[(k + 1 + i) * ld + k];
                }
                // Row-major sweep: for each row i in [k+1, n), apply rank-1
                // update on row segment row_i[k+1..i] -= s_i * col_k_buf[0..i-k-1]
                // where s_i = col_k_buf[i-k-1] * inv_d.
                for (crd::usize i = k + 1; i < n; ++i)
                {
                    const crd::usize off = i - k - 1;
                    const T s_i = col_k_buf[off] * inv_d;
                    if (s_i == T{0})
                    {
                        continue;
                    }
                    T* row_i = data + i * ld + (k + 1);
                    axpy_negate<T>(row_i, col_k_buf.data(), s_i, off + 1);
                }
                // Now normalize column k: write the L21 entries back.
                for (crd::usize i = 0; i < tail; ++i)
                {
                    data[(k + 1 + i) * ld + k] = col_k_buf[i] * inv_d;
                }
            }
            k += 1;
        }
        else  // kstep == 2
        {
            // Ensure the 2×2 block sits at rows (k, k+1) by swapping
            // k+1 and kp = imax.
            if (kp != k + 1)
            {
                swap_sym<T>(data, n, ld, k + 1, kp);
            }
            // Use LAPACK sign convention: pivots[k] = pivots[k+1] = kp
            // but record as a "2x2 block" via block_kinds.
            perm.pivot_at(k) = kp;
            perm.pivot_at(k + 1) = kp;
            block_kinds[k] = 2U;
            block_kinds[k + 1] = 0U;  // continuation marker

            const T d11 = data[k * ld + k];
            const T d21 = data[(k + 1) * ld + k];
            const T d22 = data[(k + 1) * ld + (k + 1)];
            const T det = d11 * d22 - d21 * d21;
            if (det == T{0})
            {
                ldlt.set_info(k + 1);
                return;
            }

            // 2×2 trailing update: A[i, j] -= [A_orig[i,k], A_orig[i,k+1]]
            //                                · inv(D) · [A_orig[j,k], A_orig[j,k+1]]^T
            // for i, j in [k+2, n), i >= j. Pack BOTH columns k and k+1
            // below the 2×2 diagonal into contiguous buffers, then sweep
            // rows with SIMD axpy-negate updates.
            const crd::usize tail2 = (n > k + 2) ? (n - k - 2) : 0U;
            if (tail2 > 0)
            {
                crd::containers::Array<T> col_a(packed.allocator());
                col_a.resize(tail2);
                crd::containers::Array<T> col_b(packed.allocator());
                col_b.resize(tail2);
                crd::containers::Array<T> fac_a(packed.allocator());
                fac_a.resize(tail2);
                crd::containers::Array<T> fac_b(packed.allocator());
                fac_b.resize(tail2);
                const T inv_det = T{1} / det;
                for (crd::usize i = 0; i < tail2; ++i)
                {
                    const T ajk = data[(k + 2 + i) * ld + k];
                    const T ajkp1 = data[(k + 2 + i) * ld + (k + 1)];
                    col_a[i] = ajk;
                    col_b[i] = ajkp1;
                    // factor row j = inv(D) · [ajk, ajkp1]
                    fac_a[i] = (d22 * ajk - d21 * ajkp1) * inv_det;
                    fac_b[i] = (d11 * ajkp1 - d21 * ajk) * inv_det;
                }
                // Row-major sweep: A[i, j] -= col_a[i_off] · fac_a[j_off]
                //                            + col_b[i_off] · fac_b[j_off]
                for (crd::usize i = k + 2; i < n; ++i)
                {
                    const crd::usize off = i - k - 2;
                    const T aik = col_a[off];
                    const T aikp1 = col_b[off];
                    T* row_i = data + i * ld + (k + 2);
                    axpy_negate<T>(row_i, fac_a.data(), aik, off + 1);
                    axpy_negate<T>(row_i, fac_b.data(), aikp1, off + 1);
                }
                // Now normalize columns k and k+1 to L21.
                for (crd::usize i = 0; i < tail2; ++i)
                {
                    data[(k + 2 + i) * ld + k] = fac_a[i];
                    data[(k + 2 + i) * ld + (k + 1)] = fac_b[i];
                }
            }
            k += 2;
        }
    }
}

template <typename T, Layout L>
void solve_ldlt(const LDLT<T, L>& ldlt, crd::containers::Span<T> x)
{
    static_assert(L == Layout::RowMajor, "solve_ldlt currently supports RowMajor only");

    const Matrix<T, L>& packed = ldlt.packed();
    const Permutation& perm = ldlt.permutation();
    const crd::usize n = packed.rows();
    CRD_ASSERT_MSG(x.size() == n, "solve_ldlt: RHS size != n");
    CRD_ASSERT_MSG(!ldlt.is_singular(), "solve_ldlt: factor is singular (info != 0)");

    if (n == 0)
    {
        return;
    }

    const T* data = packed.data();
    const crd::usize ld = packed.ld();

    // === Step 1: apply P (forward replay) ===
    // For each step k: swap x[k] with x[piv[k]] (LDLT convention: piv[k]
    // is the same for both rows of a 2×2 block; we still only do the
    // swap once per block step).
    {
        crd::usize k = 0;
        while (k < n)
        {
            const crd::u8 kind = ldlt.block_kind(k);
            if (kind == 1U)
            {
                const crd::usize r = perm.pivot_at(k);
                if (r != k)
                {
                    const T t = x[k];
                    x[k] = x[r];
                    x[r] = t;
                }
                k += 1;
            }
            else
            {
                // 2×2: swap was applied at row k+1 with row piv[k] during factor.
                const crd::usize r = perm.pivot_at(k);
                if (r != k + 1)
                {
                    const T t = x[k + 1];
                    x[k + 1] = x[r];
                    x[r] = t;
                }
                k += 2;
            }
        }
    }

    // === Step 2: forward sub  L · y = P·b  (in-place, x holds P·b) ===
    // Walk rows top-down, subtracting L · already-solved-prefix.
    // For 1×1 pivots at position k: standard unit-lower forward step.
    // For 2×2 pivots at (k, k+1): L has unit diagonal in positions k, k+1
    // and zero at L[k+1, k] (the (k+1, k) slot stores D[k+1, k], not L).
    {
        crd::usize k = 0;
        while (k < n)
        {
            const crd::u8 kind = ldlt.block_kind(k);
            if (kind == 1U)
            {
                T s = x[k];
                for (crd::usize j = 0; j < k; ++j)
                {
                    s -= data[k * ld + j] * x[j];
                }
                x[k] = s;
                k += 1;
            }
            else
            {
                // Row k of L: standard forward.
                T s0 = x[k];
                for (crd::usize j = 0; j < k; ++j)
                {
                    s0 -= data[k * ld + j] * x[j];
                }
                x[k] = s0;
                // Row k+1 of L: skip (k+1, k) since that slot is D[k+1, k],
                // not L[k+1, k] (which is implicitly 0 for a 2×2 pivot block).
                T s1 = x[k + 1];
                for (crd::usize j = 0; j < k; ++j)
                {
                    s1 -= data[(k + 1) * ld + j] * x[j];
                }
                x[k + 1] = s1;
                k += 2;
            }
        }
    }

    // === Step 3: D · z = y  (block-aware) ===
    {
        crd::usize k = 0;
        while (k < n)
        {
            const crd::u8 kind = ldlt.block_kind(k);
            if (kind == 1U)
            {
                x[k] = x[k] / data[k * ld + k];
                k += 1;
            }
            else
            {
                const T d11 = data[k * ld + k];
                const T d21 = data[(k + 1) * ld + k];
                const T d22 = data[(k + 1) * ld + (k + 1)];
                const T det = d11 * d22 - d21 * d21;
                const T y0 = x[k];
                const T y1 = x[k + 1];
                // [z0, z1]^T = inv(D) * [y0, y1]^T  = (1/det) * [[d22, -d21], [-d21, d11]] · [y0, y1]
                x[k] = (d22 * y0 - d21 * y1) / det;
                x[k + 1] = (d11 * y1 - d21 * y0) / det;
                k += 2;
            }
        }
    }

    // === Step 4: back sub  L^T · w = z ===
    // Walk rows bottom-up. For 2×2 block at (k, k+1), L[k+1, k] is 0.
    {
        crd::usize ii = n;
        while (ii > 0)
        {
            const crd::usize k = ii - 1;
            // Find the block kind at position k: it's a 1×1 if block_kinds[k]==1,
            // OR a continuation (block_kinds[k]==0) where the block start is at k-1.
            const crd::u8 kind_here = ldlt.block_kind(k);
            if (kind_here == 1U)
            {
                T s = x[k];
                for (crd::usize j = k + 1; j < n; ++j)
                {
                    s -= data[j * ld + k] * x[j];
                }
                x[k] = s;
                ii -= 1;
            }
            else
            {
                // 2×2 block: continuation marker at k means the block-start is at k-1.
                // Process the 2 rows together: row k (top, was the (k-1)-th step's k+1)
                // ... actually for back-sub of L^T we just walk row-by-row anyway, since
                // L^T is unit-upper-triangular and L^T[k, k+1] is the (k+1, k) entry of L
                // — which for a 2×2 block is 0 (the D[k+1, k] slot, not L). So
                // L^T's (k, k+1) is 0 and we skip it in the inner sum.
                //
                // Equivalent: just walk j = k+1..n-1 and subtract data[j*ld+k] * x[j],
                // EXCEPT skip j = k+1 if (k, k+1) is the same 2×2 block.
                T s = x[k];
                for (crd::usize j = k + 1; j < n; ++j)
                {
                    // Skip j == k+1 only if k is the continuation marker (i.e., k-1
                    // is the block start). We're at k = ii-1 and kind_here == 0,
                    // meaning k is part of a 2×2 block whose start is k-1. So the
                    // partner row is k-1, not k+1. So we DON'T skip j = k+1 here —
                    // it's a normal row beyond the 2×2 block.
                    s -= data[j * ld + k] * x[j];
                }
                x[k] = s;
                // Now handle the top row of the block (k-1).
                const crd::usize ktop = k - 1;
                T s_top = x[ktop];
                for (crd::usize j = ktop + 1; j < n; ++j)
                {
                    // Skip j = ktop + 1 = k since L[k, ktop] = D[k, ktop], not L.
                    if (j == k)
                    {
                        continue;
                    }
                    s_top -= data[j * ld + ktop] * x[j];
                }
                x[ktop] = s_top;
                ii -= 2;
            }
        }
    }

    // === Step 5: apply P^T (reverse replay of forward swaps) ===
    {
        // Walk blocks in REVERSE order and undo each swap.
        crd::usize ii = n;
        while (ii > 0)
        {
            const crd::usize k = ii - 1;
            const crd::u8 kind_here = ldlt.block_kind(k);
            if (kind_here == 1U)
            {
                const crd::usize r = perm.pivot_at(k);
                if (r != k)
                {
                    const T t = x[k];
                    x[k] = x[r];
                    x[r] = t;
                }
                ii -= 1;
            }
            else
            {
                // Continuation marker; block start is k-1.
                const crd::usize kstart = k - 1;
                const crd::usize r = perm.pivot_at(kstart);
                if (r != kstart + 1)
                {
                    const T t = x[kstart + 1];
                    x[kstart + 1] = x[r];
                    x[r] = t;
                }
                ii -= 2;
            }
        }
    }
}

// Explicit instantiations.
template void factor_ldlt<float, Layout::RowMajor>(LDLT<float, Layout::RowMajor>&);
template void factor_ldlt<double, Layout::RowMajor>(LDLT<double, Layout::RowMajor>&);
template void solve_ldlt<float, Layout::RowMajor>(const LDLT<float, Layout::RowMajor>&,
                                                   crd::containers::Span<float>);
template void solve_ldlt<double, Layout::RowMajor>(const LDLT<double, Layout::RowMajor>&,
                                                    crd::containers::Span<double>);

} // namespace crd::hesap::dense
