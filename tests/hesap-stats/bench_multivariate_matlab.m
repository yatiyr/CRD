% v12-k multivariate bench vs MATLAB R2026a, single-thread (the fair 1T row). One -batch call (startup ~44s).
% pdf/pmf vectorised over the batch; wishrnd/iwishrnd are per-call (looped). ns/op.
maxNumCompThreads(1);
N = 500000;
Nm = 50000;   % looped matrix samplers
rng(1);

mu = [1.0, -2.0, 0.5];
Sigma = [2.0 0.5 0.3; 0.5 1.5 -0.2; 0.3 -0.2 1.0];
X = mu + 6.0 * (rand(N, 3) - 0.5);

mvnpdf(X(1:1000, :), mu, Sigma);
t = tic; p = mvnpdf(X, mu, Sigma); el = toc(t);
fprintf('matlab_mvnpdf_ns %.3f\n', 1e9 * el / N);

mvnrnd(mu, Sigma, 1000);
t = tic; Y = mvnrnd(mu, Sigma, N); el = toc(t);
fprintf('matlab_mvnrnd_ns %.3f\n', 1e9 * el / N);

% MVt — MATLAB mvtpdf uses a correlation matrix; per-op cost is matrix-independent so this is a fair timing peer.
C = [1.0 0.3 0.2; 0.3 1.0 -0.1; 0.2 -0.1 1.0];
Xt = 3.0 * (rand(N, 3) - 0.5);
mvtpdf(Xt(1:1000, :), C, 5);
t = tic; pt = mvtpdf(Xt, C, 5); el = toc(t);
fprintf('matlab_mvtpdf_ns %.3f\n', 1e9 * el / N);

% Multinomial
p4 = [0.4 0.3 0.2 0.1];
Xc = mnrnd(20, p4, N);
mnpdf(Xc(1:1000, :), p4);
t = tic; pm = mnpdf(Xc, p4); el = toc(t);
fprintf('matlab_mnpdf_ns %.3f\n', 1e9 * el / N);

% Wishart / inverse-Wishart (per-call)
S3 = [2.0 0.3 0.1; 0.3 1.0 0.2; 0.1 0.2 1.5];
wishrnd(S3, 8);
t = tic; for i = 1:Nm, W = wishrnd(S3, 8); end; el = toc(t);
fprintf('matlab_wishrnd_ns %.3f\n', 1e9 * el / Nm);

iwishrnd(S3, 8);
t = tic; for i = 1:Nm, IW = iwishrnd(S3, 8); end; el = toc(t);
fprintf('matlab_iwishrnd_ns %.3f\n', 1e9 * el / Nm);
