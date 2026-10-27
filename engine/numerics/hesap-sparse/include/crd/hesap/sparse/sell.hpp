#pragma once

#include <crd/containers/array.hpp>
#include <crd/containers/sort.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/complex.hpp>
#include <crd/hesap/sparse/sparse_dispatch.hpp>
#include <crd/hesap/sparse/sparse_matrix.hpp>
#include <crd/hesap/sparse/spmv.hpp>  // Trans, detail::spmv_is_zero / spmv_conj

#include <type_traits>
#include <utility>

namespace crd::hesap::sparse
{
// -----------------------------------------------------------------------
// SELL-C-σ (SELL-C-sigma) sparse format -- the SIMD-friendly spmv primary
// (Kreutzer 2014 / SPC5 2023). Rows are grouped into slices of `C` rows;
// within a slice every row is padded to the slice's longest row, and the
// `C` lanes of each column position are stored CONTIGUOUSLY (column-major
// within the slice) so a single SIMD load fetches one entry from each of C
// rows. The spmv kernel then runs C rows in parallel across SIMD lanes,
// breaking the serial accumulator dependency chain that bottlenecks scalar
// CSR spmv.
//
// v1b-2 ships σ=1 (no row reordering) per D(sparse)-4: within-row columns
// stay ascending, so per-row reduction order == CSR -> bit-exact with the
// v1b-1 baseline. Pad slots are value 0 at column 0; row_len keeps each row's
// own length, so the kernels never multiply a pad by x (an infinite or NaN
// x[0] reaches only the rows that store column 0, as in CSR).
//
// Slice height C is per-T (D(sparse)-4): f32->8, f64->4, complex->4. Every
// type runs per-level kernels (SIMD-DISPATCH, crd/hesap/sparse/sparse_kernels.hpp).
// -----------------------------------------------------------------------

namespace detail
{
template <typename T> inline constexpr crd::u32 kSellC = kSellSliceF64;
template <> inline constexpr crd::u32 kSellC<crd::f32> = kSellSliceF32;
template <> inline constexpr crd::u32 kSellC<Complex<crd::f64>> = kSellSliceComplex;
template <> inline constexpr crd::u32 kSellC<Complex<crd::f32>> = kSellSliceComplex;

// The real scalar of a SELL value type, in which the kernels read it: a complex value is its (re, im) pair.
template <typename T> struct SellScalar
{
    using Type = T;
};
template <typename U> struct SellScalar<Complex<U>>
{
    static_assert(std::is_standard_layout_v<Complex<U>> && sizeof(Complex<U>) == 2 * sizeof(U),
                  "the kernels read Complex<U> as (re, im) pairs");
    using Type = U;
};
template <typename T> using SellScalarT = typename SellScalar<T>::Type;

template <typename T> const SellScalarT<T>* sell_scalars(const T* p) noexcept
{
    return reinterpret_cast<const SellScalarT<T>*>(p);
}
template <typename T> SellScalarT<T>* sell_scalars(T* p) noexcept
{
    return reinterpret_cast<SellScalarT<T>*>(p);
}
} // namespace detail

template <typename T>
struct SellMatrix
{
    static constexpr crd::u32 kC = detail::kSellC<T>;

    crd::u32                         rows = 0;
    crd::u32                         cols = 0;
    crd::u32                         num_slices = 0;
    crd::containers::Array<crd::u32> slice_ptr;    // length num_slices+1; element offset of each slice
    crd::containers::Array<crd::u32> slice_width;  // length num_slices; padded column count of each slice
    crd::containers::Array<crd::u32> row_len;      // length num_slices*kC; stored count of each σ row (0 = pad row)
    crd::containers::Array<T>        vals;         // column-major within slice; pad = 0
    crd::containers::Array<crd::u32> col_idx;      // parallel to vals; pad col = 0
    crd::containers::Array<crd::u32> perm;         // length rows; perm[slice-row] = original row (σ row sort)
    bool perm_is_identity = true;                  // true when the σ sort reordered no rows (uniform/banded)

    explicit SellMatrix(crd::memory::IAllocator* alloc)
        : slice_ptr(alloc), slice_width(alloc), row_len(alloc), vals(alloc), col_idx(alloc), perm(alloc)
    {
    }

    [[nodiscard]] crd::usize stored() const noexcept { return vals.size(); }
};

// Build a SELL-C-σ matrix from a compressed CSR matrix. `sigma` is the
// row-length sort window (0 => global sort over all rows). Rows are stably
// sorted by nnz within each window so similar-length rows share a slice,
// minimising padding on irregular matrices. The sort is by ROW ONLY: each
// row's entries keep their column-ascending order, so per-row reduction is
// unchanged -> spmv stays bit-exact with the CSR baseline (D(sparse)-3). For
// uniform / banded matrices (equal row lengths) the stable sort is the
// identity permutation -> zero overhead and contiguous y writes.
template <typename T>
[[nodiscard]] SellMatrix<T> to_sell(const SparseMatrix<T, SparseFormat::Csr>& a, crd::memory::IAllocator* alloc,
                                    crd::u32 sigma = 0)
{
    const SparsePattern& pat = a.pattern();
    CRD_ASSERT_MSG(pat.is_compressed(), "to_sell requires a compressed CSR matrix");

    constexpr crd::u32 height = SellMatrix<T>::kC;
    SellMatrix<T>      out(alloc);
    out.rows       = pat.rows;
    out.cols       = pat.cols;
    out.num_slices = (pat.rows + height - 1) / height;

    const crd::u32* outer = pat.outer_ptr.data();
    const crd::u32* inner = pat.inner_idx.data();
    const T*        srcv  = a.values().values.data();

    // 0. perm = rows stably sorted by length within each window of `sigma`.
    out.perm.resize(pat.rows);
    for (crd::u32 i = 0; i < pat.rows; ++i)
    {
        out.perm[i] = i;
    }
    const crd::u32 win = (sigma == 0 || sigma > pat.rows) ? pat.rows : sigma;
    if (pat.rows > 1)
    {
        for (crd::u32 w0 = 0; w0 < pat.rows; w0 += win)
        {
            const crd::u32 w1 = (w0 + win < pat.rows) ? (w0 + win) : pat.rows;
            crd::containers::stable_sort(
                out.perm.data() + w0, out.perm.data() + w1,
                [outer](crd::u32 ra, crd::u32 rb) {
                    return (outer[ra + 1] - outer[ra]) < (outer[rb + 1] - outer[rb]);
                },
                alloc);
        }
    }
    const crd::u32* perm = out.perm.data();
    for (crd::u32 i = 0; i < pat.rows; ++i)
    {
        if (perm[i] != i)
        {
            out.perm_is_identity = false;
            break;
        }
    }

    // 1. per-row length and per-slice width = max length over the slice's permuted rows.
    out.slice_width.resize(out.num_slices);
    out.row_len.resize(static_cast<crd::usize>(out.num_slices) * height); // pad rows stay 0
    out.slice_ptr.resize(static_cast<crd::usize>(out.num_slices) + 1);
    out.slice_ptr[0] = 0;
    for (crd::u32 s = 0; s < out.num_slices; ++s)
    {
        crd::u32       w     = 0;
        const crd::u32 r_end = (s * height + height < pat.rows) ? (s * height + height) : pat.rows;
        for (crd::u32 sr = s * height; sr < r_end; ++sr)
        {
            const crd::u32 r   = perm[sr];
            const crd::u32 len = outer[r + 1] - outer[r];
            out.row_len[sr] = len;
            w = len > w ? len : w;
        }
        out.slice_width[s]   = w;
        out.slice_ptr[s + 1] = out.slice_ptr[s] + height * w;
    }

    // 2. fill column-major within each slice; pad slots stay 0 (val) / 0 (col).
    const crd::u32 total = out.slice_ptr[out.num_slices];
    out.vals.resize(total);     // value-initialised to 0 -> padding is 0
    out.col_idx.resize(total);  // 0 -> padding column 0
    for (crd::u32 s = 0; s < out.num_slices; ++s)
    {
        const crd::u32 base = out.slice_ptr[s];
        for (crd::u32 lane = 0; lane < height; ++lane)
        {
            const crd::u32 sr = s * height + lane;
            if (sr >= pat.rows)
            {
                break;  // padding rows: already 0
            }
            const crd::u32 r   = perm[sr];
            const crd::u32 rs  = outer[r];
            const crd::u32 len = outer[r + 1] - rs;
            for (crd::u32 w = 0; w < len; ++w)
            {
                const crd::u32 dst = base + w * height + lane;
                out.vals[dst]      = srcv[rs + w];
                out.col_idx[dst]   = inner[rs + w];
            }
        }
    }
    return out;
}

namespace detail
{
// Core SELL spmv over a slice range [s_begin, s_end). Each slice writes its C
// lanes into y[perm[...]] -- and since perm is a bijection and slices partition
// rows, disjoint slice ranges write disjoint y rows, so the parallel driver can
// run ranges concurrently with no races (deterministic, bit-exact vs serial).
// Every type runs the hesap-sparse kernels of the process's CPU level
// (crd/hesap/sparse/sparse_kernels.hpp), complex values as (re, im) pairs.
template <typename T>
void sell_spmv_range(const SellMatrix<T>& a, T alpha, T beta, const T* xp, T* y, crd::u32 s_begin, crd::u32 s_end)
{
    using U = SellScalarT<T>;
    const SellSlices<U> slices{a.slice_ptr.data(),
                               a.slice_width.data(),
                               a.row_len.data(),
                               sell_scalars(a.vals.data()),
                               a.col_idx.data(),
                               a.perm_is_identity ? nullptr : a.perm.data(),
                               a.rows};
    const SparseKernels& k = *sparse_kernels().table;
    if constexpr (std::is_same_v<T, crd::f64>)
    {
        k.sell_spmv_f64(slices, alpha, beta, xp, y, s_begin, s_end);
    }
    else if constexpr (std::is_same_v<T, crd::f32>)
    {
        k.sell_spmv_f32(slices, alpha, beta, xp, y, s_begin, s_end);
    }
    else
    {
        static_assert(std::is_same_v<T, Complex<crd::f64>> || std::is_same_v<T, Complex<crd::f32>>,
                      "SELL holds f64, f32, Complex<f64> or Complex<f32>");
        const SellComplexScales<U> scales{alpha.re, alpha.im, beta.re, beta.im};
        if constexpr (std::is_same_v<U, crd::f64>)
        {
            k.sell_spmv_c64(slices, scales, sell_scalars(xp), sell_scalars(y), s_begin, s_end);
        }
        else
        {
            k.sell_spmv_c32(slices, scales, sell_scalars(xp), sell_scalars(y), s_begin, s_end);
        }
    }
}
} // namespace detail

// y = alpha * A * x + beta * y  (SELL, Trans::None). Bit-exact with the CSR
// baseline: each lane accumulates its row left-to-right in column order via
// the two-rounded `acc + val*x` (D(sparse)-3). beta == 0 is NaN-safe. Every
// type runs the slice-pair kernels of the CPU level.
template <typename T>
void spmv_sell(T alpha, const SellMatrix<T>& a, crd::containers::ConstSpan<T> x, T beta, crd::containers::Span<T> y)
{
    CRD_ASSERT_MSG(x.size() == a.cols && y.size() == a.rows, "spmv_sell: x must be cols(), y must be rows()");
    detail::sell_spmv_range<T>(a, alpha, beta, x.data(), y.data(), 0, a.num_slices);
}

} // namespace crd::hesap::sparse
