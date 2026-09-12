function bench_adaptive_matlab()
% MATLAB baseline for the adaptive-filter throughput bench (v11-t). dsp.LMSFilter (DSP System Toolbox).
maxNumCompThreads(1);
fprintf('MATLAB threads = %d\n', maxNumCompThreads);
n = 1000000; m = 32; reps = 20;
i = (0:n-1)';
x = sin(0.017*i) + 0.3*cos(0.05*i);
d = x;
lms = dsp.LMSFilter('Length', m, 'StepSize', 0.01, 'Method', 'LMS');
lms(x(1:100), d(1:100)); reset(lms); % warm
tic; chk = 0;
for r = 1:reps
    reset(lms);
    [y, ~, ~] = lms(x, d);
    chk = chk + y(end);
end
t = toc;
fprintf('MATLAB dsp.LMSFilter m=%d N=%d  %.4f ms/call (chk=%.4f)\n', m, n, 1e3*t/reps, chk);
end
