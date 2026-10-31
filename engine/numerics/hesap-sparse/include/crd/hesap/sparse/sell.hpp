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
// x[0] reaches only the rows that store column 0, as in CSR). A slice stores its columns as u32 or as each row's
// first column plus 16-bit gaps (SellIndex::Delta16, 2 index bytes an element where the spmv streams them).
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

// How to_sell stores each slice's columns (crd/hesap/sparse/sparse_kernels.hpp, SellSlices): as u32, or as each row's
// first column and 16-bit gaps (Delta16), which cuts an element's index bytes from 4 to 2 where the spmv streams them.
enum class SellIndex : crd::u8
{
    Auto,    // Delta16 when a whole-matrix spmv would stream from beyond the L2 (sparse_resident_bytes()), else U32
    U32,     // every slice as u32
    Delta16, // every slice wider than 2 whose gaps fit 16 bits as Delta16, the others as u32
};

template <typename T>
struct SellMatrix
{
    static constexpr crd::u32 kC = detail::kSellC<T>;

    crd::u32                         rows = 0;
    crd::u32                         cols = 0;
    crd::u32                         num_slices = 0;
    crd::u32                         max_width = 0; // the widest slice_width
    crd::containers::Array<crd::u32> slice_ptr;    // length num_slices+1; element offset of each slice
    crd::containers::Array<crd::u32> slice_width;  // length num_slices; padded column count of each slice
    crd::containers::Array<crd::u32> slice_full;   // length num_slices; columns every row of the slice stores
    crd::containers::Array<crd::u32> row_len;      // length num_slices*kC; stored count of each σ row (0 = pad row)
    crd::containers::Array<T>        vals;         // column-major within slice; pad = 0
    crd::containers::Array<crd::u32> col_idx;      // the U32 slices' columns, laid out as vals; pad col = 0
    crd::containers::Array<crd::u32> delta_ptr;    // length num_slices+1; the Delta16 elements before each slice
    crd::containers::Array<crd::u32> col_first;    // num_slices*kC when any slice is Delta16: each σ row's first column
    crd::containers::Array<crd::u16> col_delta;    // the Delta16 slices' gaps, laid out as vals; 0 at w = 0 and pads
    crd::containers::Array<crd::u32> perm;         // length rows; perm[slice-row] = original row (σ row sort)
    bool perm_is_identity = true;                  // true when the σ sort reordered no rows (uniform/banded)

    explicit SellMatrix(crd::memory::IAllocator* alloc)
        : slice_ptr(alloc), slice_width(alloc), slice_full(alloc), row_len(alloc), vals(alloc), col_idx(alloc),
          delta_ptr(alloc), col_first(alloc), col_delta(alloc), perm(alloc)
    {
    }

    [[nodiscard]] crd::usize stored() const noexcept { return vals.size(); }

    // Whether slice s stores its columns as Delta16 (a slice without columns is U32).
    [[nodiscard]] bool slice_is_delta16(crd::u32 s) const noexcept { return delta_ptr[s + 1] != delta_ptr[s]; }

    // The bytes an spmv over slices [s_begin, s_end) streams from the matrix: values, columns and row lengths.
    [[nodiscard]] crd::usize range_bytes(crd::u32 s_begin, crd::u32 s_end) const noexcept
    {
        if (slice_ptr.empty())
        {
            return 0U;
        }
        const crd::usize elements = crd::usize{slice_ptr[s_end]} - slice_ptr[s_begin];
        const crd::usize deltas = crd::usize{delta_ptr[s_end]} - delta_ptr[s_begin];
        const crd::usize row_words = col_first.empty() ? 1U : 2U; // row_len, and col_first where it exists
        return elements * sizeof(T) + (elements - deltas) * sizeof(crd::u32) + deltas * sizeof(crd::u16) +
               crd::usize{s_end - s_begin} * kC * row_words * sizeof(crd::u32);
    }

    // The bytes of every array the matrix holds.
    [[nodiscard]] crd::usize stored_bytes() const noexcept
    {
        return vals.size() * sizeof(T) + col_delta.size() * sizeof(crd::u16) +
               (col_idx.size() + row_len.size() + slice_ptr.size() + slice_width.size() + slice_full.size() +
                delta_ptr.size() + col_first.size() + perm.size()) *
                   sizeof(crd::u32);
    }
};

// Build a SELL-C-σ matrix from a compressed CSR matrix. `sigma` is the
// row-length sort window (0 => global sort over all rows). Rows are stably
// sorted by nnz within each window so similar-length rows share a slice,
// minimising padding on irregular matrices. The sort is by ROW ONLY: each
// row's entries keep their column-ascending order, so per-row reduction is
// unchanged -> spmv stays bit-exact with the CSR baseline (D(sparse)-3). For
// uniform / banded matrices (equal row lengths) the stable sort is the
// identity permutation -> zero overhead and contiguous y writes. `index` picks each slice's column form; every form
// gives the same bits.
template <typename T>
[[nodiscard]] SellMatrix<T> to_sell(const SparseMatrix<T, SparseFormat::Csr>& a, crd::memory::IAllocator* alloc,
                                    crd::u32 sigma = 0, SellIndex index = SellIndex::Auto)
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

    // 1. per-row length, per-slice width = max length over the slice's permuted rows, and its full columns = min
    //    length over its C rows (a pad row's 0 included), which the kernels run without a row's length check.
    out.slice_width.resize(out.num_slices);
    out.slice_full.resize(out.num_slices);
    out.row_len.resize(static_cast<crd::usize>(out.num_slices) * height); // pad rows stay 0
    out.slice_ptr.resize(static_cast<crd::usize>(out.num_slices) + 1);
    out.slice_ptr[0] = 0;
    for (crd::u32 s = 0; s < out.num_slices; ++s)
    {
        crd::u32       w     = 0;
        crd::u32       full  = s * height + height <= pat.rows ? ~crd::u32{0} : 0U; // a pad row stores nothing
        const crd::u32 r_end = (s * height + height < pat.rows) ? (s * height + height) : pat.rows;
        for (crd::u32 sr = s * height; sr < r_end; ++sr)
        {
            const crd::u32 r   = perm[sr];
            const crd::u32 len = outer[r + 1] - outer[r];
            out.row_len[sr] = len;
            w = len > w ? len : w;
            full = len < full ? len : full;
        }
        out.slice_width[s]   = w;
        out.max_width        = w > out.max_width ? w : out.max_width;
        out.slice_full[s]    = full;
        out.slice_ptr[s + 1] = out.slice_ptr[s] + height * w;
    }

    // 2. each slice's column form: Delta16 where asked (Auto: where a whole-matrix spmv streams), the slice is wider
    //    than 2 (it then reads fewer index bytes: 4 C first columns plus 2 C a column against 4 C a column) and every
    //    gap of its rows fits 16 bits (u32 differences, so an unsorted row takes U32); else U32.
    const crd::u32 total = out.slice_ptr[out.num_slices];
    if (index == SellIndex::Auto)
    {
        const crd::usize spmv_bytes = crd::usize{total} * (sizeof(T) + sizeof(crd::u32)) +
                                      out.row_len.size() * sizeof(crd::u32) +
                                      (crd::usize{pat.rows} + pat.cols) * sizeof(T);
        index = spmv_bytes > sparse_resident_bytes() ? SellIndex::Delta16 : SellIndex::U32;
    }
    out.delta_ptr.resize(static_cast<crd::usize>(out.num_slices) + 1); // 0: no slice is Delta16
    for (crd::u32 s = 0; index == SellIndex::Delta16 && s < out.num_slices; ++s)
    {
        bool fits = out.slice_width[s] > 2U;
        const crd::u32 r_end = (s * height + height < pat.rows) ? (s * height + height) : pat.rows;
        for (crd::u32 sr = s * height; fits && sr < r_end; ++sr)
        {
            const crd::u32 r = perm[sr];
            for (crd::u32 k = outer[r] + 1U; fits && k < outer[r + 1]; ++k)
            {
                fits = inner[k] - inner[k - 1U] <= 0xFFFFU;
            }
        }
        out.delta_ptr[s + 1] = out.delta_ptr[s] + (fits ? height * out.slice_width[s] : 0U);
    }
    const crd::u32 deltas = out.delta_ptr[out.num_slices];
    if (deltas != 0U)
    {
        out.col_first.resize(out.row_len.size()); // 0 for empty and pad rows
    }

    // 3. fill column-major within each slice; pad slots stay 0 (val) / 0 (col) / 0 (gap).
    out.vals.resize(total);             // value-initialised to 0 -> padding is 0
    out.col_idx.resize(total - deltas); // 0 -> padding column 0
    out.col_delta.resize(deltas);
    for (crd::u32 s = 0; s < out.num_slices; ++s)
    {
        const crd::u32 base = out.slice_ptr[s];
        const bool delta = out.slice_is_delta16(s);
        const crd::u32 col_base = delta ? out.delta_ptr[s] : base - out.delta_ptr[s];
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
                out.vals[base + w * height + lane] = srcv[rs + w];
                const crd::u32 at = col_base + w * height + lane;
                if (!delta)
                {
                    out.col_idx[at] = inner[rs + w];
                }
                else if (w == 0U)
                {
                    out.col_first[sr] = inner[rs];
                }
                else
                {
                    out.col_delta[at] = static_cast<crd::u16>(inner[rs + w] - inner[rs + w - 1U]);
                }
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
// (crd/hesap/sparse/sparse_kernels.hpp), complex values as (re, im) pairs. The range sweeps Resident when its
// working set fits `resident_bytes` (the whole L2 serially, a job's cut in parallel). `gather` and `pair_width` force
// the kernel's pick (SellPick; the board's arms); false, y untouched, when the level refuses it.
template <typename T>
[[nodiscard]] bool sell_spmv_range(const SellMatrix<T>& a, T alpha, T beta, const T* xp, T* y, crd::u32 s_begin,
                                   crd::u32 s_end, crd::usize resident_bytes, SellGather gather, crd::u32 pair_width)
{
    using U = SellScalarT<T>;
    const SellSlices<U> slices{a.slice_ptr.data(),
                               a.slice_width.data(),
                               a.slice_full.data(),
                               a.row_len.data(),
                               sell_scalars(a.vals.data()),
                               a.col_idx.data(),
                               a.delta_ptr.data(),
                               a.col_first.data(),
                               a.col_delta.data(),
                               a.perm_is_identity ? nullptr : a.perm.data(),
                               a.rows,
                               a.cols,
                               a.max_width};
    // Resident when the range's matrix bytes, all of x and its rows of y fit resident_bytes.
    constexpr crd::u32 height = SellMatrix<T>::kC;
    const crd::u32 row_end = s_end * height < a.rows ? s_end * height : a.rows;
    const crd::u32 row_begin = s_begin * height < row_end ? s_begin * height : row_end;
    const crd::usize bytes = a.range_bytes(s_begin, s_end) + (crd::usize{a.cols} + (row_end - row_begin)) * sizeof(T);
    const SellSweep sweep = bytes <= resident_bytes ? SellSweep::Resident : SellSweep::Streaming;
    const SellPick pick{sweep, gather, pair_width};
    const SparseKernels& k = *sparse_kernels().table;
    if constexpr (std::is_same_v<T, crd::f64>)
    {
        return k.sell_spmv_f64(slices, alpha, beta, xp, y, s_begin, s_end, pick);
    }
    else if constexpr (std::is_same_v<T, crd::f32>)
    {
        return k.sell_spmv_f32(slices, alpha, beta, xp, y, s_begin, s_end, pick);
    }
    else
    {
        static_assert(std::is_same_v<T, Complex<crd::f64>> || std::is_same_v<T, Complex<crd::f32>>,
                      "SELL holds f64, f32, Complex<f64> or Complex<f32>");
        const SellComplexScales<U> scales{alpha.re, alpha.im, beta.re, beta.im};
        if constexpr (std::is_same_v<U, crd::f64>)
        {
            return k.sell_spmv_c64(slices, scales, sell_scalars(xp), sell_scalars(y), s_begin, s_end, pick);
        }
        else
        {
            return k.sell_spmv_c32(slices, scales, sell_scalars(xp), sell_scalars(y), s_begin, s_end, pick);
        }
    }
}

// The level's own pick, which is never refused.
template <typename T>
void sell_spmv_range(const SellMatrix<T>& a, T alpha, T beta, const T* xp, T* y, crd::u32 s_begin, crd::u32 s_end,
                     crd::usize resident_bytes)
{
    CRD_VERIFY(sell_spmv_range<T>(a, alpha, beta, xp, y, s_begin, s_end, resident_bytes, SellGather::Policy, 0U));
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
    detail::sell_spmv_range<T>(a, alpha, beta, x.data(), y.data(), 0, a.num_slices, sparse_resident_bytes());
}

} // namespace crd::hesap::sparse
