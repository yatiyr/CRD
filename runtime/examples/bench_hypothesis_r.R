# v12-n parametric hypothesis-test peer bench: base-R per-call cost (ns/call), same n=100/group as the Cerid bench.
# (base R has no Levene; that needs the `car` package — omitted here.)
n <- 100; i <- 0:(n - 1)
A <- 2.0 + 0.7 * sin(0.30 * i); B <- 1.5 + 0.5 * sin(0.21 * i + 1.0); C <- 3.0 + 0.9 * sin(0.13 * i + 2.0)
N <- 20000
bench <- function(nm, f) {
  for (k in 1:200) f()
  t <- system.time(for (k in 1:N) f())[["elapsed"]]
  cat(sprintf("%-12s %9.1f ns/call\n", nm, t / N * 1e9))
}
bench("ttest_ind",   function() t.test(A, B, var.equal = TRUE))
bench("ttest_welch", function() t.test(A, B, var.equal = FALSE))
bench("ttest_rel",   function() t.test(A, B, paired = TRUE))
bench("f_oneway",    function() oneway.test(c(A, B, C) ~ factor(rep(1:3, each = n)), var.equal = TRUE))
bench("bartlett",    function() bartlett.test(list(A, B, C)))
bench("mannwhitneyu", function() wilcox.test(A, B))
bench("wilcoxon",    function() wilcox.test(A, B, paired = TRUE))
bench("kruskal",     function() kruskal.test(list(A, B, C)))
bench("friedman",    function() friedman.test(cbind(A, B, C)))
