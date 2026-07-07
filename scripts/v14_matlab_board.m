% v14 consolidated MATLAB board runner (2026-07-05).
% The license service blocks `matlab -batch` (error 5201) while interactive
% sessions work — so this ONE script batches every pending MATLAB row
% (the 44.5 s startup lesson: never one call per ref).
%
% RUN INSIDE YOUR OPEN MATLAB:
%   run('D:/Dev/cerid/scripts/v14_matlab_board.m')
%
% It prints the board AND writes scripts/v14_matlab_board_results.txt for
% ingestion. Protocol: maxNumCompThreads(1) (matched 1T), best-of-5 wall
% clock, f64, shapes matched to the C++ boards (values need not be
% bit-identical for wall-clock rows; fit parity is already gated
% elementwise-exact vs TensorLy on the C++ side).

out = fopen('D:/Dev/cerid/scripts/v14_matlab_board_results.txt', 'w');
prev_threads = maxNumCompThreads(1); %#ok<NASGU>
fprintf(out, 'MATLAB %s | maxNumCompThreads(1) | best-of-5\n', version);
fprintf('MATLAB %s | 1 thread | best-of-5\n', version);

best5 = @(f) min(arrayfun(@(k) timeit_once(f), 1:5));

%% ---- v14-h: pagemtimes batched GEMM (matched to the C++ board shapes) ----
fprintf(out, '\n[v14-h pagemtimes f64]\n'); fprintf('\n[v14-h pagemtimes f64]\n');
for cfg = [struct('n',4,'b',10000), struct('n',4,'b',100000), struct('n',6,'b',10000), ...
           struct('n',6,'b',100000), struct('n',8,'b',10000), struct('n',8,'b',100000), ...
           struct('n',16,'b',10000), struct('n',16,'b',100000)]
    rng(51, 'twister');
    A = rand(cfg.n, cfg.n, cfg.b); B = rand(cfg.n, cfg.n, cfg.b);
    t = best5(@() pagemtimes(A, B));
    line = sprintf('  n=%2d b=%6d  pagemtimes = %10.3f ms', cfg.n, cfg.b, t*1e3);
    fprintf(out, '%s\n', line); fprintf('%s\n', line);
end

%% ---- v14-h: pagemldivide (batched solve; guarded — needs R2022a+) ----
if exist('pagemldivide', 'builtin') || exist('pagemldivide', 'file')
    fprintf(out, '\n[v14-h pagemldivide f64]\n'); fprintf('\n[v14-h pagemldivide f64]\n');
    for cfg = [struct('n',4,'b',100000), struct('n',8,'b',100000), struct('n',16,'b',100000)]
        rng(52, 'twister');
        A = rand(cfg.n, cfg.n, cfg.b) + repmat(cfg.n*eye(cfg.n), 1, 1, cfg.b); % well-conditioned
        X = rand(cfg.n, 1, cfg.b);
        t = best5(@() pagemldivide(A, X));
        line = sprintf('  n=%2d b=%6d  pagemldivide = %10.3f ms', cfg.n, cfg.b, t*1e3);
        fprintf(out, '%s\n', line); fprintf('%s\n', line);
    end
else
    fprintf(out, '\n[v14-h pagemldivide] not available in this release\n');
end

%% ---- v14-j / v14-i: Sandia Tensor Toolbox rows (guarded on TTB presence) ----
if exist('sptensor', 'file') && exist('cp_als', 'file')
    fprintf(out, '\n[v14-j TTB cp_als / tucker_als]\n'); fprintf('\n[v14-j TTB]\n');
    rng(7, 'twister');
    X = tensor(rand(64, 64, 64));
    t = best5(@() cp_als(X, 16, 'maxiters', 10, 'printitn', 0));
    M = cp_als(X, 16, 'maxiters', 10, 'printitn', 0);
    fitv = 1 - norm(X - full(M)) / norm(X);
    line = sprintf('  cp_als 64^3 r=16 it=10 : %10.3f ms  fit %.6f', t*1e3, fitv);
    fprintf(out, '%s\n', line); fprintf('%s\n', line);
    t = best5(@() tucker_als(X, [16 16 16], 'maxiters', 5, 'printitn', 0));
    line = sprintf('  tucker_als 64^3 [16] it=5 : %10.3f ms', t*1e3);
    fprintf(out, '%s\n', line); fprintf('%s\n', line);

    fprintf(out, '\n[v14-i TTB sptensor mttkrp, 1024^3 / 5M nnz r=16]\n');
    rng(11, 'twister');
    nnz5m = 5e6;
    subs = [randi(1024, nnz5m, 1), randi(1024, nnz5m, 1), randi(1024, nnz5m, 1)];
    Xs = sptensor(subs, rand(nnz5m, 1), [1024 1024 1024]);
    U = {rand(1024, 16), rand(1024, 16), rand(1024, 16)};
    for m = 1:3
        t = best5(@() mttkrp(Xs, U, m));
        line = sprintf('  mttkrp mode-%d : %10.3f ms', m, t*1e3);
        fprintf(out, '%s\n', line); fprintf('%s\n', line);
    end
else
    msg = '[TTB] Tensor Toolbox (Sandia) not on path — cp_als/tucker_als/mttkrp rows skipped. Install: https://www.tensortoolbox.org (addpath after download).';
    fprintf(out, '\n%s\n', msg); fprintf('%s\n', msg);
end

fclose(out);
fprintf('\nDONE -> D:/Dev/cerid/scripts/v14_matlab_board_results.txt\n');

function t = timeit_once(f)
    tic; f(); t = toc;
end
