function bench_measure_matlab()
% MATLAB baseline for the v11-s measurement bench: thd + snr (Signal Processing Toolbox), N=1M.
maxNumCompThreads(1);
fprintf('MATLAB threads = %d\n', maxNumCompThreads);
n = 1000000; reps = 20;
i = (0:n-1)';
x = sin(0.05*i) + 0.3*(2*rand(n,1)-1);
thd(x); % warm
tic; v = 0;
for r = 1:reps
    v = v + thd(x);
end
t = toc;
fprintf('MATLAB thd N=%d  %.4f ms/call (thd=%.3f dB)\n', n, 1e3*t/reps, v/reps);
tic; v2 = 0;
for r = 1:reps
    v2 = v2 + snr(x);
end
t = toc;
fprintf('MATLAB snr N=%d  %.4f ms/call (snr=%.3f dB)\n', n, 1e3*t/reps, v2/reps);
end
