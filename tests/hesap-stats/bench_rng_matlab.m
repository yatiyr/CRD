% MATLAB R2026a RNG throughput — ns per uniform double, single-thread (fair vs Cerid next_double / NumPy .random).
maxNumCompThreads(1);
n = 4194304;
reps = 20;
fprintf('# MATLAB R2026a rand() throughput (single-thread, ns/double)\n');
gens = {'twister', 'threefry4x64_20', 'philox4x32_10', 'simdTwister', 'combRecursive'};
for i = 1:numel(gens)
    g = gens{i};
    try
        rng(12345, g);
    catch
        rng(12345);  % fallback name
    end
    rand(n, 1); % warm
    t = tic;
    for r = 1:reps
        rand(n, 1);
    end
    el = toc(t) / reps;
    fprintf('%-18s %6.3f ns/double\n', g, el / n * 1e9);
end
