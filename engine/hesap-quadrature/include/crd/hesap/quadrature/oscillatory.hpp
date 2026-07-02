#pragma once

// crd-hesap-quadrature v13-j — OSCILLATORY + SINGULAR-WEIGHT adaptive integration (the QUADPACK weighted family):
//   QAWO  ∫_a^b f(x)·{cos,sin}(ωx) dx      — modified Clenshaw-Curtis (Chebyshev moments of cos/sin), ω-robust.
//   QAWF  ∫_a^∞ f(x)·{cos,sin}(ωx) dx      — Fourier tail: QAWO over half-period cycles + Wynn-ε acceleration.
//   QAWS  ∫_a^b (x-a)^α(b-x)^β[log…] f(x)  — algebraico-logarithmic endpoint weights (modified Chebyshev moments).
//   QAWC  PV ∫_a^b f(x)/(x-c) dx           — Cauchy principal value (generalized Clenshaw-Curtis near c).
//
// Each is a faithful, goto-free transliteration of QUADPACK dqawoe/dqawfe/dqawse/dqawce (+ dqc25f/dqc25s/dqc25c +
// dqcheb/dqmomo/dqk15w/dgtsl), reconstructed-and-verified bit-exact in python vs scipy.integrate.quad(weight=…)
// BEFORE this port (the v7 NLopt-port / v13 reconstruct-first discipline). The reconstruction caught (1) the QUADPACK
// dqc25s `res24=res12+…` transcription typo — corrected here to `res24+=…`; (2) the dqawoe `done`→global-sum
// fall-through (a converged result must be the Σ rlist, not the stale first-panel estimate); (3) the dqawoe jupbnd
// "next interval" search must cycle the MAIN loop on width>small (the dqagse/scipy semantics), not the inner loop.
//
// THREE MOAT PILLARS (ADR-0095): (1) determinism — fixed FP order, crd::math (never std::), constant nodes as
// literals ⇒ bit-identical across compiler/opt/threads; (2) allocation-free streaming — the caller OscWorkspace
// (subinterval lists + the integrand-independent Chebyshev-moment cache `chebmo`) is allocated ONCE and reused; the
// adaptive driver is an ITERATIVE bounded-depth work-stack (limit = the WCET knob), never recursion; (3)
// error-tier-exposing — QuadResult{value, error_estimate (Tier-1, NOT a bound), status, eval_count, subdiv_count}.
//
// CRUSH lever (the v13 recurring one): the GK15w error heuristic `(200·abserr/resasc)^1.5` is the heavy double-double
// pow ⇒ replaced with `rat·√rat` (one hardware sqrt) — the same per-call cost that flipped QAGS/QAGI vs GSL. The
// Chebyshev moments are integrand-INDEPENDENT (depend only on ω·hlgth / α,β) ⇒ cached once per interval-length level.

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/quadrature/gauss_kronrod.hpp> // detail::qmin / qmax / quad_finite are in integrate.hpp/gk
#include <crd/hesap/quadrature/integrate.hpp>
#include <crd/hesap/quadrature/qags.hpp> // AdaptiveWorkspace pattern (we define our own 1-based OscWorkspace)
#include <crd/math/cmath.hpp>
#include <crd/memory/allocator.hpp>

#include <limits>
#include <utility>

namespace crd::hesap::quadrature
{

// Which trigonometric weight QAWO/QAWF apply: cos(ωx) or sin(ωx).
enum class OscWeight : crd::u8
{
    Cos = 1,
    Sin = 2,
};

// Which algebraico-logarithmic endpoint weight QAWS applies (w(x) = (x-a)^α (b-x)^β · …):
enum class AlgLogWeight : crd::u8
{
    Pow = 1,     // (x-a)^α (b-x)^β
    LogXmA = 2,  // · log(x-a)
    LogBmX = 3,  // · log(b-x)
    LogBoth = 4, // · log(x-a) · log(b-x)
};

// The reusable oscillatory/singular workspace: the adaptive subinterval lists (1-based, sized limit+1) + the
// integrand-INDEPENDENT Chebyshev-moment cache `chebmo` (maxp1×25, for QAWO/QAWF) + its level counter momcom.
// Allocate ONCE, integrate many — the allocation-free hot path (ADR-0095 pillar 2). QAWS/QAWC leave chebmo/nnlog
// unused. The per-call-allocating convenience overloads create one internally.
template <typename T> struct OscWorkspace
{
    crd::containers::Array<T> alist, blist, rlist, elist;
    crd::containers::Array<int> iord, nnlog;
    crd::containers::Array<T> chebmo; // maxp1 * 25, flat (1-based access via cheb())
    int limit;
    int maxp1;
    int momcom;
    // The Chebyshev moments depend ONLY on (|ω|, b−a) — not the integrand. Cache that key so a repeated QAWO call with
    // the same key REUSES the moments (QUADPACK icall>1) instead of recomputing them — the v13 "precompute integrand-
    // independent work" crush lever (GSL caches the same data in its qawo_table at alloc). Sentinel −1 = invalid.
    T cached_domega;
    T cached_len;
    // QAWS modified moments depend ONLY on (α, β, weight-type) — also integrand-independent (GSL caches them in its
    // qaws_table). Cache them here keyed by (α, β, integr) so a repeated QAWS call skips dqmomo. Sentinel α=−2 =
    // invalid.
    T qaws_ri[26], qaws_rj[26], qaws_rg[26], qaws_rh[26];
    T cached_alfa, cached_beta;
    int cached_integr;

    OscWorkspace(crd::memory::IAllocator* alloc, int subdivision_limit, int chebmo_levels = 21)
        : alist(alloc), blist(alloc), rlist(alloc), elist(alloc), iord(alloc), nnlog(alloc), chebmo(alloc),
          limit(subdivision_limit < 1 ? 1 : subdivision_limit), maxp1(chebmo_levels < 1 ? 1 : chebmo_levels), momcom(0),
          cached_domega(T{-1}), cached_len(T{-1}), qaws_ri{}, qaws_rj{}, qaws_rg{}, qaws_rh{}, cached_alfa(T{-2}),
          cached_beta(T{-2}), cached_integr(0)
    {
        const crd::usize n = static_cast<crd::usize>(limit) + 1; // 1-based: index 0 unused
        alist.resize(n);
        blist.resize(n);
        rlist.resize(n);
        elist.resize(n);
        iord.resize(n);
        nnlog.resize(n);
        chebmo.resize(static_cast<crd::usize>(maxp1) * 25);
    }

    // 1-based Chebyshev moment accessor: m in [1, maxp1], k in [1, 25].
    [[nodiscard]] T& cheb(int m, int k) noexcept { return chebmo[static_cast<crd::usize>((m - 1) * 25 + (k - 1))]; }
    [[nodiscard]] const T& cheb(int m, int k) const noexcept
    {
        return chebmo[static_cast<crd::usize>((m - 1) * 25 + (k - 1))];
    }
};

namespace detail
{

// cos(k·π/24), k = 1..11 (1-based; index 0 unused) — the Clenshaw-Curtis abscissae shared by every dqc25* panel.
// Hard literals (not crd::math::cos at init) so the moment tables are bit-identical across platforms (pillar 1).
template <typename T> struct CcX
{
    static constexpr T kX[12] = {static_cast<T>(0),
                                 static_cast<T>(0.9914448613738104111445575269285629), // cos(1*pi/24)
                                 static_cast<T>(0.9659258262890682867497431997288974), // cos(2*pi/24)
                                 static_cast<T>(0.9238795325112867561281831893967883), // cos(3*pi/24)
                                 static_cast<T>(0.8660254037844386467637231707529362), // cos(4*pi/24)
                                 static_cast<T>(0.7933533402912351645797769615012993), // cos(5*pi/24)
                                 static_cast<T>(0.707106781186547524400844362104849),  // cos(6*pi/24)
                                 static_cast<T>(0.608761429008720639416097542898164),  // cos(7*pi/24)
                                 static_cast<T>(0.5),                                  // cos(8*pi/24)
                                 static_cast<T>(0.3826834323650897717284599840303989), // cos(9*pi/24)
                                 static_cast<T>(0.2588190451025207623488988376240483), // cos(10*pi/24)
                                 static_cast<T>(0.130526192220051591548406227895489)}; // cos(11*pi/24)
};

// 15-point Gauss-Kronrod abscissae/weights for the WEIGHTED rule dqk15w (1-based; index 0 unused).
template <typename T> struct Gk15w
{
    static constexpr T kXgk[9] = {static_cast<T>(0),
                                  static_cast<T>(9.91455371120812639206854697526328516642e-1),
                                  static_cast<T>(9.49107912342758524526189684047851262401e-1),
                                  static_cast<T>(8.64864423359769072789712788640926201211e-1),
                                  static_cast<T>(7.41531185599394439863864773280788407074e-1),
                                  static_cast<T>(5.86087235467691130294144838258729598437e-1),
                                  static_cast<T>(4.05845151377397166906606412076961463347e-1),
                                  static_cast<T>(2.07784955007898467600689403773244913480e-1),
                                  static_cast<T>(0)};
    static constexpr T kWgk[9] = {static_cast<T>(0),
                                  static_cast<T>(2.29353220105292249637320080589695919936e-2),
                                  static_cast<T>(6.30920926299785532907006631892042866651e-2),
                                  static_cast<T>(1.04790010322250183839876322541518017444e-1),
                                  static_cast<T>(1.40653259715525918745189590510237920400e-1),
                                  static_cast<T>(1.69004726639267902826583426598550284106e-1),
                                  static_cast<T>(1.90350578064785409913256402421013682826e-1),
                                  static_cast<T>(2.04432940075298892414161999234649084717e-1),
                                  static_cast<T>(2.09482141084727828012999174891714263698e-1)};
    static constexpr T kWg[5] = {static_cast<T>(0), static_cast<T>(1.29484966168869693270611432679082018329e-1),
                                 static_cast<T>(2.79705391489276667901467771423779582487e-1),
                                 static_cast<T>(3.81830050505118944950369775488975133878e-1),
                                 static_cast<T>(4.17959183673469387755102040816326530612e-1)};
};

constexpr int kLimexp = 50;

// Fast deterministic x^y for the QAWS algebraic weight (x > 0): exp(y·log(x)) over crd::math's exp (1.05× libm) +
// log (1.6× libm) — ~8 ns vs crd::math::pow's ~20 ns double-double (which is 2.3× SLOWER than libm pow). The weight is
// evaluated 15-25×/panel ⇒ this is the v13-j QAWS crush lever (the double-double accuracy is wasted at a 1e-10 tol).
// Stays bit-deterministic (crd::math, not std::). x is always strictly inside (a,b) here (CC fix±u / interior GK
// nodes).
template <typename T> [[nodiscard]] inline T wpow(T x, T y) noexcept
{
    return crd::math::exp(y * crd::math::log(x));
}

// dqcheb — Chebyshev series (degree 12 + 24) of f tabulated at the 25 Clenshaw-Curtis points. 1-based: x[1..11],
// fval[1..25] (mutated), cheb12[1..13], cheb24[1..25]. Faithful transliteration.
template <typename T> void dqcheb(const T* x, T* fval, T* cheb12, T* cheb24)
{
    T v[13];
    for (int i = 1; i <= 12; ++i)
    {
        const int j = 26 - i;
        v[i] = fval[i] - fval[j];
        fval[i] = fval[i] + fval[j];
    }
    T alam1 = v[1] - v[9];
    T alam2 = x[6] * (v[3] - v[7] - v[11]);
    cheb12[4] = alam1 + alam2;
    cheb12[10] = alam1 - alam2;
    alam1 = v[2] - v[8] - v[10];
    alam2 = v[4] - v[6] - v[12];
    T alam = x[3] * alam1 + x[9] * alam2;
    cheb24[4] = cheb12[4] + alam;
    cheb24[22] = cheb12[4] - alam;
    alam = x[9] * alam1 - x[3] * alam2;
    cheb24[10] = cheb12[10] + alam;
    cheb24[16] = cheb12[10] - alam;
    const T part1 = x[4] * v[5];
    const T part2 = x[8] * v[9];
    const T part3 = x[6] * v[7];
    alam1 = v[1] + part1 + part2;
    alam2 = x[2] * v[3] + part3 + x[10] * v[11];
    cheb12[2] = alam1 + alam2;
    cheb12[12] = alam1 - alam2;
    alam = x[1] * v[2] + x[3] * v[4] + x[5] * v[6] + x[7] * v[8] + x[9] * v[10] + x[11] * v[12];
    cheb24[2] = cheb12[2] + alam;
    cheb24[24] = cheb12[2] - alam;
    alam = x[11] * v[2] - x[9] * v[4] + x[7] * v[6] - x[5] * v[8] + x[3] * v[10] - x[1] * v[12];
    cheb24[12] = cheb12[12] + alam;
    cheb24[14] = cheb12[12] - alam;
    alam1 = v[1] - part1 + part2;
    alam2 = x[10] * v[3] - part3 + x[2] * v[11];
    cheb12[6] = alam1 + alam2;
    cheb12[8] = alam1 - alam2;
    alam = x[5] * v[2] - x[9] * v[4] - x[1] * v[6] - x[11] * v[8] + x[3] * v[10] + x[7] * v[12];
    cheb24[6] = cheb12[6] + alam;
    cheb24[20] = cheb12[6] - alam;
    alam = x[7] * v[2] - x[3] * v[4] - x[11] * v[6] + x[1] * v[8] - x[9] * v[10] - x[5] * v[12];
    cheb24[8] = cheb12[8] + alam;
    cheb24[18] = cheb12[8] - alam;
    for (int i = 1; i <= 6; ++i)
    {
        const int j = 14 - i;
        v[i] = fval[i] - fval[j];
        fval[i] = fval[i] + fval[j];
    }
    alam1 = v[1] + x[8] * v[5];
    alam2 = x[4] * v[3];
    cheb12[3] = alam1 + alam2;
    cheb12[11] = alam1 - alam2;
    cheb12[7] = v[1] - v[5];
    alam = x[2] * v[2] + x[6] * v[4] + x[10] * v[6];
    cheb24[3] = cheb12[3] + alam;
    cheb24[23] = cheb12[3] - alam;
    alam = x[6] * (v[2] - v[4] - v[6]);
    cheb24[7] = cheb12[7] + alam;
    cheb24[19] = cheb12[7] - alam;
    alam = x[10] * v[2] - x[6] * v[4] + x[2] * v[6];
    cheb24[11] = cheb12[11] + alam;
    cheb24[15] = cheb12[11] - alam;
    for (int i = 1; i <= 3; ++i)
    {
        const int j = 8 - i;
        v[i] = fval[i] - fval[j];
        fval[i] = fval[i] + fval[j];
    }
    cheb12[5] = v[1] + x[8] * v[3];
    cheb12[9] = fval[1] - x[8] * fval[3];
    alam = x[4] * v[2];
    cheb24[5] = cheb12[5] + alam;
    cheb24[21] = cheb12[5] - alam;
    alam = x[8] * fval[2] - fval[4];
    cheb24[9] = cheb12[9] + alam;
    cheb24[17] = cheb12[9] - alam;
    cheb12[1] = fval[1] + fval[3];
    alam = fval[2] + fval[4];
    cheb24[1] = cheb12[1] + alam;
    cheb24[25] = cheb12[1] - alam;
    cheb12[13] = v[1] - v[3];
    cheb24[13] = cheb12[13];
    alam = static_cast<T>(1) / static_cast<T>(6);
    for (int i = 2; i <= 12; ++i)
    {
        cheb12[i] = cheb12[i] * alam;
    }
    alam = static_cast<T>(0.5) * alam;
    cheb12[1] = cheb12[1] * alam;
    cheb12[13] = cheb12[13] * alam;
    for (int i = 2; i <= 24; ++i)
    {
        cheb24[i] = cheb24[i] * alam;
    }
    cheb24[1] = static_cast<T>(0.5) * alam * cheb24[1];
    cheb24[25] = static_cast<T>(0.5) * alam * cheb24[25];
}

// dqmomo — the modified Chebyshev moments of the algebraico-log weight (1-based ri/rj/rg/rh[1..25]). Forward
// recurrence. integr 1..4 selects which of ri,rj,rg,rh to compute (ri,rj always).
template <typename T> void dqmomo(T alfa, T beta, int integr, T* ri, T* rj, T* rg, T* rh)
{
    const T alfp1 = alfa + T{1};
    const T betp1 = beta + T{1};
    const T alfp2 = alfa + T{2};
    const T betp2 = beta + T{2};
    const T ralf = wpow(T{2}, alfp1); // exp(·log 2) — avoids the double-double pow (the QAWS dqmomo lever)
    const T rbet = wpow(T{2}, betp1);
    ri[1] = ralf / alfp1;
    rj[1] = rbet / betp1;
    ri[2] = ri[1] * alfa / alfp2;
    rj[2] = rj[1] * beta / betp2;
    T an = T{2};
    T anm1 = T{1};
    for (int i = 3; i <= 25; ++i)
    {
        ri[i] = -(ralf + an * (an - alfp2) * ri[i - 1]) / (anm1 * (an + alfp1));
        rj[i] = -(rbet + an * (an - betp2) * rj[i - 1]) / (anm1 * (an + betp1));
        anm1 = an;
        an = an + T{1};
    }
    if (integr != 1)
    {
        if (integr != 3)
        {
            rg[1] = -ri[1] / alfp1;
            rg[2] = -(ralf + ralf) / (alfp2 * alfp2) - rg[1];
            an = T{2};
            anm1 = T{1};
            int im1 = 2;
            for (int i = 3; i <= 25; ++i)
            {
                rg[i] = -(an * (an - alfp2) * rg[im1] - an * ri[im1] + anm1 * ri[i]) / (anm1 * (an + alfp1));
                anm1 = an;
                an = an + T{1};
                im1 = i;
            }
        }
        if (integr != 2)
        {
            rh[1] = -rj[1] / betp1;
            rh[2] = -(rbet + rbet) / (betp2 * betp2) - rh[1];
            an = T{2};
            anm1 = T{1};
            int im1 = 2;
            for (int i = 3; i <= 25; ++i)
            {
                rh[i] = -(an * (an - betp2) * rh[im1] - an * rj[im1] + anm1 * rj[i]) / (anm1 * (an + betp1));
                anm1 = an;
                an = an + T{1};
                im1 = i;
            }
            for (int i = 2; i <= 25; i += 2)
            {
                rh[i] = -rh[i];
            }
        }
    }
    for (int i = 2; i <= 25; i += 2)
    {
        rj[i] = -rj[i];
    }
}

// dgtsl — LINPACK tridiagonal solve with partial pivoting (1-based c/d/e/b[1..n]; all mutated, b ← solution).
template <typename T> int dgtsl(int n, T* c, T* d, T* e, T* b)
{
    c[1] = d[1];
    const int nm1 = n - 1;
    if (nm1 >= 1)
    {
        d[1] = e[1];
        e[1] = T{0};
        e[n] = T{0};
        for (int k = 1; k <= nm1; ++k)
        {
            const int kp1 = k + 1;
            if (crd::math::fabs(c[kp1]) >= crd::math::fabs(c[k]))
            {
                T t = c[kp1];
                c[kp1] = c[k];
                c[k] = t;
                t = d[kp1];
                d[kp1] = d[k];
                d[k] = t;
                t = e[kp1];
                e[kp1] = e[k];
                e[k] = t;
                t = b[kp1];
                b[kp1] = b[k];
                b[k] = t;
            }
            if (c[k] == T{0})
            {
                return k;
            }
            const T t = -c[kp1] / c[k];
            c[kp1] = d[kp1] + t * d[k];
            d[kp1] = e[kp1] + t * e[k];
            e[kp1] = T{0};
            b[kp1] = b[kp1] + t * b[k];
        }
    }
    if (c[n] == T{0})
    {
        return n;
    }
    const int nm2 = n - 2;
    b[n] = b[n] / c[n];
    if (n != 1)
    {
        b[nm1] = (b[nm1] - d[nm1] * b[n]) / c[nm1];
        if (nm2 >= 1)
        {
            for (int kb = 1; kb <= nm2; ++kb)
            {
                const int k = nm2 - kb + 1;
                b[k] = (b[k] - d[k] * b[k + 1] - e[k] * b[k + 2]) / c[k];
            }
        }
    }
    return 0;
}

// dqk15w — 15-point Gauss-Kronrod with a multiplicative weight w(x) (callable T→T). The x·√x crush lever replaces
// QUADPACK's (200·abserr/resasc)^1.5 heavy pow. Returns value/abserr; resabs/resasc via out-params.
template <typename T, typename F, typename W>
void dqk15w(F&& f, W&& w, T a, T b, T& result, T& abserr, T& resabs, T& resasc)
{
    using K = Gk15w<T>;
    const T centr = static_cast<T>(0.5) * (a + b);
    const T hlgth = static_cast<T>(0.5) * (b - a);
    const T dhlgth = crd::math::fabs(hlgth);
    T fv1[8];
    T fv2[8];
    const T fc = f(centr) * w(centr);
    T resg = K::kWg[4] * fc;
    T resk = K::kWgk[8] * fc;
    resabs = crd::math::fabs(resk);
    for (int j = 1; j <= 3; ++j)
    {
        const int jtw = j * 2;
        const T absc = hlgth * K::kXgk[jtw];
        const T absc1 = centr - absc;
        const T absc2 = centr + absc;
        const T fval1 = f(absc1) * w(absc1);
        const T fval2 = f(absc2) * w(absc2);
        fv1[jtw] = fval1;
        fv2[jtw] = fval2;
        const T fsum = fval1 + fval2;
        resg += K::kWg[j] * fsum;
        resk += K::kWgk[jtw] * fsum;
        resabs += K::kWgk[jtw] * (crd::math::fabs(fval1) + crd::math::fabs(fval2));
    }
    for (int j = 1; j <= 4; ++j)
    {
        const int jtwm1 = j * 2 - 1;
        const T absc = hlgth * K::kXgk[jtwm1];
        const T absc1 = centr - absc;
        const T absc2 = centr + absc;
        const T fval1 = f(absc1) * w(absc1);
        const T fval2 = f(absc2) * w(absc2);
        fv1[jtwm1] = fval1;
        fv2[jtwm1] = fval2;
        const T fsum = fval1 + fval2;
        resk += K::kWgk[jtwm1] * fsum;
        resabs += K::kWgk[jtwm1] * (crd::math::fabs(fval1) + crd::math::fabs(fval2));
    }
    const T reskh = resk * static_cast<T>(0.5);
    resasc = K::kWgk[8] * crd::math::fabs(fc - reskh);
    for (int j = 1; j <= 7; ++j)
    {
        resasc += K::kWgk[j] * (crd::math::fabs(fv1[j] - reskh) + crd::math::fabs(fv2[j] - reskh));
    }
    result = resk * hlgth;
    resabs = resabs * dhlgth;
    resasc = resasc * dhlgth;
    abserr = crd::math::fabs((resk - resg) * hlgth);
    if (resasc != T{0} && abserr != T{0})
    {
        // Faithful QUADPACK (200·abserr/resasc)^1.5 via crd::math::pow — NOT the x·√x lever here: this rule's abserr
        // drives the adaptive subdivision, and a ~1-ulp change cascades to a ~1e-7 divergence vs scipy on sensitive
        // oscillatory integrands. The pow is on the rare GK15-fallback panel (the CC path, which has no pow,
        // dominates).
        const T rat = static_cast<T>(200) * abserr / resasc;
        abserr = resasc * qmin<T>(T{1}, crd::math::pow(rat, static_cast<T>(1.5)));
    }
    const T uflow = std::numeric_limits<T>::min();
    const T epmach = std::numeric_limits<T>::epsilon();
    if (resabs > uflow / (static_cast<T>(50) * epmach))
    {
        abserr = qmax<T>(epmach * static_cast<T>(50) * resabs, abserr);
    }
}

// dqc25f — 25-point modified Clenshaw-Curtis for ∫_a^b f·{cos,sin}(ωx) (or GK15w when |ω·hlgth| ≤ 2). The Chebyshev
// moments are cached in ws.chebmo per interval-length level (integrand-independent). integr: 1=cos, 2=sin.
template <typename T, typename F>
void dqc25f(F&& f, T a, T b, T omega, int integr, int nrmom, int ksave, OscWorkspace<T>& ws, T& result, T& abserr,
            int& neval, T& resabs, T& resasc)
{
    using X = CcX<T>;
    const T centr = static_cast<T>(0.5) * (b + a);
    const T hlgth = static_cast<T>(0.5) * (b - a);
    const T parint = omega * hlgth;
    const T oflow = std::numeric_limits<T>::max();
    const int maxp1 = ws.maxp1;
    if (crd::math::fabs(parint) > T{2})
    {
        const T conc = hlgth * crd::math::cos(centr * omega);
        const T cons = hlgth * crd::math::sin(centr * omega);
        resasc = oflow;
        neval = 25;
        int m = ws.momcom + 1;
        if (nrmom >= ws.momcom && ksave != 1)
        {
            const T par2 = parint * parint;
            const T par22 = par2 + T{2};
            const T sinpar = crd::math::sin(parint);
            const T cospar = crd::math::cos(parint);
            T v[29] = {};
            // cosine moments
            v[1] = T{2} * sinpar / parint;
            v[2] = (T{8} * cospar + (par2 + par2 - T{8}) * sinpar / parint) / par2;
            v[3] = (T{32} * (par2 - T{12}) * cospar + (T{2} * ((par2 - T{80}) * par2 + T{192}) * sinpar) / parint) /
                   (par2 * par2);
            T ac = T{8} * cospar;
            T as = T{24} * parint * sinpar;
            if (crd::math::fabs(parint) > T{24})
            {
                T an = T{4};
                for (int i = 4; i <= 13; ++i)
                {
                    const T an2 = an * an;
                    v[i] = ((an2 - T{4}) * (T{2} * (par22 - an2 - an2) * v[i - 1] - ac) + as -
                            par2 * (an + T{1}) * (an + T{2}) * v[i - 2]) /
                           (par2 * (an - T{1}) * (an - T{2}));
                    an += T{2};
                }
            }
            else
            {
                const int noequ = 25;
                const int noeq1 = noequ - 1;
                T d[26] = {};
                T d1[26] = {};
                T d2[26] = {};
                T an = T{6};
                for (int k = 1; k <= noeq1; ++k)
                {
                    const T an2 = an * an;
                    d[k] = T{-2} * (an2 - T{4}) * (par22 - an2 - an2);
                    d2[k] = (an - T{1}) * (an - T{2}) * par2;
                    d1[k + 1] = (an + T{3}) * (an + T{4}) * par2;
                    v[k + 3] = as - (an2 - T{4}) * ac;
                    an += T{2};
                }
                T an2 = an * an;
                d[noequ] = T{-2} * (an2 - T{4}) * (par22 - an2 - an2);
                v[noequ + 3] = as - (an2 - T{4}) * ac;
                v[4] = v[4] - static_cast<T>(56) * par2 * v[3];
                const T ass = parint * sinpar;
                const T asap = (((((static_cast<T>(210) * par2 - T{1}) * cospar -
                                   (static_cast<T>(105) * par2 - static_cast<T>(63)) * ass) /
                                      an2 -
                                  (T{1} - static_cast<T>(15) * par2) * cospar + static_cast<T>(15) * ass) /
                                     an2 -
                                 cospar + T{3} * ass) /
                                    an2 -
                                cospar) /
                               an2;
                v[noequ + 3] = v[noequ + 3] - T{2} * asap * par2 * (an - T{1}) * (an - T{2});
                // solve tridiag on v[4..28] (noequ unknowns): dgtsl(noequ, c=d1, d=d, e=d2, b=v+3)
                T c1[26], d1d[26], e1[26], bb[26];
                for (int k = 1; k <= noequ; ++k)
                {
                    c1[k] = d1[k];
                    d1d[k] = d[k];
                    e1[k] = d2[k];
                    bb[k] = v[k + 3];
                }
                dgtsl<T>(noequ, c1, d1d, e1, bb);
                for (int k = 1; k <= noequ; ++k)
                {
                    v[k + 3] = bb[k];
                }
            }
            for (int j = 1; j <= 13; ++j)
            {
                ws.cheb(m, 2 * j - 1) = v[j];
            }
            // sine moments
            T vs[29] = {};
            vs[1] = T{2} * (sinpar - parint * cospar) / par2;
            vs[2] = (static_cast<T>(18) - static_cast<T>(48) / par2) * sinpar / par2 +
                    (static_cast<T>(-2) + static_cast<T>(48) / par2) * cospar / parint;
            ac = static_cast<T>(-24) * parint * cospar;
            as = static_cast<T>(-8) * sinpar;
            if (crd::math::fabs(parint) > T{24})
            {
                T an = T{3};
                for (int i = 3; i <= 12; ++i)
                {
                    const T an2 = an * an;
                    vs[i] = ((an2 - T{4}) * (T{2} * (par22 - an2 - an2) * vs[i - 1] + as) + ac -
                             par2 * (an + T{1}) * (an + T{2}) * vs[i - 2]) /
                            (par2 * (an - T{1}) * (an - T{2}));
                    an += T{2};
                }
            }
            else
            {
                const int noequ = 25;
                const int noeq1 = noequ - 1;
                T d[26] = {};
                T d1[26] = {};
                T d2[26] = {};
                T an = T{5};
                for (int k = 1; k <= noeq1; ++k)
                {
                    const T an2 = an * an;
                    d[k] = T{-2} * (an2 - T{4}) * (par22 - an2 - an2);
                    d2[k] = (an - T{1}) * (an - T{2}) * par2;
                    d1[k + 1] = (an + T{3}) * (an + T{4}) * par2;
                    vs[k + 2] = ac + (an2 - T{4}) * as;
                    an += T{2};
                }
                T an2 = an * an;
                d[noequ] = T{-2} * (an2 - T{4}) * (par22 - an2 - an2);
                vs[noequ + 2] = ac + (an2 - T{4}) * as;
                vs[3] = vs[3] - static_cast<T>(42) * par2 * vs[2];
                const T ass = parint * cospar;
                const T asap = (((((static_cast<T>(105) * par2 - static_cast<T>(63)) * ass +
                                   (static_cast<T>(210) * par2 - T{1}) * sinpar) /
                                      an2 +
                                  (static_cast<T>(15) * par2 - T{1}) * sinpar - static_cast<T>(15) * ass) /
                                     an2 -
                                 T{3} * ass - sinpar) /
                                    an2 -
                                sinpar) /
                               an2;
                vs[noequ + 2] = vs[noequ + 2] - T{2} * asap * par2 * (an - T{1}) * (an - T{2});
                T c1[26], d1d[26], e1[26], bb[26];
                for (int k = 1; k <= noequ; ++k)
                {
                    c1[k] = d1[k];
                    d1d[k] = d[k];
                    e1[k] = d2[k];
                    bb[k] = vs[k + 2];
                }
                dgtsl<T>(noequ, c1, d1d, e1, bb);
                for (int k = 1; k <= noequ; ++k)
                {
                    vs[k + 2] = bb[k];
                }
            }
            for (int j = 1; j <= 12; ++j)
            {
                ws.cheb(m, 2 * j) = vs[j];
            }
        }
        if (nrmom < ws.momcom)
        {
            m = nrmom + 1;
        }
        if (ws.momcom < (maxp1 - 1) && nrmom >= ws.momcom)
        {
            ws.momcom = ws.momcom + 1;
        }
        // Chebyshev expansion of f.
        T fval[26];
        fval[1] = static_cast<T>(0.5) * f(centr + hlgth);
        fval[13] = f(centr);
        fval[25] = static_cast<T>(0.5) * f(centr - hlgth);
        for (int i = 2; i <= 12; ++i)
        {
            const int isym = 26 - i;
            fval[i] = f(hlgth * X::kX[i - 1] + centr);
            fval[isym] = f(centr - hlgth * X::kX[i - 1]);
        }
        T cheb12[14];
        T cheb24[26];
        dqcheb<T>(X::kX, fval, cheb12, cheb24);
        T resc12 = cheb12[13] * ws.cheb(m, 13);
        T ress12 = T{0};
        int k = 11;
        for (int j = 1; j <= 6; ++j)
        {
            resc12 += cheb12[k] * ws.cheb(m, k);
            ress12 += cheb12[k + 1] * ws.cheb(m, k + 1);
            k -= 2;
        }
        T resc24 = cheb24[25] * ws.cheb(m, 25);
        T ress24 = T{0};
        resabs = crd::math::fabs(cheb24[25]);
        k = 23;
        for (int j = 1; j <= 12; ++j)
        {
            resc24 += cheb24[k] * ws.cheb(m, k);
            ress24 += cheb24[k + 1] * ws.cheb(m, k + 1);
            resabs += crd::math::fabs(cheb24[k]) + crd::math::fabs(cheb24[k + 1]);
            k -= 2;
        }
        const T estc = crd::math::fabs(resc24 - resc12);
        const T ests = crd::math::fabs(ress24 - ress12);
        resabs = resabs * crd::math::fabs(hlgth);
        if (integr == 2)
        {
            result = conc * ress24 + cons * resc24;
            abserr = crd::math::fabs(conc * ests) + crd::math::fabs(cons * estc);
        }
        else
        {
            result = conc * resc24 - cons * ress24;
            abserr = crd::math::fabs(conc * estc) + crd::math::fabs(cons * ests);
        }
    }
    else
    {
        const auto w = [omega, integr](T x)
        {
            return integr == 2 ? crd::math::sin(omega * x) : crd::math::cos(omega * x);
        };
        dqk15w<T>(std::forward<F>(f), w, a, b, result, abserr, resabs, resasc);
        neval = 15;
    }
}

// dqc25c — 25-point generalized Clenshaw-Curtis for the Cauchy weight 1/(x-c) (or GK15w when c is outside the central
// 10% of [a,b]). krul is decremented when the GK fallback is used (tracked by the driver).
template <typename T, typename F> void dqc25c(F&& f, T a, T b, T c, int& krul, T& result, T& abserr, int& neval)
{
    using X = CcX<T>;
    const T cc = (T{2} * c - b - a) / (b - a);
    if (crd::math::fabs(cc) < static_cast<T>(1.1))
    {
        const T hlgth = static_cast<T>(0.5) * (b - a);
        const T centr = static_cast<T>(0.5) * (b + a);
        neval = 25;
        T fval[26];
        fval[1] = static_cast<T>(0.5) * f(hlgth + centr);
        fval[13] = f(centr);
        fval[25] = static_cast<T>(0.5) * f(centr - hlgth);
        for (int i = 2; i <= 12; ++i)
        {
            const T u = hlgth * X::kX[i - 1];
            const int isym = 26 - i;
            fval[i] = f(u + centr);
            fval[isym] = f(centr - u);
        }
        T cheb12[14];
        T cheb24[26];
        dqcheb<T>(X::kX, fval, cheb12, cheb24);
        T amom0 = crd::math::log(crd::math::fabs((T{1} - cc) / (T{1} + cc)));
        T amom1 = T{2} + cc * amom0;
        T res12 = cheb12[1] * amom0 + cheb12[2] * amom1;
        T res24 = cheb24[1] * amom0 + cheb24[2] * amom1;
        for (int k = 3; k <= 13; ++k)
        {
            T amom2 = T{2} * cc * amom1 - amom0;
            const int ak22 = (k - 2) * (k - 2);
            if ((k / 2) * 2 == k)
            {
                amom2 = amom2 - T{4} / (static_cast<T>(ak22) - T{1});
            }
            res12 += cheb12[k] * amom2;
            res24 += cheb24[k] * amom2;
            amom0 = amom1;
            amom1 = amom2;
        }
        for (int k = 14; k <= 25; ++k)
        {
            T amom2 = T{2} * cc * amom1 - amom0;
            const int ak22 = (k - 2) * (k - 2);
            if ((k / 2) * 2 == k)
            {
                amom2 = amom2 - T{4} / (static_cast<T>(ak22) - T{1});
            }
            res24 += cheb24[k] * amom2;
            amom0 = amom1;
            amom1 = amom2;
        }
        result = res24;
        abserr = crd::math::fabs(res24 - res12);
    }
    else
    {
        krul -= 1;
        T resabs, resasc;
        const auto w = [c](T x)
        {
            return T{1} / (x - c);
        };
        dqk15w<T>(std::forward<F>(f), w, a, b, result, abserr, resabs, resasc);
        neval = 15;
        if (resasc == abserr)
        {
            krul += 1;
        }
    }
}

// dqc25s — 25-point generalized Clenshaw-Curtis for the algebraico-log weight (or GK15w when the panel [bl,br] is
// interior to [a,b]). ri/rj/rg/rh are the dqmomo moments. integr 1..4 selects the log factor.
template <typename T, typename F>
void dqc25s(F&& f, T a, T b, T bl, T br, T alfa, T beta, const T* ri, const T* rj, const T* rg, const T* rh, int integr,
            T& result, T& abserr, int& nev)
{
    using X = CcX<T>;
    nev = 25;
    T fval[26];
    if (bl == a && (alfa != T{0} || integr == 2 || integr == 4))
    {
        const T hlgth = static_cast<T>(0.5) * (br - bl);
        const T centr = static_cast<T>(0.5) * (br + bl);
        const T fix = b - centr;
        fval[1] = static_cast<T>(0.5) * f(hlgth + centr) * wpow(fix - hlgth, beta);
        fval[13] = f(centr) * wpow(fix, beta);
        fval[25] = static_cast<T>(0.5) * f(centr - hlgth) * wpow(fix + hlgth, beta);
        for (int i = 2; i <= 12; ++i)
        {
            const T u = hlgth * X::kX[i - 1];
            const int isym = 26 - i;
            fval[i] = f(u + centr) * wpow(fix - u, beta);
            fval[isym] = f(centr - u) * wpow(fix + u, beta);
        }
        const T factor = wpow(hlgth, alfa + T{1});
        result = T{0};
        abserr = T{0};
        T res12 = T{0};
        T res24 = T{0};
        if (integr > 2)
        {
            fval[1] = fval[1] * crd::math::log(fix - hlgth);
            fval[13] = fval[13] * crd::math::log(fix);
            fval[25] = fval[25] * crd::math::log(fix + hlgth);
            for (int i = 2; i <= 12; ++i)
            {
                const T u = hlgth * X::kX[i - 1];
                const int isym = 26 - i;
                fval[i] = fval[i] * crd::math::log(fix - u);
                fval[isym] = fval[isym] * crd::math::log(fix + u);
            }
            T cheb12[14];
            T cheb24[26];
            dqcheb<T>(X::kX, fval, cheb12, cheb24);
            for (int i = 1; i <= 13; ++i)
            {
                res12 += cheb12[i] * ri[i];
                res24 += cheb24[i] * ri[i];
            }
            for (int i = 14; i <= 25; ++i)
            {
                res24 += cheb24[i] * ri[i];
            }
            if (integr != 3)
            {
                const T dc = crd::math::log(br - bl);
                result = res24 * dc;
                abserr = crd::math::fabs((res24 - res12) * dc);
                res12 = T{0};
                res24 = T{0};
                for (int i = 1; i <= 13; ++i)
                {
                    res12 += cheb12[i] * rg[i];
                    res24 += cheb24[i] * rg[i];
                }
                for (int i = 14; i <= 25; ++i)
                {
                    res24 += cheb24[i] * rg[i];
                }
            }
        }
        else
        {
            T cheb12[14];
            T cheb24[26];
            dqcheb<T>(X::kX, fval, cheb12, cheb24);
            for (int i = 1; i <= 13; ++i)
            {
                res12 += cheb12[i] * ri[i];
                res24 += cheb24[i] * ri[i];
            }
            for (int i = 14; i <= 25; ++i)
            {
                res24 += cheb24[i] * ri[i];
            }
            if (integr != 1)
            {
                const T dc = crd::math::log(br - bl);
                result = res24 * dc;
                abserr = crd::math::fabs((res24 - res12) * dc);
                res12 = T{0};
                res24 = T{0};
                for (int i = 1; i <= 13; ++i)
                {
                    res12 += cheb12[i] * rg[i];
                    res24 += cheb24[i] * rg[i]; // CORRECTED: QUADPACK dqc25s has the res24=res12+… typo here
                }
                for (int i = 14; i <= 25; ++i)
                {
                    res24 += cheb24[i] * rg[i];
                }
            }
        }
        result = (result + res24) * factor;
        abserr = (abserr + crd::math::fabs(res24 - res12)) * factor;
    }
    else if (br == b && (beta != T{0} || integr == 3 || integr == 4))
    {
        const T hlgth = static_cast<T>(0.5) * (br - bl);
        const T centr = static_cast<T>(0.5) * (br + bl);
        const T fix = centr - a;
        fval[1] = static_cast<T>(0.5) * f(hlgth + centr) * wpow(fix + hlgth, alfa);
        fval[13] = f(centr) * wpow(fix, alfa);
        fval[25] = static_cast<T>(0.5) * f(centr - hlgth) * wpow(fix - hlgth, alfa);
        for (int i = 2; i <= 12; ++i)
        {
            const T u = hlgth * X::kX[i - 1];
            const int isym = 26 - i;
            fval[i] = f(u + centr) * wpow(fix + u, alfa);
            fval[isym] = f(centr - u) * wpow(fix - u, alfa);
        }
        const T factor = wpow(hlgth, beta + T{1});
        result = T{0};
        abserr = T{0};
        T res12 = T{0};
        T res24 = T{0};
        if (integr == 2 || integr == 4)
        {
            fval[1] = fval[1] * crd::math::log(hlgth + fix);
            fval[13] = fval[13] * crd::math::log(fix);
            fval[25] = fval[25] * crd::math::log(fix - hlgth);
            for (int i = 2; i <= 12; ++i)
            {
                const T u = hlgth * X::kX[i - 1];
                const int isym = 26 - i;
                fval[i] = fval[i] * crd::math::log(u + fix);
                fval[isym] = fval[isym] * crd::math::log(fix - u);
            }
            T cheb12[14];
            T cheb24[26];
            dqcheb<T>(X::kX, fval, cheb12, cheb24);
            for (int i = 1; i <= 13; ++i)
            {
                res12 += cheb12[i] * rj[i];
                res24 += cheb24[i] * rj[i];
            }
            for (int i = 14; i <= 25; ++i)
            {
                res24 += cheb24[i] * rj[i];
            }
            if (integr != 2)
            {
                const T dc = crd::math::log(br - bl);
                result = res24 * dc;
                abserr = crd::math::fabs((res24 - res12) * dc);
                res12 = T{0};
                res24 = T{0};
                for (int i = 1; i <= 13; ++i)
                {
                    res12 += cheb12[i] * rh[i];
                    res24 += cheb24[i] * rh[i];
                }
                for (int i = 14; i <= 25; ++i)
                {
                    res24 += cheb24[i] * rh[i];
                }
            }
        }
        else
        {
            T cheb12[14];
            T cheb24[26];
            dqcheb<T>(X::kX, fval, cheb12, cheb24);
            for (int i = 1; i <= 13; ++i)
            {
                res12 += cheb12[i] * rj[i];
                res24 += cheb24[i] * rj[i];
            }
            for (int i = 14; i <= 25; ++i)
            {
                res24 += cheb24[i] * rj[i];
            }
            if (integr != 1)
            {
                const T dc = crd::math::log(br - bl);
                result = res24 * dc;
                abserr = crd::math::fabs((res24 - res12) * dc);
                res12 = T{0};
                res24 = T{0};
                for (int i = 1; i <= 13; ++i)
                {
                    res12 += cheb12[i] * rh[i];
                    res24 += cheb24[i] * rh[i];
                }
                for (int i = 14; i <= 25; ++i)
                {
                    res24 += cheb24[i] * rh[i];
                }
            }
        }
        result = (result + res24) * factor;
        abserr = (abserr + crd::math::fabs(res24 - res12)) * factor;
    }
    else
    {
        T resabs, resasc;
        const auto w = [a, b, alfa, beta, integr](T x)
        {
            const T xma = x - a;
            const T bmx = b - x;
            T r = wpow(xma, alfa) * wpow(bmx, beta);
            if (integr == 1)
            {
                return r;
            }
            if (integr == 3)
            {
                return r * crd::math::log(bmx);
            }
            if (integr == 4)
            {
                return r * crd::math::log(xma) * crd::math::log(bmx);
            }
            return r * crd::math::log(xma);
        };
        dqk15w<T>(std::forward<F>(f), w, bl, br, result, abserr, resabs, resasc);
        nev = 15;
    }
}

// dqpsrt — 1-based error-ordering maintenance (matches the validated python). iord mutated; returns nothing,
// updates maxerr/ermax/nrmax through references.
template <typename T> void oqpsrt(int limit, int last, int& maxerr, T& ermax, const T* elist, int* iord, int& nrmax)
{
    if (last <= 2)
    {
        iord[1] = 1;
        iord[2] = 2;
        maxerr = iord[nrmax];
        ermax = elist[maxerr];
        return;
    }
    const T errmax = elist[maxerr];
    if (nrmax != 1)
    {
        const int ido = nrmax - 1;
        for (int i = 1; i <= ido; ++i)
        {
            const int isucc = iord[nrmax - 1];
            if (errmax <= elist[isucc])
            {
                break;
            }
            iord[nrmax] = isucc;
            nrmax -= 1;
        }
    }
    int jupbn = last;
    if (last > (limit / 2 + 2))
    {
        jupbn = limit + 3 - last;
    }
    const T errmin = elist[last];
    const int jbnd = jupbn - 1;
    const int ibeg = nrmax + 1;
    bool placed = false;
    if (ibeg <= jbnd)
    {
        for (int i = ibeg; i <= jbnd; ++i)
        {
            const int isucc = iord[i];
            if (errmax >= elist[isucc])
            {
                iord[i - 1] = maxerr;
                int k = jbnd;
                bool done60 = false;
                for (int j = i; j <= jbnd; ++j)
                {
                    const int isucc2 = iord[k];
                    if (errmin < elist[isucc2])
                    {
                        iord[k + 1] = last;
                        done60 = true;
                        break;
                    }
                    iord[k + 1] = isucc2;
                    k -= 1;
                }
                if (!done60)
                {
                    iord[i] = last;
                }
                placed = true;
                break;
            }
            iord[i - 1] = isucc;
        }
    }
    if (!placed)
    {
        iord[jbnd] = maxerr;
        iord[jupbn] = last;
    }
    maxerr = iord[nrmax];
    ermax = elist[maxerr];
}

// dqelg — 1-based epsilon (Wynn) extrapolation matching the validated python. epstab[1..52], res3la[1..3]. n mutated.
template <typename T> void oqelg(int& n, T* epstab, T& result, T& abserr, T* res3la, int& nres)
{
    const T epmach = std::numeric_limits<T>::epsilon();
    const T oflow = std::numeric_limits<T>::max();
    nres += 1;
    abserr = oflow;
    result = epstab[n];
    if (n < 3)
    {
        abserr = qmax<T>(abserr, static_cast<T>(5) * epmach * crd::math::fabs(result));
        return;
    }
    epstab[n + 2] = epstab[n];
    const int newelm = (n - 1) / 2;
    epstab[n] = oflow;
    const int num = n;
    int k1 = n;
    for (int i = 1; i <= newelm; ++i)
    {
        const int k2 = k1 - 1;
        const int k3 = k1 - 2;
        T res = epstab[k1 + 2];
        const T e0 = epstab[k3];
        const T e1 = epstab[k2];
        const T e2 = res;
        const T e1abs = crd::math::fabs(e1);
        const T delta2 = e2 - e1;
        const T err2 = crd::math::fabs(delta2);
        const T tol2 = qmax<T>(crd::math::fabs(e2), e1abs) * epmach;
        const T delta3 = e1 - e0;
        const T err3 = crd::math::fabs(delta3);
        const T tol3 = qmax<T>(e1abs, crd::math::fabs(e0)) * epmach;
        if (err2 > tol2 || err3 > tol3)
        {
            const T e3 = epstab[k1];
            epstab[k1] = e1;
            const T delta1 = e1 - e3;
            const T err1 = crd::math::fabs(delta1);
            const T tol1 = qmax<T>(e1abs, crd::math::fabs(e3)) * epmach;
            if (err1 > tol1 && err2 > tol2 && err3 > tol3)
            {
                const T ss = T{1} / delta1 + T{1} / delta2 - T{1} / delta3;
                const T epsinf = crd::math::fabs(ss * e1);
                if (epsinf > static_cast<T>(1e-4))
                {
                    res = e1 + T{1} / ss;
                    epstab[k1] = res;
                    k1 -= 2;
                    const T error = err2 + crd::math::fabs(res - e2) + err3;
                    if (error <= abserr)
                    {
                        abserr = error;
                        result = res;
                    }
                    continue;
                }
            }
            n = i + i - 1;
            break;
        }
        result = res;
        abserr = err2 + err3;
        abserr = qmax<T>(abserr, static_cast<T>(5) * epmach * crd::math::fabs(result));
        return;
    }
    if (n == kLimexp)
    {
        n = 2 * (kLimexp / 2) - 1;
    }
    int ib = 1;
    if ((num / 2) * 2 == num)
    {
        ib = 2;
    }
    const int ie = newelm + 1;
    for (int i = 1; i <= ie; ++i)
    {
        const int ib2 = ib + 2;
        epstab[ib] = epstab[ib2];
        ib = ib2;
    }
    if (num != n)
    {
        int indx = num - n + 1;
        for (int i = 1; i <= n; ++i)
        {
            epstab[i] = epstab[indx];
            indx += 1;
        }
    }
    if (nres >= 4)
    {
        abserr = crd::math::fabs(result - res3la[3]) + crd::math::fabs(result - res3la[2]) +
                 crd::math::fabs(result - res3la[1]);
        res3la[1] = res3la[2];
        res3la[2] = res3la[3];
        res3la[3] = result;
    }
    else
    {
        res3la[nres] = result;
        abserr = oflow;
    }
    abserr = qmax<T>(abserr, static_cast<T>(5) * epmach * crd::math::fabs(result));
}

// dqawoe — adaptive oscillatory integrator (QAWO). icall>1 reuses ws.chebmo across calls (QAWF). Faithful port of the
// validated python (correct done→global-sum + main-loop-cycle-on-width>small).
template <typename T, typename F>
void dqawoe(F&& f, T a, T b, T omega, int integr, T epsabs, T epsrel, int icall, OscWorkspace<T>& ws, T& result,
            T& abserr, int& neval, int& ier, int& last)
{
    const T epmach = std::numeric_limits<T>::epsilon();
    const T uflow = std::numeric_limits<T>::min();
    const T oflow = std::numeric_limits<T>::max();
    const int limit = ws.limit;
    const int maxp1 = ws.maxp1;
    T* alist = ws.alist.data();
    T* blist = ws.blist.data();
    T* rlist = ws.rlist.data();
    T* elist = ws.elist.data();
    int* iord = ws.iord.data();
    int* nnlog = ws.nnlog.data();
    T rlist2[kLimexp + 3] = {};
    T res3la[4] = {};

    ier = 0;
    neval = 0;
    last = 0;
    result = T{0};
    abserr = T{0};
    alist[1] = a;
    blist[1] = b;
    rlist[1] = T{0};
    elist[1] = T{0};
    iord[1] = 0;
    nnlog[1] = 0; // CRITICAL: the loop's first nrmom = nnlog[maxerr=1]+1 — without this, a REUSED workspace leaks the
                  // prior call's stale subdivision level, selecting wrong Chebyshev moments (masked by a fresh ws).
    if ((integr != 1 && integr != 2) ||
        (epsabs <= T{0} && epsrel < qmax<T>(static_cast<T>(50) * epmach, static_cast<T>(0.5e-28))) || icall < 1 ||
        maxp1 < 1)
    {
        ier = 6;
        return;
    }
    const T domega = crd::math::fabs(omega);
    int nrmom = 0;
    if (icall <= 1)
    {
        ws.momcom = 0;
    }
    T defabs, resabs;
    dqc25f<T>(f, a, b, domega, integr, nrmom, 0, ws, result, abserr, neval, defabs, resabs);
    const T dres = crd::math::fabs(result);
    T errbnd = qmax<T>(epsabs, epsrel * dres);
    rlist[1] = result;
    elist[1] = abserr;
    iord[1] = 1;
    if (abserr <= static_cast<T>(100) * epmach * defabs && abserr > errbnd)
    {
        ier = 2;
    }
    if (limit == 1)
    {
        ier = 1;
    }
    if (ier != 0 || abserr <= errbnd)
    {
        if (integr == 2 && omega < T{0})
        {
            result = -result;
        }
        last = 1;
        return;
    }
    T errmax = abserr;
    int maxerr = 1;
    T area = result;
    T errsum = abserr;
    abserr = oflow;
    int nrmax = 1;
    bool extrap = false, noext = false;
    int ierro = 0, iroff1 = 0, iroff2 = 0, iroff3 = 0, ktmin = 0, nres = 0, numrl2 = 0;
    bool extall = false;
    T small = crd::math::fabs(b - a) * static_cast<T>(0.75);
    T erlarg = T{0}, ertest = T{0}, correc = T{0};
    if (static_cast<T>(0.5) * crd::math::fabs(b - a) * domega <= T{2})
    {
        numrl2 = 1;
        extall = true;
        rlist2[1] = result;
    }
    if (static_cast<T>(0.25) * crd::math::fabs(b - a) * domega <= T{2})
    {
        extall = true;
    }
    int ksgn = -1;
    if (dres >= (T{1} - static_cast<T>(50) * epmach) * defabs)
    {
        ksgn = 1;
    }
    bool done = false;
    for (last = 2; last <= limit; ++last)
    {
        nrmom = nnlog[maxerr] + 1;
        const T a1 = alist[maxerr];
        const T b1 = static_cast<T>(0.5) * (alist[maxerr] + blist[maxerr]);
        const T a2 = b1;
        const T b2 = blist[maxerr];
        const T erlast = errmax;
        T area1, error1, defab1, area2, error2, defab2, rdummy;
        int nev = 0;
        dqc25f<T>(f, a1, b1, domega, integr, nrmom, 0, ws, area1, error1, nev, rdummy, defab1);
        neval += nev;
        dqc25f<T>(f, a2, b2, domega, integr, nrmom, 1, ws, area2, error2, nev, rdummy, defab2);
        neval += nev;
        const T area12 = area1 + area2;
        const T erro12 = error1 + error2;
        errsum = errsum + erro12 - errmax;
        area = area + area12 - rlist[maxerr];
        if (defab1 != error1 && defab2 != error2)
        {
            if (crd::math::fabs(rlist[maxerr] - area12) <= static_cast<T>(0.1e-4) * crd::math::fabs(area12) &&
                erro12 >= static_cast<T>(0.99) * errmax)
            {
                if (extrap)
                {
                    ++iroff2;
                }
                else
                {
                    ++iroff1;
                }
            }
            if (last > 10 && erro12 > errmax)
            {
                ++iroff3;
            }
        }
        rlist[maxerr] = area1;
        rlist[last] = area2;
        nnlog[maxerr] = nrmom;
        nnlog[last] = nrmom;
        errbnd = qmax<T>(epsabs, epsrel * crd::math::fabs(area));
        if (iroff1 + iroff2 >= 10 || iroff3 >= 20)
        {
            ier = 2;
        }
        if (iroff2 >= 5)
        {
            ierro = 3;
        }
        if (last == limit)
        {
            ier = 1;
        }
        if (qmax<T>(crd::math::fabs(a1), crd::math::fabs(b2)) <=
            (T{1} + static_cast<T>(100) * epmach) * (crd::math::fabs(a2) + static_cast<T>(1000) * uflow))
        {
            ier = 4;
        }
        if (error2 > error1)
        {
            alist[maxerr] = a2;
            alist[last] = a1;
            blist[last] = b1;
            rlist[maxerr] = area2;
            rlist[last] = area1;
            elist[maxerr] = error2;
            elist[last] = error1;
        }
        else
        {
            alist[last] = a2;
            blist[maxerr] = b1;
            blist[last] = b2;
            elist[maxerr] = error1;
            elist[last] = error2;
        }
        oqpsrt<T>(limit, last, maxerr, errmax, elist, iord, nrmax);
        if (errsum <= errbnd)
        {
            done = true;
            break;
        }
        if (ier != 0)
        {
            break;
        }
        if (last == 2 && extall)
        {
            small = small * static_cast<T>(0.5);
            ++numrl2;
            rlist2[numrl2] = area;
            ertest = errbnd;
            erlarg = errsum;
            continue;
        }
        if (noext)
        {
            continue;
        }
        bool test = true;
        if (extall)
        {
            erlarg -= erlast;
            if (crd::math::fabs(b1 - a1) > small)
            {
                erlarg += erro12;
            }
            if (extrap)
            {
                test = false;
            }
        }
        if (test)
        {
            const T width = crd::math::fabs(blist[maxerr] - alist[maxerr]);
            if (width > small)
            {
                continue;
            }
            if (extall)
            {
                extrap = true;
                nrmax = 2;
            }
            else
            {
                small = small * static_cast<T>(0.5);
                if (static_cast<T>(0.25) * width * domega > T{2})
                {
                    continue;
                }
                extall = true;
                ertest = errbnd;
                erlarg = errsum;
                continue;
            }
        }
        if (ierro != 3 && erlarg > ertest)
        {
            int jupbnd = last;
            if (last > (limit / 2 + 2))
            {
                jupbnd = limit + 3 - last;
            }
            bool skip = false;
            for (int k = nrmax; k <= jupbnd; ++k)
            {
                maxerr = iord[nrmax];
                errmax = elist[maxerr];
                if (crd::math::fabs(blist[maxerr] - alist[maxerr]) > small)
                {
                    skip = true;
                    break;
                }
                ++nrmax;
            }
            if (skip)
            {
                continue; // cycle the MAIN loop (dqagse/scipy semantics)
            }
        }
        ++numrl2;
        rlist2[numrl2] = area;
        if (numrl2 >= 3)
        {
            T reseps, abseps;
            oqelg<T>(numrl2, rlist2, reseps, abseps, res3la, nres);
            ++ktmin;
            if (ktmin > 5 && abserr < static_cast<T>(0.1e-2) * errsum)
            {
                ier = 5;
            }
            if (abseps < abserr)
            {
                ktmin = 0;
                abserr = abseps;
                result = reseps;
                correc = erlarg;
                ertest = qmax<T>(epsabs, epsrel * crd::math::fabs(reseps));
                if (abserr <= ertest)
                {
                    break;
                }
            }
            if (numrl2 == 1)
            {
                noext = true;
            }
            if (ier == 5)
            {
                break;
            }
        }
        maxerr = iord[1];
        errmax = elist[maxerr];
        nrmax = 1;
        extrap = false;
        small = small * static_cast<T>(0.5);
        erlarg = errsum;
    }

    bool go_global = false;
    if (done)
    {
        go_global = true;
    }
    else if (abserr != oflow && nres != 0)
    {
        if (ier + ierro != 0)
        {
            if (ierro == 3)
            {
                abserr += correc;
            }
            if (ier == 0)
            {
                ier = 3;
            }
            if (result == T{0} || area == T{0})
            {
                if (abserr > errsum)
                {
                    go_global = true;
                }
                else if (area == T{0})
                {
                    if (ier > 2)
                    {
                        --ier;
                    }
                    if (integr == 2 && omega < T{0})
                    {
                        result = -result;
                    }
                    return;
                }
            }
            else if (abserr / crd::math::fabs(result) > errsum / crd::math::fabs(area))
            {
                go_global = true;
            }
        }
        if (!go_global)
        {
            if (ksgn != -1 || qmax<T>(crd::math::fabs(result), crd::math::fabs(area)) > defabs * static_cast<T>(0.01))
            {
                if (static_cast<T>(0.01) > (result / area) || (result / area) > static_cast<T>(100) ||
                    errsum >= crd::math::fabs(area))
                {
                    ier = 6;
                }
            }
            if (ier > 2)
            {
                --ier;
            }
            if (integr == 2 && omega < T{0})
            {
                result = -result;
            }
            return;
        }
    }
    else
    {
        go_global = true;
    }
    if (go_global)
    {
        result = T{0};
        for (int k = 1; k <= last; ++k)
        {
            result += rlist[k];
        }
        abserr = errsum;
    }
    if (ier > 2)
    {
        --ier;
    }
    if (integr == 2 && omega < T{0})
    {
        result = -result;
    }
}

// Map a QUADPACK ier to QuadResult.
template <typename T> [[nodiscard]] QuadResult<T> make_result(T value, T abserr, int neval, int last, int ier)
{
    QuadResult<T> r;
    r.value = value;
    r.error_estimate = abserr;
    r.eval_count = static_cast<crd::u32>(neval < 0 ? 0 : neval);
    r.subdiv_count = static_cast<crd::u32>(last < 0 ? 0 : last);
    r.tolerance_met = (ier == 0);
    r.status = (ier == 0)   ? QuadStatus::Ok
               : (ier == 6) ? QuadStatus::BadInput
               : (ier == 1) ? QuadStatus::MaxSubdivisions
                            : QuadStatus::RoundoffError;
    return r;
}

} // namespace detail

// QAWO — adaptive ∫_a^b f(x)·{cos,sin}(ωx) dx. The ws.chebmo cache is reset (icall=1). Pass a preallocated
// OscWorkspace to amortize allocation across calls (ADR-0095 pillar 2).
template <typename T, typename F>
[[nodiscard]] QuadResult<T> integrate_qawo(OscWorkspace<T>& ws, F&& f, T a, T b, T omega, OscWeight w, T epsabs,
                                           T epsrel)
{
    if (!detail::quad_finite(a) || !detail::quad_finite(b))
    {
        return QuadResult<T>{T{0}, T{0}, 0, 0, QuadStatus::BadInput, false};
    }
    // Reuse the cached Chebyshev moments (icall=2) when (|ω|, b−a) is unchanged and the table is non-empty — the
    // integrand-independent-precompute crush lever. icall=1 forces a fresh moment table otherwise.
    const T dom = crd::math::fabs(omega);
    const T len = b - a;
    const int icall = (ws.momcom > 0 && ws.cached_domega == dom && ws.cached_len == len) ? 2 : 1;
    ws.cached_domega = dom;
    ws.cached_len = len;
    T result, abserr;
    int neval = 0, ier = 0, last = 0;
    detail::dqawoe<T>(std::forward<F>(f), a, b, omega, static_cast<int>(w), epsabs, epsrel, icall, ws, result, abserr,
                      neval, ier, last);
    return detail::make_result<T>(result, abserr, neval, last, ier);
}

// QAWO convenience overload — allocates the workspace + moment cache once per call.
template <typename T, typename F>
[[nodiscard]] QuadResult<T> integrate_qawo(crd::memory::IAllocator* alloc, F&& f, T a, T b, T omega, OscWeight w,
                                           T epsabs, T epsrel, int limit = 50, int maxp1 = 21)
{
    OscWorkspace<T> ws(alloc, limit, maxp1);
    return integrate_qawo<T>(ws, static_cast<F&&>(f), a, b, omega, w, epsabs, epsrel);
}

// QAWC — Cauchy principal value PV ∫_a^b f(x)/(x-c) dx (c must not equal a or b).
template <typename T, typename F>
[[nodiscard]] QuadResult<T> integrate_qawc(OscWorkspace<T>& ws, F&& f, T a, T b, T c, T epsabs, T epsrel)
{
    if (!detail::quad_finite(a) || !detail::quad_finite(b) || !detail::quad_finite(c))
    {
        return QuadResult<T>{T{0}, T{0}, 0, 0, QuadStatus::BadInput, false};
    }
    const T epmach = std::numeric_limits<T>::epsilon();
    const T uflow = std::numeric_limits<T>::min();
    const int limit = ws.limit;
    T* alist = ws.alist.data();
    T* blist = ws.blist.data();
    T* rlist = ws.rlist.data();
    T* elist = ws.elist.data();
    int* iord = ws.iord.data();
    int ier = 6, neval = 0, last = 0;
    T result = T{0}, abserr = T{0};
    if (c == a || c == b ||
        (epsabs <= T{0} && epsrel < detail::qmax<T>(static_cast<T>(50) * epmach, static_cast<T>(0.5e-28))))
    {
        return detail::make_result<T>(result, abserr, 0, 0, 6);
    }
    T aa = a, bb = b;
    if (a > b)
    {
        aa = b;
        bb = a;
    }
    ier = 0;
    int krule = 1;
    detail::dqc25c<T>(f, aa, bb, c, krule, result, abserr, neval);
    last = 1;
    rlist[1] = result;
    elist[1] = abserr;
    iord[1] = 1;
    alist[1] = a;
    blist[1] = b;
    T errbnd = detail::qmax<T>(epsabs, epsrel * crd::math::fabs(result));
    if (limit == 1)
    {
        ier = 1;
    }
    if (abserr >= detail::qmin<T>(static_cast<T>(0.01) * crd::math::fabs(result), errbnd) && ier != 1)
    {
        alist[1] = aa;
        blist[1] = bb;
        rlist[1] = result;
        T errmax = abserr;
        int maxerr = 1;
        T area = result;
        T errsum = abserr;
        int nrmax = 1;
        int iroff1 = 0, iroff2 = 0;
        for (last = 2; last <= limit; ++last)
        {
            const T a1 = alist[maxerr];
            T b1 = static_cast<T>(0.5) * (alist[maxerr] + blist[maxerr]);
            const T b2 = blist[maxerr];
            if (c <= b1 && c > a1)
            {
                b1 = static_cast<T>(0.5) * (c + b2);
            }
            if (c > b1 && c < b2)
            {
                b1 = static_cast<T>(0.5) * (a1 + c);
            }
            const T a2 = b1;
            int krule2 = 2;
            T area1, error1, area2, error2;
            int nev = 0;
            detail::dqc25c<T>(f, a1, b1, c, krule2, area1, error1, nev);
            neval += nev;
            detail::dqc25c<T>(f, a2, b2, c, krule2, area2, error2, nev);
            neval += nev;
            const T area12 = area1 + area2;
            const T erro12 = error1 + error2;
            errsum = errsum + erro12 - errmax;
            area = area + area12 - rlist[maxerr];
            if (crd::math::fabs(rlist[maxerr] - area12) < static_cast<T>(0.1e-4) * crd::math::fabs(area12) &&
                erro12 >= static_cast<T>(0.99) * errmax && krule2 == 0)
            {
                ++iroff1;
            }
            if (last > 10 && erro12 > errmax && krule2 == 0)
            {
                ++iroff2;
            }
            rlist[maxerr] = area1;
            rlist[last] = area2;
            errbnd = detail::qmax<T>(epsabs, epsrel * crd::math::fabs(area));
            if (errsum > errbnd)
            {
                if (iroff1 >= 6 && iroff2 > 20)
                {
                    ier = 2;
                }
                if (last == limit)
                {
                    ier = 1;
                }
                if (detail::qmax<T>(crd::math::fabs(a1), crd::math::fabs(b2)) <=
                    (T{1} + static_cast<T>(100) * epmach) * (crd::math::fabs(a2) + static_cast<T>(1000) * uflow))
                {
                    ier = 3;
                }
            }
            if (error2 > error1)
            {
                alist[maxerr] = a2;
                alist[last] = a1;
                blist[last] = b1;
                rlist[maxerr] = area2;
                rlist[last] = area1;
                elist[maxerr] = error2;
                elist[last] = error1;
            }
            else
            {
                alist[last] = a2;
                blist[maxerr] = b1;
                blist[last] = b2;
                elist[maxerr] = error1;
                elist[last] = error2;
            }
            detail::oqpsrt<T>(limit, last, maxerr, errmax, elist, iord, nrmax);
            if (ier != 0 || errsum <= errbnd)
            {
                break;
            }
        }
        if (last > limit)
        {
            last = limit;
        }
        result = T{0};
        for (int k = 1; k <= last; ++k)
        {
            result += rlist[k];
        }
        abserr = errsum;
    }
    if (aa == b)
    {
        result = -result;
    }
    return detail::make_result<T>(result, abserr, neval, last, ier);
}

// QAWC convenience overload — allocates the workspace once per call.
template <typename T, typename F>
[[nodiscard]] QuadResult<T> integrate_qawc(crd::memory::IAllocator* alloc, F&& f, T a, T b, T c, T epsabs, T epsrel,
                                           int limit = 50)
{
    OscWorkspace<T> ws(alloc, limit, 1);
    return integrate_qawc<T>(ws, static_cast<F&&>(f), a, b, c, epsabs, epsrel);
}

// QAWS — adaptive ∫_a^b (x-a)^α (b-x)^β · [log…] · f(x) dx with α,β > -1. The log factor is chosen by `w`.
template <typename T, typename F>
[[nodiscard]] QuadResult<T> integrate_qaws(OscWorkspace<T>& ws, F&& f, T a, T b, T alfa, T beta, AlgLogWeight w,
                                           T epsabs, T epsrel)
{
    if (!detail::quad_finite(a) || !detail::quad_finite(b) || !detail::quad_finite(alfa) || !detail::quad_finite(beta))
    {
        return QuadResult<T>{T{0}, T{0}, 0, 0, QuadStatus::BadInput, false};
    }
    const int integr = static_cast<int>(w);
    const T epmach = std::numeric_limits<T>::epsilon();
    const T uflow = std::numeric_limits<T>::min();
    const int limit = ws.limit;
    T* alist = ws.alist.data();
    T* blist = ws.blist.data();
    T* rlist = ws.rlist.data();
    T* elist = ws.elist.data();
    int* iord = ws.iord.data();
    int ier = 0, neval = 0, last = 0;
    T result = T{0}, abserr = T{0};
    if (b <= a || (epsabs == T{0} && epsrel < detail::qmax<T>(static_cast<T>(50) * epmach, static_cast<T>(0.5e-28))) ||
        alfa <= T{-1} || beta <= T{-1} || integr < 1 || integr > 4 || limit < 2)
    {
        return detail::make_result<T>(T{0}, T{0}, 0, 0, 6);
    }
    // Reuse the cached modified moments when (α, β, integr) is unchanged — the integrand-independent-precompute lever
    // (GSL caches the same data in its qaws_table). dqmomo is otherwise recomputed every call.
    if (!(ws.cached_alfa == alfa && ws.cached_beta == beta && ws.cached_integr == integr))
    {
        detail::dqmomo<T>(alfa, beta, integr, ws.qaws_ri, ws.qaws_rj, ws.qaws_rg, ws.qaws_rh);
        ws.cached_alfa = alfa;
        ws.cached_beta = beta;
        ws.cached_integr = integr;
    }
    const T* ri = ws.qaws_ri;
    const T* rj = ws.qaws_rj;
    const T* rg = ws.qaws_rg;
    const T* rh = ws.qaws_rh;
    const T centre = static_cast<T>(0.5) * (b + a);
    T area1, error1, area2, error2;
    int nev = 0;
    detail::dqc25s<T>(f, a, b, a, centre, alfa, beta, ri, rj, rg, rh, integr, area1, error1, nev);
    neval = nev;
    detail::dqc25s<T>(f, a, b, centre, b, alfa, beta, ri, rj, rg, rh, integr, area2, error2, nev);
    last = 2;
    neval += nev;
    result = area1 + area2;
    abserr = error1 + error2;
    T errbnd = detail::qmax<T>(epsabs, epsrel * crd::math::fabs(result));
    if (error2 > error1)
    {
        alist[1] = centre;
        alist[2] = a;
        blist[1] = b;
        blist[2] = centre;
        rlist[1] = area2;
        rlist[2] = area1;
        elist[1] = error2;
        elist[2] = error1;
    }
    else
    {
        alist[1] = a;
        alist[2] = centre;
        blist[1] = centre;
        blist[2] = b;
        rlist[1] = area1;
        rlist[2] = area2;
        elist[1] = error1;
        elist[2] = error2;
    }
    iord[1] = 1;
    iord[2] = 2;
    if (limit == 2)
    {
        ier = 1;
    }
    if (abserr > errbnd && ier != 1)
    {
        T errmax = elist[1];
        int maxerr = 1;
        int nrmax = 1;
        T area = result;
        T errsum = abserr;
        int iroff1 = 0, iroff2 = 0;
        for (last = 3; last <= limit; ++last)
        {
            const T a1 = alist[maxerr];
            const T b1 = static_cast<T>(0.5) * (alist[maxerr] + blist[maxerr]);
            const T a2 = b1;
            const T b2 = blist[maxerr];
            T resas1 = error1, resas2 = error2; // dqc25s GK path resasc unused here; roundoff guard via a/b edge
            detail::dqc25s<T>(f, a, b, a1, b1, alfa, beta, ri, rj, rg, rh, integr, area1, error1, nev);
            resas1 = error1;
            neval += nev;
            detail::dqc25s<T>(f, a, b, a2, b2, alfa, beta, ri, rj, rg, rh, integr, area2, error2, nev);
            resas2 = error2;
            neval += nev;
            const T area12 = area1 + area2;
            const T erro12 = error1 + error2;
            errsum = errsum + erro12 - errmax;
            area = area + area12 - rlist[maxerr];
            if (a != a1 && b != b2)
            {
                if (resas1 != error1 && resas2 != error2)
                {
                    if (crd::math::fabs(rlist[maxerr] - area12) < static_cast<T>(0.1e-4) * crd::math::fabs(area12) &&
                        erro12 >= static_cast<T>(0.99) * errmax)
                    {
                        ++iroff1;
                    }
                    if (last > 10 && erro12 > errmax)
                    {
                        ++iroff2;
                    }
                }
            }
            rlist[maxerr] = area1;
            rlist[last] = area2;
            errbnd = detail::qmax<T>(epsabs, epsrel * crd::math::fabs(area));
            if (errsum > errbnd)
            {
                if (last == limit)
                {
                    ier = 1;
                }
                if (iroff1 >= 6 || iroff2 >= 20)
                {
                    ier = 2;
                }
                if (detail::qmax<T>(crd::math::fabs(a1), crd::math::fabs(b2)) <=
                    (T{1} + static_cast<T>(100) * epmach) * (crd::math::fabs(a2) + static_cast<T>(1000) * uflow))
                {
                    ier = 3;
                }
            }
            if (error2 > error1)
            {
                alist[maxerr] = a2;
                alist[last] = a1;
                blist[last] = b1;
                rlist[maxerr] = area2;
                rlist[last] = area1;
                elist[maxerr] = error2;
                elist[last] = error1;
            }
            else
            {
                alist[last] = a2;
                blist[maxerr] = b1;
                blist[last] = b2;
                elist[maxerr] = error1;
                elist[last] = error2;
            }
            detail::oqpsrt<T>(limit, last, maxerr, errmax, elist, iord, nrmax);
            if (ier != 0 || errsum <= errbnd)
            {
                break;
            }
        }
        if (last > limit)
        {
            last = limit;
        }
        result = T{0};
        for (int k = 1; k <= last; ++k)
        {
            result += rlist[k];
        }
        abserr = errsum;
    }
    return detail::make_result<T>(result, abserr, neval, last, ier);
}

// QAWS convenience overload — allocates the workspace once per call.
template <typename T, typename F>
[[nodiscard]] QuadResult<T> integrate_qaws(crd::memory::IAllocator* alloc, F&& f, T a, T b, T alfa, T beta,
                                           AlgLogWeight w, T epsabs, T epsrel, int limit = 50)
{
    OscWorkspace<T> ws(alloc, limit, 1);
    return integrate_qaws<T>(ws, static_cast<F&&>(f), a, b, alfa, beta, w, epsabs, epsrel);
}

// QAWF — adaptive Fourier integral ∫_a^∞ f(x)·{cos,sin}(ωx) dx. Integrates over half-period cycles of length
// (2⌊|ω|⌋+1)π/|ω| with QAWO, then accelerates the cycle series with the Wynn-ε algorithm. epsabs > 0 required.
// Workspace overload: allocation-free (stack scratch; limlst clamped to 64 = the WCET bound) + reuses ws.chebmo's
// Chebyshev moments across calls (every cycle shares the length `cycle` ⇒ the moments are computed ONCE for a given
// (|ω|, cycle) and reused — the integrand-independent-precompute crush lever).
template <typename T, typename F>
[[nodiscard]] QuadResult<T> integrate_qawf(OscWorkspace<T>& ws, F&& f, T a, T omega, OscWeight w, T epsabs,
                                           int limlst = 50)
{
    const int integr = static_cast<int>(w);
    if (!detail::quad_finite(a) || !detail::quad_finite(omega) || omega == T{0} || epsabs <= T{0} || limlst < 3)
    {
        return QuadResult<T>{T{0}, T{0}, 0, 0, QuadStatus::BadInput, false};
    }
    if (limlst > 64)
    {
        limlst = 64;
    }
    const T uflow = std::numeric_limits<T>::min();
    T psum[detail::kLimexp + 3] = {};
    T res3la[4] = {};
    const T p = static_cast<T>(0.9);
    T result = T{0}, abserr = T{0};
    int neval = 0, lst = 0, ier = 0;
    const T domega = crd::math::fabs(omega);
    const int l = static_cast<int>(domega);
    const T dl = static_cast<T>(2 * l + 1);
    const T pi = static_cast<T>(3.14159265358979323846264338327950288);
    const T cycle = dl * pi / domega;
    // moment reuse: every cycle has length `cycle` ⇒ if ws already holds moments for (|ω|, cycle), reuse them.
    const bool cache_hit = (ws.momcom > 0 && ws.cached_domega == domega && ws.cached_len == cycle);
    ws.cached_domega = domega;
    ws.cached_len = cycle;
    int ktmin = 0, numrl2 = 0, nres = 0, ll = 0;
    T c1 = a, c2 = cycle + a;
    const T p1 = T{1} - p;
    T eps = epsabs;
    if (epsabs > uflow / p1)
    {
        eps = epsabs * p1;
    }
    const T ep = eps;
    T fact = T{1}, correc = T{0}, errsum = T{0}, drl = T{0};
    bool broke_main = false;
    for (lst = 1; lst <= limlst; ++lst)
    {
        const T epsa = eps * fact;
        T rl, el;
        int nev = 0, ierl = 0, lastc = 0;
        // cycle 1 reuses cached moments on a hit (icall=2); cycles ≥2 always reuse (icall=lst≥2).
        const int ic = (lst >= 2) ? lst : (cache_hit ? 2 : 1);
        detail::dqawoe<T>(f, c1, c2, omega, integr, epsa, T{0}, ic, ws, rl, el, nev, ierl, lastc);
        neval += nev;
        fact = fact * p;
        errsum += el;
        drl = static_cast<T>(50) * crd::math::fabs(rl);
        if ((errsum + drl) <= epsabs && lst >= 6)
        {
            broke_main = true;
            break;
        }
        correc = detail::qmax<T>(correc, el);
        if (ierl != 0)
        {
            eps = detail::qmax<T>(ep, correc * p1);
            ier = 7;
        }
        if (ier == 7 && (errsum + drl) <= correc * static_cast<T>(10) && lst > 5)
        {
            broke_main = true;
            break;
        }
        ++numrl2;
        if (lst > 1)
        {
            psum[static_cast<crd::usize>(numrl2)] = psum[static_cast<crd::usize>(ll)] + rl;
            if (lst != 2)
            {
                if (lst == limlst)
                {
                    ier = 1;
                }
                T reseps, abseps;
                detail::oqelg<T>(numrl2, psum, reseps, abseps, res3la, nres);
                ++ktmin;
                if (ktmin >= 15 && abserr <= static_cast<T>(0.1e-2) * (errsum + drl))
                {
                    ier = 4;
                }
                if (abseps <= abserr || lst == 3)
                {
                    abserr = abseps;
                    result = reseps;
                    ktmin = 0;
                    if ((abserr + static_cast<T>(10) * correc) <= epsabs ||
                        (abserr <= epsabs && static_cast<T>(10) * correc >= epsabs))
                    {
                        break;
                    }
                }
                if (ier != 0 && ier != 7)
                {
                    break;
                }
            }
        }
        else
        {
            psum[1] = rl;
        }
        ll = numrl2;
        c1 = c2;
        c2 = c2 + cycle;
    }
    if (lst > limlst)
    {
        lst = limlst;
    }
    abserr += static_cast<T>(10) * correc;
    if (ier == 0)
    {
        return detail::make_result<T>(result, abserr, neval, lst, ier);
    }
    if (!broke_main)
    {
        if (result == T{0} || psum[static_cast<crd::usize>(numrl2)] == T{0})
        {
            if (abserr > errsum)
            {
                result = psum[static_cast<crd::usize>(numrl2)];
                abserr = errsum + drl;
                return detail::make_result<T>(result, abserr, neval, lst, ier);
            }
            if (psum[static_cast<crd::usize>(numrl2)] == T{0})
            {
                return detail::make_result<T>(result, abserr, neval, lst, ier);
            }
        }
        if (abserr / crd::math::fabs(result) <= (errsum + drl) / crd::math::fabs(psum[static_cast<crd::usize>(numrl2)]))
        {
            if (ier >= 1 && ier != 7)
            {
                abserr += drl;
            }
            return detail::make_result<T>(result, abserr, neval, lst, ier);
        }
    }
    result = psum[static_cast<crd::usize>(numrl2)];
    abserr = errsum + drl;
    return detail::make_result<T>(result, abserr, neval, lst, ier);
}

// QAWF convenience overload — allocates the workspace once per call (pass an OscWorkspace to amortize + cache moments).
template <typename T, typename F>
[[nodiscard]] QuadResult<T> integrate_qawf(crd::memory::IAllocator* alloc, F&& f, T a, T omega, OscWeight w, T epsabs,
                                           int limlst = 50, int limit = 50, int maxp1 = 21)
{
    OscWorkspace<T> ws(alloc, limit, maxp1);
    return integrate_qawf<T>(ws, static_cast<F&&>(f), a, omega, w, epsabs, limlst);
}

} // namespace crd::hesap::quadrature
