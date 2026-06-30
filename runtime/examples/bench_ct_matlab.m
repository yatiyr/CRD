function bench_ct_matlab()
    % MATLAB peer for v13-f Clough-Tocher (n=100 scattered 2-D). griddata(...,'cubic') is MATLAB's
    % triangulation-based C1 cubic (the Clough-Tocher equivalent). It is one-shot (fit+eval combined),
    % so we compare its total against Cerid's fit + 1000 evals.
    rng(1);
    n = 100;
    P = [0 0; 1 0; 0 1; 1 1; rand(96, 2)];
    v = rand(n, 1) * 2 - 1;
    Q = rand(1000, 2) * 0.96 + 0.02;
    xq = Q(:, 1);
    yq = Q(:, 2);
    vq = griddata(P(:, 1), P(:, 2), v, xq, yq, 'cubic'); %#ok<NASGU> warm
    reps = 200;
    t = tic;
    for r = 1:reps
        vq = griddata(P(:, 1), P(:, 2), v, xq, yq, 'cubic'); %#ok<NASGU>
    end
    fprintf('MATLAB_griddata_cubic_fit+1000eval %.2f us\n', toc(t) / reps * 1e6);
end
