% v12-o resampling peer bench: MATLAB bootci wall-time for a percentile CI of the mean (n=100, B resamples).
maxNumCompThreads(1);
n = 100;
data = 2.0 + 0.7 * sin(0.30 * (0:n-1))';
B = 100000;
f = @() bootci(B, {@mean, data}, 'type', 'per');
ci = f();                 % warmup
t = timeit(f);
fprintf('MATLAB bootci           %8.3f ms  CI=[%.6f, %.6f]\n', t*1000, ci(1), ci(2));
