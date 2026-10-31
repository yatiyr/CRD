// The hesap-sparse kernel levels (SIMD-DISPATCH, ADR-0134 §5, §7): every level the host runs gives the CSR spmv's
// bits for the f64, f32, Complex<f64> and Complex<f32> SELL-C-σ spmv, over slices that run as a pair and alone, empty
// and long rows, rows past the last full slice, a σ order and the identity, any slice range, both beta paths (beta == 0
// never reads y), IEEE special values in A, an infinite or NaN x[0] (padding never multiplies x), both sweeps, x
// declared wider than a complex f64 gather's index reaches and wider than every gather's, and both column forms: u32
// and Delta16, mixed in one matrix where a gap needs more than 16 bits. Every forced pick (SellPick: the scalar-load
// and the gather form, pair widths 1, 8 and 32 and none) gives the same bits, and a gather pick is refused, y
// untouched, where the level has no gather or x is too wide for it. The SDDMM rows of f64 and f32 give the bits of
// their declared order on every level at r 0 to 17 and past it, over empty rows, sub-ranges (guards around them) and
// IEEE special values in X and Y, and sddmm and sddmm_parallel give them too. crd-hesap-sparse-kernels-at-<level> runs
// these [kernels] cases and the [sddmm] suite under each forced level.

#include "kernel_levels.hpp"
#include "sparse_jobs_fixture.hpp"

#include <crd/core/cpu_features.hpp>
#include <crd/hesap/sparse/sparse.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <bit>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <limits>
#include <type_traits>

namespace sp = crd::hesap::sparse;
using crd::CpuLevel;
using crd::u32;
using crd::u64;
using crd::usize;
using crd::hesap::Complex;
using crd::math::KernelLevel;
using sp::SparseKernels;

namespace
{

using crd::test::kernel_level_runs_here;

template <typename T> inline constexpr bool kIsComplex = false;
template <typename U> inline constexpr bool kIsComplex<Complex<U>> = true;
template <typename T> using Scalar = sp::detail::SellScalarT<T>;

// A value of T from its parts (a real T keeps re).
template <typename T> T value(Scalar<T> re, Scalar<T> im)
{
    if constexpr (kIsComplex<T>)
    {
        return T{re, im};
    }
    else
    {
        (void)im;
        return re;
    }
}

template <typename U> bool same_scalar(U a, U b)
{
    using Bits = std::conditional_t<sizeof(U) == 8, u64, u32>;
    return (std::isnan(a) && std::isnan(b)) || std::bit_cast<Bits>(a) == std::bit_cast<Bits>(b); // any NaN, any NaN
}

template <typename T> bool same_bits(T a, T b)
{
    if constexpr (kIsComplex<T>)
    {
        return same_scalar(a.re, b.re) && same_scalar(a.im, b.im);
    }
    else
    {
        return same_scalar(a, b);
    }
}

struct Lcg
{
    u64 s;
    u32 next()
    {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<u32>(s >> 33);
    }
    // One of span evenly spaced values around 0, over den.
    template <typename U> U fraction(u32 span, U den)
    {
        return static_cast<U>(static_cast<int>(next() % span) - static_cast<int>(span / 2U)) / den;
    }
};

// The x of the wide matrices: past 2^17 columns.
inline constexpr u32 kWideColumns = 200003U;

// The columns of row r of a wide matrix, ascending: gaps of exactly 65535 (Delta16) and 65536 (the row's slice keeps
// u32), the first and the last column, empty rows, and walks of short and long gaps of 1 to 25 columns.
u32 wide_row(u32 r, Lcg& g, u32 (&c)[25])
{
    switch (r % 7U)
    {
        case 0U:
            return 0U;
        case 1U:
            c[0] = 0U;
            c[1] = 65535U;
            c[2] = 131070U;
            c[3] = 196605U;
            return 4U;
        case 2U:
            c[0] = 5U + r;
            c[1] = c[0] + 65536U;
            c[2] = kWideColumns - 1U;
            return 3U;
        case 3U:
            c[0] = kWideColumns - 1U;
            return 1U;
        default:
            break;
    }
    const u32 len = r % 7U == 6U ? 25U : 3U + g.next() % 10U;
    const u32 step = r % 7U == 4U ? 7U : 7000U;
    u32 at = g.next() % 1000U;
    for (u32 k = 0; k < len; ++k)
    {
        c[k] = at;
        at += 1U + g.next() % step;
    }
    return len;
}

// Rows of 0 to 5 entries, every fifth row empty, runs of three rows of 8 to 12 and every seventeenth row 25 long, so
// neighbouring slices are wide enough to run as a pair (with unequal widths) or too narrow, and a σ sort reorders the
// rows; a wide matrix (kWideColumns) takes wide_row's columns instead. The values are small fractions of both signs
// with a signed zero, a subnormal, both infinities and a NaN (in the real or the imaginary part of a complex value).
template <typename T>
sp::SparseMatrix<T, sp::SparseFormat::Csr> test_matrix(crd::memory::IAllocator* alloc, u32 rows, u32 cols, u64 seed)
{
    using U = Scalar<T>;
    const U specials[] = {U(-0.0), std::numeric_limits<U>::denorm_min(), std::numeric_limits<U>::infinity(),
                          -std::numeric_limits<U>::infinity(), std::numeric_limits<U>::quiet_NaN()};
    Lcg g{seed};
    sp::TripletBuilder<T> b(alloc, rows, cols);
    usize at = 0;
    const bool wide = cols == kWideColumns;
    u32 wide_cols[25];
    for (u32 r = 0; r < rows; ++r)
    {
        u32 len = r % 5U == 4U ? 0U : g.next() % 6U;
        len = r % 11U < 3U ? 8U + r % 5U : len;
        len = r % 17U == 3U ? 25U : len;
        len = wide ? wide_row(r, g, wide_cols) : len;
        for (u32 k = 0; k < len; ++k)
        {
            const u32 c = wide ? wide_cols[k] : g.next() % cols;
            U re = g.fraction<U>(19U, U(7));
            U im = g.fraction<U>(19U, U(7));
            if (++at % 41U == 0U)
            {
                ((at / 41U) % 2U == 0U ? re : im) = specials[(at / 41U) % 5U];
            }
            b.add(r, c, value<T>(re, im));
        }
    }
    return b.compress();
}

template <typename T> sp::SellSlices<Scalar<T>> slices_of(const sp::SellMatrix<T>& a)
{
    return {a.slice_ptr.data(),
            a.slice_width.data(),
            a.slice_full.data(),
            a.row_len.data(),
            sp::detail::sell_scalars(a.vals.data()),
            a.col_idx.data(),
            a.delta_ptr.data(),
            a.col_first.data(),
            a.col_delta.data(),
            a.perm_is_identity ? nullptr : a.perm.data(),
            a.rows,
            a.cols,
            a.max_width};
}

// The column forms to_sell is asked for.
inline constexpr sp::SellIndex kForms[] = {sp::SellIndex::U32, sp::SellIndex::Delta16};
inline constexpr usize kFormCount = 2;

template <typename T>
bool run_kernel(const SparseKernels& k, const sp::SellSlices<Scalar<T>>& a, T alpha, T beta, const T* x, T* y,
                u32 s_begin, u32 s_end, sp::SellPick pick)
{
    if constexpr (std::is_same_v<T, crd::f64>)
    {
        return k.sell_spmv_f64(a, alpha, beta, x, y, s_begin, s_end, pick);
    }
    else if constexpr (std::is_same_v<T, crd::f32>)
    {
        return k.sell_spmv_f32(a, alpha, beta, x, y, s_begin, s_end, pick);
    }
    else
    {
        const sp::SellComplexScales<Scalar<T>> scales{alpha.re, alpha.im, beta.re, beta.im};
        const auto* xs = sp::detail::sell_scalars(x);
        auto* ys = sp::detail::sell_scalars(y);
        if constexpr (std::is_same_v<T, Complex<crd::f64>>)
        {
            return k.sell_spmv_c64(a, scales, xs, ys, s_begin, s_end, pick);
        }
        else
        {
            return k.sell_spmv_c32(a, scales, xs, ys, s_begin, s_end, pick);
        }
    }
}

// The picks forced besides the level's own: each gather form at each pair width (0 is the policy's).
inline constexpr sp::SellGather kGathers[] = {sp::SellGather::Policy, sp::SellGather::Loads, sp::SellGather::Gather};
inline constexpr u32 kPairWidths[] = {0U, 1U, 8U, 32U, sp::kSellNoPairs};
inline constexpr usize kForcedPicks = 3U * 5U - 1U;
inline constexpr usize kForcedCalls = 2U * 2U; // two alpha/beta and x[0] cases, two ranges

// Whether `level` runs a gather pick of T over x of `cols` columns: AVX2 and AVX-512 gather, their signed index
// reaching 2^31 columns (2^30 for a complex f64 value, two indices).
template <typename T> bool gathers_at(CpuLevel level, u32 cols)
{
    const u32 reach = std::is_same_v<T, Complex<crd::f64>> ? 0x40000000U : 0x80000000U;
    return (level == CpuLevel::Avx2 || level == CpuLevel::Avx512) && cols <= reach;
}

struct Counts
{
    usize cases = 0;
    usize wrong = 0;   // a row of the range without the CSR spmv's bits
    usize touched = 0; // a row outside the range, or the guard past y, written
    usize layout = 0;  // a slice whose form or columns differ from the CSR rows
    usize mixed = 0;   // Delta16 matrices holding a u32 slice wider than 2 (a gap past 16 bits) too
    usize picks = 0;   // forced picks run (or refused)
    usize refused = 0; // a pick's answer other than gathers_at's (Gather) or true (the rest)
};

// Each slice's form against the rule (Delta16 when asked, wider than 2 and every gap within 16 bits) and its columns,
// decoded from the form, against the CSR rows; a Delta16 slice's gaps are 0 at w = 0 and in the padding. The forms
// change at most twice a σ window (to_sell orders a Delta16 build's rows by the form they allow first).
template <typename T>
void check_layout(const sp::SparseMatrix<T, sp::SparseFormat::Csr>& csr, const sp::SellMatrix<T>& sell,
                  sp::SellIndex form, u32 sigma, Counts& counts)
{
    constexpr u32 height = sp::SellMatrix<T>::kC;
    const u32* outer = csr.pattern().outer_ptr.data();
    const u32* inner = csr.pattern().inner_idx.data();
    counts.layout += sell.delta_ptr.size() == sell.num_slices + 1U ? 0U : 1U;
    counts.layout += sell.col_idx.size() + sell.col_delta.size() == sell.vals.size() ? 0U : 1U;
    counts.layout += sell.col_first.size() == (sell.col_delta.empty() ? 0U : sell.row_len.size()) ? 0U : 1U;
    usize delta_slices = 0;
    usize u32_slices = 0;
    u32 widest = 0;
    u32 changes = 0;
    for (u32 s = 0; s < sell.num_slices; ++s)
    {
        const u32 width = sell.slice_width[s];
        widest = width > widest ? width : widest;
        changes += s > 0U && sell.slice_is_delta16(s) != sell.slice_is_delta16(s - 1U) ? 1U : 0U;
        bool fits = form == sp::SellIndex::Delta16 && width > 2U;
        for (u32 lane = 0; lane < height && s * height + lane < sell.rows; ++lane)
        {
            const u32 r = sell.perm[s * height + lane];
            for (u32 k = outer[r] + 1U; k < outer[r + 1U]; ++k)
            {
                fits = fits && inner[k] - inner[k - 1U] <= 0xFFFFU;
            }
        }
        const bool delta = sell.slice_is_delta16(s);
        counts.layout += delta == fits ? 0U : 1U;
        delta_slices += delta ? 1U : 0U;
        u32_slices += !delta && width > 2U ? 1U : 0U;
        for (u32 lane = 0; lane < height; ++lane)
        {
            const u32 sr = s * height + lane;
            const u32 r = sr < sell.rows ? sell.perm[sr] : 0U;
            const u32 len = sell.row_len[sr];
            u32 col = delta ? sell.col_first[sr] : 0U;
            for (u32 w = 0; w < width; ++w)
            {
                const u32 at = w * height + lane;
                if (delta)
                {
                    const u32 gap = sell.col_delta[sell.delta_ptr[s] + at];
                    counts.layout += (w == 0U || w >= len) && gap != 0U ? 1U : 0U;
                    col += gap;
                }
                else
                {
                    col = sell.col_idx[sell.slice_ptr[s] - sell.delta_ptr[s] + at];
                }
                counts.layout += w < len && col != inner[outer[r] + w] ? 1U : 0U;
            }
        }
    }
    counts.mixed += form == sp::SellIndex::Delta16 && delta_slices > 0U && u32_slices > 0U ? 1U : 0U;
    counts.layout += sell.max_width == widest ? 0U : 1U; // the driver drops the pair test below it
    const u32 windows = sigma == 0U || sigma >= sell.rows ? 1U : (sell.rows + sigma - 1U) / sigma;
    counts.layout += changes <= 2U * windows ? 0U : 1U;
}

inline constexpr usize kScalePairs = 5;
inline constexpr usize kFirstColumns = 3;
// Resident, streaming, streaming with x past every gather's 2^31 columns, resident past a complex f64 gather's 2^30.
inline constexpr usize kSweeps = 4;
inline constexpr usize kRanges = 6;

// Slices [range) of y = y0 by the kernel's pick against want; a refused pick must leave y as it was.
template <typename T>
void check_call(const SparseKernels& k, const sp::SellMatrix<T>& sell, const sp::SellSlices<Scalar<T>>& swept,
                const T (&ab)[2], const T* x, const crd::containers::Array<T>& y0,
                const crd::containers::Array<T>& want, crd::containers::Array<T>& y, const u32 (&range)[2],
                sp::SellPick pick, bool ran_expected, Counts& counts)
{
    constexpr u32 height = sp::SellMatrix<T>::kC;
    const u32 rows = sell.rows;
    y = y0;
    const bool ran = run_kernel<T>(k, swept, ab[0], ab[1], x, y.data(), range[0], range[1], pick);
    counts.refused += ran == ran_expected ? 0U : 1U;
    const u32 first = range[0] * height;
    const u32 last = range[1] * height < rows ? range[1] * height : rows;
    for (u32 sr = 0; sr < rows; ++sr)
    {
        const u32 r = sell.perm[sr];
        if (ran && sr >= first && sr < last)
        {
            counts.wrong += same_bits(y[r], want[r]) ? 0U : 1U;
        }
        else
        {
            counts.touched += same_bits(y[r], y0[r]) ? 0U : 1U;
        }
    }
    counts.touched += same_bits(y[rows], y0[rows]) ? 0U : 1U;
}

// Every slice range of one matrix, column form and level at five alpha/beta pairs and three x[0] against the CSR
// spmv; a wide matrix has kWideColumns columns.
template <typename T>
void check_level(const SparseKernels& k, CpuLevel level, crd::memory::IAllocator* alloc, u32 rows, u32 sigma, bool wide,
                 sp::SellIndex form, Counts& counts)
{
    using U = Scalar<T>;
    const u32 cols = wide ? kWideColumns : rows + 3U;
    const auto csr = test_matrix<T>(alloc, rows, cols, 0x51ULL + rows * 977ULL + sigma);
    const sp::SellMatrix<T> sell = sp::to_sell(csr, alloc, sigma, form);
    check_layout(csr, sell, form, sigma, counts);
    const sp::SellSlices<U> slices = slices_of(sell);

    crd::containers::Array<T> x(alloc);
    Lcg g{0x77ULL + rows};
    for (u32 i = 0; i < cols; ++i)
    {
        x.push_back(value<T>(g.fraction<U>(23U, U(13)), g.fraction<U>(23U, U(11))));
    }
    x[cols / 2U] = value<T>(U(-0.0), U(-0.0));

    const U nan = std::numeric_limits<U>::quiet_NaN();
    const T guard = value<T>(U(12345.5), U(-6789.25));
    // Real types keep the real parts: beta 0, -1.25, 0, -0 and 1. Complex beta (0, 1) and (-0, 0): zero is both parts.
    const T pairs[kScalePairs][2] = {{value<T>(1, 0), value<T>(0, 0)},
                                     {value<T>(U(0.75), U(-0.5)), value<T>(U(-1.25), U(0.25))},
                                     {value<T>(2, 1), value<T>(0, 1)},
                                     {value<T>(U(-0.5), 2), value<T>(U(-0.0), 0)},
                                     {value<T>(U(1.5), 0), value<T>(1, -2)}};
    // Padding never reads x[0]: a CSR row that stores column 0 alone sees it.
    const T x0s[kFirstColumns] = {x[0], value<T>(std::numeric_limits<U>::infinity(), 0), value<T>(nan, 1)};
    crd::containers::Array<T> y0(alloc);
    crd::containers::Array<T> want(alloc);
    crd::containers::Array<T> y(alloc);
    for (usize v = 0; v < kScalePairs * kFirstColumns; ++v)
    {
        const auto& ab = pairs[v % kScalePairs];
        x[0] = x0s[v / kScalePairs];
        const bool beta_zero = sp::detail::spmv_is_zero(ab[1]);
        y0.resize(rows + 1U);
        for (u32 r = 0; r < rows; ++r)
        {
            y0[r] = beta_zero ? value<T>(nan, nan) : value<T>(g.fraction<U>(9U, U(3)), g.fraction<U>(9U, U(5)));
        }
        y0[rows] = guard;
        want = y0;
        sp::spmv<T>(ab[0], csr, sp::Trans::None, crd::containers::ConstSpan<T>{x.data(), cols}, ab[1],
                    crd::containers::Span<T>{want.data(), rows});

        const u32 n = sell.num_slices;
        const u32 ranges[kRanges][2] = {{0, n},
                                        {n > 0U ? 1U : 0U, n},
                                        {0, n > 0U ? n - 1U : 0U},
                                        {n / 2U, n / 2U + (n > 0U ? 1U : 0U)},
                                        {n / 3U, n - n / 3U},
                                        {n, n}};
        for (usize c = 0; c < kRanges * kSweeps; ++c)
        {
            const auto& range = ranges[c % kRanges];
            const usize sweep_case = c / kRanges;
            const u32 swept_cols[kSweeps] = {slices.cols, slices.cols, 0x80000001U, 0x40000001U};
            sp::SellSlices<U> swept = slices; // past a gather's reach: the scalar-load form of a gathering body
            swept.cols = swept_cols[sweep_case];
            const sp::SellSweep sweep =
                sweep_case == 1U || sweep_case == 2U ? sp::SellSweep::Streaming : sp::SellSweep::Resident;
            ++counts.cases;
            check_call<T>(k, sell, swept, ab, x.data(), y0, want, y, range, {sweep, sp::SellGather::Policy, 0U}, true,
                          counts);
            // The forced picks over the whole range and an inner one, at a plain and a special x[0] and beta.
            if ((v != 1U && v != kScalePairs * kFirstColumns - 1U) || (c % kRanges != 0U && c % kRanges != 4U))
            {
                continue;
            }
            for (const sp::SellGather gather : kGathers)
            {
                for (const u32 pair_width : kPairWidths)
                {
                    if (gather == sp::SellGather::Policy && pair_width == 0U)
                    {
                        continue;
                    }
                    ++counts.picks;
                    const bool runs = gather != sp::SellGather::Gather || gathers_at<T>(level, swept.cols);
                    check_call<T>(k, sell, swept, ab, x.data(), y0, want, y, range, {sweep, gather, pair_width}, runs,
                                  counts);
                }
            }
        }
    }
}

template <typename T> void check_every_level()
{
    crd::memory::TlsfAllocator alloc(32U << 20);
    usize ran = 0;
    for (const KernelLevel<SparseKernels>& level : sp::sparse_kernel_levels())
    {
        if (!kernel_level_runs_here(level))
        {
            continue;
        }
        ++ran;
        INFO("level " << crd::cpu_level_name(level.level) << ", " << sizeof(T) * 8U << "-bit"
                      << (kIsComplex<T> ? " complex" : ""));
        Counts counts;
        for (const sp::SellIndex form : kForms)
        {
            for (const u32 rows : {0U, 1U, 3U, 4U, 7U, 8U, 9U, 12U, 16U, 17U, 33U, 70U, 129U, 300U})
            {
                for (const u32 sigma : {0U, 1U, 6U})
                {
                    check_level<T>(*level.table, level.level, &alloc, rows, sigma, false, form, counts);
                }
            }
            for (const u32 rows : {9U, 70U, 300U})
            {
                for (const u32 sigma : {0U, 6U})
                {
                    check_level<T>(*level.table, level.level, &alloc, rows, sigma, true, form, counts);
                }
            }
        }
        const usize matrices = kFormCount * (14U * 3U + 3U * 2U);
        CHECK(counts.cases == matrices * kScalePairs * kFirstColumns * kRanges * kSweeps);
        CHECK(counts.picks == matrices * kForcedCalls * kSweeps * kForcedPicks);
        CHECK(counts.refused == 0U);
        CHECK(counts.wrong == 0U);
        CHECK(counts.touched == 0U);
        CHECK(counts.layout == 0U);
        CHECK(counts.mixed == 3U * 2U); // every wide Delta16 matrix holds both forms
    }
    CHECK(ran >= 1U); // portable runs everywhere
}

// An SDDMM dot in SddmmRows' order, one scalar operation at a time: f64 as two four-lane sums over stripes of 8, then
// stripes of 4 folded alone; f32 as one eight-lane sum over stripes of 8; then column by column.
template <typename U> U sddmm_oracle(const U* x, const U* y, u32 r)
{
    U lanes[8] = {};
    u32 col = 0;
    for (; col + 8U <= r; col += 8U)
    {
        for (u32 l = 0; l < 8U; ++l)
        {
            lanes[l] = lanes[l] + x[col + l] * y[col + l];
        }
    }
    U acc;
    if constexpr (std::is_same_v<U, crd::f64>)
    {
        U s[4];
        for (u32 l = 0; l < 4U; ++l)
        {
            s[l] = lanes[l] + lanes[l + 4U];
        }
        acc = (s[0] + s[1]) + (s[2] + s[3]);
        for (; col + 4U <= r; col += 4U)
        {
            U p[4];
            for (u32 l = 0; l < 4U; ++l)
            {
                p[l] = x[col + l] * y[col + l];
            }
            acc = acc + ((p[0] + p[1]) + (p[2] + p[3]));
        }
    }
    else
    {
        acc = ((lanes[0] + lanes[1]) + (lanes[2] + lanes[3])) + ((lanes[4] + lanes[5]) + (lanes[6] + lanes[7]));
    }
    for (; col < r; ++col)
    {
        acc = acc + x[col] * y[col];
    }
    return acc;
}

inline constexpr u32 kSddmmRows = 13U;
inline constexpr u32 kSddmmCols = 11U;
inline constexpr u32 kSddmmMaxR = 70U;
inline constexpr u32 kSddmmPad = 3U; // columns past r in each row of x and y, NaN: a dot never reads them
inline constexpr u32 kSddmmNnz = kSddmmRows * 7U;

// X and Y rows of fractions of both signs at two magnitudes, so the order of the sum shows in its bits; rows 1, 5 and
// 9 of X and row 2 of Y also hold a signed zero, a subnormal, an infinity, a NaN or the largest finite value at
// columns of the stripes and of the tail.
template <typename U> void sddmm_operands(Lcg& g, u32 r, U* x, U* y)
{
    const U specials[] = {U(-0.0),
                          std::numeric_limits<U>::denorm_min(),
                          std::numeric_limits<U>::infinity(),
                          -std::numeric_limits<U>::infinity(),
                          std::numeric_limits<U>::quiet_NaN(),
                          std::numeric_limits<U>::max()};
    const u32 ld = r + kSddmmPad;
    const auto fill = [&](U* m, u32 rows)
    {
        for (u32 i = 0; i < rows; ++i)
        {
            for (u32 c = 0; c < ld; ++c)
            {
                const U scale = g.next() % 3U == 0U ? U(1048576) : U(1);
                m[i * ld + c] = c < r ? g.fraction<U>(19U, U(7)) * scale : std::numeric_limits<U>::quiet_NaN();
            }
        }
    };
    fill(x, kSddmmRows);
    fill(y, kSddmmCols);
    if (r == 0U)
    {
        return;
    }
    for (const u32 i : {1U, 5U, 9U})
    {
        x[i * ld + (i * 7U) % r] = specials[(i + r) % 6U];
        x[i * ld + r - 1U] = specials[(i + r + 3U) % 6U];
    }
    y[2U * ld + r / 2U] = specials[r % 6U];
}

// Every level's SDDMM rows against sddmm_oracle at r 0 to 17 and past it, three row ranges (the whole mask, a middle
// one, an empty one) and two alphas, with a mask of empty rows and rows of up to 6 entries; every value slot outside
// the range, and a guard past the last, keeps its guard.
template <typename U> void check_sddmm_levels()
{
    using Rows = sp::SddmmRows<U>;
    u32 outer[kSddmmRows + 1U];
    u32 inner[kSddmmNnz];
    Lcg g{0x5DD3ULL};
    outer[0] = 0U;
    for (u32 i = 0; i < kSddmmRows; ++i)
    {
        const u32 len = i % 6U == 3U ? 0U : (i * 5U) % 7U;
        for (u32 k = 0; k < len; ++k)
        {
            inner[outer[i] + k] = g.next() % kSddmmCols;
        }
        outer[i + 1U] = outer[i] + len;
    }
    const u32 nnz = outer[kSddmmRows];
    const U guard = U(12345.5);
    const U alphas[] = {U(1), U(-1.5)};
    const u32 ranges[][2] = {{0U, kSddmmRows}, {2U, 9U}, {5U, 5U}};
    U x[kSddmmRows * (kSddmmMaxR + kSddmmPad)];
    U y[kSddmmCols * (kSddmmMaxR + kSddmmPad)];
    U out[kSddmmNnz + 1U];
    usize ran = 0;
    for (const KernelLevel<SparseKernels>& level : sp::sparse_kernel_levels())
    {
        if (!kernel_level_runs_here(level))
        {
            continue;
        }
        ++ran;
        INFO("level " << crd::cpu_level_name(level.level) << ", " << sizeof(U) * 8U << "-bit");
        usize cases = 0;
        usize wrong = 0;
        usize touched = 0;
        for (const u32 r :
             {0U, 1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U, 9U, 10U, 11U, 12U, 13U, 14U, 15U, 16U, 17U, 31U, 32U, 33U, 64U, 70U})
        {
            sddmm_operands<U>(g, r, x, y);
            const u32 ld = r + kSddmmPad;
            for (const U alpha : alphas)
            {
                for (const auto& range : ranges)
                {
                    ++cases;
                    for (u32 k = 0; k <= nnz; ++k)
                    {
                        out[k] = guard;
                    }
                    const Rows rows{outer, inner, x, y, out, ld, ld, r, alpha};
                    if constexpr (std::is_same_v<U, crd::f64>)
                    {
                        level.table->sddmm_rows_f64(rows, range[0], range[1]);
                    }
                    else
                    {
                        level.table->sddmm_rows_f32(rows, range[0], range[1]);
                    }
                    for (u32 i = 0; i < kSddmmRows; ++i)
                    {
                        const bool in = i >= range[0] && i < range[1];
                        for (u32 k = outer[i]; k < outer[i + 1U]; ++k)
                        {
                            const U want = in ? alpha * sddmm_oracle(x + i * ld, y + inner[k] * ld, r) : guard;
                            (in ? wrong : touched) += same_scalar(out[k], want) ? 0U : 1U;
                        }
                    }
                    touched += same_scalar(out[nnz], guard) ? 0U : 1U;
                }
            }
        }
        CHECK(cases == 23U * 2U * 3U);
        CHECK(wrong == 0U);
        CHECK(touched == 0U);
    }
    CHECK(ran >= 1U); // portable runs everywhere
}

// sddmm and sddmm_parallel run the selected level: the oracle's bits on a TripletBuilder mask.
template <typename U> void check_sddmm_public()
{
    crd::memory::TlsfAllocator alloc(8U << 20);
    constexpr u32 m = 37U;
    constexpr u32 n = 29U;
    constexpr u32 r = 45U;
    Lcg g{0x5DD5ULL};
    sp::TripletBuilder<U> b(&alloc, m, n);
    for (u32 i = 0; i < m; ++i)
    {
        for (u32 k = 0; k < i % 9U; ++k)
        {
            b.add(i, g.next() % n, U(1));
        }
    }
    const auto mask = b.compress();
    U x[m * r];
    U y[n * r];
    for (U& v : x)
    {
        v = g.fraction<U>(23U, U(9));
    }
    for (U& v : y)
    {
        v = g.fraction<U>(23U, U(9)) * U(4096);
    }
    const auto serial = sp::sddmm<U>(mask, x, r, y, r, r, U(0.75), &alloc);
    crd_hesap_sparse_tests::sparse_jobs_listener();
    const auto parallel = sp::sddmm_parallel<U>(mask, x, r, y, r, r, U(0.75), &alloc, 3U);
    const u32* outer = mask.pattern().outer_ptr.data();
    const u32* inner = mask.pattern().inner_idx.data();
    REQUIRE(serial.nnz() == mask.nnz());
    REQUIRE(parallel.nnz() == mask.nnz());
    usize wrong = 0;
    for (u32 i = 0; i < m; ++i)
    {
        for (u32 k = outer[i]; k < outer[i + 1U]; ++k)
        {
            const U want = U(0.75) * sddmm_oracle(x + i * r, y + inner[k] * r, r);
            wrong += same_scalar(serial.values().values[k], want) ? 0U : 1U;
            wrong += same_scalar(parallel.values().values[k], want) ? 0U : 1U;
        }
    }
    CHECK(wrong == 0U);
}

} // namespace

TEST_CASE("sparse kernels: the archive holds portable and each body level the build compiles", "[sparse][kernels]")
{
    const auto levels = sp::sparse_kernel_levels();
    REQUIRE(!levels.empty());
    CHECK(levels[0].level == CpuLevel::Portable);
    bool sse2 = false;
    bool avx2 = false;
    bool avx512 = false;
    for (usize i = 0; i < levels.size(); ++i)
    {
        REQUIRE(levels[i].table != nullptr);
        CHECK(levels[i].table->sddmm_rows_f64 != nullptr);
        CHECK(levels[i].table->sddmm_rows_f32 != nullptr);
        CHECK(static_cast<int>(levels[i].level) <= CRD_TEST_KERNEL_LEVELS_TOP);
        if (i > 0U)
        {
            CHECK(static_cast<int>(levels[i - 1U].level) < static_cast<int>(levels[i].level));
        }
        sse2 = sse2 || levels[i].level == CpuLevel::Sse2;
        avx2 = avx2 || levels[i].level == CpuLevel::Avx2;
        avx512 = avx512 || levels[i].level == CpuLevel::Avx512;
    }
    const bool x64 = crd::cpu_selection().arch == crd::CpuArch::X64;
    CHECK(sse2 == (x64 && CRD_TEST_KERNEL_LEVELS_TOP >= static_cast<int>(CpuLevel::Sse2)));
    CHECK(avx2 == (x64 && CRD_TEST_KERNEL_LEVELS_TOP >= static_cast<int>(CpuLevel::Avx2)));
    CHECK(avx512 == (x64 && CRD_TEST_KERNEL_LEVELS_TOP >= static_cast<int>(CpuLevel::Avx512)));
}

TEST_CASE("sparse kernels: every level the host runs gives the CSR spmv bits (f64)", "[sparse][kernels]")
{
    check_every_level<crd::f64>();
}

TEST_CASE("sparse kernels: every level the host runs gives the CSR spmv bits (f32)", "[sparse][kernels]")
{
    check_every_level<crd::f32>();
}

TEST_CASE("sparse kernels: every level the host runs gives the CSR spmv bits (c64)", "[sparse][kernels]")
{
    check_every_level<Complex<crd::f64>>();
}

TEST_CASE("sparse kernels: every level the host runs gives the CSR spmv bits (c32)", "[sparse][kernels]")
{
    check_every_level<Complex<crd::f32>>();
}

TEST_CASE("sparse kernels: to_sell records each sigma row's length, and the bytes a sweep judges", "[sparse][kernels]")
{
    crd::memory::TlsfAllocator alloc(8U << 20);
    const auto csr = test_matrix<crd::f64>(&alloc, 70U, 73U, 0x5EEDULL);
    const auto sell = sp::to_sell(csr, &alloc, 6U, sp::SellIndex::U32);
    REQUIRE(sell.row_len.size() == static_cast<usize>(sell.num_slices) * sp::SellMatrix<crd::f64>::kC);
    const u32* outer = csr.pattern().outer_ptr.data();
    usize wrong = 0;
    for (u32 sr = 0; sr < sell.row_len.size(); ++sr)
    {
        const u32 want = sr < 70U ? outer[sell.perm[sr] + 1U] - outer[sell.perm[sr]] : 0U;
        wrong += sell.row_len[sr] == want ? 0U : 1U;
    }
    CHECK(wrong == 0U);
    // Each slice's full columns: its shortest row, a pad row's 0 included.
    constexpr u32 height = sp::SellMatrix<crd::f64>::kC;
    REQUIRE(sell.slice_full.size() == sell.num_slices);
    usize wrong_full = 0;
    for (u32 s = 0; s < sell.num_slices; ++s)
    {
        u32 shortest = sell.row_len[s * height];
        for (u32 lane = 1; lane < height; ++lane)
        {
            const u32 len = sell.row_len[s * height + lane];
            shortest = len < shortest ? len : shortest;
        }
        wrong_full += sell.slice_full[s] == shortest ? 0U : 1U;
    }
    CHECK(wrong_full == 0U);
    CHECK(sell.slice_full[sell.num_slices - 1U] == 0U); // 70 rows: the last slice holds two pad rows

    const usize elements = sell.vals.size();
    const usize words = sell.row_len.size() + sell.slice_ptr.size() + sell.slice_width.size() + sell.slice_full.size() +
                        sell.delta_ptr.size() + sell.perm.size();
    CHECK(sell.range_bytes(0U, sell.num_slices) == elements * 12U + sell.row_len.size() * 4U);
    CHECK(sell.range_bytes(3U, 3U) == 0U);
    CHECK(sell.stored_bytes() == elements * 12U + words * 4U);

    // Delta16: 2 bytes a gap and 4 a σ row's first column in place of 4 bytes an element.
    const auto packed = sp::to_sell(csr, &alloc, 6U, sp::SellIndex::Delta16);
    const usize gaps = packed.delta_ptr[packed.num_slices];
    REQUIRE(gaps > 0U);
    REQUIRE(gaps < elements);
    CHECK(packed.col_delta.size() == gaps);
    CHECK(packed.range_bytes(0U, packed.num_slices) ==
          elements * 8U + (elements - gaps) * 4U + gaps * 2U + packed.row_len.size() * 8U);
    CHECK(packed.stored_bytes() ==
          elements * 8U + (elements - gaps) * 4U + gaps * 2U + (words + packed.row_len.size()) * 4U);
    const crd::CpuSelection& cpus = crd::cpu_selection();
    const u32 l2 = cpus.features.caches.l2_bytes != 0U ? cpus.features.caches.l2_bytes : cpus.os_l2_bytes;
    CHECK(sp::sparse_resident_bytes() == (l2 != 0U ? usize{l2} : usize{1} << 21U)); // the sweep's and operators' cut
    const u32 share = cpus.l2_share_bytes;
    CHECK(sp::sparse_job_resident_bytes() == (share != 0U ? usize{share} : sp::sparse_resident_bytes())); // a job's
}

TEST_CASE("sparse kernels: Auto stores Delta16 exactly where a whole-matrix spmv streams", "[sparse][kernels]")
{
    // A tridiagonal n by n matrix: 3 n elements of 12 bytes, 4 bytes a row length, x and y: 56 n bytes a spmv.
    const usize resident = sp::sparse_resident_bytes();
    const u32 fits = static_cast<u32>(resident / 56U) / 4U * 4U;
    crd::memory::TlsfAllocator alloc(usize{64} << 20);
    for (const u32 n : {fits, fits + 4U})
    {
        sp::TripletBuilder<crd::f64> b(&alloc, n, n);
        for (u32 r = 0; r < n; ++r)
        {
            for (u32 c = r > 0U ? r - 1U : 0U; c <= r + 1U && c < n; ++c)
            {
                b.add(r, c, 1.0);
            }
        }
        const auto sell = sp::to_sell(b.compress(), &alloc);
        INFO("n " << n << ", resident " << resident);
        CHECK((sell.delta_ptr[sell.num_slices] != 0U) == (n != fits));
    }
}

TEST_CASE("sparse kernels: a Delta16 build runs the rows its form holds as one run of slices", "[sparse][kernels]")
{
    // Rows of four values alternate between gaps of 1 (Delta16 holds them) and gaps of 70,000 (it does not): in row
    // order every slice mixes both and stays U32, so to_sell orders the rows the form holds after the others.
    constexpr u32 n = 4096U;
    constexpr u32 cols = 4U * 70000U;
    crd::memory::TlsfAllocator alloc(usize{64} << 20);
    sp::TripletBuilder<crd::f64> b(&alloc, n, cols);
    for (u32 r = 0; r < n; ++r)
    {
        for (u32 k = 0; k < 4U; ++k)
        {
            b.add(r, r % 2U == 0U ? r + k : k * 70000U + r % 7U, 1.0 + static_cast<crd::f64>((r + k) % 5U));
        }
    }
    const auto csr = b.compress();
    const auto packed = sp::to_sell(csr, &alloc, 0U, sp::SellIndex::Delta16);
    const auto plain = sp::to_sell(csr, &alloc, 0U, sp::SellIndex::U32);
    constexpr u32 height = sp::SellMatrix<crd::f64>::kC;
    u32 delta_slices = 0;
    u32 changes = 0;
    for (u32 s = 0; s < packed.num_slices; ++s)
    {
        delta_slices += packed.slice_is_delta16(s) ? 1U : 0U;
        changes += s > 0U && packed.slice_is_delta16(s) != packed.slice_is_delta16(s - 1U) ? 1U : 0U;
    }
    CHECK(delta_slices == n / 2U / height);
    CHECK(changes == 1U);
    CHECK(plain.perm_is_identity);
    crd::containers::Array<crd::f64> x(&alloc);
    for (u32 i = 0; i < cols; ++i)
    {
        x.push_back(static_cast<crd::f64>(i % 11U) - 4.5);
    }
    crd::containers::Array<crd::f64> want(&alloc);
    crd::containers::Array<crd::f64> got(&alloc);
    want.resize(n);
    got.resize(n);
    const crd::containers::ConstSpan<crd::f64> xs{x.data(), x.size()};
    sp::spmv<crd::f64>(1.0, csr, sp::Trans::None, xs, 0.0, crd::containers::Span<crd::f64>{want.data(), n});
    sp::spmv_sell<crd::f64>(1.0, packed, xs, 0.0, crd::containers::Span<crd::f64>{got.data(), n});
    usize wrong = 0;
    for (u32 r = 0; r < n; ++r)
    {
        wrong += same_bits(got[r], want[r]) ? 0U : 1U;
    }
    CHECK(wrong == 0U);
}

TEST_CASE("sparse kernels: spmv_sell runs the selected level and gives the CSR bits", "[sparse][kernels]")
{
    crd::memory::TlsfAllocator alloc(8U << 20);
    const auto csr = test_matrix<crd::f64>(&alloc, 129U, 131U, 0x99ULL);
    for (const sp::SellIndex form : kForms)
    {
        const auto sell = sp::to_sell(csr, &alloc, 0U, form);
        REQUIRE(!sell.perm_is_identity);
        crd::containers::Array<crd::f64> x(&alloc);
        for (u32 i = 0; i < 131U; ++i)
        {
            x.push_back(static_cast<crd::f64>(i % 7U) - 2.5);
        }
        crd::containers::Array<crd::f64> want(&alloc);
        crd::containers::Array<crd::f64> got(&alloc);
        want.resize(129U);
        got.resize(129U);
        for (u32 r = 0; r < 129U; ++r)
        {
            want[r] = 0.5 * static_cast<crd::f64>(r);
            got[r] = want[r];
        }
        const crd::containers::ConstSpan<crd::f64> xs{x.data(), x.size()};
        sp::spmv<crd::f64>(1.5, csr, sp::Trans::None, xs, -0.5, crd::containers::Span<crd::f64>{want.data(), 129U});
        sp::spmv_sell<crd::f64>(1.5, sell, xs, -0.5, crd::containers::Span<crd::f64>{got.data(), 129U});
        usize wrong = 0;
        for (u32 r = 0; r < 129U; ++r)
        {
            wrong += same_bits(got[r], want[r]) ? 0U : 1U;
        }
        CHECK(wrong == 0U);
    }
}

TEST_CASE("sparse kernels: the process runs the level its CPU selection resolves", "[sparse][kernels]")
{
    const crd::CpuSelection& s = crd::cpu_selection();
    const auto levels = sp::sparse_kernel_levels();
    const KernelLevel<SparseKernels>& chosen = sp::sparse_kernels();
    INFO("selected " << crd::cpu_level_name(s.selected) << ", running " << crd::cpu_level_name(chosen.level));
    CHECK(&chosen == &crd::math::select_kernel_level<SparseKernels>(levels, s.selected, s.kernel_features,
                                                                    crd::math::kernel_level_cap()));
    CHECK(crd::cpu_level_includes(s.selected, chosen.level));
    CHECK(crd::cpu_level_includes(crd::math::kernel_level_cap(), chosen.level));
}

TEST_CASE("sparse kernels: every level the host runs gives the SDDMM order's bits (f64)", "[sparse][kernels]")
{
    check_sddmm_levels<crd::f64>();
}

TEST_CASE("sparse kernels: every level the host runs gives the SDDMM order's bits (f32)", "[sparse][kernels]")
{
    check_sddmm_levels<crd::f32>();
}

TEST_CASE("sparse kernels: sddmm and sddmm_parallel run the selected level", "[sparse][kernels]")
{
    check_sddmm_public<crd::f64>();
    check_sddmm_public<crd::f32>();
}
