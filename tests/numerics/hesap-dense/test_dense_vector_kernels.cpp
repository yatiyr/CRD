// The hesap-dense vector and matrix-vector kernel levels (SIMD-DISPATCH, ADR-0134 §5, §7): every level the host runs
// gives the bits of the reduction shape dense_kernels.hpp declares for dot, gemv, symv, trsv, the unblocked Cholesky
// and its panel solve (rounding each multiply and add below avx2, fusing each vector step from avx2, the scalar tails
// rounding twice), and the dot products of two levels agree within the inner-product bound. Lengths cover every
// remainder of the accumulator rotation and the prefetching rows; elements a kernel must not read (padding, the
// other triangle, a unit diagonal, past the end) hold NaN, and elements it must not write are checked unchanged.
// crd-hesap-dense-kernels-at-<level> runs these [kernels] cases under each forced level.

#include <crd/containers/array.hpp>
#include <crd/core/cpu_features.hpp>
#include <crd/hesap/dense/dense_dispatch.hpp>
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

namespace
{

bool runs_here(const KernelLevel<DenseKernels>& level)
{
    return crd::cpu_level_includes(crd::cpu_selection().detected, level.level);
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

template <typename T> constexpr usize kWidth = sizeof(T) == 8 ? 4 : 8;

// ---- the declared arithmetic ----

template <typename T> T step(T a, T b, T c, bool fma)
{
    return fma ? std::fma(a, b, c) : a * b + c;
}

template <typename T> T lane_sum(const T* l)
{
    if constexpr (kWidth<T> == 4)
    {
        return (l[0] + l[1]) + (l[2] + l[3]);
    }
    else
    {
        return ((l[0] + l[1]) + (l[2] + l[3])) + ((l[4] + l[5]) + (l[6] + l[7]));
    }
}

template <typename T> T model_dot(const T* a, const T* x, usize n, CpuLevel level)
{
    constexpr usize w = kWidth<T>;
    const bool fma = fuses(level);
    const usize count = fma ? 8U : 4U;
    T acc[8][8] = {};
    usize k = 0;
    for (usize v = 0; k + w <= n; k += w, ++v)
    {
        for (usize l = 0; l < w; ++l)
        {
            acc[v % count][l] = step(a[k + l], x[k + l], acc[v % count][l], fma);
        }
    }
    T total[8];
    for (usize l = 0; l < w; ++l)
    {
        total[l] = count == 8U ? ((acc[0][l] + acc[1][l]) + (acc[2][l] + acc[3][l])) +
                                     ((acc[4][l] + acc[5][l]) + (acc[6][l] + acc[7][l]))
                               : (acc[0][l] + acc[1][l]) + (acc[2][l] + acc[3][l]);
    }
    T s = lane_sum(total);
    for (; k < n; ++k)
    {
        s += a[k] * x[k];
    }
    return s;
}

template <typename T> T model_gemv_row(const T* row, const T* x, usize cols, T alpha, T beta, T y, CpuLevel level)
{
    constexpr usize w = kWidth<T>;
    T acc[8] = {};
    usize k = 0;
    for (; k + w <= cols; k += w)
    {
        for (usize l = 0; l < w; ++l)
        {
            acc[l] = step(row[k + l], x[k + l], acc[l], fuses(level));
        }
    }
    T s = lane_sum(acc);
    for (; k < cols; ++k)
    {
        s += row[k] * x[k];
    }
    return alpha * s + beta * y;
}

// ---- the kernels of one level ----

template <typename T> T run_dot(const DenseKernels& k, const T* a, const T* x, usize n)
{
    if constexpr (std::is_same_v<T, crd::f64>)
    {
        return k.dot_f64(a, x, n);
    }
    else
    {
        return k.dot_f32(a, x, n);
    }
}

template <typename T> void run_gemv(const DenseKernels& k, const dn::GemvRows<T>& g)
{
    if constexpr (std::is_same_v<T, crd::f64>)
    {
        k.gemv_f64(g);
    }
    else
    {
        k.gemv_f32(g);
    }
}

template <typename T> void run_symv(const DenseKernels& k, const dn::SymvLower<T>& s)
{
    if constexpr (std::is_same_v<T, crd::f64>)
    {
        k.symv_f64(s);
    }
    else
    {
        k.symv_f32(s);
    }
}

template <typename T> void run_trsv(const DenseKernels& k, const dn::TrsvRows<T>& t)
{
    if constexpr (std::is_same_v<T, crd::f64>)
    {
        k.trsv_f64(t);
    }
    else
    {
        k.trsv_f32(t);
    }
}

template <typename T> usize run_cholesky(const DenseKernels& k, T* a, usize n, usize ld)
{
    if constexpr (std::is_same_v<T, crd::f64>)
    {
        return k.cholesky_f64(a, n, ld);
    }
    else
    {
        return k.cholesky_f32(a, n, ld);
    }
}

template <typename T> void run_trsm(const DenseKernels& k, T* a, usize ld, usize col, usize nb, usize first, usize end)
{
    if constexpr (std::is_same_v<T, crd::f64>)
    {
        k.cholesky_trsm_f64(a, ld, col, nb, first, end);
    }
    else
    {
        k.cholesky_trsm_f32(a, ld, col, nb, first, end);
    }
}

// Every remainder of an eight-accumulator rotation twice over, plus long and prefetching lengths.
template <typename T> crd::containers::Array<usize> lengths(crd::memory::IAllocator* alloc)
{
    crd::containers::Array<usize> out(alloc);
    for (usize n = 0; n <= 2U * 8U * kWidth<T> + kWidth<T> + 2U; ++n)
    {
        out.push_back(n);
    }
    out.push_back(255);
    out.push_back(1000);
    return out;
}

struct Counts
{
    usize ran = 0;
    usize cases = 0;
    usize wrong = 0;   // a result without the declared bits
    usize touched = 0; // an element the kernel must not write changed
    usize apart = 0;   // two levels' dot products further apart than the inner-product bound
};

template <typename T> void check_dot(crd::memory::IAllocator* alloc, Counts& c)
{
    const auto levels = dn::dense_kernel_levels();
    const T nan = std::numeric_limits<T>::quiet_NaN();
    const auto ns = lengths<T>(alloc);
    crd::containers::Array<T> a(alloc);
    crd::containers::Array<T> x(alloc);
    Lcg g{0x51ULL};
    for (const auto& level : levels)
    {
        if (!runs_here(level))
        {
            continue;
        }
        ++c.ran;
        for (const usize n : ns)
        {
            a.resize(n + 1U);
            x.resize(n + 1U);
            T magnitude = T(0);
            for (usize i = 0; i < n; ++i)
            {
                a[i] = g.value<T>();
                x[i] = g.value<T>();
                magnitude += std::fabs(a[i] * x[i]);
            }
            a[n] = nan;
            x[n] = nan;
            const T d = run_dot(*level.table, a.data(), x.data(), n);
            const T sq = run_dot(*level.table, a.data(), a.data(), n);
            c.cases += 2U;
            c.wrong += same_bits(d, model_dot(a.data(), x.data(), n, level.level)) ? 0U : 1U;
            c.wrong += same_bits(sq, model_dot(a.data(), a.data(), n, level.level)) ? 0U : 1U;
            const T portable = run_dot(*levels[0].table, a.data(), x.data(), n);
            const T bound = T(2) * static_cast<T>(n) * std::numeric_limits<T>::epsilon() * magnitude;
            c.apart += std::fabs(d - portable) <= bound ? 0U : 1U;
        }
    }
}

template <typename T> void check_gemv(crd::memory::IAllocator* alloc, Counts& c)
{
    constexpr usize w = kWidth<T>;
    const T nan = std::numeric_limits<T>::quiet_NaN();
    crd::containers::Array<T> a(alloc);
    crd::containers::Array<T> x(alloc);
    crd::containers::Array<T> y(alloc);
    crd::containers::Array<T> y0(alloc);
    Lcg g{0x9EULL};
    for (const auto& level : dn::dense_kernel_levels())
    {
        if (!runs_here(level))
        {
            continue;
        }
        ++c.ran;
        for (const usize rows : {usize{0}, usize{1}, usize{3}, usize{4}, usize{5}, usize{8}, usize{9}, usize{17}})
        {
            for (const usize cols : {usize{0}, usize{1}, w - 1U, w, w + 1U, 2U * w + 3U, usize{600}})
            {
                for (const usize wider : {usize{0}, usize{3}})
                {
                    const usize lda = cols + wider;
                    a.resize(rows * lda + 1U);
                    x.resize(cols + 1U);
                    y.resize(rows + 1U);
                    y0.resize(rows + 1U);
                    for (usize i = 0; i < rows * lda + 1U; ++i)
                    {
                        a[i] = i < rows * lda && i % lda < cols ? g.value<T>() : nan; // padding and the end: NaN
                    }
                    for (usize k = 0; k < cols; ++k)
                    {
                        x[k] = g.value<T>();
                    }
                    x[cols] = nan;
                    for (const T alpha : {T(1), T(0.75)})
                    {
                        const T beta = alpha == T(1) ? T(0) : T(-1.25);
                        for (usize i = 0; i <= rows; ++i)
                        {
                            y[i] = g.value<T>();
                            y0[i] = y[i];
                        }
                        run_gemv(*level.table,
                                 dn::GemvRows<T>{a.data(), x.data(), y.data(), lda, rows, cols, alpha, beta});
                        ++c.cases;
                        for (usize i = 0; i < rows; ++i)
                        {
                            const T want =
                                model_gemv_row(a.data() + i * lda, x.data(), cols, alpha, beta, y0[i], level.level);
                            c.wrong += same_bits(y[i], want) ? 0U : 1U;
                        }
                        c.touched += same_bits(y[rows], y0[rows]) ? 0U : 1U;
                    }
                }
            }
        }
    }
}

template <typename T> void check_symv(crd::memory::IAllocator* alloc, Counts& c)
{
    constexpr usize w = kWidth<T>;
    const T nan = std::numeric_limits<T>::quiet_NaN();
    crd::containers::Array<T> a(alloc);
    crd::containers::Array<T> x(alloc);
    crd::containers::Array<T> y(alloc);
    crd::containers::Array<T> want(alloc);
    Lcg g{0x5BULL};
    for (const auto& level : dn::dense_kernel_levels())
    {
        if (!runs_here(level))
        {
            continue;
        }
        ++c.ran;
        const bool fma = fuses(level.level);
        for (const usize n :
             {usize{0}, usize{1}, usize{2}, w - 1U, w, w + 1U, 4U * w + 3U, 8U * w + 1U, 17U * w + 2U, usize{600}})
        {
            for (const usize wider : {usize{0}, usize{2}})
            {
                const usize lda = n + wider;
                a.resize(n * lda + 1U);
                x.resize(n + 1U);
                y.resize(n + 1U);
                want.resize(n + 1U);
                for (usize i = 0; i < n * lda + 1U; ++i)
                {
                    a[i] = i < n * lda && i % lda <= i / lda ? g.value<T>() : nan; // only the lower triangle
                }
                for (usize i = 0; i < n; ++i)
                {
                    x[i] = g.value<T>();
                }
                x[n] = nan;
                for (usize i = 0; i <= n; ++i)
                {
                    y[i] = g.value<T>();
                }
                const T alpha = T(0.75);
                const T beta = T(-1.25);
                for (usize i = 0; i < n; ++i)
                {
                    want[i] = beta * y[i];
                }
                want[n] = y[n];
                for (usize i = 0; i < n; ++i)
                {
                    const T* row = a.data() + i * lda;
                    const T ax = alpha * x[i];
                    const usize whole = i - i % w;
                    for (usize k = 0; k < i; ++k)
                    {
                        want[k] = k < whole ? step(row[k], ax, want[k], fma) : want[k] + row[k] * ax;
                    }
                    want[i] = want[i] + alpha * (model_dot(row, x.data(), i, level.level) + row[i] * x[i]);
                }
                run_symv(*level.table, dn::SymvLower<T>{a.data(), x.data(), y.data(), lda, n, alpha, beta});
                ++c.cases;
                for (usize i = 0; i < n; ++i)
                {
                    c.wrong += same_bits(y[i], want[i]) ? 0U : 1U;
                }
                c.touched += same_bits(y[n], want[n]) ? 0U : 1U;
            }
        }
    }
}

template <typename T> void check_trsv(crd::memory::IAllocator* alloc, Counts& c)
{
    constexpr usize w = kWidth<T>;
    const T nan = std::numeric_limits<T>::quiet_NaN();
    crd::containers::Array<T> a(alloc);
    crd::containers::Array<T> x(alloc);
    crd::containers::Array<T> want(alloc);
    Lcg g{0x7AULL};
    for (const auto& level : dn::dense_kernel_levels())
    {
        if (!runs_here(level))
        {
            continue;
        }
        ++c.ran;
        for (const usize n : {usize{1}, usize{2}, w + 1U, 4U * w + 3U, 17U * w + 2U, usize{300}})
        {
            for (const bool lower : {true, false})
            {
                for (const bool unit : {false, true})
                {
                    const usize lda = n + 1U;
                    a.resize(n * lda);
                    x.resize(n + 1U);
                    want.resize(n + 1U);
                    for (usize i = 0; i < n; ++i)
                    {
                        for (usize k = 0; k < lda; ++k)
                        {
                            const bool stored = k < n && (lower ? k < i : k > i);
                            a[i * lda + k] = stored ? g.value<T>() / T(n) : nan;
                        }
                        a[i * lda + i] = unit ? nan : T(1.5) + std::fabs(g.value<T>());
                    }
                    for (usize i = 0; i <= n; ++i)
                    {
                        x[i] = g.value<T>();
                        want[i] = x[i];
                    }
                    for (usize step_i = 0; step_i < n; ++step_i)
                    {
                        const usize i = lower ? step_i : n - 1U - step_i;
                        const T* row = a.data() + i * lda;
                        const T s =
                            lower ? want[i] - model_dot(row, want.data(), i, level.level)
                                  : want[i] - model_dot(row + i + 1, want.data() + i + 1, n - i - 1U, level.level);
                        want[i] = unit ? s : s / row[i];
                    }
                    run_trsv(*level.table, dn::TrsvRows<T>{a.data(), x.data(), lda, n, lower, unit});
                    ++c.cases;
                    for (usize i = 0; i < n; ++i)
                    {
                        c.wrong += same_bits(x[i], want[i]) ? 0U : 1U;
                    }
                    c.touched += same_bits(x[n], want[n]) ? 0U : 1U;
                }
            }
        }
    }
}

// A symmetric positive definite lower triangle (B B^T + n I, rows of B in [-9/7, 9/7]) with NaN above the diagonal;
// `fail` > 0 makes column fail - 1 indefinite.
template <typename T>
void spd(crd::containers::Array<T>& a, usize n, usize ld, usize fail, Lcg& g, crd::memory::IAllocator* alloc)
{
    crd::containers::Array<T> b(alloc);
    b.resize(n * n);
    for (usize i = 0; i < n * n; ++i)
    {
        b[i] = g.value<T>();
    }
    a.resize(n * ld);
    for (usize i = 0; i < n; ++i)
    {
        for (usize j = 0; j < ld; ++j)
        {
            T v = std::numeric_limits<T>::quiet_NaN();
            if (j <= i)
            {
                v = i == j ? static_cast<T>(n) : T(0);
                for (usize p = 0; p < n; ++p)
                {
                    v += b[i * n + p] * b[j * n + p];
                }
            }
            a[i * ld + j] = v;
        }
    }
    if (fail > 0U)
    {
        a[(fail - 1U) * ld + (fail - 1U)] = T(-1);
    }
}

template <typename T> usize model_cholesky(T* a, usize n, usize ld, CpuLevel level)
{
    for (usize j = 0; j < n; ++j)
    {
        T* row_j = a + j * ld;
        const T pivot = row_j[j] - model_dot(row_j, row_j, j, level);
        if (!(pivot > T(0)))
        {
            return j + 1U;
        }
        const T diag = std::sqrt(pivot);
        row_j[j] = diag;
        const T inv = T(1) / diag;
        for (usize i = j + 1U; i < n; ++i)
        {
            T* row_i = a + i * ld;
            row_i[j] = (row_i[j] - model_dot(row_i, row_j, j, level)) * inv;
        }
    }
    return 0;
}

template <typename T> void check_cholesky(crd::memory::IAllocator* alloc, Counts& c)
{
    crd::containers::Array<T> a(alloc);
    crd::containers::Array<T> want(alloc);
    for (const auto& level : dn::dense_kernel_levels())
    {
        if (!runs_here(level))
        {
            continue;
        }
        ++c.ran;
        Lcg g{0xC4ULL};
        for (const usize n : {usize{1}, usize{5}, usize{9}, usize{33}, usize{70}})
        {
            for (const usize wider : {usize{0}, usize{3}})
            {
                for (const usize fail : {usize{0}, (n + 1U) / 2U})
                {
                    const usize ld = n + wider;
                    spd<T>(a, n, ld, fail, g, alloc);
                    want.resize(a.size());
                    for (usize i = 0; i < a.size(); ++i)
                    {
                        want[i] = a[i];
                    }
                    const usize info = run_cholesky(*level.table, a.data(), n, ld);
                    ++c.cases;
                    c.wrong += info == model_cholesky(want.data(), n, ld, level.level) ? 0U : 1U;
                    for (usize i = 0; i < a.size(); ++i)
                    {
                        c.wrong += same_bits(a[i], want[i]) ? 0U : 1U; // the upper triangle stays NaN
                    }
                }
            }
        }
    }
}

template <typename T> void check_trsm(crd::memory::IAllocator* alloc, Counts& c)
{
    const T nan = std::numeric_limits<T>::quiet_NaN();
    crd::containers::Array<T> a(alloc);
    crd::containers::Array<T> want(alloc);
    for (const auto& level : dn::dense_kernel_levels())
    {
        if (!runs_here(level))
        {
            continue;
        }
        ++c.ran;
        Lcg g{0x3DULL};
        for (const usize col : {usize{0}, usize{5}})
        {
            for (const usize nb : {usize{1}, usize{7}, usize{33}})
            {
                const usize n = col + nb + 20U;
                const usize ld = n + 2U;
                a.resize(n * ld);
                for (usize i = 0; i < n; ++i)
                {
                    for (usize j = 0; j < ld; ++j)
                    {
                        const bool panel = j >= col && j < col + nb && (i >= col + nb || j - col <= i - col);
                        a[i * ld + j] = panel && i >= col ? g.value<T>() : nan;
                    }
                    if (i >= col && i < col + nb)
                    {
                        a[i * ld + i] = T(1.5) + std::fabs(g.value<T>());
                    }
                }
                const usize first = col + nb + 3U;
                const usize end = n - 4U;
                want.resize(a.size());
                for (usize i = 0; i < a.size(); ++i)
                {
                    want[i] = a[i];
                }
                for (usize i = first; i < end; ++i)
                {
                    T* row_i = want.data() + i * ld + col;
                    for (usize j = 0; j < nb; ++j)
                    {
                        const T* row_j = want.data() + (col + j) * ld + col;
                        row_i[j] = (row_i[j] - model_dot(row_i, row_j, j, level.level)) / row_j[j];
                    }
                }
                run_trsm(*level.table, a.data(), ld, col, nb, first, end);
                ++c.cases;
                for (usize i = 0; i < a.size(); ++i)
                {
                    c.wrong += same_bits(a[i], want[i]) ? 0U : 1U; // rows outside [first, end) unchanged
                }
            }
        }
    }
}

template <typename Check> Counts run(Check check)
{
    crd::memory::TlsfAllocator alloc(16U << 20);
    Counts c;
    check(&alloc, c);
    return c;
}

} // namespace

TEST_CASE("dense kernels: every level the host runs gives its declared dot product bits", "[dense][kernels]")
{
    for (const Counts& c : {run(check_dot<crd::f64>), run(check_dot<crd::f32>)})
    {
        CHECK(c.ran >= 1U);
        CHECK(c.cases > 0U);
        CHECK(c.wrong == 0U);
        CHECK(c.apart == 0U);
    }
}

TEST_CASE("dense kernels: every level the host runs gives its declared gemv bits", "[dense][kernels]")
{
    for (const Counts& c : {run(check_gemv<crd::f64>), run(check_gemv<crd::f32>)})
    {
        CHECK(c.ran >= 1U);
        CHECK(c.cases == c.ran * 8U * 7U * 2U * 2U);
        CHECK(c.wrong == 0U);
        CHECK(c.touched == 0U);
    }
}

TEST_CASE("dense kernels: every level the host runs gives its declared symv bits", "[dense][kernels]")
{
    for (const Counts& c : {run(check_symv<crd::f64>), run(check_symv<crd::f32>)})
    {
        CHECK(c.ran >= 1U);
        CHECK(c.cases == c.ran * 10U * 2U);
        CHECK(c.wrong == 0U);
        CHECK(c.touched == 0U);
    }
}

TEST_CASE("dense kernels: every level the host runs gives its declared triangular solve bits", "[dense][kernels]")
{
    for (const Counts& c : {run(check_trsv<crd::f64>), run(check_trsv<crd::f32>)})
    {
        CHECK(c.ran >= 1U);
        CHECK(c.cases == c.ran * 6U * 4U);
        CHECK(c.wrong == 0U);
        CHECK(c.touched == 0U);
    }
}

TEST_CASE("dense kernels: every level the host runs gives its declared Cholesky bits", "[dense][kernels]")
{
    for (const Counts& c : {run(check_cholesky<crd::f64>), run(check_cholesky<crd::f32>), run(check_trsm<crd::f64>),
                            run(check_trsm<crd::f32>)})
    {
        CHECK(c.ran >= 1U);
        CHECK(c.cases > 0U);
        CHECK(c.wrong == 0U);
    }
}
