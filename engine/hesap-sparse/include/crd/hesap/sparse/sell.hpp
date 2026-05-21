#pragma once

#include <crd/containers/array.hpp>
#include <crd/containers/sort.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/complex.hpp>
#include <crd/hesap/sparse/sparse_matrix.hpp>
#include <crd/hesap/sparse/spmv.hpp>  // Trans, detail::spmv_is_zero / spmv_conj
#include <crd/math/simd/backend.hpp>
#include <crd/math/simd/vec4d.hpp>
#include <crd/math/simd/vec8f.hpp>

#include <type_traits>
#include <utility>

#if CRD_SIMD_HAS_AVX2
#include <immintrin.h>
#endif

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
// v1b-1 baseline. Pad slots are value 0 at column 0 (a + 0 == a no-op).
//
// Slice height C is per-T (D(sparse)-4): f32->8, f64->4, complex->4 (scalar).
// -----------------------------------------------------------------------

namespace detail
{
template <typename T>
inline constexpr crd::u32 kSellC = 4;  // f64 + complex
template <>
inline constexpr crd::u32 kSellC<crd::f32> = 8;
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
    crd::containers::Array<T>        vals;         // column-major within slice; pad = 0
    crd::containers::Array<crd::u32> col_idx;      // parallel to vals; pad col = 0
    crd::containers::Array<crd::u32> perm;         // length rows; perm[slice-row] = original row (σ row sort)
    bool perm_is_identity = true;                  // true when the σ sort reordered no rows (uniform/banded)

    explicit SellMatrix(crd::memory::IAllocator* alloc)
        : slice_ptr(alloc), slice_width(alloc), vals(alloc), col_idx(alloc), perm(alloc)
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

    constexpr crd::u32 C = SellMatrix<T>::kC;
    SellMatrix<T>      out(alloc);
    out.rows       = pat.rows;
    out.cols       = pat.cols;
    out.num_slices = (pat.rows + C - 1) / C;

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

    // 1. per-slice width = max length over the slice's permuted rows.
    out.slice_width.resize(out.num_slices);
    out.slice_ptr.resize(static_cast<crd::usize>(out.num_slices) + 1);
    out.slice_ptr[0] = 0;
    for (crd::u32 s = 0; s < out.num_slices; ++s)
    {
        crd::u32       w     = 0;
        const crd::u32 r_end = (s * C + C < pat.rows) ? (s * C + C) : pat.rows;
        for (crd::u32 sr = s * C; sr < r_end; ++sr)
        {
            const crd::u32 r   = perm[sr];
            const crd::u32 len = outer[r + 1] - outer[r];
            w = len > w ? len : w;
        }
        out.slice_width[s]   = w;
        out.slice_ptr[s + 1] = out.slice_ptr[s] + C * w;
    }

    // 2. fill column-major within each slice; pad slots stay 0 (val) / 0 (col).
    const crd::u32 total = out.slice_ptr[out.num_slices];
    out.vals.resize(total);     // value-initialised to 0 -> padding is 0
    out.col_idx.resize(total);  // 0 -> padding column 0
    for (crd::u32 s = 0; s < out.num_slices; ++s)
    {
        const crd::u32 base = out.slice_ptr[s];
        for (crd::u32 lane = 0; lane < C; ++lane)
        {
            const crd::u32 sr = s * C + lane;
            if (sr >= pat.rows)
            {
                break;  // padding rows: already 0
            }
            const crd::u32 r   = perm[sr];
            const crd::u32 rs  = outer[r];
            const crd::u32 len = outer[r + 1] - rs;
            for (crd::u32 w = 0; w < len; ++w)
            {
                const crd::u32 dst = base + w * C + lane;
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
template <typename T>
void sell_spmv_range(const SellMatrix<T>& a, T alpha, T beta, const T* xp, T* y, crd::u32 s_begin, crd::u32 s_end)
{
    constexpr crd::u32 C     = SellMatrix<T>::kC;
    const bool         bzero = detail::spmv_is_zero(beta);
    const bool         ident = a.perm_is_identity;  // skip perm[] indirection for uniform/banded
    const T*           vals  = a.vals.data();
    const crd::u32*    cols  = a.col_idx.data();
    const crd::u32*    perm  = a.perm.data();

    // Generic per-slice path: f32 via Vec8f; f64-without-AVX2 + complex via C
    // independent scalar accumulators. (f64-with-AVX2 uses the slice-pair path
    // below.) Factored into a lambda so the AVX2 branch needs no trailing
    // unreachable code.
    auto generic_path = [&]() {
        for (crd::u32 s = s_begin; s < s_end; ++s)
        {
            const crd::u32 base  = a.slice_ptr[s];
            const crd::u32 w_n   = a.slice_width[s];
            const crd::u32 r_end = (s * C + C < a.rows) ? C : (a.rows - s * C);

            T acc[C];
            if constexpr (std::is_same_v<T, crd::f32>)  // C == 8
            {
                crd::math::simd::Vec8f accv(0.0F);
                for (crd::u32 w = 0; w < w_n; ++w)
                {
                    const crd::u32 off = base + w * C;
                    crd::f32       xg[8];
                    for (crd::u32 lane = 0; lane < 8; ++lane)
                    {
                        xg[lane] = xp[cols[off + lane]];
                    }
                    accv = mul_add(crd::math::simd::Vec8f::load(&vals[off]), crd::math::simd::Vec8f::load(xg), accv);
                }
                accv.store(acc);
            }
            else  // f64 (no-AVX2 fallback) / complex
            {
                for (crd::u32 lane = 0; lane < C; ++lane)
                {
                    acc[lane] = T{};
                }
                for (crd::u32 w = 0; w < w_n; ++w)
                {
                    const crd::u32 off = base + w * C;
                    for (crd::u32 lane = 0; lane < C; ++lane)
                    {
                        acc[lane] = acc[lane] + vals[off + lane] * xp[cols[off + lane]];
                    }
                }
            }

            for (crd::u32 lane = 0; lane < r_end; ++lane)
            {
                const crd::u32 r = ident ? (s * C + lane) : perm[s * C + lane];  // de-permute (σ)
                y[r] = bzero ? (alpha * acc[lane]) : (alpha * acc[lane] + beta * y[r]);
            }
        }
    };

#if CRD_SIMD_HAS_AVX2
    if constexpr (std::is_same_v<T, crd::f64>)  // C == 4
    {
        // Write the C accumulator lanes of slice s into y (alpha/beta, NaN-safe).
        auto write_slice = [&](crd::u32 s, const crd::f64* acc) {
            const crd::u32 r_end = (s * C + C < a.rows) ? C : (a.rows - s * C);
            for (crd::u32 lane = 0; lane < r_end; ++lane)
            {
                const crd::u32 r = ident ? (s * C + lane) : perm[s * C + lane];  // de-permute (σ)
                y[r] = bzero ? (alpha * acc[lane]) : (alpha * acc[lane] + beta * y[r]);
            }
        };
        // One column step of a slice: acc += vals * gather(x). Two ops = two
        // roundings (D(sparse)-3) -- NOT _mm256_fmadd (single-rounded).
        auto step = [&](__m256d acc, crd::u32 off) -> __m256d {
            const __m256d v = _mm256_loadu_pd(vals + off);
            // Hardware gather: 4 contiguous u32 column indices -> 4 doubles of x
            // (scale 8). Avoids the stack store-forward of a manual set_pd gather.
            const __m128i idx = _mm_loadu_si128(reinterpret_cast<const __m128i*>(cols + off));
            const __m256d xv  = _mm256_i32gather_pd(xp, idx, 8);
            return _mm256_add_pd(acc, _mm256_mul_pd(v, xv));
        };
        constexpr crd::u32 kPf = 16U;  // prefetch distance (elements ahead)

        crd::u32 s = s_begin;
        for (; s + 1 < s_end; s += 2)  // slice-pair: two independent accumulator chains
        {
            const crd::u32 b0 = a.slice_ptr[s], w0 = a.slice_width[s];
            const crd::u32 b1 = a.slice_ptr[s + 1], w1 = a.slice_width[s + 1];
            __m256d        acc0 = _mm256_setzero_pd();
            __m256d        acc1 = _mm256_setzero_pd();
            const crd::u32 wm = w0 < w1 ? w0 : w1;
            for (crd::u32 w = 0; w < wm; ++w)
            {
                const crd::u32 o0 = b0 + w * C;
                const crd::u32 o1 = b1 + w * C;
                _mm_prefetch(reinterpret_cast<const char*>(cols + o0 + kPf), _MM_HINT_T0);
                _mm_prefetch(reinterpret_cast<const char*>(cols + o1 + kPf), _MM_HINT_T0);
                acc0 = step(acc0, o0);
                acc1 = step(acc1, o1);
            }
            for (crd::u32 w = wm; w < w0; ++w)
            {
                acc0 = step(acc0, b0 + w * C);
            }
            for (crd::u32 w = wm; w < w1; ++w)
            {
                acc1 = step(acc1, b1 + w * C);
            }
            crd::f64 a0[4];
            crd::f64 a1[4];
            _mm256_storeu_pd(a0, acc0);
            _mm256_storeu_pd(a1, acc1);
            write_slice(s, a0);
            write_slice(s + 1, a1);
        }
        for (; s < s_end; ++s)  // trailing odd slice
        {
            const crd::u32 b0 = a.slice_ptr[s], w0 = a.slice_width[s];
            __m256d        acc0 = _mm256_setzero_pd();
            for (crd::u32 w = 0; w < w0; ++w)
            {
                acc0 = step(acc0, b0 + w * C);
            }
            crd::f64 a0[4];
            _mm256_storeu_pd(a0, acc0);
            write_slice(s, a0);
        }
    }
    else
    {
        generic_path();  // f32 / complex under AVX2
    }
#else
    generic_path();  // no AVX2: all types
#endif
}
} // namespace detail

// y = alpha * A * x + beta * y  (SELL, Trans::None). Bit-exact with the CSR
// baseline: each lane accumulates its row left-to-right in column order via
// the two-rounded `acc + val*x` (D(sparse)-3). beta == 0 is NaN-safe. For f64
// the kernel is slice-pair AVX2 (gather + prefetch); f32 uses Vec8f; complex
// is scalar.
template <typename T>
void spmv_sell(T alpha, const SellMatrix<T>& a, crd::containers::ConstSpan<T> x, T beta, crd::containers::Span<T> y)
{
    CRD_ASSERT_MSG(x.size() == a.cols && y.size() == a.rows, "spmv_sell: x must be cols(), y must be rows()");
    detail::sell_spmv_range<T>(a, alpha, beta, x.data(), y.data(), 0, a.num_slices);
}

} // namespace crd::hesap::sparse
