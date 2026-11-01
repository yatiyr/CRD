// The hesap-sparse kernels of this archive, their known-answer self-check (HW-RESILIENCE.3) and the level the process
// runs (SIMD-DISPATCH).

#include <crd/core/cpu_features.hpp>
#include <crd/core/kernel_level.hpp>
#include <crd/hesap/sparse/sparse_dispatch.hpp>

#include <bit>
#include <limits>
#include <type_traits>

namespace crd::hesap::sparse
{
namespace
{

using crd::i64;
using crd::KernelLevel;
using crd::u16;
using crd::u32;
using crd::usize;

// CRD_KERNELS_SPARSE: the levels crd_add_cpu_kernels built (../CMakeLists.txt). Each needs its entry below.
constexpr int kLevelsWithBody = (1 << CRD_KERNEL_LEVEL_PORTABLE) | (1 << CRD_KERNEL_LEVEL_SSE2) |
                                (1 << CRD_KERNEL_LEVEL_AVX2) | (1 << CRD_KERNEL_LEVEL_AVX512);
static_assert((CRD_KERNELS_SPARSE & ~kLevelsWithBody) == 0, "a built level has no table entry here");
static_assert(CRD_KERNEL_BUILT(CRD_KERNELS_SPARSE, PORTABLE), "the portable level is always built");

constexpr KernelLevel<SparseKernels> kLevels[] = {
    {crd::CpuLevel::Portable, &kSparseKernelsPortable, {}},
#if CRD_KERNEL_BUILT(CRD_KERNELS_SPARSE, SSE2)
    {crd::CpuLevel::Sse2, &kSparseKernelsSse2, {}},
#endif
#if CRD_KERNEL_BUILT(CRD_KERNELS_SPARSE, AVX2)
    {crd::CpuLevel::Avx2, &kSparseKernelsAvx2, {}},
#endif
#if CRD_KERNEL_BUILT(CRD_KERNELS_SPARSE, AVX512)
    {crd::CpuLevel::Avx512, &kSparseKernelsAvx512, {}},
#endif
};

// ---- Known-answer self-check (HW-RESILIENCE.3) ---------------------------------------------------------------------

// One SELL-C-σ matrix per slice height C (4: f64 and both complex types; 8: f32), laid out as to_sell lays one out
// (crd/hesap/sparse/sell.hpp): two U32 slices wide enough to pair, two Delta16 slices that pair, a narrow Delta16 slice
// and a last Delta16 slice with an empty row and a pad row. Every slice but the fourth is shorter in some row than its
// width, so the masked step runs after the unmasked one, and the pairs share full columns with columns of each slice
// left after them. No row stores column 0, and x[0] is +infinity: a padded lane that read x[0] in place of a zero
// would give NaN. One gap of each matrix needs the high byte of its 16 bits.
constexpr u32 kSlices = 6U;
constexpr u32 kWidths[kSlices] = {9U, 10U, 9U, 8U, 5U, 3U};
constexpr bool kDelta16[kSlices] = {false, false, true, true, true, true};
constexpr u32 kColumns = 430U; // x's length

constexpr u32 stored_columns(bool delta_only) noexcept
{
    u32 n = 0;
    for (u32 s = 0; s < kSlices; ++s)
    {
        n += (!delta_only || kDelta16[s]) ? kWidths[s] : 0U;
    }
    return n;
}

// The values σ row `sr` of a C-high matrix stores (rows - 1 rows: the last slice has one pad row).
constexpr u32 row_length(u32 c, u32 sr) noexcept
{
    const u32 s = sr / c;
    const u32 lane = sr % c;
    if (s == 0U)
    {
        return kWidths[0] - (lane * 3U) % 5U;
    }
    if (s == 1U)
    {
        return kWidths[1] - (lane * 2U) % 4U;
    }
    if (s == 2U)
    {
        return kWidths[2] - lane % 7U;
    }
    if (s == 3U)
    {
        return kWidths[3];
    }
    if (s == 4U)
    {
        return kWidths[4] - lane % 3U;
    }
    return lane == 1U ? 0U : kWidths[5] - lane % 3U;
}

// Column w of σ row sr: a first column of at least 1, then gaps of 1 to 13 and one of 300.
constexpr u32 first_column(u32 sr) noexcept
{
    return 1U + (sr * 5U) % 11U;
}

constexpr u32 column_gap(u32 sr, u32 w) noexcept
{
    return sr % 5U == 2U && w == 4U ? 300U : 1U + (sr * 3U + w * 7U) % 13U;
}

constexpr u32 column_of(u32 sr, u32 w) noexcept
{
    u32 col = first_column(sr);
    for (u32 v = 1; v <= w; ++v)
    {
        col += column_gap(sr, v);
    }
    return col;
}

template <u32 C> struct SellLayout
{
    static constexpr u32 kRows = kSlices * C - 1U;
    static constexpr u32 kElements = C * stored_columns(false);
    static constexpr u32 kDeltas = C * stored_columns(true);

    u32 slice_ptr[kSlices + 1U];
    u32 slice_width[kSlices];
    u32 slice_full[kSlices];
    u32 row_len[kSlices * C];
    u32 col_idx[kElements - kDeltas];
    u32 delta_ptr[kSlices + 1U];
    u32 col_first[kSlices * C];
    u16 col_delta[kDeltas];
    u32 perm[kRows];
    u32 max_width;
};

template <u32 C> constexpr SellLayout<C> make_sell_layout() noexcept
{
    constexpr u32 rows = SellLayout<C>::kRows;
    SellLayout<C> l{};
    for (u32 sr = 0; sr < rows; ++sr)
    {
        l.perm[sr] = (sr * 7U + 3U) % rows;
    }
    for (u32 s = 0; s < kSlices; ++s)
    {
        l.slice_ptr[s + 1U] = l.slice_ptr[s] + C * kWidths[s];
        l.delta_ptr[s + 1U] = l.delta_ptr[s] + (kDelta16[s] ? C * kWidths[s] : 0U);
        const u32 col_base = kDelta16[s] ? l.delta_ptr[s] : l.slice_ptr[s] - l.delta_ptr[s];
        u32 width = 0;
        u32 full = ~u32{0};
        for (u32 lane = 0; lane < C; ++lane)
        {
            const u32 sr = s * C + lane;
            const u32 len = sr < rows ? row_length(C, sr) : 0U;
            l.row_len[sr] = len;
            width = len > width ? len : width;
            full = len < full ? len : full;
            for (u32 w = 0; w < len; ++w)
            {
                const u32 at = col_base + w * C + lane;
                if (!kDelta16[s])
                {
                    l.col_idx[at] = column_of(sr, w);
                }
                else if (w == 0U)
                {
                    l.col_first[sr] = first_column(sr);
                }
                else
                {
                    l.col_delta[at] = static_cast<u16>(column_gap(sr, w));
                }
            }
        }
        l.slice_width[s] = width;
        l.slice_full[s] = full;
        l.max_width = width > l.max_width ? width : l.max_width;
    }
    return l;
}

constexpr SellLayout<4U> kLayout4 = make_sell_layout<4U>();
constexpr SellLayout<8U> kLayout8 = make_sell_layout<8U>();

// The shapes the check relies on, as to_sell would store them.
template <u32 C> constexpr bool layout_holds(const SellLayout<C>& l) noexcept
{
    bool ok = l.max_width == 10U && l.slice_ptr[kSlices] == SellLayout<C>::kElements &&
              l.delta_ptr[kSlices] == SellLayout<C>::kDeltas && l.slice_full[kSlices - 1U] == 0U &&
              l.slice_full[0] != 0U && l.slice_full[2] != 0U && l.slice_full[0] < l.slice_full[1] &&
              l.slice_full[1] < l.slice_width[1] && l.slice_full[3] == l.slice_width[3];
    for (u32 s = 0; s < kSlices; ++s)
    {
        ok = ok && l.slice_width[s] == kWidths[s] && (s == 3U || l.slice_full[s] < l.slice_width[s]);
    }
    for (u32 sr = 0; sr < SellLayout<C>::kRows; ++sr)
    {
        const u32 len = l.row_len[sr];
        ok = ok && (len == 0U || column_of(sr, len - 1U) < kColumns);
    }
    return ok;
}
static_assert(layout_holds(kLayout4) && layout_holds(kLayout8), "the check's matrix lost a shape it runs");
static_assert(SellLayout<4U>::kRows % 7U != 0U && SellLayout<8U>::kRows % 7U != 0U, "perm must be a bijection");

template <typename T> constexpr T ratio(i64 m, i64 d) noexcept
{
    return static_cast<T>(m) / static_cast<T>(d);
}

// A nonzero ratio m / d, m in [-20, 21] and d odd.
template <typename T> constexpr T value_of(u32 a, u32 b, u32 part) noexcept
{
    const i64 m = static_cast<i64>((a * 13U + b * 29U + part * 17U) % 41U) - 20;
    return ratio<T>(m == 0 ? 21 : m, 3 + 2 * static_cast<i64>((a + b + part) % 4U));
}

// The spmv of one value type, P parts a value (1 real, 2 complex) at slice height C: y = alpha A x + beta y0 and
// alpha A x (beta 0, y never read), in the order the kernels declare (sparse_kernels.hpp): each row sums its columns
// in order from +0 with two roundings a step, then the scalar write. Every value is a non-dyadic ratio, so a level that
// fused a step would round differently.
template <typename T, u32 P, u32 C> struct SpmvCheck
{
    static constexpr u32 kRows = SellLayout<C>::kRows;
    static constexpr bool kPermuted = C == 4U; // the f32 matrix keeps the identity order (perm nullptr)

    T alpha[P];
    T beta[P];
    T vals[SellLayout<C>::kElements * P];
    T x[kColumns * P];
    T y0[kRows * P];
    T want_beta[kRows * P];
    T want_zero[kRows * P];
};

template <typename T, u32 P, u32 C> constexpr SpmvCheck<T, P, C> make_spmv_check(const SellLayout<C>& l) noexcept
{
    using Check = SpmvCheck<T, P, C>;
    Check k{};
    k.alpha[0] = ratio<T>(-7, 3);
    k.beta[0] = ratio<T>(4, 7);
    if constexpr (P == 2U)
    {
        k.alpha[1] = ratio<T>(5, 9);
        k.beta[1] = ratio<T>(-2, 5);
    }
    for (u32 part = 0; part < P; ++part)
    {
        k.x[part] = std::numeric_limits<T>::infinity();
        for (u32 c = 1; c < kColumns; ++c)
        {
            const i64 m = static_cast<i64>((c * 17U + part * 5U) % 37U) - 18;
            k.x[c * P + part] = ratio<T>(m == 0 ? 19 : m, 7 + 2 * static_cast<i64>(c % 3U));
        }
        for (u32 r = 0; r < Check::kRows; ++r)
        {
            const i64 m = static_cast<i64>((r * 11U + part * 3U) % 19U) - 9;
            k.y0[r * P + part] = ratio<T>(m == 0 ? 10 : m, 3 + 2 * static_cast<i64>(r % 3U));
        }
    }
    for (u32 sr = 0; sr < Check::kRows; ++sr)
    {
        const u32 s = sr / C;
        const u32 lane = sr % C;
        T sum[P] = {};
        for (u32 w = 0; w < l.row_len[sr]; ++w)
        {
            const u32 e = l.slice_ptr[s] + w * C + lane;
            const T* xv = k.x + static_cast<usize>(column_of(sr, w)) * P;
            for (u32 part = 0; part < P; ++part)
            {
                k.vals[e * P + part] = value_of<T>(sr, w, part);
            }
            const T* v = k.vals + static_cast<usize>(e) * P;
            if constexpr (P == 1U)
            {
                sum[0] = sum[0] + v[0] * xv[0];
            }
            else
            {
                sum[0] = sum[0] + (v[0] * xv[0] - v[1] * xv[1]);
                sum[1] = sum[1] + (v[0] * xv[1] + v[1] * xv[0]);
            }
        }
        const u32 r = Check::kPermuted ? l.perm[sr] : sr;
        const T* y = k.y0 + static_cast<usize>(r) * P;
        T* zero = k.want_zero + static_cast<usize>(r) * P;
        T* beta = k.want_beta + static_cast<usize>(r) * P;
        if constexpr (P == 1U)
        {
            zero[0] = k.alpha[0] * sum[0];
            beta[0] = k.alpha[0] * sum[0] + k.beta[0] * y[0];
        }
        else
        {
            zero[0] = k.alpha[0] * sum[0] - k.alpha[1] * sum[1];
            zero[1] = k.alpha[0] * sum[1] + k.alpha[1] * sum[0];
            beta[0] = zero[0] + (k.beta[0] * y[0] - k.beta[1] * y[1]);
            beta[1] = zero[1] + (k.beta[0] * y[1] + k.beta[1] * y[0]);
        }
    }
    return k;
}

constexpr SpmvCheck<crd::f64, 1U, 4U> kSpmvF64 = make_spmv_check<crd::f64, 1U>(kLayout4);
constexpr SpmvCheck<crd::f32, 1U, 8U> kSpmvF32 = make_spmv_check<crd::f32, 1U>(kLayout8);
constexpr SpmvCheck<crd::f64, 2U, 4U> kSpmvC64 = make_spmv_check<crd::f64, 2U>(kLayout4);
constexpr SpmvCheck<crd::f32, 2U, 4U> kSpmvC32 = make_spmv_check<crd::f32, 2U>(kLayout4);

// Each spmv runs three picks, its slices in two ranges [0, split) and [split, 6), so a range starts mid-matrix:
// - the resident policy over a U32-only range (S0 and S1 pair) and a range with both forms (S2 and S3 pair);
// - the streaming policy at pair width 2, beta 0 into NaN: S0 alone, S1 alone beside a Delta16 slice, then S2 with S3
//   and the narrow S4 with the last S5 as pairs;
// - scalar loads with no pairs, every slice alone.
struct SpmvCall
{
    SellPick pick;
    u32 split;
    bool beta;
};

constexpr SpmvCall kSpmvCalls[3] = {
    {{SellSweep::Resident, SellGather::Policy, 0U}, 2U, true},
    {{SellSweep::Streaming, SellGather::Policy, 2U}, 1U, false},
    {{SellSweep::Resident, SellGather::Loads, kSellNoPairs}, 3U, true},
};

// The names a failure reports: each spmv's three picks, then the SDDMM's dot and empty dot.
constexpr const char* kF64Names[3] = {"sell_spmv_f64 resident", "sell_spmv_f64 streaming", "sell_spmv_f64 loads"};
constexpr const char* kF32Names[3] = {"sell_spmv_f32 resident", "sell_spmv_f32 streaming", "sell_spmv_f32 loads"};
constexpr const char* kC64Names[3] = {"sell_spmv_c64 resident", "sell_spmv_c64 streaming", "sell_spmv_c64 loads"};
constexpr const char* kC32Names[3] = {"sell_spmv_c32 resident", "sell_spmv_c32 streaming", "sell_spmv_c32 loads"};
constexpr const char* kSddmmF64Names[2] = {"sddmm_rows_f64", "sddmm_rows_f64 empty dot"};
constexpr const char* kSddmmF32Names[2] = {"sddmm_rows_f32", "sddmm_rows_f32 empty dot"};

template <typename T> bool same_bits(const T* got, const T* want, usize n) noexcept
{
    using Bits = std::conditional_t<sizeof(T) == sizeof(crd::u64), crd::u64, crd::u32>;
    for (usize i = 0; i < n; ++i)
    {
        if (std::bit_cast<Bits>(got[i]) != std::bit_cast<Bits>(want[i]))
        {
            return false;
        }
    }
    return true;
}

template <typename T, u32 P, u32 C, typename Spmv>
const char* check_spmv(Spmv spmv, const SpmvCheck<T, P, C>& k, const SellLayout<C>& l,
                       const char* const (&names)[3]) noexcept
{
    using Check = SpmvCheck<T, P, C>;
    constexpr usize n = static_cast<usize>(Check::kRows) * P;
    const SellSlices<T> a{l.slice_ptr,  l.slice_width, l.slice_full, l.row_len,   k.vals,
                          l.col_idx,    l.delta_ptr,   l.col_first,  l.col_delta, Check::kPermuted ? l.perm : nullptr,
                          Check::kRows, kColumns,      l.max_width};
    for (u32 i = 0; i < 3U; ++i)
    {
        const SpmvCall& call = kSpmvCalls[i];
        T y[n];
        for (usize j = 0; j < n; ++j)
        {
            y[j] = call.beta ? k.y0[j] : std::numeric_limits<T>::quiet_NaN();
        }
        T beta[P] = {};
        if (call.beta)
        {
            for (u32 part = 0; part < P; ++part)
            {
                beta[part] = k.beta[part];
            }
        }
        const bool ran = spmv(a, k.alpha, beta, k.x, y, 0U, call.split, call.pick) &&
                         spmv(a, k.alpha, beta, k.x, y, call.split, kSlices, call.pick);
        if (!ran || !same_bits(y, call.beta ? k.want_beta : k.want_zero, n))
        {
            return names[i];
        }
    }
    return nullptr;
}

// One SDDMM: a 4-row mask (row 1 empty) over 5 Y rows, depth 23 so every loop runs (f64: two stripes of 8, one of 4
// and three columns; f32: two stripes of 8 and seven columns), each X and Y row padded with NaN past the depth; the
// answers in SddmmRows' order (sparse_kernels.hpp), then the same mask at depth 0, whose empty dots give alpha * +0.
constexpr u32 kSddmmRows = 4U;
constexpr u32 kSddmmYRows = 5U;
constexpr u32 kSddmmDepth = 23U;
constexpr u32 kSddmmLdx = 24U;
constexpr u32 kSddmmLdy = 26U;
constexpr u32 kSddmmOuter[kSddmmRows + 1U] = {0U, 3U, 3U, 5U, 9U};
constexpr u32 kSddmmInner[9] = {4U, 0U, 2U, 1U, 3U, 0U, 1U, 2U, 4U};
constexpr usize kSddmmNnz = 9U;

template <typename T> struct SddmmCheck
{
    T alpha;
    T x[kSddmmRows * kSddmmLdx];
    T y[kSddmmYRows * kSddmmLdy];
    T want[kSddmmNnz];
    T want_empty[kSddmmNnz];
};

template <typename T> constexpr T sddmm_dot(const T* xi, const T* yj) noexcept
{
    T acc{};
    u32 col = 0;
    if constexpr (std::is_same_v<T, crd::f64>)
    {
        T a0[4] = {};
        T a1[4] = {};
        for (; col + 8U <= kSddmmDepth; col += 8U)
        {
            for (u32 l = 0; l < 4U; ++l)
            {
                a0[l] = a0[l] + xi[col + l] * yj[col + l];
                a1[l] = a1[l] + xi[col + 4U + l] * yj[col + 4U + l];
            }
        }
        acc = ((a0[0] + a1[0]) + (a0[1] + a1[1])) + ((a0[2] + a1[2]) + (a0[3] + a1[3]));
        for (; col + 4U <= kSddmmDepth; col += 4U)
        {
            const T p[4] = {xi[col] * yj[col], xi[col + 1U] * yj[col + 1U], xi[col + 2U] * yj[col + 2U],
                            xi[col + 3U] * yj[col + 3U]};
            acc = acc + ((p[0] + p[1]) + (p[2] + p[3]));
        }
    }
    else
    {
        T a0[8] = {};
        for (; col + 8U <= kSddmmDepth; col += 8U)
        {
            for (u32 l = 0; l < 8U; ++l)
            {
                a0[l] = a0[l] + xi[col + l] * yj[col + l];
            }
        }
        acc = ((a0[0] + a0[1]) + (a0[2] + a0[3])) + ((a0[4] + a0[5]) + (a0[6] + a0[7]));
    }
    for (; col < kSddmmDepth; ++col)
    {
        acc = acc + xi[col] * yj[col];
    }
    return acc;
}

template <typename T> constexpr SddmmCheck<T> make_sddmm_check() noexcept
{
    SddmmCheck<T> k{};
    k.alpha = ratio<T>(-3, 5);
    for (u32 i = 0; i < kSddmmRows; ++i)
    {
        for (u32 c = 0; c < kSddmmLdx; ++c)
        {
            const i64 m = static_cast<i64>((i * 7U + c * 11U) % 23U) - 11;
            k.x[i * kSddmmLdx + c] = c < kSddmmDepth ? ratio<T>(m == 0 ? 12 : m, 3 + 2 * static_cast<i64>((i + c) % 5U))
                                                     : std::numeric_limits<T>::quiet_NaN();
        }
    }
    for (u32 j = 0; j < kSddmmYRows; ++j)
    {
        for (u32 c = 0; c < kSddmmLdy; ++c)
        {
            const i64 m = static_cast<i64>((j * 5U + c * 13U) % 29U) - 14;
            k.y[j * kSddmmLdy + c] = c < kSddmmDepth
                                         ? ratio<T>(m == 0 ? 15 : m, 5 + 2 * static_cast<i64>((j + 2U * c) % 3U))
                                         : std::numeric_limits<T>::quiet_NaN();
        }
    }
    for (u32 i = 0; i < kSddmmRows; ++i)
    {
        for (u32 e = kSddmmOuter[i]; e < kSddmmOuter[i + 1U]; ++e)
        {
            k.want[e] = k.alpha * sddmm_dot(k.x + i * kSddmmLdx, k.y + kSddmmInner[e] * kSddmmLdy);
            k.want_empty[e] = k.alpha * T{};
        }
    }
    return k;
}

constexpr SddmmCheck<crd::f64> kSddmmF64 = make_sddmm_check<crd::f64>();
constexpr SddmmCheck<crd::f32> kSddmmF32 = make_sddmm_check<crd::f32>();

template <typename T>
const char* check_sddmm(void (*rows)(const SddmmRows<T>& s, u32 i_begin, u32 i_end) noexcept, const SddmmCheck<T>& k,
                        const char* const (&names)[2]) noexcept
{
    for (u32 i = 0; i < 2U; ++i)
    {
        T out[kSddmmNnz];
        for (T& v : out)
        {
            v = std::numeric_limits<T>::quiet_NaN();
        }
        const SddmmRows<T> s{kSddmmOuter, kSddmmInner, k.x, k.y, out, kSddmmLdx, kSddmmLdy, i == 0U ? kSddmmDepth : 0U,
                             k.alpha};
        rows(s, 0U, 1U);
        rows(s, 1U, kSddmmRows);
        if (!same_bits(out, i == 0U ? k.want : k.want_empty, kSddmmNnz))
        {
            return names[i];
        }
    }
    return nullptr;
}

// The table's real and complex spmv entries in one call shape, alpha and beta as P parts.
template <typename T> struct RealCall
{
    bool (*entry)(const SellSlices<T>& a, T alpha, T beta, const T* x, T* y, u32 s_begin, u32 s_end,
                  SellPick pick) noexcept;
    bool operator()(const SellSlices<T>& a, const T* alpha, const T* beta, const T* x, T* y, u32 s_begin, u32 s_end,
                    SellPick pick) const noexcept
    {
        return entry(a, alpha[0], beta[0], x, y, s_begin, s_end, pick);
    }
};

template <typename T> struct ComplexCall
{
    bool (*entry)(const SellSlices<T>& a, const SellComplexScales<T>& scales, const T* x, T* y, u32 s_begin, u32 s_end,
                  SellPick pick) noexcept;
    bool operator()(const SellSlices<T>& a, const T* alpha, const T* beta, const T* x, T* y, u32 s_begin, u32 s_end,
                    SellPick pick) const noexcept
    {
        return entry(a, SellComplexScales<T>{alpha[0], alpha[1], beta[0], beta[1]}, x, y, s_begin, s_end, pick);
    }
};

void resolve_sparse() noexcept
{
    (void)sparse_kernels();
}

constinit crd::KernelFamily g_family{"hesap.sparse", &resolve_sparse, {}, nullptr};
const crd::KernelFamilyListing kListing{g_family};

} // namespace

const char* sparse_self_check(const SparseKernels& table) noexcept
{
    const char* failed = check_spmv(RealCall<crd::f64>{table.sell_spmv_f64}, kSpmvF64, kLayout4, kF64Names);
    if (failed == nullptr)
    {
        failed = check_spmv(RealCall<crd::f32>{table.sell_spmv_f32}, kSpmvF32, kLayout8, kF32Names);
    }
    if (failed == nullptr)
    {
        failed = check_spmv(ComplexCall<crd::f64>{table.sell_spmv_c64}, kSpmvC64, kLayout4, kC64Names);
    }
    if (failed == nullptr)
    {
        failed = check_spmv(ComplexCall<crd::f32>{table.sell_spmv_c32}, kSpmvC32, kLayout4, kC32Names);
    }
    if (failed == nullptr)
    {
        failed = check_sddmm(table.sddmm_rows_f64, kSddmmF64, kSddmmF64Names);
    }
    return failed != nullptr ? failed : check_sddmm(table.sddmm_rows_f32, kSddmmF32, kSddmmF32Names);
}

std::span<const KernelLevel<SparseKernels>> sparse_kernel_levels() noexcept
{
    return kLevels;
}

const KernelLevel<SparseKernels>& sparse_kernels() noexcept
{
    static const KernelLevel<SparseKernels>& selected =
        crd::process_checked_kernel_level<SparseKernels>(kLevels, &sparse_self_check, g_family);
    return selected;
}

crd::usize sparse_resident_bytes() noexcept
{
    const crd::CpuSelection& s = crd::cpu_selection();
    const crd::u32 l2 = s.features.caches.l2_bytes != 0U ? s.features.caches.l2_bytes : s.os_l2_bytes;
    return l2 != 0U ? crd::usize{l2} : (crd::usize{1} << 21);
}

crd::usize sparse_job_resident_bytes() noexcept
{
    const crd::u32 share = crd::cpu_selection().l2_share_bytes;
    return share != 0U ? crd::usize{share} : sparse_resident_bytes();
}

} // namespace crd::hesap::sparse
