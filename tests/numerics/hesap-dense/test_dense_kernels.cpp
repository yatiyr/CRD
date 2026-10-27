// The hesap-dense kernel levels (SIMD-DISPATCH, ADR-0134 §5, §7): every level the host runs gives the bits of its
// declared arithmetic for the f64 and f32 GEMM blocks (each product element summed in depth order from +0, rounding
// each multiply and add on portable and sse2, by single-rounded fused multiply-add from avx2) and the levels agree
// within the inner-product bound. The blocks cover partial and full tiles, depth 0 to 33, an A row stride wider than
// the depth, every update (add at alpha 1 and otherwise, subtract, assign), both C layouts and lower blocks whose
// diagonal crosses, skips or covers tiles; panel padding and A columns past the depth hold NaN, which no written
// element may reach. crd-hesap-dense-kernels-at-<level> runs these [kernels] cases under each forced level.

#include "hesap_jobs_fixture.hpp"

#include <crd/core/cpu_features.hpp>
#include <crd/hesap/dense/blas3.hpp>
#include <crd/hesap/dense/dense_dispatch.hpp>
#include <crd/hesap/dense/detail/syrk_microkernel.hpp>
#include <crd/hesap/dense/matrix.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <bit>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <limits>
#include <type_traits>

namespace dn = crd::hesap::dense;
using crd::CpuLevel;
using crd::u32;
using crd::u64;
using crd::usize;
using crd::math::KernelLevel;
using dn::DenseKernels;
using dn::GemmBlock;
using dn::GemmUpdate;

namespace
{

bool runs_here(const KernelLevel<DenseKernels>& level)
{
    return crd::cpu_level_includes(crd::cpu_selection().detected, level.level);
}

// The levels from avx2 up sum by fused multiply-add (dense_kernels.hpp).
bool sums_by_fma(CpuLevel level)
{
    return crd::cpu_level_includes(level, CpuLevel::Avx2);
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
    template <typename T> T value() { return static_cast<T>(static_cast<int>(next() % 19U) - 9) / T(7); }
};

template <typename T> void run_block(const DenseKernels& k, const GemmBlock<T>& g)
{
    if constexpr (std::is_same_v<T, crd::f64>)
    {
        k.gemm_block_f64(g);
    }
    else
    {
        k.gemm_block_f32(g);
    }
}

// Element (i, j) of the block's product in the declared arithmetic.
template <typename T> T product(const GemmBlock<T>& g, usize i, usize j, bool fma)
{
    constexpr usize nr = dn::detail::GemmTraits<T>::NR;
    const T* a = g.a + i * g.lda;
    const T* b = g.b + (j / nr) * g.depth * nr + j % nr;
    T acc = T(0);
    for (usize p = 0; p < g.depth; ++p)
    {
        acc = fma ? std::fma(a[p], b[p * nr], acc) : acc + a[p] * b[p * nr];
    }
    return acc;
}

template <typename T> T updated(const GemmBlock<T>& g, T c, T p)
{
    if (g.update == GemmUpdate::Assign)
    {
        return p;
    }
    if (g.update == GemmUpdate::Subtract)
    {
        return c - p;
    }
    return g.alpha == T(1) ? c + p : c + g.alpha * p;
}

struct Counts
{
    usize cases = 0;
    usize wrong = 0;   // a written element without the declared bits
    usize touched = 0; // an element outside the block or above a lower block's diagonal changed
    usize apart = 0;   // a product further from the portable level's than the inner-product bound
};

struct Shape
{
    usize rows, cols, depth, lda, row0, col0;
    bool lower;
};

template <typename T> struct Buffers
{
    crd::containers::Array<T> a, b, c0, c, portable;
    explicit Buffers(crd::memory::IAllocator* alloc) : a(alloc), b(alloc), c0(alloc), c(alloc), portable(alloc) {}
};

// One shape at every update and layout on one level.
template <typename T>
void check_shape(const DenseKernels& k, CpuLevel level, const DenseKernels& portable_k, const Shape& s, Buffers<T>& buf,
                 Lcg& g, Counts& counts)
{
    constexpr usize mr = dn::detail::GemmTraits<T>::MR;
    constexpr usize nr = dn::detail::GemmTraits<T>::NR;
    const T nan = std::numeric_limits<T>::quiet_NaN();
    const usize panels_m = (s.rows + mr - 1) / mr;
    const usize panels_n = (s.cols + nr - 1) / nr;
    buf.a.resize(panels_m * mr * s.lda + 1U);
    buf.b.resize(panels_n * nr * s.depth + 1U);
    for (usize r = 0; r < panels_m * mr; ++r)
    {
        for (usize p = 0; p < s.lda; ++p)
        {
            buf.a[r * s.lda + p] = (r < s.rows && p < s.depth) ? g.value<T>() : nan;
        }
    }
    for (usize pn = 0; pn < panels_n; ++pn)
    {
        for (usize p = 0; p < s.depth; ++p)
        {
            for (usize j = 0; j < nr; ++j)
            {
                buf.b[(pn * s.depth + p) * nr + j] = pn * nr + j < s.cols ? g.value<T>() : nan;
            }
        }
    }
    const usize ldc = (s.rows > s.cols ? s.rows : s.cols) + 3U;
    buf.c0.resize(ldc * ldc);
    for (usize e = 0; e < ldc * ldc; ++e)
    {
        buf.c0[e] = g.value<T>();
    }
    const bool fma = sums_by_fma(level);
    const T eps = std::numeric_limits<T>::epsilon();
    const struct
    {
        GemmUpdate update;
        T alpha;
    } updates[] = {
        {GemmUpdate::Add, T(1)}, {GemmUpdate::Add, T(0.75)}, {GemmUpdate::Subtract, T(0)}, {GemmUpdate::Assign, T(0)}};
    for (const auto& u : updates)
    {
        for (const bool col_major : {false, true})
        {
            ++counts.cases;
            buf.c = buf.c0;
            const GemmBlock<T> blk{.a = buf.a.data(),
                                   .b = buf.b.data(),
                                   .c = buf.c.data(),
                                   .lda = s.lda,
                                   .ldc = ldc,
                                   .rows = s.rows,
                                   .cols = s.cols,
                                   .depth = s.depth,
                                   .row0 = s.row0,
                                   .col0 = s.col0,
                                   .alpha = u.alpha,
                                   .update = u.update,
                                   .col_major = col_major,
                                   .lower = s.lower};
            run_block(k, blk);
            buf.portable = buf.c0;
            GemmBlock<T> ref = blk;
            ref.c = buf.portable.data();
            ref.update = GemmUpdate::Assign;
            run_block(portable_k, ref);
            for (usize i = 0; i < ldc; ++i)
            {
                for (usize j = 0; j < ldc; ++j)
                {
                    const usize at = col_major ? j * ldc + i : i * ldc + j;
                    const bool written = i < s.rows && j < s.cols && (!s.lower || s.row0 + i >= s.col0 + j);
                    if (!written)
                    {
                        counts.touched += same_bits(buf.c[at], buf.c0[at]) ? 0U : 1U;
                        continue;
                    }
                    const T p = product(blk, i, j, fma);
                    counts.wrong += same_bits(buf.c[at], updated(blk, buf.c0[at], p)) ? 0U : 1U;
                    T bound = T(0);
                    for (usize q = 0; q < s.depth; ++q)
                    {
                        bound += std::fabs(buf.a[i * s.lda + q] * buf.b[((j / nr) * s.depth + q) * nr + j % nr]);
                    }
                    counts.apart += std::fabs(p - buf.portable[at]) <= T(s.depth) * eps * bound ? 0U : 1U;
                }
            }
        }
    }
}

template <typename T> void check_every_level()
{
    constexpr usize mr = dn::detail::GemmTraits<T>::MR;
    constexpr usize nr = dn::detail::GemmTraits<T>::NR;
    crd::memory::TlsfAllocator alloc(16U << 20);
    const auto levels = dn::dense_kernel_levels();
    usize ran = 0;
    for (const KernelLevel<DenseKernels>& level : levels)
    {
        if (!runs_here(level))
        {
            continue;
        }
        ++ran;
        INFO("level " << crd::cpu_level_name(level.level) << ", " << sizeof(T) * 8U << "-bit");
        Buffers<T> buf(&alloc);
        Lcg g{0x5EEDULL + static_cast<u64>(level.level)};
        Counts counts;
        usize shapes = 0;
        const usize origins[][2] = {{0, 0}, {5, 0}, {0, 5}, {11, 3}};
        for (const usize rows : {usize{1}, mr - 1U, mr, mr + 1U, 2U * mr + 3U})
        {
            for (const usize cols : {usize{1}, nr - 1U, nr, nr + 1U, 2U * nr + 5U})
            {
                for (const usize depth : {usize{0}, usize{1}, usize{7}, usize{8}, usize{33}})
                {
                    for (const usize wider : {usize{0}, usize{3}})
                    {
                        ++shapes;
                        check_shape<T>(*level.table, level.level, *levels[0].table,
                                       Shape{rows, cols, depth, depth + wider, 0, 0, false}, buf, g, counts);
                        for (const auto& o : origins)
                        {
                            check_shape<T>(*level.table, level.level, *levels[0].table,
                                           Shape{rows, cols, depth, depth + wider, o[0], o[1], true}, buf, g, counts);
                        }
                    }
                }
            }
        }
        CHECK(counts.cases == shapes * 5U * 4U * 2U);
        CHECK(counts.wrong == 0U);
        CHECK(counts.touched == 0U);
        CHECK(counts.apart == 0U);
    }
    CHECK(ran >= 1U); // portable runs everywhere
}

} // namespace

TEST_CASE("dense kernels: the archive holds portable and each body level the build compiles", "[dense][kernels]")
{
    const auto levels = dn::dense_kernel_levels();
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
            CHECK(levels[i - 1U].table != levels[i].table); // bit-identical levels still run their own body
        }
        sse2 = sse2 || levels[i].level == CpuLevel::Sse2;
        avx2 = avx2 || levels[i].level == CpuLevel::Avx2;
    }
    const bool x64 = crd::cpu_selection().arch == crd::CpuArch::X64;
    CHECK(sse2 == (x64 && CRD_TEST_KERNEL_LEVELS_TOP >= static_cast<int>(CpuLevel::Sse2)));
    CHECK(avx2 == (x64 && CRD_TEST_KERNEL_LEVELS_TOP >= static_cast<int>(CpuLevel::Avx2)));
}

TEST_CASE("dense kernels: every level the host runs gives its declared GEMM block bits (f64)", "[dense][kernels]")
{
    check_every_level<crd::f64>();
}

TEST_CASE("dense kernels: every level the host runs gives its declared GEMM block bits (f32)", "[dense][kernels]")
{
    check_every_level<crd::f32>();
}

// m = 13 takes gemm's packed path; m = 60 (ten row panels) gemm_parallel's small-GEMM path, whose three workers each
// run their panels as one block.
TEST_CASE("dense kernels: gemm, the small-GEMM path and the lower syrk run the selected level", "[dense][kernels]")
{
    crd_hesap_dense_tests::hesap_jobs_listener();
    crd::memory::TlsfAllocator alloc(8U << 20);
    const bool fma = sums_by_fma(dn::dense_kernels().level);
    constexpr usize n = 11;
    constexpr usize k = 9;
    Lcg g{0xD3ULL};
    usize wrong = 0;
    for (const usize m : {usize{13}, usize{60}})
    {
        dn::Matrix<crd::f64> a(&alloc, m, k);
        dn::Matrix<crd::f64> b(&alloc, k, n);
        dn::Matrix<crd::f64> c(&alloc, m, n);
        dn::Matrix<crd::f64> s(&alloc, m, m);
        dn::Matrix<crd::f64> s0(&alloc, m, m);
        for (usize i = 0; i < m; ++i)
        {
            for (usize p = 0; p < k; ++p)
            {
                a(i, p) = g.value<crd::f64>();
            }
        }
        for (usize p = 0; p < k; ++p)
        {
            for (usize j = 0; j < n; ++j)
            {
                b(p, j) = g.value<crd::f64>();
            }
        }
        for (usize i = 0; i < m; ++i)
        {
            for (usize j = 0; j < m; ++j)
            {
                s(i, j) = g.value<crd::f64>();
                s0(i, j) = s(i, j);
            }
        }
        if (m == 13)
        {
            dn::gemm<crd::f64, dn::Layout::RowMajor>(1.0, a, b, 0.0, c);
        }
        else
        {
            dn::gemm_parallel<crd::f64, dn::Layout::RowMajor>(3, 1.0, a, b, 0.0, c);
        }
        dn::detail::syrk_lower_minus<crd::f64, dn::Layout::RowMajor>(a.cview(), s.view(), &alloc, false);
        for (usize i = 0; i < m; ++i)
        {
            for (usize j = 0; j < n; ++j)
            {
                crd::f64 acc = 0.0;
                for (usize p = 0; p < k; ++p)
                {
                    acc = fma ? std::fma(a(i, p), b(p, j), acc) : acc + a(i, p) * b(p, j);
                }
                wrong += same_bits(c(i, j), 0.0 + acc) ? 0U : 1U;
            }
            for (usize j = 0; j < m; ++j)
            {
                crd::f64 acc = 0.0;
                for (usize p = 0; p < k; ++p)
                {
                    acc = fma ? std::fma(a(i, p), a(j, p), acc) : acc + a(i, p) * a(j, p);
                }
                wrong += same_bits(s(i, j), j <= i ? s0(i, j) - acc : s0(i, j)) ? 0U : 1U;
            }
        }
    }
    CHECK(wrong == 0U);
}

TEST_CASE("dense kernels: the process runs the level its CPU selection resolves", "[dense][kernels]")
{
    const crd::CpuSelection& s = crd::cpu_selection();
    const auto levels = dn::dense_kernel_levels();
    const KernelLevel<DenseKernels>& chosen = dn::dense_kernels();
    INFO("selected " << crd::cpu_level_name(s.selected) << ", running " << crd::cpu_level_name(chosen.level));
    CHECK(&chosen == &crd::math::select_kernel_level<DenseKernels>(levels, s.selected, s.kernel_features,
                                                                   crd::math::kernel_level_cap()));
    CHECK(crd::cpu_level_includes(s.selected, chosen.level));
    CHECK(crd::cpu_level_includes(crd::math::kernel_level_cap(), chosen.level));
}
