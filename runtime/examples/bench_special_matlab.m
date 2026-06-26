% v12-a special-function timing — MATLAB R2026a (vectorized) peer for the all-peers board.
% Run: matlab -batch "run('runtime/examples/bench_special_matlab.m')"   (~44s startup)
% NOTE: MATLAB auto-multithreads elementwise array ops. For the apples-to-apples single-thread board set
% CRD_MATLAB_1T=1 in the environment (maxNumCompThreads(1)); leave unset for MATLAB's default (all cores).
if ~isempty(getenv('CRD_MATLAB_1T')); maxNumCompThreads(1); fprintf('# pinned to 1 thread\n'); end
N = 4000000;
rng(12345);
xe = rand(N,1)*8 - 4;        % erf   [-4,4]
xg = 0.1 + rand(N,1)*49.9;   % gamma [0.1,50]
xy = rand(N,1)*1.98 - 0.99;  % erfinv (-0.99,0.99)
xa = 0.05 + rand(N,1)*14.95; % gammainc(.,2.5) [0.05,15]
reps = 3;
function ns = t(f, x, N, reps)
    best = inf;
    for r = 1:reps
        tic; y = f(x); e = toc; %#ok<NASGU>
        best = min(best, e/N*1e9);
    end
    ns = best;
end
fprintf('# MATLAB R2026a vectorized, N = %d\n', N);
fprintf('erf         %8.2f ns/elem\n', t(@erf,    xe, N, reps));
fprintf('erfc        %8.2f ns/elem\n', t(@erfc,   xe, N, reps));
fprintf('erfinv      %8.2f ns/elem\n', t(@erfinv, xy, N, reps));
fprintf('lgamma      %8.2f ns/elem\n', t(@gammaln,xg, N, reps));
fprintf('tgamma      %8.2f ns/elem\n', t(@gamma,  xg, N, reps));
fprintf('digamma     %8.2f ns/elem\n', t(@psi,    xg, N, reps));
fprintf('gammainc_p  %8.2f ns/elem\n', t(@(x) gammainc(x,2.5), xa, N, reps));
