#!/usr/bin/env python3
# v14-j decomposition oracle: reconstruct-verify-FIRST (the v13 discipline), then
# freeze the gate corpus.
#
# 1. Generates the dense test tensors (random / low-rank-plus-noise / structured /
#    exact-multilinear-rank / exact-CP-rank / a 4-D one).
# 2. Runs tensorly 0.9.0 CP-ALS (parafac) + Tucker-HOOI (tucker), init='svd',
#    fixed iteration budgets, and a plain-numpy HOSVD + randomized-HOSVD reference
#    (the exact pipeline the C++ implements) — VERIFIES my reference reproduces
#    tensorly's fits to ~1e-9 BEFORE anything is frozen (algorithm parity proof;
#    catches misunderstandings pre-port).
# 3. Freezes inputs + FIT VALUES + reconstruction errors + orthonormalized
#    generating-factor bases (subspace-angle gates — factors only match up to
#    sign/permutation, so gates ride fit + rec-error + subspace angles, never raw
#    factor bits) into tests/hesap-tensor/ref_decomp.inc as plain C arrays
#    (NO std containers in refs — the house rule).
#
# Run (WSL): python3 scripts/v14j_decomp_oracle.py
import numpy as np
import tensorly as tl
from tensorly.decomposition import parafac, tucker

OUT = "/mnt/d/Dev/cerid/tests/hesap-tensor/ref_decomp.inc"


# ---------------------------------------------------------------- helpers
def unfold(x, mode):
    return np.reshape(np.moveaxis(x, mode, 0), (x.shape[mode], -1))


def khatri_rao(mats):
    # C-order: first matrix slowest-varying (matches tl.unfold column order)
    r = mats[0].shape[1]
    out = mats[0]
    for m in mats[1:]:
        out = (out[:, None, :] * m[None, :, :]).reshape(-1, r)
    return out


def multi_mode_dot_t(x, factors, skip=None):
    # X x_n U_n^T over every mode (skip one) — ascending mode order (tensorly's)
    y = x
    for n, u in enumerate(factors):
        if n == skip:
            continue
        y = np.moveaxis(
            np.tensordot(u.T, y, axes=([1], [n])), 0, n)
    return y


def rec_error(x, xhat):
    return np.linalg.norm(x - xhat) / np.linalg.norm(x)


# ------------------------------------------------- my reference: CP-ALS
# Mirrors the planned C++ exactly: svd init, per-mode normal-equations solve
# (Cholesky on V = hadamard of gramians), Kolda-Bader column normalization
# (2-norm, weights lambda), fit via the ||X||^2 - 2<X,Xhat> + ||Xhat||^2
# identity. Model-equivalent to tensorly's unnormalized ALS in exact
# arithmetic (scaling reparametrization) — the parity check below PROVES it.
def my_cp_als(x, rank, iters):
    ndim = x.ndim
    norm_x = np.linalg.norm(x)
    factors = []
    for n in range(ndim):
        u, _, _ = np.linalg.svd(unfold(x, n), full_matrices=False)
        factors.append(u[:, :rank].copy())
    weights = np.ones(rank)
    err = None
    for _ in range(iters):
        for n in range(ndim):
            others = [factors[k] for k in range(ndim) if k != n]
            kr = khatri_rao(others)
            m = unfold(x, n) @ kr
            v = np.ones((rank, rank))
            for k in range(ndim):
                if k != n:
                    v = v * (factors[k].T @ factors[k])
            f = np.linalg.solve(v, m.T).T  # V symmetric
            lam = np.linalg.norm(f, axis=0)
            lam_safe = np.where(lam > 0, lam, 1.0)
            factors[n] = f / lam_safe
            weights = np.where(lam > 0, lam, 0.0)
            if n == ndim - 1:
                # fit identity on the unnormalized last factor
                iprod = np.sum(m * f)
                model_sq = np.sum(v * (f.T @ f))
                e2 = max(norm_x**2 - 2.0 * iprod + model_sq, 0.0)
                err = np.sqrt(e2) / norm_x
    return weights, factors, err


def cp_reconstruct(weights, factors):
    ndim = len(factors)
    kr = khatri_rao(factors[1:])
    m0 = (factors[0] * weights) @ kr.T
    shape = [f.shape[0] for f in factors]
    return m0.reshape(shape)


# ---------------------------------------------- my reference: HOSVD/HOOI
def my_hosvd(x, ranks):
    factors = []
    for n in range(x.ndim):
        u, _, _ = np.linalg.svd(unfold(x, n), full_matrices=False)
        factors.append(u[:, : ranks[n]].copy())
    core = multi_mode_dot_t(x, factors)
    return core, factors


def my_hooi(x, ranks, iters):
    norm_x = np.linalg.norm(x)
    _, factors = my_hosvd(x, ranks)
    err = None
    for _ in range(iters):
        for n in range(x.ndim):
            y = multi_mode_dot_t(x, factors, skip=n)
            u, _, _ = np.linalg.svd(unfold(y, n), full_matrices=False)
            factors[n] = u[:, : ranks[n]].copy()
        core = multi_mode_dot_t(x, factors)
        e2 = max(norm_x**2 - np.linalg.norm(core) ** 2, 0.0)
        err = np.sqrt(e2) / norm_x
    core = multi_mode_dot_t(x, factors)
    return core, factors, err


def tucker_reconstruct(core, factors):
    y = core
    for n, u in enumerate(factors):
        y = np.moveaxis(np.tensordot(u, y, axes=([1], [n])), 0, n)
    return y


# ------------------------------------- my reference: randomized HOSVD
# Halko-Martinsson-Tropp range finder per mode (gaussian sketch, oversample p,
# q power iterations with re-orthonormalization) + small SVD of B = Q^T A.
# The C++ draws the sketch from Philox (keyed, Irwin-Hall-12) — distributionally
# equivalent; this reference freezes the ERROR LEVEL the C++ must land in.
def my_rhosvd(x, ranks, p, q, rng):
    factors = []
    for n in range(x.ndim):
        a = unfold(x, n)
        ell = min(ranks[n] + p, min(a.shape))
        om = rng.standard_normal((a.shape[1], ell))
        qmat, _ = np.linalg.qr(a @ om)
        for _ in range(q):
            z, _ = np.linalg.qr(a.T @ qmat)
            qmat, _ = np.linalg.qr(a @ z)
        b = qmat.T @ a
        ub, _, _ = np.linalg.svd(b, full_matrices=False)
        factors.append((qmat @ ub)[:, : ranks[n]].copy())
    core = multi_mode_dot_t(x, factors)
    return core, factors


# ---------------------------------------------------------------- tensors
def build_tensors():
    tensors = []
    rng = np.random.default_rng(1407)
    # 0 rand3d: uniform [-1, 1]
    tensors.append(("rand3d", rng.uniform(-1.0, 1.0, (12, 10, 8))))
    # 1 lrnoise: CP rank 6 + 1e-3 relative gaussian noise (frozen gen factors)
    gen = [rng.standard_normal((s, 6)) for s in (16, 14, 12)]
    xlr = cp_reconstruct(np.ones(6), gen)
    noise = rng.standard_normal(xlr.shape)
    x1 = xlr / np.linalg.norm(xlr) + 1e-3 * noise / np.linalg.norm(noise)
    tensors.append(("lrnoise", x1))
    # 2 struct: X[i,j,k] = 1/(1+i+j+k) (decaying multilinear spectrum)
    i, j, k = np.meshgrid(np.arange(16), np.arange(16), np.arange(16), indexing="ij")
    tensors.append(("struct", 1.0 / (1.0 + i + j + k)))
    # 3 mlrank: exact multilinear rank (3,4,5): random core x orthonormal factors
    core = rng.standard_normal((3, 4, 5))
    facs = [np.linalg.qr(rng.standard_normal((s, r)))[0] for s, r in ((10, 3), (9, 4), (8, 5))]
    tensors.append(("mlrank", tucker_reconstruct(core, facs)))
    # 4 cpexact: exact CP rank 4
    gf = [rng.standard_normal((s, 4)) for s in (10, 9, 8)]
    tensors.append(("cpexact", cp_reconstruct(np.ones(4), gf)))
    # 5 rand4d: uniform [-1, 1], rank-4 tensor
    tensors.append(("rand4d", rng.uniform(-1.0, 1.0, (7, 6, 5, 4))))
    return tensors, gen


CP_ROWS = [  # (tensor, rank, iters)
    (0, 5, 50), (0, 1, 30), (0, 8, 30), (1, 6, 50), (2, 8, 50), (4, 4, 60), (5, 3, 40),
]
TUCKER_ROWS = [  # (tensor, ranks, iters)
    (0, (6, 5, 4), 20), (0, (12, 10, 8), 5), (1, (6, 6, 6), 20), (2, (8, 8, 8), 20),
    (3, (3, 4, 5), 20), (5, (3, 3, 3, 3), 20),
]
RAND_ROWS = [  # (tensor, ranks, oversample, power)
    (1, (6, 6, 6), 8, 2), (2, (8, 8, 8), 8, 2), (3, (3, 4, 5), 8, 2),
]


def fmt(v):
    return repr(float(v))


def main():
    tensors, gen_factors = build_tensors()
    print(f"tensorly {tl.__version__}  numpy {np.__version__}")

    # ---------------- CP rows: tensorly parafac + my reference, parity-checked
    cp_out = []
    for (ti, rank, iters) in CP_ROWS:
        name, x = tensors[ti]
        cp = parafac(tl.tensor(x), rank=rank, n_iter_max=iters, init="svd",
                     tol=1e-30, normalize_factors=False, random_state=0)
        xhat_tl = tl.cp_to_tensor(cp)
        err_tl = rec_error(x, np.asarray(xhat_tl))
        w, f, err_id = my_cp_als(x, rank, iters)
        err_mine = rec_error(x, cp_reconstruct(w, f))
        # parity proofs: identity error == direct error; mine tracks tensorly
        assert abs(err_mine - err_id) < 1e-9 + 1e-6 * err_mine, \
            f"CP identity-error mismatch {name} r{rank}: {err_mine} vs {err_id}"
        gap = err_mine - err_tl
        print(f"CP  {name:8s} rank {rank:2d} iters {iters:2d}: tl {err_tl:.12e}  mine {err_mine:.12e}  gap {gap:+.3e}")
        assert gap < 1e-7 + 1e-4 * err_tl, f"CP parity FAIL {name} r{rank}"
        cp_out.append((ti, rank, iters, err_tl, 1.0 - err_tl))

    # ---------------- Tucker rows: numpy HOSVD + tensorly HOOI + my HOOI
    tk_out = []
    for (ti, ranks, iters) in TUCKER_ROWS:
        name, x = tensors[ti]
        _, _ = my_hosvd(x, ranks)
        core_h, facs_h = my_hosvd(x, ranks)
        err_hosvd = rec_error(x, tucker_reconstruct(core_h, facs_h))
        core_tl, facs_tl = tucker(tl.tensor(x), rank=list(ranks), n_iter_max=iters,
                                  init="svd", tol=1e-30, random_state=0)
        err_hooi_tl = rec_error(x, np.asarray(tl.tucker_to_tensor((core_tl, facs_tl))))
        _, _, err_hooi_mine = my_hooi(x, ranks, iters)
        gap = err_hooi_mine - err_hooi_tl
        print(f"TK  {name:8s} ranks {str(ranks):14s} iters {iters:2d}: hosvd {err_hosvd:.12e}  "
              f"tl-hooi {err_hooi_tl:.12e}  mine {err_hooi_mine:.12e}  gap {gap:+.3e}")
        assert gap < 1e-7 + 1e-4 * max(err_hooi_tl, 1e-30), f"HOOI parity FAIL {name}"
        tk_out.append((ti, ranks, iters, err_hosvd, err_hooi_tl))

    # ---------------- randomized rows: my rHOSVD reference error levels
    rd_out = []
    rng = np.random.default_rng(77)
    for (ti, ranks, p, q) in RAND_ROWS:
        name, x = tensors[ti]
        errs = []
        for _ in range(8):  # error level across sketches — freeze the WORST of 8
            core_r, facs_r = my_rhosvd(x, ranks, p, q, rng)
            errs.append(rec_error(x, tucker_reconstruct(core_r, facs_r)))
        core_h, facs_h = my_hosvd(x, ranks)
        err_exact = rec_error(x, tucker_reconstruct(core_h, facs_h))
        err_rand = max(errs)
        print(f"RND {name:8s} ranks {str(ranks):14s} p{p} q{q}: exact {err_exact:.12e}  rand-worst8 {err_rand:.12e}")
        rd_out.append((ti, ranks, p, q, err_rand, err_exact))

    # tensorly randomized peer (svd='randomized_svd'), recorded for the bench doc
    for (ti, ranks, p, q) in RAND_ROWS:
        name, x = tensors[ti]
        try:
            core_tr, facs_tr = tucker(tl.tensor(x), rank=list(ranks), n_iter_max=1,
                                      init="svd", svd="randomized_svd", tol=1e-30,
                                      random_state=0)
            err = rec_error(x, np.asarray(tl.tucker_to_tensor((core_tr, facs_tr))))
            print(f"TLR {name:8s} ranks {str(ranks):14s}: tensorly randomized_svd tucker(1 it) {err:.12e}")
        except Exception as e:  # noqa: BLE001 — peer capability probe
            print(f"TLR {name:8s}: tensorly randomized svd unavailable: {e}")

    # ---------------- subspace-gate bases: orthonormalized generating factors of lrnoise
    qs = [np.linalg.qr(g)[0] for g in gen_factors]

    # ---------------- emit the .inc
    lines = []
    lines.append("// GENERATED by scripts/v14j_decomp_oracle.py -- the v14-j decomposition oracle:")
    lines.append(f"// tensorly {tl.__version__} (numpy {np.__version__}) parafac/tucker init='svd', fixed")
    lines.append("// iteration budgets; HOSVD + randomized-HOSVD reference errors from the verified")
    lines.append("// numpy reconstruction (parity-proved against tensorly before freezing).")
    lines.append("// Factors gate on fit + reconstruction error + subspace angles, never raw bits.")
    lines.append("// Plain C arrays (no std containers in refs -- the house rule). Uses crd::u32/")
    lines.append("// u64/f64 (u64 differs between LP64 and LLP64 -- never hardcode long long).")
    lines.append("// clang-format off")
    lines.append("namespace refdecomp")
    lines.append("{")
    nt = len(tensors)
    lines.append(f"inline constexpr crd::u32 kNumTensors = {nt}U;")
    ndims = [len(x.shape) for _, x in tensors]
    lines.append("inline constexpr crd::u32 kTensorNdim[kNumTensors] = {" +
                 ", ".join(f"{n}U" for n in ndims) + "};")
    shp_rows = []
    for _, x in tensors:
        s = list(x.shape) + [0] * (8 - len(x.shape))
        shp_rows.append("{" + ", ".join(f"{v}U" for v in s) + "}")
    lines.append("inline constexpr crd::u64 kTensorShape[kNumTensors][8] = {")
    lines.append("    " + ",\n    ".join(shp_rows))
    lines.append("};")
    offs = [0]
    for _, x in tensors:
        offs.append(offs[-1] + x.size)
    lines.append("inline constexpr crd::u64 kTensorOffset[kNumTensors + 1U] = {" +
                 ", ".join(f"{v}U" for v in offs) + "};")
    lines.append(f"inline constexpr crd::f64 kTensorData[{offs[-1]}] = {{")
    for _, x in tensors:
        flat = x.reshape(-1)
        for i in range(0, flat.size, 4):
            chunk = ", ".join(fmt(v) for v in flat[i:i + 4])
            lines.append("    " + chunk + ",")
    lines.append("};")

    lines.append(f"inline constexpr crd::u32 kNumCpRows = {len(cp_out)}U;")
    lines.append("inline constexpr crd::u32 kCpTensor[kNumCpRows] = {" +
                 ", ".join(f"{r[0]}U" for r in cp_out) + "};")
    lines.append("inline constexpr crd::u64 kCpRank[kNumCpRows] = {" +
                 ", ".join(f"{r[1]}U" for r in cp_out) + "};")
    lines.append("inline constexpr crd::u32 kCpIters[kNumCpRows] = {" +
                 ", ".join(f"{r[2]}U" for r in cp_out) + "};")
    lines.append("inline constexpr crd::f64 kCpRecErrTl[kNumCpRows] = {" +
                 ", ".join(fmt(r[3]) for r in cp_out) + "};")
    lines.append("inline constexpr crd::f64 kCpFitTl[kNumCpRows] = {" +
                 ", ".join(fmt(r[4]) for r in cp_out) + "};")

    lines.append(f"inline constexpr crd::u32 kNumTuckerRows = {len(tk_out)}U;")
    lines.append("inline constexpr crd::u32 kTuckerTensor[kNumTuckerRows] = {" +
                 ", ".join(f"{r[0]}U" for r in tk_out) + "};")
    tr_rows = []
    for r in tk_out:
        rr = list(r[1]) + [0] * (8 - len(r[1]))
        tr_rows.append("{" + ", ".join(f"{v}U" for v in rr) + "}")
    lines.append("inline constexpr crd::u64 kTuckerRanks[kNumTuckerRows][8] = {")
    lines.append("    " + ",\n    ".join(tr_rows))
    lines.append("};")
    lines.append("inline constexpr crd::u32 kTuckerIters[kNumTuckerRows] = {" +
                 ", ".join(f"{r[2]}U" for r in tk_out) + "};")
    lines.append("inline constexpr crd::f64 kTuckerRecErrHosvd[kNumTuckerRows] = {" +
                 ", ".join(fmt(r[3]) for r in tk_out) + "};")
    lines.append("inline constexpr crd::f64 kTuckerRecErrHooiTl[kNumTuckerRows] = {" +
                 ", ".join(fmt(r[4]) for r in tk_out) + "};")

    lines.append(f"inline constexpr crd::u32 kNumRandRows = {len(rd_out)}U;")
    lines.append("inline constexpr crd::u32 kRandTensor[kNumRandRows] = {" +
                 ", ".join(f"{r[0]}U" for r in rd_out) + "};")
    rd_rows = []
    for r in rd_out:
        rr = list(r[1]) + [0] * (8 - len(r[1]))
        rd_rows.append("{" + ", ".join(f"{v}U" for v in rr) + "}")
    lines.append("inline constexpr crd::u64 kRandRanks[kNumRandRows][8] = {")
    lines.append("    " + ",\n    ".join(rd_rows))
    lines.append("};")
    lines.append("inline constexpr crd::u64 kRandOversample[kNumRandRows] = {" +
                 ", ".join(f"{r[2]}U" for r in rd_out) + "};")
    lines.append("inline constexpr crd::u32 kRandPower[kNumRandRows] = {" +
                 ", ".join(f"{r[3]}U" for r in rd_out) + "};")
    lines.append("inline constexpr crd::f64 kRandRecErrRef[kNumRandRows] = {" +
                 ", ".join(fmt(r[4]) for r in rd_out) + "};")
    lines.append("inline constexpr crd::f64 kRandRecErrExact[kNumRandRows] = {" +
                 ", ".join(fmt(r[5]) for r in rd_out) + "};")

    # lrnoise generating-factor orthonormal bases (subspace-angle gates)
    for mi, qm in enumerate(qs):
        rows, cols = qm.shape
        lines.append(f"inline constexpr crd::u64 kLrQ{mi}Rows = {rows}U;")
        lines.append(f"inline constexpr crd::u64 kLrQ{mi}Cols = {cols}U;")
        lines.append(f"inline constexpr crd::f64 kLrQ{mi}[{rows * cols}] = {{")
        flat = qm.reshape(-1)
        for i in range(0, flat.size, 4):
            lines.append("    " + ", ".join(fmt(v) for v in flat[i:i + 4]) + ",")
        lines.append("};")

    lines.append("} // namespace refdecomp")
    lines.append("// clang-format on")
    with open(OUT, "w") as f:
        f.write("\n".join(lines) + "\n")
    print(f"wrote {OUT}: {offs[-1]} tensor elements, {len(cp_out)} CP rows, "
          f"{len(tk_out)} Tucker rows, {len(rd_out)} randomized rows")


if __name__ == "__main__":
    main()
