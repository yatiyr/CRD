// The known-answer self-check of the hesap-dense kernels (HW-RESILIENCE.3, crd/core/kernel_dispatch.hpp): every
// routine of a table, f64 and f32, on inputs whose every product and sum is an exact small integer, so the levels that
// round each multiply and add and the fused ones give the same bits in any order. Each answer is that integer,
// evaluated at compile time; the inputs are filled from the integer formulas the answers were evaluated from.
//
// The shapes run every loop of every level (dense_kernels.cpp): the vector lengths run a dot's groups of whole
// vectors, one remaining whole vector and a scalar tail at each level's accumulator count and width, for the real and
// the split-complex reductions and the column blocks alike (runs_every_dot); the read-only matrices (gemv, gemvt,
// symv, trsv, cgemvt, chemv) are Hankel matrices, row i at h + i, so a matrix of that order costs a vector. The kernels
// that write a matrix run smaller orders: cher2 and the LDLT updates run whole vectors and a partial one at every
// width, and the Cholesky kernels run a dot's every phase below avx2 and whole vectors and a tail above it (dot runs
// every phase there). A GEMM block takes the table's own panels: 17 rows by 2w + 5 columns (w: avx512's lanes) run a
// whole tile and a partial one in both directions at every tile shape, through each update, both C layouts and lower
// blocks whose diagonal skips, masks and keeps a whole tile.
//
// A zero answer is +0 on every level: every sum starts from a nonzero input or +0, so none is of two -0 terms. Every
// output has a NaN guard after it, and a merge that reads no C starts from NaN, so a stray write or read fails too.
// The scratch is one stack array of under 10 KiB per element type; nothing is allocated.

#include <crd/core/platform.hpp>
#include <crd/hesap/dense/dense_dispatch.hpp>

#include <bit>
#include <limits>
#include <type_traits>

namespace crd::hesap::dense
{
namespace
{

using crd::f32;
using crd::f64;
using crd::i16;
using crd::i64;
using crd::u8;
using crd::usize;

// ---- Inputs ---------------------------------------------------------------------------------------------------------

// The streams of inputs; each fills its arrays with value(k, its salt).
enum class Salt : u8
{
    X,
    Y,
    Hankel,
    Solution,
    Reflector,
    SplitRowRe,
    SplitRowIm,
    SplitVRe,
    SplitVIm,
    SplitHankelRe,
    SplitHankelIm,
    SplitXRe,
    SplitXIm,
    SplitYRe,
    SplitYIm,
    HerRe,
    HerIm,
    HerWRe,
    HerWIm,
    Ldlt,
    Factor,
    Gemm,
    Merge,
};

// Nonzero, at most 2 in magnitude, and without a period that divides a vector width, so no lane repeats another's.
constexpr i64 kValues[7] = {1, -2, 2, -1, -2, 1, 2};

constexpr i64 value(usize k, Salt salt) noexcept
{
    const auto s = static_cast<usize>(salt);
    return kValues[(k * (s % 5U + 2U) + s * 3U + k / 7U) % 7U];
}

template <typename T> void fill(T* out, usize n, Salt salt) noexcept
{
    for (usize k = 0; k < n; ++k)
    {
        out[k] = static_cast<T>(value(k, salt));
    }
}

// The same values for an answer's builder.
template <usize N> struct Stream
{
    i64 v[N];
};

template <usize N> constexpr Stream<N> stream(Salt salt) noexcept
{
    Stream<N> s{};
    for (usize k = 0; k < N; ++k)
    {
        s.v[k] = value(k, salt);
    }
    return s;
}

// The guard after every output, and the C of a merge that must not read it.
template <typename T> constexpr T kGuard = std::numeric_limits<T>::quiet_NaN();

template <typename T> using Bits = std::conditional_t<sizeof(T) == sizeof(crd::u64), crd::u64, crd::u32>;

template <typename T> bool same(T got, T want) noexcept
{
    return std::bit_cast<Bits<T>>(got) == std::bit_cast<Bits<T>>(want);
}

// ---- Shapes ---------------------------------------------------------------------------------------------------------

// The sizes from a vector's lanes at avx512 (kWide) and below it (kNarrow).
template <typename T> struct Sizes
{
    static constexpr usize kWide = 64U / sizeof(T);
    static constexpr usize kNarrow = 32U / sizeof(T);
    static constexpr usize kN = 9U * kWide + kWide / 2U + 1U; // 77 and 153: the vector lengths and matrix orders
    static constexpr usize kOrder = 5U * kNarrow + 5U;        // 25 and 45: the Cholesky kernels' order
    static constexpr usize kLdlt = 2U * kWide + 3U;           // 19 and 35: the LDLT updates' order
    static constexpr usize kHer = kWide + 3U;                 // 11 and 19: cher2's order
    static constexpr usize kCols = 2U * kWide + 5U;           // 21 and 37: a GEMM block's columns
    static constexpr usize kMaxNr = 2U * kWide;               // the widest tile, avx512's
};

// Whether a reduction of n elements over a accumulators of w lanes runs whole groups of a vectors, a whole vector
// after them (a > 1) and a scalar tail.
constexpr bool runs_phases(usize n, usize a, usize w) noexcept
{
    const usize rest = n % (a * w);
    return n >= a * w && (a == 1U || rest >= w) && rest % w != 0U;
}

// dense_kernels.cpp's reductions: a dot over 4 accumulators below avx2 and 8 from it, a split dot over 1 and 2, the
// column blocks of gemvt as the dot's and of cgemvt over 2 and 4; the width is kNarrow up to avx2, kWide at avx512.
template <typename T> constexpr bool runs_every_dot(usize n) noexcept
{
    using S = Sizes<T>;
    return runs_phases(n, 4U, S::kNarrow) && runs_phases(n, 8U, S::kNarrow) && runs_phases(n, 8U, S::kWide) &&
           runs_phases(n, 1U, S::kNarrow) && runs_phases(n, 2U, S::kNarrow) && runs_phases(n, 2U, S::kWide) &&
           runs_phases(n, 4U, S::kWide);
}

static_assert(runs_every_dot<f64>(Sizes<f64>::kN) && runs_every_dot<f32>(Sizes<f32>::kN));
// cholesky_trsm's longest dot (kOrder - 4) runs every phase below avx2.
static_assert(runs_phases(Sizes<f64>::kOrder - 4U, 4U, Sizes<f64>::kNarrow) &&
              runs_phases(Sizes<f32>::kOrder - 4U, 4U, Sizes<f32>::kNarrow));

constexpr usize kGemvRows = 9U;  // one block of eight rows and one more (two of four and one below avx2)
constexpr usize kSweepRows = 3U; // gemvt, ger, cgemvt and cger
constexpr usize kReflectRows = 2U;

// The coefficients: small integers other than 1, so every multiply by them is a real one.
constexpr i64 kAxpyAlpha = -3;
constexpr i64 kGemvAlpha = 2;
constexpr i64 kGemvBeta = -3;
constexpr i64 kGerAlpha = 2;
constexpr i64 kSymvAlpha = -2;
constexpr i64 kSymvBeta = 3;
constexpr i64 kRotC = 2;
constexpr i64 kRotS = -3;
constexpr i64 kReflectTau = 2;
constexpr i64 kSplitTauRe = -1;
constexpr i64 kSplitTauIm = 2;
constexpr i64 kSplitAlphaRe = 2;
constexpr i64 kSplitAlphaIm = -1;

// The answers are stored as i16; a builder clears `fits` when one would not fit, which a static_assert refuses.
struct Answers
{
    bool fits = true;

    constexpr i16 keep(i64 v) noexcept
    {
        fits = fits && v >= std::numeric_limits<i16>::min() && v <= std::numeric_limits<i16>::max();
        return static_cast<i16>(v);
    }
};

// ---- The vector and matrix-vector kernels ---------------------------------------------------------------------------

// dot(x, y), axpy, gemv and gemvt over the Hankel rows (gemvt's x at stride 2), ger onto copies of them, rot, and the
// reflector v = x with tau kReflectTau on two rows of n + 1 values (value(k, Reflector) over both).
template <typename T> struct VectorAnswers : Answers
{
    static constexpr usize kN = Sizes<T>::kN;
    i16 dot = 0;
    i16 axpy[kN];
    i16 gemv[kGemvRows];
    i16 gemvt[kN];
    i16 ger[kSweepRows * kN];
    i16 rot_x[kN];
    i16 rot_y[kN];
    i16 reflect[kReflectRows * (kN + 1U)];
};

template <typename T> constexpr VectorAnswers<T> make_vector_answers() noexcept
{
    constexpr usize n = Sizes<T>::kN;
    constexpr Stream<n> x = stream<n>(Salt::X);
    constexpr Stream<n> y = stream<n>(Salt::Y);
    constexpr Stream<2U * n> h = stream<2U * n>(Salt::Hankel);
    constexpr usize cells = kReflectRows * (n + 1U);
    constexpr Stream<cells> c = stream<cells>(Salt::Reflector);
    VectorAnswers<T> a{};
    i64 dot = 0;
    for (usize k = 0; k < n; ++k)
    {
        dot += x.v[k] * y.v[k];
        a.axpy[k] = a.keep(y.v[k] + kAxpyAlpha * x.v[k]);
        a.rot_x[k] = a.keep(kRotC * x.v[k] + kRotS * y.v[k]);
        a.rot_y[k] = a.keep(kRotC * y.v[k] - kRotS * x.v[k]);
        i64 t = y.v[k];
        for (usize r = 0; r < kSweepRows; ++r)
        {
            t += x.v[2U * r] * h.v[r + k];
            a.ger[r * n + k] = a.keep(h.v[r + k] + kGerAlpha * x.v[r] * y.v[k]);
        }
        a.gemvt[k] = a.keep(t);
    }
    a.dot = a.keep(dot);
    for (usize i = 0; i < kGemvRows; ++i)
    {
        i64 s = 0;
        for (usize k = 0; k < n; ++k)
        {
            s += h.v[i + k] * x.v[k];
        }
        a.gemv[i] = a.keep(kGemvAlpha * s + kGemvBeta * y.v[i]);
    }
    for (usize row = 0; row < cells; row += n + 1U)
    {
        i64 w = c.v[row];
        for (usize k = 0; k < n; ++k)
        {
            w += x.v[k] * c.v[row + 1U + k];
        }
        w *= kReflectTau;
        a.reflect[row] = a.keep(c.v[row] - w);
        for (usize k = 0; k < n; ++k)
        {
            a.reflect[row + 1U + k] = a.keep(c.v[row + 1U + k] - w * x.v[k]);
        }
    }
    return a;
}

// symv over the Hankel matrix.
template <typename T> struct SymvAnswers : Answers
{
    i16 symv[Sizes<T>::kN];
};

template <typename T> constexpr SymvAnswers<T> make_symv_answers() noexcept
{
    constexpr usize n = Sizes<T>::kN;
    constexpr Stream<n> x = stream<n>(Salt::X);
    constexpr Stream<n> y = stream<n>(Salt::Y);
    constexpr Stream<2U * n> h = stream<2U * n>(Salt::Hankel);
    SymvAnswers<T> a{};
    for (usize i = 0; i < n; ++i)
    {
        i64 s = 0;
        for (usize k = 0; k < n; ++k)
        {
            s += h.v[i + k] * x.v[k];
        }
        a.symv[i] = a.keep(kSymvBeta * y.v[i] + kSymvAlpha * s);
    }
    return a;
}

// trsv's right-hand sides: the Hankel matrix's lower and upper triangles, with a unit or its own diagonal, times the
// solution value(k, Solution), in the order lower unit, lower, upper unit, upper.
constexpr usize kTrsvCases = 4U;

template <typename T> struct TrsvAnswers : Answers
{
    i16 b[kTrsvCases][Sizes<T>::kN];
};

template <typename T> constexpr TrsvAnswers<T> make_trsv_answers() noexcept
{
    constexpr usize n = Sizes<T>::kN;
    constexpr Stream<n> s = stream<n>(Salt::Solution);
    constexpr Stream<2U * n> h = stream<2U * n>(Salt::Hankel);
    TrsvAnswers<T> a{};
    for (usize i = 0; i < n; ++i)
    {
        i64 below = 0;
        for (usize k = 0; k < i; ++k)
        {
            below += h.v[i + k] * s.v[k];
        }
        i64 above = 0;
        for (usize k = i + 1U; k < n; ++k)
        {
            above += h.v[i + k] * s.v[k];
        }
        const i64 d = h.v[2U * i] * s.v[i];
        a.b[0][i] = a.keep(below + s.v[i]);
        a.b[1][i] = a.keep(below + d);
        a.b[2][i] = a.keep(above + s.v[i]);
        a.b[3][i] = a.keep(above + d);
    }
    return a;
}

// ---- The split-complex kernels --------------------------------------------------------------------------------------

constexpr i64 complex_re(i64 are, i64 aim, i64 bre, i64 bim) noexcept
{
    return are * bre - aim * bim;
}

constexpr i64 complex_im(i64 are, i64 aim, i64 bre, i64 bim) noexcept
{
    return are * bim + aim * bre;
}

// creflect on two rows of n values (row r at r (n + 1), SplitRow) with v = (SplitV) and tau (kSplitTau); cgemvt over
// the Hankel rows (SplitHankel) with x(r) = (SplitX), plain and conjugated, onto y = (SplitY); cger onto three rows
// (SplitRow) with alpha x(r) times y; cher2 on cher2's order (Her): A - v w^H - w v^H with v = (SplitV), w = (HerW).
template <typename T> struct SplitAnswers : Answers
{
    static constexpr usize kN = Sizes<T>::kN;
    static constexpr usize kHer = Sizes<T>::kHer;
    i16 creflect_re[kReflectRows * kN];
    i16 creflect_im[kReflectRows * kN];
    i16 cgemvt_re[2][kN]; // conj_x false, then true
    i16 cgemvt_im[2][kN];
    i16 cger_re[kSweepRows * kN];
    i16 cger_im[kSweepRows * kN];
    i16 cher2_re[kHer * kHer];
    i16 cher2_im[kHer * kHer];
};

template <typename T> constexpr SplitAnswers<T> make_split_answers() noexcept
{
    constexpr usize n = Sizes<T>::kN;
    constexpr usize ld = n + 1U;
    constexpr usize her = Sizes<T>::kHer;
    SplitAnswers<T> a{};
    for (usize r = 0; r < kReflectRows; ++r)
    {
        i64 ure = 0;
        i64 uim = 0;
        for (usize k = 0; k < n; ++k)
        {
            const i64 cre = value(r * ld + k, Salt::SplitRowRe);
            const i64 cim = value(r * ld + k, Salt::SplitRowIm);
            ure += complex_re(cre, cim, value(k, Salt::SplitVRe), value(k, Salt::SplitVIm));
            uim += complex_im(cre, cim, value(k, Salt::SplitVRe), value(k, Salt::SplitVIm));
        }
        const i64 sre = complex_re(kSplitTauRe, kSplitTauIm, ure, uim);
        const i64 sim = complex_im(kSplitTauRe, kSplitTauIm, ure, uim);
        for (usize k = 0; k < n; ++k) // row - s conj(v)
        {
            const i64 vre = value(k, Salt::SplitVRe);
            const i64 vim = -value(k, Salt::SplitVIm);
            a.creflect_re[r * n + k] = a.keep(value(r * ld + k, Salt::SplitRowRe) - complex_re(sre, sim, vre, vim));
            a.creflect_im[r * n + k] = a.keep(value(r * ld + k, Salt::SplitRowIm) - complex_im(sre, sim, vre, vim));
        }
    }
    for (usize c = 0; c < 2U; ++c)
    {
        for (usize k = 0; k < n; ++k)
        {
            i64 yre = value(k, Salt::SplitYRe);
            i64 yim = value(k, Salt::SplitYIm);
            for (usize r = 0; r < kSweepRows; ++r)
            {
                const i64 xre = value(r, Salt::SplitXRe);
                const i64 xim = c == 0U ? value(r, Salt::SplitXIm) : -value(r, Salt::SplitXIm);
                const i64 hre = value(r + k, Salt::SplitHankelRe);
                const i64 him = value(r + k, Salt::SplitHankelIm);
                yre += complex_re(xre, xim, hre, him);
                yim += complex_im(xre, xim, hre, him);
            }
            a.cgemvt_re[c][k] = a.keep(yre);
            a.cgemvt_im[c][k] = a.keep(yim);
        }
    }
    for (usize r = 0; r < kSweepRows; ++r)
    {
        const i64 sre = complex_re(kSplitAlphaRe, kSplitAlphaIm, value(r, Salt::SplitXRe), value(r, Salt::SplitXIm));
        const i64 sim = complex_im(kSplitAlphaRe, kSplitAlphaIm, value(r, Salt::SplitXRe), value(r, Salt::SplitXIm));
        for (usize k = 0; k < n; ++k)
        {
            const i64 yre = value(k, Salt::SplitYRe);
            const i64 yim = value(k, Salt::SplitYIm);
            a.cger_re[r * n + k] = a.keep(value(r * ld + k, Salt::SplitRowRe) + complex_re(sre, sim, yre, yim));
            a.cger_im[r * n + k] = a.keep(value(r * ld + k, Salt::SplitRowIm) + complex_im(sre, sim, yre, yim));
        }
    }
    for (usize i = 0; i < her; ++i)
    {
        for (usize k = 0; k < her; ++k)
        {
            i64 re = value(i * her + k, Salt::HerRe);
            i64 im = value(i * her + k, Salt::HerIm);
            if (k <= i) // less v(i) conj(w(k)) + w(i) conj(v(k)); the diagonal's imaginary part becomes 0
            {
                const i64 wkre = value(k, Salt::HerWRe);
                const i64 wkim = -value(k, Salt::HerWIm);
                const i64 vkre = value(k, Salt::SplitVRe);
                const i64 vkim = -value(k, Salt::SplitVIm);
                const i64 vire = value(i, Salt::SplitVRe);
                const i64 viim = value(i, Salt::SplitVIm);
                const i64 wire = value(i, Salt::HerWRe);
                const i64 wiim = value(i, Salt::HerWIm);
                re -= complex_re(vire, viim, wkre, wkim) + complex_re(wire, wiim, vkre, vkim);
                im -= complex_im(vire, viim, wkre, wkim) + complex_im(wire, wiim, vkre, vkim);
                im = k == i ? 0 : im;
            }
            a.cher2_re[i * her + k] = a.keep(re);
            a.cher2_im[i * her + k] = a.keep(im);
        }
    }
    return a;
}

// chemv: x = A v for the Hermitian matrix whose lower triangle is the Hankel matrix (SplitHankel), v = (SplitV).
template <typename T> struct ChemvAnswers : Answers
{
    i16 re[Sizes<T>::kN];
    i16 im[Sizes<T>::kN];
};

template <typename T> constexpr ChemvAnswers<T> make_chemv_answers() noexcept
{
    constexpr usize n = Sizes<T>::kN;
    constexpr Stream<2U * n> hre = stream<2U * n>(Salt::SplitHankelRe);
    constexpr Stream<2U * n> him = stream<2U * n>(Salt::SplitHankelIm);
    constexpr Stream<n> vre = stream<n>(Salt::SplitVRe);
    constexpr Stream<n> vim = stream<n>(Salt::SplitVIm);
    ChemvAnswers<T> a{};
    for (usize i = 0; i < n; ++i)
    {
        i64 re = 0;
        i64 im = 0;
        for (usize k = 0; k < n; ++k)
        {
            // (i, k) below the diagonal, conj((k, i)) above it, the real part on it
            i64 aim = 0;
            if (k < i)
            {
                aim = him.v[i + k];
            }
            else if (k > i)
            {
                aim = -him.v[i + k];
            }
            re += complex_re(hre.v[i + k], aim, vre.v[k], vim.v[k]);
            im += complex_im(hre.v[i + k], aim, vre.v[k], vim.v[k]);
        }
        a.re[i] = a.keep(re);
        a.im[i] = a.keep(im);
    }
    return a;
}

// ---- The factorization kernels --------------------------------------------------------------------------------------

// The Cholesky factor L: a diagonal of 1, 2 and 4, so each square root and reciprocal is exact.
constexpr i64 kFactorDiagonal[3] = {1, 2, 4};

constexpr i64 factor(usize i, usize j, usize order) noexcept
{
    if (j > i)
    {
        return 0;
    }
    if (j == i)
    {
        return kFactorDiagonal[i % 3U];
    }
    return value(i * order + j, Salt::Factor);
}

// The LDLT updates' U: u0 and u1 of segment s by s % 5. With the 2 by 2 pivot d11 = 1, d21 = 1, d22 = 3/2 (det 1/2,
// inv_d 2) the factors f0 = 3 u0 - 2 u1 and f1 = 2 u1 - 2 u0 are integers, both nonzero, f0 = 0, f1 = 0, both 0 and
// both nonzero again; the 1 by 1 pivot d11 = 1/2 (inv_d 2) gives f0 = 2 u0.
constexpr i64 kLdltU0[5] = {1, 2, -2, 0, -1};
constexpr i64 kLdltU1[5] = {-2, 3, -2, 0, 1};

constexpr i64 ldlt_f0(usize s) noexcept
{
    return 3 * kLdltU0[s % 5U] - 2 * kLdltU1[s % 5U];
}

constexpr i64 ldlt_f1(usize s) noexcept
{
    return 2 * kLdltU1[s % 5U] - 2 * kLdltU0[s % 5U];
}

// The Cholesky kernels' A = L L^T (its lower triangle), and an order kLdlt block (Ldlt) after the 1 by 1 update of
// every row and after the 2 by 2 trailing update of every column but the last.
template <typename T> struct FactorAnswers : Answers
{
    static constexpr usize kOrder = Sizes<T>::kOrder;
    static constexpr usize kLdlt = Sizes<T>::kLdlt;
    i16 cholesky_a[kOrder * kOrder];
    i16 ldlt_one[kLdlt * kLdlt];
    i16 ldlt_two[kLdlt * kLdlt];
};

template <typename T> constexpr FactorAnswers<T> make_factor_answers() noexcept
{
    constexpr usize order = Sizes<T>::kOrder;
    constexpr usize lo = Sizes<T>::kLdlt;
    FactorAnswers<T> a{};
    i64 l[order * order]{};
    for (usize i = 0; i < order; ++i)
    {
        for (usize j = 0; j <= i; ++j)
        {
            l[i * order + j] = factor(i, j, order);
        }
    }
    for (usize i = 0; i < order; ++i)
    {
        for (usize j = 0; j <= i; ++j)
        {
            i64 s = 0;
            for (usize m = 0; m <= j; ++m)
            {
                s += l[i * order + m] * l[j * order + m];
            }
            a.cholesky_a[i * order + j] = a.keep(s);
        }
    }
    for (usize s = 0; s < lo; ++s)
    {
        for (usize t = 0; t < lo; ++t)
        {
            const i64 v = value(s * lo + t, Salt::Ldlt);
            // row s of the 1 by 1 update, columns 0 to s; stored row s of the trailing update, from column s
            const i64 one = t <= s ? v - 2 * kLdltU0[s % 5U] * kLdltU0[t % 5U] : v;
            const bool trailing = t >= s && s + 1U < lo;
            a.ldlt_one[s * lo + t] = a.keep(one);
            a.ldlt_two[s * lo + t] =
                a.keep(trailing ? v - ldlt_f0(s) * kLdltU0[t % 5U] - ldlt_f1(s) * kLdltU1[t % 5U] : v);
        }
    }
    return a;
}

// ---- The GEMM kernels -----------------------------------------------------------------------------------------------

constexpr usize kRows = 17U; // 14 + 3: a whole and a partial tile at mr 14, 6 and 4
constexpr usize kDepth = 9U; // two four-deep pack steps and one more
constexpr usize kMaxMr = 14U;

// A strided view of the source g: element (r, c) at g[r rs + c cs]. A1 and B1 read rows as runs (cs 1); A2 reads down
// the columns (rs 1, pack_a's interleaved loop) and B2 likewise. The blocks multiply A2's and B2's panels.
struct View
{
    usize rs;
    usize cs;
};

constexpr View kA1{kDepth + 1U, 1U};
constexpr View kA2{1U, kRows + 2U};

template <typename T> constexpr View kB1{Sizes<T>::kCols + 1U, 1U};
constexpr View kB2{1U, kDepth + 2U};

constexpr usize view_end(View v, usize rows, usize cols) noexcept
{
    return (rows - 1U) * v.rs + (cols - 1U) * v.cs + 1U;
}

constexpr usize larger(usize a, usize b) noexcept
{
    return a > b ? a : b;
}

template <typename T>
constexpr usize kSourceValues =
    larger(larger(view_end(kA1, kRows, kDepth), view_end(kA2, kRows, kDepth)),
           larger(view_end(kB1<T>, kDepth, Sizes<T>::kCols), view_end(kB2, kDepth, Sizes<T>::kCols)));

// The product A2 B2, row-major.
template <typename T> struct GemmAnswers : Answers
{
    i16 product[kRows * Sizes<T>::kCols];
};

template <typename T> constexpr GemmAnswers<T> make_gemm_answers() noexcept
{
    constexpr usize cols = Sizes<T>::kCols;
    GemmAnswers<T> a{};
    for (usize i = 0; i < kRows; ++i)
    {
        for (usize j = 0; j < cols; ++j)
        {
            i64 s = 0;
            for (usize p = 0; p < kDepth; ++p)
            {
                s += value(i * kA2.rs + p * kA2.cs, Salt::Gemm) * value(p * kB2.rs + j * kB2.cs, Salt::Gemm);
            }
            a.product[i * cols + j] = a.keep(s);
        }
    }
    return a;
}

template <typename T> constexpr VectorAnswers<T> kVectorAnswers = make_vector_answers<T>();
template <typename T> constexpr SymvAnswers<T> kSymvAnswers = make_symv_answers<T>();
template <typename T> constexpr TrsvAnswers<T> kTrsvAnswers = make_trsv_answers<T>();
template <typename T> constexpr SplitAnswers<T> kSplitAnswers = make_split_answers<T>();
template <typename T> constexpr ChemvAnswers<T> kChemvAnswers = make_chemv_answers<T>();
template <typename T> constexpr FactorAnswers<T> kFactorAnswers = make_factor_answers<T>();
template <typename T> constexpr GemmAnswers<T> kGemmAnswers = make_gemm_answers<T>();

template <typename T> constexpr bool all_fit() noexcept
{
    return kVectorAnswers<T>.fits && kSymvAnswers<T>.fits && kTrsvAnswers<T>.fits && kSplitAnswers<T>.fits &&
           kChemvAnswers<T>.fits && kFactorAnswers<T>.fits && kGemmAnswers<T>.fits;
}

static_assert(all_fit<f64>() && all_fit<f32>(), "an answer outgrew i16");

// The blocks: every update and merge (Scale at beta 0 is the merge that reads no C) in row-major C, so the fused tiles
// run each, two more in column-major C, and two lower blocks: BelowTile (the diagonal nr - 1 columns right of row 0)
// keeps the first tile whole, so it runs fused, and masks the next; AboveTile (the diagonal mr rows down) skips the
// first tile row and masks the second.
enum class Lower : u8
{
    None,
    BelowTile,
    AboveTile,
};

struct GemmMode
{
    GemmUpdate update;
    i64 alpha;
    i64 beta;
    bool col_major;
    Lower lower;
};

constexpr GemmMode kGemmModes[] = {
    {GemmUpdate::Assign, 1, 0, false, Lower::None},      {GemmUpdate::Scale, 2, 0, false, Lower::None},
    {GemmUpdate::Scale, 1, -1, false, Lower::BelowTile}, {GemmUpdate::Subtract, 1, 0, false, Lower::None},
    {GemmUpdate::Add, 1, 0, false, Lower::None},         {GemmUpdate::Add, -2, 0, false, Lower::None},
    {GemmUpdate::Scale, 1, 0, true, Lower::AboveTile},   {GemmUpdate::Scale, -3, 2, true, Lower::None},
};

// ---- The check ------------------------------------------------------------------------------------------------------

// The routines in DenseTypeKernels' order, and the names a failure reports.
enum class Routine : u8
{
    GemmShape,
    PackA,
    PackB,
    Block,
    Dot,
    Axpy,
    Gemv,
    Gemvt,
    Ger,
    Symv,
    Trsv,
    Rot,
    Reflect,
    LdltUpdate,
    Creflect,
    Cgemvt,
    Cger,
    Chemv,
    Cher2,
    Cholesky,
    CholeskyTrsm,
};

constexpr const char* kF64Names[] = {
    "f64 gemm shape", "f64 gemm.pack_a", "f64 gemm.pack_b",  "f64 gemm.block", "f64 dot",  "f64 axpy",
    "f64 gemv",       "f64 gemvt",       "f64 ger",          "f64 symv",       "f64 trsv", "f64 rot",
    "f64 reflect",    "f64 ldlt_update", "f64 creflect",     "f64 cgemvt",     "f64 cger", "f64 chemv",
    "f64 cher2",      "f64 cholesky",    "f64 cholesky_trsm"};
constexpr const char* kF32Names[] = {
    "f32 gemm shape", "f32 gemm.pack_a", "f32 gemm.pack_b",  "f32 gemm.block", "f32 dot",  "f32 axpy",
    "f32 gemv",       "f32 gemvt",       "f32 ger",          "f32 symv",       "f32 trsv", "f32 rot",
    "f32 reflect",    "f32 ldlt_update", "f32 creflect",     "f32 cgemvt",     "f32 cger", "f32 chemv",
    "f32 cher2",      "f32 cholesky",    "f32 cholesky_trsm"};

template <typename T> const char* name(Routine r) noexcept
{
    if constexpr (std::is_same_v<T, f64>)
    {
        return kF64Names[static_cast<usize>(r)];
    }
    else
    {
        return kF32Names[static_cast<usize>(r)];
    }
}

template <typename T> void fill_guarded(T* out, usize n, Salt salt) noexcept
{
    fill(out, n, salt);
    out[n] = kGuard<T>;
}

// `rows` rows of n values and a guard, at stride n + 1, row r from value(r (n + 1) + k, salt).
template <typename T> void fill_rows(T* out, usize rows, usize n, Salt salt) noexcept
{
    for (usize at = 0; at < rows * (n + 1U); ++at)
    {
        out[at] = at % (n + 1U) < n ? static_cast<T>(value(at, salt)) : kGuard<T>;
    }
}

template <typename T> void copy_guarded(T* out, const T* in, usize n) noexcept
{
    for (usize k = 0; k < n; ++k)
    {
        out[k] = in[k];
    }
    out[n] = kGuard<T>;
}

template <typename T> void fill_with_guard(T* out, usize n) noexcept
{
    for (usize k = 0; k < n; ++k)
    {
        out[k] = kGuard<T>;
    }
}

// The first n values of got against the answers, and the guard after them.
template <typename T> bool holds(const T* got, const i16* want, usize n) noexcept
{
    for (usize k = 0; k < n; ++k)
    {
        if (!same(got[k], static_cast<T>(want[k])))
        {
            return false;
        }
    }
    return same(got[n], kGuard<T>);
}

// `rows` rows of n values at stride n + 1 against rows of n answers, and each row's guard.
template <typename T> bool rows_hold(const T* got, const i16* want, usize rows, usize n) noexcept
{
    for (usize r = 0; r < rows; ++r)
    {
        if (!holds(got + r * (n + 1U), want + r * n, n))
        {
            return false;
        }
    }
    return true;
}

// ---- GEMM: the packers, then the blocks on their panels ----

constexpr usize kPackAValues = (kRows + kMaxMr - 1U) * kDepth + 1U;
template <typename T> constexpr usize kPackBValues = (Sizes<T>::kCols + Sizes<T>::kMaxNr - 1U) * kDepth + 1U;
template <typename T> constexpr usize kCValues = larger(kRows * (Sizes<T>::kCols + 2U), (kRows + 2U) * Sizes<T>::kCols);
template <typename T> constexpr usize kGemmValues = kSourceValues<T> + kPackAValues + kPackBValues<T> + kCValues<T>;

template <typename T> bool packed_a_holds(const T* out, const T* g, View v, usize mr) noexcept
{
    const usize padded = (kRows + mr - 1U) / mr * mr;
    for (usize r = 0; r < padded; ++r)
    {
        for (usize p = 0; p < kDepth; ++p)
        {
            if (!same(out[r * kDepth + p], r < kRows ? g[r * v.rs + p * v.cs] : T{}))
            {
                return false;
            }
        }
    }
    return same(out[padded * kDepth], kGuard<T>);
}

template <typename T> bool packed_b_holds(const T* out, const T* g, View v, usize nr) noexcept
{
    constexpr usize cols = Sizes<T>::kCols;
    const usize padded = (cols + nr - 1U) / nr * nr;
    for (usize c = 0; c < padded; ++c)
    {
        for (usize p = 0; p < kDepth; ++p)
        {
            if (!same(out[c / nr * nr * kDepth + p * nr + c % nr], c < cols ? g[p * v.rs + c * v.cs] : T{}))
            {
                return false;
            }
        }
    }
    return same(out[padded * kDepth], kGuard<T>);
}

// One block of mode m over the packed panels. C holds C0 = value(Merge) (NaN when the merge reads no C) with a NaN
// guard around it (ldc two wider); the kept elements end as the integer combination of the product and C0 the update
// makes, the others as they began.
template <typename T>
bool block_holds(const GemmKernels<T>& k, const T* pa, const T* pb, T* c, const GemmMode& m) noexcept
{
    constexpr usize cols = Sizes<T>::kCols;
    const bool reads_c = m.update != GemmUpdate::Assign && (m.update != GemmUpdate::Scale || m.beta != 0);
    i64 p_weight = m.alpha;
    i64 c_weight = 1;
    if (m.update == GemmUpdate::Assign)
    {
        p_weight = 1;
        c_weight = 0;
    }
    else if (m.update == GemmUpdate::Subtract)
    {
        p_weight = -1;
    }
    else if (m.update == GemmUpdate::Scale)
    {
        c_weight = m.beta;
    }
    const usize ldc = m.col_major ? kRows + 2U : cols + 2U;
    const usize outer = m.col_major ? cols : kRows;
    const usize row0 = m.lower == Lower::BelowTile ? k.shape.nr - 1U : 0U;
    const usize col0 = m.lower == Lower::AboveTile ? k.shape.mr : 0U;
    fill_with_guard(c, outer * ldc);
    for (usize i = 0; i < kRows; ++i)
    {
        for (usize j = 0; j < cols; ++j)
        {
            c[m.col_major ? j * ldc + i : i * ldc + j] =
                reads_c ? static_cast<T>(value(i * cols + j, Salt::Merge)) : kGuard<T>;
        }
    }
    k.block(GemmBlock<T>{pa, pb, c, kDepth, ldc, kRows, cols, kDepth, row0, col0, static_cast<T>(m.alpha),
                         static_cast<T>(m.beta), m.update, m.col_major, m.lower != Lower::None});
    for (usize at = 0; at < outer * ldc; ++at)
    {
        const usize i = m.col_major ? at % ldc : at / ldc;
        const usize j = m.col_major ? at / ldc : at % ldc;
        T want = kGuard<T>;
        if (i < kRows && j < cols)
        {
            const i64 c0 = value(i * cols + j, Salt::Merge);
            const bool kept = m.lower == Lower::None || row0 + i >= col0 + j;
            const i64 merged = p_weight * kGemmAnswers<T>.product[i * cols + j] + c_weight * c0;
            if (kept)
            {
                want = static_cast<T>(merged);
            }
            else if (reads_c)
            {
                want = static_cast<T>(c0);
            }
        }
        if (!same(c[at], want))
        {
            return false;
        }
    }
    return true;
}

template <typename T> const char* check_gemm(const GemmKernels<T>& k, T* work) noexcept
{
    if (k.shape.mr == 0U || k.shape.mr > kMaxMr || k.shape.nr == 0U || k.shape.nr > Sizes<T>::kMaxNr)
    {
        return name<T>(Routine::GemmShape);
    }
    T* g = work;
    T* pa = g + kSourceValues<T>;
    T* pb = pa + kPackAValues;
    T* c = pb + kPackBValues<T>;
    fill(g, kSourceValues<T>, Salt::Gemm);
    const View a_views[2] = {kA1, kA2};
    for (const View& v : a_views)
    {
        fill_with_guard(pa, kPackAValues);
        k.pack_a(GemmSource<T>{g, v.rs, v.cs, false}, kRows, kDepth, pa);
        if (!packed_a_holds(pa, g, v, k.shape.mr))
        {
            return name<T>(Routine::PackA);
        }
    }
    const View b_views[2] = {kB1<T>, kB2};
    for (const View& v : b_views)
    {
        fill_with_guard(pb, kPackBValues<T>);
        k.pack_b(GemmSource<T>{g, v.rs, v.cs, false}, kDepth, Sizes<T>::kCols, pb);
        if (!packed_b_holds(pb, g, v, k.shape.nr))
        {
            return name<T>(Routine::PackB);
        }
    }
    for (const GemmMode& m : kGemmModes)
    {
        if (!block_holds(k, pa, pb, c, m))
        {
            return name<T>(Routine::Block);
        }
    }
    return nullptr;
}

// ---- The vector and matrix-vector kernels ----

template <typename T> constexpr usize kVectorValues = 9U * Sizes<T>::kN + 5U;

template <typename T> const char* check_vectors(const DenseTypeKernels<T>& k, T* work) noexcept
{
    constexpr usize n = Sizes<T>::kN;
    const VectorAnswers<T>& a = kVectorAnswers<T>;
    T* x = work;             // n
    T* y = x + n;            // n
    T* h = y + n;            // 2n - 1: the Hankel matrix, row i at h + i
    T* out = h + 2U * n;     // n and a guard
    T* out2 = out + n + 1U;  // n and a guard
    T* rows = out2 + n + 1U; // kSweepRows rows of n and a guard
    fill(x, n, Salt::X);
    fill(y, n, Salt::Y);
    fill(h, 2U * n - 1U, Salt::Hankel);
    if (!same(k.dot(x, y, n), static_cast<T>(a.dot)))
    {
        return name<T>(Routine::Dot);
    }
    copy_guarded(out, y, n);
    k.axpy(out, x, static_cast<T>(kAxpyAlpha), n);
    if (!holds(out, a.axpy, n))
    {
        return name<T>(Routine::Axpy);
    }
    copy_guarded(out, y, kGemvRows);
    k.gemv(GemvRows<T>{h, x, out, 1U, kGemvRows, n, static_cast<T>(kGemvAlpha), static_cast<T>(kGemvBeta)});
    if (!holds(out, a.gemv, kGemvRows))
    {
        return name<T>(Routine::Gemv);
    }
    copy_guarded(out, y, n);
    k.gemvt(GemvtRows<T>{h, x, out, 1U, 2U, kSweepRows, n});
    if (!holds(out, a.gemvt, n))
    {
        return name<T>(Routine::Gemvt);
    }
    for (usize r = 0; r < kSweepRows; ++r)
    {
        copy_guarded(rows + r * (n + 1U), h + r, n);
    }
    k.ger(GerRows<T>{rows, x, y, n + 1U, kSweepRows, n, static_cast<T>(kGerAlpha)});
    if (!rows_hold(rows, a.ger, kSweepRows, n))
    {
        return name<T>(Routine::Ger);
    }
    copy_guarded(out, y, n);
    k.symv(SymvLower<T>{h, x, out, 1U, n, static_cast<T>(kSymvAlpha), static_cast<T>(kSymvBeta)});
    if (!holds(out, kSymvAnswers<T>.symv, n))
    {
        return name<T>(Routine::Symv);
    }
    for (usize t = 0; t < kTrsvCases; ++t)
    {
        for (usize i = 0; i < n; ++i)
        {
            out[i] = static_cast<T>(kTrsvAnswers<T>.b[t][i]);
        }
        out[n] = kGuard<T>;
        k.trsv(TrsvRows<T>{h, out, 1U, n, t < 2U, t % 2U == 0U});
        for (usize i = 0; i <= n; ++i)
        {
            if (!same(out[i], i < n ? static_cast<T>(value(i, Salt::Solution)) : kGuard<T>))
            {
                return name<T>(Routine::Trsv);
            }
        }
    }
    copy_guarded(out, x, n);
    copy_guarded(out2, y, n);
    k.rot(out, out2, static_cast<T>(kRotC), static_cast<T>(kRotS), n);
    if (!holds(out, a.rot_x, n) || !holds(out2, a.rot_y, n))
    {
        return name<T>(Routine::Rot);
    }
    fill_guarded(rows, kReflectRows * (n + 1U), Salt::Reflector);
    k.reflect(ReflectorRows<T>{x, rows, n + 1U, kReflectRows, n, static_cast<T>(kReflectTau)});
    if (!holds(rows, a.reflect, kReflectRows * (n + 1U)))
    {
        return name<T>(Routine::Reflect);
    }
    return nullptr;
}

// ---- The LDLT updates ----

template <typename T> constexpr usize kLdltValues = Sizes<T>::kLdlt * Sizes<T>::kLdlt + 1U + 2U * Sizes<T>::kLdlt;

// The 1 by 1 update of every row, then the 2 by 2 trailing update of every column but the last; the cells an update
// leaves alone keep their values.
template <typename T> bool ldlt_holds(const DenseTypeKernels<T>& k, T* work) noexcept
{
    constexpr usize lo = Sizes<T>::kLdlt;
    T* a = work;              // lo by lo and a guard
    T* u0 = a + lo * lo + 1U; // lo
    T* u1 = u0 + lo;          // lo
    for (usize t = 0; t < lo; ++t)
    {
        u0[t] = static_cast<T>(kLdltU0[t % 5U]);
        u1[t] = static_cast<T>(kLdltU1[t % 5U]);
    }
    const T zero = static_cast<T>(0);
    fill_guarded(a, lo * lo, Salt::Ldlt);
    k.ldlt_update(LdltUpdate<T>{a, u0, nullptr, lo, lo, lo, static_cast<T>(2), static_cast<T>(0.5), zero, zero, false});
    if (!holds(a, kFactorAnswers<T>.ldlt_one, lo * lo))
    {
        return false;
    }
    fill_guarded(a, lo * lo, Salt::Ldlt);
    k.ldlt_update(LdltUpdate<T>{a, u0, u1, lo, lo, lo - 1U, static_cast<T>(2), static_cast<T>(1), static_cast<T>(1),
                                static_cast<T>(1.5), true});
    return holds(a, kFactorAnswers<T>.ldlt_two, lo * lo);
}

// ---- The split-complex kernels ----

template <typename T> constexpr usize kSplitValues = 14U * Sizes<T>::kN + 8U;

// cher2's block and its guard fit in a kSweepRows by n + 1 region.
static_assert(Sizes<f64>::kHer * Sizes<f64>::kHer < kSweepRows * (Sizes<f64>::kN + 1U) &&
              Sizes<f32>::kHer * Sizes<f32>::kHer < kSweepRows * (Sizes<f32>::kN + 1U));

template <typename T> const char* check_split(const DenseTypeKernels<T>& k, T* work) noexcept
{
    constexpr usize n = Sizes<T>::kN;
    constexpr usize ld = n + 1U;
    constexpr usize her = Sizes<T>::kHer;
    const SplitAnswers<T>& a = kSplitAnswers<T>;
    T* vre = work;         // n
    T* vim = vre + n;      // n
    T* hre = vim + n;      // 2n - 1: the Hankel matrices
    T* him = hre + 2U * n; // 2n - 1
    T* re = him + 2U * n;  // kSweepRows rows of n and a guard
    T* im = re + 3U * ld;  // kSweepRows rows of n and a guard
    T* yre = im + 3U * ld; // n and a guard
    T* yim = yre + ld;     // n and a guard
    T xre[kSweepRows];
    T xim[kSweepRows];
    fill(vre, n, Salt::SplitVRe);
    fill(vim, n, Salt::SplitVIm);
    fill(hre, 2U * n - 1U, Salt::SplitHankelRe);
    fill(him, 2U * n - 1U, Salt::SplitHankelIm);
    fill(xre, kSweepRows, Salt::SplitXRe);
    fill(xim, kSweepRows, Salt::SplitXIm);
    fill_rows(re, kReflectRows, n, Salt::SplitRowRe);
    fill_rows(im, kReflectRows, n, Salt::SplitRowIm);
    k.creflect(SplitReflectorRows<T>{re, im, vre, vim, ld, kReflectRows, n, static_cast<T>(kSplitTauRe),
                                     static_cast<T>(kSplitTauIm)});
    if (!rows_hold(re, a.creflect_re, kReflectRows, n) || !rows_hold(im, a.creflect_im, kReflectRows, n))
    {
        return name<T>(Routine::Creflect);
    }
    for (usize c = 0; c < 2U; ++c)
    {
        fill_guarded(yre, n, Salt::SplitYRe);
        fill_guarded(yim, n, Salt::SplitYIm);
        k.cgemvt(SplitGemvtRows<T>{hre, him, xre, xim, yre, yim, 1U, kSweepRows, n, c == 1U});
        if (!holds(yre, a.cgemvt_re[c], n) || !holds(yim, a.cgemvt_im[c], n))
        {
            return name<T>(Routine::Cgemvt);
        }
    }
    fill(yre, n, Salt::SplitYRe);
    fill(yim, n, Salt::SplitYIm);
    fill_rows(re, kSweepRows, n, Salt::SplitRowRe);
    fill_rows(im, kSweepRows, n, Salt::SplitRowIm);
    k.cger(SplitGerRows<T>{re, im, xre, xim, yre, yim, ld, kSweepRows, n, static_cast<T>(kSplitAlphaRe),
                           static_cast<T>(kSplitAlphaIm)});
    if (!rows_hold(re, a.cger_re, kSweepRows, n) || !rows_hold(im, a.cger_im, kSweepRows, n))
    {
        return name<T>(Routine::Cger);
    }
    fill_with_guard(yre, ld);
    fill_with_guard(yim, ld);
    k.chemv(SplitHemvLower<T>{hre, him, vre, vim, yre, yim, 1U, n});
    if (!holds(yre, kChemvAnswers<T>.re, n) || !holds(yim, kChemvAnswers<T>.im, n))
    {
        return name<T>(Routine::Chemv);
    }
    T* wre = yre; // cher2's block in re and im, w in y's space
    T* wim = yim;
    fill_guarded(re, her * her, Salt::HerRe);
    fill_guarded(im, her * her, Salt::HerIm);
    fill(wre, her, Salt::HerWRe);
    fill(wim, her, Salt::HerWIm);
    k.cher2(SplitHer2Lower<T>{re, im, vre, vim, wre, wim, her, her});
    if (!holds(re, a.cher2_re, her * her) || !holds(im, a.cher2_im, her * her))
    {
        return name<T>(Routine::Cher2);
    }
    return nullptr;
}

// ---- The Cholesky kernels ----

template <typename T> constexpr usize kCholeskyValues = Sizes<T>::kOrder * Sizes<T>::kOrder;

// Cell (i, j) of the order kOrder block: L in the lower triangle's first `factored` columns, A in the rest of it (the
// pivot of column zero_pivot less L's square, so that pivot is 0), the guard above it.
template <typename T> T cholesky_cell(usize i, usize j, usize factored, usize zero_pivot) noexcept
{
    constexpr usize order = Sizes<T>::kOrder;
    if (j > i)
    {
        return kGuard<T>;
    }
    const i64 l = factor(i, j, order);
    if (j < factored)
    {
        return static_cast<T>(l);
    }
    return static_cast<T>(kFactorAnswers<T>.cholesky_a[i * order + j] - (j == zero_pivot && i == j ? l * l : 0));
}

template <typename T> bool cholesky_block_holds(const T* a, usize factored, usize zero_pivot) noexcept
{
    constexpr usize order = Sizes<T>::kOrder;
    for (usize at = 0; at < order * order; ++at)
    {
        if (!same(a[at], cholesky_cell<T>(at / order, at % order, factored, zero_pivot)))
        {
            return false;
        }
    }
    return true;
}

// cholesky on A, then on A with the pivot of column c zeroed (it stops there and reports c + 1, the columns before c
// factored); cholesky_trsm on L with the last two rows' columns 1 to order - 3 replaced by their right-hand sides
// (their column 0 taken out), solved against L's block at (1, 1).
template <typename T> const char* check_cholesky(const DenseTypeKernels<T>& k, T* work) noexcept
{
    constexpr usize order = Sizes<T>::kOrder;
    constexpr usize none = order;
    constexpr usize c = order - 3U;
    constexpr usize nb = order - 3U;
    T* a = work;
    const auto fill_block = [a](usize factored, usize zero_pivot)
    {
        for (usize at = 0; at < order * order; ++at)
        {
            a[at] = cholesky_cell<T>(at / order, at % order, factored, zero_pivot);
        }
    };
    fill_block(0U, none);
    if (k.cholesky(a, order, order) != 0U || !cholesky_block_holds(a, order, none))
    {
        return name<T>(Routine::Cholesky);
    }
    fill_block(0U, c);
    if (k.cholesky(a, order, order) != c + 1U || !cholesky_block_holds(a, c, c))
    {
        return name<T>(Routine::Cholesky);
    }
    fill_block(order, none);
    for (usize i = order - 2U; i < order; ++i)
    {
        for (usize j = 1U; j <= nb; ++j)
        {
            a[i * order + j] = static_cast<T>(kFactorAnswers<T>.cholesky_a[i * order + j] -
                                              factor(i, 0U, order) * factor(j, 0U, order));
        }
    }
    k.cholesky_trsm(a, order, 1U, nb, order - 2U, order);
    if (!cholesky_block_holds(a, order, none))
    {
        return name<T>(Routine::CholeskyTrsm);
    }
    return nullptr;
}

template <typename T>
constexpr usize kWorkValues = larger(larger(kGemmValues<T>, kVectorValues<T>),
                                     larger(larger(kLdltValues<T>, kSplitValues<T>), kCholeskyValues<T>));

// One element type's routines in DenseTypeKernels' order; out of line, so the two types' scratch never shares a frame.
template <typename T> CRD_NOINLINE const char* check_type(const DenseTypeKernels<T>& k) noexcept
{
    T work[kWorkValues<T>];
    if (const char* failed = check_gemm(k.gemm, work))
    {
        return failed;
    }
    if (const char* failed = check_vectors(k, work))
    {
        return failed;
    }
    if (!ldlt_holds(k, work))
    {
        return name<T>(Routine::LdltUpdate);
    }
    if (const char* failed = check_split(k, work))
    {
        return failed;
    }
    return check_cholesky(k, work);
}

} // namespace

const char* dense_self_check(const DenseKernels& table) noexcept
{
    const char* failed = check_type(table.f64_kernels);
    return failed != nullptr ? failed : check_type(table.f32_kernels);
}

} // namespace crd::hesap::dense
