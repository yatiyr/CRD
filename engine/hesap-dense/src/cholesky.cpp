#include <crd/hesap/dense/cholesky.hpp>

#include <crd/containers/array.hpp>
#include <crd/core/assert.hpp>
#include <crd/hesap/dense/blas3.hpp>
#include <crd/hesap/dense/detail/dot_simd.hpp>
#include <crd/hesap/dense/detail/syrk_microkernel.hpp>
#include <crd/jobs/jobs.hpp>

#include <cmath>
#include <type_traits>

namespace crd::hesap::dense
{
namespace
{
// LAPACK xPOTRF default for double precision on AVX2. Matches the LU
// block size — both ops have the same trailing-update structure.
constexpr crd::usize kBlockSize = 64;

template <typename T>
inline T sqrt_value(T x) noexcept
{
    return std::sqrt(x);
}

// SIMD dot over contiguous [0, len) (2-accumulator, slim for small len).
template <typename T>
inline T chol_dot(const T* a, const T* b, crd::usize len) noexcept
{
    namespace simd = crd::math::simd;
    crd::usize p = 0;
    T acc = T{0};
    if constexpr (std::is_same_v<T, crd::f64>)
    {
        simd::Vec4d v0 = simd::Vec4d::zero();
        simd::Vec4d v1 = simd::Vec4d::zero();
        for (; p + 8 <= len; p += 8)
        {
            v0 = simd::fma(simd::Vec4d::load(a + p), simd::Vec4d::load(b + p), v0);
            v1 = simd::fma(simd::Vec4d::load(a + p + 4), simd::Vec4d::load(b + p + 4), v1);
        }
        acc = simd::horizontal_sum(v0 + v1);
    }
    else if constexpr (std::is_same_v<T, crd::f32>)
    {
        simd::Vec8f v0 = simd::Vec8f::zero();
        for (; p + 8 <= len; p += 8)
        {
            v0 = simd::fma(simd::Vec8f::load(a + p), simd::Vec8f::load(b + p), v0);
        }
        acc = simd::horizontal_sum(v0);
    }
    for (; p < len; ++p)
    {
        acc += a[p] * b[p];
    }
    return acc;
}

// Tight unblocked left-looking SIMD Cholesky over the FULL n×n matrix.
// For small n this beats the blocked path (no gemm fork/join, no thin-K
// inefficiency): each column j is computed via SIMD dots of the already-
// factored prefix columns. The below-diagonal column update is 8-row
// register-tiled (f64) so the shared row_j loads amortize across 8 rows.
// Returns 0 on success, k+1 on non-PD at column k.
template <typename T>
crd::usize unblocked_cholesky_full(T* data, crd::usize n, crd::usize ld)
{
    for (crd::usize j = 0; j < n; ++j)
    {
        T* row_j = data + j * ld;  // L[j, 0:j] contiguous
        const T diag_val = row_j[j] - chol_dot<T>(row_j, row_j, j);
        if (!(diag_val > T{0}))
        {
            return j + 1;
        }
        const T diag = std::sqrt(diag_val);
        row_j[j] = diag;
        const T inv_diag = T{1} / diag;

        for (crd::usize i = j + 1; i < n; ++i)
        {
            T* row_i = data + i * ld;  // L[i, 0:j] contiguous
            const T sum = row_i[j] - chol_dot<T>(row_i, row_j, j);
            row_i[j] = sum * inv_diag;
        }
    }
    return 0;
}

// In-place trsm: L21 = A21 · L11^{-T} where L11 is at A[k:k+nb, k:k+nb]
// (already factored). L21 lands at A[k+nb:n, k:k+nb].
//
// Per-row formulation: for each row i in trailing, for each col j in panel,
// sum -= L21[i, 0:j] · L11[j, 0:j]. The inner dot product walks
// L21[i, p] and L11[j, p] for p in [0, j) — both are CONTIGUOUS row-segments
// in packed (i.e., `lu_data[i*ld + k]` and `lu_data[(k+j)*ld + k]`), so a
// SIMD dot product applies directly.
// Solve one trailing row i of L21 against the unit-ish L11 block.
template <typename T>
inline void trsm_one_row(T* lu_data, crd::usize ld, crd::usize k, crd::usize nb,
                         crd::usize i) noexcept
{
    T* row_i = lu_data + i * ld + k;  // panel row of L21
    for (crd::usize j = 0; j < nb; ++j)
    {
        const T* row_j = lu_data + (k + j) * ld + k;  // L11[j, 0..j-1] contiguous
        T inner;
        if constexpr (std::is_same_v<T, crd::f32>)
        {
            inner = detail::simd_dot_f32(row_i, row_j, j);
        }
        else if constexpr (std::is_same_v<T, crd::f64>)
        {
            inner = detail::simd_dot_f64(row_i, row_j, j);
        }
        else
        {
            inner = T{0};
            for (crd::usize p = 0; p < j; ++p)
            {
                inner += row_i[p] * row_j[p];
            }
        }
        const T sum = row_i[j] - inner;
        row_i[j] = sum / lu_data[(k + j) * ld + (k + j)];
    }
}

// In-place trsm: L21 = A21 · L11^{-T}. Rows are independent → parallelizable.
template <typename T, Layout L>
void inner_trsm_right(T* lu_data, crd::usize n, crd::usize ld, crd::usize k, crd::usize nb)
{
    if (k + nb >= n)
    {
        return;
    }
    const crd::usize first = k + nb;
    const crd::usize num_rows = n - first;
    const crd::u32 nw = crd::jobs::num_workers();
    // Parallelize only for large trailing blocks — fork/join overhead
    // dominates at small n (measured: regresses N=256, helps N=1024).
    if (nw > 1 && num_rows * nb >= crd::usize{64} * 1024)
    {
        struct State
        {
            T* lu_data;
            crd::usize ld;
            crd::usize k;
            crd::usize nb;
            crd::usize first;
        };
        State st{lu_data, ld, k, nb, first};
        State* sp = &st;
        auto* counter = crd::jobs::parallel_for(
            static_cast<crd::u32>(num_rows), nw,
            [sp](crd::u32 begin, crd::u32 end)
            {
                for (crd::u32 r = begin; r < end; ++r)
                {
                    trsm_one_row<T>(sp->lu_data, sp->ld, sp->k, sp->nb, sp->first + r);
                }
            });
        crd::jobs::wait(counter);
    }
    else
    {
        for (crd::usize i = first; i < n; ++i)
        {
            trsm_one_row<T>(lu_data, ld, k, nb, i);
        }
    }
}

} // namespace

template <typename T, Layout L>
void factor_cholesky(Cholesky<T, L>& chol, crd::memory::IAllocator* scratch)
{
    static_assert(L == Layout::RowMajor, "factor_cholesky currently supports RowMajor only");

    Matrix<T, L>& packed = chol.packed();
    const crd::usize n = packed.rows();
    CRD_ASSERT_MSG(packed.cols() == n, "factor_cholesky: matrix must be square");
    chol.set_info(0);

    if (n == 0)
    {
        return;
    }

    T* data = packed.data();
    const crd::usize ld = packed.ld();

    crd::memory::IAllocator* alloc = (scratch != nullptr) ? scratch : chol.allocator();

    // Small/mid matrices: pure unblocked SIMD. Blocking overhead (per-panel
    // diagonal factor + syrk packing) isn't amortized until the trailing
    // update dominates (n > 256 on the i9-14900K). Measured: unblocked wins
    // ≤256, blocked-syrk wins ≥512.
    if (n <= 256)
    {
        const crd::usize info = unblocked_cholesky_full<T>(data, n, ld);
        chol.set_info(info);
        return;
    }

    // Right-looking blocked Cholesky. The bulk of FLOPs flows through the
    // PACKED register-tiled SYRK trailing update (high arithmetic intensity,
    // no horizontal sums) — the principled fix for the small/mid-N regime
    // where the per-column dot was latency-bound. Each step:
    //   1. factor the nb×nb diagonal block via the SIMD unblocked kernel,
    //   2. trsm the below-diagonal L21 panel (parallel for big trailing),
    //   3. A22 -= L21·L21ᵀ via `syrk_lower_minus` (register-tiled, lower
    //      triangle only = half a gemm's FLOPs; serial at small n, parallel
    //      at large n).
    for (crd::usize k = 0; k < n; k += kBlockSize)
    {
        const crd::usize nb = (k + kBlockSize <= n) ? kBlockSize : (n - k);

        // Step 1: factor diagonal block L11 via SIMD unblocked Cholesky on
        // the nb×nb sub-block at offset (k, k).
        const crd::usize blk_info = unblocked_cholesky_full<T>(data + k * ld + k, nb, ld);
        if (blk_info != 0)
        {
            chol.set_info(k + blk_info);
            return;
        }

        // Step 2: solve L21 · L11ᵀ = A21 for the below-diagonal block.
        inner_trsm_right<T, L>(data, n, ld, k, nb);

        // Step 3: syrk trailing update A22 -= L21 · L21ᵀ (lower triangle).
        if (k + nb < n)
        {
            const crd::usize tr_dim = n - k - nb;
            MatrixView<const T, L> l21{data + (k + nb) * ld + k, tr_dim, nb, ld};
            MatrixView<T, L> a22{data + (k + nb) * ld + (k + nb), tr_dim, tr_dim, ld};
            detail::syrk_lower_minus<T, L>(l21, a22, alloc);
        }
    }
}

template <typename T, Layout L>
void solve_cholesky(const Cholesky<T, L>& chol, crd::containers::Span<T> x)
{
    static_assert(L == Layout::RowMajor, "solve_cholesky currently supports RowMajor only");

    const Matrix<T, L>& packed = chol.packed();
    const crd::usize n = packed.rows();
    CRD_ASSERT_MSG(x.size() == n, "solve_cholesky: RHS size != n");
    CRD_ASSERT_MSG(!chol.is_singular(), "solve_cholesky: factor is singular (info != 0)");

    if (n == 0)
    {
        return;
    }

    const T* data = packed.data();
    const crd::usize ld = packed.ld();

    // Forward sub: L · y = b (explicit diagonal lower triangle).
    for (crd::usize i = 0; i < n; ++i)
    {
        T sum = x[i];
        for (crd::usize j = 0; j < i; ++j)
        {
            sum -= data[i * ld + j] * x[j];
        }
        x[i] = sum / data[i * ld + i];
    }

    // Back sub: L^T · x = y (transposed lower triangle = upper triangle
    // walk with the same diagonal).
    for (crd::usize i = n; i > 0; --i)
    {
        const crd::usize idx = i - 1;
        T sum = x[idx];
        for (crd::usize j = idx + 1; j < n; ++j)
        {
            sum -= data[j * ld + idx] * x[j];
        }
        x[idx] = sum / data[idx * ld + idx];
    }
}

template <typename T, Layout L>
void solve_cholesky(const Cholesky<T, L>& chol, MatrixView<T, L> b)
{
    static_assert(L == Layout::RowMajor, "solve_cholesky currently supports RowMajor only");
    const crd::usize n = chol.packed().rows();
    CRD_ASSERT_MSG(b.rows() == n, "solve_cholesky: RHS rows != n");
    const crd::usize nrhs = b.cols();
    const T* data = chol.packed().data();
    const crd::usize ld = chol.packed().ld();
    for (crd::usize r = 0; r < nrhs; ++r)
    {
        // Forward sub.
        for (crd::usize i = 0; i < n; ++i)
        {
            T sum = b.at(i, r);
            for (crd::usize j = 0; j < i; ++j)
            {
                sum -= data[i * ld + j] * b.at(j, r);
            }
            b.at(i, r) = sum / data[i * ld + i];
        }
        // Back sub.
        for (crd::usize i = n; i > 0; --i)
        {
            const crd::usize idx = i - 1;
            T sum = b.at(idx, r);
            for (crd::usize j = idx + 1; j < n; ++j)
            {
                sum -= data[j * ld + idx] * b.at(j, r);
            }
            b.at(idx, r) = sum / data[idx * ld + idx];
        }
    }
}

// Explicit instantiations — f32 / f64 RowMajor only.
template void factor_cholesky<float, Layout::RowMajor>(Cholesky<float, Layout::RowMajor>&,
                                                       crd::memory::IAllocator*);
template void factor_cholesky<double, Layout::RowMajor>(Cholesky<double, Layout::RowMajor>&,
                                                        crd::memory::IAllocator*);
template void solve_cholesky<float, Layout::RowMajor>(const Cholesky<float, Layout::RowMajor>&,
                                                      crd::containers::Span<float>);
template void solve_cholesky<double, Layout::RowMajor>(const Cholesky<double, Layout::RowMajor>&,
                                                       crd::containers::Span<double>);
template void solve_cholesky<float, Layout::RowMajor>(const Cholesky<float, Layout::RowMajor>&,
                                                      MatrixView<float, Layout::RowMajor>);
template void solve_cholesky<double, Layout::RowMajor>(const Cholesky<double, Layout::RowMajor>&,
                                                       MatrixView<double, Layout::RowMajor>);

} // namespace crd::hesap::dense
