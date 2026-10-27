// The hesap-sparse kernel levels (SIMD-DISPATCH, ADR-0134 §5, §7): every level the host runs gives the CSR spmv's
// bits for the f64 and f32 SELL-C-σ spmv, over slices that run as a pair and alone, empty and long rows, rows past the
// last full slice, a σ order and the identity, any slice range, both beta paths (beta == 0 never reads y) and IEEE
// special values. crd-hesap-sparse-kernels-at-<level> runs these [kernels] cases under each forced level.

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
using crd::math::KernelLevel;
using sp::SparseKernels;

namespace
{

bool runs_here(const KernelLevel<SparseKernels>& level)
{
    return crd::cpu_level_includes(crd::cpu_selection().detected, level.level);
}

template <typename T> bool same_bits(T a, T b)
{
    using Bits = std::conditional_t<sizeof(T) == 8, u64, u32>;
    return (std::isnan(a) && std::isnan(b)) || std::bit_cast<Bits>(a) == std::bit_cast<Bits>(b); // any NaN, any NaN
}

struct Lcg
{
    u64 s;
    u32 next()
    {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<u32>(s >> 33);
    }
};

// Rows of 0 to 5 entries, every fifth row empty, runs of three rows of 8 to 12 and every seventeenth row 25 long, so
// neighbouring slices are wide enough to run as a pair (with unequal widths) or too narrow, and a σ sort reorders the
// rows; the values are small fractions of both signs with a signed zero, a subnormal, both infinities and a NaN.
template <typename T>
sp::SparseMatrix<T, sp::SparseFormat::Csr> test_matrix(crd::memory::IAllocator* alloc, u32 rows, u32 cols, u64 seed)
{
    const T specials[] = {T(-0.0), std::numeric_limits<T>::denorm_min(), std::numeric_limits<T>::infinity(),
                          -std::numeric_limits<T>::infinity(), std::numeric_limits<T>::quiet_NaN()};
    Lcg g{seed};
    sp::TripletBuilder<T> b(alloc, rows, cols);
    usize at = 0;
    for (u32 r = 0; r < rows; ++r)
    {
        u32 len = r % 5U == 4U ? 0U : g.next() % 6U;
        len = r % 11U < 3U ? 8U + r % 5U : len;
        len = r % 17U == 3U ? 25U : len;
        for (u32 k = 0; k < len; ++k)
        {
            const u32 c = g.next() % cols;
            T v = static_cast<T>(static_cast<int>(g.next() % 19U) - 9) / T(7);
            if (++at % 41U == 0U)
            {
                v = specials[(at / 41U) % 5U];
            }
            b.add(r, c, v);
        }
    }
    return b.compress();
}

template <typename T> sp::SellSlices<T> slices_of(const sp::SellMatrix<T>& a)
{
    return {a.slice_ptr.data(),
            a.slice_width.data(),
            a.vals.data(),
            a.col_idx.data(),
            a.perm_is_identity ? nullptr : a.perm.data(),
            a.rows};
}

template <typename T>
void run_kernel(const SparseKernels& k, const sp::SellSlices<T>& a, T alpha, T beta, const T* x, T* y, u32 s_begin,
                u32 s_end)
{
    if constexpr (std::is_same_v<T, crd::f64>)
    {
        k.sell_spmv_f64(a, alpha, beta, x, y, s_begin, s_end);
    }
    else
    {
        k.sell_spmv_f32(a, alpha, beta, x, y, s_begin, s_end);
    }
}

struct Counts
{
    usize cases = 0;
    usize wrong = 0;   // a row of the range without the CSR spmv's bits
    usize touched = 0; // a row outside the range, or the guard past y, written
};

// Every slice range of one matrix and level at three alpha/beta pairs against the CSR spmv.
template <typename T>
void check_level(const SparseKernels& k, crd::memory::IAllocator* alloc, u32 rows, u32 sigma, Counts& counts)
{
    const u32 cols = rows + 3U;
    const auto csr = test_matrix<T>(alloc, rows, cols, 0x51ULL + rows * 977ULL + sigma);
    const sp::SellMatrix<T> sell = sp::to_sell(csr, alloc, sigma);
    const sp::SellSlices<T> slices = slices_of(sell);
    constexpr u32 height = sp::SellMatrix<T>::kC;

    crd::containers::Array<T> x(alloc);
    Lcg g{0x77ULL + rows};
    for (u32 i = 0; i < cols; ++i)
    {
        x.push_back(static_cast<T>(static_cast<int>(g.next() % 23U) - 11) / T(13));
    }
    x[cols / 2U] = T(-0.0);

    const T nan = std::numeric_limits<T>::quiet_NaN();
    const T guard = T(12345.5);
    const T pairs[][2] = {{T(1), T(0)}, {T(0.75), T(-1.25)}, {T(2), T(1)}};
    crd::containers::Array<T> y0(alloc);
    crd::containers::Array<T> want(alloc);
    crd::containers::Array<T> y(alloc);
    for (const auto& ab : pairs)
    {
        const bool beta_zero = ab[1] == T(0);
        y0.resize(rows + 1U);
        for (u32 r = 0; r < rows; ++r)
        {
            y0[r] = beta_zero ? nan : static_cast<T>(static_cast<int>(g.next() % 9U) - 4) / T(3); // NaN: never read
        }
        y0[rows] = guard;
        want = y0;
        sp::spmv<T>(ab[0], csr, sp::Trans::None, crd::containers::ConstSpan<T>{x.data(), cols}, ab[1],
                    crd::containers::Span<T>{want.data(), rows});

        const u32 n = sell.num_slices;
        const u32 ranges[][2] = {{0, n},
                                 {n > 0U ? 1U : 0U, n},
                                 {0, n > 0U ? n - 1U : 0U},
                                 {n / 2U, n / 2U + (n > 0U ? 1U : 0U)},
                                 {n / 3U, n - n / 3U},
                                 {n, n}};
        for (const auto& range : ranges)
        {
            ++counts.cases;
            y = y0;
            run_kernel<T>(k, slices, ab[0], ab[1], x.data(), y.data(), range[0], range[1]);
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
        INFO("level " << crd::cpu_level_name(level.level) << ", " << sizeof(T) * 8U << "-bit");
        Counts counts;
        for (const u32 rows : {0U, 1U, 3U, 4U, 7U, 8U, 9U, 12U, 16U, 17U, 33U, 70U, 129U, 300U})
        {
            for (const u32 sigma : {0U, 1U, 6U})
            {
                check_level<T>(*level.table, &alloc, rows, sigma, counts);
            }
        }
        CHECK(counts.cases == 14U * 3U * 3U * 6U);
        CHECK(counts.wrong == 0U);
        CHECK(counts.touched == 0U);
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
    }
    const bool x64 = crd::cpu_selection().arch == crd::CpuArch::X64;
    CHECK(sse2 == (x64 && CRD_TEST_KERNEL_LEVELS_TOP >= static_cast<int>(CpuLevel::Sse2)));
    CHECK(avx2 == (x64 && CRD_TEST_KERNEL_LEVELS_TOP >= static_cast<int>(CpuLevel::Avx2)));
}

TEST_CASE("sparse kernels: every level the host runs gives the CSR spmv bits (f64)", "[sparse][kernels]")
{
    check_every_level<crd::f64>();
}

TEST_CASE("sparse kernels: every level the host runs gives the CSR spmv bits (f32)", "[sparse][kernels]")
{
    check_every_level<crd::f32>();
}

TEST_CASE("sparse kernels: spmv_sell runs the selected level and gives the CSR bits", "[sparse][kernels]")
{
    crd::memory::TlsfAllocator alloc(8U << 20);
    const auto csr = test_matrix<crd::f64>(&alloc, 129U, 131U, 0x99ULL);
    const auto sell = sp::to_sell(csr, &alloc);
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
