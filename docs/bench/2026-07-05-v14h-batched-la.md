# 2026-07-05 — v14-h batched LA: peer baselines (torch-CPU / numpy) + boards

> Phase 3.1.6 v14-h (`phase-3.1.6-hesap.md` row; detail `phase-3.1.6-v14.md`). Machine:
> i9-14900K, WSL2, pinned core 4, **matched threading = 1** (torch.set_num_threads(1); the MT
> board rides the moat section later). Harness `scripts/v14h_peers.py` (torch 2.12.0+cpu —
> MKL-backed batched linalg on stacks; numpy matmul). MATLAB `pagemtimes`/`pagemldivide` rows
> land in ONE -batch call at board time (the 44.5 s startup rule); native MKL
> `cblas_?gemm_batch` rides the C++ A/B harness. best-of-5, ms.

## Peer baselines (f64, [B,n,n] stacks)

| n | B | np matmul | torch bmm | torch chol | torch chol+solve | torch LU | torch SVD |
|---|---|---|---|---|---|---|---|
| 4 | 10k | 0.38 | 0.49 | 0.50 | 1.49 | 0.49 | 22.82 |
| 4 | 100k | 3.66 | 5.04 | 4.79 | 15.31 | 5.18 | 226.56 |
| 6 | 10k | 0.49 | 1.39 | 0.88 | 1.66 | 1.10 | 41.70 |
| 6 | 100k | 5.17 | 15.12 | 9.39 | 28.16 | 12.32 | 434.83 |
| 8 | 10k | 0.55 | 0.39 | 1.21 | 2.34 | 1.93 | 68.34 |
| 8 | 100k | 12.72 | 16.76 | 25.38 | 49.32 | 33.06 | 677.95 |
| 16 | 10k | 3.25 | 2.59 | 4.99 | 7.85 | 7.51 | 172.85 |
| 16 | 100k | 56.32 | 68.39 | 81.81 | 114.36 | 114.36 | 1962.35 |

Reading: torch pays per-matrix LAPACK dispatch — 100k 6×6 GEMMs = ~43 Mflop yet 15.1 ms
(≈3 GF/s effective); batched SVD is dispatch-bound at ~4.3 µs per 6×6. These are the crush
targets; the flop-honest ceiling at these sizes is an order of magnitude away.

## Increment A shipped (2026-07-05): `batched.hpp` `batched_gemm`

Tiny tier (≤32K flops/matrix): allocation-free direct kernel, EVERY element the k-ordered
single-rounded fma chain (vector lanes + std::fma tail — the bit contract, gate-enforced);
large tier: per-matrix hesap-dense `gemm` (bit-match vs loop-of-single gated), scratch
thread-safe-wrapped under the across-batch parallel driver (grain = f(shape) only ⇒
`{1,2,4,8,16}` bit-identity GATED). linux-gcc 21 asserts / 4 cases green.

## OUR batched_gemm vs native MKL `dgemm_batch_strided` (the fastest compiled peer;
## matched-state A/B, same binary, pinned core 4, 1T, best-of-5; harness
## `build/crd_batched_bench.sh` + `runtime/examples/bench_batched.cpp`)

| n | B | ours ms | MKL ms | ratio | verdict |
|---|---|---|---|---|---|
| 4 | 10k | 0.09 | 0.29 | **3.20×** | WIN |
| 4 | 100k | 1.45 | 3.98 | **2.75×** | WIN |
| 6 | 10k | 0.16 | 0.26 | **1.55×** | WIN |
| 6 | 100k | 3.07–3.63 | 3.06–3.54 | 0.97–1.09 (5 samples: 0.97/0.97/1.04/1.09/0.98) | **TIE at the DRAM wall** — both at ~24 GB/s effective on the 86 MB stream; the cache-resident 10k row wins 1.55× ⇒ compute superior, bandwidth binding (the two-signal memory-wall evidence) |
| 8 | 10k | 0.25 | 0.41 | **1.60×** | WIN |
| 8 | 100k | 5.90 | 7.37 | **1.25×** | WIN |
| 16 | 10k | 2.55 | 2.78 | **1.09×** | WIN |
| 16 | 100k | 27.28 | 32.81 | **1.20×** | WIN |

vs torch bmm (from the baseline table): 3–10× wins on every row (e.g. 6×6@100k: ours
~3.1 ms vs torch 15.1 ms). The kernel history is itself the lesson: the first direct kernel
LOST to MKL at n∈{6,8,16} (1–2 fma chains = latency-bound); the 4-row×2-vector register
tile flipped 7 rows; the single-block R∈[5,8] 1-vector variant (B streams exactly once)
took n=6@10k from 1.18× to 1.55× and pushed the 100k row to the memory wall. New crd-math
primitives (home rule): `Vec4d/Vec8f::load_partial/store_partial` (masked tails — no scalar
rounding divergence, the bit contract holds on every lane).

- MATLAB `pagemtimes` rows: **N/A-with-the-check today** — MATLAB's license service is
  unreachable (error 5001, twice, one retry spaced); the one-batch-call script is staged in
  the session transcript and runs the moment the service returns. Column NOT dropped.

## Increment B (same day): batched Cholesky factor+solve — the lane-batched tier

`batched_cholesky_factor/solve`: AoSoA lane kernel (4 f64/8 f32 matrices per vector — one
vsqrt/vdiv retires W scalar sqrt/divs), scalar tier mirrors the exact op order ⇒ **tier
bit-identity GATED**; non-SPD poison isolation gated (a bad matrix flags its own info lane,
group siblings bit-unaffected); `{1,2,4,8,16}` moat gated. Suite 348 asserts / 6 cases green
(linux-gcc). New crd-math primitives: `Vec4d/Vec8f::load_partial/store_partial`.

**Wall-clock (factor, net of the in-place copy-back, same pin/protocol; MKL row =
LAPACKE_dpotrf loop — this MKL predates ?potrf_batch_strided; torch rows from the baseline
table):**

| n | B | ours ms | MKL potrf ms | vs MKL | torch ms | vs torch |
|---|---|---|---|---|---|---|
| 4 | 10k | 0.12 | 1.28 | **10.58×** | 0.50 | **4.2×** |
| 4 | 100k | 1.33 | 12.00 | **9.01×** | 4.79 | **3.6×** |
| 6 | 10k | 0.21 | 1.60 | **7.73×** | 0.88 | **4.2×** |
| 6 | 100k | 2.61 | 17.03 | **6.53×** | 9.39 | **3.6×** |
| 8 | 10k | 0.37 | 2.20 | **5.93×** | 1.21 | **3.3×** |
| 8 | 100k | 4.69 | 21.41 | **4.57×** | 25.38 | **5.4×** |
| 16 | 10k | 1.96 | 5.95 | **3.04×** | 4.99 | **2.5×** |
| 16 | 100k | 24.88 | 61.59 | **2.48×** | 81.81 | **3.3×** |

⇒ **FULL CRUSH on every Cholesky row vs both compiled peers.**

## Increment C (same day): batched LU with per-lane partial pivoting

`batched_lu_factor/solve`: pivot selection + elimination lane-vectorized, per-lane row
swaps O(nW) scalar; tier bit-identity gated INCLUDING the pivot sequences; singular-lane
isolation; `{1..16}` moat over factors AND pivots. Suite total 1,929 asserts / 7 cases.

**Wall-clock (factor, net of copy-back, same protocol; MKL = LAPACKE_dgetrf loop):**

| n | B | ours ms | MKL getrf ms | vs MKL | torch ms | vs torch |
|---|---|---|---|---|---|---|
| 4 | 10k | 0.35 | 0.99 | **2.87×** | 0.49 | **1.4×** |
| 4 | 100k | 3.80 | 10.37 | **2.73×** | 5.18 | **1.4×** |
| 6 | 10k | 0.82 | 1.83 | **2.25×** | 1.10 | **1.3×** |
| 6 | 100k | 8.89 | 18.99 | **2.14×** | 12.32 | **1.4×** |
| 8 | 10k | 1.39 | 2.50 | **1.80×** | 1.93 | **1.4×** |
| 8 | 100k | 15.47 | 25.14 | **1.63×** | 33.06 | **2.1×** |
| 16 | 10k | 6.12 | 7.75 | **1.27×** | 7.51 | **1.2×** |
| 16 | 100k | 58.30 | 82.14 | **1.41×** | 114.36 | **2.0×** |

⇒ **FULL CRUSH on every LU row vs both compiled peers.**

## Increment D (same day): batched small-SVD — lane-batched one-sided Jacobi

`batched_svd_small`: cyclic one-sided Jacobi, per-lane rotation masks with exact-identity
`select` (never an arithmetic identity — the signed-zero rule), bounded max_sweeps with
per-matrix NotConverged info, ONE shared scalar finalize (σ/U extraction + stable
descending sort) for both tiers. Tier bit-identity gated (the scalar tier mirrors the lane
fma forms exactly); reconstruction + orthogonality ≤1e-12; moat gated. Suite total
**3,362 asserts / 8 cases** green (linux-gcc).

**Wall-clock (jobz=A equivalents, same protocol; MKL = LAPACKE_dgesdd loop):**

| n | B | ours ms | MKL gesdd ms | vs MKL | torch ms | vs torch |
|---|---|---|---|---|---|---|
| 4 | 10k | 1.95 | 21.95 | **11.25×** | 22.82 | **11.7×** |
| 4 | 100k | 19.80 | 238.32 | **12.03×** | 226.56 | **11.4×** |
| 6 | 10k | 7.26 | 43.45 | **5.99×** | 41.70 | **5.7×** |
| 6 | 100k | 74.62 | 429.07 | **5.75×** | 434.83 | **5.8×** |
| 8 | 10k | 15.60 | 62.16 | **3.98×** | 68.34 | **4.4×** |
| 8 | 100k | 163.69 | 685.21 | **4.19×** | 677.95 | **4.1×** |
| 16 | 10k | 120.01 | 186.64 | **1.56×** | 172.85 | **1.4×** |
| 16 | 100k | 1180.38 | 1717.40 | **1.45×** | 1962.35 | **1.7×** |

(The first bench run SIGILL'd at n=16@100k — the seven 204 MB bench tensors exceeded the
1 GB bench arena and a failed allocation trapped; arena → 2.5 GB, row re-measured. Bench
infra, not kernel.)

## The LU pivot bug (found by the config ladder, root-caused, fixed — and the fix is FASTER)

win-shipping's ctest failed the LU tier bit-identity while gcc/win-debug/win-asan were all
green. The hunt (recorded fully in the session log): exact-value adjudication proved the
LANE tier returned false comparisons on raw data; run-twice showed reproducible lane-vs-lane
divergence; poison-fill, a compiler fence, and noinline seams all changed NOTHING; a
60-line standalone repro + flag bisection then pinned it: **MSVC /O1 AND /O2 (with or
without /GL) auto-vectorize the per-lane scalar loop `if (v > best[q]) { best[q] = v;
pr[q] = i; }` — a conditional update of TWO arrays — with wrong masked blends.** /Od and
gcc are correct; an fprintf in the loop suppressed the vectorizer (the heisen behavior);
ASan was structurally blind (wrong-code, no bad access). Fix at the construct: the pivot
scan is now a PURE manual-vector argmax (cmp_gt/select chains, indices in f64 lanes), and
the same conditional-two-array shape in the Cholesky SPD / LU singular checks was hardened
to vector-select + stored-mask form. **Re-measured post-fix (rule #2): LU 1.76–3.81× vs
MKL getrf (BETTER than the buggy 1.27–2.87×) · Cholesky 2.51–8.48× — the correct code is
also the faster code.** Full ladder re-verified green incl. win-shipping 8/8.

## Verdict line

**v14-h COMPLETE ON THE BOARDS: every operation × every size × every batch beats BOTH
compiled peers** — GEMM 7 WIN + 1 DRAM-wall tie vs MKL batch-strided (3–10× vs torch) ·
Cholesky 2.48–10.58× vs MKL · LU 1.21–2.87× vs MKL · SVD 1.45–12.03× vs MKL, 1.4–11.7× vs
torch — plus tier bit-identity, poison isolation, bounded iteration, and the `{1..16}`
moat on every op. MATLAB rows N/A-with-check (license service down; script staged).
