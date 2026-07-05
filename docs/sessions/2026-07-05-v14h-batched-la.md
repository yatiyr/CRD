# 2026-07-05 — v14-h: batched LA (GEMM/Chol/LU/small-SVD) — full crush + the autovec scar

> Boards: **`docs/bench/2026-07-05-v14h-batched-la.md`**. Same-day siblings: the v10 FFT
> shipping-gate close (C1002) and the v14-g hyper-optimizer crush (their own logs). This log
> = the v14-h arc + the day's THIRD MSVC-optimizer scar, the nastiest.

## What shipped (`engine/hesap-tensor/include/crd/hesap/tensor/batched.hpp`, uncommitted)

- **`batched_gemm`** — tiny tier: register-tiled direct kernels (4-row×2-vector + the
  single-block R∈[5,8] 1-vector variant that streams B exactly once), EVERY element the
  k-ordered single-rounded fma chain (the bit contract, gate-enforced); large tier =
  per-matrix hesap-dense gemm (bit-match vs loop-of-single gated; scratch thread-safe
  wrapped). New crd-math primitives (home rule): `Vec4d/Vec8f::load_partial/store_partial`
  (masked vector tails).
- **`batched_cholesky_factor/solve`** — lane-batched AoSoA (W matrices/vector; one
  vsqrt/vdiv retires W scalar ones); scalar tier mirrors the exact op order ⇒ tier
  bit-identity GATED; non-SPD poison isolation gated.
- **`batched_lu_factor/solve`** — per-lane partial pivoting (pure-vector argmax selection —
  see the scar), tier bit-identity gated INCLUDING pivot sequences.
- **`batched_svd_small`** — cyclic one-sided Jacobi, per-lane rotation masks with
  exact-identity `select` (signed-zero-safe), bounded max_sweeps + per-matrix NotConverged
  info, ONE shared scalar finalize (σ/U + stable descending sort) for both tiers.
- All four ops: across-batch deterministic parallel driver (grain = f(shape) only) with the
  `{1,2,4,8,16}` moat gated bit-identical. Suite **3,362 asserts / 8 cases**.

## Boards (matched-state, pinned, 1T; MKL = the strongest compiled peer available per op)

GEMM 7 WIN + 1 DRAM-wall tie vs `cblas_dgemm_batch_strided` (3.2× best; 3–10× vs torch) ·
Cholesky 2.51–8.48× vs LAPACKE_dpotrf loop · LU 1.76–3.81× vs dgetrf loop · SVD
1.45–12.03× vs dgesdd loop — **every op × every size × every batch beats BOTH compiled
peers**; all rows beat torch (to 11.7×). MATLAB: license service down all day (error 5001,
retried) — rows N/A-with-the-check, one-batch-call script staged.

## The LU pivot bug — the day's third MSVC-optimizer scar (SANITY ledger 2026-07-05)

win-shipping ctest failed the LU tier bit-identity; gcc/debug/asan green. The hunt in full:
1. Exact-value adjudication vs raw data → the LANE tier's comparisons were provably false.
2. Run-twice on identical inputs → reproducible lane-vs-lane divergence (34 values).
3. Three theories MEASURED AND KILLED: stack UMR (poison-fill — no change), alias
   reordering (atomic_signal_fence — no change), LTCG inline mis-scheduling (noinline
   seams — no change). Each would have shipped as a false mechanism without the refutation.
4. A 60-line standalone repro + flag bisection pinned it: **MSVC /O1 AND /O2 (±/GL)
   auto-vectorize the per-lane conditional TWO-ARRAY update
   `if (v > best[q]) { best[q]=v; pr[q]=i; }` with wrong masked blends.** /Od + gcc
   correct; fprintf-in-loop suppressed it (the heisen phase); ASan structurally blind.
5. Root fix at the construct: pivot scan = pure manual-vector argmax (cmp/select chains,
   indices in f64 lanes); the same shape hardened in the chol-SPD/LU-singular checks
   (vector substitute + stored-mask flags). **Re-measured: the fix is FASTER (LU 1.27–2.87×
   → 1.76–3.81× vs MKL).** Rule extracted: per-lane scalar conditional multi-array updates
   are forbidden in lane kernels — express as select chains. Memory:
   `feedback_msvc_autovec_conditional_two_array_update`.

## En-route fixes

- Bench arena OOM at n=16@100k (seven 204 MB tensors > 1 GB arena; SIGILL via alloc-fail
  trap) — arena 2.5 GB, row re-measured. Bench infra, not kernel.
- `hesap-dense/src/svd.cpp`: two `std::sort` → `crd::containers::sort` (the 14.51 STL's new
  xutility code trips clang-tidy through std::sort's innards; also the determinism-doctrine
  alignment). Dense suite re-verified after the swap.

## Verification at close (the fixed artifact)

linux-gcc 3,362/8 ✓ · win-debug ctest 8/8 ✓ · win-asan 3,362/8 zero errors ✓ ·
**win-shipping ctest 8/8 ✓ (post-fix)** · win-tidy ✓ (after the svd.cpp sort swap) ·
hesap-dense suite re-verified post-swap (gcc + win-debug).

## Handoff

v14-h core = SHIPPED + crushing. Rows updated in BOTH phase docs (the two-homes rule).
MATLAB board rows land when the license service returns. Commit proposal in-chat (v10 +
v14-g + v14-h ride the user's next commit).
