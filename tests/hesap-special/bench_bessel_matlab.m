% MATLAB R2026a Bessel/Airy timing (vectorized) — ns/element peer for the Cerid v12-b bench.
% Run: matlab -batch "run('tests/hesap-special/bench_bessel_matlab.m')"
% SINGLE-THREADED to match Cerid's per-call bench (MATLAB auto-multithreads element-wise ops otherwise — the
% honest-benchmark rule: match threading). The MT numbers belong against a Cerid parallel *_batch (future).
maxNumCompThreads(1);
n = 1e6;
rng(1);
nuset = [0 1 2 5 0.5 2.5];
nu = nuset(randi(6, n, 1)).';
x = 0.5 + 39.5 * rand(n, 1);
fprintf('# MATLAB R2026a Bessel/Airy — vectorized ns/element\n');
names = {'cyl_J','cyl_Y','cyl_I','cyl_K'};
fns = {@(v,z) besselj(v,z), @(v,z) bessely(v,z), @(v,z) besseli(v,z), @(v,z) besselk(v,z)};
for k = 1:4
    f = fns{k};
    f(nu, x); % warm
    t = tic; for r = 1:20, f(nu, x); end; el = toc(t) / 20;
    fprintf('%-10s MATLAB %8.2f ns/elem\n', names{k}, el / n * 1e9);
end
ax = -10 + 20 * rand(n, 1);
airy(0, ax); t = tic; for r = 1:20, airy(0, ax); end; el = toc(t) / 20;
fprintf('airy_Ai    MATLAB %8.2f ns/elem\n', el / n * 1e9);
airy(2, ax); t = tic; for r = 1:20, airy(2, ax); end; el = toc(t) / 20;
fprintf('airy_Bi    MATLAB %8.2f ns/elem\n', el / n * 1e9);
