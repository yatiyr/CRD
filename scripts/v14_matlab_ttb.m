% v14 TTB rows (Sandia Tensor Toolbox, cloned 2026-07-05): cp_als/tucker_als
% (v14-j) + sptensor mttkrp (v14-i). Appends to the results file.
addpath('C:/Users/abici/tools/tensor_toolbox');
out = fopen('D:/Dev/cerid/scripts/v14_matlab_board_results.txt', 'a');
maxNumCompThreads(1);
best5 = @(f) min(arrayfun(@(k) timeit_once(f), 1:5));

fprintf(out, '\n[v14-j TTB cp_als / tucker_als, 1T best-of-5, tol=0 FIXED-BUDGET (default tol early-stops)]\n');
fprintf('[v14-j TTB]\n');
rng(7, 'twister');
X = tensor(rand(64, 64, 64));
t = best5(@() cp_als(X, 16, 'maxiters', 10, 'tol', 0, 'printitn', 0));
M = cp_als(X, 16, 'maxiters', 10, 'tol', 0, 'printitn', 0);
fitv = 1 - norm(X - full(M)) / norm(X);
line = sprintf('  cp_als 64^3 r=16 it=10 : %10.3f ms  fit %.6f', t*1e3, fitv);
fprintf(out, '%s\n', line); fprintf('%s\n', line);
t = best5(@() tucker_als(X, [16 16 16], 'maxiters', 5, 'tol', 0, 'printitn', 0));
line = sprintf('  tucker_als 64^3 core16 it=5 : %10.3f ms', t*1e3);
fprintf(out, '%s\n', line); fprintf('%s\n', line);
rng(9, 'twister');
X4 = tensor(rand(32, 32, 32, 32));
t = best5(@() cp_als(X4, 8, 'maxiters', 10, 'tol', 0, 'printitn', 0));
line = sprintf('  cp_als 32^4 r=8 it=10 : %10.3f ms', t*1e3);
fprintf(out, '%s\n', line); fprintf('%s\n', line);

fprintf(out, '\n[v14-i TTB sptensor mttkrp, 1024^3 / 5M nnz r=16, 1T best-of-5]\n');
fprintf('[v14-i TTB mttkrp]\n');
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
fclose(out);
fprintf('TTB DONE\n');

function t = timeit_once(f)
    tic; f(); t = toc;
end
