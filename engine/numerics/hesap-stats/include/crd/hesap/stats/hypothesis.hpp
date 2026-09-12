#pragma once

// v12-n (parametric) — classical hypothesis tests for location and scale.
//
//   t_test_1samp / t_test_ind / t_test_rel   Student / Welch / paired t-tests
//   f_oneway                                 one-way ANOVA (between-group F)
//   bartlett                                 equal-variance test (chi-square)
//   levene                                   robust equal-variance test (Brown-Forsythe / Levene F)
//
// p-values reuse the shipped special functions verbatim (SANITY rule 8): the Student two-sided tail is the regularized
// incomplete beta P(|T|>|t|) = I_{nu/(nu+t^2)}(nu/2, 1/2); the F upper tail is I_{d2/(d2+d1 F)}(d2/2, d1/2); the
// chi-square upper tail is Q(df/2, stat/2). Sample moments reuse crd::hesap::stats::mean/variance/median (v12-m).
//
// Gate (ADR-0094): bit-for-bit vs scipy.stats (ttest_1samp/ind/rel, f_oneway, bartlett, levene) + R/MATLAB conventions
// (Welch-Satterthwaite df; Bartlett's correction C; Levene defaults to the median center = Brown-Forsythe, scipy's
// default). Boost.Math ships the distributions but no one-call tests (its distribution primitives are already crushed
// in v12-a/d) — the test peers are scipy / R / MATLAB.

#include <crd/hesap/stats/descriptive.hpp>   // mean, variance, median_sorted, Real
#include <crd/hesap/quadrature/gauss.hpp>    // gauss_hermite/gauss_laguerre (studentized range for Tukey HSD)
#include <crd/hesap/special/bessel.hpp>      // cyl_bessel_k (Cramér-von-Mises asymptotic distribution)
#include <crd/hesap/special/erf.hpp>         // erfc (normal tail for the rank-test z->p)
#include <crd/hesap/special/gamma.hpp>       // lbeta, lgamma, gamma
#include <crd/hesap/special/incomplete.hpp>  // betainc, gammainc_q

#include <crd/containers/array.hpp>
#include <crd/containers/sort.hpp> // stable_sort (Levene median center)
#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/math/cmath.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::hesap::stats
{

// Tail selection (scipy `alternative=`). TwoSided is the default everywhere.
enum class Alternative : crd::u8
{
    TwoSided,
    Less,
    Greater
};

// Center for Levene's test. Median = Brown-Forsythe (scipy default, robust to non-normality); Mean = original Levene.
enum class Center : crd::u8
{
    Median,
    Mean
};

template <Real T> struct TestResult
{
    T statistic;
    T pvalue;
    T df; // degrees of freedom (fractional for Welch)
};

template <Real T> struct FTestResult
{
    T statistic;
    T pvalue;
    T df_num;
    T df_den;
};

namespace detail
{

// Student-t p-value from the t statistic + df. two = P(|T| > |t|) = I_{df/(df+t^2)}(df/2, 1/2) (the two-sided tail);
// one-sided tails are derived from it without a second special-function call.
template <Real T> [[nodiscard]] T t_pvalue(T t, T df, Alternative alt) noexcept
{
    const T half = static_cast<T>(0.5);
    const T z = df / (df + t * t);
    const T two = special::betainc(half * df, half, z, special::lbeta(half * df, half));
    switch (alt)
    {
    case Alternative::Less:
        return (t > static_cast<T>(0)) ? static_cast<T>(1) - half * two : half * two; // P(T < t)
    case Alternative::Greater:
        return (t > static_cast<T>(0)) ? half * two : static_cast<T>(1) - half * two; // P(T > t)
    case Alternative::TwoSided:
    default:
        return two;
    }
}

// F upper-tail p-value P(F_{d1,d2} > f) = I_{d2/(d2+d1 f)}(d2/2, d1/2).
template <Real T> [[nodiscard]] T f_pvalue(T f, T d1, T d2) noexcept
{
    const T half = static_cast<T>(0.5);
    return special::betainc(half * d2, half * d1, d2 / (d2 + d1 * f), special::lbeta(half * d2, half * d1));
}

// chi-square upper-tail p-value P(X^2_df > stat) = Q(df/2, stat/2).
template <Real T> [[nodiscard]] T chi2_pvalue(T stat, T df) noexcept
{
    const T half = static_cast<T>(0.5);
    return special::gammainc_q(half * df, half * stat, special::lgamma(half * df));
}

} // namespace detail

// ───────────────────────────── t-tests ─────────────────────────────

// One-sample t-test: H0 mean(x) == popmean. scipy.stats.ttest_1samp.
template <Real T>
[[nodiscard]] TestResult<T> t_test_1samp(crd::containers::ConstSpan<T> x, T popmean,
                                         Alternative alt = Alternative::TwoSided)
{
    const T n = static_cast<T>(x.size());
    const T m = mean(x);
    const T v = variance(x, 1);
    const T t = (m - popmean) / crd::math::sqrt(v / n);
    const T df = n - static_cast<T>(1);
    return {t, detail::t_pvalue(t, df, alt), df};
}

// Two independent samples. equal_var=true → pooled (Student); false → Welch (unequal variance, Satterthwaite df).
// scipy.stats.ttest_ind (the `equal_var` flag matches scipy's).
template <Real T>
[[nodiscard]] TestResult<T> t_test_ind(crd::containers::ConstSpan<T> a, crd::containers::ConstSpan<T> b,
                                       bool equal_var = true, Alternative alt = Alternative::TwoSided)
{
    const T na = static_cast<T>(a.size());
    const T nb = static_cast<T>(b.size());
    const T ma = mean(a);
    const T mb = mean(b);
    const T va = variance(a, 1);
    const T vb = variance(b, 1);
    T t = static_cast<T>(0);
    T df = static_cast<T>(0);
    if (equal_var)
    {
        const T sp2 = ((na - static_cast<T>(1)) * va + (nb - static_cast<T>(1)) * vb) / (na + nb - static_cast<T>(2));
        t = (ma - mb) / crd::math::sqrt(sp2 * (static_cast<T>(1) / na + static_cast<T>(1) / nb));
        df = na + nb - static_cast<T>(2);
    }
    else // Welch
    {
        const T sa = va / na;
        const T sb = vb / nb;
        t = (ma - mb) / crd::math::sqrt(sa + sb);
        df = (sa + sb) * (sa + sb) / (sa * sa / (na - static_cast<T>(1)) + sb * sb / (nb - static_cast<T>(1)));
    }
    return {t, detail::t_pvalue(t, df, alt), df};
}

// Paired (related) samples: a 1-sample t-test on the differences a-b against 0. scipy.stats.ttest_rel. |a| == |b|.
template <Real T>
[[nodiscard]] TestResult<T> t_test_rel(crd::containers::ConstSpan<T> a, crd::containers::ConstSpan<T> b,
                                       Alternative alt = Alternative::TwoSided)
{
    const crd::usize n = a.size();
    const T nt = static_cast<T>(n);
    T sd = static_cast<T>(0);
    for (crd::usize i = 0; i < n; ++i)
    {
        sd += a[i] - b[i];
    }
    const T md = sd / nt;
    T ss = static_cast<T>(0);
    for (crd::usize i = 0; i < n; ++i)
    {
        const T d = (a[i] - b[i]) - md;
        ss += d * d;
    }
    const T v = ss / (nt - static_cast<T>(1));
    const T t = md / crd::math::sqrt(v / nt);
    const T df = nt - static_cast<T>(1);
    return {t, detail::t_pvalue(t, df, alt), df};
}

// ───────────────────────────── ANOVA / variance tests ─────────────────────────────

// One-way ANOVA: H0 all group means equal. F = MS_between / MS_within. scipy.stats.f_oneway.
template <Real T>
[[nodiscard]] FTestResult<T> f_oneway(crd::containers::ConstSpan<crd::containers::ConstSpan<T>> groups)
{
    const T k = static_cast<T>(groups.size());
    T grand_sum = static_cast<T>(0);
    crd::usize total = 0;
    for (const auto& g : groups)
    {
        for (T x : g)
        {
            grand_sum += x;
        }
        total += g.size();
    }
    const T n_total = static_cast<T>(total);
    const T grand_mean = grand_sum / n_total;
    T ssb = static_cast<T>(0);
    T ssw = static_cast<T>(0);
    for (const auto& g : groups)
    {
        T gsum = static_cast<T>(0);
        for (T x : g)
        {
            gsum += x;
        }
        const T gm = gsum / static_cast<T>(g.size());
        const T d = gm - grand_mean;
        ssb += static_cast<T>(g.size()) * d * d;
        for (T x : g)
        {
            const T e = x - gm;
            ssw += e * e;
        }
    }
    const T df_num = k - static_cast<T>(1);
    const T df_den = n_total - k;
    const T f = (ssb / df_num) / (ssw / df_den);
    return {f, detail::f_pvalue(f, df_num, df_den), df_num, df_den};
}

// Bartlett's test: H0 all group variances equal (sensitive to normality). scipy.stats.bartlett.
template <Real T>
[[nodiscard]] TestResult<T> bartlett(crd::containers::ConstSpan<crd::containers::ConstSpan<T>> groups)
{
    const T k = static_cast<T>(groups.size());
    crd::usize total = 0;
    T pooled_num = static_cast<T>(0); // sum (n_i-1) s_i^2
    T sum_ln = static_cast<T>(0);     // sum (n_i-1) ln s_i^2
    T sum_inv = static_cast<T>(0);    // sum 1/(n_i-1)
    for (const auto& g : groups)
    {
        const T ni = static_cast<T>(g.size());
        const T s2 = variance(g, 1);
        pooled_num += (ni - static_cast<T>(1)) * s2;
        sum_ln += (ni - static_cast<T>(1)) * crd::math::log(s2);
        sum_inv += static_cast<T>(1) / (ni - static_cast<T>(1));
        total += g.size();
    }
    const T n_total = static_cast<T>(total);
    const T sp2 = pooled_num / (n_total - k);
    const T num = (n_total - k) * crd::math::log(sp2) - sum_ln;
    const T corr = static_cast<T>(1) +
                   (sum_inv - static_cast<T>(1) / (n_total - k)) / (static_cast<T>(3) * (k - static_cast<T>(1)));
    const T stat = num / corr;
    const T df = k - static_cast<T>(1);
    return {stat, detail::chi2_pvalue(stat, df), df};
}

// Levene's test: robust equal-variance test = a one-way ANOVA on z_ij = |x_ij - center_i|. center=Median is the
// Brown-Forsythe variant (scipy's default, robust to non-normal tails); center=Mean is the original Levene. Needs an
// allocator for the median sort + the z buffer. scipy.stats.levene.
template <Real T>
[[nodiscard]] FTestResult<T> levene(crd::containers::ConstSpan<crd::containers::ConstSpan<T>> groups,
                                    crd::memory::IAllocator* alloc, Center center = Center::Median)
{
    const crd::usize k = groups.size();
    crd::usize total = 0;
    for (const auto& g : groups)
    {
        total += g.size();
    }
    crd::containers::Array<T> z(alloc); // |x - center| flattened, group by group
    z.reserve(total);
    crd::containers::Array<crd::usize> gsz(alloc);
    gsz.reserve(k);
    crd::containers::Array<T> sorted(alloc); // scratch for the median center
    for (const auto& g : groups)
    {
        T c = static_cast<T>(0);
        if (center == Center::Median)
        {
            sorted.resize(g.size());
            for (crd::usize i = 0; i < g.size(); ++i)
            {
                sorted[i] = g[i];
            }
            crd::containers::stable_sort(sorted.data(), sorted.data() + sorted.size(), alloc);
            c = median_sorted(crd::containers::ConstSpan<T>{sorted.data(), sorted.size()});
        }
        else
        {
            c = mean(g);
        }
        for (T x : g)
        {
            z.push_back(crd::math::fabs(x - c));
        }
        gsz.push_back(g.size());
    }
    // one-way ANOVA on z
    const T n_total = static_cast<T>(total);
    T grand_sum = static_cast<T>(0);
    for (T zi : z)
    {
        grand_sum += zi;
    }
    const T grand_mean = grand_sum / n_total;
    T ssb = static_cast<T>(0);
    T ssw = static_cast<T>(0);
    crd::usize off = 0;
    for (crd::usize gi = 0; gi < k; ++gi)
    {
        const crd::usize n = gsz[gi];
        T gsum = static_cast<T>(0);
        for (crd::usize i = 0; i < n; ++i)
        {
            gsum += z[off + i];
        }
        const T gm = gsum / static_cast<T>(n);
        const T d = gm - grand_mean;
        ssb += static_cast<T>(n) * d * d;
        for (crd::usize i = 0; i < n; ++i)
        {
            const T e = z[off + i] - gm;
            ssw += e * e;
        }
        off += n;
    }
    const T df_num = static_cast<T>(k) - static_cast<T>(1);
    const T df_den = n_total - static_cast<T>(k);
    const T f = (ssb / df_num) / (ssw / df_den);
    return {f, detail::f_pvalue(f, df_num, df_den), df_num, df_den};
}

// ───────────────────────────── rank-based (nonparametric) ─────────────────────────────

namespace detail
{
// Standard-normal upper tail P(Z > z) = 0.5 erfc(z/sqrt 2) — the z->p map for the rank tests' normal approximation.
template <Real T> [[nodiscard]] T norm_sf(T z) noexcept
{
    return static_cast<T>(0.5) * special::erfc(z / crd::math::sqrt(static_cast<T>(2)));
}
} // namespace detail

// Average-rank transform (scipy.stats.rankdata, method 'average'): tied values share the mean of their ranks. If
// tie_term != nullptr it receives sum(t^3 - t) over tie groups (the correction shared by Mann-Whitney / Kruskal).
template <Real T>
[[nodiscard]] crd::containers::Array<T> rankdata(crd::containers::ConstSpan<T> x, crd::memory::IAllocator* alloc,
                                                 T* tie_term = nullptr)
{
    const crd::usize n = x.size();
    crd::containers::Array<crd::usize> idx(alloc);
    idx.resize(n);
    for (crd::usize i = 0; i < n; ++i)
    {
        idx[i] = i;
    }
    crd::containers::sort(idx.data(), idx.data() + n, [&](crd::usize i, crd::usize j) { return x[i] < x[j]; });
    crd::containers::Array<T> ranks(alloc);
    ranks.resize(n);
    T ties = static_cast<T>(0);
    crd::usize i = 0;
    while (i < n)
    {
        crd::usize j = i;
        while (j + 1 < n && !(x[idx[i]] < x[idx[j + 1]])) // equal (ascending-sorted, so not greater either)
        {
            ++j;
        }
        const T avg = static_cast<T>(i + j + 2) * static_cast<T>(0.5); // mean of 1-based ranks (i+1)..(j+1)
        for (crd::usize m = i; m <= j; ++m)
        {
            ranks[idx[m]] = avg;
        }
        const T t = static_cast<T>(j - i + 1);
        ties += t * t * t - t;
        i = j + 1;
    }
    if (tie_term != nullptr)
    {
        *tie_term = ties;
    }
    return ranks;
}

// Mann-Whitney U (rank-sum), asymptotic with tie + continuity correction. Returns U1 (scipy's reported statistic).
// scipy.stats.mannwhitneyu(method='asymptotic').
template <Real T>
[[nodiscard]] TestResult<T> mann_whitney_u(crd::containers::ConstSpan<T> a, crd::containers::ConstSpan<T> b,
                                           crd::memory::IAllocator* alloc, Alternative alt = Alternative::TwoSided,
                                           bool continuity = true)
{
    const crd::usize na = a.size();
    const crd::usize nb = b.size();
    crd::containers::Array<T> comb(alloc);
    comb.reserve(na + nb);
    for (T x : a)
    {
        comb.push_back(x);
    }
    for (T x : b)
    {
        comb.push_back(x);
    }
    T tie = static_cast<T>(0);
    const auto ranks = rankdata(crd::containers::ConstSpan<T>{comb.data(), comb.size()}, alloc, &tie);
    T r1 = static_cast<T>(0);
    for (crd::usize i = 0; i < na; ++i)
    {
        r1 += ranks[i];
    }
    const T nat = static_cast<T>(na);
    const T nbt = static_cast<T>(nb);
    const T u1 = r1 - nat * (nat + static_cast<T>(1)) * static_cast<T>(0.5);
    const T u2 = nat * nbt - u1;
    const T nt = nat + nbt;
    const T mu = nat * nbt * static_cast<T>(0.5);
    const T s = crd::math::sqrt(nat * nbt / static_cast<T>(12) * ((nt + static_cast<T>(1)) - tie / (nt * (nt - 1))));
    T u = (u1 > u2) ? u1 : u2;
    T f = static_cast<T>(2);
    if (alt == Alternative::Greater)
    {
        u = u1;
        f = static_cast<T>(1);
    }
    else if (alt == Alternative::Less)
    {
        u = u2;
        f = static_cast<T>(1);
    }
    T num = u - mu;
    if (continuity)
    {
        num -= static_cast<T>(0.5);
    }
    T p = f * detail::norm_sf(num / s);
    if (p > static_cast<T>(1))
    {
        p = static_cast<T>(1);
    }
    return {u1, p, static_cast<T>(0)};
}

// Wilcoxon signed-rank (paired), asymptotic, zeros dropped (scipy 'wilcox'), tie-corrected. Two-sided statistic is
// min(R+, R-). scipy.stats.wilcoxon(mode/method='approx', correction=False).
template <Real T>
[[nodiscard]] TestResult<T> wilcoxon(crd::containers::ConstSpan<T> a, crd::containers::ConstSpan<T> b,
                                     crd::memory::IAllocator* alloc, Alternative alt = Alternative::TwoSided)
{
    crd::containers::Array<T> d(alloc);  // signed nonzero differences
    crd::containers::Array<T> ad(alloc); // |d|
    for (crd::usize i = 0; i < a.size(); ++i)
    {
        const T di = a[i] - b[i];
        if (di != static_cast<T>(0))
        {
            d.push_back(di);
            ad.push_back(di < static_cast<T>(0) ? -di : di);
        }
    }
    const crd::usize n = d.size();
    const T nt = static_cast<T>(n);
    T tie = static_cast<T>(0);
    const auto r = rankdata(crd::containers::ConstSpan<T>{ad.data(), ad.size()}, alloc, &tie);
    T r_plus = static_cast<T>(0);
    T r_minus = static_cast<T>(0);
    for (crd::usize i = 0; i < n; ++i)
    {
        (d[i] > static_cast<T>(0) ? r_plus : r_minus) += r[i];
    }
    T stat = (r_plus < r_minus) ? r_plus : r_minus;
    T f = static_cast<T>(2);
    if (alt == Alternative::Greater)
    {
        stat = r_plus;
        f = static_cast<T>(1);
    }
    else if (alt == Alternative::Less)
    {
        stat = r_minus;
        f = static_cast<T>(1);
    }
    const T mn = nt * (nt + static_cast<T>(1)) * static_cast<T>(0.25);
    T se = nt * (nt + static_cast<T>(1)) * (static_cast<T>(2) * nt + static_cast<T>(1));
    se -= static_cast<T>(0.5) * tie; // scipy: se -= 0.5 * sum(t^3 - t)
    se = crd::math::sqrt(se / static_cast<T>(24));
    const T z = (stat - mn) / se;
    T p = f * detail::norm_sf(z < static_cast<T>(0) ? -z : z);
    if (p > static_cast<T>(1))
    {
        p = static_cast<T>(1);
    }
    return {stat, p, static_cast<T>(0)};
}

// Kruskal-Wallis H (rank-based one-way ANOVA), tie-corrected, p from chi-square(k-1). scipy.stats.kruskal.
template <Real T>
[[nodiscard]] TestResult<T> kruskal(crd::containers::ConstSpan<crd::containers::ConstSpan<T>> groups,
                                    crd::memory::IAllocator* alloc)
{
    const crd::usize k = groups.size();
    crd::usize total = 0;
    for (const auto& g : groups)
    {
        total += g.size();
    }
    crd::containers::Array<T> comb(alloc);
    comb.reserve(total);
    for (const auto& g : groups)
    {
        for (T x : g)
        {
            comb.push_back(x);
        }
    }
    T tie = static_cast<T>(0);
    const auto ranks = rankdata(crd::containers::ConstSpan<T>{comb.data(), comb.size()}, alloc, &tie);
    const T nt = static_cast<T>(total);
    T h = static_cast<T>(0);
    crd::usize off = 0;
    for (const auto& g : groups)
    {
        T rsum = static_cast<T>(0);
        for (crd::usize i = 0; i < g.size(); ++i)
        {
            rsum += ranks[off + i];
        }
        h += rsum * rsum / static_cast<T>(g.size());
        off += g.size();
    }
    h = static_cast<T>(12) / (nt * (nt + static_cast<T>(1))) * h - static_cast<T>(3) * (nt + static_cast<T>(1));
    h /= static_cast<T>(1) - tie / (nt * nt * nt - nt); // tie correction
    const T df = static_cast<T>(k) - static_cast<T>(1);
    return {h, detail::chi2_pvalue(h, df), df};
}

// Friedman chi-square (repeated-measures, k treatments x n blocks): rank within each block, tie-corrected. Each group
// is one treatment column of length n (the block count). scipy.stats.friedmanchisquare.
template <Real T>
[[nodiscard]] TestResult<T> friedman(crd::containers::ConstSpan<crd::containers::ConstSpan<T>> groups,
                                     crd::memory::IAllocator* alloc)
{
    const crd::usize k = groups.size();
    const crd::usize n = groups[0].size();
    crd::containers::Array<T> colsum(alloc);
    colsum.resize(k);
    for (crd::usize j = 0; j < k; ++j)
    {
        colsum[j] = static_cast<T>(0);
    }
    crd::containers::Array<T> block(alloc);
    block.resize(k);
    T ties = static_cast<T>(0);
    for (crd::usize i = 0; i < n; ++i)
    {
        for (crd::usize j = 0; j < k; ++j)
        {
            block[j] = groups[j][i];
        }
        T bt = static_cast<T>(0);
        const auto r = rankdata(crd::containers::ConstSpan<T>{block.data(), block.size()}, alloc, &bt);
        ties += bt;
        for (crd::usize j = 0; j < k; ++j)
        {
            colsum[j] += r[j];
        }
    }
    const T kt = static_cast<T>(k);
    const T nt = static_cast<T>(n);
    T ssbn = static_cast<T>(0);
    for (crd::usize j = 0; j < k; ++j)
    {
        ssbn += colsum[j] * colsum[j];
    }
    T chisq = static_cast<T>(12) / (kt * nt * (kt + static_cast<T>(1))) * ssbn - static_cast<T>(3) * nt * (kt + static_cast<T>(1));
    chisq /= static_cast<T>(1) - ties / (kt * (kt * kt - static_cast<T>(1)) * nt); // tie correction
    const T df = kt - static_cast<T>(1);
    return {chisq, detail::chi2_pvalue(chisq, df), df};
}

// ───────────────────────────── correlation ─────────────────────────────

// Pearson product-moment correlation + two-sided p (t-distribution, df = n-2). scipy.stats.pearsonr.
template <Real T>
[[nodiscard]] TestResult<T> pearsonr(crd::containers::ConstSpan<T> x, crd::containers::ConstSpan<T> y)
{
    const crd::usize n = x.size();
    const T mx = mean(x);
    const T my = mean(y);
    T sxy = static_cast<T>(0);
    T sxx = static_cast<T>(0);
    T syy = static_cast<T>(0);
    for (crd::usize i = 0; i < n; ++i)
    {
        const T dx = x[i] - mx;
        const T dy = y[i] - my;
        sxy += dx * dy;
        sxx += dx * dx;
        syy += dy * dy;
    }
    const T r = sxy / crd::math::sqrt(sxx * syy);
    const T df = static_cast<T>(n) - static_cast<T>(2);
    const T t = r * crd::math::sqrt(df / (static_cast<T>(1) - r * r));
    return {r, detail::t_pvalue(t, df, Alternative::TwoSided), df};
}

// Spearman rank correlation = Pearson on the ranks; p via the same t-approximation. scipy.stats.spearmanr.
template <Real T>
[[nodiscard]] TestResult<T> spearmanr(crd::containers::ConstSpan<T> x, crd::containers::ConstSpan<T> y,
                                      crd::memory::IAllocator* alloc)
{
    const auto rx = rankdata(x, alloc);
    const auto ry = rankdata(y, alloc);
    return pearsonr(crd::containers::ConstSpan<T>{rx.data(), rx.size()},
                    crd::containers::ConstSpan<T>{ry.data(), ry.size()});
}

namespace detail
{
// Kendall tie terms over one variable's tie groups: tx = sum t(t-1)/2, t0 = sum t(t-1)(t-2), t1 = sum t(t-1)(2t+5).
template <Real T> struct TieTerms
{
    T tx;
    T t0;
    T t1;
};
template <Real T> [[nodiscard]] TieTerms<T> tie_terms(crd::containers::ConstSpan<T> x, crd::memory::IAllocator* alloc)
{
    crd::containers::Array<T> s(alloc);
    s.resize(x.size());
    for (crd::usize i = 0; i < x.size(); ++i)
    {
        s[i] = x[i];
    }
    crd::containers::stable_sort(s.data(), s.data() + s.size(), alloc);
    TieTerms<T> r{static_cast<T>(0), static_cast<T>(0), static_cast<T>(0)};
    crd::usize i = 0;
    while (i < s.size())
    {
        crd::usize j = i;
        while (j + 1 < s.size() && s[j + 1] == s[i])
        {
            ++j;
        }
        const T t = static_cast<T>(j - i + 1);
        if (t > static_cast<T>(1))
        {
            r.tx += t * (t - 1) * static_cast<T>(0.5);
            r.t0 += t * (t - 1) * (t - 2);
            r.t1 += t * (t - 1) * (static_cast<T>(2) * t + 5);
        }
        i = j + 1;
    }
    return r;
}
} // namespace detail

// Kendall tau-b + two-sided p (normal approximation, tie-corrected variance). scipy.stats.kendalltau.
template <Real T>
[[nodiscard]] TestResult<T> kendalltau(crd::containers::ConstSpan<T> x, crd::containers::ConstSpan<T> y,
                                       crd::memory::IAllocator* alloc)
{
    const crd::usize n = x.size();
    T cmd = static_cast<T>(0); // concordant - discordant
    for (crd::usize i = 0; i < n; ++i)
    {
        for (crd::usize j = i + 1; j < n; ++j)
        {
            const T s = (x[i] - x[j]) * (y[i] - y[j]);
            if (s > static_cast<T>(0))
            {
                cmd += static_cast<T>(1);
            }
            else if (s < static_cast<T>(0))
            {
                cmd -= static_cast<T>(1);
            }
        }
    }
    const T tot = static_cast<T>(n) * (static_cast<T>(n) - 1) * static_cast<T>(0.5);
    const auto xt = detail::tie_terms(x, alloc);
    const auto yt = detail::tie_terms(y, alloc);
    const T tau = cmd / crd::math::sqrt((tot - xt.tx) * (tot - yt.tx));
    const T m = static_cast<T>(n) * (static_cast<T>(n) - 1);
    const T var = (m * (static_cast<T>(2) * static_cast<T>(n) + 5) - xt.t1 - yt.t1) / static_cast<T>(18) +
                  (static_cast<T>(2) * xt.tx * yt.tx) / m +
                  xt.t0 * yt.t0 / (static_cast<T>(9) * m * (static_cast<T>(n) - 2));
    const T z = cmd / crd::math::sqrt(var);
    return {tau, static_cast<T>(2) * detail::norm_sf(z < static_cast<T>(0) ? -z : z), static_cast<T>(0)};
}

// ───────────────────────────── categorical ─────────────────────────────

// Chi-square test of independence on an r x c contingency table (row-major `observed`). Yates' continuity correction
// is applied when correction && df==1 (the 2x2 case), matching scipy's default. scipy.stats.chi2_contingency.
template <Real T>
[[nodiscard]] TestResult<T> chi2_contingency(crd::containers::ConstSpan<T> observed, crd::usize rows, crd::usize cols,
                                             crd::memory::IAllocator* alloc, bool correction = true)
{
    crd::containers::Array<T> rt(alloc);
    crd::containers::Array<T> ct(alloc);
    rt.resize(rows);
    ct.resize(cols);
    for (crd::usize r = 0; r < rows; ++r)
    {
        rt[r] = static_cast<T>(0);
    }
    for (crd::usize c = 0; c < cols; ++c)
    {
        ct[c] = static_cast<T>(0);
    }
    T grand = static_cast<T>(0);
    for (crd::usize r = 0; r < rows; ++r)
    {
        for (crd::usize c = 0; c < cols; ++c)
        {
            const T o = observed[r * cols + c];
            rt[r] += o;
            ct[c] += o;
            grand += o;
        }
    }
    const T df = static_cast<T>(rows - 1) * static_cast<T>(cols - 1);
    const bool yates = correction && (df == static_cast<T>(1));
    T chi2 = static_cast<T>(0);
    for (crd::usize r = 0; r < rows; ++r)
    {
        for (crd::usize c = 0; c < cols; ++c)
        {
            const T e = rt[r] * ct[c] / grand;
            T diff = observed[r * cols + c] - e;
            if (diff < static_cast<T>(0))
            {
                diff = -diff;
            }
            if (yates)
            {
                diff -= static_cast<T>(0.5);
            }
            chi2 += diff * diff / e;
        }
    }
    return {chi2, detail::chi2_pvalue(chi2, df), df};
}

namespace detail
{
// Two-sided Fisher exact p for a 2x2 table [[a,b],[c,d]]: sum of hypergeometric probabilities <= P(observed) (scipy's
// relative-tolerance definition). Probabilities via lgamma log-choose.
template <Real T> [[nodiscard]] T fisher_two_sided(T a, T b, T c, T d)
{
    const T r1 = a + b;
    const T r2 = c + d;
    const T c1 = a + c;
    const T nn = a + b + c + d;
    const auto lchoose = [](T n, T k) {
        return special::lgamma(n + 1) - special::lgamma(k + 1) - special::lgamma(n - k + 1);
    };
    const auto lpmf = [&](T k) { return lchoose(r1, k) + lchoose(r2, c1 - k) - lchoose(nn, c1); };
    const crd::isize kmin = static_cast<crd::isize>((c1 - r2 > static_cast<T>(0)) ? (c1 - r2) : static_cast<T>(0));
    const crd::isize kmax = static_cast<crd::isize>((r1 < c1) ? r1 : c1);
    const T p_obs = crd::math::exp(lpmf(a));
    const T tol = p_obs * (static_cast<T>(1) + static_cast<T>(1e-7));
    T p = static_cast<T>(0);
    for (crd::isize k = kmin; k <= kmax; ++k)
    {
        const T pk = crd::math::exp(lpmf(static_cast<T>(k)));
        if (pk <= tol)
        {
            p += pk;
        }
    }
    return (p > static_cast<T>(1)) ? static_cast<T>(1) : p;
}
} // namespace detail

// Fisher exact test for a 2x2 table [[a,b],[c,d]]: statistic = sample odds ratio (a d)/(b c), two-sided exact p.
// scipy.stats.fisher_exact.
template <Real T> [[nodiscard]] TestResult<T> fisher_exact(T a, T b, T c, T d)
{
    return {(a * d) / (b * c), detail::fisher_two_sided(a, b, c, d), static_cast<T>(0)};
}

// McNemar's test on the two discordant cells (b, c) of a 2x2 paired table. correction = Edwards' continuity (default).
// statistic = (|b-c| - corr)^2 / (b+c), p from chi-square(1). (statsmodels.stats.contingency_tables.mcnemar, chi2 form.)
template <Real T> [[nodiscard]] TestResult<T> mcnemar(T b, T c, bool correction = true)
{
    T diff = (b > c) ? b - c : c - b;
    if (correction)
    {
        diff -= static_cast<T>(1);
    }
    const T stat = diff * diff / (b + c);
    return {stat, detail::chi2_pvalue(stat, static_cast<T>(1)), static_cast<T>(1)};
}

// ───────────────────────────── goodness-of-fit ─────────────────────────────

namespace detail
{
// Kolmogorov distribution upper tail Q(t) = 2 sum_{k>=1} (-1)^{k-1} e^{-2 k^2 t^2} (= scipy.special.kolmogorov), the
// asymptotic p for the KS statistic. Converges fast; alternating series.
template <Real T> [[nodiscard]] T kolmogorov_sf(T t) noexcept
{
    if (t <= static_cast<T>(0))
    {
        return static_cast<T>(1);
    }
    T sum = static_cast<T>(0);
    T sign = static_cast<T>(1);
    for (int k = 1; k <= 200; ++k)
    {
        const T kk = static_cast<T>(k);
        const T term = crd::math::exp(static_cast<T>(-2) * kk * kk * t * t);
        sum += sign * term;
        sign = -sign;
        if (term < static_cast<T>(1e-20))
        {
            break;
        }
    }
    T p = static_cast<T>(2) * sum;
    p = (p < static_cast<T>(0)) ? static_cast<T>(0) : p;
    return (p > static_cast<T>(1)) ? static_cast<T>(1) : p;
}
} // namespace detail

// Jarque-Bera normality test: JB = n/6 (skew^2 + excess_kurt^2/4), p from chi-square(2). scipy.stats.jarque_bera.
template <Real T> [[nodiscard]] TestResult<T> jarque_bera(crd::containers::ConstSpan<T> x)
{
    const T n = static_cast<T>(x.size());
    const T s = skewness(x);
    const T k = kurtosis(x); // excess
    const T jb = n / static_cast<T>(6) * (s * s + k * k / static_cast<T>(4));
    return {jb, detail::chi2_pvalue(jb, static_cast<T>(2)), static_cast<T>(2)};
}

// One-sample Kolmogorov-Smirnov against a continuous CDF (callable T->T); asymptotic p (scipy.special.kolmogorov of
// sqrt(n) D). For large n this matches scipy.kstest; scipy uses the exact small-n distribution.
template <Real T, typename Cdf>
[[nodiscard]] TestResult<T> ks_1samp(crd::containers::ConstSpan<T> x, Cdf cdf, crd::memory::IAllocator* alloc)
{
    const crd::usize n = x.size();
    crd::containers::Array<T> s(alloc);
    s.resize(n);
    for (crd::usize i = 0; i < n; ++i)
    {
        s[i] = x[i];
    }
    crd::containers::sort(s.data(), s.data() + n);
    const T nt = static_cast<T>(n);
    T d = static_cast<T>(0);
    for (crd::usize i = 0; i < n; ++i)
    {
        const T f = cdf(s[i]);
        const T dp = static_cast<T>(i + 1) / nt - f;
        const T dm = f - static_cast<T>(i) / nt;
        d = (dp > d) ? dp : d;
        d = (dm > d) ? dm : d;
    }
    return {d, detail::kolmogorov_sf(crd::math::sqrt(nt) * d), static_cast<T>(0)};
}

// Two-sample Kolmogorov-Smirnov: D = sup|F_a - F_b|, asymptotic p (kolmogorov of sqrt(nm/(n+m)) D). scipy.stats.ks_2samp.
template <Real T>
[[nodiscard]] TestResult<T> ks_2samp(crd::containers::ConstSpan<T> a, crd::containers::ConstSpan<T> b,
                                     crd::memory::IAllocator* alloc)
{
    const crd::usize na = a.size();
    const crd::usize nb = b.size();
    crd::containers::Array<T> sa(alloc);
    crd::containers::Array<T> sb(alloc);
    sa.resize(na);
    sb.resize(nb);
    for (crd::usize i = 0; i < na; ++i)
    {
        sa[i] = a[i];
    }
    for (crd::usize i = 0; i < nb; ++i)
    {
        sb[i] = b[i];
    }
    crd::containers::sort(sa.data(), sa.data() + na);
    crd::containers::sort(sb.data(), sb.data() + nb);
    const T inva = static_cast<T>(1) / static_cast<T>(na);
    const T invb = static_cast<T>(1) / static_cast<T>(nb);
    crd::usize i = 0;
    crd::usize j = 0;
    T fa = static_cast<T>(0);
    T fb = static_cast<T>(0);
    T d = static_cast<T>(0);
    while (i < na && j < nb)
    {
        if (sa[i] < sb[j])
        {
            fa += inva;
            ++i;
        }
        else if (sb[j] < sa[i])
        {
            fb += invb;
            ++j;
        }
        else
        {
            const T v = sa[i];
            while (i < na && sa[i] == v)
            {
                fa += inva;
                ++i;
            }
            while (j < nb && sb[j] == v)
            {
                fb += invb;
                ++j;
            }
        }
        const T diff = (fa > fb) ? fa - fb : fb - fa;
        d = (diff > d) ? diff : d;
    }
    const T en = crd::math::sqrt(static_cast<T>(na) * static_cast<T>(nb) / static_cast<T>(na + nb));
    return {d, detail::kolmogorov_sf(en * d), static_cast<T>(0)};
}

// ───────────────────────────── effect sizes ─────────────────────────────

// Cohen's d (two independent samples), pooled standard deviation. (No standard scipy function; the textbook formula.)
template <Real T> [[nodiscard]] T cohens_d(crd::containers::ConstSpan<T> a, crd::containers::ConstSpan<T> b)
{
    const T na = static_cast<T>(a.size());
    const T nb = static_cast<T>(b.size());
    const T va = variance(a, 1);
    const T vb = variance(b, 1);
    const T sp = crd::math::sqrt(((na - 1) * va + (nb - 1) * vb) / (na + nb - 2));
    return (mean(a) - mean(b)) / sp;
}

// Eta-squared (one-way ANOVA effect size) = SS_between / SS_total.
template <Real T> [[nodiscard]] T eta_squared(crd::containers::ConstSpan<crd::containers::ConstSpan<T>> groups)
{
    T gs = static_cast<T>(0);
    crd::usize total = 0;
    for (const auto& g : groups)
    {
        for (T x : g)
        {
            gs += x;
        }
        total += g.size();
    }
    const T gm = gs / static_cast<T>(total);
    T ssb = static_cast<T>(0);
    T sst = static_cast<T>(0);
    for (const auto& g : groups)
    {
        T m = static_cast<T>(0);
        for (T x : g)
        {
            m += x;
        }
        m /= static_cast<T>(g.size());
        const T dm = m - gm;
        ssb += static_cast<T>(g.size()) * dm * dm;
        for (T x : g)
        {
            const T e = x - gm;
            sst += e * e;
        }
    }
    return ssb / sst;
}

// ───────────────────────────── multiple-comparison p-adjustment ─────────────────────────────

// Holm-Bonferroni step-down adjusted p-values (statsmodels multipletests 'holm'). `out` is filled in input order.
template <Real T>
void holm(crd::containers::ConstSpan<T> p, crd::containers::Span<T> out, crd::memory::IAllocator* alloc)
{
    const crd::usize m = p.size();
    crd::containers::Array<crd::usize> idx(alloc);
    idx.resize(m);
    for (crd::usize i = 0; i < m; ++i)
    {
        idx[i] = i;
    }
    crd::containers::sort(idx.data(), idx.data() + m, [&](crd::usize i, crd::usize j) { return p[i] < p[j]; });
    T run = static_cast<T>(0);
    for (crd::usize k = 0; k < m; ++k)
    {
        const crd::usize id = idx[k];
        const T adj = static_cast<T>(m - k) * p[id];
        run = (adj > run) ? adj : run;
        out[id] = (run > static_cast<T>(1)) ? static_cast<T>(1) : run;
    }
}

// Benjamini-Hochberg FDR adjusted p-values (scipy.stats.false_discovery_control, method 'bh'). `out` in input order.
template <Real T>
void benjamini_hochberg(crd::containers::ConstSpan<T> p, crd::containers::Span<T> out, crd::memory::IAllocator* alloc)
{
    const crd::usize m = p.size();
    crd::containers::Array<crd::usize> idx(alloc);
    idx.resize(m);
    for (crd::usize i = 0; i < m; ++i)
    {
        idx[i] = i;
    }
    crd::containers::sort(idx.data(), idx.data() + m, [&](crd::usize i, crd::usize j) { return p[i] < p[j]; });
    T prev = static_cast<T>(1);
    for (crd::usize kk = m; kk > 0; --kk) // step-up from the largest rank (SANITY #10: no `k-- > 0` idiom)
    {
        const crd::usize k = kk - 1;
        const crd::usize id = idx[k];
        const T val = p[id] * static_cast<T>(m) / static_cast<T>(k + 1);
        prev = (val < prev) ? val : prev;
        out[id] = (prev > static_cast<T>(1)) ? static_cast<T>(1) : prev;
    }
}

// ───────────────────────────── GoF (hard) + distance correlation ─────────────────────────────

// Anderson-Darling test for normality (parameters estimated): A^2 statistic + D'Agostino-Stephens approximate p.
// scipy.stats.anderson(x, 'norm') gives the same statistic (it standardizes with the ddof=1 std).
template <Real T>
[[nodiscard]] TestResult<T> anderson_darling(crd::containers::ConstSpan<T> x, crd::memory::IAllocator* alloc)
{
    const crd::usize n = x.size();
    const T nt = static_cast<T>(n);
    const T xbar = mean(x);
    const T sd = crd::math::sqrt(variance(x, 1));
    crd::containers::Array<T> z(alloc);
    z.resize(n);
    for (crd::usize i = 0; i < n; ++i)
    {
        z[i] = (x[i] - xbar) / sd;
    }
    crd::containers::sort(z.data(), z.data() + n);
    T acc = static_cast<T>(0);
    for (crd::usize i = 0; i < n; ++i)
    {
        const T log_cdf = crd::math::log(detail::norm_sf(-z[i]));        // ln Phi(z_i)
        const T log_sf = crd::math::log(detail::norm_sf(z[n - 1 - i]));  // ln(1 - Phi(z_{n+1-i}))
        acc += static_cast<T>(2 * (i + 1) - 1) * (log_cdf + log_sf);
    }
    const T a2 = -nt - acc / nt;
    const T as = a2 * (static_cast<T>(1) + static_cast<T>(0.75) / nt + static_cast<T>(2.25) / (nt * nt));
    T p;
    if (as < static_cast<T>(0.2))
    {
        p = static_cast<T>(1) - crd::math::exp(static_cast<T>(-13.436) + static_cast<T>(101.14) * as -
                                               static_cast<T>(223.73) * as * as);
    }
    else if (as < static_cast<T>(0.34))
    {
        p = static_cast<T>(1) - crd::math::exp(static_cast<T>(-8.318) + static_cast<T>(42.796) * as -
                                               static_cast<T>(59.938) * as * as);
    }
    else if (as < static_cast<T>(0.6))
    {
        p = crd::math::exp(static_cast<T>(0.9177) - static_cast<T>(4.279) * as - static_cast<T>(1.38) * as * as);
    }
    else
    {
        p = crd::math::exp(static_cast<T>(1.2937) - static_cast<T>(5.709) * as + static_cast<T>(0.0186) * as * as);
    }
    return {a2, p, static_cast<T>(0)};
}

// Shapiro-Wilk normality test (Royston 1992, AS R94) for 4 <= n <= ~5000. W statistic + normalized p. scipy.stats.shapiro.
template <Real T>
[[nodiscard]] TestResult<T> shapiro(crd::containers::ConstSpan<T> x, crd::memory::IAllocator* alloc)
{
    const crd::usize n = x.size();
    const T nt = static_cast<T>(n);
    crd::containers::Array<T> s(alloc);
    s.resize(n);
    for (crd::usize i = 0; i < n; ++i)
    {
        s[i] = x[i];
    }
    crd::containers::sort(s.data(), s.data() + n);
    crd::containers::Array<T> mm(alloc);
    mm.resize(n);
    T ssm = static_cast<T>(0);
    for (crd::usize i = 0; i < n; ++i)
    {
        mm[i] = special::ndtri((static_cast<T>(i + 1) - static_cast<T>(0.375)) / (nt + static_cast<T>(0.25)));
        ssm += mm[i] * mm[i];
    }
    const T u = static_cast<T>(1) / crd::math::sqrt(nt);
    const auto poly1 = [](T t) {
        return ((((static_cast<T>(-2.706056) * t + static_cast<T>(4.434685)) * t - static_cast<T>(2.071190)) * t -
                 static_cast<T>(0.147981)) *
                    t +
                static_cast<T>(0.221157)) *
               t;
    };
    const auto poly2 = [](T t) {
        return ((((static_cast<T>(-3.582633) * t + static_cast<T>(5.682633)) * t - static_cast<T>(1.752461)) * t -
                 static_cast<T>(0.293762)) *
                    t +
                static_cast<T>(0.042981)) *
               t;
    };
    const T rs = crd::math::sqrt(ssm);
    const T an = mm[n - 1] / rs + poly1(u);
    const T anm1 = mm[n - 2] / rs + poly2(u);
    const T phi = (ssm - static_cast<T>(2) * mm[n - 1] * mm[n - 1] - static_cast<T>(2) * mm[n - 2] * mm[n - 2]) /
                  (static_cast<T>(1) - static_cast<T>(2) * an * an - static_cast<T>(2) * anm1 * anm1);
    crd::containers::Array<T> aa(alloc);
    aa.resize(n);
    aa[n - 1] = an;
    aa[n - 2] = anm1;
    aa[0] = -an;
    aa[1] = -anm1;
    const T sphi = crd::math::sqrt(phi);
    for (crd::usize i = 2; i < n - 2; ++i)
    {
        aa[i] = mm[i] / sphi;
    }
    const T xbar = mean(crd::containers::ConstSpan<T>{s.data(), n});
    T num = static_cast<T>(0);
    T den = static_cast<T>(0);
    for (crd::usize i = 0; i < n; ++i)
    {
        num += aa[i] * s[i];
        const T d = s[i] - xbar;
        den += d * d;
    }
    const T w = num * num / den;
    // Royston p-value: separate normalizing transforms for 4<=n<=11 and n>=12.
    T z;
    if (n <= 11)
    {
        const T g = static_cast<T>(-2.273) + static_cast<T>(0.459) * nt;
        const T w1 = -crd::math::log(g - crd::math::log(static_cast<T>(1) - w));
        const T mu = static_cast<T>(0.5440) - static_cast<T>(0.39978) * nt + static_cast<T>(0.025054) * nt * nt -
                     static_cast<T>(0.0006714) * nt * nt * nt;
        const T sig = crd::math::exp(static_cast<T>(1.3822) - static_cast<T>(0.77857) * nt +
                                     static_cast<T>(0.062767) * nt * nt - static_cast<T>(0.0020322) * nt * nt * nt);
        z = (w1 - mu) / sig;
    }
    else
    {
        const T ln = crd::math::log(nt);
        const T w1 = crd::math::log(static_cast<T>(1) - w);
        const T mu = static_cast<T>(-1.5861) - static_cast<T>(0.31082) * ln - static_cast<T>(0.083751) * ln * ln +
                     static_cast<T>(0.0038915) * ln * ln * ln;
        const T sig =
            crd::math::exp(static_cast<T>(-0.4803) - static_cast<T>(0.082676) * ln + static_cast<T>(0.0030302) * ln * ln);
        z = (w1 - mu) / sig;
    }
    return {w, detail::norm_sf(z), static_cast<T>(0)};
}

// Distance correlation (Szekely-Rizzo): the statistic in [0,1] (0 iff independent for the population). O(n^2). The
// permutation p-value is omitted (it is randomized). Reference: the dcor package / hand-computed double-centering.
template <Real T>
[[nodiscard]] T distance_correlation(crd::containers::ConstSpan<T> x, crd::containers::ConstSpan<T> y,
                                     crd::memory::IAllocator* alloc)
{
    const crd::usize n = x.size();
    crd::containers::Array<T> a(alloc);
    crd::containers::Array<T> b(alloc);
    a.resize(n * n);
    b.resize(n * n);
    for (crd::usize i = 0; i < n; ++i)
    {
        for (crd::usize j = 0; j < n; ++j)
        {
            a[i * n + j] = crd::math::fabs(x[i] - x[j]);
            b[i * n + j] = crd::math::fabs(y[i] - y[j]);
        }
    }
    const auto center = [&](crd::containers::Array<T>& mtx) {
        crd::containers::Array<T> rm(alloc);
        crd::containers::Array<T> cm(alloc);
        rm.resize(n);
        cm.resize(n);
        for (crd::usize i = 0; i < n; ++i)
        {
            rm[i] = static_cast<T>(0);
            cm[i] = static_cast<T>(0);
        }
        T gm = static_cast<T>(0);
        for (crd::usize i = 0; i < n; ++i)
        {
            for (crd::usize j = 0; j < n; ++j)
            {
                const T v = mtx[i * n + j];
                rm[i] += v;
                cm[j] += v;
                gm += v;
            }
        }
        for (crd::usize i = 0; i < n; ++i)
        {
            rm[i] /= static_cast<T>(n);
            cm[i] /= static_cast<T>(n);
        }
        gm /= static_cast<T>(n) * static_cast<T>(n);
        for (crd::usize i = 0; i < n; ++i)
        {
            for (crd::usize j = 0; j < n; ++j)
            {
                mtx[i * n + j] = mtx[i * n + j] - rm[i] - cm[j] + gm;
            }
        }
    };
    center(a);
    center(b);
    T dcov2 = static_cast<T>(0);
    T dvx = static_cast<T>(0);
    T dvy = static_cast<T>(0);
    for (crd::usize k = 0; k < n * n; ++k)
    {
        dcov2 += a[k] * b[k];
        dvx += a[k] * a[k];
        dvy += b[k] * b[k];
    }
    return crd::math::sqrt(dcov2 / crd::math::sqrt(dvx * dvy));
}

// ───────────────────────────── Tukey HSD (studentized range) ─────────────────────────────

namespace detail
{
// Studentized range upper tail P(Q_{k,nu} > q). The CDF is a double integral; computed exactly (no truncation) as
// Gauss-Hermite over the range CDF G(w;k) = k/sqrt(pi) sum w_i [Phi(z_i sqrt2) - Phi(z_i sqrt2 - w)]^{k-1}, then
// Gauss-Laguerre(alpha = nu/2 - 1) over the chi mixing: F(q) = (1/Gamma(nu/2)) sum w_j G(q sqrt(2 x_j/nu); k).
template <Real T> [[nodiscard]] T studentized_range_sf(T q, T k, T nu, crd::memory::IAllocator* alloc)
{
    constexpr int nh = 64;
    constexpr int nl = 64;
    crd::containers::Array<T> hz(alloc);
    crd::containers::Array<T> hw(alloc);
    crd::containers::Array<T> lz(alloc);
    crd::containers::Array<T> lw(alloc);
    hz.resize(nh);
    hw.resize(nh);
    lz.resize(nl);
    lw.resize(nl);
    crd::hesap::quadrature::gauss_hermite<T>(alloc, nh, hz.data(), hw.data());
    crd::hesap::quadrature::gauss_laguerre<T>(alloc, nl, nu / static_cast<T>(2) - static_cast<T>(1), lz.data(),
                                              lw.data());
    const T sqrt2 = crd::math::sqrt(static_cast<T>(2));
    const T inv_sqrtpi = static_cast<T>(1) / crd::math::sqrt(kPi<T>);
    const auto range_cdf = [&](T w) {
        T s = static_cast<T>(0);
        for (int i = 0; i < nh; ++i)
        {
            const T zz = hz[i] * sqrt2;
            const T base = norm_sf(-zz) - norm_sf(-(zz - w)); // Phi(zz) - Phi(zz - w)
            s += hw[i] * crd::math::pow(base, k - static_cast<T>(1));
        }
        return k * inv_sqrtpi * s;
    };
    const T gnu = special::gamma(nu / static_cast<T>(2));
    T f = static_cast<T>(0);
    for (int j = 0; j < nl; ++j)
    {
        f += lw[j] * range_cdf(q * crd::math::sqrt(static_cast<T>(2) * lz[j] / nu));
    }
    f /= gnu;
    const T sf = static_cast<T>(1) - f;
    return (sf < static_cast<T>(0)) ? static_cast<T>(0) : ((sf > static_cast<T>(1)) ? static_cast<T>(1) : sf);
}
} // namespace detail

// Tukey HSD pairwise comparison of groups i and j after a one-way layout. statistic = mean_i - mean_j (raw difference,
// as scipy reports); p from the studentized range on |diff|/se, se = sqrt(MSE/2 (1/n_i + 1/n_j)), k groups, df = N-k.
// scipy.stats.tukey_hsd.
template <Real T>
[[nodiscard]] TestResult<T> tukey_hsd(crd::containers::ConstSpan<crd::containers::ConstSpan<T>> groups, crd::usize i,
                                      crd::usize j, crd::memory::IAllocator* alloc)
{
    const crd::usize k = groups.size();
    crd::usize total = 0;
    T ssw = static_cast<T>(0);
    for (const auto& g : groups)
    {
        total += g.size();
        const T m = mean(g);
        for (T x : g)
        {
            const T e = x - m;
            ssw += e * e;
        }
    }
    const T df = static_cast<T>(total - k);
    const T mse = ssw / df;
    const T diff = mean(groups[i]) - mean(groups[j]);
    const T se = crd::math::sqrt(mse / static_cast<T>(2) *
                                 (static_cast<T>(1) / static_cast<T>(groups[i].size()) +
                                  static_cast<T>(1) / static_cast<T>(groups[j].size())));
    const T q = (diff < static_cast<T>(0) ? -diff : diff) / se;
    return {diff, detail::studentized_range_sf(q, static_cast<T>(k), df, alloc), df};
}

// ───────────────────────────── v12-n variants (close-out) ─────────────────────────────

// One-sample z-test (large-sample): z = (mean - popmean)/(s/sqrt n), s = sample std (ddof=1), normal tail.
// statsmodels.stats.weightstats.ztest.
template <Real T>
[[nodiscard]] TestResult<T> z_test_1samp(crd::containers::ConstSpan<T> x, T popmean,
                                         Alternative alt = Alternative::TwoSided)
{
    const T n = static_cast<T>(x.size());
    const T z = (mean(x) - popmean) / (crd::math::sqrt(variance(x, 1)) / crd::math::sqrt(n));
    T p;
    if (alt == Alternative::Greater)
    {
        p = detail::norm_sf(z);
    }
    else if (alt == Alternative::Less)
    {
        p = static_cast<T>(1) - detail::norm_sf(z);
    }
    else
    {
        p = static_cast<T>(2) * detail::norm_sf(z < static_cast<T>(0) ? -z : z);
    }
    return {z, p, static_cast<T>(0)};
}

// Sign test (paired): zeros dropped, statistic = min(#+, #-), two-sided p = 2 Binom.cdf(k; m, 1/2) via betainc.
// statsmodels.stats.descriptivestats.sign_test.
template <Real T>
[[nodiscard]] TestResult<T> sign_test(crd::containers::ConstSpan<T> a, crd::containers::ConstSpan<T> b)
{
    crd::usize pos = 0;
    crd::usize neg = 0;
    for (crd::usize i = 0; i < a.size(); ++i)
    {
        const T d = a[i] - b[i];
        if (d > static_cast<T>(0))
        {
            ++pos;
        }
        else if (d < static_cast<T>(0))
        {
            ++neg;
        }
    }
    const T m = static_cast<T>(pos + neg);
    const T k = static_cast<T>((pos < neg) ? pos : neg);
    // Binom.cdf(k; m, 1/2) = I_{1/2}(m-k, k+1)
    T p = static_cast<T>(2) * special::betainc(m - k, k + static_cast<T>(1), static_cast<T>(0.5),
                                               special::lbeta(m - k, k + static_cast<T>(1)));
    if (p > static_cast<T>(1))
    {
        p = static_cast<T>(1);
    }
    return {k, p, static_cast<T>(0)};
}

// G-test (likelihood-ratio chi-square) of independence: G = 2 sum O ln(O/E), p from chi-square((r-1)(c-1)).
// scipy.stats.chi2_contingency(lambda_='log-likelihood').
template <Real T>
[[nodiscard]] TestResult<T> g_test(crd::containers::ConstSpan<T> observed, crd::usize rows, crd::usize cols,
                                   crd::memory::IAllocator* alloc)
{
    crd::containers::Array<T> rt(alloc);
    crd::containers::Array<T> ct(alloc);
    rt.resize(rows);
    ct.resize(cols);
    for (crd::usize r = 0; r < rows; ++r)
    {
        rt[r] = static_cast<T>(0);
    }
    for (crd::usize c = 0; c < cols; ++c)
    {
        ct[c] = static_cast<T>(0);
    }
    T grand = static_cast<T>(0);
    for (crd::usize r = 0; r < rows; ++r)
    {
        for (crd::usize c = 0; c < cols; ++c)
        {
            const T o = observed[r * cols + c];
            rt[r] += o;
            ct[c] += o;
            grand += o;
        }
    }
    T g = static_cast<T>(0);
    for (crd::usize r = 0; r < rows; ++r)
    {
        for (crd::usize c = 0; c < cols; ++c)
        {
            const T o = observed[r * cols + c];
            if (o > static_cast<T>(0))
            {
                g += o * crd::math::log(o / (rt[r] * ct[c] / grand));
            }
        }
    }
    g *= static_cast<T>(2);
    const T df = static_cast<T>(rows - 1) * static_cast<T>(cols - 1);
    return {g, detail::chi2_pvalue(g, df), df};
}

// Cramer's V effect size for an r x c table = sqrt(chi2 / (N * (min(r,c)-1))).
template <Real T>
[[nodiscard]] T cramers_v(crd::containers::ConstSpan<T> observed, crd::usize rows, crd::usize cols,
                          crd::memory::IAllocator* alloc)
{
    const T chi2 = chi2_contingency(observed, rows, cols, alloc, false).statistic;
    T grand = static_cast<T>(0);
    for (T o : observed)
    {
        grand += o;
    }
    const crd::usize mindim = ((rows < cols) ? rows : cols) - 1;
    return crd::math::sqrt(chi2 / (grand * static_cast<T>(mindim)));
}

// Bonferroni adjusted p-values: out[i] = min(1, m * p[i]). statsmodels multipletests('bonferroni').
template <Real T> void bonferroni(crd::containers::ConstSpan<T> p, crd::containers::Span<T> out)
{
    const T m = static_cast<T>(p.size());
    for (crd::usize i = 0; i < p.size(); ++i)
    {
        const T v = m * p[i];
        out[i] = (v > static_cast<T>(1)) ? static_cast<T>(1) : v;
    }
}

// Games-Howell pairwise (unequal variances): like Tukey but a per-pair Welch-Satterthwaite df and se =
// sqrt((s_i^2/n_i + s_j^2/n_j)/2); p from the studentized range. (pingouin.pairwise_gameshowell.)
template <Real T>
[[nodiscard]] TestResult<T> games_howell(crd::containers::ConstSpan<crd::containers::ConstSpan<T>> groups, crd::usize i,
                                         crd::usize j, crd::memory::IAllocator* alloc)
{
    const T k = static_cast<T>(groups.size());
    const T ni = static_cast<T>(groups[i].size());
    const T nj = static_cast<T>(groups[j].size());
    const T si = variance(groups[i], 1) / ni;
    const T sj = variance(groups[j], 1) / nj;
    const T diff = mean(groups[i]) - mean(groups[j]);
    const T se = crd::math::sqrt((si + sj) / static_cast<T>(2));
    const T q = (diff < static_cast<T>(0) ? -diff : diff) / se;
    const T dfw = (si + sj) * (si + sj) / (si * si / (ni - 1) + sj * sj / (nj - 1));
    return {diff, detail::studentized_range_sf(q, k, dfw, alloc), dfw};
}

// Mood's test for equal scale parameters (two-sample, rank-based, normal approximation; the no-tie variance —
// scipy applies a Mielke squared-rank tie correction when samples tie, not modeled here). scipy.stats.mood.
template <Real T>
[[nodiscard]] TestResult<T> mood_scale(crd::containers::ConstSpan<T> x, crd::containers::ConstSpan<T> y,
                                       crd::memory::IAllocator* alloc)
{
    const crd::usize nx = x.size();
    const crd::usize ny = y.size();
    crd::containers::Array<T> comb(alloc);
    comb.reserve(nx + ny);
    for (T v : x)
    {
        comb.push_back(v);
    }
    for (T v : y)
    {
        comb.push_back(v);
    }
    const auto ranks = rankdata(crd::containers::ConstSpan<T>{comb.data(), comb.size()}, alloc);
    const T nt = static_cast<T>(nx + ny);
    T stat = static_cast<T>(0);
    for (crd::usize i = 0; i < nx; ++i)
    {
        const T d = ranks[i] - (nt + static_cast<T>(1)) / static_cast<T>(2);
        stat += d * d;
    }
    const T et = static_cast<T>(nx) * (nt * nt - 1) / static_cast<T>(12);
    const T vart = static_cast<T>(nx) * static_cast<T>(ny) * (nt + 1) * (nt * nt - 4) / static_cast<T>(180);
    const T z = (stat - et) / crd::math::sqrt(vart);
    return {z, static_cast<T>(2) * detail::norm_sf(z < static_cast<T>(0) ? -z : z), static_cast<T>(0)};
}

// D'Agostino-Pearson K^2 omnibus normality test: K^2 = Z_skew^2 + Z_kurt^2 (the skew + Anscombe-Glynn kurtosis
// transforms), p from chi-square(2). scipy.stats.normaltest. (n >= 8.)
template <Real T> [[nodiscard]] TestResult<T> dagostino_k2(crd::containers::ConstSpan<T> x)
{
    const T n = static_cast<T>(x.size());
    const T b1 = skewness(x);                   // g1 (biased) — scipy.stats.skew default
    const T b2 = kurtosis(x) + static_cast<T>(3); // non-excess — scipy.stats.kurtosis(fisher=False)
    // skewness test (D'Agostino 1970)
    const T y = b1 * crd::math::sqrt((n + 1) * (n + 3) / (static_cast<T>(6) * (n - 2)));
    const T beta2 = static_cast<T>(3) * (n * n + 27 * n - 70) * (n + 1) * (n + 3) /
                    ((n - 2) * (n + 5) * (n + 7) * (n + 9));
    const T w2 = static_cast<T>(-1) + crd::math::sqrt(static_cast<T>(2) * (beta2 - 1));
    const T delta = static_cast<T>(1) / crd::math::sqrt(static_cast<T>(0.5) * crd::math::log(w2));
    const T alpha = crd::math::sqrt(static_cast<T>(2) / (w2 - 1));
    const T ya = y / alpha;
    const T zs = delta * crd::math::log(ya + crd::math::sqrt(ya * ya + 1)); // = delta * asinh(y/alpha)
    // kurtosis test (Anscombe-Glynn 1983)
    const T e = static_cast<T>(3) * (n - 1) / (n + 1);
    const T varb2 = static_cast<T>(24) * n * (n - 2) * (n - 3) / ((n + 1) * (n + 1) * (n + 3) * (n + 5));
    const T xk = (b2 - e) / crd::math::sqrt(varb2);
    const T sqrtb1 = static_cast<T>(6) * (n * n - 5 * n + 2) / ((n + 7) * (n + 9)) *
                     crd::math::sqrt(static_cast<T>(6) * (n + 3) * (n + 5) / (n * (n - 2) * (n - 3)));
    const T a = static_cast<T>(6) + static_cast<T>(8) / sqrtb1 *
                                        (static_cast<T>(2) / sqrtb1 +
                                         crd::math::sqrt(1 + static_cast<T>(4) / (sqrtb1 * sqrtb1)));
    const T t2 = crd::math::cbrt((1 - static_cast<T>(2) / a) /
                                 (1 + xk * crd::math::sqrt(static_cast<T>(2) / (a - 4))));
    const T zk = ((1 - static_cast<T>(2) / (static_cast<T>(9) * a)) - t2) /
                 crd::math::sqrt(static_cast<T>(2) / (static_cast<T>(9) * a));
    const T k2 = zs * zs + zk * zk;
    return {k2, detail::chi2_pvalue(k2, static_cast<T>(2)), static_cast<T>(2)};
}

// Cramér-von Mises one-sample GoF against a continuous CDF (callable T->T). W^2 statistic + asymptotic p via the
// limiting distribution (Bessel K_{1/4} series). scipy.stats.cramervonmises (which adds a small finite-n correction).
template <Real T, typename Cdf>
[[nodiscard]] TestResult<T> cramervonmises(crd::containers::ConstSpan<T> x, Cdf cdf, crd::memory::IAllocator* alloc)
{
    const crd::usize n = x.size();
    const T nt = static_cast<T>(n);
    crd::containers::Array<T> s(alloc);
    s.resize(n);
    for (crd::usize i = 0; i < n; ++i)
    {
        s[i] = x[i];
    }
    crd::containers::sort(s.data(), s.data() + n);
    T w2 = static_cast<T>(1) / (static_cast<T>(12) * nt);
    for (crd::usize i = 0; i < n; ++i)
    {
        const T diff = cdf(s[i]) - static_cast<T>(2 * (i + 1) - 1) / (static_cast<T>(2) * nt);
        w2 += diff * diff;
    }
    const auto cvm_cdf_inf = [&](T xx) {
        T tot = static_cast<T>(0);
        for (int k = 0; k < 100; ++k)
        {
            const T kt = static_cast<T>(k);
            const T u = crd::math::exp(special::lgamma(kt + static_cast<T>(0.5)) - special::lgamma(kt + 1)) /
                        (crd::math::pow(detail::kPi<T>, static_cast<T>(1.5)) * crd::math::sqrt(xx));
            const T yk = static_cast<T>(4) * kt + 1;
            const T q = yk * yk / (static_cast<T>(16) * xx);
            const T term = u * crd::math::sqrt(yk) * crd::math::exp(-q) * special::cyl_bessel_k(static_cast<T>(0.25), q);
            tot += term;
            if (crd::math::fabs(term) < static_cast<T>(1e-10))
            {
                break;
            }
        }
        return tot;
    };
    const T p = static_cast<T>(1) - cvm_cdf_inf(w2);
    return {w2, (p < static_cast<T>(0)) ? static_cast<T>(0) : ((p > static_cast<T>(1)) ? static_cast<T>(1) : p),
            static_cast<T>(0)};
}

// Lilliefors test (KS for normality with estimated mean/std): D statistic + Dallal-Wilkinson (1986) p approximation,
// clipped to [0.001, 0.99]. statsmodels.stats.diagnostic.lilliefors (pvalmethod='approx').
template <Real T>
[[nodiscard]] TestResult<T> lilliefors(crd::containers::ConstSpan<T> x, crd::memory::IAllocator* alloc)
{
    const crd::usize n = x.size();
    const T nt = static_cast<T>(n);
    const T m = mean(x);
    const T sd = crd::math::sqrt(variance(x, 1));
    crd::containers::Array<T> z(alloc);
    z.resize(n);
    for (crd::usize i = 0; i < n; ++i)
    {
        z[i] = (x[i] - m) / sd;
    }
    crd::containers::sort(z.data(), z.data() + n);
    T d = static_cast<T>(0);
    for (crd::usize i = 0; i < n; ++i)
    {
        const T f = detail::norm_sf(-z[i]); // Phi(z_i)
        const T dp = static_cast<T>(i + 1) / nt - f;
        const T dm = f - static_cast<T>(i) / nt;
        d = (dp > d) ? dp : d;
        d = (dm > d) ? dm : d;
    }
    T nn = nt;
    T dd = d;
    if (nn > static_cast<T>(100))
    {
        dd *= crd::math::pow(nn / static_cast<T>(100), static_cast<T>(0.49));
        nn = static_cast<T>(100);
    }
    T p = crd::math::exp(static_cast<T>(-7.01256) * dd * dd * (nn + static_cast<T>(2.78019)) +
                         static_cast<T>(2.99587) * dd * crd::math::sqrt(nn + static_cast<T>(2.78019)) -
                         static_cast<T>(0.122119) + static_cast<T>(0.974598) / crd::math::sqrt(nn) +
                         static_cast<T>(1.67997) / nn);
    p = (p > static_cast<T>(0.99)) ? static_cast<T>(0.99) : ((p < static_cast<T>(0.001)) ? static_cast<T>(0.001) : p);
    return {d, p, static_cast<T>(0)};
}

namespace detail
{
// Equicorrelation-(rho) multivariate-t survival used by Dunnett: P(max_j |T_j| > d) over m treatments sharing a common
// control, nu df. The equicorrelated MVt factors through a common factor into the SAME Gauss-Hermite × Gauss-Laguerre
// integral as the studentized range (reuses hesap-quadrature).
template <Real T> [[nodiscard]] T dunnett_sf(T d, int m, T nu, T rho, crd::memory::IAllocator* alloc)
{
    constexpr int nh = 64;
    constexpr int nl = 64;
    crd::containers::Array<T> hz(alloc);
    crd::containers::Array<T> hw(alloc);
    crd::containers::Array<T> lz(alloc);
    crd::containers::Array<T> lw(alloc);
    hz.resize(nh);
    hw.resize(nh);
    lz.resize(nl);
    lw.resize(nl);
    crd::hesap::quadrature::gauss_hermite<T>(alloc, nh, hz.data(), hw.data());
    crd::hesap::quadrature::gauss_laguerre<T>(alloc, nl, nu / static_cast<T>(2) - static_cast<T>(1), lz.data(),
                                              lw.data());
    const T sqrt2 = crd::math::sqrt(static_cast<T>(2));
    const T inv_sqrtpi = static_cast<T>(1) / crd::math::sqrt(kPi<T>);
    const T sr = crd::math::sqrt(rho);
    const T s1mr = crd::math::sqrt(static_cast<T>(1) - rho);
    const auto inner = [&](T a) { // P(all |Z_j| <= a) for equicorrelated Z, a = d*s
        T sum = static_cast<T>(0);
        for (int i = 0; i < nh; ++i)
        {
            const T u = hz[i] * sqrt2;
            const T hi = norm_sf(-(a - sr * u) / s1mr);  // Phi((a - sqrt(rho) u)/sqrt(1-rho))
            const T lo = norm_sf(-(-a - sr * u) / s1mr); // Phi((-a - sqrt(rho) u)/sqrt(1-rho))
            sum += hw[i] * crd::math::pow(hi - lo, static_cast<T>(m));
        }
        return inv_sqrtpi * sum;
    };
    const T gnu = special::gamma(nu / static_cast<T>(2));
    T f = static_cast<T>(0);
    for (int j = 0; j < nl; ++j)
    {
        f += lw[j] * inner(d * crd::math::sqrt(static_cast<T>(2) * lz[j] / nu));
    }
    f /= gnu;
    const T sf = static_cast<T>(1) - f;
    return (sf < static_cast<T>(0)) ? static_cast<T>(0) : ((sf > static_cast<T>(1)) ? static_cast<T>(1) : sf);
}
} // namespace detail

// Scheffé post-hoc pairwise contrast: F = (mean_i - mean_j)^2 / (MSE (1/n_i + 1/n_j)) / (k-1), p from F(k-1, N-k).
template <Real T>
[[nodiscard]] TestResult<T> scheffe(crd::containers::ConstSpan<crd::containers::ConstSpan<T>> groups, crd::usize i,
                                    crd::usize j)
{
    const crd::usize k = groups.size();
    crd::usize total = 0;
    T ssw = static_cast<T>(0);
    for (const auto& g : groups)
    {
        total += g.size();
        const T mm = mean(g);
        for (T x : g)
        {
            const T e = x - mm;
            ssw += e * e;
        }
    }
    const T dfe = static_cast<T>(total - k);
    const T mse = ssw / dfe;
    const T df1 = static_cast<T>(k - 1);
    const T diff = mean(groups[i]) - mean(groups[j]);
    const T fstat = diff * diff /
                    (mse * (static_cast<T>(1) / static_cast<T>(groups[i].size()) +
                            static_cast<T>(1) / static_cast<T>(groups[j].size()))) /
                    df1;
    return {fstat, detail::f_pvalue(fstat, df1, dfe), dfe};
}

// Dunnett's test (treatments vs one control), balanced / equal-treatment-size design (equicorrelation rho). statistic
// = (mean_treat - mean_control)/sqrt(MSE(1/n_t + 1/n_c)); two-sided p from the m-variate equicorrelated t.
// scipy.stats.dunnett. (Unequal treatment sizes => unequal rho => full MVt, not modeled here.)
template <Real T>
[[nodiscard]] TestResult<T> dunnett(crd::containers::ConstSpan<crd::containers::ConstSpan<T>> groups, crd::usize control,
                                    crd::usize treatment, crd::memory::IAllocator* alloc)
{
    const crd::usize k = groups.size();
    crd::usize total = 0;
    T ssw = static_cast<T>(0);
    for (const auto& g : groups)
    {
        total += g.size();
        const T mm = mean(g);
        for (T x : g)
        {
            const T e = x - mm;
            ssw += e * e;
        }
    }
    const T dfe = static_cast<T>(total - k);
    const T mse = ssw / dfe;
    const T nt = static_cast<T>(groups[treatment].size());
    const T nc = static_cast<T>(groups[control].size());
    const T se = crd::math::sqrt(mse * (static_cast<T>(1) / nt + static_cast<T>(1) / nc));
    const T t = (mean(groups[treatment]) - mean(groups[control])) / se;
    const T rho = nt / (nt + nc); // = 1/2 for balanced
    const T p = detail::dunnett_sf((t < static_cast<T>(0) ? -t : t), static_cast<int>(k - 1), dfe, rho, alloc);
    return {t, p, dfe};
}

template <Real T> struct TwoWayAnovaResult
{
    T f_a;
    T p_a;
    T f_b;
    T p_b;
    T f_ab;
    T p_ab;
    T df_a;
    T df_b;
    T df_ab;
    T df_e;
};

// Balanced two-way ANOVA. y laid out factor-A-major, factor-B-minor, replicates innermost: y[(ia*nb + ib)*reps + r].
// statsmodels anova_lm (balanced => type I/II/III agree).
template <Real T>
[[nodiscard]] TwoWayAnovaResult<T> anova_two_way(crd::containers::ConstSpan<T> y, crd::usize na, crd::usize nb,
                                                 crd::usize reps, crd::memory::IAllocator* alloc)
{
    const T total = static_cast<T>(na * nb * reps);
    T grand = static_cast<T>(0);
    for (T v : y)
    {
        grand += v;
    }
    grand /= total;
    crd::containers::Array<T> rowmean(alloc);
    crd::containers::Array<T> colmean(alloc);
    rowmean.resize(na);
    colmean.resize(nb);
    for (crd::usize ia = 0; ia < na; ++ia)
    {
        rowmean[ia] = static_cast<T>(0);
    }
    for (crd::usize ib = 0; ib < nb; ++ib)
    {
        colmean[ib] = static_cast<T>(0);
    }
    T ss_total = static_cast<T>(0);
    T ss_cells = static_cast<T>(0);
    for (crd::usize ia = 0; ia < na; ++ia)
    {
        for (crd::usize ib = 0; ib < nb; ++ib)
        {
            T cm = static_cast<T>(0);
            for (crd::usize r = 0; r < reps; ++r)
            {
                const T v = y[(ia * nb + ib) * reps + r];
                cm += v;
                const T e = v - grand;
                ss_total += e * e;
            }
            cm /= static_cast<T>(reps);
            rowmean[ia] += cm;
            colmean[ib] += cm;
            const T ce = cm - grand;
            ss_cells += static_cast<T>(reps) * ce * ce;
        }
    }
    T ss_a = static_cast<T>(0);
    for (crd::usize ia = 0; ia < na; ++ia)
    {
        const T rm = rowmean[ia] / static_cast<T>(nb) - grand;
        ss_a += static_cast<T>(nb * reps) * rm * rm;
    }
    T ss_b = static_cast<T>(0);
    for (crd::usize ib = 0; ib < nb; ++ib)
    {
        const T cmn = colmean[ib] / static_cast<T>(na) - grand;
        ss_b += static_cast<T>(na * reps) * cmn * cmn;
    }
    const T ss_ab = ss_cells - ss_a - ss_b;
    const T ss_e = ss_total - ss_cells;
    const T df_a = static_cast<T>(na - 1);
    const T df_b = static_cast<T>(nb - 1);
    const T df_ab = static_cast<T>((na - 1) * (nb - 1));
    const T df_e = static_cast<T>(na * nb * (reps - 1));
    const T mse = ss_e / df_e;
    const T fa = (ss_a / df_a) / mse;
    const T fb = (ss_b / df_b) / mse;
    const T fab = (ss_ab / df_ab) / mse;
    return {fa,
            detail::f_pvalue(fa, df_a, df_e),
            fb,
            detail::f_pvalue(fb, df_b, df_e),
            fab,
            detail::f_pvalue(fab, df_ab, df_e),
            df_a,
            df_b,
            df_ab,
            df_e};
}

template <Real T> struct RepeatedAnovaResult
{
    T statistic;
    T pvalue;
    T df_num;
    T df_den;
};

// One-way repeated-measures ANOVA. y laid out subject-major: y[s*conditions + c]. statsmodels AnovaRM.
template <Real T>
[[nodiscard]] RepeatedAnovaResult<T> anova_rm(crd::containers::ConstSpan<T> y, crd::usize subjects,
                                              crd::usize conditions, crd::memory::IAllocator* alloc)
{
    const T total = static_cast<T>(subjects * conditions);
    T grand = static_cast<T>(0);
    for (T v : y)
    {
        grand += v;
    }
    grand /= total;
    crd::containers::Array<T> condsum(alloc);
    condsum.resize(conditions);
    for (crd::usize c = 0; c < conditions; ++c)
    {
        condsum[c] = static_cast<T>(0);
    }
    T ss_total = static_cast<T>(0);
    T ss_subj = static_cast<T>(0);
    for (crd::usize s = 0; s < subjects; ++s)
    {
        T subjsum = static_cast<T>(0);
        for (crd::usize c = 0; c < conditions; ++c)
        {
            const T v = y[s * conditions + c];
            condsum[c] += v;
            subjsum += v;
            const T e = v - grand;
            ss_total += e * e;
        }
        const T sm = subjsum / static_cast<T>(conditions) - grand;
        ss_subj += static_cast<T>(conditions) * sm * sm;
    }
    T ss_treat = static_cast<T>(0);
    for (crd::usize c = 0; c < conditions; ++c)
    {
        const T cm = condsum[c] / static_cast<T>(subjects) - grand;
        ss_treat += static_cast<T>(subjects) * cm * cm;
    }
    const T ss_error = ss_total - ss_treat - ss_subj;
    const T df_treat = static_cast<T>(conditions - 1);
    const T df_error = static_cast<T>((conditions - 1) * (subjects - 1));
    const T f = (ss_treat / df_treat) / (ss_error / df_error);
    return {f, detail::f_pvalue(f, df_treat, df_error), df_treat, df_error};
}

} // namespace crd::hesap::stats
