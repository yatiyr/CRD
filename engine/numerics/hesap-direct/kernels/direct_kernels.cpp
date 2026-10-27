// The hesap-direct kernels of crd/hesap/direct/direct_kernels.hpp, compiled once per CPU level by crd_add_cpu_kernels
// (../CMakeLists.txt, ADR-0134 §5). Everything here lives in the level's namespace (CRD_MATH_LEVEL_NS).
//
// The bodies are written once over the dense vector steps (crd/hesap/dense/detail/vector_steps.hpp). The single-RHS
// walks read four panel columns together: one sequential read stream reached about 22.7 GB/s on the measured host and
// four interleaved ones about 36.9 GB/s, as a dgemv-based solve does. The multi-RHS walks keep a 256-row block of
// their scratch in cache while each panel column streams once. Neither changes an element's chain.

#include <crd/hesap/dense/detail/vector_steps.hpp>
#include <crd/hesap/direct/direct_kernels.hpp>

namespace crd::hesap::direct::detail::inline CRD_MATH_LEVEL_NS
{
namespace
{

using crd::hesap::dense::detail::for_each_vector;
using crd::hesap::dense::detail::load_n;
using crd::hesap::dense::detail::madd;
using crd::hesap::dense::detail::store_n;
using crd::hesap::dense::detail::unroll;
using crd::hesap::dense::detail::VectorOf;

// The panel columns a single-RHS walk reads together.
constexpr crd::usize kStreams = 4;

// The rows of a multi-RHS block that stay in cache while the panel columns stream.
constexpr crd::usize kRowBlock = 256;

// The panel columns a multi-RHS reduction sums together, one accumulator vector each: eight YMM from avx2; below it a
// vector is two XMM (or an array), so four.
#if CRD_SIMD_HAS_AVX2
constexpr crd::usize kReduceColumns = 8;
#else
constexpr crd::usize kReduceColumns = 4;
#endif

// y[i] + sum over q < G of s[q] col[q][i] for i < n, q ascending, the level's step: two vectors a pass (two independent
// chains), then the remaining vectors and the partial one.
template <crd::usize G, typename T>
CRD_FORCEINLINE void columns_into(T* y, const T* const* col, const T* s, crd::usize n) noexcept
{
    using V = typename VectorOf<T>::Type;
    constexpr crd::usize w = VectorOf<T>::kWidth;
    crd::usize i = 0;
    for (; i + 2U * w <= n; i += 2U * w)
    {
        V ta = V::load(y + i);
        V tb = V::load(y + i + w);
        auto step = [&](auto q)
        {
            constexpr crd::usize at = decltype(q)::kValue;
            const V sq(s[at]);
            ta = madd(sq, V::load(col[at] + i), ta);
            tb = madd(sq, V::load(col[at] + i + w), tb);
        };
        unroll<G>(step);
        ta.store(y + i);
        tb.store(y + i + w);
    }
    for_each_vector<T>(n - i,
                       [y, col, s, i](crd::usize k, crd::usize count)
                       {
                           V t = load_n<V>(y + i + k, count);
                           auto step = [&](auto q)
                           {
                               constexpr crd::usize at = decltype(q)::kValue;
                               t = madd(V(s[at]), load_n<V>(col[at] + i + k, count), t);
                           };
                           unroll<G>(step);
                           store_n(t, y + i + k, count);
                       });
}

// y + A^T x over rows of a (row r at a + r * lda, the scale x[r * incx], negated when Minus), kStreams rows a pass.
template <bool Minus, typename T>
CRD_FORCEINLINE void rows_into(T* y, const T* a, crd::usize lda, const T* x, crd::usize incx, crd::usize rows,
                               crd::usize n) noexcept
{
    crd::usize r = 0;
    for (; r + kStreams <= rows; r += kStreams)
    {
        const T* col[kStreams];
        T s[kStreams];
        for (crd::usize q = 0; q < kStreams; ++q)
        {
            col[q] = a + (r + q) * lda;
            s[q] = Minus ? -x[(r + q) * incx] : x[(r + q) * incx];
        }
        columns_into<kStreams>(y, col, s, n);
    }
    for (; r < rows; ++r)
    {
        const T* col[1] = {a + r * lda};
        const T s[1] = {Minus ? -x[r * incx] : x[r * incx]};
        columns_into<1>(y, col, s, n);
    }
}

template <typename T> void forward_below_kernel(const dense::GemvtRows<T>& view) noexcept
{
    const dense::GemvtRows<T> g = view;
    rows_into<false>(g.y, g.a, g.lda, g.x, g.incx, g.rows, g.cols);
}

template <typename T> void forward_diagonal_kernel(T* x, const T* panel, crd::usize ld, crd::usize nc) noexcept
{
    for (crd::usize j0 = 0; j0 < nc; j0 += kStreams)
    {
        const crd::usize end = j0 + kStreams <= nc ? j0 + kStreams : nc;
        for (crd::usize j = j0; j < end; ++j)
        {
            const T* col = panel + j * ld;
            const T yj = x[j] / col[j];
            x[j] = yj;
            for (crd::usize i = j + 1U; i < end; ++i)
            {
                x[i] = x[i] - col[i] * yj;
            }
        }
        if (end < nc)
        {
            rows_into<true>(x + end, panel + j0 * ld + end, ld, x + j0, 1U, end - j0, nc - end);
        }
    }
}

template <typename T> void block_forward_below_kernel(const PanelProduct<T>& view) noexcept
{
    const PanelProduct<T> p = view;
    for (crd::usize i = 0; i < p.rows * p.nrhs; ++i)
    {
        p.out[i] = T(0);
    }
    for (crd::usize r0 = 0; r0 < p.rows; r0 += kRowBlock)
    {
        const crd::usize n = r0 + kRowBlock <= p.rows ? kRowBlock : p.rows - r0;
        crd::usize k = 0;
        for (; k + kStreams <= p.nc; k += kStreams)
        {
            const T* col[kStreams];
            for (crd::usize q = 0; q < kStreams; ++q)
            {
                col[q] = p.panel + (k + q) * p.ld + r0;
            }
            for (crd::usize c = 0; c < p.nrhs; ++c)
            {
                T s[kStreams];
                for (crd::usize q = 0; q < kStreams; ++q)
                {
                    s[q] = p.y[k + q + c * p.ldy];
                }
                columns_into<kStreams>(p.out + c * p.rows + r0, col, s, n);
            }
        }
        for (; k < p.nc; ++k)
        {
            const T* col[1] = {p.panel + k * p.ld + r0};
            for (crd::usize c = 0; c < p.nrhs; ++c)
            {
                const T s[1] = {p.y[k + c * p.ldy]};
                columns_into<1>(p.out + c * p.rows + r0, col, s, n);
            }
        }
    }
}

// acc(q, c) + sum over r in [r0, r1) of col[q][r] wt(r, c), r ascending, for the G columns q and every RHS c: the
// vectors run over c (a wt row's values are contiguous), one accumulator a column.
template <crd::usize G, typename T>
CRD_FORCEINLINE void reduce_columns(T* acc, const T* const* col, const T* wt, crd::usize nrhs, crd::usize r0,
                                    crd::usize r1) noexcept
{
    using V = typename VectorOf<T>::Type;
    for_each_vector<T>(nrhs,
                       [acc, col, wt, nrhs, r0, r1](crd::usize c, crd::usize count)
                       {
                           V a[G];
                           auto load = [&](auto q)
                           {
                               constexpr crd::usize at = decltype(q)::kValue;
                               a[at] = load_n<V>(acc + at * nrhs + c, count);
                           };
                           unroll<G>(load);
                           for (crd::usize r = r0; r < r1; ++r)
                           {
                               const V row = load_n<V>(wt + r * nrhs + c, count);
                               auto step = [&](auto q)
                               {
                                   constexpr crd::usize at = decltype(q)::kValue;
                                   a[at] = madd(V(col[at][r]), row, a[at]);
                               };
                               unroll<G>(step);
                           }
                           auto store = [&](auto q)
                           {
                               constexpr crd::usize at = decltype(q)::kValue;
                               store_n(a[at], acc + at * nrhs + c, count);
                           };
                           unroll<G>(store);
                       });
}

template <crd::usize G, typename T>
CRD_FORCEINLINE void reduce_group(const PanelReduce<T>& p, crd::usize k, crd::usize r0, crd::usize r1) noexcept
{
    const T* col[G];
    for (crd::usize q = 0; q < G; ++q)
    {
        col[q] = p.panel + (k + q) * p.ld;
    }
    reduce_columns<G>(p.acc + k * p.nrhs, col, p.wt, p.nrhs, r0, r1);
}

template <typename T> void block_backward_below_kernel(const PanelReduce<T>& view) noexcept
{
    const PanelReduce<T> p = view;
    for (crd::usize i = 0; i < p.nc * p.nrhs; ++i)
    {
        p.acc[i] = T(0);
    }
    for (crd::usize r0 = 0; r0 < p.rows; r0 += kRowBlock)
    {
        const crd::usize r1 = r0 + kRowBlock <= p.rows ? r0 + kRowBlock : p.rows;
        crd::usize k = 0;
        for (; k + kReduceColumns <= p.nc; k += kReduceColumns)
        {
            reduce_group<kReduceColumns>(p, k, r0, r1);
        }
        for (; k < p.nc; ++k)
        {
            reduce_group<1>(p, k, r0, r1);
        }
    }
    for (crd::usize k = 0; k < p.nc; ++k)
    {
        for (crd::usize c = 0; c < p.nrhs; ++c)
        {
            p.y[k + c * p.ldy] = p.y[k + c * p.ldy] - p.acc[k * p.nrhs + c];
        }
    }
}

template <typename T>
void block_forward_diagonal_kernel(T* d, crd::usize nrhs, const T* panel, crd::usize ld, crd::usize nc) noexcept
{
    using V = typename VectorOf<T>::Type;
    for (crd::usize j0 = 0; j0 < nc; j0 += kStreams)
    {
        const crd::usize end = j0 + kStreams <= nc ? j0 + kStreams : nc;
        for (crd::usize j = j0; j < end; ++j)
        {
            const T* col = panel + j * ld;
            T* dj = d + j * nrhs;
            for (crd::usize c = 0; c < nrhs; ++c)
            {
                dj[c] = dj[c] / col[j];
            }
            for (crd::usize i = j + 1U; i < end; ++i)
            {
                T* di = d + i * nrhs;
                for (crd::usize c = 0; c < nrhs; ++c)
                {
                    di[c] = di[c] - col[i] * dj[c];
                }
            }
        }
        if (end == nc)
        {
            break;
        }
        // A whole block: each row below it takes the block's four columns in order, the vectors running over c.
        const T* col0 = panel + j0 * ld;
        const T* rows0 = d + j0 * nrhs;
        for_each_vector<T>(nrhs,
                           [d, nrhs, col0, rows0, ld, end, nc](crd::usize c, crd::usize count)
                           {
                               V y[kStreams];
                               auto load = [&](auto q)
                               {
                                   constexpr crd::usize at = decltype(q)::kValue;
                                   y[at] = load_n<V>(rows0 + at * nrhs + c, count);
                               };
                               unroll<kStreams>(load);
                               for (crd::usize i = end; i < nc; ++i)
                               {
                                   T* di = d + i * nrhs + c;
                                   V t = load_n<V>(di, count);
                                   auto step = [&](auto q)
                                   {
                                       constexpr crd::usize at = decltype(q)::kValue;
                                       t = madd(V(-col0[at * ld + i]), y[at], t);
                                   };
                                   unroll<kStreams>(step);
                                   store_n(t, di, count);
                               }
                           });
    }
}

// Each of the G block rows j0 + q loses the sum from +0 over the rows k in [far0, nc) ascending of col[q][k] d(k, c).
template <crd::usize G, typename T>
CRD_FORCEINLINE void far_terms(T* d, crd::usize nrhs, const T* const* col, crd::usize j0, crd::usize far0,
                               crd::usize nc) noexcept
{
    using V = typename VectorOf<T>::Type;
    for_each_vector<T>(nrhs,
                       [d, nrhs, col, j0, far0, nc](crd::usize c, crd::usize count)
                       {
                           V a[G];
                           auto zero = [&](auto q)
                           {
                               a[decltype(q)::kValue] = V::zero();
                           };
                           unroll<G>(zero);
                           for (crd::usize k = far0; k < nc; ++k)
                           {
                               const V dk = load_n<V>(d + k * nrhs + c, count);
                               auto step = [&](auto q)
                               {
                                   constexpr crd::usize at = decltype(q)::kValue;
                                   a[at] = madd(V(col[at][k]), dk, a[at]);
                               };
                               unroll<G>(step);
                           }
                           auto subtract = [&](auto q)
                           {
                               constexpr crd::usize at = decltype(q)::kValue;
                               T* row = d + (j0 + at) * nrhs + c;
                               store_n(load_n<V>(row, count) - a[at], row, count);
                           };
                           unroll<G>(subtract);
                       });
}

template <typename T>
void block_backward_diagonal_kernel(T* d, crd::usize nrhs, const T* panel, crd::usize ld, crd::usize nc) noexcept
{
    if (nc == 0)
    {
        return;
    }
    for (crd::usize j0 = ((nc - 1U) / kStreams) * kStreams;; j0 -= kStreams)
    {
        const crd::usize end = j0 + kStreams <= nc ? j0 + kStreams : nc;
        if (end < nc)
        {
            const T* col[kStreams];
            for (crd::usize q = 0; q < end - j0; ++q)
            {
                col[q] = panel + (j0 + q) * ld;
            }
            if (end - j0 == kStreams)
            {
                far_terms<kStreams>(d, nrhs, col, j0, end, nc);
            }
            else
            {
                for (crd::usize q = 0; q < end - j0; ++q)
                {
                    far_terms<1>(d, nrhs, col + q, j0 + q, end, nc);
                }
            }
        }
        for (crd::usize j = end; j-- > j0;)
        {
            const T* col = panel + j * ld;
            T* dj = d + j * nrhs;
            for (crd::usize k = j + 1U; k < end; ++k)
            {
                const T* dk = d + k * nrhs;
                for (crd::usize c = 0; c < nrhs; ++c)
                {
                    dj[c] = dj[c] - col[k] * dk[c];
                }
            }
            for (crd::usize c = 0; c < nrhs; ++c)
            {
                dj[c] = dj[c] / col[j];
            }
        }
        if (j0 == 0)
        {
            break;
        }
    }
}

} // namespace

template <typename T> constexpr DirectTypeKernels<T> type_kernels() noexcept
{
    return {
        &forward_below_kernel<T>,        &forward_diagonal_kernel<T>,       &block_forward_below_kernel<T>,
        &block_backward_below_kernel<T>, &block_forward_diagonal_kernel<T>, &block_backward_diagonal_kernel<T>,
    };
}

} // namespace crd::hesap::direct::detail::inline CRD_MATH_LEVEL_NS

namespace crd::hesap::direct
{

namespace level = detail::CRD_MATH_LEVEL_NS;

const DirectKernels CRD_KERNEL_TABLE(kDirectKernels) = {
    level::type_kernels<crd::f64>(),
    level::type_kernels<crd::f32>(),
};

} // namespace crd::hesap::direct
