// The hesap-dense kernel levels (SIMD-DISPATCH, ADR-0134 §5, §7): every level the host runs gives the bits of its
// declared arithmetic for the f64 and f32 GEMM blocks (each product element summed in depth order from +0, rounding
// each multiply and add on portable and sse2, by single-rounded fused multiply-add from avx2) and the levels agree
// within the inner-product bound. Each level has its own tile shape (GemmShape), so the blocks run on panels of that
// shape and cover partial and full tiles, depth 0 to 33, an A row stride wider than the depth, every update (add at
// alpha 1 and otherwise, subtract, assign, the first group's beta merge at beta 0 and otherwise), both C layouts and
// lower blocks whose diagonal crosses, skips or covers tiles; panel padding and A columns past the depth hold NaN,
// which no written element may reach. Each level's packers write the panels a scalar model does from every source
// stride. crd-hesap-dense-kernels-at-<level> runs these [kernels] cases under each forced level.

#include "hesap_jobs_fixture.hpp"

#include <crd/core/cpu_features.hpp>
#include <crd/hesap/dense/blas3.hpp>
#include <crd/hesap/dense/dense_dispatch.hpp>
#include <crd/hesap/dense/detail/gemm_pack.hpp>
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
using dn::GemmShape;
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

template <typename T> bool exact_bits(T a, T b)
{
    using Bits = std::conditional_t<sizeof(T) == 8, u64, u32>;
    return std::bit_cast<Bits>(a) == std::bit_cast<Bits>(b);
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

// The sum of a[p] b[p * b_stride] over p < depth in the declared arithmetic.
template <typename T> T inner(const T* a, const T* b, usize b_stride, usize depth, bool fma)
{
    T acc = T(0);
    for (usize p = 0; p < depth; ++p)
    {
        acc = fma ? std::fma(a[p], b[p * b_stride], acc) : acc + a[p] * b[p * b_stride];
    }
    return acc;
}

template <typename T> T updated(GemmUpdate update, T alpha, T beta, T c, T p)
{
    if (update == GemmUpdate::Assign)
    {
        return p;
    }
    if (update == GemmUpdate::Scale)
    {
        return (beta == T(0) ? T(0) : beta * c) + alpha * p;
    }
    if (update == GemmUpdate::Subtract)
    {
        return c - p;
    }
    return alpha == T(1) ? c + p : c + alpha * p;
}

struct Counts
{
    usize cases = 0;
    usize wrong = 0;   // a written element without the declared bits
    usize touched = 0; // an element outside the block or above a lower block's diagonal changed
    usize apart = 0;   // a product further from the two-rounding one than the inner-product bound
};

struct Shape
{
    usize rows, cols, depth, lda, row0, col0;
    bool lower;
};

template <typename T> struct Buffers
{
    crd::containers::Array<T> a, b, logical_b, c0, c_nan, c;
    explicit Buffers(crd::memory::IAllocator* alloc)
        : a(alloc), b(alloc), logical_b(alloc), c0(alloc), c_nan(alloc), c(alloc)
    {
    }
};

// One shape at every update and layout on one level, on panels of the level's tile shape.
template <typename T>
void check_shape(const DenseKernels& k, CpuLevel level, const Shape& s, Buffers<T>& buf, Lcg& g, Counts& counts)
{
    const GemmShape shape = dn::kernels_of<T>(k).gemm_shape;
    const usize mr = shape.mr;
    const usize nr = shape.nr;
    const T nan = std::numeric_limits<T>::quiet_NaN();
    const usize panels_m = (s.rows + mr - 1) / mr;
    const usize panels_n = (s.cols + nr - 1) / nr;
    buf.a.resize(panels_m * mr * s.lda + 1U);
    buf.b.resize(panels_n * nr * s.depth + 1U);
    buf.logical_b.resize(s.depth * s.cols + 1U);
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
                const usize col = pn * nr + j;
                const T v = col < s.cols ? g.value<T>() : nan;
                buf.b[(pn * s.depth + p) * nr + j] = v;
                if (col < s.cols)
                {
                    buf.logical_b[p * s.cols + col] = v;
                }
            }
        }
    }
    const usize ldc = (s.rows > s.cols ? s.rows : s.cols) + 3U;
    buf.c0.resize(ldc * ldc);
    buf.c_nan.resize(ldc * ldc);
    for (usize e = 0; e < ldc * ldc; ++e)
    {
        buf.c_nan[e] = nan; // beta 0 reads no C: an uninitialized C gives finite elements
        buf.c0[e] = g.value<T>();
    }
    const bool fma = sums_by_fma(level);
    const T eps = std::numeric_limits<T>::epsilon();
    const struct
    {
        GemmUpdate update;
        T alpha;
        T beta;
    } updates[] = {{GemmUpdate::Add, T(1), T(0)},        {GemmUpdate::Add, T(0.75), T(0)},
                   {GemmUpdate::Subtract, T(0), T(0)},   {GemmUpdate::Assign, T(0), T(0)},
                   {GemmUpdate::Scale, T(1), T(0)},      {GemmUpdate::Scale, T(-0.75), T(0)},
                   {GemmUpdate::Scale, T(0.75), T(-0.5)}};
    for (const auto& u : updates)
    {
        for (const bool col_major : {false, true})
        {
            ++counts.cases;
            const bool unread = u.update == GemmUpdate::Scale && u.beta == T(0);
            const crd::containers::Array<T>& start = unread ? buf.c_nan : buf.c0;
            buf.c = start;
            dn::kernels_of<T>(k).gemm_block(GemmBlock<T>{.a = buf.a.data(),
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
                                                         .beta = u.beta,
                                                         .update = u.update,
                                                         .col_major = col_major,
                                                         .lower = s.lower});
            for (usize i = 0; i < ldc; ++i)
            {
                for (usize j = 0; j < ldc; ++j)
                {
                    const usize at = col_major ? j * ldc + i : i * ldc + j;
                    const bool written = i < s.rows && j < s.cols && (!s.lower || s.row0 + i >= s.col0 + j);
                    if (!written)
                    {
                        counts.touched += same_bits(buf.c[at], start[at]) ? 0U : 1U;
                        continue;
                    }
                    const T* a_row = buf.a.data() + i * s.lda;
                    const T* b_col = buf.logical_b.data() + j;
                    const T p = inner(a_row, b_col, s.cols, s.depth, fma);
                    const T want = updated(u.update, u.alpha, u.beta, start[at], p);
                    counts.wrong += (unread ? exact_bits(buf.c[at], want) : same_bits(buf.c[at], want)) ? 0U : 1U;
                    T bound = T(0);
                    for (usize q = 0; q < s.depth; ++q)
                    {
                        bound += std::fabs(a_row[q] * b_col[q * s.cols]);
                    }
                    const T two_roundings = inner(a_row, b_col, s.cols, s.depth, false);
                    counts.apart += std::fabs(p - two_roundings) <= T(s.depth) * eps * bound ? 0U : 1U;
                }
            }
        }
    }
}

template <typename T> void check_every_level()
{
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
        const GemmShape shape = dn::kernels_of<T>(*level.table).gemm_shape;
        const usize mr = shape.mr;
        const usize nr = shape.nr;
        INFO("level " << crd::cpu_level_name(level.level) << ", " << sizeof(T) * 8U << "-bit, " << mr << " by " << nr);
        CHECK(mr >= 1U);
        CHECK(nr >= 1U);
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
                        if (rows == 0 || cols == 0)
                        {
                            continue; // a 1-row or 1-column tile has no mr - 1 or nr - 1 case
                        }
                        ++shapes;
                        check_shape<T>(*level.table, level.level, Shape{rows, cols, depth, depth + wider, 0, 0, false},
                                       buf, g, counts);
                        for (const auto& o : origins)
                        {
                            check_shape<T>(*level.table, level.level,
                                           Shape{rows, cols, depth, depth + wider, o[0], o[1], true}, buf, g, counts);
                        }
                    }
                }
            }
        }
        CHECK(counts.cases == shapes * 5U * 7U * 2U);
        CHECK(counts.wrong == 0U);
        CHECK(counts.touched == 0U);
        CHECK(counts.apart == 0U);
    }
    CHECK(ran >= 1U); // portable runs everywhere
}

// Each level's pack_a and pack_b against the scalar model: the panels of op(X) from every stride pairing (a row-major
// and a column-major source, each read plain and transposed, at an offset), the padding +0, and nothing written past
// the panels (a NaN guard word).
template <typename T> void check_packers()
{
    crd::memory::TlsfAllocator alloc(8U << 20);
    const T nan = std::numeric_limits<T>::quiet_NaN();
    crd::containers::Array<T> src(&alloc);
    crd::containers::Array<T> out(&alloc);
    Lcg g{0xFACEULL};
    constexpr usize ld = 41;
    src.resize(ld * ld);
    for (usize e = 0; e < ld * ld; ++e)
    {
        src[e] = g.value<T>() + T(e % 5U); // distinct enough to catch a misplaced value
    }
    usize ran = 0;
    usize wrong = 0;
    for (const KernelLevel<DenseKernels>& level : dn::dense_kernel_levels())
    {
        if (!runs_here(level))
        {
            continue;
        }
        ++ran;
        const auto& kt = dn::kernels_of<T>(*level.table);
        const GemmShape shape = kt.gemm_shape;
        INFO("level " << crd::cpu_level_name(level.level) << ", " << sizeof(T) * 8U << "-bit");
        const usize strides[][2] = {{ld, 1}, {1, ld}, {ld, 3}};
        for (const auto& st : strides)
        {
            const dn::GemmSource<T> s{
                .data = src.data() + 2U * st[0] + 3U * st[1], .row_stride = st[0], .col_stride = st[1]};
            auto at = [&s](usize r, usize c)
            {
                return s.data[r * s.row_stride + c * s.col_stride];
            };
            for (const usize rows : {usize{1}, shape.mr - 1U, shape.mr, 2U * shape.mr + 1U})
            {
                for (const usize depth : {usize{0}, usize{1}, usize{3}, usize{4}, usize{9}})
                {
                    if (rows == 0)
                    {
                        continue;
                    }
                    const usize size = dn::detail::packed_a_size(shape, rows, depth);
                    out.resize(size + 1U);
                    for (usize e = 0; e <= size; ++e)
                    {
                        out[e] = nan;
                    }
                    kt.pack_a(s, rows, depth, out.data());
                    for (usize e = 0; e < size; ++e)
                    {
                        const usize panel = e / (shape.mr * depth);
                        const usize r = panel * shape.mr + e % (shape.mr * depth) / depth;
                        const usize p = e % depth;
                        wrong += exact_bits(out[e], r < rows ? at(r, p) : T(0)) ? 0U : 1U;
                    }
                    wrong += std::isnan(out[size]) ? 0U : 1U;
                }
            }
            for (const usize cols : {usize{1}, shape.nr - 1U, shape.nr, 2U * shape.nr + 1U})
            {
                for (const usize depth : {usize{0}, usize{1}, usize{3}, usize{4}, usize{9}})
                {
                    if (cols == 0)
                    {
                        continue;
                    }
                    const usize size = dn::detail::packed_b_size(shape, depth, cols);
                    out.resize(size + 1U);
                    for (usize e = 0; e <= size; ++e)
                    {
                        out[e] = nan;
                    }
                    kt.pack_b(s, depth, cols, out.data());
                    for (usize e = 0; e < size; ++e)
                    {
                        const usize panel = e / (depth * shape.nr);
                        const usize p = e % (depth * shape.nr) / shape.nr;
                        const usize c = panel * shape.nr + e % shape.nr;
                        wrong += exact_bits(out[e], c < cols ? at(p, c) : T(0)) ? 0U : 1U;
                    }
                    wrong += std::isnan(out[size]) ? 0U : 1U;
                }
            }
        }
    }
    CHECK(ran >= 1U);
    CHECK(wrong == 0U);
}

// C = alpha op(A) B + beta C in the declared arithmetic: each kKc group's sum from +0, the first merged as
// beta c + alpha p (beta 0: +0 + alpha p), the later ones as c + alpha p (alpha 1: c + p) (dense_kernels.hpp).
template <typename T>
T grouped(T alpha, T beta, T c, usize i, usize j, usize k, const crd::containers::Array<T>& a,
          const crd::containers::Array<T>& b, usize n, bool fma)
{
    T out = c;
    for (usize pc = 0; pc < k; pc += dn::detail::kGemmKc)
    {
        const usize kc = k - pc < dn::detail::kGemmKc ? k - pc : dn::detail::kGemmKc;
        const T p = inner(a.data() + i * k + pc, b.data() + pc * n + j, n, kc, fma);
        if (pc == 0)
        {
            out = (beta == T(0) ? T(0) : beta * out) + alpha * p;
        }
        else
        {
            out = alpha == T(1) ? out + p : out + alpha * p;
        }
    }
    return out;
}

// One element type through every GEMM path on the selected level: gemm, gemm_parallel's resident-A small path and
// its packed path, at 1, 2 and 3 workers, both layouts and both transposes of A, and the lower syrk, at m and n off
// the tile shape and a depth across a kKc group. Every path gives the grouped model's bits.
template <typename T> void check_paths()
{
    crd_hesap_dense_tests::hesap_jobs_listener();
    crd::memory::TlsfAllocator alloc(32U << 20);
    const bool fma = sums_by_fma(dn::dense_kernels().level);
    const GemmShape shape = dn::detail::gemm_shape_of<T>();
    INFO(sizeof(T) * 8U << "-bit, " << shape.mr << " by " << shape.nr);
    using RowView = dn::MatrixView<const T, dn::Layout::RowMajor>;
    using ColView = dn::MatrixView<const T, dn::Layout::ColMajor>;
    Lcg g{0xD3ULL};
    usize wrong = 0;
    usize runs = 0;
    for (const usize k : {usize{9}, usize{300}})
    {
        for (const usize m : {usize{13}, 6U * shape.mr * shape.nr + 1U})
        {
            const usize n = shape.nr + 3U;
            crd::containers::Array<T> a(&alloc);  // op(A), row-major m by k
            crd::containers::Array<T> at(&alloc); // op(A) transposed, row-major k by m
            crd::containers::Array<T> b(&alloc);  // B, row-major k by n
            crd::containers::Array<T> bt(&alloc); // B, column-major k by n
            crd::containers::Array<T> c0(&alloc);
            crd::containers::Array<T> c(&alloc);
            a.resize(m * k);
            at.resize(m * k);
            b.resize(k * n);
            bt.resize(k * n);
            c0.resize(m * (m > n ? m : n));
            for (usize i = 0; i < m; ++i)
            {
                for (usize p = 0; p < k; ++p)
                {
                    a[i * k + p] = g.value<T>();
                    at[p * m + i] = a[i * k + p];
                }
            }
            for (usize p = 0; p < k; ++p)
            {
                for (usize j = 0; j < n; ++j)
                {
                    b[p * n + j] = g.value<T>();
                    bt[j * k + p] = b[p * n + j];
                }
            }
            for (usize e = 0; e < c0.size(); ++e)
            {
                c0[e] = g.value<T>();
            }
            const struct
            {
                T alpha, beta;
            } scales[] = {{T(1), T(0)}, {T(0.5), T(0.25)}};
            for (const auto& sc : scales)
            {
                // row_major C (m by n, ldc n) or column-major (ldc m); 7 runs: 3 paths a worker count, then syrk.
                for (const bool col : {false, true})
                {
                    for (const u32 workers : {1U, 2U, 3U})
                    {
                        for (const bool transposed : {false, true})
                        {
                            c = c0;
                            if (col)
                            {
                                const ColView av =
                                    transposed ? ColView(a.data(), k, m, k) : ColView(at.data(), m, k, m);
                                const dn::MatrixView<T, dn::Layout::ColMajor> cv(c.data(), m, n, m);
                                dn::gemm_parallel<T, dn::Layout::ColMajor>(
                                    workers, sc.alpha, av, ColView(bt.data(), k, n, k), sc.beta, cv,
                                    transposed ? dn::Trans::Transpose : dn::Trans::None, dn::Trans::None, &alloc);
                            }
                            else
                            {
                                const RowView av =
                                    transposed ? RowView(at.data(), k, m, m) : RowView(a.data(), m, k, k);
                                const dn::MatrixView<T, dn::Layout::RowMajor> cv(c.data(), m, n, n);
                                dn::gemm_parallel<T, dn::Layout::RowMajor>(
                                    workers, sc.alpha, av, RowView(b.data(), k, n, n), sc.beta, cv,
                                    transposed ? dn::Trans::Transpose : dn::Trans::None, dn::Trans::None, &alloc);
                            }
                            ++runs;
                            for (usize i = 0; i < m; ++i)
                            {
                                for (usize j = 0; j < n; ++j)
                                {
                                    const usize at_c = col ? j * m + i : i * n + j;
                                    const T want = grouped(sc.alpha, sc.beta, c0[at_c], i, j, k, a, b, n, fma);
                                    wrong += same_bits(c[at_c], want) ? 0U : 1U;
                                }
                            }
                        }
                    }
                }
            }
            // syrk: C -= A A^T on the lower triangle, group by group.
            c = c0;
            dn::detail::syrk_lower_minus<T, dn::Layout::RowMajor>(
                RowView(a.data(), m, k, k), dn::MatrixView<T, dn::Layout::RowMajor>(c.data(), m, m, m), &alloc, false);
            ++runs;
            for (usize i = 0; i < m; ++i)
            {
                for (usize j = 0; j < m; ++j)
                {
                    T want = c0[i * m + j];
                    for (usize pc = 0; j <= i && pc < k; pc += dn::detail::kGemmKc)
                    {
                        const usize kc = k - pc < dn::detail::kGemmKc ? k - pc : dn::detail::kGemmKc;
                        want = want - inner(a.data() + i * k + pc, a.data() + j * k + pc, 1U, kc, fma);
                    }
                    wrong += same_bits(c[i * m + j], want) ? 0U : 1U;
                }
            }
        }
    }
    CHECK(runs == 2U * 2U * (2U * 2U * 3U * 2U + 1U));
    CHECK(wrong == 0U);
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

TEST_CASE("dense kernels: every level packs the GEMM panels of its tile shape (f64)", "[dense][kernels]")
{
    check_packers<crd::f64>();
}

TEST_CASE("dense kernels: every level packs the GEMM panels of its tile shape (f32)", "[dense][kernels]")
{
    check_packers<crd::f32>();
}

TEST_CASE("dense kernels: every GEMM path and worker count gives the grouped bits (f64)", "[dense][kernels]")
{
    check_paths<crd::f64>();
}

TEST_CASE("dense kernels: every GEMM path and worker count gives the grouped bits (f32)", "[dense][kernels]")
{
    check_paths<crd::f32>();
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
