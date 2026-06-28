# v12-n parametric hypothesis-test peer bench: scipy.stats per-call cost (ns/call), same n=100/group as the Cerid bench.
import numpy as np
import timeit
from scipy import stats

n = 100
r = np.arange(n)
A = 2.0 + 0.7 * np.sin(0.30 * r)
B = 1.5 + 0.5 * np.sin(0.21 * r + 1.0)
C = 3.0 + 0.9 * np.sin(0.13 * r + 2.0)

N = 20000


def bench(name, fn):
    for _ in range(200):
        fn()
    t = timeit.timeit(fn, number=N)
    print("%-14s %8.1f ns/call" % (name, t / N * 1e9))


bench("ttest_ind", lambda: stats.ttest_ind(A, B, equal_var=True))
bench("ttest_welch", lambda: stats.ttest_ind(A, B, equal_var=False))
bench("ttest_rel", lambda: stats.ttest_rel(A, B))
bench("f_oneway", lambda: stats.f_oneway(A, B, C))
bench("bartlett", lambda: stats.bartlett(A, B, C))
bench("levene", lambda: stats.levene(A, B, C, center="median"))
bench("mannwhitneyu", lambda: stats.mannwhitneyu(A, B, method="asymptotic"))
bench("wilcoxon", lambda: stats.wilcoxon(A, B, method="approx"))
bench("kruskal", lambda: stats.kruskal(A, B, C))
bench("friedman", lambda: stats.friedmanchisquare(A, B, C))
bench("pearsonr", lambda: stats.pearsonr(A, B))
bench("spearmanr", lambda: stats.spearmanr(A, B))
bench("kendalltau", lambda: stats.kendalltau(A, B))
bench("jarque_bera", lambda: stats.jarque_bera(A))
bench("ks_2samp", lambda: stats.ks_2samp(A, B))
bench("shapiro", lambda: stats.shapiro(A))
bench("anderson", lambda: stats.anderson(A, "norm"))
from statsmodels.stats.weightstats import ztest as _ztest
from statsmodels.stats.diagnostic import lilliefors as _lillie
bench("ztest", lambda: _ztest(A, value=2.0))
bench("mood", lambda: stats.mood(A, B))
bench("dagostino", lambda: stats.normaltest(A))
bench("cramervonmises", lambda: stats.cramervonmises(A, "norm"))
bench("lilliefors", lambda: _lillie(A, dist="norm", pvalmethod="approx"))
