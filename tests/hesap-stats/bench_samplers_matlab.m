% MATLAB R2026a sampler throughput — ns/sample, single-thread (fair vs Cerid + NumPy).
maxNumCompThreads(1);
n = 5e6;
function t = bench(f, n)
    f();          % warm
    tic; f(); t = toc / n * 1e9;
end
fprintf('# MATLAB R2026a sampler throughput (single-thread, ns/sample)\n');
fprintf('normal            %7.3f\n', bench(@() randn(n,1), n));
fprintf('exponential       %7.3f\n', bench(@() exprnd(1,n,1), n));
fprintf('gamma(2.5)        %7.3f\n', bench(@() gamrnd(2.5,1,n,1), n));
fprintf('beta(2,5)         %7.3f\n', bench(@() betarnd(2,5,n,1), n));
fprintf('poisson(4)        %7.3f\n', bench(@() poissrnd(4,n,1), n));
fprintf('poisson(30)       %7.3f\n', bench(@() poissrnd(30,n,1), n));
fprintf('binomial(20,.3)   %7.3f\n', bench(@() binornd(20,0.3,n,1), n));
fprintf('binomial(1000,.5) %7.3f\n', bench(@() binornd(1000,0.5,n,1), n));
