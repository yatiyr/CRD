#include <crd/hesap/dense/blas1.hpp>

#include <crd/core/assert.hpp>
#include <crd/hesap/dense/detail/pairwise_sum.hpp>

#include <cmath>
#include <utility>

namespace crd::hesap::dense
{

// =======================================================================
// 1. axpy — y += alpha * x
// =======================================================================
template <typename T>
void axpy(T alpha, crd::containers::ConstSpan<T> x, crd::containers::Span<T> y)
{
    CRD_ASSERT_MSG(x.size() == y.size(), "axpy: x.size() != y.size()");
    for (crd::usize i = 0; i < x.size(); ++i)
    {
        y[i] = y[i] + alpha * x[i];
    }
}

// =======================================================================
// 2. dot — real-only sum of x_i * y_i
// =======================================================================
template <typename T>
T dot(crd::containers::ConstSpan<T> x, crd::containers::ConstSpan<T> y)
{
    static_assert(!is_complex_v<T>, "dot<T> is real-only; use dotu / dotc for complex");
    CRD_ASSERT_MSG(x.size() == y.size(), "dot: x.size() != y.size()");
    return detail::pairwise_sum_produced<T>(x.size(), [&](crd::usize i) { return x[i] * y[i]; });
}

// =======================================================================
// 3. dotu — complex unconjugated sum x_i * y_i
// =======================================================================
template <typename T>
Complex<T> dotu(
    crd::containers::ConstSpan<Complex<T>> x,
    crd::containers::ConstSpan<Complex<T>> y)
{
    CRD_ASSERT_MSG(x.size() == y.size(), "dotu: x.size() != y.size()");
    return detail::pairwise_sum_produced<Complex<T>>(
        x.size(),
        [&](crd::usize i) { return x[i] * y[i]; });
}

// =======================================================================
// 4. dotc — complex Hermitian sum conj(x_i) * y_i
// =======================================================================
template <typename T>
Complex<T> dotc(
    crd::containers::ConstSpan<Complex<T>> x,
    crd::containers::ConstSpan<Complex<T>> y)
{
    CRD_ASSERT_MSG(x.size() == y.size(), "dotc: x.size() != y.size()");
    return detail::pairwise_sum_produced<Complex<T>>(
        x.size(),
        [&](crd::usize i) { return conj(x[i]) * y[i]; });
}

// =======================================================================
// 5. nrm2 — Euclidean norm; returns real type
// =======================================================================
template <typename T>
RealType<T> nrm2(crd::containers::ConstSpan<T> x)
{
    using R = RealType<T>;
    // sqrt(sum |x_i|^2). For real T: |x_i|^2 = x_i * x_i. For complex T:
    // |x_i|^2 = re*re + im*im = norm_sq(z). Pairwise-sum the squared
    // magnitudes (real-valued), then sqrt.
    if constexpr (is_complex_v<T>)
    {
        const R sum_sq = detail::pairwise_sum_produced<R>(
            x.size(),
            [&](crd::usize i) -> R { return norm_sq(x[i]); });
        return std::sqrt(sum_sq);
    }
    else
    {
        const R sum_sq = detail::pairwise_sum_produced<R>(
            x.size(),
            [&](crd::usize i) -> R { return x[i] * x[i]; });
        return std::sqrt(sum_sq);
    }
}

// =======================================================================
// 6. scal — x *= alpha
// =======================================================================
template <typename T>
void scal(T alpha, crd::containers::Span<T> x)
{
    for (crd::usize i = 0; i < x.size(); ++i)
    {
        x[i] = alpha * x[i];
    }
}

// =======================================================================
// 7. copy — dst = src
// =======================================================================
template <typename T>
void copy(crd::containers::ConstSpan<T> src, crd::containers::Span<T> dst)
{
    CRD_ASSERT_MSG(src.size() == dst.size(), "copy: src.size() != dst.size()");
    for (crd::usize i = 0; i < src.size(); ++i)
    {
        dst[i] = src[i];
    }
}

// =======================================================================
// 8. swap — exchange x and y
// =======================================================================
template <typename T>
void swap(crd::containers::Span<T> x, crd::containers::Span<T> y)
{
    CRD_ASSERT_MSG(x.size() == y.size(), "swap: x.size() != y.size()");
    for (crd::usize i = 0; i < x.size(); ++i)
    {
        std::swap(x[i], y[i]);
    }
}

// =======================================================================
// 9. asum — sum of magnitudes; returns real type
// =======================================================================
template <typename T>
RealType<T> asum(crd::containers::ConstSpan<T> x)
{
    using R = RealType<T>;
    if constexpr (is_complex_v<T>)
    {
        // BLAS convention: complex asum = sum(|re| + |im|), not sum(hypot(re, im)).
        // The cheaper componentwise definition matches LAPACK reference.
        return detail::pairwise_sum_produced<R>(
            x.size(),
            [&](crd::usize i) -> R
            {
                const R r = x[i].re < R(0) ? -x[i].re : x[i].re;
                const R im_v = x[i].im < R(0) ? -x[i].im : x[i].im;
                return r + im_v;
            });
    }
    else
    {
        return detail::pairwise_sum_produced<R>(
            x.size(),
            [&](crd::usize i) -> R { return x[i] < R(0) ? -x[i] : x[i]; });
    }
}

// =======================================================================
// 10. iamax — argmax |x_i|, ties broken by FIRST index (D16)
// =======================================================================
template <typename T>
crd::usize iamax(crd::containers::ConstSpan<T> x)
{
    using R = RealType<T>;
    if (x.empty())
    {
        return 0;
    }
    crd::usize best_idx = 0;
    R best_mag = [&]() -> R
    {
        if constexpr (is_complex_v<T>)
        {
            const R r = x[0].re < R(0) ? -x[0].re : x[0].re;
            const R im_v = x[0].im < R(0) ? -x[0].im : x[0].im;
            return r + im_v;
        }
        else
        {
            return x[0] < R(0) ? -x[0] : x[0];
        }
    }();
    for (crd::usize i = 1; i < x.size(); ++i)
    {
        R mag;
        if constexpr (is_complex_v<T>)
        {
            const R r = x[i].re < R(0) ? -x[i].re : x[i].re;
            const R im_v = x[i].im < R(0) ? -x[i].im : x[i].im;
            mag = r + im_v;
        }
        else
        {
            mag = x[i] < R(0) ? -x[i] : x[i];
        }
        // Strict greater-than → ties stay with the earlier index.
        if (mag > best_mag)
        {
            best_mag = mag;
            best_idx = i;
        }
    }
    return best_idx;
}

// =======================================================================
// Explicit instantiations for f32 / f64 / Complex32 / Complex64.
// =======================================================================

// axpy
template void axpy<crd::f32>(crd::f32, crd::containers::ConstSpan<crd::f32>, crd::containers::Span<crd::f32>);
template void axpy<crd::f64>(crd::f64, crd::containers::ConstSpan<crd::f64>, crd::containers::Span<crd::f64>);
template void axpy<Complex32>(Complex32, crd::containers::ConstSpan<Complex32>, crd::containers::Span<Complex32>);
template void axpy<Complex64>(Complex64, crd::containers::ConstSpan<Complex64>, crd::containers::Span<Complex64>);

// dot (real-only)
template crd::f32 dot<crd::f32>(crd::containers::ConstSpan<crd::f32>, crd::containers::ConstSpan<crd::f32>);
template crd::f64 dot<crd::f64>(crd::containers::ConstSpan<crd::f64>, crd::containers::ConstSpan<crd::f64>);

// dotu / dotc (complex-only)
template Complex<crd::f32> dotu<crd::f32>(crd::containers::ConstSpan<Complex32>, crd::containers::ConstSpan<Complex32>);
template Complex<crd::f64> dotu<crd::f64>(crd::containers::ConstSpan<Complex64>, crd::containers::ConstSpan<Complex64>);
template Complex<crd::f32> dotc<crd::f32>(crd::containers::ConstSpan<Complex32>, crd::containers::ConstSpan<Complex32>);
template Complex<crd::f64> dotc<crd::f64>(crd::containers::ConstSpan<Complex64>, crd::containers::ConstSpan<Complex64>);

// nrm2
template crd::f32 nrm2<crd::f32>(crd::containers::ConstSpan<crd::f32>);
template crd::f64 nrm2<crd::f64>(crd::containers::ConstSpan<crd::f64>);
template crd::f32 nrm2<Complex32>(crd::containers::ConstSpan<Complex32>);
template crd::f64 nrm2<Complex64>(crd::containers::ConstSpan<Complex64>);

// scal
template void scal<crd::f32>(crd::f32, crd::containers::Span<crd::f32>);
template void scal<crd::f64>(crd::f64, crd::containers::Span<crd::f64>);
template void scal<Complex32>(Complex32, crd::containers::Span<Complex32>);
template void scal<Complex64>(Complex64, crd::containers::Span<Complex64>);

// copy
template void copy<crd::f32>(crd::containers::ConstSpan<crd::f32>, crd::containers::Span<crd::f32>);
template void copy<crd::f64>(crd::containers::ConstSpan<crd::f64>, crd::containers::Span<crd::f64>);
template void copy<Complex32>(crd::containers::ConstSpan<Complex32>, crd::containers::Span<Complex32>);
template void copy<Complex64>(crd::containers::ConstSpan<Complex64>, crd::containers::Span<Complex64>);

// swap
template void swap<crd::f32>(crd::containers::Span<crd::f32>, crd::containers::Span<crd::f32>);
template void swap<crd::f64>(crd::containers::Span<crd::f64>, crd::containers::Span<crd::f64>);
template void swap<Complex32>(crd::containers::Span<Complex32>, crd::containers::Span<Complex32>);
template void swap<Complex64>(crd::containers::Span<Complex64>, crd::containers::Span<Complex64>);

// asum
template crd::f32 asum<crd::f32>(crd::containers::ConstSpan<crd::f32>);
template crd::f64 asum<crd::f64>(crd::containers::ConstSpan<crd::f64>);
template crd::f32 asum<Complex32>(crd::containers::ConstSpan<Complex32>);
template crd::f64 asum<Complex64>(crd::containers::ConstSpan<Complex64>);

// iamax
template crd::usize iamax<crd::f32>(crd::containers::ConstSpan<crd::f32>);
template crd::usize iamax<crd::f64>(crd::containers::ConstSpan<crd::f64>);
template crd::usize iamax<Complex32>(crd::containers::ConstSpan<Complex32>);
template crd::usize iamax<Complex64>(crd::containers::ConstSpan<Complex64>);

} // namespace crd::hesap::dense
