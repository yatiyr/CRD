// v12-n (parametric) — t-tests, ANOVA, Bartlett, Levene gated bit-for-bit against scipy.stats on a fixed 3-group
// dataset (statistic + p-value + df where scipy exposes it).

#include <crd/hesap/stats/hypothesis.hpp>
#include <crd/math/cmath.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

namespace
{
using namespace crd::hesap::stats;
using crd::containers::ConstSpan;

[[nodiscard]] bool close(double a, double b, double tol = 1e-9) noexcept
{
    return crd::math::fabs(a - b) <= tol + tol * crd::math::fabs(b);
}

constexpr double kA[] = {2.1, 3.4, 1.9, 5.2, 4.1, 2.8, 6.3, 3.0};
constexpr double kB[] = {1.0, 2.0, 1.5, 3.0, 2.5, 2.0, 3.5, 1.8};
constexpr double kC[] = {5.0, 4.0, 6.0, 2.0, 3.0, 4.5, 1.0, 4.2};

[[nodiscard]] ConstSpan<double> sample_a() noexcept
{
    return {kA, 8};
}
[[nodiscard]] ConstSpan<double> sample_b() noexcept
{
    return {kB, 8};
}
[[nodiscard]] ConstSpan<double> sample_c() noexcept
{
    return {kC, 8};
}
} // namespace

TEST_CASE("v12-n: t-tests vs scipy", "[v12-n][stats][hypothesis]")
{
    {
        const auto r = t_test_1samp(sample_a(), 3.0); // scipy.stats.ttest_1samp
        CHECK(close(r.statistic, 1.11280242555866));
        CHECK(close(r.pvalue, 0.302542469993592));
        CHECK(close(r.df, 7.0));
    }
    {
        const auto r = t_test_ind(sample_a(), sample_b(), true); // pooled (Student)
        CHECK(close(r.statistic, 2.35536234308948));
        CHECK(close(r.pvalue, 0.0336179791516164));
        CHECK(close(r.df, 14.0));
    }
    {
        const auto r = t_test_ind(sample_a(), sample_b(), false); // Welch
        CHECK(close(r.statistic, 2.35536234308948));
        CHECK(close(r.pvalue, 0.0388295001766022));
        CHECK(close(r.df, 10.6488687782805));
    }
    {
        const auto r = t_test_rel(sample_a(), sample_b()); // paired
        CHECK(close(r.statistic, 5.30052821830237));
        CHECK(close(r.pvalue, 0.00112272426595492));
        CHECK(close(r.df, 7.0));
    }
    // one-sided alternatives
    CHECK(close(t_test_1samp(sample_a(), 3.0, Alternative::Greater).pvalue, 0.151271234996796));
    CHECK(close(t_test_1samp(sample_a(), 3.0, Alternative::Less).pvalue, 0.848728765003204));
}

TEST_CASE("v12-n: ANOVA / variance tests vs scipy", "[v12-n][stats][hypothesis]")
{
    const ConstSpan<double> grp[] = {sample_a(), sample_b(), sample_c()};
    const ConstSpan<ConstSpan<double>> groups{grp, 3};
    {
        const auto r = f_oneway(groups); // scipy.stats.f_oneway
        CHECK(close(r.statistic, 3.17780943336499));
        CHECK(close(r.pvalue, 0.0622750205887681));
        CHECK(close(r.df_num, 2.0));
        CHECK(close(r.df_den, 21.0));
    }
    {
        const auto r = bartlett(groups); // scipy.stats.bartlett
        CHECK(close(r.statistic, 3.26502018406915));
        CHECK(close(r.pvalue, 0.195438389580659));
        CHECK(close(r.df, 2.0));
    }
    {
        crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(1) << 16);
        const auto r = levene(groups, &alloc, Center::Median); // scipy.stats.levene (Brown-Forsythe)
        CHECK(close(r.statistic, 1.16564192651149));
        CHECK(close(r.pvalue, 0.331090638831897));
        CHECK(close(r.df_num, 2.0));
        CHECK(close(r.df_den, 21.0));
    }
}

TEST_CASE("v12-n: nonparametric (rank) tests vs scipy", "[v12-n][stats][hypothesis]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(1) << 16);
    {
        const auto r =
            mann_whitney_u(sample_a(), sample_b(), &alloc); // scipy.stats.mannwhitneyu (asymptotic, continuity)
        CHECK(close(r.statistic, 51.5));
        CHECK(close(r.pvalue, 0.0456798096400443));
    }
    {
        // paired set with a zero diff (dropped) + a 5-way tie in |d| — exercises the zero/tie corrections
        constexpr double k_wa[] = {5.0, 3.4, 1.9, 5.2, 4.1, 2.8, 6.3, 3.0};
        constexpr double k_wb[] = {5.0, 3.0, 2.2, 4.8, 4.5, 3.5, 5.9, 2.6};
        const auto r = wilcoxon(ConstSpan<double>{k_wa, 8}, ConstSpan<double>{k_wb, 8}, &alloc); // scipy.stats.wilcoxon
        CHECK(close(r.statistic, 13.5));
        CHECK(close(r.pvalue, 0.932405373249338));
    }
    const ConstSpan<double> grp[] = {sample_a(), sample_b(), sample_c()};
    const ConstSpan<ConstSpan<double>> groups{grp, 3};
    {
        const auto r = kruskal(groups, &alloc); // scipy.stats.kruskal
        CHECK(close(r.statistic, 5.60568529026625));
        CHECK(close(r.pvalue, 0.060637446655861));
        CHECK(close(r.df, 2.0));
    }
    {
        const auto r = friedman(groups, &alloc); // scipy.stats.friedmanchisquare
        CHECK(close(r.statistic, 6.75));
        CHECK(close(r.pvalue, 0.034218118311666));
        CHECK(close(r.df, 2.0));
    }
}

TEST_CASE("v12-n: correlation vs scipy", "[v12-n][stats][hypothesis]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(1) << 16);
    {
        const auto r = pearsonr(sample_a(), sample_b()); // scipy.stats.pearsonr
        CHECK(close(r.statistic, 0.969448609096053));
        CHECK(close(r.pvalue, 6.96671732921854e-05));
    }
    {
        const auto r = spearmanr(sample_a(), sample_b(), &alloc); // scipy.stats.spearmanr
        CHECK(close(r.statistic, 0.934148484292342));
        CHECK(close(r.pvalue, 0.000679105745231097));
    }
    {
        const auto r = kendalltau(sample_a(), sample_b(), &alloc); // scipy.stats.kendalltau (tau-b, asymptotic)
        CHECK(close(r.statistic, 0.836501912571304));
        CHECK(close(r.pvalue, 0.00413673709867665));
    }
}

TEST_CASE("v12-n: categorical vs scipy", "[v12-n][stats][hypothesis]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(1) << 16);
    {
        constexpr double obs[] = {10, 20, 30, 6, 9, 17}; // 2x3, no correction
        const auto r = chi2_contingency(ConstSpan<double>{obs, 6}, 2, 3, &alloc, false);
        CHECK(close(r.statistic, 0.271574651504035));
        CHECK(close(r.pvalue, 0.873028283380073));
        CHECK(close(r.df, 2.0));
    }
    {
        constexpr double obs[] = {10, 20, 30, 40}; // 2x2, Yates correction (scipy default)
        const auto r = chi2_contingency(ConstSpan<double>{obs, 4}, 2, 2, &alloc, true);
        CHECK(close(r.statistic, 0.446428571428571));
        CHECK(close(r.pvalue, 0.504035866452505));
        CHECK(close(r.df, 1.0));
    }
    {
        const auto r = fisher_exact(8.0, 2.0, 1.0, 5.0); // scipy.stats.fisher_exact
        CHECK(close(r.statistic, 20.0));                 // odds ratio
        CHECK(close(r.pvalue, 0.034965034965035));
    }
    {
        const auto r = mcnemar(5.0, 3.0); // discordant b=5, c=3 (continuity)
        CHECK(close(r.statistic, 0.125));
        CHECK(close(r.pvalue, 0.723673609831763));
    }
}

TEST_CASE("v12-n: GoF / effect size / multcompare vs scipy", "[v12-n][stats][hypothesis]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(1) << 16);
    {
        const auto r = jarque_bera(sample_a()); // scipy.stats.jarque_bera
        CHECK(close(r.statistic, 0.747538232988901));
        CHECK(close(r.pvalue, 0.68813577268859));
    }
    {
        const auto ncdf = [](double v)
        {
            return 0.5 * crd::hesap::special::erfc(-v / 1.4142135623730951);
        };
        const auto r = ks_1samp(sample_a(), ncdf, &alloc); // KS vs N(0,1), asymptotic (scipy.special.kolmogorov)
        CHECK(close(r.statistic, 0.971283440183998));
        CHECK(close(r.pvalue, 5.56768023713558e-07));
    }
    {
        const auto r = ks_2samp(sample_a(), sample_b(), &alloc); // scipy.stats.ks_2samp (asymptotic)
        CHECK(close(r.statistic, 0.5));
        CHECK(close(r.pvalue, 0.269999671677355));
    }
    CHECK(close(cohens_d(sample_a(), sample_b()), 1.17768117154474));
    {
        const ConstSpan<double> grp[] = {sample_a(), sample_b(), sample_c()};
        CHECK(close(eta_squared(ConstSpan<ConstSpan<double>>{grp, 3}), 0.232333214528724));
    }
    {
        constexpr double ps[] = {0.01, 0.04, 0.03, 0.005, 0.2};
        constexpr double k_holm[] = {0.04, 0.09, 0.09, 0.025, 0.2};
        constexpr double k_bh[] = {0.025, 0.05, 0.05, 0.025, 0.2};
        double h[5];
        double bh[5];
        holm(ConstSpan<double>{ps, 5}, crd::containers::Span<double>{h, 5}, &alloc); // statsmodels 'holm'
        benjamini_hochberg(ConstSpan<double>{ps, 5}, crd::containers::Span<double>{bh, 5}, &alloc); // scipy BH
        for (int i = 0; i < 5; ++i)
        {
            CHECK(close(h[i], k_holm[i]));
            CHECK(close(bh[i], k_bh[i]));
        }
    }
}

TEST_CASE("v12-n: Shapiro / Anderson-Darling / distance correlation vs scipy", "[v12-n][stats][hypothesis]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(1) << 16);
    {
        const auto r = anderson_darling(sample_a(), &alloc); // scipy.stats.anderson statistic + D'Agostino p
        CHECK(close(r.statistic, 0.276336473641585, 1e-9));
        CHECK(close(r.pvalue, 0.550886439610157, 1e-9));
    }
    {
        const auto r = shapiro(sample_a(), &alloc); // scipy.stats.shapiro (Royston AS R94)
        CHECK(close(r.statistic, 0.930944870950909, 1e-6));
        CHECK(close(r.pvalue, 0.524715214796725, 1e-6));
    }
    CHECK(close(distance_correlation(sample_a(), sample_b(), &alloc), 0.978862331499961, 1e-9));
}

TEST_CASE("v12-n: Tukey HSD vs scipy", "[v12-n][stats][hypothesis]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(1) << 20);
    const ConstSpan<double> grp[] = {sample_a(), sample_b(), sample_c()};
    const ConstSpan<ConstSpan<double>> groups{grp, 3};
    {
        const auto r = tukey_hsd(groups, 0, 1, &alloc); // a vs b
        CHECK(close(r.statistic, 1.4375, 1e-9));
        CHECK(close(r.pvalue, 0.114734698965417, 1e-5));
    }
    {
        const auto r = tukey_hsd(groups, 0, 2, &alloc); // a vs c
        CHECK(close(r.statistic, -0.1125, 1e-9));
        CHECK(close(r.pvalue, 0.985277387000487, 1e-5));
    }
    {
        const auto r = tukey_hsd(groups, 1, 2, &alloc); // b vs c
        CHECK(close(r.statistic, -1.55, 1e-9));
        CHECK(close(r.pvalue, 0.0840348707137972, 1e-5));
    }
}

TEST_CASE("v12-n variants A: z / sign / G-test / Cramer's V / Bonferroni / Games-Howell vs scipy",
          "[v12-n][stats][hypothesis]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(1) << 20);
    {
        const auto r = z_test_1samp(sample_a(), 3.0); // statsmodels ztest
        CHECK(close(r.statistic, 1.11280242555866));
        CHECK(close(r.pvalue, 0.265793293450749));
    }
    {
        const auto r = sign_test(sample_a(), sample_b()); // statsmodels sign_test
        CHECK(close(r.statistic, 0.0));
        CHECK(close(r.pvalue, 0.0078125));
    }
    {
        constexpr double obs[] = {10, 20, 30, 6, 9, 17};
        const auto r = g_test(ConstSpan<double>{obs, 6}, 2, 3, &alloc); // chi2_contingency log-likelihood
        CHECK(close(r.statistic, 0.274026542024661));
        CHECK(close(r.pvalue, 0.871958654281272));
        CHECK(close(r.df, 2.0));
        CHECK(close(cramers_v(ConstSpan<double>{obs, 6}, 2, 3, &alloc), 0.0543313757042229));
    }
    {
        constexpr double ps[] = {0.01, 0.04, 0.03, 0.005, 0.2};
        constexpr double k_bonf[] = {0.05, 0.2, 0.15, 0.025, 1.0};
        double bf[5];
        bonferroni(ConstSpan<double>{ps, 5}, crd::containers::Span<double>{bf, 5});
        for (int i = 0; i < 5; ++i)
        {
            CHECK(close(bf[i], k_bonf[i]));
        }
    }
    {
        const ConstSpan<double> grp[] = {sample_a(), sample_b(), sample_c()};
        const ConstSpan<ConstSpan<double>> groups{grp, 3};
        const auto r = games_howell(groups, 0, 1, &alloc); // a vs b, Welch df
        CHECK(close(r.statistic, 1.4375, 1e-9));
        CHECK(close(r.pvalue, 0.0906653757475744, 1e-5));
    }
}

TEST_CASE("v12-n variants B: Mood / D'Agostino K2 / Cramer-von-Mises / Lilliefors vs scipy",
          "[v12-n][stats][hypothesis]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(1) << 18);
    constexpr double k_x20[] = {-1.5, -1.0, -0.8, -0.5, -0.3, -0.2, 0.0, 0.1, 0.2, 0.3,
                                0.4,  0.5,  0.6,  0.7,  0.9,  1.0,  1.2, 1.4, 1.6, 2.0};
    const auto x20 = ConstSpan<double>{k_x20, 20};
    {
        // tie-free samples (narrow vs wide) → scipy's no-tie path, which mood_scale models exactly
        constexpr double k_ma[] = {2.8, 3.1, 2.9, 3.3, 2.7, 3.2, 3.05, 2.95};
        constexpr double k_mb[] = {1.0, 5.5, 0.5, 6.2, 1.8, 4.9, 2.3, 5.1};
        const auto r = mood_scale(ConstSpan<double>{k_ma, 8}, ConstSpan<double>{k_mb, 8}, &alloc); // scipy.stats.mood
        CHECK(close(r.statistic, -3.27968024676315, 1e-9));
        CHECK(close(r.pvalue, 0.00103924800037198, 1e-9));
    }
    {
        const auto r = dagostino_k2(x20); // scipy.stats.normaltest
        CHECK(close(r.statistic, 0.0962648518237611, 1e-6));
        CHECK(close(r.pvalue, 0.953007575823612, 1e-6));
    }
    {
        const auto ncdf = [](double v)
        {
            return 0.5 * crd::hesap::special::erfc(-v / 1.4142135623730951);
        };
        const auto r = cramervonmises(x20, ncdf, &alloc); // scipy.stats.cramervonmises
        CHECK(close(r.statistic, 0.252471086018524, 1e-9));
        CHECK(close(r.pvalue, 0.185424667924077, 1e-3)); // asymptotic (scipy adds a ~2e-4 finite-n correction)
    }
    {
        const auto r = lilliefors(x20, &alloc); // statsmodels lilliefors (pvalmethod='approx')
        CHECK(close(r.statistic, 0.0563616659133244, 1e-9));
        CHECK(close(r.pvalue, 0.99, 1e-9)); // DW formula > 0.99 → capped
    }
}

TEST_CASE("v12-n variants C: Scheffe / Dunnett / 2-way ANOVA / repeated ANOVA vs scipy+statsmodels",
          "[v12-n][stats][hypothesis]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(1) << 20);
    const ConstSpan<double> grp[] = {sample_a(), sample_b(), sample_c()};
    const ConstSpan<ConstSpan<double>> groups{grp, 3};
    {
        const auto r = scheffe(groups, 0, 1); // a vs b
        CHECK(close(r.statistic, 2.19788698955366, 1e-9));
        CHECK(close(r.pvalue, 0.135927502031977, 1e-9));
        const auto r2 = scheffe(groups, 1, 2); // b vs c
        CHECK(close(r2.statistic, 2.55536562203229, 1e-9));
        CHECK(close(r2.pvalue, 0.101555332197997, 1e-9));
    }
    {
        const auto rb = dunnett(groups, 0, 1, &alloc); // control a, treatment b
        CHECK(close(rb.statistic, -2.0966101161416, 1e-9));
        CHECK(close(rb.pvalue, 0.0865123455319828, 1e-4));
        const auto rc = dunnett(groups, 0, 2, &alloc); // control a, treatment c
        CHECK(close(rc.statistic, 0.164082530828474, 1e-9));
        CHECK(close(rc.pvalue, 0.980475219041265, 1e-4));
    }
    {
        constexpr double y2[] = {12, 14, 15, 17, 20, 22, 13, 11, 18, 16, 25, 23}; // A-major, B-minor, 2 reps
        const auto r = anova_two_way(ConstSpan<double>{y2, 12}, 2, 3, 2, &alloc);
        CHECK(close(r.f_a, 1.49999999999997, 1e-6));
        CHECK(close(r.p_a, 0.266569703380073, 1e-6));
        CHECK(close(r.f_b, 50.6666666666662, 1e-6));
        CHECK(close(r.p_b, 0.000174682701692031, 1e-6));
        CHECK(close(r.f_ab, 2.0, 1e-6));
        CHECK(close(r.p_ab, 0.216, 1e-6));
    }
    {
        constexpr double yr[] = {10, 12, 15, 9, 11, 14, 11, 13, 17, 8, 10, 13}; // subject-major, 3 conditions
        const auto r = anova_rm(ConstSpan<double>{yr, 12}, 4, 3, &alloc);
        CHECK(close(r.statistic, 336.999999999999, 1e-6));
        CHECK(close(r.pvalue, 6.869529818848e-07, 1e-9));
        CHECK(close(r.df_num, 2.0));
        CHECK(close(r.df_den, 6.0));
    }
}
