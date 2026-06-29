# v13-a/b interpolation peer bench: scipy.interpolate build + eval throughput on the same 1000-knot / 100k-query setup.
import time
import numpy as np
from scipy.interpolate import (PchipInterpolator, CubicSpline, Akima1DInterpolator,
                               BarycentricInterpolator, FloaterHormannInterpolator, RBFInterpolator,
                               RegularGridInterpolator)

s = np.uint64(12345)
def rnd():
    global s
    s = np.uint64(s * np.uint64(6364136223846793005) + np.uint64(1442695040888963407))
    return float(s >> np.uint64(11)) / float(1 << 53)
n, nq = 1000, 100000
x = np.zeros(n); y = np.zeros(n); acc = 0.0
for i in range(n):
    acc += 0.5 + rnd(); x[i] = acc; y[i] = rnd() * 10 - 5
lo, hi = x[0], x[-1]
qs = lo + (hi - lo) * np.arange(nq) / nq
qr = np.array([lo + (hi - lo) * rnd() for _ in range(nq)])

def bench_build(name, reps, fn):
    fn()
    t0 = time.perf_counter()
    for _ in range(reps):
        fn()
    us = (time.perf_counter() - t0) / reps * 1e6
    print("%-18s build %8.2f us/fit  %10.0f fits/s" % (name, us, 1e6 / us))

def bench_eval(name, qn, q, interp, reps=20):
    interp(q)
    t0 = time.perf_counter()
    for _ in range(reps):
        interp(q)
    ns = (time.perf_counter() - t0) / reps / len(q) * 1e9
    print("%-18s eval-%-6s %6.2f ns/pt  %8.1f Mevals/s" % (name, qn, ns, 1e3 / ns))

bench_build("PCHIP", 300, lambda: PchipInterpolator(x, y))
bench_build("CubicSpline-nak", 300, lambda: CubicSpline(x, y))
bench_build("CubicSpline-nat", 300, lambda: CubicSpline(x, y, bc_type="natural"))
ph = PchipInterpolator(x, y); cs = CubicSpline(x, y)
bench_eval("PCHIP", "sorted", qs, ph)
bench_eval("PCHIP", "random", qr, ph)
bench_eval("CubicSpline-nak", "sorted", qs, cs)
bench_eval("linear", "sorted", qs, lambda q: np.interp(q, x, y))

# --- v13-c ---
bench_build("makima", 300, lambda: Akima1DInterpolator(x, y, method="makima"))
mak = Akima1DInterpolator(x, y, method="makima")
bench_eval("makima", "sorted", qs, mak)
ng, ngq = 30, 10000
gx = np.zeros(ng); gy = np.zeros(ng); ga = 0.0
for i in range(ng):
    ga += 0.5 + rnd(); gx[i] = ga; gy[i] = rnd() * 10 - 5
gq = gx[0] + (gx[-1] - gx[0]) * np.arange(ngq) / ngq
bench_build("barycentric n30", 2000, lambda: BarycentricInterpolator(gx, gy))
bench_build("FloaterHorm n30", 2000, lambda: FloaterHormannInterpolator(gx, gy, d=3))
bc = BarycentricInterpolator(gx, gy); fh = FloaterHormannInterpolator(gx, gy, d=3)
bench_eval("barycentric n30", "n30", gq, bc, reps=200)
bench_eval("FloaterHorm n30", "n30", gq, fh, reps=200)

# --- v13-f: gridded N-linear (100x100 2D, 100k queries) ---
gn = 100
gax = np.arange(gn, dtype=float)
gV = np.array([[rnd() for _ in range(gn)] for _ in range(gn)])
grgi = RegularGridInterpolator((gax, gax), gV, method="linear")
gQ = np.array([[rnd() * (gn - 1), rnd() * (gn - 1)] for _ in range(100000)])
bench_eval("grid-linear 2D", "grid", gQ, grgi, reps=100)
grgic = RegularGridInterpolator((gax, gax), gV, method="cubic")
bench_eval("grid-cubic 2D", "grid", gQ, grgic, reps=20)

# --- v13-d ---
from numpy.polynomial import chebyshev as npcheb
from scipy.interpolate import pade
ncb = 64
fcheb = lambda t: 1.0 / (1.0 + t * t)
bench_build("Chebyshev n64", 300, lambda: npcheb.chebinterpolate(fcheb, ncb - 1))
ccheb = npcheb.chebinterpolate(fcheb, ncb - 1)
xcb = np.linspace(-1, 1, 20000)
bench_eval("Chebyshev n64", "n64", xcb, lambda q: npcheb.chebval(q, ccheb), reps=200)
tcf = np.zeros(9); tcf[0] = 1.0
for i in range(1, 9):
    tcf[i] = tcf[i - 1] / i
bench_build("Pade 4/4", 2000, lambda: pade(tcf, 4, 4))
pp, qq = pade(tcf, 4, 4)
xpd = np.linspace(0, 2, 100000)
bench_eval("Pade 4/4", "pd", xpd, lambda q: pp(q) / qq(q), reps=50)

# --- v13-e: RBF (N=100 scattered 2-D, gaussian) ---
nrb = 100
rpts = np.column_stack([np.array([rnd() for _ in range(nrb)]), np.array([rnd() for _ in range(nrb)])])
rval = np.array([rnd() * 2 - 1 for _ in range(nrb)])
bench_build("RBF gauss n100", 300, lambda: RBFInterpolator(rpts, rval, kernel="gaussian", epsilon=1.0))
rbfp = RBFInterpolator(rpts, rval, kernel="gaussian", epsilon=1.0)
rq = np.column_stack([np.array([rnd() for _ in range(1000)]), np.array([rnd() for _ in range(1000)])])
bench_eval("RBF gauss n100", "n100", rq, rbfp, reps=200)
