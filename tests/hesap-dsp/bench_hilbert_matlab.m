function bench_hilbert_matlab()
% MATLAB baseline for the Hilbert / analytic-signal benchmark (v11-l). hilbert() is FFT-based, same as Cerid/scipy.
maxNumCompThreads(1);  % FAIR: force single-thread (MATLAB FFT is multi-threaded MKL by default) vs 1-thread Cerid/scipy
fprintf('MATLAB threads = %d\n', maxNumCompThreads);
sizes = [2^16, 2^20];
reps  = [500, 50];
for s = 1:numel(sizes)
    n = sizes(s); i = (0:n-1)';
    x = sin(0.01*i) + 0.3*cos(0.023*i);
    hilbert(x); % warm
    tic; chk = 0;
    for r = 1:reps(s)
        h = hilbert(x);
        chk = chk + imag(h(n/2+1));
    end
    t = toc;
    fprintf('MATLAB hilbert N=%-8d  %.4f ms/call (chk=%.4f)\n', n, 1e3*t/reps(s), chk);
end
end
