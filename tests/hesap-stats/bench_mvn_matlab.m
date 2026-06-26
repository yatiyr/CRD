% v12-k MVN bench vs MATLAB R2026a (single-thread for the fair 1T row). mvnpdf computes pdf; mvnrnd samples.
maxNumCompThreads(1);
mu = [1.0, -2.0, 0.5];
Sigma = [2.0 0.5 0.3; 0.5 1.5 -0.2; 0.3 -0.2 1.0];
N = 500000;
rng(1);
X = mu + 6.0 * (rand(N, 3) - 0.5);

mvnpdf(X(1:1000, :), mu, Sigma); % warm
t = tic; p = mvnpdf(X, mu, Sigma); el = toc(t);
fprintf('matlab_mvnpdf_ns %.3f\n', 1e9 * el / N);

mvnrnd(mu, Sigma, 1000); % warm
t = tic; Y = mvnrnd(mu, Sigma, N); el = toc(t);
fprintf('matlab_mvnrnd_ns %.3f\n', 1e9 * el / N);
