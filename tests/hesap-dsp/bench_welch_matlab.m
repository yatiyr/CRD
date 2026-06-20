% MATLAB pwelch baseline (multi-threaded, MKL FFT) for v11-m. Same params as the Cerid bench.
N = 10000000; nperseg = 4096; reps = 10;
x = sin(0.05 * (0:N-1))' + 0.3 * sin(0.21 * (0:N-1))';
w = hann(nperseg, 'periodic');
pwelch(x, w, nperseg/2, nperseg, 1.0);  % warmup
t = tic;
for r = 1:reps, P = pwelch(x, w, nperseg/2, nperseg, 1.0); end
fprintf('MATLAB pwelch(10M, nperseg=4096) multi-thread  %.3f ms/call\n', toc(t)/reps*1e3);
maxNumCompThreads(1);
pwelch(x, w, nperseg/2, nperseg, 1.0);
t = tic;
for r = 1:reps, P = pwelch(x, w, nperseg/2, nperseg, 1.0); end
fprintf('MATLAB pwelch(10M, nperseg=4096) 1-thread      %.3f ms/call\n', toc(t)/reps*1e3);
exit(0);
