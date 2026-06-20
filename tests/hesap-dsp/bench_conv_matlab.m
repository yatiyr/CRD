% MATLAB FFT-convolution: BOTH single-thread (fair vs Cerid's single-threaded FftConvolver) and multi-thread.
N = 1000000; reps = 20;
x = sin(0.01 * (0:N-1))';
h = (cos(0.02 * (0:N-1)) .* exp(-1e-6 * (0:N-1)))';

maxNumCompThreads(1);
for w = 1:3, c = ifft(fft(x, 2^21) .* fft(h, 2^21), 'symmetric'); c = c(1:2*N-1); end
t = tic;
for r = 1:reps, c = ifft(fft(x, 2^21) .* fft(h, 2^21), 'symmetric'); c = c(1:2*N-1); end
fprintf('MATLAB fftconv 1-thread  %.3f ms/call\n', toc(t)/reps*1e3);

maxNumCompThreads('automatic');
for w = 1:3, c = ifft(fft(x, 2^21) .* fft(h, 2^21), 'symmetric'); c = c(1:2*N-1); end
t = tic;
for r = 1:reps, c = ifft(fft(x, 2^21) .* fft(h, 2^21), 'symmetric'); c = c(1:2*N-1); end
fprintf('MATLAB fftconv multi-thread  %.3f ms/call\n', toc(t)/reps*1e3);
exit(0);
