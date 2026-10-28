// The hesap-sparse kernel levels (SIMD-DISPATCH, ADR-0134 §5, §7): every level the host runs gives the CSR spmv's
// bits for the f64, f32, Complex<f64> and Complex<f32> SELL-C-σ spmv, over slices that run as a pair and alone, empty
// and long rows, rows past the last full slice, a σ order and the identity, any slice range, both beta paths (beta == 0
// never reads y), IEEE special values in A, an infinite or NaN x[0] (padding never multiplies x), both sweeps, one
// with x declared wider than a gather's index reaches, and both column forms: u32 and Delta16, mixed in one matrix
// where a gap needs more than 16 bits. crd-hesap-sparse-kernels-at-<level> runs these [kernels] cases under each
// forced level.

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

bool runs_here(const KernelLevel<SparseKernels>& level)
{
    return crd::cpu_level_includes(crd::cpu_selection().detected, level.level);
}

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
            a.row_len.data(),
            sp::detail::sell_scalars(a.vals.data()),
            a.col_idx.data(),
            a.delta_ptr.data(),
            a.col_first.data(),
            a.col_delta.data(),
            a.perm_is_identity ? nullptr : a.perm.data(),
            a.rows,
            a.cols};
}

// The column forms to_sell is asked for.
inline constexpr sp::SellIndex kForms[] = {sp::SellIndex::U32, sp::SellIndex::Delta16};
inline constexpr usize kFormCount = 2;

template <typename T>
void run_kernel(const SparseKernels& k, const sp::SellSlices<Scalar<T>>& a, T alpha, T beta, const T* x, T* y,
                u32 s_begin, u32 s_end, sp::SellSweep sweep)
{
    if constexpr (std::is_same_v<T, crd::f64>)
    {
        k.sell_spmv_f64(a, alpha, beta, x, y, s_begin, s_end, sweep);
    }
    else if constexpr (std::is_same_v<T, crd::f32>)
    {
        k.sell_spmv_f32(a, alpha, beta, x, y, s_begin, s_end, sweep);
    }
    else
    {
        const sp::SellComplexScales<Scalar<T>> scales{alpha.re, alpha.im, beta.re, beta.im};
        const auto* xs = sp::detail::sell_scalars(x);
        auto* ys = sp::detail::sell_scalars(y);
        if constexpr (std::is_same_v<T, Complex<crd::f64>>)
        {
            k.sell_spmv_c64(a, scales, xs, ys, s_begin, s_end, sweep);
        }
        else
        {
            k.sell_spmv_c32(a, scales, xs, ys, s_begin, s_end, sweep);
        }
    }
}

struct Counts
{
    usize cases = 0;
    usize wrong = 0;   // a row of the range without the CSR spmv's bits
    usize touched = 0; // a row outside the range, or the guard past y, written
    usize layout = 0;  // a slice whose form or columns differ from the CSR rows
    usize mixed = 0;   // Delta16 matrices holding a u32 slice wider than 2 (a gap past 16 bits) too
};

// Each slice's form against the rule (Delta16 when asked, wider than 2 and every gap within 16 bits) and its columns,
// decoded from the form, against the CSR rows; a Delta16 slice's gaps are 0 at w = 0 and in the padding.
template <typename T>
void check_layout(const sp::SparseMatrix<T, sp::SparseFormat::Csr>& csr, const sp::SellMatrix<T>& sell,
                  sp::SellIndex form, Counts& counts)
{
    constexpr u32 height = sp::SellMatrix<T>::kC;
    const u32* outer = csr.pattern().outer_ptr.data();
    const u32* inner = csr.pattern().inner_idx.data();
    counts.layout += sell.delta_ptr.size() == sell.num_slices + 1U ? 0U : 1U;
    counts.layout += sell.col_idx.size() + sell.col_delta.size() == sell.vals.size() ? 0U : 1U;
    counts.layout += sell.col_first.size() == (sell.col_delta.empty() ? 0U : sell.row_len.size()) ? 0U : 1U;
    usize delta_slices = 0;
    usize u32_slices = 0;
    for (u32 s = 0; s < sell.num_slices; ++s)
    {
        const u32 width = sell.slice_width[s];
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
}

inline constexpr usize kScalePairs = 5;
inline constexpr usize kFirstColumns = 3;
inline constexpr usize kSweeps = 3; // resident, streaming, and streaming with x past a gather's 2^31 columns
inline constexpr usize kRanges = 6;

// Every slice range of one matrix, column form and level at five alpha/beta pairs and three x[0] against the CSR
// spmv; a wide matrix has kWideColumns columns.
template <typename T>
void check_level(const SparseKernels& k, crd::memory::IAllocator* alloc, u32 rows, u32 sigma, bool wide,
                 sp::SellIndex form, Counts& counts)
{
    using U = Scalar<T>;
    const u32 cols = wide ? kWideColumns : rows + 3U;
    const auto csr = test_matrix<T>(alloc, rows, cols, 0x51ULL + rows * 977ULL + sigma);
    const sp::SellMatrix<T> sell = sp::to_sell(csr, alloc, sigma, form);
    check_layout(csr, sell, form, counts);
    const sp::SellSlices<U> slices = slices_of(sell);
    constexpr u32 height = sp::SellMatrix<T>::kC;

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
            sp::SellSlices<U> swept = slices;
            swept.cols = c / kRanges == 2U ? 0x80000001U : slices.cols; // the scalar-load form of a gathering body
            const sp::SellSweep sweep = c / kRanges == 0U ? sp::SellSweep::Resident : sp::SellSweep::Streaming;
            ++counts.cases;
            y = y0;
            run_kernel<T>(k, swept, ab[0], ab[1], x.data(), y.data(), range[0], range[1], sweep);
            const u32 first = range[0] * height;
            const u32 last = range[1] * height < rows ? range[1] * height : rows;
            for (u32 sr = 0; sr < rows; ++sr)
            {
                const u32 r = sell.perm[sr];
                if (sr >= first && sr < last)
                {
                    counts.wrong += same_bits(y[r], want[r]) ? 0U : 1U;
                }
                else
                {
                    counts.touched += same_bits(y[r], y0[r]) ? 0U : 1U;
                }
            }
            counts.touched += same_bits(y[rows], guard) ? 0U : 1U;
        }
    }
}

template <typename T> void check_every_level()
{
    crd::memory::TlsfAllocator alloc(32U << 20);
    usize ran = 0;
    for (const KernelLevel<SparseKernels>& level : sp::sparse_kernel_levels())
    {
        if (!runs_here(level))
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
                    check_level<T>(*level.table, &alloc, rows, sigma, false, form, counts);
                }
            }
            for (const u32 rows : {9U, 70U, 300U})
            {
                for (const u32 sigma : {0U, 6U})
                {
                    check_level<T>(*level.table, &alloc, rows, sigma, true, form, counts);
                }
            }
        }
        CHECK(counts.cases == kFormCount * (14U * 3U + 3U * 2U) * kScalePairs * kFirstColumns * kRanges * kSweeps);
        CHECK(counts.wrong == 0U);
        CHECK(counts.touched == 0U);
        CHECK(counts.layout == 0U);
        CHECK(counts.mixed == 3U * 2U); // every wide Delta16 matrix holds both forms
    }
    CHECK(ran >= 1U); // portable runs everywhere
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
        CHECK(levels[i].table != nullptr);
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

    const usize elements = sell.vals.size();
    const usize words = sell.row_len.size() + sell.slice_ptr.size() + sell.slice_width.size() + sell.delta_ptr.size() +
                        sell.perm.size();
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
    const u32 l2 = crd::cpu_selection().features.caches.l2_bytes;
    CHECK(sp::sparse_resident_bytes() == (l2 != 0U ? usize{l2} : usize{1} << 21U)); // the sweep's and operators' cut
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
