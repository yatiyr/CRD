#include <crd/hesap/dense/qr_colpiv.hpp>

#include <crd/core/assert.hpp>
#include <crd/hesap/dense/detail/householder.hpp>
#include <crd/math/simd/vec4d.hpp>
#include <crd/math/simd/vec8f.hpp>

#include <cmath>
#include <limits>
#include <type_traits>

namespace crd::hesap::dense
{
namespace
{
// Contiguous SIMD dot over [0, len).
template <typename T>
inline T cp_dot(const T* a, const T* b, crd::usize len) noexcept
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

// row[p] += s * col[p] over [0, len).
template <typename T>
inline void cp_axpy(T* row, const T* col, T s, crd::usize len) noexcept
{
    namespace simd = crd::math::simd;
    crd::usize p = 0;
    if constexpr (std::is_same_v<T, crd::f64>)
    {
        const simd::Vec4d sv(s);
        for (; p + 8 <= len; p += 8)
        {
            simd::Vec4d r0 = simd::fma(sv, simd::Vec4d::load(col + p), simd::Vec4d::load(row + p));
            simd::Vec4d r1 =
                simd::fma(sv, simd::Vec4d::load(col + p + 4), simd::Vec4d::load(row + p + 4));
            r0.store(row + p);
            r1.store(row + p + 4);
        }
    }
    else if constexpr (std::is_same_v<T, crd::f32>)
    {
        const simd::Vec8f sv(s);
        for (; p + 8 <= len; p += 8)
        {
            simd::Vec8f r = simd::fma(sv, simd::Vec8f::load(col + p), simd::Vec8f::load(row + p));
            r.store(row + p);
        }
    }
    for (; p < len; ++p)
    {
        row[p] += s * col[p];
    }
}

template <typename T>
inline T cp_nrm2(const T* a, crd::usize len) noexcept
{
    return std::sqrt(cp_dot<T>(a, a, len));
}

} // namespace

template <typename T, Layout L>
void factor_qr_colpiv(QRColPiv<T, L>& qr, RealType<T> rcond)
{
    static_assert(L == Layout::RowMajor, "factor_qr_colpiv currently supports RowMajor only");
    static_assert(!is_complex_v<T>, "factor_qr_colpiv is real-only (complex LS routes through SVD)");

    Matrix<T, L>& packed = qr.packed();
    const crd::usize m = packed.rows();
    const crd::usize n = packed.cols();
    const crd::usize k_count = m < n ? m : n;
    auto& taus = qr.taus();
    auto& jpvt = qr.jpvt();
    CRD_ASSERT_MSG(taus.size() == k_count, "factor_qr_colpiv: taus size mismatch");
    CRD_ASSERT_MSG(jpvt.size() == n, "factor_qr_colpiv: jpvt size mismatch");

    if (k_count == 0)
    {
        qr.set_rank(0);
        return;
    }

    crd::memory::IAllocator* alloc = packed.allocator();
    T* data = packed.data();
    const crd::usize ld = packed.ld();

    // Work on a TRANSPOSED scratch wt (n × m): wt[j*m + i] = A[i][j]. Each
    // column of A is then a contiguous length-m row of wt, so every pivot
    // norm / reflector / apply / column swap is a contiguous SIMD sweep.
    crd::containers::Array<T> wt(alloc);
    wt.resize(n * m);
    for (crd::usize i = 0; i < m; ++i)
    {
        const T* row = data + i * ld;
        for (crd::usize j = 0; j < n; ++j)
        {
            wt[j * m + i] = row[j];
        }
    }

    // Partial column norms (vn1 = tracked/downdated, vn2 = last exact recompute).
    crd::containers::Array<T> vn1(alloc);
    crd::containers::Array<T> vn2(alloc);
    vn1.resize(n);
    vn2.resize(n);
    for (crd::usize j = 0; j < n; ++j)
    {
        const T nrm = cp_nrm2<T>(wt.data() + j * m, m);
        vn1[j] = nrm;
        vn2[j] = nrm;
        jpvt[j] = j;
    }

    const T tol3z = std::sqrt(std::numeric_limits<T>::epsilon());

    for (crd::usize k = 0; k < k_count; ++k)
    {
        // Businger-Golub pivot: column in [k, n) of largest tracked norm.
        // Tie-break by lowest index (D(lstsq) determinism).
        crd::usize pvt = k;
        T pmax = vn1[k];
        for (crd::usize j = k + 1; j < n; ++j)
        {
            if (vn1[j] > pmax)
            {
                pmax = vn1[j];
                pvt = j;
            }
        }
        if (pvt != k)
        {
            T* a = wt.data() + pvt * m;
            T* b = wt.data() + k * m;
            for (crd::usize i = 0; i < m; ++i)
            {
                const T t = a[i];
                a[i] = b[i];
                b[i] = t;
            }
            const crd::usize jt = jpvt[pvt];
            jpvt[pvt] = jpvt[k];
            jpvt[k] = jt;
            vn1[pvt] = vn1[k];
            vn2[pvt] = vn2[k];
        }

        // Householder on the sub-column wt[k*m + k .. k*m + m).
        T* col = wt.data() + k * m;
        const crd::usize sub = m - k;  // length of x
        const auto h = detail::make_householder<T>(col + k, sub);
        taus[k] = h.tau;

        const crd::usize tail = sub - (sub > 0 ? 1 : 0);  // = m-k-1 when sub>0

        // Apply H_k to trailing columns j > k (implicit v[k]=1; tail = col[k+1..]).
        for (crd::usize j = k + 1; j < n; ++j)
        {
            T* cj = wt.data() + j * m;
            T w = cj[k];
            if (tail > 0)
            {
                w += cp_dot<T>(col + k + 1, cj + k + 1, tail);
            }
            w *= h.tau;
            cj[k] -= w;
            if (tail > 0)
            {
                cp_axpy<T>(cj + k + 1, col + k + 1, -w, tail);
            }

            // DLAQP2 partial-norm downdate on column j.
            if (vn1[j] != T{0})
            {
                T temp = std::abs(cj[k]) / vn1[j];
                temp = T{1} - temp * temp;
                if (temp < T{0})
                {
                    temp = T{0};
                }
                const T ratio = vn1[j] / vn2[j];
                const T temp2 = temp * ratio * ratio;
                if (temp2 <= tol3z)
                {
                    if (k + 1 < m)
                    {
                        vn1[j] = cp_nrm2<T>(cj + k + 1, m - k - 1);
                        vn2[j] = vn1[j];
                    }
                    else
                    {
                        vn1[j] = T{0};
                        vn2[j] = T{0};
                    }
                }
                else
                {
                    vn1[j] = vn1[j] * std::sqrt(temp);
                }
            }
        }

        col[k] = h.beta;  // store R diagonal (was alpha; v[k]=1 is implicit).
    }

    // Transpose wt back into the RowMajor packed layout: R in the upper
    // triangle (incl. diagonal), reflector tails in the strict lower.
    for (crd::usize j = 0; j < n; ++j)
    {
        const T* colj = wt.data() + j * m;
        for (crd::usize i = 0; i < m; ++i)
        {
            data[i * ld + j] = colj[i];
        }
    }

    // Numerical rank from the non-increasing |R[k,k]| diagonal.
    const T eps = std::numeric_limits<T>::epsilon();
    const T rc = (rcond < T{0}) ? static_cast<T>(m > n ? m : n) * eps : rcond;
    const T r00 = std::abs(data[0]);  // |R[0,0]| (= largest pivot)
    const T thresh = rc * r00;
    crd::usize rnk = 0;
    for (crd::usize k = 0; k < k_count; ++k)
    {
        if (std::abs(data[k * ld + k]) > thresh)
        {
            ++rnk;
        }
        else
        {
            break;  // diagonal is non-increasing — no later entry can exceed.
        }
    }
    qr.set_rank(rnk);
}

template <typename T, Layout L>
void apply_q_transpose(const QRColPiv<T, L>& qr, crd::containers::Span<T> x)
{
    static_assert(L == Layout::RowMajor, "apply_q_transpose currently supports RowMajor only");
    const Matrix<T, L>& packed = qr.packed();
    const crd::usize m = packed.rows();
    const crd::usize k_count = qr.num_reflectors();
    CRD_ASSERT_MSG(x.size() == m, "apply_q_transpose: x size != m");
    const T* data = packed.data();
    const crd::usize ld = packed.ld();
    const auto& taus = qr.taus();

    for (crd::usize k = 0; k < k_count; ++k)
    {
        const T tau = taus[k];
        if (tau == T{0})
        {
            continue;
        }
        T w = x[k];
        for (crd::usize i = k + 1; i < m; ++i)
        {
            w += data[i * ld + k] * x[i];
        }
        w *= tau;
        x[k] -= w;
        for (crd::usize i = k + 1; i < m; ++i)
        {
            x[i] -= w * data[i * ld + k];
        }
    }
}

template <typename T, Layout L>
void apply_q(const QRColPiv<T, L>& qr, crd::containers::Span<T> x)
{
    static_assert(L == Layout::RowMajor, "apply_q currently supports RowMajor only");
    const Matrix<T, L>& packed = qr.packed();
    const crd::usize m = packed.rows();
    const crd::usize k_count = qr.num_reflectors();
    CRD_ASSERT_MSG(x.size() == m, "apply_q: x size != m");
    const T* data = packed.data();
    const crd::usize ld = packed.ld();
    const auto& taus = qr.taus();

    for (crd::usize kk = k_count; kk-- > 0;)
    {
        const T tau = taus[kk];
        if (tau == T{0})
        {
            continue;
        }
        T w = x[kk];
        for (crd::usize i = kk + 1; i < m; ++i)
        {
            w += data[i * ld + kk] * x[i];
        }
        w *= tau;
        x[kk] -= w;
        for (crd::usize i = kk + 1; i < m; ++i)
        {
            x[i] -= w * data[i * ld + kk];
        }
    }
}

template void factor_qr_colpiv<float, Layout::RowMajor>(QRColPiv<float, Layout::RowMajor>&, float);
template void factor_qr_colpiv<double, Layout::RowMajor>(QRColPiv<double, Layout::RowMajor>&, double);
template void apply_q_transpose<float, Layout::RowMajor>(const QRColPiv<float, Layout::RowMajor>&,
                                                         crd::containers::Span<float>);
template void apply_q_transpose<double, Layout::RowMajor>(const QRColPiv<double, Layout::RowMajor>&,
                                                          crd::containers::Span<double>);
template void apply_q<float, Layout::RowMajor>(const QRColPiv<float, Layout::RowMajor>&,
                                               crd::containers::Span<float>);
template void apply_q<double, Layout::RowMajor>(const QRColPiv<double, Layout::RowMajor>&,
                                                crd::containers::Span<double>);

} // namespace crd::hesap::dense
