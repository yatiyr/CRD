function bench_multirate_matlab()
% MATLAB baseline for resample_poly (v11-k). resample(x,p,q) uses a Kaiser-windowed polyphase anti-alias FIR.
maxNumCompThreads(1);
fprintf('MATLAB threads = %d\n', maxNumCompThreads);
cfg = [3 2; 2 3];
n = 1000000; reps = 30;
i = (0:n-1)';
x = sin(2*pi*0.05*i) + 0.5*sin(2*pi*0.13*i);
for c = 1:size(cfg,1)
    p = cfg(c,1); q = cfg(c,2);
    resample(x, p, q); % warm
    tic; chk = 0;
    for r = 1:reps
        y = resample(x, p, q);
        chk = chk + y(floor(numel(y)/2)+1);
    end
    t = toc;
    fprintf('MATLAB resample N=%d up=%d down=%d  %.4f ms/call (out=%d chk=%.5f)\n', n, p, q, 1e3*t/reps, numel(y), chk);
end
end
