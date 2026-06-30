function bench_gp_matlab()
    % MATLAB fitrgp peer for v13-f kriging (n=100 scattered 2-D). Fixed hyperparameters (ℓ=1, σ_f=1, noise=1e-6),
    % no optimization, no trend — to match Cerid's GaussianProcessInterpolant.
    rng(12345);
    n = 100; nq = 1000;
    X = rand(n, 2); y = rand(n, 1) * 2 - 1;
    Xq = rand(nq, 2);
    fitfn = @() fitrgp(X, y, 'KernelFunction', 'squaredexponential', 'KernelParameters', [1.0; 1.0], ...
                       'Sigma', 1e-3, 'FitMethod', 'none', 'BasisFunction', 'none', ...
                       'Standardize', false, 'ConstantSigma', true);
    gp = fitfn(); % warm
    reps = 30;
    t = tic;
    for r = 1:reps
        gp = fitfn(); %#ok<NASGU>
    end
    fprintf('MATLAB_fitrgp_fit %.2f us\n', toc(t) / reps * 1e6);
    [m, s] = predict(gp, Xq); %#ok<ASGLU> % warm
    t = tic;
    for r = 1:reps
        [m, s] = predict(gp, Xq); %#ok<ASGLU>
    end
    fprintf('MATLAB_fitrgp_predict %.2f ns/pt  (mean+std)\n', toc(t) / reps * 1e9 / nq);
end
