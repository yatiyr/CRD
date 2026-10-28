// The hesap-direct kernel levels (SIMD-DISPATCH, ADR-0134 §5, §7): every level the host runs gives the bits
// crd/hesap/direct/direct_kernels.hpp declares for the supernodal panel solves (each step rounding its multiply and add
// below avx2, fused from avx2, the in-block recurrences rounding twice on every level); forward_below equals the dense
// gemvt of the same level bit for bit, block_forward_diagonal gives forward_diagonal's bits on every column, and the
// backward below-update's gemv with alpha -1 and beta 1 gives the bits of y - s. Panels hold NaN in the upper triangle
// and the padding, outputs end in a sentinel, and one pass draws infinities, signed zeros and subnormals (a skipped
// term or flush-to-zero shows). crd-hesap-direct-kernels-at-<level> runs these [kernels] cases under each forced level.

#include <crd/containers/array.hpp>
#include <crd/core/cpu_features.hpp>
#include <crd/hesap/direct/direct_dispatch.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <bit>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <type_traits>

namespace dn = crd::hesap::dense;
namespace dr = crd::hesap::direct;
using crd::CpuLevel;
using crd::u32;
using crd::u64;
using crd::usize;
using crd::containers::Array;

namespace
{

bool runs_here(CpuLevel level)
{
    return crd::cpu_level_includes(crd::cpu_selection().detected, level);
}

bool fuses(CpuLevel level)
{
    return crd::cpu_level_includes(level, CpuLevel::Avx2);
}

template <typename T> bool same_bits(T a, T b)
{
    using Bits = std::conditional_t<sizeof(T) == 8, u64, u32>;
    return (std::isnan(a) && std::isnan(b)) || std::bit_cast<Bits>(a) == std::bit_cast<Bits>(b); // any NaN, any NaN
}

template <typename T> T step(T a, T b, T c, bool fma)
{
    return fma ? std::fma(a, b, c) : a * b + c;
}

template <typename T> constexpr T kKeep = T(77);

// Values in [-9/7, 9/7]; with `specials`, one in eight is an infinity, a signed zero or a subnormal.
struct Values
{
    u64 s;
    bool specials = false;
    u32 next()
    {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<u32>(s >> 33);
    }
    template <typename T> T value()
    {
        const u32 r = next();
        if (specials && r % 8U == 0U)
        {
            const T inf = std::numeric_limits<T>::infinity();
            const T tiny = std::numeric_limits<T>::denorm_min();
            const T pick[] = {inf, -inf, T(0), -T(0), tiny * T(3), -tiny};
            return pick[(r >> 3U) % 6U];
        }
        return static_cast<T>(static_cast<int>(r % 19U) - 9) / T(7);
    }
    // A diagonal entry: away from zero, finite.
    template <typename T> T pivot() { return T(1) + static_cast<T>(next() % 7U) / T(4); }
};

const dn::DenseTypeKernels<crd::f64>* dense_at(CpuLevel level, crd::f64*)
{
    for (const auto& l : dn::dense_kernel_levels())
    {
        if (l.level == level)
        {
            return &l.table->f64_kernels;
        }
    }
    return nullptr;
}

const dn::DenseTypeKernels<crd::f32>* dense_at(CpuLevel level, crd::f32*)
{
    for (const auto& l : dn::dense_kernel_levels())
    {
        if (l.level == level)
        {
            return &l.table->f32_kernels;
        }
    }
    return nullptr;
}

struct Counts
{
    usize ran = 0;
    usize cases = 0;
    usize wrong = 0;   // a result without the declared bits
    usize touched = 0; // an element the kernel must not write changed
    usize missing = 0; // a direct level without the dense level of the same CpuLevel
};

// A column-major panel of `rows` rows (ld = rows + 2) and nc columns: NaN above the diagonal of its first nc rows, in
// the padding and one past the end; a pivot on the diagonal.
template <typename T> void fill_panel(Array<T>& p, usize rows, usize nc, Values& g)
{
    const T nan = std::numeric_limits<T>::quiet_NaN();
    const usize ld = rows + 2U;
    p.resize(nc * ld + 1U);
    for (usize k = 0; k < nc; ++k)
    {
        for (usize i = 0; i < ld; ++i)
        {
            if (i >= rows || i < k)
            {
                p[k * ld + i] = nan;
            }
            else
            {
                p[k * ld + i] = i == k ? g.pivot<T>() : g.value<T>();
            }
        }
    }
    p[nc * ld] = nan;
}

template <typename T> void check_forward_below(crd::memory::IAllocator* alloc, Counts& c, bool specials)
{
    constexpr usize w = sizeof(T) == 8 ? 4U : 8U; // the 256-bit width, and the 512-bit one at avx512
    constexpr usize wide = 2U * w;
    const T nan = std::numeric_limits<T>::quiet_NaN();
    Array<T> a(alloc);
    Array<T> x(alloc);
    Array<T> y(alloc);
    Array<T> want(alloc);
    Array<T> dense(alloc);
    Values g{0xD1ULL, specials};
    for (const auto& level : dr::direct_kernel_levels())
    {
        if (!runs_here(level.level))
        {
            continue;
        }
        ++c.ran;
        const auto* dk = dense_at(level.level, static_cast<T*>(nullptr));
        c.missing += dk == nullptr ? 1U : 0U;
        const bool fma = fuses(level.level);
        for (usize rows = 0; rows <= 9U; ++rows)
        {
            for (const usize n : {usize{0}, usize{1}, w - 1U, w, 2U * w + 1U, 4U * w + 3U, wide - 1U, 2U * wide + 1U,
                                  4U * wide + 3U, usize{257}, usize{600}})
            {
                const usize lda = n + 3U;
                a.resize(rows * lda + 1U);
                x.resize(rows + 1U);
                y.resize(n + 1U);
                want.resize(n + 1U);
                dense.resize(n + 1U);
                for (usize i = 0; i < a.size(); ++i)
                {
                    a[i] = i < rows * lda && i % lda < n ? g.value<T>() : nan;
                }
                for (usize r = 0; r < rows; ++r)
                {
                    x[r] = g.value<T>();
                }
                x[rows] = nan;
                for (usize j = 0; j < n; ++j)
                {
                    y[j] = g.value<T>();
                    dense[j] = y[j];
                    want[j] = y[j];
                    for (usize r = 0; r < rows; ++r)
                    {
                        want[j] = step(x[r], a[r * lda + j], want[j], fma);
                    }
                }
                y[n] = kKeep<T>;
                dense[n] = kKeep<T>;
                dr::kernels_of<T>(*level.table)
                    .forward_below(dn::GemvtRows<T>{a.data(), x.data(), y.data(), lda, 1U, rows, n});
                if (dk != nullptr)
                {
                    dk->gemvt(dn::GemvtRows<T>{a.data(), x.data(), dense.data(), lda, 1U, rows, n});
                }
                ++c.cases;
                for (usize j = 0; j < n; ++j)
                {
                    c.wrong += same_bits(y[j], want[j]) && same_bits(y[j], dense[j]) ? 0U : 1U;
                }
                c.touched += same_bits(y[n], kKeep<T>) ? 0U : 1U;
            }
        }
    }
}

// The declared forward_diagonal on one column x (stride 1).
template <typename T> void model_forward_diagonal(T* x, const T* p, usize ld, usize nc, bool fma)
{
    for (usize j0 = 0; j0 < nc; j0 += 4U)
    {
        const usize end = j0 + 4U <= nc ? j0 + 4U : nc;
        for (usize j = j0; j < end; ++j)
        {
            x[j] = x[j] / p[j * ld + j];
            for (usize i = j + 1U; i < end; ++i)
            {
                x[i] = x[i] - p[j * ld + i] * x[j];
            }
        }
        for (usize i = end; i < nc; ++i)
        {
            for (usize j = j0; j < end; ++j)
            {
                x[i] = step(-p[j * ld + i], x[j], x[i], fma);
            }
        }
    }
}

template <typename T> void check_forward_diagonal(crd::memory::IAllocator* alloc, Counts& c, bool specials)
{
    Array<T> p(alloc);
    Array<T> x(alloc);
    Array<T> want(alloc);
    Array<T> d(alloc);
    Values g{0xD2ULL, specials};
    for (const auto& level : dr::direct_kernel_levels())
    {
        if (!runs_here(level.level))
        {
            continue;
        }
        ++c.ran;
        const auto& k = dr::kernels_of<T>(*level.table);
        for (const usize nc : {usize{0}, usize{1}, usize{2}, usize{3}, usize{4}, usize{5}, usize{7}, usize{8}, usize{9},
                               usize{13}, usize{50}})
        {
            const usize rows = nc + 3U; // rows below the diagonal block are not read
            const usize ld = rows + 2U;
            fill_panel(p, rows, nc, g);
            x.resize(nc + 1U);
            want.resize(nc + 1U);
            for (usize i = 0; i < nc; ++i)
            {
                x[i] = g.value<T>();
                want[i] = x[i];
            }
            x[nc] = kKeep<T>;
            model_forward_diagonal(want.data(), p.data(), ld, nc, fuses(level.level));
            k.forward_diagonal(x.data(), p.data(), ld, nc);
            ++c.cases;
            for (usize i = 0; i < nc; ++i)
            {
                c.wrong += same_bits(x[i], want[i]) ? 0U : 1U;
            }
            c.touched += same_bits(x[nc], kKeep<T>) ? 0U : 1U;
            // Each column of the row-major block gives forward_diagonal's bits.
            for (const usize nrhs : {usize{1}, usize{2}, usize{3}, usize{5}, usize{8}, usize{9}, usize{17}})
            {
                d.resize(nc * nrhs + 1U);
                for (usize i = 0; i < nc * nrhs; ++i)
                {
                    d[i] = g.value<T>();
                }
                d[nc * nrhs] = kKeep<T>;
                Array<T> cols(alloc);
                cols.resize(nc * nrhs);
                for (usize i = 0; i < nc * nrhs; ++i)
                {
                    cols[(i % nrhs) * nc + i / nrhs] = d[i];
                }
                k.block_forward_diagonal(d.data(), nrhs, p.data(), ld, nc);
                ++c.cases;
                for (usize col = 0; col < nrhs; ++col)
                {
                    k.forward_diagonal(cols.data() + col * nc, p.data(), ld, nc);
                    for (usize i = 0; i < nc; ++i)
                    {
                        c.wrong += same_bits(d[i * nrhs + col], cols[col * nc + i]) ? 0U : 1U;
                    }
                }
                c.touched += same_bits(d[nc * nrhs], kKeep<T>) ? 0U : 1U;
            }
        }
    }
}

template <typename T> void check_block_forward_below(crd::memory::IAllocator* alloc, Counts& c, bool specials)
{
    const T nan = std::numeric_limits<T>::quiet_NaN();
    Array<T> p(alloc);
    Array<T> y(alloc);
    Array<T> out(alloc);
    Values g{0xD3ULL, specials};
    for (const auto& level : dr::direct_kernel_levels())
    {
        if (!runs_here(level.level))
        {
            continue;
        }
        ++c.ran;
        const bool fma = fuses(level.level);
        for (const usize nc : {usize{0}, usize{1}, usize{3}, usize{4}, usize{5}, usize{9}})
        {
            for (const usize rows : {usize{0}, usize{1}, usize{7}, usize{19}, usize{256}, usize{257}, usize{600}})
            {
                for (const usize nrhs : {usize{1}, usize{2}, usize{5}, usize{9}})
                {
                    const usize ld = rows + 2U;
                    const usize ldy = nc + 1U;
                    p.resize(nc * ld + 1U);
                    for (usize i = 0; i < p.size(); ++i)
                    {
                        p[i] = i < nc * ld && i % ld < rows ? g.value<T>() : nan;
                    }
                    y.resize(ldy * nrhs + 1U);
                    for (usize i = 0; i < y.size(); ++i)
                    {
                        y[i] = i % ldy < nc && i < ldy * nrhs ? g.value<T>() : nan;
                    }
                    out.resize(rows * nrhs + 1U);
                    for (usize i = 0; i < rows * nrhs; ++i)
                    {
                        out[i] = nan; // written, never read
                    }
                    out[rows * nrhs] = kKeep<T>;
                    dr::kernels_of<T>(*level.table)
                        .block_forward_below(
                            dr::PanelProduct<T>{out.data(), p.data(), y.data(), ld, ldy, nc, rows, nrhs});
                    ++c.cases;
                    for (usize col = 0; col < nrhs; ++col)
                    {
                        for (usize r = 0; r < rows; ++r)
                        {
                            T want = T(0);
                            for (usize k = 0; k < nc; ++k)
                            {
                                want = step(y[k + col * ldy], p[k * ld + r], want, fma);
                            }
                            c.wrong += same_bits(out[col * rows + r], want) ? 0U : 1U;
                        }
                    }
                    c.touched += same_bits(out[rows * nrhs], kKeep<T>) ? 0U : 1U;
                }
            }
        }
    }
}

template <typename T> void check_block_backward_below(crd::memory::IAllocator* alloc, Counts& c, bool specials)
{
    const T nan = std::numeric_limits<T>::quiet_NaN();
    Array<T> p(alloc);
    Array<T> wt(alloc);
    Array<T> y(alloc);
    Array<T> y0(alloc);
    Array<T> acc(alloc);
    Values g{0xD4ULL, specials};
    for (const auto& level : dr::direct_kernel_levels())
    {
        if (!runs_here(level.level))
        {
            continue;
        }
        ++c.ran;
        const bool fma = fuses(level.level);
        for (const usize nc : {usize{0}, usize{1}, usize{3}, usize{4}, usize{5}, usize{8}, usize{9}, usize{17}})
        {
            for (const usize rows : {usize{0}, usize{1}, usize{7}, usize{257}, usize{600}})
            {
                for (const usize nrhs : {usize{1}, usize{3}, usize{4}, usize{5}, usize{8}, usize{9}, usize{17}})
                {
                    const usize ld = rows + 2U;
                    const usize ldy = nc + 2U;
                    p.resize(nc * ld + 1U);
                    for (usize i = 0; i < p.size(); ++i)
                    {
                        p[i] = i < nc * ld && i % ld < rows ? g.value<T>() : nan;
                    }
                    wt.resize(rows * nrhs + 1U);
                    for (usize i = 0; i < rows * nrhs; ++i)
                    {
                        wt[i] = g.value<T>();
                    }
                    wt[rows * nrhs] = nan;
                    y.resize(ldy * nrhs + 1U);
                    y0.resize(y.size());
                    for (usize i = 0; i < y.size(); ++i)
                    {
                        y[i] = i % ldy < nc && i < ldy * nrhs ? g.value<T>() : kKeep<T>;
                        y0[i] = y[i];
                    }
                    acc.resize(nc * nrhs + 1U);
                    for (usize i = 0; i < nc * nrhs; ++i)
                    {
                        acc[i] = nan; // scratch, written before it is read
                    }
                    acc[nc * nrhs] = kKeep<T>;
                    dr::kernels_of<T>(*level.table)
                        .block_backward_below(
                            dr::PanelReduce<T>{y.data(), acc.data(), p.data(), wt.data(), ldy, ld, nc, rows, nrhs});
                    ++c.cases;
                    for (usize i = 0; i < y.size(); ++i)
                    {
                        const usize k = i % ldy;
                        const usize col = i / ldy;
                        if (k >= nc || col >= nrhs)
                        {
                            c.touched += same_bits(y[i], y0[i]) ? 0U : 1U;
                            continue;
                        }
                        T s = T(0);
                        for (usize r = 0; r < rows; ++r)
                        {
                            s = step(p[k * ld + r], wt[r * nrhs + col], s, fma);
                        }
                        c.wrong += same_bits(y[i], y0[i] - s) ? 0U : 1U;
                    }
                    c.touched += same_bits(acc[nc * nrhs], kKeep<T>) ? 0U : 1U;
                }
            }
        }
    }
}

template <typename T> void check_block_backward_diagonal(crd::memory::IAllocator* alloc, Counts& c, bool specials)
{
    Array<T> p(alloc);
    Array<T> d(alloc);
    Array<T> want(alloc);
    Values g{0xD5ULL, specials};
    for (const auto& level : dr::direct_kernel_levels())
    {
        if (!runs_here(level.level))
        {
            continue;
        }
        ++c.ran;
        const bool fma = fuses(level.level);
        for (const usize nc : {usize{0}, usize{1}, usize{2}, usize{3}, usize{4}, usize{5}, usize{7}, usize{8}, usize{9},
                               usize{13}, usize{50}})
        {
            const usize rows = nc + 2U;
            const usize ld = rows + 2U;
            fill_panel(p, rows, nc, g);
            for (const usize nrhs : {usize{1}, usize{2}, usize{3}, usize{4}, usize{5}, usize{8}, usize{9}, usize{17}})
            {
                d.resize(nc * nrhs + 1U);
                want.resize(nc * nrhs);
                for (usize i = 0; i < nc * nrhs; ++i)
                {
                    d[i] = g.value<T>();
                    want[i] = d[i];
                }
                d[nc * nrhs] = kKeep<T>;
                if (nc > 0U)
                {
                    for (usize j0 = ((nc - 1U) / 4U) * 4U;; j0 -= 4U)
                    {
                        const usize end = j0 + 4U <= nc ? j0 + 4U : nc;
                        for (usize j = j0; j < end; ++j)
                        {
                            for (usize col = 0; col < nrhs; ++col)
                            {
                                T s = T(0);
                                for (usize k = end; k < nc; ++k)
                                {
                                    s = step(p[j * ld + k], want[k * nrhs + col], s, fma);
                                }
                                want[j * nrhs + col] = want[j * nrhs + col] - s;
                            }
                        }
                        for (usize j = end; j-- > j0;)
                        {
                            for (usize col = 0; col < nrhs; ++col)
                            {
                                T v = want[j * nrhs + col];
                                for (usize k = j + 1U; k < end; ++k)
                                {
                                    v = v - p[j * ld + k] * want[k * nrhs + col];
                                }
                                want[j * nrhs + col] = v / p[j * ld + j];
                            }
                        }
                        if (j0 == 0U)
                        {
                            break;
                        }
                    }
                }
                dr::kernels_of<T>(*level.table).block_backward_diagonal(d.data(), nrhs, p.data(), ld, nc);
                ++c.cases;
                for (usize i = 0; i < nc * nrhs; ++i)
                {
                    c.wrong += same_bits(d[i], want[i]) ? 0U : 1U;
                }
                c.touched += same_bits(d[nc * nrhs], kKeep<T>) ? 0U : 1U;
            }
        }
    }
}

// The single-RHS backward below-update runs the dense gemv with alpha -1 and beta 1: the bits of y - s, s the gemv
// row's sum (alpha 1, beta 0), signed zeros in y included.
template <typename T> void check_backward_below_gemv(crd::memory::IAllocator* alloc, Counts& c, bool specials)
{
    Array<T> a(alloc);
    Array<T> x(alloc);
    Array<T> y(alloc);
    Array<T> s(alloc);
    Values g{0xD6ULL, specials};
    for (const auto& level : dn::dense_kernel_levels())
    {
        if (!runs_here(level.level))
        {
            continue;
        }
        ++c.ran;
        const auto& k = dn::kernels_of<T>(*level.table);
        for (const usize nc : {usize{1}, usize{4}, usize{9}, usize{17}})
        {
            for (const usize below : {usize{0}, usize{1}, usize{13}, usize{300}})
            {
                const usize ld = below + nc + 1U;
                a.resize(nc * ld);
                for (usize i = 0; i < a.size(); ++i)
                {
                    a[i] = g.value<T>();
                }
                x.resize(below + 1U);
                for (usize i = 0; i < x.size(); ++i)
                {
                    x[i] = g.value<T>();
                }
                y.resize(nc + 1U);
                s.resize(nc);
                for (usize i = 0; i < nc; ++i)
                {
                    const T zero = i % 3U == 0U ? T(0) : -T(0);
                    y[i] = i % 3U == 2U ? g.value<T>() : zero;
                    s[i] = T(0);
                }
                y[nc] = kKeep<T>;
                Array<T> y0(alloc);
                y0.resize(nc);
                for (usize i = 0; i < nc; ++i)
                {
                    y0[i] = y[i];
                }
                k.gemv(dn::GemvRows<T>{a.data() + nc, x.data(), s.data(), ld, nc, below, T(1), T(0)});
                k.gemv(dn::GemvRows<T>{a.data() + nc, x.data(), y.data(), ld, nc, below, T(-1), T(1)});
                ++c.cases;
                for (usize i = 0; i < nc; ++i)
                {
                    c.wrong += same_bits(y[i], y0[i] - s[i]) ? 0U : 1U;
                }
                c.touched += same_bits(y[nc], kKeep<T>) ? 0U : 1U;
            }
        }
    }
}

template <typename Check> Counts run(Check check)
{
    crd::memory::TlsfAllocator alloc(16U << 20);
    Counts c;
    check(&alloc, c, false);
    check(&alloc, c, true);
    return c;
}

void expect(const Counts& c)
{
    CHECK(c.ran >= 2U);
    CHECK(c.cases > 0U);
    CHECK(c.wrong == 0U);
    CHECK(c.touched == 0U);
    CHECK(c.missing == 0U);
}

} // namespace

TEST_CASE("direct kernels: the archive holds portable and each body level the build compiles", "[direct][kernels]")
{
    const auto levels = dr::direct_kernel_levels();
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

TEST_CASE("direct kernels: forward_below gives gemvt's bits on every level the host runs", "[direct][kernels]")
{
    expect(run(check_forward_below<crd::f64>));
    expect(run(check_forward_below<crd::f32>));
}

TEST_CASE("direct kernels: the forward diagonal solves give their declared bits", "[direct][kernels]")
{
    expect(run(check_forward_diagonal<crd::f64>));
    expect(run(check_forward_diagonal<crd::f32>));
}

TEST_CASE("direct kernels: the multi-RHS below-updates give their declared bits", "[direct][kernels]")
{
    expect(run(check_block_forward_below<crd::f64>));
    expect(run(check_block_forward_below<crd::f32>));
    expect(run(check_block_backward_below<crd::f64>));
    expect(run(check_block_backward_below<crd::f32>));
}

TEST_CASE("direct kernels: the multi-RHS backward diagonal solve gives its declared bits", "[direct][kernels]")
{
    expect(run(check_block_backward_diagonal<crd::f64>));
    expect(run(check_block_backward_diagonal<crd::f32>));
}

TEST_CASE("direct kernels: the backward below-update's gemv gives the bits of y - s", "[direct][kernels]")
{
    expect(run(check_backward_below_gemv<crd::f64>));
    expect(run(check_backward_below_gemv<crd::f32>));
}
