# 2026-07-05 — v14-z: the ALL-PEERS SCOREBOARD (crd-hesap-tensor vs every measured gold standard)

> Phase 3.1.6 v14-z close artifact. This is a CONSOLIDATION — every number below is quoted verbatim
> from the linked source board (the single home for measurements, per `docs/bench/README.md`); nothing
> here was re-measured or recomputed except where a row explicitly says "computed from the board's own
> table" (and is then flagged in the audit notes). Machine for every board: i9-14900K, WSL2, pinned
> core(s), **matched threading** (peers pinned to the same thread count), best-of-N per the source doc.
> Honest-scoreboard rules apply: ties, losses, open cells, and N/A-with-the-check rows are all here.

## 1. dtype converts + deterministic SR (v14-a) — vs numpy / ml_dtypes / torch

Source: [2026-07-02-v14a-dtype-converts.md](2026-07-02-v14a-dtype-converts.md) (1M elems, 1 pinned core; numpy 2.4.6, ml_dtypes 0.5.4, torch 2.12.0+cpu).

| op | ours ns/elem | peer | ratio | verdict |
|---|---|---|---|---|
| f32→f16 | 0.099 | torch (F16C) 0.146 | **1.5×** | WIN |
| f32→f16 | 0.099 | numpy 1.217 | **12.3×** | WIN |
| f16→f32 | 0.103 | numpy 0.659 | **6.4×** | WIN |
| f32→bf16 | 0.120 | ml_dtypes 0.362 | **3.0×** | WIN |
| f32→bf16 | 0.120 | torch 0.491 | **4.1×** | WIN |
| f32→fp8 e4m3fn | 0.596 | ml_dtypes 1.356 | **2.3×** | WIN |
| f32→fp8 e5m2 | 0.604 | ml_dtypes (e4m3-class) | **~2.2×** | WIN |
| f32→f16 **deterministic SR** | 1.204 | — | — | **NO peer ships deterministic SR** (checked: numpy/ml_dtypes none; torch no CPU SR); cheaper than numpy's plain RNE f16 convert (1.217) |
| f32→bf16 / e4m3 SR | 0.860 / 1.193 | — | — | same — peerless capability |

Caveat (recorded, not silently dropped): after the 2026-07-02 migration of the convert primitives to
`crd/math/float_convert.hpp`, the phase doc's post-migration re-verification logged slightly different
values (f16 0.102 ns = 1.30× torch · f16→f32 0.106 · bf16 0.122 · e4m3 0.598 = 2.2× ml_dtypes) — same
verdicts, the bench board above was not re-issued post-migration. Q8_0/Q4_0 quantize peer comparison
deliberately landed at v14-m (per the locked table); parity was byte-exact-gated at v14-a.

## 2. Elementwise + broadcasting (v14-b) — vs numpy / torch

Source: [2026-07-02-v14b-elementwise-broadcast.md](2026-07-02-v14b-elementwise-broadcast.md) (1T pinned; numpy `out=`, torch 1 thread `out=`).

| case | ours ns/elem | numpy | torch | verdict |
|---|---|---|---|---|
| contiguous add f32 1M | 0.209 | 0.220 (**1.05×**) | 0.605 (**2.9×**) | WIN — the numpy edge is at the DRAM ceiling (both stream 12 B/elem) |
| outer broadcast mul f32 16M | 0.213 | **1.50×** | **1.68×** | WIN |
| row broadcast add f64 4M | 0.583 | **1.27×** | **1.49×** | WIN |
| strided-row (::2) mul f32 4M | 0.209 | **1.57×** | — (not measured) | WIN |

Zero losses; NumPy **bit-exact** corpus gates ride the same kernels.

## 3. Reductions — Tier D + Tier R (v14-c) — vs ReproBLAS

Source: [2026-07-02-v14c-reduce-vs-reproblas.md](2026-07-02-v14c-reduce-vs-reproblas.md) (f64 sum, cancellation workload, fold=3, same machine/pinning as the recorded ReproBLAS v2.1.0 oracle).

| N | Tier-R binned (ours) ns/elem | ReproBLAS rdsum(3) | verdict |
|---|---|---|---|
| 1M | 0.220 | 0.353–0.360 | **1.60–1.63× CRUSH** |
| 16M | 0.466–0.480 | 0.486–0.571 | **1.01–1.23× WIN** |

Tier-D fixed-tree (the default, `{1..16}`-bit-identical): 0.120 ns/elem @1M ≈ 3× the naive serial loop,
DRAM ceiling @16M. The reproducible partition-independent sum now costs LESS than a plain naive serial
sum at DRAM sizes. No other peer ships either tier (NumPy-MKL/PyTorch: thread-count-dependent bits).

## 4. Transpose/permute (v14-d) — vs HPTT

Source: [2026-07-02-v14d-permute-vs-hptt.md](2026-07-02-v14d-permute-vs-hptt.md) (HPTT SHA 9425386, `ESTIMATE` plans; 8T rows = matched-state A/B protocol, increment 4 final).

| case | protocol | ours | HPTT | verdict |
|---|---|---|---|---|
| 2D 4096² {1,0} f32 | 1T | 0.4949 ns/elem | 0.5505 | **1.11× WIN** |
| 4D 64⁴ {3,2,1,0} | 1T | 0.4180 | 0.4849 | **1.16× WIN** |
| 3D 512³ {2,0,1} | 1T | 0.5962 | 0.7828 | **1.31× WIN** |
| 2D 4096² | 8T matched-state A/B | 0.1527–0.1688 | 0.1743–0.1798 | **WIN both rounds (1.03–1.18×)** |
| 4D 64⁴ | 8T matched-state A/B | 0.1412–0.1600 | 0.1801–0.2344 | **WIN both rounds (1.28–1.47×)** |
| 3D 512³ | 8T matched-state A/B | 0.1667–0.1752 | 0.1557–0.1793 | **PARITY** (rounds split, ranges overlap) — was a consistent 0.82× loss, closed by full-column staged strips (increment 4) |

Plus: zero planning cost (HPTT plans per shape) and the `{1..16}` bit-identity HPTT does not carry.

## 5. einsum path optimizer (v14-e) — vs opt_einsum 3.4.0

Source: [2026-07-03-v14e-einsum-path-vs-opteinsum.md](2026-07-03-v14e-einsum-path-vs-opteinsum.md).

| axis | result |
|---|---|
| path quality, optimal vs their optimal (their reported metric) | **≤ on every one of the 33 oracle cases; strictly better where their search's internal inconsistency bites** (verified: their optimal search minimizes a different objective than its reported cost) |
| path quality, auto/greedy vs their greedy | **≤ on every case** |
| planning cost | **8.15 µs/plan = 9.0× their greedy (72.96), 41× their optimal (336.81)** — and `EinsumPlan` is build-once/execute-many where NumPy re-plans every call |

## 6. einsum execution (v14-f) — vs numpy / torch / TBLIS (TBLIS measured 2026-07-05 evening)

Source: [2026-07-03-v14f-einsum-exec-vs-numpy-torch.md](2026-07-03-v14f-einsum-exec-vs-numpy-torch.md) (f64, 1T matched, THE FINAL TABLE after the direct-kernel pass).

| case | ours µs | numpy | torch | verdict |
|---|---|---|---|---|
| `ij,jk->ik` @32 (plan reuse) | 1.86 | 7.32 (**3.9×**) | 6.73 (**3.6×**) | CRUSH |
| `ea,fb,abcd,gc,hd->efgh` @24 (TN) | 1496 | 2291 (**1.53×**) | 1596 (**1.07×**) | WIN |
| `abc,bad->dc` @96 | 3302 | 2931 (**0.89×**) | 4026 (**1.22×**) | torch WIN · **numpy sub-parity — OPEN** |
| `ab,bc,cd->ad` @512 (GEMM-bound) | 8154 | 7792 (**0.96×**) | 6725 (**0.82×**) | **OPEN both cells** |

The two sub-par columns share ONE pinned root (SANITY #9, named owner): **raw large-K f64 GEMM rate —
v0d ~65 GF/s vs OpenBLAS ~72 / MKL ~78 at 512-class shapes, 1T**; the einsum layer adds nothing to those
rows (copies eliminated; numpy pays the identical re-layout). The v0d block constants are bit-locked by
ADR-0063; sanctioned paths tracked as the engine-wide v0d GEMM row (see also
[2026-07-03-v0d-gemm-zeroinit-pass.md](2026-07-03-v0d-gemm-zeroinit-pass.md), which halved the gap
order-preservingly after this board).

**TBLIS: measured 2026-07-05 evening** (docs/bench/2026-07-05-v14z-tblis-xtensor.md): TTGT-heavy case WON 1.14×; pure-GEMM rows 0.95–0.98× = the named v0d GEMM-kernel gap. Original note: not installed at v14-f board time; the board deferred an oracle
build "with the v14-g close", which did not land during the slices; closed at the v14-z close instead.

## 7. einsum hyper-optimizer (v14-g) — vs cotengra 0.8.2 (+kahypar 1.3.5) / cotengrust

Source: [2026-07-05-v14g-hyperopt-oracle.md](2026-07-05-v14g-hyperopt-oracle.md) (C++ port boards §5 — the shipped, UAF-fixed artifact; ctest-enforced frozen corpus gate).

Quality (log10 flops, at-or-under their hq default greedy+kahypar @64 matched seeded trials, 6/6):

| network | ours | cotengra-hq | verdict |
|---|---|---|---|
| rand30 | **4.8942** | 4.8943 | WIN |
| rand60 | **7.8697** | 7.9993 | WIN |
| rand120 | **9.7879** | 9.8715 | WIN |
| rand200 | **12.2102** | 13.0768 | **WIN — 7.4× fewer flops** |
| lat8x8 | **5.3158** | 5.3188 | WIN |
| lat4x4x4 | **7.5823** | 7.5824 | WIN (the greedy optimum, matched to the last digit) |

Wall-clock, matched 64 trials, both serial (their kahypar runs C++ internally): **2.03× / 1.81× /
3.25× / 5.83× / 2.05× / 1.92× faster — AND better trees on every row** (no quality-for-speed trade).
Engine level vs **cotengrust (Rust, their compiled fastest)**: **2.17× / 1.80× / 1.07× / 1.33× /
2.08× / 1.60× faster** (quality deltas = the quantified ±0.2 seed noise). Slicer, matched-tree
protocol: **parity** (rand120 + lat4x4x4 IDENTICAL slice sets; rand200 hair better, 1.001 vs 1.003
overhead) — the memory bound honored EXACTLY on both sides, `NotFound`-never-best-effort on ours.
Structural moat cotengra lacks by construction: seeded counter-keyed reproducibility at any worker
count (their stack: unseeded optuna-TPE + global-rng trials + completion-order feedback).

## 8. Batched linear algebra (v14-h) — vs native MKL / torch / MATLAB

Source: [2026-07-05-v14h-batched-la.md](2026-07-05-v14h-batched-la.md) (f64 `[B,n,n]` stacks, n∈{4,6,8,16}, B∈{10k,100k}, 1T pinned matched-state A/B).
⚠ Protocol note: the chol/LU MKL ranges below are the board's **post-fix re-measured** values (the
MSVC-autovec LU-pivot fix — rule #2: pre-fix numbers re-measured; the fix was FASTER); the board's
increment-B/C tables predate the fix.

| op | vs MKL (strongest compiled peer per op) | vs torch | vs MATLAB |
|---|---|---|---|
| GEMM | `dgemm_batch_strided`: **7 WIN (1.09–3.20×) + 1 TIE at the DRAM wall** (6×6@100k: 0.97–1.09× across 5 samples — both at ~24 GB/s on the 86 MB stream; the cache-resident 10k row wins 1.55× ⇒ compute superior, bandwidth binding) | all 8 rows WIN (computed from the board's own baseline table: 1.02–8.7×; board prose says "3–10×" — see audit note 6) | `pagemtimes`: **8/8 WIN, 2.04–8.23×** |
| Cholesky factor | `dpotrf` loop: **8/8 WIN, 2.51–8.48×** (post-fix re-measure) | 8/8 WIN, 2.5–5.4× (increment-B table) | — |
| LU factor (per-lane pivoting) | `dgetrf` loop: **8/8 WIN, 1.76–3.81×** (post-fix re-measure) | 8/8 WIN, 1.2–2.1× (increment-C table) | `pagemldivide` (A\x vs our LU factor+solve nrhs=1): **3/3 WIN, 1.11–1.14×** |
| small-SVD (one-sided Jacobi) | `dgesdd` loop: **8/8 WIN, 1.45–12.03×** | 8/8 WIN, 1.4–11.7× | — |

MATLAB rows: R2026a, `maxNumCompThreads(1)`, measured 2026-07-05 evening (`scripts/v14_matlab_board.m`).

## 9. Sparse tensors (v14-i) — vs SPLATT / TACO / scipy / torch / MATLAB TTB

Source: [2026-07-05-v14i-sparse.md](2026-07-05-v14i-sparse.md) (1024³, 5M nnz, f64, R=F=16, 1T pinned, bit-identical inputs on every tool, checksums cross-validated, format prep excluded on every side).

| op | ours ms | best peer | ratio vs best | full row |
|---|---|---|---|---|
| MTTKRP mode-0 | 10.61 | SPLATT 16.0 | **1.51×** | TACO 24.7 (**2.3×** — the phase-row value) · torch 63.46 · scipy 111.79 — all WIN (board prose: scipy/torch beaten 3.5–10.8× on MTTKRP) |
| MTTKRP mode-1 / mode-2 | 10.50 / 10.72 | SPLATT 16.0 / 16.0 | **1.52× / 1.49×** | WIN |
| TTM mode-1 (folded layout) | 33.66 | TACO 92.35 | **2.74×** | scipy 94.22 · torch 115.92 — WIN |
| TTM mode-2 | 24.64 | scipy 68.95 | **2.80×** | torch 76.75 — WIN |
| sparse add (union) | 30.80 | scipy 39.30 | **1.28×** | torch 242.17 — WIN |
| sparse mul (intersection) | 21.55 | scipy 38.42 | **1.78×** | torch 94.64 (loses 4.4×) — WIN |
| reduce sum-total | 1.14 | torch 1.41–1.53 | **1.24×** | scipy 1.59–1.79 — WIN (conservative read stays a win: 1.16×) |
| reduce sum-mode0 (CSF root) | 1.02 | scipy 1.65–1.80 | **1.62×** | torch 220.81 — WIN (conservative 1.56×) |
| reduce max-mode0 (CSF root) | 1.06 | scipy 1.30–1.52 | **1.23×** | **torch N/A-with-the-check** (torch.sparse has no max reduction — verified) · conservative 1.10× |
| MTTKRP modes 0/1/2 vs MATLAB TTB `sptensor/mttkrp` | 10.6 / 10.5 / 10.7 | TTB 413.6 / 326.6 / 313.2 | **39.0× / 31.1× / 29.3×** | WIN |

**13/13 measured rows WIN across ALL contracted sparse peers** (SPLATT v2.0.0 built from source; TACO's
broken `-time` CLI patched on THEIR side for honest numbers).

## 10. Dense decompositions (v14-j) — vs TensorLy 0.9.0 / MATLAB TTB

Source: [2026-07-05-v14j-decomp.md](2026-07-05-v14j-decomp.md) (bit-identical f64 inputs both languages, fixed iteration budgets, fits computed identically; 1T pinned).

| size | op (budget) | speedup vs TensorLy | fit |
|---|---|---|---|
| 64³ | CP-ALS r16 (10 sweeps) | **5.37×** | equal (to ~1e-12) |
| 64³ | Tucker-HOOI r16 (5 sweeps) | **5.06×** | equal |
| 64³ | randomized Tucker r16 | **1.22×** | **ours strictly better** |
| 32⁴ | CP-ALS r8 | **5.63×** | equal |
| 32⁴ | Tucker-HOOI r8 | **5.82×** | equal |
| 32⁴ | randomized Tucker r8 | **1.38×** | **ours strictly better** |
| 128³ | CP-ALS r32 | **5.20×** | equal |
| 128³ | Tucker-HOOI r32 | **5.13×** | equal |
| 128³ | randomized Tucker r32 | **1.71×** | **ours strictly better** |

**9/9 WINS at equal-or-better fit** — and the randomized rows run MORE power iterations than TensorLy's
shipped defaults (q=4 vs q=2) while staying faster. MATLAB TTB rows (R2026a, 1T, `'tol',0` forced —
TTB's default tol EARLY-STOPS, the protocol trap is recorded in the board): `cp_als` 64³ **1.67×** ·
`cp_als` 32⁴ **1.11×** · `tucker_als` 64³ **1.63×** — **3/3 WIN** (TTB = the strongest MATLAB-side peer
measured; still every row ours). First-board honesty: the initial board LOST every row (0.10–0.52×) —
root-caused (bidiagonal SVD of short-fat unfoldings) and flipped via Gram+`eig_sym`, batched Philox
sketch, Gram-operator power iteration; all re-gated.

## 11. Tensor trains (v14-k) — vs tntorch 1.1.2; ttpy N/A

Source: [2026-07-05-v14k-tt.md](2026-07-05-v14k-tt.md) (matched problems, budgets, accuracy; 1T pinned).

| op | problem | ours | tntorch | ratio |
|---|---|---|---|---|
| tt_svd | Hilbert 20⁴ @1e-8 | 4.29 ms | 14.31 | **3.34×** |
| tt_svd | smooth 16⁴ @1e-10 | 2.09 | 5.92 | **2.83×** |
| tt_svd | smooth 32⁴ @1e-8 | 31.92 | 201.80 | **6.32×** |
| tt_add + tt_round | 16⁴, (t+t)→round | 0.31 | 0.76 | **2.45×** |
| tt_eval_many | 1M pts, 16⁴ | 54.1 ns/pt | 706.9 | **13.1×** |
| tt_eval_many | 1M pts, 16⁶ | 143.5 ns/pt | 1792.3 | **12.5×** |
| tt_cross | 16⁴ (both 13,760 evals) | 0.40 ms | 5.03 | **12.6×** |
| tt_cross | 16⁶ two-body (both 19,008 evals; ours MORE accurate: 1.71e-4 vs 1.92e-4) | 0.69 | 10.37 | **15.0×** |

**8/8 WINS** (a first-cut 0.53× tt_svd row was root-fixed in-session: QR-first wide unfoldings). The
LUT demo (16⁶ potential): **1748× compression** from 19,008 evals, **1.50× faster** than 64-corner
interpolation of the materialized 128 MB table in the continuous-eval hot path — honesty rows stated
plainly: raw grid-index reads still favor the dense table (10.7 ns single gather vs 143.5), and the
first (overlapping-domain) kernel is a genuine TT-rank wall that **tntorch fails identically** on the
same budget (0.435 vs 0.442 val_err — the kernel, not the implementation). **ttpy: N/A-with-the-check**
(pip install fails on py3.12 — `numpy.distutils` removed; attempted twice 2026-07-05; the maxvol source
of truth is on-disk via tntorch's verbatim `py_maxvol`).

## 12. I/O + interop (v14-l) — vs numpy / safetensors-python

Source: [2026-07-05-v14l-io.md](2026-07-05-v14l-io.md) (512 MB f32, page-cached ext4, sync-separated phases, 3 alternating rounds; python safetensors read forced honest via `.clone()` — bare `load_file` is a lazy mmap measuring 0.000 s).

| metric | ratio range across the 3 rounds | verdict |
|---|---|---|
| npy write | **2.03× / 2.50× / 2.07×** vs numpy | 3/3 WIN |
| npy read | **1.42× / 1.63× / 1.09×** vs numpy | 3/3 WIN |
| safetensors write | **2.78× / 2.99× / 1.84×** vs safetensors-py | 3/3 WIN |
| safetensors read | **2.97× / 6.30× / 2.07×** (peak 11.15 GB/s) | 3/3 WIN |

**12/12 WIN, every metric, every round** (round 3 globally slower on BOTH sides — writeback pressure;
ratios hold). Correctness riding the same binaries: our npy v1.0 writer **byte-identical to `np.save`
on all 17 corpus cases**; 39/39 cross-tool round-trips bit-exact (incl. bf16 + fp8-e4m3 safetensors,
npz STORED + DEFLATE read). **npz-DEFLATE WRITE = N/A-with-the-check** (no deflate compressor in the
engine — zstd is the house codec; write emits STORED exactly as `np.savez` itself does, compress_type 0
verified, and numpy reads our STORED npz bit-exact — nothing in the round-trip contract requires
DEFLATE output). DLPack: zero-copy pointer-equal both directions (not a throughput row).

## 13. NN inference (v14-m) — vs torch / torch-int8 / onnxruntime / ort-int8 / ggml(llama.cpp)

Source: [2026-07-05-v14m-nn.md](2026-07-05-v14m-nn.md) (frozen torch-exported corpus: MLP + CNN; 1T matched; ours built `-mavx2 -mfma -mavxvnni`, peers runtime-dispatch the same ISA; value parity ≤1e-6 gated, measured 6e-8; ours-Q8 and ort-int8 at the SAME measured accuracy on this corpus).

f32 (ns/sample):

| model@batch | ours | vs torch | vs onnxruntime (MLAS) |
|---|---|---|---|
| mlp@16 | 215.8 | **5.7×** | **2.1×** |
| mlp@4096 | 218.1 | **1.9×** | **1.15×** |
| cnn@8 | 1858.0 | **9.7×** | **1.30×** |
| cnn@4096 | 1974.3 | **7.6×** | **1.16×** |

Quantized (ns/sample; ours = ggml-compatible Q8_0, per-32-block f16 scales, byte-interop gated):

| model@batch | ours Q8_0 | vs torch-int8 (fbgemm dynamic) | vs ort-int8 (MatMulInteger) |
|---|---|---|---|
| mlp@16 | 377.9 | **6.6×** | **1.27×** |
| mlp@4096 | 398.5 | **1.10×** | **0.48× — OPEN cell** (see below) |
| cnn@8 | 1910.9 | **10.2×** | **8.4×** |
| cnn@4096 | 2018.7 | **7.5×** | **9.3×** |

**The ort-int8 mlp@4096 cell (flagged, not accepted):** a quantization-SCHEME cost, not an
implementation gap — ort uses ONE dynamic per-tensor activation scale + per-column weight scales
(one rescale per output); our locked contract pins ggml-compatible Q8_0 per-32-block f16 scales
(byte-interop with the frozen refs / llama.cpp), measured ~60% of the inner-loop op count. Inside the
Q8_0 contract the measured floor is ~280–330 ns/sample — still short of 192. Beating the cell honestly
requires a **per-tensor int8 tier — IN PROGRESS** as the named follow-up (a new dtypes/storage decision
filed at the v14-m close; outside the locked v14-m row). The same-format proof that the gap is the
scheme, not fat:

| layer (Q8_0×Q8_0, batch 4096, 1T) | ours ns/row | ggml (llama.cpp, GGML_NATIVE) | ratio |
|---|---|---|---|
| fc1 64→128 | 188.8 | 278.9 | **1.48×** |
| fc2 128→32 | 112.1 | 150.2 | **1.34×** |
| fc3 32→10 | 25.5 | 31.2 | **1.22×** |
| all three linears | 326.4 | 460.3 | **1.41×** |

Our WHOLE Q8 inference (398.5 ns/sample incl. activation quantize + layernorm + softmax + fused relu)
is faster than ggml's three linears alone (460.3). (`test-backend-ops perf` has no tiny-MLP shapes —
N/A-with-the-check for the stock binary; the driver on the exact shapes is the comparable row.)

---

## The honest rows, consolidated (nothing dropped)

| row | status | owner / disposition |
|---|---|---|
| batched GEMM 6×6@100k vs MKL `dgemm_batch_strided` | **TIE at the DRAM wall** (0.97–1.09× over 5 samples; both ~24 GB/s on an 86 MB stream; the cache-resident sibling row wins 1.55×) | closed as a documented memory-wall tie — the two-signal evidence is in the board |
| einsum exec `abc,bad->dc`@96 vs numpy (0.89×) and `ab,bc,cd->ad`@512 vs numpy/torch (0.96×/0.82×) | **OPEN** | pinned to the raw v0d f64 GEMM rate (bit-locked by ADR-0063); tracked as the engine-wide v0d GEMM row (order-preserving pass already halved the gap 2026-07-03) |
| ort-int8 mlp@4096 (0.48×) | **OPEN — per-tensor int8 tier IN PROGRESS** (named follow-up) | quantization-scheme cost inside the locked Q8_0 contract; same-format peer (ggml) beaten 1.22–1.48× at equal measured accuracy |
| permute 3D 512³ @8T vs HPTT | **PARITY** (ranges overlap in both final rounds; was 0.82×, closed by staged strips) | 1T remains a 3/3 crush |
| elementwise contiguous add vs numpy | WIN at 1.05× — **the DRAM ceiling**, stated | — |
| TT LUT raw grid-index reads | dense 128 MB table wins that one access pattern (10.7 vs 143.5 ns/pt) — stated plainly in the board | TT wins the actual interpolation hot path 1.50× at 1748× less storage |
| npz DEFLATE **write** | **N/A-with-the-check** (no deflate compressor in the engine; STORED = what np.savez emits; numpy reads ours bit-exact) | zstd is the house codec; inflate (read) IS implemented + CRC-gated |
| ttpy | **N/A-with-the-check** (py3.12 `numpy.distutils` removal; attempted twice) | maxvol source of truth on-disk via tntorch's verbatim `py_maxvol` |
| TBLIS | **MEASURED at v14-z close** (docs/bench/2026-07-05-v14z-tblis-xtensor.md): transpose-heavy TTGT case WON 1.14×; three pure-GEMM rows 0.95–0.98× = the SAME named v0d f64 GEMM-kernel gap (three more cells on the existing open bug, no new owner) | closed as a measurement item |
| torch sparse max reduction | **N/A-with-the-check** (feature absent in torch.sparse — verified) | — |
| xtensor | **MEASURED at v14-z close** (same board): 3/3 WINS · broadcast 3.38× · strided 1.18× · contiguous 1.07× at the DRAM wall | closed |

## Bottom line — rows per peer (as counted in the family tables above)

| peer | rows | won | tie/parity | open | N/A |
|---|---|---|---|---|---|
| NumPy (2.4.6) | 16 | **14** | — | 2 (einsum 0.89×/0.96× — v0d GEMM row) | — |
| torch-CPU 2.12 (incl. torch-int8) | 56 | **55** | — | 1 (einsum @512 0.82× — same root) | 1 (sparse max) |
| MKL native (batch-strided GEMM, potrf/getrf/gesdd loops) | 32 | **31** | 1 (DRAM-wall GEMM tie) | — | — |
| MATLAB R2026a (pagemtimes, pagemldivide, Tensor Toolbox) | 17 | **17** | — | — | — |
| ml_dtypes 0.5.4 | 3 | **3** | — | — | — |
| ReproBLAS v2.1.0 | 2 | **2** | — | — | — |
| HPTT | 6 | **5** | 1 (3D@8T) | — | — |
| opt_einsum 3.4.0 | 33 quality + 2 speed | **2 speed; quality ≤ on 33/33** (strictly better where their inconsistency bites) | (equal-cost cases within the 33) | — | — |
| cotengra 0.8.2 (+kahypar) | 12 + 3 slicer | **12** | 3 (matched-tree slicer parity) | — | — |
| cotengrust (Rust) | 6 | **6** | — | — | — |
| SPLATT v2.0.0 (source-built) | 3 | **3** | — | — | — |
| TACO (source-built, CLI patched) | 2 | **2** | — | — | — |
| scipy 1.17.1 (sparse) | 8 | **8** | — | — | — |
| TensorLy 0.9.0 | 9 | **9** | — | — | — |
| tntorch 1.1.2 | 8 | **8** | — | — | — |
| safetensors-python 0.8.0 | 6 | **6** | — | — | — |
| onnxruntime 1.27.0 (f32 + int8) | 8 | **8** | — | — (the ort-int8 mlp@4096 cell CLOSED same evening: per-tensor i8 tier 155.1 ns vs 191.1 = 1.23× at better accuracy) | — |
| llama.cpp/ggml (native build, same format) | 4 | **4** | — | — | — |
| ttpy | — | — | — | — | all (install impossible on py3.12) |
| TBLIS | 4 | **1** | 2 (~ties 0.97–0.98×) | 1 (0.95× @512) — all 3 non-wins = the one named v0d GEMM-kernel bug | — |
| xtensor | 3 | **3** | — | — | — |

**Totals (final, after the same-evening closes — ort per-tensor cell won, TBLIS + xtensor measured):
210 measured comparison rows (+ the 33 einsum path-quality parity-or-better cases counted
separately): 199 won · 7 tie/parity (1 DRAM-wall GEMM tie, 1 HPTT 3D@8T parity, 3 matched-tree slicer
parity, 2 TBLIS pure-GEMM ~ties 0.97–0.98×) · 4 open — and ALL FOUR open cells are the SAME single
named bug (the v0d raw f64 GEMM-kernel gap: 2 einsum-exec cells + 1 einsum-exec torch cell + 1 TBLIS
@512 cell; pinned mechanism, named owner, ADR-0100 proposal on file) · ttpy + torch-sparse-max
N/A-with-the-check.** No peer beats crd-hesap-tensor on any measured row while carrying ANY of the
moat properties below.

---

## Conformance audit — every moat claim → the ctest gate(s) that enforce it

All gates are Catch2 TEST_CASEs under `tests/hesap-tensor/`, ctest-registered via `catch_discover`
(the per-slice-verification-runs-ctest rule); file:name pairs below are the enforcement points.

### Tier D — `{1..16}` workers bit-identical (fixed-order trees, grain = f(shape) only)

| op family | gate |
|---|---|
| reductions (full) | `test_reduce.cpp` — "v14-c Tier D: the {1,2,4,8,16} moat - serial == parallel, bit-identical" |
| axis reductions (vertical + row parallel paths) | `test_reduce_axes.cpp` — "v14-c axes: the {1,2,4,8,16} moat on vertical + row paths" |
| permute (MT super-block tasks) | `test_permute.cpp` — "v14-d MT: {1,2,4,8,16} workers produce bit-identical permutes" |
| einsum execution (through `gemm_parallel`) | `test_einsum_exec.cpp` — "v14-f exec: run-twice + the {1,2,4,8,16} moat" |
| hyper-optimizer (plans + winner trial) | `test_hyperopt.cpp` — "hyperopt: the {1,2,4,8,16} moat - bit-identical plans at every worker count" |
| batched GEMM | `test_batched.cpp` — "batched: the {1,2,4,8,16} moat - gemm bit-identical at every worker count" |
| batched Cholesky | `test_batched.cpp` — "batched: cholesky {1,2,4,8,16} moat" |
| batched LU (factors AND pivot sequences) | `test_batched.cpp` — "batched: LU factor+solve - tier bit-identity incl pivots, residual, moat" |
| batched small-SVD | `test_batched.cpp` — "batched: small-SVD Jacobi - reconstruction, orthogonality, tier bit-identity, moat" |
| sparse MTTKRP + TTM (both partition modes) | `test_sparse.cpp` — "sparse: the 1 2 4 8 16 worker moat on mttkrp and contraction" |
| decompositions (cp_als + hooi_rand, MT permute path exercised) | `test_decomp.cpp` — "decomp: the 1-2-4-8-16 worker moat - bit-identical decompositions" |
| philox_fill (order-independent keying, strided ≡ contiguous) | `test_io.cpp` — "io: philox_fill is order-independent - the worker moat and the sequential anchor" |
| NN inference, f32 AND Q8, both models + dense-GEMM tier @8192 | `test_nn.cpp` — "nn: inference is bit-identical at 1 2 4 8 16 workers for f32 and q8" |

(TT is deliberately serial-only v1 — stated in `tt.hpp` and the board; its determinism is gated as
run-twice bit-identity, below, so no `{1..16}` claim is made for `tt_*`.)

### Tier R — partition-INDEPENDENT reproducible reduction (ReproBLAS-class binned)

- `test_reduce.cpp` — "v14-c Tier R: partition/shuffle-INDEPENDENT bits + accuracy": bit-identity under
  forced REPARTITION ({3,7,16} chunks × forward/reverse merge) AND full element shuffle + integer
  exactness + accuracy ≥ naive on cancellation.

### Deterministic stochastic rounding (Philox counter-keyed, reproducible-by-seed)

- `test_dtypes.cpp` — "v14-a dtypes: deterministic SR - validity, unbiasedness, bit-identity" (grid
  unbiasedness + run-twice bits, ×4 formats).
- `test_dtypes.cpp` — "v14-a StorageTensor: strided converts + SR chunk/stride independence" (permuted
  view ≡ materialized copy — the canonical-destination-index keying contract).
- `test_dtypes.cpp` — "v14-a dtypes: batch (SIMD) converts are bit-identical to scalar" (SIMD SR ≡
  scalar SR lane-for-lane over the corpus + 100k Philox patterns).
- `test_reduce.cpp` — "v14-c SR accumulation: reproducible-by-seed + on-grid" (the 3-tuple keying).

### Deterministic-randomized decompositions (same seed ⇒ bit-identical at any worker count)

- `test_decomp.cpp` — "decomp: run-twice determinism - identical bits on every surface" +
  "decomp: the 1-2-4-8-16 worker moat" (covers `hosvd_rand`/`hooi_rand` Philox-keyed sketches).
- `test_tt.cpp` — "tt: tt_svd hilbert - frozen ranks, error <= eps, per-point oracle, run-twice bits" +
  "tt: tt_cross builds the smooth kernel from evaluations within the frozen budget" (Philox-seeded
  starts, run-twice bit-identity).
- `test_hyperopt.cpp` — "hyperopt: T>0 greedy is run-twice bit-identical under a fixed seed" +
  "hyperopt: the driver beats-or-matches plain greedy and is run-twice identical" (every stochastic
  tier seeded).
- `test_sparse_cp.cpp` — "sparse-cp: cp_als_sparse runs, converges direction, and is run-twice
  bit-identical" (the sparse seam inherits the property).

### Integer inference — bit-exact quantized path (the strongest certification tier)

- `test_dtypes.cpp` — "v14-a dtypes: ggml Q8_0/Q4_0 quantization is byte-exact vs the reference".
- `test_nn.cpp` — "nn: q8 quantize-on-load is byte-exact vs the frozen q8 refs for every weight"
  (D-v14m-1 semantics, documented in nn.hpp) · "nn: q8 quantize-on-load and raw-ref weight loading are
  bit-exact against each other" · "nn: the vectorized activation quantizer is bit-identical to dtypes
  quantize_q8_0" · "nn: quantized inference is run-twice deterministic with bounded error vs f32" ·
  the `{1..16}` moat case above (f32 AND q8). The Q8 kernel's AVX-VNNI path is integer-EXACT — the
  full gate suite verified green under BOTH `-mavx2`-only and `-mavxvnni` builds (board note).

### Certification-pillar gates riding the same suites

- Zero-heap execute: `test_nn.cpp` — "nn: infer performs ZERO allocations - counting-allocator gate";
  `test_tt.cpp` — "tt: evaluation is allocation-free and the batch evaluator bit-matches".
- Bounded iteration + status adversaries: `test_decomp.cpp` boundary case (NotConverged budgets),
  `test_tt.cpp` boundary case, `test_batched.cpp` SVD bounded max_sweeps, plus the per-suite
  adversary cases (io/npz/safetensors/dlpack rejects → clean statuses).
- Link isolation (lean dense-only consumers): `runtime/examples/smoke_hesap_tensor.cpp` — links ONLY
  core/containers/memory/math + tensor.

### Audit notes (discrepancies found while consolidating — reported, not silently fixed)

1. The v14-h board's **verdict line is stale/pre-fix**: it says "Cholesky 2.48–10.58×" and "LU
   1.21–2.87×" while the same doc's post-fix re-measurement (and both phase rows) record chol
   **2.51–8.48×** and LU **1.76–3.81×**; it also still says "MATLAB rows N/A-with-check" although the
   measured MATLAB tables were added above it the same evening. This scoreboard uses the post-fix +
   measured values.
2. The v14-i board's verdict line says "TACO 2.4×/2.8×" and "torch's sparse mul loses 4.5×"; the
   board's own tables give MTTKRP vs TACO 24.7/10.61 = **2.3×** (the value both phase rows use) and
   torch mul **4.4×**. Table values used here.
3. v14-g wall-clock: the master phase row quotes "2.2–6.1×" (the board's §5 driver-timing list) while
   the detail phase row and verdict line quote "1.8–5.8×" (the matched-64-trial table). Both protocols
   are in the board; this scoreboard uses the matched-budget table (1.81–5.83×).
4. v14-a: the board was not re-issued after the float_convert.hpp migration; the post-migration
   re-verify values live only in the phase doc's session log (§1 caveat above).
5. TBLIS/TCL oracle build promised "with the v14-g close" (v14-f board) never landed — CLOSED at v14-z: built + measured 2026-07-05 evening (docs/bench/2026-07-05-v14z-tblis-xtensor.md).
6. v14-h "vs torch bmm: 3–10× wins on every row": computed from the board's own baseline table, the
   n=8@10k and n=16@10k rows are 1.56× and 1.02× — every row still a WIN, but the stated range only
   holds for the 100k rows. The table-derived range (1.02–8.7×) is used here.
7. **v14-m rows are unwritten in both phase docs**: the board + `test_nn.cpp` (16 cases / 4,988
   asserts) exist, but neither `phase-3.1.6-v14.md` nor the master table carries a shipped verdict
   for m, `context.md` still says "m in flight", no v14-m session log exists, and no config-ladder
   record beyond the board's both-ISA gate note. Likewise the DETAIL doc's v14-e and v14-f rows
   never received the shipped verdicts the master rows carry (the two-homes rule, pending).
8. The `docs/bench/README.md` index has no entries for the v14-a board or any of the seven
   2026-07-05 boards (g/h/i/j/k/l/m) — or this file.
