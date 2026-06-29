function bench_rbf_matlab()
    % MATLAB RBF peer for v13-e: griddata 'v4' is the biharmonic-spline RBF (Sandwell 1987 ==
    % thin-plate Green's function r^2 log r). Combined build+eval call (n source + nq query).
    rng(12345);
    n = 100; nq = 1000;
    P = rand(n, 2); v = rand(n, 1) * 2 - 1;
    Q = rand(nq, 2);
    griddata(P(:, 1), P(:, 2), v, Q(:, 1), Q(:, 2), 'v4'); % warm
    reps = 30;
    t = tic;
    for r = 1:reps
        vq = griddata(P(:, 1), P(:, 2), v, Q(:, 1), Q(:, 2), 'v4'); %#ok<NASGU>
    end
    el = toc(t) / reps;
    fprintf('MATLAB_griddata_v4_total_us %.2f  (n=%d src, nq=%d query, build+eval combined)\n', el * 1e6, n, nq);
end
