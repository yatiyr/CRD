% MATLAB R2026a distribution throughput — ns/element, single-thread (fair vs Cerid + scipy).
maxNumCompThreads(1);
n = 1e6;
x = linspace(-5, 7, n)';
p = linspace(1e-4, 1-1e-4, n)';
xg = 0.01 + x + 5.0;
k25 = mod((0:n-1)', 25);
k41 = mod((0:n-1)', 41);
function t = bench(f, n)
    f(); tic; f(); t = toc / n * 1e9;
end
fprintf('# MATLAB R2026a distribution throughput (single-thread, ns/elem)\n');
fprintf('normal.pdf       %8.3f\n', bench(@() normpdf(x,0,1), n));
fprintf('normal.cdf       %8.3f\n', bench(@() normcdf(x,0,1), n));
fprintf('normal.ppf       %8.3f\n', bench(@() norminv(p,0,1), n));
fprintf('gamma.pdf        %8.3f\n', bench(@() gampdf(xg,2.5,1.5), n));
fprintf('gamma.cdf        %8.3f\n', bench(@() gamcdf(xg,2.5,1.5), n));
fprintf('gamma.ppf        %8.3f\n', bench(@() gaminv(p,2.5,1.5), n));
fprintf('beta.pdf         %8.3f\n', bench(@() betapdf(p,2,5), n));
fprintf('beta.cdf         %8.3f\n', bench(@() betacdf(p,2,5), n));
fprintf('beta.ppf         %8.3f\n', bench(@() betainv(p,2,5), n));
fprintf('studentt.pdf     %8.3f\n', bench(@() tpdf(x,7), n));
fprintf('studentt.cdf     %8.3f\n', bench(@() tcdf(x,7), n));
fprintf('studentt.ppf     %8.3f\n', bench(@() tinv(p,7), n));
fprintf('poisson.pmf      %8.3f\n', bench(@() poisspdf(k25,8), n));
fprintf('poisson.cdf      %8.3f\n', bench(@() poisscdf(k25,8), n));
fprintf('binomial.pmf     %8.3f\n', bench(@() binopdf(k41,40,0.3), n));
fprintf('binomial.cdf     %8.3f\n', bench(@() binocdf(k41,40,0.3), n));
