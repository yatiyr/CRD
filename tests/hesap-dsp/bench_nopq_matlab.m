function bench_nopq_matlab()
% MATLAB baseline for the v11-q CZT + v11-o arburg benchmarks.
maxNumCompThreads(1);
fprintf('MATLAB threads = %d\n', maxNumCompThreads);
% CZT N=4096, M=N (default contour) — same deterministic x as Cerid (chk should match).
n = 4096; i = (0:n-1)';
xc = sin(0.013*i) + 0.4*cos(0.071*i);
czt(xc); % warm
tic; chk = 0;
for r = 1:100
    X = czt(xc);
    chk = chk + imag(X(3));
end
t = toc;
fprintf('MATLAB czt   N=%d M=%d  %.4f ms/call (chk=%.4f)\n', n, n, 1e3*t/100, chk);
% arburg N=100000, p=20 (timing only — data need not match Cerid).
nar = 100000; p = 20;
xa = filter(1, [1 -1.27 0.81], randn(nar,1));
arburg(xa, p); % warm
tic; chk2 = 0;
for r = 1:50
    aa = arburg(xa, p);
    chk2 = chk2 + aa(2);
end
t = toc;
fprintf('MATLAB arburg N=%d p=%d  %.4f ms/call (chk=%.4f)\n', nar, p, 1e3*t/50, chk2);
end
