# v12-o resampling peer bench: R boot::boot wall-time for a percentile CI of the mean (n=100, B resamples).
suppressMessages(library(boot))
n <- 100
data <- 2.0 + 0.7 * sin(0.30 * (0:(n - 1)))
B <- 100000
f <- function(d, i) mean(d[i])
invisible(boot(data, f, R = B)) # warmup
reps <- 3
t <- system.time(for (i in 1:reps) boot(data, f, R = B))[3] / reps
b <- boot(data, f, R = B)
ci <- boot.ci(b, type = "perc")$percent[4:5]
cat(sprintf("R boot::boot            %8.3f ms  CI=[%.6f, %.6f]\n", t * 1000, ci[1], ci[2]))
