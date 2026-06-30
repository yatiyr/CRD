function bench_grid_matlab()
    % MATLAB interpn peer for v13-f gridded N-linear (100x100 2-D, 100k queries).
    rng(12345);
    gn = 100; nq = 100000;
    [X, Y] = ndgrid(0:gn-1, 0:gn-1);
    V = rand(gn, gn);
    qx = rand(nq, 1) * (gn - 1);
    qy = rand(nq, 1) * (gn - 1);
    interpn(X, Y, V, qx, qy, 'linear'); % warm
    interpn(X, Y, V, qx, qy, 'cubic');  % warm both code paths before timing
    reps = 100;
    t = tic;
    for r = 1:reps
        vq = interpn(X, Y, V, qx, qy, 'linear'); %#ok<NASGU>
    end
    el = toc(t) / reps;
    fprintf('MATLAB_interpn_linear %.2f ns/pt  (100x100 grid, %d queries)\n', el * 1e9 / nq, nq);
    interpn(X, Y, V, qx, qy, 'cubic'); % warm
    t = tic;
    for r = 1:reps
        vq = interpn(X, Y, V, qx, qy, 'cubic'); %#ok<NASGU>
    end
    el = toc(t) / reps;
    fprintf('MATLAB_interpn_cubic %.2f ns/pt  (100x100 grid, %d queries)\n', el * 1e9 / nq, nq);
    interpn(X, Y, V, qx, qy, 'spline'); % warm
    t = tic;
    for r = 1:reps
        vq = interpn(X, Y, V, qx, qy, 'spline'); %#ok<NASGU>
    end
    el = toc(t) / reps;
    fprintf('MATLAB_interpn_spline %.2f ns/pt  (100x100 grid, %d queries)\n', el * 1e9 / nq, nq);
end
