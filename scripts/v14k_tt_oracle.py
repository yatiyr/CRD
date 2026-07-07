#!/usr/bin/env python3
# v14-k TT oracle — reconstruct-verify-first (the v13 discipline).
#
# Reconstructs, IN NUMPY, the exact algorithms tt.hpp ports:
#   - maxvol            (Goreinov-Oseledets-Savostyanov-Tyrtyshnikov-Zamarashkin,
#                        "How to Find a Good Submatrix", 2010 — transliterated from
#                        tntorch/maxvol.py `py_maxvol`, itself taken verbatim from
#                        A. Mikhalev's maxvolpy; LU-pivot init, C = A*Ahat^-1 via two
#                        triangular solves, argmax + rank-1 update, tol/max_iters bounded)
#   - TT-SVD            (Oseledets 2011, Alg. 1: sequential unfolding SVD, delta =
#                        eps*||A||_F/sqrt(d-1) tail-energy chop)
#   - TT-rounding       (Oseledets 2011, Alg. 2: right-to-left QR orthogonalization,
#                        left-to-right SVD truncation)
#   - TT-cross          (Oseledets-Tyrtyshnikov 2010 ALS sweeps with QR+maxvol pivoting —
#                        the tntorch/cross.py scheme with FIXED ranks: L2R sweep updates
#                        left index sets, R2L updates right sets, first core = raw fibers,
#                        validation-error convergence, max_sweeps bounded)
# verifies each against tntorch (TT-SVD/round/point-eval/py_maxvol) on frozen problems,
# and emits tests/hesap-tensor/ref_tt.inc (plain C arrays — the no-std-containers rule).
#
# Peers: tntorch 1.1.2 (torch-based, py3.12-safe). ttpy: N/A-WITH-CHECK — `pip3 install
# --break-system-packages ttpy` fails metadata generation on Python 3.12 (its setup.py
# imports numpy.distutils, removed from numpy for py>=3.12); recorded 2026-07-05.
#
# Run (WSL): python3 scripts/v14k_tt_oracle.py
import os
import sys

import numpy as np
import torch
import tntorch as tn
from tntorch.maxvol import py_maxvol

torch.set_default_dtype(torch.float64)
np.random.seed(0)
torch.manual_seed(0)

FAIL = []


def check(name, ok, detail=""):
    tag = "ok " if ok else "FAIL"
    print(f"[{tag}] {name} {detail}")
    if not ok:
        FAIL.append(name)


# ---------------------------------------------------------------------------
# deterministic integer-hash values (bit-exact in C++: u32 wrap arithmetic)
# ---------------------------------------------------------------------------
M32 = 0xFFFFFFFF


def hash_val(k, a, i, b):
    h = ((k * 73856093) & M32) ^ ((a * 19349663) & M32) ^ ((i * 83492791) & M32) ^ ((b * 2971215073) & M32)
    return h * (1.0 / 4294967296.0) - 0.5


# ---------------------------------------------------------------------------
# maxvol — the exact algorithm tt.hpp ports
# ---------------------------------------------------------------------------
def np_maxvol(A, tol=1.05, max_iters=100):
    """Returns (index[:r] selected rows, C = A @ inv(A[index[:r]]), iters, converged)."""
    A = np.asarray(A, dtype=np.float64)
    N, r = A.shape
    if N <= r:
        return np.arange(N), np.eye(N), 0, True
    # LU with partial pivoting on a copy of the TALL matrix (pivot rows = init set)
    lu = A.copy()
    ipiv = np.zeros(r, dtype=np.int64)
    for j in range(r):
        p = j + int(np.argmax(np.abs(lu[j:, j])))  # first max wins ties (lowest row)
        ipiv[j] = p
        if p != j:
            lu[[j, p]] = lu[[p, j]]
        piv = lu[j, j]
        if piv == 0.0:
            piv = 1.0  # singular guard (flagged upstream by callers; keeps bounded)
        lu[j + 1:, j] /= piv
        lu[j + 1:, j + 1:] -= np.outer(lu[j + 1:, j], lu[j, j + 1:])
    index = np.arange(N)
    for i in range(r):
        index[i], index[ipiv[i]] = index[ipiv[i]], index[i]
    # C = A U^-1 L1^-1  (two row-wise triangular solves; Ahat = A[index[:r]] = L1 U)
    U = np.triu(lu[:r])
    L1 = np.tril(lu[:r], -1) + np.eye(r)
    C = np.empty((N, r))
    for row in range(N):
        y = np.empty(r)
        for j in range(r):
            y[j] = (A[row, j] - y[:j] @ U[:j, j]) / U[j, j]
        z = np.empty(r)
        for j in range(r - 1, -1, -1):
            z[j] = y[j] - z[j + 1:] @ L1[j + 1:, j]
        C[row] = z
    # swap loop: argmax |C| (row-major first-max tie rule), swap, rank-1 update
    iters = 0
    converged = False
    while iters < max_iters:
        flat = int(np.argmax(np.abs(C)))
        row, slot = divmod(flat, r)
        if abs(C[row, slot]) <= tol:
            converged = True
            break
        index[slot] = row
        col = C[:, slot].copy()
        rowv = C[row, :].copy()
        rowv[slot] -= 1.0
        C -= np.outer(col, rowv) / C[row, slot]
        iters += 1
    return index[:r].copy(), C, iters, converged


# ---------------------------------------------------------------------------
# TT-SVD / rounding — the exact algorithms tt.hpp ports
# ---------------------------------------------------------------------------
def rank_chop(s, delta, rmax):
    """Smallest r >= 1 with sum_{i>=r} s_i^2 <= delta^2, capped at rmax."""
    k = len(s)
    tail = 0.0
    r = k
    for i in range(k - 1, 0, -1):
        tail += s[i] * s[i]
        if tail > delta * delta:
            break
        r = i
    return max(1, min(r, rmax, k))


def np_tt_svd(A, eps, rmax=1 << 30):
    d = A.ndim
    shape = A.shape
    nrm = np.linalg.norm(A)
    delta = eps * nrm / max(np.sqrt(d - 1), 1.0)
    C = A.reshape(shape[0], -1)
    r_prev = 1
    cores = []
    for k in range(d - 1):
        C = C.reshape(r_prev * shape[k], -1)
        U, s, Vt = np.linalg.svd(C, full_matrices=False)
        r = rank_chop(s, delta, rmax)
        cores.append(U[:, :r].reshape(r_prev, shape[k], r).copy())
        C = s[:r, None] * Vt[:r]
        r_prev = r
    cores.append(C.reshape(r_prev, shape[d - 1], 1).copy())
    return cores


def tt_full(cores):
    P = cores[0].reshape(cores[0].shape[1], cores[0].shape[2])
    shape = [cores[0].shape[1]]
    for c in cores[1:]:
        rl, n, rr = c.shape
        P = P @ c.reshape(rl, n * rr)
        P = P.reshape(-1, rr)
        shape.append(n)
    return P.reshape(shape)


def tt_ranks(cores):
    return [c.shape[2] for c in cores[:-1]]


def np_tt_round(cores, eps, rmax=1 << 30):
    d = len(cores)
    cores = [c.copy() for c in cores]
    # right-to-left QR orthogonalization (rows of core k orthonormalized)
    for k in range(d - 1, 0, -1):
        rl, n, rr = cores[k].shape
        Q, R = np.linalg.qr(cores[k].reshape(rl, n * rr).T)  # (n*rr) x kk, kk x rl
        kk = Q.shape[1]
        cores[k] = np.ascontiguousarray(Q.T).reshape(kk, n, rr)
        prev = cores[k - 1]
        pl, pn, pr = prev.shape
        cores[k - 1] = (prev.reshape(pl * pn, pr) @ R.T).reshape(pl, pn, kk)
    nrm = np.linalg.norm(cores[0])
    delta = eps * nrm / max(np.sqrt(d - 1), 1.0)
    # left-to-right SVD truncation
    for k in range(d - 1):
        rl, n, rr = cores[k].shape
        U, s, Vt = np.linalg.svd(cores[k].reshape(rl * n, rr), full_matrices=False)
        r = rank_chop(s, delta, rmax)
        cores[k] = U[:, :r].reshape(rl, n, r).copy()
        carry = s[:r, None] * Vt[:r]
        nxt = cores[k + 1]
        nl, nn, nr2 = nxt.shape
        cores[k + 1] = (carry @ nxt.reshape(nl, nn * nr2)).reshape(r, nn, nr2)
    return cores


def np_tt_add(a, b):
    d = len(a)
    if d == 1:
        return [a[0] + b[0]]
    out = []
    for k in range(d):
        al, n, ar = a[k].shape
        bl, _, br = b[k].shape
        if k == 0:
            c = np.concatenate([a[k], b[k]], axis=2)
        elif k == d - 1:
            c = np.concatenate([a[k], b[k]], axis=0)
        else:
            c = np.zeros((al + bl, n, ar + br))
            c[:al, :, :ar] = a[k]
            c[al:, :, ar:] = b[k]
        out.append(c)
    return out


# ---------------------------------------------------------------------------
# TT-cross — the exact fixed-rank ALS/maxvol scheme tt.hpp ports
# ---------------------------------------------------------------------------
def np_tt_cross(f, shape, max_rank, max_sweeps, tol, seed, val_size=200):
    d = len(shape)
    rng = np.random.default_rng(seed)
    Rs = np.array([1] + [max_rank] * (d - 1) + [1], dtype=np.int64)
    for n in list(range(1, d)) + list(range(d - 1, 0, -1)):
        Rs[n] = min(Rs[n - 1] * shape[n - 1], Rs[n], shape[n] * Rs[n + 1])
    # nested random right sets (the tntorch construction)
    maxR = int(Rs.max())
    randmat = np.zeros((maxR, d), dtype=np.int64)
    for n in range(d - 1):
        randmat[:, n] = rng.integers(0, shape[n + 1], maxR)
    lsets = [np.zeros((1, 0), dtype=np.int64)] + [None] * (d - 1)
    rsets = [randmat[: Rs[j + 1], j : d - 1].copy() for j in range(d - 1)] + [np.zeros((1, 0), dtype=np.int64)]
    # validation set
    val_idx = np.stack([rng.integers(0, shape[k], val_size) for k in range(d)], axis=1)
    ys_val = np.array([f(ix) for ix in val_idx])
    norm_val = np.linalg.norm(ys_val)
    cores = [None] * d
    nevals = 0

    def eval_fibers(j):
        nonlocal nevals
        rl, n, rr = len(lsets[j]), shape[j], len(rsets[j])
        V = np.empty((rl, n, rr))
        for a in range(rl):
            for i in range(n):
                for b in range(rr):
                    idx = np.concatenate([lsets[j][a], [i], rsets[j][b]])
                    V[a, i, b] = f(idx)
        nevals += V.size
        return V

    def tt_eval(cores, idx):
        v = cores[0][0, idx[0], :]
        for k in range(1, d):
            v = v @ cores[k][:, idx[k], :]
        return v[0]

    sweeps_done = 0
    val_err = np.inf
    converged = False
    for it in range(max_sweeps):
        # left-to-right
        for j in range(d - 1):
            V = eval_fibers(j)
            rl, n, rr = V.shape
            Q, _ = np.linalg.qr(V.reshape(rl * n, rr))
            sel, _, _, _ = np_maxvol(Q)
            cores[j] = np.linalg.solve(Q[sel].T, Q.T).T.reshape(rl, n, Q.shape[1])
            lr, li = np.unravel_index(sel, (rl, n))
            lsets[j + 1] = np.concatenate([lsets[j][lr], li[:, None]], axis=1)
        # right-to-left
        for j in range(d - 1, 0, -1):
            V = eval_fibers(j)
            rl, n, rr = V.shape
            Q, _ = np.linalg.qr(V.reshape(rl, n * rr).T)
            sel, _, _, _ = np_maxvol(Q)
            cores[j] = np.linalg.solve(Q[sel].T, Q.T).reshape(rl, n, rr)
            li, lr = np.unravel_index(sel, (n, rr))
            rsets[j - 1] = np.concatenate([li[:, None], rsets[j][lr]], axis=1)
        cores[0] = eval_fibers(0)
        sweeps_done = it + 1
        pred = np.array([tt_eval(cores, ix) for ix in val_idx])
        val_err = np.linalg.norm(ys_val - pred) / norm_val
        if val_err < tol:
            converged = True
            break
    return cores, val_err, sweeps_done, nevals, converged


# ===========================================================================
# frozen problems
# ===========================================================================
print("=== v14-k TT oracle (tntorch reference + numpy mirrors) ===")

# --- A. maxvol known answer (deterministic integer-hash 16x4 matrix) --------
A_mv = np.array([[hash_val(9, r, c, 0) for c in range(4)] for r in range(16)])
mv_idx, mv_C, mv_iters, mv_conv = np_maxvol(A_mv, tol=1.05, max_iters=100)
tnt_idx, tnt_C = py_maxvol(A_mv, tol=1.05, max_iters=100)
mv_sorted = sorted(mv_idx.tolist())
check("maxvol: mirror == tntorch py_maxvol row set", mv_sorted == sorted(tnt_idx.tolist()),
      f"rows={mv_sorted} iters={mv_iters}")
check("maxvol: |C| <= 1.05 and C[sel] == I", mv_conv and np.abs(mv_C).max() <= 1.05 + 1e-12 and
      np.allclose(mv_C[mv_idx], np.eye(4), atol=1e-10), f"max|C|={np.abs(mv_C).max():.6f}")

# --- B. Hilbert 4D (12^4): TT-SVD ranks + errors ----------------------------
HN, HD = 12, 4
grid = np.indices((HN,) * HD).sum(axis=0)
A_h = 1.0 / (1.0 + grid)
At_h = torch.tensor(A_h)
h_eps = [1e-4, 1e-8, 1e-12]
h_ranks = []
h_errs = []
for eps in h_eps:
    cores = np_tt_svd(A_h, eps)
    err = np.linalg.norm(tt_full(cores) - A_h) / np.linalg.norm(A_h)
    t = tn.Tensor(At_h, eps=eps)
    terr = (torch.norm(t.torch() - At_h) / torch.norm(At_h)).item()
    tranks = [int(x) for x in t.ranks_tt[1:-1]]
    h_ranks.append(tt_ranks(cores))
    h_errs.append(err)
    check(f"hilbert tt_svd eps={eps:g}: err <= eps (mirror {err:.3e}, tntorch {terr:.3e})", err <= eps)
    print(f"       ranks mirror={tt_ranks(cores)} tntorch={tranks}")

# per-point gate calibration at eps=1e-8: mirror TT vs tntorch TT point values
eval_idx = np.array([[(3 * p + 2 * k + p * k) % HN for k in range(HD)] for p in range(8)], dtype=np.int64)
cores_h8 = np_tt_svd(A_h, 1e-8)


def np_tt_eval(cores, idx):
    v = cores[0][0, idx[0], :]
    for k in range(1, len(cores)):
        v = v @ cores[k][:, idx[k], :]
    return float(v[0])


t_h8 = tn.Tensor(At_h, eps=1e-8)
pts_mirror = np.array([np_tt_eval(cores_h8, ix) for ix in eval_idx])
pts_tnt = np.array([float(t_h8[tuple(ix.tolist())]) for ix in eval_idx])
pts_true = np.array([A_h[tuple(ix)] for ix in eval_idx])
gap = np.abs(pts_mirror - pts_tnt).max()
eval_gate = max(1e-10, 100.0 * gap)
check("hilbert eps=1e-8 per-point: mirror vs tntorch", gap < 1e-8, f"max gap {gap:.3e} -> gate {eval_gate:.3e}")

# --- C. exact low-TT-rank (8^4, ranks 3,3,3, integer-hash cores) ------------
LN, LD, LR = 8, 4, 3
lr_ranks = [1, LR, LR, LR, 1]
lr_cores = []
for k in range(LD):
    rl, rr = lr_ranks[k], lr_ranks[k + 1]
    c = np.array([[[hash_val(k, a, i, b) for b in range(rr)] for i in range(LN)] for a in range(rl)])
    lr_cores.append(c)
A_lr = tt_full(lr_cores)
lr_norm = float(np.linalg.norm(A_lr))
cores_lr = np_tt_svd(A_lr, 1e-10)
err_lr = np.linalg.norm(tt_full(cores_lr) - A_lr) / lr_norm
check("lowrank tt_svd: exact rank recovery (3,3,3)", tt_ranks(cores_lr) == [3, 3, 3], f"{tt_ranks(cores_lr)}")
check("lowrank tt_svd: err ~ machine", err_lr <= 1e-13, f"{err_lr:.3e}")
t_lr = tn.Tensor(torch.tensor(A_lr), eps=1e-10)
check("lowrank tntorch agrees (3,3,3)", [int(x) for x in t_lr.ranks_tt[1:-1]] == [3, 3, 3])

# rounding: (A+A) has structural ranks (6,6,6); round(1e-10) -> (3,3,3), == 2A
sum_cores = np_tt_add(lr_cores, lr_cores)
check("lowrank add: structural ranks (6,6,6)", tt_ranks(sum_cores) == [6, 6, 6])
rounded = np_tt_round(sum_cores, 1e-10)
err_round = np.linalg.norm(tt_full(rounded) - 2.0 * A_lr) / (2.0 * lr_norm)
check("lowrank round(1e-10): ranks back to (3,3,3)", tt_ranks(rounded) == [3, 3, 3], f"{tt_ranks(rounded)}")
check("lowrank round: err ~ machine", err_round <= 1e-13, f"{err_round:.3e}")
tt_sum = tn.Tensor([torch.tensor(c) for c in sum_cores])
tt_sum.round_tt(1e-10)
check("lowrank tntorch round_tt agrees (3,3,3)", [int(x) for x in tt_sum.ranks_tt[1:-1]] == [3, 3, 3])

# hilbert: round a fine TT (eps=1e-12) at 1e-4 -> same ranks as direct tt_svd @1e-4
h_rounded = np_tt_round(np_tt_svd(A_h, 1e-12), 1e-4)
err_hr = np.linalg.norm(tt_full(h_rounded) - A_h) / np.linalg.norm(A_h)
check("hilbert round(1e-12 TT -> 1e-4): ranks == tt_svd@1e-4", tt_ranks(h_rounded) == h_ranks[0],
      f"{tt_ranks(h_rounded)} vs {h_ranks[0]}")
check("hilbert round: err <= 1e-4", err_hr <= 1e-4, f"{err_hr:.3e}")

# --- D. smooth 4D gravitational-like kernel (16^4 grid) ---------------------
SN, SD = 16, 4
SOFT = 0.1
sx = np.array([-1.0 + 2.0 * i / (SN - 1) for i in range(SN)])


def f_smooth_idx(idx):
    s = SOFT
    for k in range(SD):
        s += sx[idx[k]] * sx[idx[k]]
    return 1.0 / np.sqrt(s)


Xg = np.meshgrid(*([sx] * SD), indexing="ij")
A_s = 1.0 / np.sqrt(SOFT + sum(x * x for x in Xg))
s_eps = 1e-10
cores_s = np_tt_svd(A_s, s_eps)
err_s = np.linalg.norm(tt_full(cores_s) - A_s) / np.linalg.norm(A_s)
t_s = tn.Tensor(torch.tensor(A_s), eps=s_eps)
check(f"smooth tt_svd eps={s_eps:g}: err <= eps", err_s <= s_eps,
      f"mirror err {err_s:.3e} ranks {tt_ranks(cores_s)} | tntorch ranks {[int(x) for x in t_s.ranks_tt[1:-1]]}")
smooth_svd_ranks = tt_ranks(cores_s)

# --- E. TT-cross on the smooth function (frozen budget) ---------------------
CR_RANK, CR_SWEEPS, CR_TOL, CR_VAL = 10, 6, 1e-9, 200
cr_cores, cr_err, cr_sweeps, cr_evals, cr_conv = np_tt_cross(
    f_smooth_idx, (SN,) * SD, CR_RANK, CR_SWEEPS, CR_TOL, seed=42, val_size=CR_VAL)
check("cross mirror: converged within budget", cr_conv,
      f"val_err {cr_err:.3e} sweeps {cr_sweeps} evals {cr_evals}")
# probe accuracy on frozen indices
probe_idx = np.array([[(5 * p + 3 * k + p * p) % SN for k in range(SD)] for p in range(16)], dtype=np.int64)
probe_true = np.array([f_smooth_idx(ix) for ix in probe_idx])
probe_cross = np.array([np_tt_eval(cr_cores, ix) for ix in probe_idx])
cr_probe_err = np.abs(probe_cross - probe_true).max() / np.abs(probe_true).max()
check("cross mirror: probe rel err small", cr_probe_err < 1e-7, f"{cr_probe_err:.3e}")
# second seed (the C++ Philox starts differ from numpy's): design must be start-robust
cr2 = np_tt_cross(f_smooth_idx, (SN,) * SD, CR_RANK, CR_SWEEPS, CR_TOL, seed=1234, val_size=CR_VAL)
check("cross mirror: seed-robust", cr2[4], f"val_err {cr2[1]:.3e}")
cross_gate = 1e-7  # frozen: >=100x margin over both mirror runs

# tntorch cross reference on the SAME function/budget (recorded for the bench doc)
def f_torch(x1, x2, x3, x4):
    return 1.0 / torch.sqrt(SOFT + x1 * x1 + x2 * x2 + x3 * x3 + x4 * x4)


domain = [torch.tensor(sx)] * SD
t_cr, info = tn.cross(function=f_torch, domain=domain, ranks_tt=CR_RANK, max_iter=CR_SWEEPS,
                      eps=CR_TOL, val_size=CR_VAL, verbose=False, return_info=True,
                      suppress_warnings=True)
tnt_cr_err = float(info["val_epss"][-1])
print(f"[ref] tntorch cross: val_err {tnt_cr_err:.3e} evals {info['nsamples']} "
      f"ranks {[int(x) for x in t_cr.ranks_tt[1:-1]]}")

# ===========================================================================
# emit tests/hesap-tensor/ref_tt.inc
# ===========================================================================
root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
out_path = os.path.join(root, "tests", "hesap-tensor", "ref_tt.inc")


def arr_u64(name, vals):
    return f"inline constexpr crd::u64 {name}[{len(vals)}] = {{{', '.join(str(int(v)) + 'ULL' for v in vals)}}};\n"


def arr_f64(name, vals):
    return f"inline constexpr double {name}[{len(vals)}] = {{{', '.join(repr(float(v)) for v in vals)}}};\n"


with open(out_path, "w", newline="\n") as f:
    f.write("// ref_tt.inc — v14-k TT oracle expectations. GENERATED by scripts/v14k_tt_oracle.py")
    f.write(" (2026-07-05). DO NOT EDIT.\n")
    f.write("// Reference: tntorch 1.1.2 (torch 2.12.0+cpu) + numpy mirrors of the ported algorithms.\n")
    f.write("// maxvol source of truth: tntorch/maxvol.py py_maxvol (verbatim from A. Mikhalev's maxvolpy;\n")
    f.write("// the Goreinov-Oseledets 'How to Find a Good Submatrix' algorithm).\n")
    f.write("// ttpy: N/A-WITH-CHECK — pip install fails on py3.12 (setup.py imports numpy.distutils,\n")
    f.write("// removed from numpy on py>=3.12); attempted 2026-07-05.\n\n")
    f.write("// --- maxvol known answer: 16x4 integer-hash matrix, tol=1.05, max_iters=100 ---\n")
    f.write(arr_u64("kTtRefMaxvolRows", mv_sorted))
    f.write(f"inline constexpr crd::u64 kTtRefMaxvolIters = {mv_iters}ULL;\n\n")
    f.write("// --- Hilbert 4D 12^4: A[i,j,k,l] = 1/(1+i+j+k+l) ---\n")
    f.write(f"inline constexpr crd::u64 kTtRefHilbertN = {HN}ULL;\n")
    f.write(arr_f64("kTtRefHilbertEps", h_eps))
    for e, r, err in zip(h_eps, h_ranks, h_errs):
        tag = f"{e:.0e}".replace("-", "m").replace("1e", "E")
        f.write(arr_u64(f"kTtRefHilbertRanks{tag}", r))
        f.write(f"inline constexpr double kTtRefHilbertErr{tag} = {float(err)!r};\n")
    f.write("\n// per-point gate at eps=1e-8 (tntorch TT values; gate calibrated 100x the\n")
    f.write("// mirror-vs-tntorch gap — truncation subspaces match, rounding noise differs)\n")
    f.write(arr_u64("kTtRefEvalIdxFlat", eval_idx.reshape(-1)))
    f.write(arr_f64("kTtRefEvalTt", pts_tnt))
    f.write(arr_f64("kTtRefEvalTrue", pts_true))
    f.write(f"inline constexpr double kTtRefEvalGate = {eval_gate!r};\n\n")
    f.write("// --- exact low-TT-rank 8^4 ranks (3,3,3), integer-hash cores ---\n")
    f.write(f"inline constexpr crd::u64 kTtRefLowrankN = {LN}ULL;\n")
    f.write(f"inline constexpr crd::u64 kTtRefLowrankRank = {LR}ULL;\n")
    f.write(f"inline constexpr double kTtRefLowrankNorm = {lr_norm!r};\n\n")
    f.write("// --- smooth 4D gravitational-like kernel 16^4: f = 1/sqrt(0.1 + sum x_k^2),\n")
    f.write("//     x_k = -1 + 2i/15 (bit-exact formula both sides) ---\n")
    f.write(f"inline constexpr crd::u64 kTtRefSmoothN = {SN}ULL;\n")
    f.write(f"inline constexpr double kTtRefSmoothSoft = {SOFT!r};\n")
    f.write(f"inline constexpr double kTtRefSmoothSvdEps = {s_eps!r};\n")
    f.write(arr_u64("kTtRefSmoothSvdRanks", smooth_svd_ranks))
    f.write(f"inline constexpr double kTtRefSmoothSvdErr = {float(err_s)!r};\n\n")
    f.write("// --- TT-cross frozen budget + gate on the smooth function ---\n")
    f.write(f"inline constexpr crd::u64 kTtRefCrossRank = {CR_RANK}ULL;\n")
    f.write(f"inline constexpr crd::u64 kTtRefCrossSweeps = {CR_SWEEPS}ULL;\n")
    f.write(f"inline constexpr crd::u64 kTtRefCrossValSize = {CR_VAL}ULL;\n")
    f.write(f"inline constexpr double kTtRefCrossTol = {CR_TOL!r};\n")
    f.write(f"inline constexpr double kTtRefCrossGate = {cross_gate!r};  // probe rel-err gate\n")
    f.write(f"inline constexpr double kTtRefCrossMirrorErr = {float(cr_err)!r};\n")
    f.write(f"inline constexpr double kTtRefCrossTntorchErr = {tnt_cr_err!r};\n")
    f.write(f"inline constexpr crd::u64 kTtRefCrossMirrorEvals = {cr_evals}ULL;\n")
    f.write(arr_u64("kTtRefCrossProbeIdxFlat", probe_idx.reshape(-1)))
    f.write(arr_f64("kTtRefCrossProbeTrue", probe_true))

print(f"\nwrote {out_path}")
if FAIL:
    print("ORACLE FAILURES:", FAIL)
    sys.exit(1)
print("ORACLE ALL GREEN")
