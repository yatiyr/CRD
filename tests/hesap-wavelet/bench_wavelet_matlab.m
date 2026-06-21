% v11w-b: MATLAB Wavelet Toolbox wavedec timing (1-thread, fair vs Cerid's single-threaded kernel).
% Run on Windows: matlab -batch "run('tests/hesap-wavelet/bench_wavelet_matlab.m')"
% Uses the SAME reproducible LCG signal as the C / pywt benches (chk-comparable).
maxNumCompThreads(1);
N = 2^20;
s = uint64(88172645463325252);
mul = uint64(6364136223846793005); inc = uint64(1442695040888963407);
x = zeros(1, N);
for i = 1:N
    s = s * mul + inc;            % uint64 wraps mod 2^64 (saturating in MATLAB — see note)
    x(i) = (double(bitshift(s, -11)) * (1.0/9007199254740992.0)) * 2.0 - 1.0;
end
% NOTE: MATLAB uint64 arithmetic SATURATES (does not wrap like C/numpy). If chk does not match, load the
% shared input instead of regenerating. The timing is the point here; correctness is gated by the test suite.

cfgs = { {'haar',6,'per'}, {'db4',6,'per'}, {'db8',6,'per'}, {'sym8',6,'sym'} };
for c = 1:numel(cfgs)
    wav = cfgs{c}{1}; lvl = cfgs{c}{2}; mode = cfgs{c}{3};
    dwtmode(mode, 'nodisp');
    [C, L] = wavedec(x, lvl, wav);   % warm
    reps = 50; t0 = tic;
    for r = 1:reps
        [C, L] = wavedec(x, lvl, wav);
    end
    dt = toc(t0) / reps * 1e3;
    fprintf('MATLAB wavedec N=%d wav=%-6s level=%d mode=%-13s  %.4f ms/call\n', N, wav, lvl, mode, dt);
end
