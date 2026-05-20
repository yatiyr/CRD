#include <crd/hesap/dense/blas2.hpp>

#include <crd/containers/array.hpp>
#include <crd/core/assert.hpp>
#include <crd/hesap/dense/detail/dot_simd.hpp>
#include <crd/hesap/dense/detail/pairwise_sum.hpp>
#include <crd/memory/allocator.hpp>

#include <cstring>

namespace crd::hesap::dense
{

namespace
{
// Element of A at (i, j) for the layout-dispatched MatrixView. Avoids
// re-deriving the offset inside every BLAS L2 op below.
template <typename T, Layout L>
[[nodiscard]] inline T mat_at(const MatrixView<const T, L>& a, crd::usize i, crd::usize j) noexcept
{
    if constexpr (L == Layout::RowMajor)
    {
        return a.data()[i * a.ld() + j];
    }
    else
    {
        return a.data()[j * a.ld() + i];
    }
}

template <typename T, Layout L>
[[nodiscard]] inline T& mat_at_ref(MatrixView<T, L>& a, crd::usize i, crd::usize j) noexcept
{
    if constexpr (L == Layout::RowMajor)
    {
        return a.data()[i * a.ld() + j];
    }
    else
    {
        return a.data()[j * a.ld() + i];
    }
}

template <typename T>
[[nodiscard]] inline T maybe_conj(T v, bool do_conj) noexcept
{
    if constexpr (is_complex_v<T>)
    {
        return do_conj ? crd::hesap::conj(v) : v;
    }
    else
    {
        (void)do_conj;
        return v;
    }
}
} // namespace

// =======================================================================
// gemv: y = alpha * op(A) * x + beta * y
// =======================================================================
template <typename T, Layout L>
void gemv(T alpha, MatrixView<const T, L> a, crd::containers::ConstSpan<T> x, T beta,
          crd::containers::Span<T> y, Trans trans)
{
    const crd::usize m = a.rows();
    const crd::usize n = a.cols();
    if (trans == Trans::None)
    {
        CRD_ASSERT_MSG(x.size() == n && y.size() == m, "gemv(None): size mismatch");
        // 4-row tiled SIMD gemv: process 4 rows simultaneously to share the
        // x-load across 4 INDEPENDENT FMA chains. Each row gets its own
        // accumulator → 4-way ILP, perfect for two FMA ports + ~4-cycle
        // latency. Wins at large N where memory is the bottleneck (sharing
        // x reduces L1 pressure even when x fits).
        if constexpr (L == Layout::RowMajor && std::is_same_v<T, crd::f64>)
        {
            if (a.ld() == n)
            {
                namespace simd = crd::math::simd;
                // NOLINTBEGIN(readability-identifier-naming) — matrix-notation locals
                const T* A = a.data();
                const T* x_data = x.data();
                T* y_data = y.data();

                // 8-row tile: 8 accumulators share one x load per inner iter.
                // 2 FMA ports × 4-cyc latency = 8 in-flight needed for peak;
                // 8-way ILP saturates the FMA pipeline. Register usage:
                // 8 accumulators + 1 x_reg + 1 A_temp = 10 of 16 YMM.
                const bool use_prefetch = (n > 512);
                crd::usize i = 0;
                for (; i + 8 <= m; i += 8)
                {
                    const T* A0 = A + (i + 0) * n;
                    const T* A1 = A + (i + 1) * n;
                    const T* A2 = A + (i + 2) * n;
                    const T* A3 = A + (i + 3) * n;
                    const T* A4 = A + (i + 4) * n;
                    const T* A5 = A + (i + 5) * n;
                    const T* A6 = A + (i + 6) * n;
                    const T* A7 = A + (i + 7) * n;
                    // Prefetch first cache lines of NEXT row block (i+8..i+16).
                    // Eight rows × first cache line each = 8 64-byte prefetches.
#if CRD_SIMD_HAS_AVX2
                    if (use_prefetch && i + 16 <= m)
                    {
                        _mm_prefetch(reinterpret_cast<const char*>(A + (i + 8) * n), _MM_HINT_T1);
                        _mm_prefetch(reinterpret_cast<const char*>(A + (i + 9) * n), _MM_HINT_T1);
                        _mm_prefetch(reinterpret_cast<const char*>(A + (i + 10) * n), _MM_HINT_T1);
                        _mm_prefetch(reinterpret_cast<const char*>(A + (i + 11) * n), _MM_HINT_T1);
                        _mm_prefetch(reinterpret_cast<const char*>(A + (i + 12) * n), _MM_HINT_T1);
                        _mm_prefetch(reinterpret_cast<const char*>(A + (i + 13) * n), _MM_HINT_T1);
                        _mm_prefetch(reinterpret_cast<const char*>(A + (i + 14) * n), _MM_HINT_T1);
                        _mm_prefetch(reinterpret_cast<const char*>(A + (i + 15) * n), _MM_HINT_T1);
                    }
#endif
                    simd::Vec4d acc0 = simd::Vec4d::zero();
                    simd::Vec4d acc1 = simd::Vec4d::zero();
                    simd::Vec4d acc2 = simd::Vec4d::zero();
                    simd::Vec4d acc3 = simd::Vec4d::zero();
                    simd::Vec4d acc4 = simd::Vec4d::zero();
                    simd::Vec4d acc5 = simd::Vec4d::zero();
                    simd::Vec4d acc6 = simd::Vec4d::zero();
                    simd::Vec4d acc7 = simd::Vec4d::zero();
                    crd::usize k = 0;
                    for (; k + 4 <= n; k += 4)
                    {
                        const simd::Vec4d xk = simd::Vec4d::load(x_data + k);
                        acc0 = simd::fma(simd::Vec4d::load(A0 + k), xk, acc0);
                        acc1 = simd::fma(simd::Vec4d::load(A1 + k), xk, acc1);
                        acc2 = simd::fma(simd::Vec4d::load(A2 + k), xk, acc2);
                        acc3 = simd::fma(simd::Vec4d::load(A3 + k), xk, acc3);
                        acc4 = simd::fma(simd::Vec4d::load(A4 + k), xk, acc4);
                        acc5 = simd::fma(simd::Vec4d::load(A5 + k), xk, acc5);
                        acc6 = simd::fma(simd::Vec4d::load(A6 + k), xk, acc6);
                        acc7 = simd::fma(simd::Vec4d::load(A7 + k), xk, acc7);
                    }
                    crd::f64 s0 = simd::horizontal_sum(acc0);
                    crd::f64 s1 = simd::horizontal_sum(acc1);
                    crd::f64 s2 = simd::horizontal_sum(acc2);
                    crd::f64 s3 = simd::horizontal_sum(acc3);
                    crd::f64 s4 = simd::horizontal_sum(acc4);
                    crd::f64 s5 = simd::horizontal_sum(acc5);
                    crd::f64 s6 = simd::horizontal_sum(acc6);
                    crd::f64 s7 = simd::horizontal_sum(acc7);
                    for (; k < n; ++k)
                    {
                        const crd::f64 xk = x_data[k];
                        s0 += A0[k] * xk;
                        s1 += A1[k] * xk;
                        s2 += A2[k] * xk;
                        s3 += A3[k] * xk;
                        s4 += A4[k] * xk;
                        s5 += A5[k] * xk;
                        s6 += A6[k] * xk;
                        s7 += A7[k] * xk;
                    }
                    y_data[i + 0] = alpha * s0 + beta * y_data[i + 0];
                    y_data[i + 1] = alpha * s1 + beta * y_data[i + 1];
                    y_data[i + 2] = alpha * s2 + beta * y_data[i + 2];
                    y_data[i + 3] = alpha * s3 + beta * y_data[i + 3];
                    y_data[i + 4] = alpha * s4 + beta * y_data[i + 4];
                    y_data[i + 5] = alpha * s5 + beta * y_data[i + 5];
                    y_data[i + 6] = alpha * s6 + beta * y_data[i + 6];
                    y_data[i + 7] = alpha * s7 + beta * y_data[i + 7];
                }
                // Tail rows (m % 8 != 0): handle 4-row block then 1-row.
                for (; i + 4 <= m; i += 4)
                {
                    const T* A0 = A + (i + 0) * n;
                    const T* A1 = A + (i + 1) * n;
                    const T* A2 = A + (i + 2) * n;
                    const T* A3 = A + (i + 3) * n;
                    simd::Vec4d acc0 = simd::Vec4d::zero();
                    simd::Vec4d acc1 = simd::Vec4d::zero();
                    simd::Vec4d acc2 = simd::Vec4d::zero();
                    simd::Vec4d acc3 = simd::Vec4d::zero();
                    crd::usize k = 0;
                    for (; k + 4 <= n; k += 4)
                    {
                        const simd::Vec4d xk = simd::Vec4d::load(x_data + k);
                        acc0 = simd::fma(simd::Vec4d::load(A0 + k), xk, acc0);
                        acc1 = simd::fma(simd::Vec4d::load(A1 + k), xk, acc1);
                        acc2 = simd::fma(simd::Vec4d::load(A2 + k), xk, acc2);
                        acc3 = simd::fma(simd::Vec4d::load(A3 + k), xk, acc3);
                    }
                    crd::f64 s0 = simd::horizontal_sum(acc0);
                    crd::f64 s1 = simd::horizontal_sum(acc1);
                    crd::f64 s2 = simd::horizontal_sum(acc2);
                    crd::f64 s3 = simd::horizontal_sum(acc3);
                    for (; k < n; ++k)
                    {
                        const crd::f64 xk = x_data[k];
                        s0 += A0[k] * xk;
                        s1 += A1[k] * xk;
                        s2 += A2[k] * xk;
                        s3 += A3[k] * xk;
                    }
                    y_data[i + 0] = alpha * s0 + beta * y_data[i + 0];
                    y_data[i + 1] = alpha * s1 + beta * y_data[i + 1];
                    y_data[i + 2] = alpha * s2 + beta * y_data[i + 2];
                    y_data[i + 3] = alpha * s3 + beta * y_data[i + 3];
                }
                for (; i < m; ++i)
                {
                    const crd::f64 sum = detail::simd_dot_f64(A + i * n, x_data, n);
                    y_data[i] = alpha * sum + beta * y_data[i];
                }
                // NOLINTEND(readability-identifier-naming)
                return;
            }
        }
        else if constexpr (L == Layout::RowMajor && std::is_same_v<T, crd::f32>)
        {
            if (a.ld() == n)
            {
                namespace simd = crd::math::simd;
                // NOLINTBEGIN(readability-identifier-naming) — matrix-notation locals
                const T* A = a.data();
                const T* x_data = x.data();
                T* y_data = y.data();

                crd::usize i = 0;
                for (; i + 4 <= m; i += 4)
                {
                    simd::Vec8f acc0 = simd::Vec8f::zero();
                    simd::Vec8f acc1 = simd::Vec8f::zero();
                    simd::Vec8f acc2 = simd::Vec8f::zero();
                    simd::Vec8f acc3 = simd::Vec8f::zero();
                    crd::usize k = 0;
                    for (; k + 8 <= n; k += 8)
                    {
                        const simd::Vec8f xk = simd::Vec8f::load(x_data + k);
                        const simd::Vec8f a0 = simd::Vec8f::load(A + (i + 0) * n + k);
                        const simd::Vec8f a1 = simd::Vec8f::load(A + (i + 1) * n + k);
                        const simd::Vec8f a2 = simd::Vec8f::load(A + (i + 2) * n + k);
                        const simd::Vec8f a3 = simd::Vec8f::load(A + (i + 3) * n + k);
                        acc0 = simd::fma(a0, xk, acc0);
                        acc1 = simd::fma(a1, xk, acc1);
                        acc2 = simd::fma(a2, xk, acc2);
                        acc3 = simd::fma(a3, xk, acc3);
                    }
                    crd::f32 s0 = simd::horizontal_sum(acc0);
                    crd::f32 s1 = simd::horizontal_sum(acc1);
                    crd::f32 s2 = simd::horizontal_sum(acc2);
                    crd::f32 s3 = simd::horizontal_sum(acc3);
                    for (; k < n; ++k)
                    {
                        s0 += A[(i + 0) * n + k] * x_data[k];
                        s1 += A[(i + 1) * n + k] * x_data[k];
                        s2 += A[(i + 2) * n + k] * x_data[k];
                        s3 += A[(i + 3) * n + k] * x_data[k];
                    }
                    y_data[i + 0] = alpha * s0 + beta * y_data[i + 0];
                    y_data[i + 1] = alpha * s1 + beta * y_data[i + 1];
                    y_data[i + 2] = alpha * s2 + beta * y_data[i + 2];
                    y_data[i + 3] = alpha * s3 + beta * y_data[i + 3];
                }
                for (; i < m; ++i)
                {
                    const crd::f32 sum = detail::simd_dot_f32(A + i * n, x_data, n);
                    y_data[i] = alpha * sum + beta * y_data[i];
                }
                // NOLINTEND(readability-identifier-naming)
                return;
            }
        }
        for (crd::usize i = 0; i < m; ++i)
        {
            T sum = detail::pairwise_sum_produced<T>(
                n, [&](crd::usize k) { return mat_at<T, L>(a, i, k) * x[k]; });
            y[i] = alpha * sum + beta * y[i];
        }
    }
    else
    {
        const bool do_conj = (trans == Trans::ConjTranspose);
        CRD_ASSERT_MSG(x.size() == m && y.size() == n, "gemv(Trans): size mismatch");
        for (crd::usize j = 0; j < n; ++j)
        {
            T sum = detail::pairwise_sum_produced<T>(
                m, [&](crd::usize k) { return maybe_conj<T>(mat_at<T, L>(a, k, j), do_conj) * x[k]; });
            y[j] = alpha * sum + beta * y[j];
        }
    }
}

// =======================================================================
// ger: A += alpha * x * y^T (real)
// =======================================================================
template <typename T, Layout L>
void ger(T alpha, crd::containers::ConstSpan<T> x, crd::containers::ConstSpan<T> y, MatrixView<T, L> a)
{
    const crd::usize m = a.rows();
    const crd::usize n = a.cols();
    CRD_ASSERT_MSG(x.size() == m && y.size() == n, "ger: size mismatch");
    for (crd::usize i = 0; i < m; ++i)
    {
        const T axi = alpha * x[i];
        for (crd::usize j = 0; j < n; ++j)
        {
            mat_at_ref<T, L>(a, i, j) = mat_at_ref<T, L>(a, i, j) + axi * y[j];
        }
    }
}

// =======================================================================
// geru: A += alpha * x * y^T (complex, unconjugated)
// =======================================================================
template <typename T, Layout L>
void geru(Complex<T> alpha, crd::containers::ConstSpan<Complex<T>> x,
          crd::containers::ConstSpan<Complex<T>> y, MatrixView<Complex<T>, L> a)
{
    const crd::usize m = a.rows();
    const crd::usize n = a.cols();
    CRD_ASSERT_MSG(x.size() == m && y.size() == n, "geru: size mismatch");
    for (crd::usize i = 0; i < m; ++i)
    {
        const Complex<T> axi = alpha * x[i];
        for (crd::usize j = 0; j < n; ++j)
        {
            mat_at_ref<Complex<T>, L>(a, i, j) =
                mat_at_ref<Complex<T>, L>(a, i, j) + axi * y[j];
        }
    }
}

// =======================================================================
// gerc: A += alpha * x * conj(y)^T (complex, conjugated y)
// =======================================================================
template <typename T, Layout L>
void gerc(Complex<T> alpha, crd::containers::ConstSpan<Complex<T>> x,
          crd::containers::ConstSpan<Complex<T>> y, MatrixView<Complex<T>, L> a)
{
    const crd::usize m = a.rows();
    const crd::usize n = a.cols();
    CRD_ASSERT_MSG(x.size() == m && y.size() == n, "gerc: size mismatch");
    for (crd::usize i = 0; i < m; ++i)
    {
        const Complex<T> axi = alpha * x[i];
        for (crd::usize j = 0; j < n; ++j)
        {
            mat_at_ref<Complex<T>, L>(a, i, j) =
                mat_at_ref<Complex<T>, L>(a, i, j) + axi * crd::hesap::conj(y[j]);
        }
    }
}

// =======================================================================
// gbmv: y = alpha * op(A) * x + beta * y (banded)
// =======================================================================
template <typename T>
void gbmv(T alpha, const Banded<T>& a, crd::containers::ConstSpan<T> x, T beta,
          crd::containers::Span<T> y, Trans trans)
{
    const crd::usize m = a.rows();
    const crd::usize n = a.cols();
    const bool do_conj = (trans == Trans::ConjTranspose);
    if (trans == Trans::None)
    {
        CRD_ASSERT_MSG(x.size() == n && y.size() == m, "gbmv(None): size mismatch");
        for (crd::usize i = 0; i < m; ++i)
        {
            T sum{};
            for (crd::usize j = 0; j < n; ++j)
            {
                if (a.in_band(i, j))
                {
                    sum = sum + a.at_value(i, j) * x[j];
                }
            }
            y[i] = alpha * sum + beta * y[i];
        }
    }
    else
    {
        CRD_ASSERT_MSG(x.size() == m && y.size() == n, "gbmv(Trans): size mismatch");
        for (crd::usize j = 0; j < n; ++j)
        {
            T sum{};
            for (crd::usize i = 0; i < m; ++i)
            {
                if (a.in_band(i, j))
                {
                    sum = sum + maybe_conj<T>(a.at_value(i, j), do_conj) * x[i];
                }
            }
            y[j] = alpha * sum + beta * y[j];
        }
    }
}

// =======================================================================
// symv / hemv
// =======================================================================
template <typename T>
void symv(T alpha, const Symmetric<T>& a, crd::containers::ConstSpan<T> x, T beta,
          crd::containers::Span<T> y)
{
    const crd::usize n = a.n();
    CRD_ASSERT_MSG(x.size() == n && y.size() == n, "symv: size mismatch");
    // Classic single-pass BLAS symv (UPLO=Lower): each lower-half element
    // A[i,k] is touched ONCE and used to update BOTH y[i] (dot product into
    // row i's accumulator) AND y[k] (rank-1 update: y[k] += alpha*A[i,k]*x[i]).
    // This halves memory bandwidth vs naive gemv-on-full-matrix or
    // reconstruct-and-dot. With SIMD over the inner k-loop, peaks at memory
    // bandwidth (50 GB/s on the dev box → ~25 GFLOPS f64).
    if constexpr (std::is_same_v<T, crd::f32> || std::is_same_v<T, crd::f64>)
    {
        namespace simd = crd::math::simd;
        // Scale y by beta upfront (the rank-1 update inside the loop assumes
        // y already holds the post-beta value).
        for (crd::usize i = 0; i < n; ++i)
        {
            y[i] = beta * y[i];
        }

        // NOLINTBEGIN(readability-identifier-naming) — matrix-notation locals
        const T* A = a.data();
        const T* x_data = x.data();
        T* y_data = y.data();

        if constexpr (std::is_same_v<T, crd::f64>)
        {
            // Prefetch only when the matrix is large enough that next-row
            // memory hasn't been touched by the HW streaming prefetcher.
            // At small N the whole matrix lower-half fits in L1/L2 and the
            // prefetch instructions are pure overhead (measured 6% loss at N=64).
            const bool use_prefetch = (n > 512);
            for (crd::usize i = 0; i < n; ++i)
            {
                const T* A_row = A + i * n;
                const T* A_next_row = (i + 1 < n) ? (A + (i + 1) * n) : nullptr;
                const crd::f64 x_i = x_data[i];
                const simd::Vec4d vec_alpha_x_i(alpha * x_i);  // fused alpha*x[i] once

                // 4 independent dot accumulators for ILP. Rank-1 update is
                // a SEPARATE dependency chain (y store→load) so giving the
                // dot more independent chains decouples the two paths.
                simd::Vec4d dot_acc0 = simd::Vec4d::zero();
                simd::Vec4d dot_acc1 = simd::Vec4d::zero();
                simd::Vec4d dot_acc2 = simd::Vec4d::zero();
                simd::Vec4d dot_acc3 = simd::Vec4d::zero();

                crd::usize k = 0;
                // Prefetch first cache lines of NEXT row so the HW streaming
                // prefetcher has lead time across the row-transition.
#if CRD_SIMD_HAS_AVX2
                if (use_prefetch && A_next_row != nullptr)
                {
                    _mm_prefetch(reinterpret_cast<const char*>(A_next_row), _MM_HINT_T1);
                    _mm_prefetch(reinterpret_cast<const char*>(A_next_row + 8), _MM_HINT_T1);
                }
#endif
                // 16-wide unroll: 4 a-loads + 4 x-loads + 4 y-loads + 4 dot
                // fmas + 4 rank-1 fmas + 4 stores per iter. Saturates 2 load
                // + 1 store + 2 fma ports.
                for (; k + 16 <= i; k += 16)
                {
#if CRD_SIMD_HAS_AVX2
                    // Prefetch next row 64-byte chunks just ahead of the
                    // current row's processing position.
                    if (use_prefetch && A_next_row != nullptr && (k & 31) == 0)
                    {
                        _mm_prefetch(reinterpret_cast<const char*>(A_next_row + k), _MM_HINT_T1);
                    }
#endif
                    const simd::Vec4d a0 = simd::Vec4d::load(A_row + k + 0);
                    const simd::Vec4d a1 = simd::Vec4d::load(A_row + k + 4);
                    const simd::Vec4d a2 = simd::Vec4d::load(A_row + k + 8);
                    const simd::Vec4d a3 = simd::Vec4d::load(A_row + k + 12);
                    const simd::Vec4d x0 = simd::Vec4d::load(x_data + k + 0);
                    const simd::Vec4d x1 = simd::Vec4d::load(x_data + k + 4);
                    const simd::Vec4d x2 = simd::Vec4d::load(x_data + k + 8);
                    const simd::Vec4d x3 = simd::Vec4d::load(x_data + k + 12);
                    // Dot path (4 independent chains).
                    dot_acc0 = simd::fma(a0, x0, dot_acc0);
                    dot_acc1 = simd::fma(a1, x1, dot_acc1);
                    dot_acc2 = simd::fma(a2, x2, dot_acc2);
                    dot_acc3 = simd::fma(a3, x3, dot_acc3);
                    // Rank-1 path: y[k] += alpha * x[i] * A[i,k].
                    simd::Vec4d y0 = simd::Vec4d::load(y_data + k + 0);
                    simd::Vec4d y1 = simd::Vec4d::load(y_data + k + 4);
                    simd::Vec4d y2 = simd::Vec4d::load(y_data + k + 8);
                    simd::Vec4d y3 = simd::Vec4d::load(y_data + k + 12);
                    y0 = simd::fma(a0, vec_alpha_x_i, y0);
                    y1 = simd::fma(a1, vec_alpha_x_i, y1);
                    y2 = simd::fma(a2, vec_alpha_x_i, y2);
                    y3 = simd::fma(a3, vec_alpha_x_i, y3);
                    y0.store(y_data + k + 0);
                    y1.store(y_data + k + 4);
                    y2.store(y_data + k + 8);
                    y3.store(y_data + k + 12);
                }
                for (; k + 4 <= i; k += 4)
                {
                    const simd::Vec4d a0 = simd::Vec4d::load(A_row + k);
                    const simd::Vec4d x0 = simd::Vec4d::load(x_data + k);
                    simd::Vec4d y0 = simd::Vec4d::load(y_data + k);
                    dot_acc0 = simd::fma(a0, x0, dot_acc0);
                    y0 = simd::fma(a0, vec_alpha_x_i, y0);
                    y0.store(y_data + k);
                }
                // Tail: k in [k, i) — scalar.
                crd::f64 dot_tail = 0.0;
                for (; k < i; ++k)
                {
                    const crd::f64 a_ik = A_row[k];
                    dot_tail += a_ik * x_data[k];
                    y_data[k] += alpha * a_ik * x_i;
                }
                // Diagonal (k == i): contributes only to y[i].
                const crd::f64 a_ii = A_row[i];
                const simd::Vec4d combined =
                    (dot_acc0 + dot_acc1) + (dot_acc2 + dot_acc3);
                const crd::f64 dot_total =
                    simd::horizontal_sum(combined) + dot_tail + a_ii * x_i;
                y_data[i] += alpha * dot_total;
            }
        }
        else  // f32
        {
            for (crd::usize i = 0; i < n; ++i)
            {
                const T* A_row = A + i * n;
                const crd::f32 x_i = x_data[i];
                const simd::Vec8f vec_x_i(x_i);
                const simd::Vec8f vec_alpha(alpha);

                simd::Vec8f dot_acc0 = simd::Vec8f::zero();
                simd::Vec8f dot_acc1 = simd::Vec8f::zero();

                crd::usize k = 0;
                for (; k + 16 <= i; k += 16)
                {
                    const simd::Vec8f a0 = simd::Vec8f::load(A_row + k + 0);
                    const simd::Vec8f a1 = simd::Vec8f::load(A_row + k + 8);
                    const simd::Vec8f x0 = simd::Vec8f::load(x_data + k + 0);
                    const simd::Vec8f x1 = simd::Vec8f::load(x_data + k + 8);
                    simd::Vec8f y0 = simd::Vec8f::load(y_data + k + 0);
                    simd::Vec8f y1 = simd::Vec8f::load(y_data + k + 8);
                    dot_acc0 = simd::fma(a0, x0, dot_acc0);
                    dot_acc1 = simd::fma(a1, x1, dot_acc1);
                    const simd::Vec8f alpha_a0 = vec_alpha * a0;
                    const simd::Vec8f alpha_a1 = vec_alpha * a1;
                    y0 = simd::fma(alpha_a0, vec_x_i, y0);
                    y1 = simd::fma(alpha_a1, vec_x_i, y1);
                    y0.store(y_data + k + 0);
                    y1.store(y_data + k + 8);
                }
                for (; k + 8 <= i; k += 8)
                {
                    const simd::Vec8f a0 = simd::Vec8f::load(A_row + k);
                    const simd::Vec8f x0 = simd::Vec8f::load(x_data + k);
                    simd::Vec8f y0 = simd::Vec8f::load(y_data + k);
                    dot_acc0 = simd::fma(a0, x0, dot_acc0);
                    const simd::Vec8f alpha_a0 = vec_alpha * a0;
                    y0 = simd::fma(alpha_a0, vec_x_i, y0);
                    y0.store(y_data + k);
                }
                crd::f32 dot_tail = 0.0F;
                for (; k < i; ++k)
                {
                    const crd::f32 a_ik = A_row[k];
                    dot_tail += a_ik * x_data[k];
                    y_data[k] += alpha * a_ik * x_i;
                }
                const crd::f32 a_ii = A_row[i];
                const crd::f32 dot_total =
                    simd::horizontal_sum(dot_acc0 + dot_acc1) + dot_tail + a_ii * x_i;
                y_data[i] += alpha * dot_total;
            }
        }
        // NOLINTEND(readability-identifier-naming)
    }
    else
    {
        for (crd::usize i = 0; i < n; ++i)
        {
            T sum = detail::pairwise_sum_produced<T>(n,
                                                     [&](crd::usize k) { return a.at(i, k) * x[k]; });
            y[i] = alpha * sum + beta * y[i];
        }
    }
}

template <typename T>
void hemv(Complex<T> alpha, const Hermitian<Complex<T>>& a, crd::containers::ConstSpan<Complex<T>> x,
          Complex<T> beta, crd::containers::Span<Complex<T>> y)
{
    const crd::usize n = a.n();
    CRD_ASSERT_MSG(x.size() == n && y.size() == n, "hemv: size mismatch");
    for (crd::usize i = 0; i < n; ++i)
    {
        Complex<T> sum = detail::pairwise_sum_produced<Complex<T>>(
            n, [&](crd::usize k) { return a.at_value(i, k) * x[k]; });
        y[i] = alpha * sum + beta * y[i];
    }
}

// =======================================================================
// syr / her  rank-1 updates
// =======================================================================
template <typename T>
void syr(T alpha, crd::containers::ConstSpan<T> x, Symmetric<T>& a)
{
    const crd::usize n = a.n();
    CRD_ASSERT_MSG(x.size() == n, "syr: size mismatch");
    // Update only the canonical (lower) triangle; symmetric access mirrors.
    for (crd::usize i = 0; i < n; ++i)
    {
        const T axi = alpha * x[i];
        for (crd::usize j = 0; j <= i; ++j)
        {
            a.at(i, j) = a.at(i, j) + axi * x[j];
        }
    }
}

template <typename T>
void her(T alpha, crd::containers::ConstSpan<Complex<T>> x, Hermitian<Complex<T>>& a)
{
    const crd::usize n = a.n();
    CRD_ASSERT_MSG(x.size() == n, "her: size mismatch");
    // A += alpha * x * x^H. Update only lower triangle of A.
    // Diagonal: A_ii += alpha * |x_i|^2 (real-valued).
    for (crd::usize i = 0; i < n; ++i)
    {
        for (crd::usize j = 0; j <= i; ++j)
        {
            // Real-alpha rank-1 update: alpha * x_i * conj(x_j).
            const Complex<T> update = Complex<T>{alpha, T{}} * x[i] * crd::hesap::conj(x[j]);
            a.at_lower(i, j) = a.at_lower(i, j) + update;
        }
    }
}

// =======================================================================
// syr2 / her2  rank-2 updates
// =======================================================================
template <typename T>
void syr2(T alpha, crd::containers::ConstSpan<T> x, crd::containers::ConstSpan<T> y, Symmetric<T>& a)
{
    const crd::usize n = a.n();
    CRD_ASSERT_MSG(x.size() == n && y.size() == n, "syr2: size mismatch");
    for (crd::usize i = 0; i < n; ++i)
    {
        const T axi = alpha * x[i];
        const T ayi = alpha * y[i];
        for (crd::usize j = 0; j <= i; ++j)
        {
            a.at(i, j) = a.at(i, j) + axi * y[j] + ayi * x[j];
        }
    }
}

template <typename T>
void her2(Complex<T> alpha, crd::containers::ConstSpan<Complex<T>> x,
          crd::containers::ConstSpan<Complex<T>> y, Hermitian<Complex<T>>& a)
{
    const crd::usize n = a.n();
    CRD_ASSERT_MSG(x.size() == n && y.size() == n, "her2: size mismatch");
    // A += alpha * x * y^H + conj(alpha) * y * x^H
    const Complex<T> alpha_conj = crd::hesap::conj(alpha);
    for (crd::usize i = 0; i < n; ++i)
    {
        const Complex<T> axi = alpha * x[i];
        const Complex<T> acyi = alpha_conj * y[i];
        for (crd::usize j = 0; j <= i; ++j)
        {
            const Complex<T> update = axi * crd::hesap::conj(y[j]) + acyi * crd::hesap::conj(x[j]);
            a.at_lower(i, j) = a.at_lower(i, j) + update;
        }
    }
}

// =======================================================================
// sbmv / hbmv  banded symmetric / Hermitian
// =======================================================================
template <typename T>
void sbmv(T alpha, const Banded<T>& a, crd::containers::ConstSpan<T> x, T beta,
          crd::containers::Span<T> y)
{
    const crd::usize n = a.cols();
    CRD_ASSERT_MSG(a.rows() == n, "sbmv: A must be square");
    CRD_ASSERT_MSG(x.size() == n && y.size() == n, "sbmv: size mismatch");
    for (crd::usize i = 0; i < n; ++i)
    {
        T sum{};
        for (crd::usize j = 0; j < n; ++j)
        {
            // Symmetric: use stored band entry, mirror for j > i if stored as lower band.
            // Banded<T> stores entries in-band only; sym implies A_ij = A_ji.
            if (a.in_band(i, j))
            {
                sum = sum + a.at_value(i, j) * x[j];
            }
            else if (a.in_band(j, i))
            {
                sum = sum + a.at_value(j, i) * x[j];
            }
        }
        y[i] = alpha * sum + beta * y[i];
    }
}

template <typename T>
void hbmv(Complex<T> alpha, const Banded<Complex<T>>& a, crd::containers::ConstSpan<Complex<T>> x,
          Complex<T> beta, crd::containers::Span<Complex<T>> y)
{
    const crd::usize n = a.cols();
    CRD_ASSERT_MSG(a.rows() == n, "hbmv: A must be square");
    CRD_ASSERT_MSG(x.size() == n && y.size() == n, "hbmv: size mismatch");
    for (crd::usize i = 0; i < n; ++i)
    {
        Complex<T> sum{};
        for (crd::usize j = 0; j < n; ++j)
        {
            if (a.in_band(i, j))
            {
                sum = sum + a.at_value(i, j) * x[j];
            }
            else if (a.in_band(j, i))
            {
                // Hermitian: A_ij = conj(A_ji).
                sum = sum + crd::hesap::conj(a.at_value(j, i)) * x[j];
            }
        }
        y[i] = alpha * sum + beta * y[i];
    }
}

// =======================================================================
// trmv / trsv  triangular
// =======================================================================
template <typename T, TriangularSide Side, TriangularDiag Diag>
void trmv(const Triangular<T, Side, Diag>& a, crd::containers::Span<T> x, Trans trans)
{
    const crd::usize n = a.n();
    CRD_ASSERT_MSG(x.size() == n, "trmv: size mismatch");
    const bool do_conj = (trans == Trans::ConjTranspose);

    if (trans == Trans::None)
    {
        // x = A * x (in-place; must traverse such that we don't clobber unread values).
        if constexpr (Side == TriangularSide::Lower)
        {
            // Lower: y_i = sum_{j<=i} A_ij * x_j. Traverse i from N-1 down to 0
            // so x_0 ... x_{i-1} aren't yet updated when computing y_i.
            for (crd::usize ii = n; ii-- > 0;)
            {
                T sum{};
                for (crd::usize j = 0; j <= ii; ++j)
                {
                    sum = sum + a.at_value(ii, j) * x[j];
                }
                x[ii] = sum;
            }
        }
        else  // Upper
        {
            // Upper: y_i = sum_{j>=i} A_ij * x_j. Traverse i ascending.
            for (crd::usize i = 0; i < n; ++i)
            {
                T sum{};
                for (crd::usize j = i; j < n; ++j)
                {
                    sum = sum + a.at_value(i, j) * x[j];
                }
                x[i] = sum;
            }
        }
    }
    else
    {
        // x = A^T * x or A^H * x. For Lower-T, equivalent to Upper-N traversal.
        if constexpr (Side == TriangularSide::Lower)
        {
            for (crd::usize i = 0; i < n; ++i)
            {
                T sum{};
                for (crd::usize j = i; j < n; ++j)
                {
                    sum = sum + maybe_conj<T>(a.at_value(j, i), do_conj) * x[j];
                }
                x[i] = sum;
            }
        }
        else  // Upper
        {
            for (crd::usize ii = n; ii-- > 0;)
            {
                T sum{};
                for (crd::usize j = 0; j <= ii; ++j)
                {
                    sum = sum + maybe_conj<T>(a.at_value(j, ii), do_conj) * x[j];
                }
                x[ii] = sum;
            }
        }
    }
}

template <typename T, TriangularSide Side, TriangularDiag Diag>
void trsv(const Triangular<T, Side, Diag>& a, crd::containers::Span<T> x, Trans trans)
{
    const crd::usize n = a.n();
    CRD_ASSERT_MSG(x.size() == n, "trsv: size mismatch");
    const bool do_conj = (trans == Trans::ConjTranspose);

    if (trans == Trans::None)
    {
        if constexpr (Side == TriangularSide::Lower)
        {
            // Forward substitution: L * x = b.  x_i = (b_i - sum_{j<i} L_ij*x_j) / L_ii
            // SIMD fast path for f32/f64: each inner sum is a row · prefix-x dot.
            if constexpr (std::is_same_v<T, crd::f32> || std::is_same_v<T, crd::f64>)
            {
                const T* lower = a.data();
                T* x_data = x.data();
                for (crd::usize i = 0; i < n; ++i)
                {
                    T sum_off_diag;
                    if constexpr (std::is_same_v<T, crd::f32>)
                    {
                        sum_off_diag = detail::simd_dot_f32(lower + i * n, x_data, i);
                    }
                    else
                    {
                        sum_off_diag = detail::simd_dot_f64(lower + i * n, x_data, i);
                    }
                    const T s = x_data[i] - sum_off_diag;
                    if constexpr (Diag == TriangularDiag::UnitDiag)
                    {
                        x_data[i] = s;
                    }
                    else
                    {
                        x_data[i] = s / lower[i * n + i];
                    }
                }
            }
            else
            {
                for (crd::usize i = 0; i < n; ++i)
                {
                    T s = x[i];
                    for (crd::usize j = 0; j < i; ++j)
                    {
                        s = s - a.at_value(i, j) * x[j];
                    }
                    if constexpr (Diag == TriangularDiag::UnitDiag)
                    {
                        x[i] = s;
                    }
                    else
                    {
                        x[i] = s / a.at_value(i, i);
                    }
                }
            }
        }
        else
        {
            // Back substitution: U * x = b.  x_i = (b_i - sum_{j>i} U_ij*x_j) / U_ii
            // SIMD fast path: each inner sum is a row · suffix-x dot.
            if constexpr (std::is_same_v<T, crd::f32> || std::is_same_v<T, crd::f64>)
            {
                const T* upper = a.data();
                T* x_data = x.data();
                for (crd::usize ii = n; ii-- > 0;)
                {
                    const crd::usize tail = n - (ii + 1);
                    T sum_off_diag;
                    if (tail > 0)
                    {
                        if constexpr (std::is_same_v<T, crd::f32>)
                        {
                            sum_off_diag = detail::simd_dot_f32(upper + ii * n + ii + 1,
                                                                x_data + ii + 1, tail);
                        }
                        else
                        {
                            sum_off_diag = detail::simd_dot_f64(upper + ii * n + ii + 1,
                                                                x_data + ii + 1, tail);
                        }
                    }
                    else
                    {
                        sum_off_diag = T{};
                    }
                    const T s = x_data[ii] - sum_off_diag;
                    if constexpr (Diag == TriangularDiag::UnitDiag)
                    {
                        x_data[ii] = s;
                    }
                    else
                    {
                        x_data[ii] = s / upper[ii * n + ii];
                    }
                }
            }
            else
            {
                for (crd::usize ii = n; ii-- > 0;)
                {
                    T s = x[ii];
                    for (crd::usize j = ii + 1; j < n; ++j)
                    {
                        s = s - a.at_value(ii, j) * x[j];
                    }
                    if constexpr (Diag == TriangularDiag::UnitDiag)
                    {
                        x[ii] = s;
                    }
                    else
                    {
                        x[ii] = s / a.at_value(ii, ii);
                    }
                }
            }
        }
    }
    else
    {
        // op(A) = A^T or A^H. For Lower-T, equivalent to Upper-N solve.
        if constexpr (Side == TriangularSide::Lower)
        {
            for (crd::usize ii = n; ii-- > 0;)
            {
                T s = x[ii];
                for (crd::usize j = ii + 1; j < n; ++j)
                {
                    s = s - maybe_conj<T>(a.at_value(j, ii), do_conj) * x[j];
                }
                if constexpr (Diag == TriangularDiag::UnitDiag)
                {
                    x[ii] = s;
                }
                else
                {
                    x[ii] = s / maybe_conj<T>(a.at_value(ii, ii), do_conj);
                }
            }
        }
        else
        {
            for (crd::usize i = 0; i < n; ++i)
            {
                T s = x[i];
                for (crd::usize j = 0; j < i; ++j)
                {
                    s = s - maybe_conj<T>(a.at_value(j, i), do_conj) * x[j];
                }
                if constexpr (Diag == TriangularDiag::UnitDiag)
                {
                    x[i] = s;
                }
                else
                {
                    x[i] = s / maybe_conj<T>(a.at_value(i, i), do_conj);
                }
            }
        }
    }
}

// =======================================================================
// tbmv / tbsv  triangular banded
//
// Banded triangular: the band represents only the canonical (Side) half.
// For Lower: kl = number of sub-diagonals, ku = 0.
// For Upper: kl = 0, ku = number of super-diagonals.
// =======================================================================
template <typename T>
void tbmv(const Banded<T>& a, TriangularSide side, TriangularDiag diag, crd::containers::Span<T> x,
          Trans trans)
{
    const crd::usize n = a.cols();
    CRD_ASSERT_MSG(a.rows() == n, "tbmv: A must be square");
    CRD_ASSERT_MSG(x.size() == n, "tbmv: size mismatch");
    const bool do_conj = (trans == Trans::ConjTranspose);

    auto a_diag_aware = [&](crd::usize i, crd::usize j) -> T
    {
        if (diag == TriangularDiag::UnitDiag && i == j)
        {
            return T{1};
        }
        if (!a.in_band(i, j))
        {
            return T{};
        }
        return a.at_value(i, j);
    };

    if (trans == Trans::None)
    {
        if (side == TriangularSide::Lower)
        {
            for (crd::usize ii = n; ii-- > 0;)
            {
                T sum{};
                for (crd::usize j = 0; j <= ii; ++j)
                {
                    sum = sum + a_diag_aware(ii, j) * x[j];
                }
                x[ii] = sum;
            }
        }
        else
        {
            for (crd::usize i = 0; i < n; ++i)
            {
                T sum{};
                for (crd::usize j = i; j < n; ++j)
                {
                    sum = sum + a_diag_aware(i, j) * x[j];
                }
                x[i] = sum;
            }
        }
    }
    else
    {
        if (side == TriangularSide::Lower)
        {
            for (crd::usize i = 0; i < n; ++i)
            {
                T sum{};
                for (crd::usize j = i; j < n; ++j)
                {
                    sum = sum + maybe_conj<T>(a_diag_aware(j, i), do_conj) * x[j];
                }
                x[i] = sum;
            }
        }
        else
        {
            for (crd::usize ii = n; ii-- > 0;)
            {
                T sum{};
                for (crd::usize j = 0; j <= ii; ++j)
                {
                    sum = sum + maybe_conj<T>(a_diag_aware(j, ii), do_conj) * x[j];
                }
                x[ii] = sum;
            }
        }
    }
}

template <typename T>
void tbsv(const Banded<T>& a, TriangularSide side, TriangularDiag diag, crd::containers::Span<T> x,
          Trans trans)
{
    const crd::usize n = a.cols();
    CRD_ASSERT_MSG(a.rows() == n, "tbsv: A must be square");
    CRD_ASSERT_MSG(x.size() == n, "tbsv: size mismatch");
    const bool do_conj = (trans == Trans::ConjTranspose);

    auto a_diag_aware = [&](crd::usize i, crd::usize j) -> T
    {
        if (diag == TriangularDiag::UnitDiag && i == j)
        {
            return T{1};
        }
        if (!a.in_band(i, j))
        {
            return T{};
        }
        return a.at_value(i, j);
    };

    if (trans == Trans::None)
    {
        if (side == TriangularSide::Lower)
        {
            for (crd::usize i = 0; i < n; ++i)
            {
                T s = x[i];
                for (crd::usize j = 0; j < i; ++j)
                {
                    s = s - a_diag_aware(i, j) * x[j];
                }
                if (diag == TriangularDiag::UnitDiag)
                {
                    x[i] = s;
                }
                else
                {
                    x[i] = s / a_diag_aware(i, i);
                }
            }
        }
        else
        {
            for (crd::usize ii = n; ii-- > 0;)
            {
                T s = x[ii];
                for (crd::usize j = ii + 1; j < n; ++j)
                {
                    s = s - a_diag_aware(ii, j) * x[j];
                }
                if (diag == TriangularDiag::UnitDiag)
                {
                    x[ii] = s;
                }
                else
                {
                    x[ii] = s / a_diag_aware(ii, ii);
                }
            }
        }
    }
    else
    {
        if (side == TriangularSide::Lower)
        {
            for (crd::usize ii = n; ii-- > 0;)
            {
                T s = x[ii];
                for (crd::usize j = ii + 1; j < n; ++j)
                {
                    s = s - maybe_conj<T>(a_diag_aware(j, ii), do_conj) * x[j];
                }
                if (diag == TriangularDiag::UnitDiag)
                {
                    x[ii] = s;
                }
                else
                {
                    x[ii] = s / maybe_conj<T>(a_diag_aware(ii, ii), do_conj);
                }
            }
        }
        else
        {
            for (crd::usize i = 0; i < n; ++i)
            {
                T s = x[i];
                for (crd::usize j = 0; j < i; ++j)
                {
                    s = s - maybe_conj<T>(a_diag_aware(j, i), do_conj) * x[j];
                }
                if (diag == TriangularDiag::UnitDiag)
                {
                    x[i] = s;
                }
                else
                {
                    x[i] = s / maybe_conj<T>(a_diag_aware(i, i), do_conj);
                }
            }
        }
    }
}

// =======================================================================
// Explicit instantiations
// =======================================================================

// gemv: 4 types × 2 layouts
template void gemv<crd::f32, Layout::RowMajor>(crd::f32, MatrixView<const crd::f32, Layout::RowMajor>,
    crd::containers::ConstSpan<crd::f32>, crd::f32, crd::containers::Span<crd::f32>, Trans);
template void gemv<crd::f64, Layout::RowMajor>(crd::f64, MatrixView<const crd::f64, Layout::RowMajor>,
    crd::containers::ConstSpan<crd::f64>, crd::f64, crd::containers::Span<crd::f64>, Trans);
template void gemv<Complex32, Layout::RowMajor>(Complex32, MatrixView<const Complex32, Layout::RowMajor>,
    crd::containers::ConstSpan<Complex32>, Complex32, crd::containers::Span<Complex32>, Trans);
template void gemv<Complex64, Layout::RowMajor>(Complex64, MatrixView<const Complex64, Layout::RowMajor>,
    crd::containers::ConstSpan<Complex64>, Complex64, crd::containers::Span<Complex64>, Trans);
template void gemv<crd::f32, Layout::ColMajor>(crd::f32, MatrixView<const crd::f32, Layout::ColMajor>,
    crd::containers::ConstSpan<crd::f32>, crd::f32, crd::containers::Span<crd::f32>, Trans);
template void gemv<crd::f64, Layout::ColMajor>(crd::f64, MatrixView<const crd::f64, Layout::ColMajor>,
    crd::containers::ConstSpan<crd::f64>, crd::f64, crd::containers::Span<crd::f64>, Trans);
template void gemv<Complex32, Layout::ColMajor>(Complex32, MatrixView<const Complex32, Layout::ColMajor>,
    crd::containers::ConstSpan<Complex32>, Complex32, crd::containers::Span<Complex32>, Trans);
template void gemv<Complex64, Layout::ColMajor>(Complex64, MatrixView<const Complex64, Layout::ColMajor>,
    crd::containers::ConstSpan<Complex64>, Complex64, crd::containers::Span<Complex64>, Trans);

// ger (real): 2 types × 2 layouts (4 total)
template void ger<crd::f32, Layout::RowMajor>(crd::f32, crd::containers::ConstSpan<crd::f32>,
    crd::containers::ConstSpan<crd::f32>, MatrixView<crd::f32, Layout::RowMajor>);
template void ger<crd::f64, Layout::RowMajor>(crd::f64, crd::containers::ConstSpan<crd::f64>,
    crd::containers::ConstSpan<crd::f64>, MatrixView<crd::f64, Layout::RowMajor>);
template void ger<crd::f32, Layout::ColMajor>(crd::f32, crd::containers::ConstSpan<crd::f32>,
    crd::containers::ConstSpan<crd::f32>, MatrixView<crd::f32, Layout::ColMajor>);
template void ger<crd::f64, Layout::ColMajor>(crd::f64, crd::containers::ConstSpan<crd::f64>,
    crd::containers::ConstSpan<crd::f64>, MatrixView<crd::f64, Layout::ColMajor>);

// geru / gerc (complex): 2 types × 2 layouts each (8 total)
template void geru<crd::f32, Layout::RowMajor>(Complex32, crd::containers::ConstSpan<Complex32>,
    crd::containers::ConstSpan<Complex32>, MatrixView<Complex32, Layout::RowMajor>);
template void geru<crd::f64, Layout::RowMajor>(Complex64, crd::containers::ConstSpan<Complex64>,
    crd::containers::ConstSpan<Complex64>, MatrixView<Complex64, Layout::RowMajor>);
template void geru<crd::f32, Layout::ColMajor>(Complex32, crd::containers::ConstSpan<Complex32>,
    crd::containers::ConstSpan<Complex32>, MatrixView<Complex32, Layout::ColMajor>);
template void geru<crd::f64, Layout::ColMajor>(Complex64, crd::containers::ConstSpan<Complex64>,
    crd::containers::ConstSpan<Complex64>, MatrixView<Complex64, Layout::ColMajor>);
template void gerc<crd::f32, Layout::RowMajor>(Complex32, crd::containers::ConstSpan<Complex32>,
    crd::containers::ConstSpan<Complex32>, MatrixView<Complex32, Layout::RowMajor>);
template void gerc<crd::f64, Layout::RowMajor>(Complex64, crd::containers::ConstSpan<Complex64>,
    crd::containers::ConstSpan<Complex64>, MatrixView<Complex64, Layout::RowMajor>);
template void gerc<crd::f32, Layout::ColMajor>(Complex32, crd::containers::ConstSpan<Complex32>,
    crd::containers::ConstSpan<Complex32>, MatrixView<Complex32, Layout::ColMajor>);
template void gerc<crd::f64, Layout::ColMajor>(Complex64, crd::containers::ConstSpan<Complex64>,
    crd::containers::ConstSpan<Complex64>, MatrixView<Complex64, Layout::ColMajor>);

// gbmv: 4 types
template void gbmv<crd::f32>(crd::f32, const Banded<crd::f32>&, crd::containers::ConstSpan<crd::f32>,
    crd::f32, crd::containers::Span<crd::f32>, Trans);
template void gbmv<crd::f64>(crd::f64, const Banded<crd::f64>&, crd::containers::ConstSpan<crd::f64>,
    crd::f64, crd::containers::Span<crd::f64>, Trans);
template void gbmv<Complex32>(Complex32, const Banded<Complex32>&, crd::containers::ConstSpan<Complex32>,
    Complex32, crd::containers::Span<Complex32>, Trans);
template void gbmv<Complex64>(Complex64, const Banded<Complex64>&, crd::containers::ConstSpan<Complex64>,
    Complex64, crd::containers::Span<Complex64>, Trans);

// symv: real-only
template void symv<crd::f32>(crd::f32, const Symmetric<crd::f32>&, crd::containers::ConstSpan<crd::f32>,
    crd::f32, crd::containers::Span<crd::f32>);
template void symv<crd::f64>(crd::f64, const Symmetric<crd::f64>&, crd::containers::ConstSpan<crd::f64>,
    crd::f64, crd::containers::Span<crd::f64>);

// hemv: complex-only
template void hemv<crd::f32>(Complex32, const Hermitian<Complex32>&,
    crd::containers::ConstSpan<Complex32>, Complex32, crd::containers::Span<Complex32>);
template void hemv<crd::f64>(Complex64, const Hermitian<Complex64>&,
    crd::containers::ConstSpan<Complex64>, Complex64, crd::containers::Span<Complex64>);

// syr / syr2: real-only
template void syr<crd::f32>(crd::f32, crd::containers::ConstSpan<crd::f32>, Symmetric<crd::f32>&);
template void syr<crd::f64>(crd::f64, crd::containers::ConstSpan<crd::f64>, Symmetric<crd::f64>&);
template void syr2<crd::f32>(crd::f32, crd::containers::ConstSpan<crd::f32>,
    crd::containers::ConstSpan<crd::f32>, Symmetric<crd::f32>&);
template void syr2<crd::f64>(crd::f64, crd::containers::ConstSpan<crd::f64>,
    crd::containers::ConstSpan<crd::f64>, Symmetric<crd::f64>&);

// her / her2: complex-only (T = f32/f64 = underlying real)
template void her<crd::f32>(crd::f32, crd::containers::ConstSpan<Complex32>, Hermitian<Complex32>&);
template void her<crd::f64>(crd::f64, crd::containers::ConstSpan<Complex64>, Hermitian<Complex64>&);
template void her2<crd::f32>(Complex32, crd::containers::ConstSpan<Complex32>,
    crd::containers::ConstSpan<Complex32>, Hermitian<Complex32>&);
template void her2<crd::f64>(Complex64, crd::containers::ConstSpan<Complex64>,
    crd::containers::ConstSpan<Complex64>, Hermitian<Complex64>&);

// sbmv: real-only banded
template void sbmv<crd::f32>(crd::f32, const Banded<crd::f32>&, crd::containers::ConstSpan<crd::f32>,
    crd::f32, crd::containers::Span<crd::f32>);
template void sbmv<crd::f64>(crd::f64, const Banded<crd::f64>&, crd::containers::ConstSpan<crd::f64>,
    crd::f64, crd::containers::Span<crd::f64>);

// hbmv: complex-only banded
template void hbmv<crd::f32>(Complex32, const Banded<Complex32>&,
    crd::containers::ConstSpan<Complex32>, Complex32, crd::containers::Span<Complex32>);
template void hbmv<crd::f64>(Complex64, const Banded<Complex64>&,
    crd::containers::ConstSpan<Complex64>, Complex64, crd::containers::Span<Complex64>);

// trmv / trsv: 4 types × 2 sides × 2 diags = 16 each, but unit-diag-upper
// is rare; for v0c we ship the 4 type variants for {Lower, Upper} × {Explicit}.
// UnitDiag variants are filed as v0c-unitdiag follow-on (matches LAPACK API
// shape where unit-diag is a runtime flag, not a type parameter).
template void trmv<crd::f32, TriangularSide::Lower, TriangularDiag::Explicit>(
    const Triangular<crd::f32, TriangularSide::Lower, TriangularDiag::Explicit>&,
    crd::containers::Span<crd::f32>, Trans);
template void trmv<crd::f64, TriangularSide::Lower, TriangularDiag::Explicit>(
    const Triangular<crd::f64, TriangularSide::Lower, TriangularDiag::Explicit>&,
    crd::containers::Span<crd::f64>, Trans);
template void trmv<Complex32, TriangularSide::Lower, TriangularDiag::Explicit>(
    const Triangular<Complex32, TriangularSide::Lower, TriangularDiag::Explicit>&,
    crd::containers::Span<Complex32>, Trans);
template void trmv<Complex64, TriangularSide::Lower, TriangularDiag::Explicit>(
    const Triangular<Complex64, TriangularSide::Lower, TriangularDiag::Explicit>&,
    crd::containers::Span<Complex64>, Trans);
template void trmv<crd::f32, TriangularSide::Upper, TriangularDiag::Explicit>(
    const Triangular<crd::f32, TriangularSide::Upper, TriangularDiag::Explicit>&,
    crd::containers::Span<crd::f32>, Trans);
template void trmv<crd::f64, TriangularSide::Upper, TriangularDiag::Explicit>(
    const Triangular<crd::f64, TriangularSide::Upper, TriangularDiag::Explicit>&,
    crd::containers::Span<crd::f64>, Trans);
template void trmv<Complex32, TriangularSide::Upper, TriangularDiag::Explicit>(
    const Triangular<Complex32, TriangularSide::Upper, TriangularDiag::Explicit>&,
    crd::containers::Span<Complex32>, Trans);
template void trmv<Complex64, TriangularSide::Upper, TriangularDiag::Explicit>(
    const Triangular<Complex64, TriangularSide::Upper, TriangularDiag::Explicit>&,
    crd::containers::Span<Complex64>, Trans);

template void trsv<crd::f32, TriangularSide::Lower, TriangularDiag::Explicit>(
    const Triangular<crd::f32, TriangularSide::Lower, TriangularDiag::Explicit>&,
    crd::containers::Span<crd::f32>, Trans);
template void trsv<crd::f64, TriangularSide::Lower, TriangularDiag::Explicit>(
    const Triangular<crd::f64, TriangularSide::Lower, TriangularDiag::Explicit>&,
    crd::containers::Span<crd::f64>, Trans);
template void trsv<Complex32, TriangularSide::Lower, TriangularDiag::Explicit>(
    const Triangular<Complex32, TriangularSide::Lower, TriangularDiag::Explicit>&,
    crd::containers::Span<Complex32>, Trans);
template void trsv<Complex64, TriangularSide::Lower, TriangularDiag::Explicit>(
    const Triangular<Complex64, TriangularSide::Lower, TriangularDiag::Explicit>&,
    crd::containers::Span<Complex64>, Trans);
template void trsv<crd::f32, TriangularSide::Upper, TriangularDiag::Explicit>(
    const Triangular<crd::f32, TriangularSide::Upper, TriangularDiag::Explicit>&,
    crd::containers::Span<crd::f32>, Trans);
template void trsv<crd::f64, TriangularSide::Upper, TriangularDiag::Explicit>(
    const Triangular<crd::f64, TriangularSide::Upper, TriangularDiag::Explicit>&,
    crd::containers::Span<crd::f64>, Trans);
template void trsv<Complex32, TriangularSide::Upper, TriangularDiag::Explicit>(
    const Triangular<Complex32, TriangularSide::Upper, TriangularDiag::Explicit>&,
    crd::containers::Span<Complex32>, Trans);
template void trsv<Complex64, TriangularSide::Upper, TriangularDiag::Explicit>(
    const Triangular<Complex64, TriangularSide::Upper, TriangularDiag::Explicit>&,
    crd::containers::Span<Complex64>, Trans);

// tbmv / tbsv: 4 types each (Side / Diag are runtime params here for ergonomics
// since banded storage doesn't compile-time-vary).
template void tbmv<crd::f32>(const Banded<crd::f32>&, TriangularSide, TriangularDiag,
    crd::containers::Span<crd::f32>, Trans);
template void tbmv<crd::f64>(const Banded<crd::f64>&, TriangularSide, TriangularDiag,
    crd::containers::Span<crd::f64>, Trans);
template void tbmv<Complex32>(const Banded<Complex32>&, TriangularSide, TriangularDiag,
    crd::containers::Span<Complex32>, Trans);
template void tbmv<Complex64>(const Banded<Complex64>&, TriangularSide, TriangularDiag,
    crd::containers::Span<Complex64>, Trans);

template void tbsv<crd::f32>(const Banded<crd::f32>&, TriangularSide, TriangularDiag,
    crd::containers::Span<crd::f32>, Trans);
template void tbsv<crd::f64>(const Banded<crd::f64>&, TriangularSide, TriangularDiag,
    crd::containers::Span<crd::f64>, Trans);
template void tbsv<Complex32>(const Banded<Complex32>&, TriangularSide, TriangularDiag,
    crd::containers::Span<Complex32>, Trans);
template void tbsv<Complex64>(const Banded<Complex64>&, TriangularSide, TriangularDiag,
    crd::containers::Span<Complex64>, Trans);

} // namespace crd::hesap::dense
