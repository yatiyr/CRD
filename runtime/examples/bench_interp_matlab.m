% v13-a/b interpolation peer bench: MATLAB pchip/spline/makima/interp1 build + eval on a 1000-knot / 100k-query setup.
n = 1000; nq = 100000;
rng(12345);
x = cumsum(0.5 + rand(1, n)); y = rand(1, n) * 10 - 5;
lo = x(1); hi = x(end);
qs = linspace(lo, hi, nq);
qr = lo + (hi - lo) * rand(1, nq);

reps = 3000;
tic; for k = 1:reps, pp = pchip(x, y);  end; fprintf('PCHIP            build %8.2f us/fit\n', toc/reps*1e6);
tic; for k = 1:reps, pp = spline(x, y); end; fprintf('Spline           build %8.2f us/fit\n', toc/reps*1e6);
tic; for k = 1:reps, pp = makima(x, y); end; fprintf('makima           build %8.2f us/fit\n', toc/reps*1e6);

pp = pchip(x, y); sp = spline(x, y); reps = 80;
tic; for k = 1:reps, v = ppval(pp, qs); end; fprintf('PCHIP     eval-sorted %8.2f ns/pt\n', toc/reps/nq*1e9);
tic; for k = 1:reps, v = ppval(pp, qr); end; fprintf('PCHIP     eval-random %8.2f ns/pt\n', toc/reps/nq*1e9);
tic; for k = 1:reps, v = ppval(sp, qs); end; fprintf('Spline    eval-sorted %8.2f ns/pt\n', toc/reps/nq*1e9);
tic; for k = 1:reps, v = interp1(x, y, qs, 'linear'); end; fprintf('linear  build+eval %8.2f ns/pt\n', toc/reps/nq*1e9);
