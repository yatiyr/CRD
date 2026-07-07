# Phase 3.1.6 — v14: the TENSORS cluster (`crd-hesap-tensor` — the engine's N-D substrate)

> **Status: ✅ COMPLETE a–z (2026-07-05).** Every slice shipped; every contracted peer measured (the all-peers
> scoreboard `docs/bench/2026-07-05-v14z-scoreboard.md` — 210 rows: 199 won / 7 tie/parity / 4 open = the one
> named v0d raw-f64 GEMM-kernel gap, ADR-0100 proposal on file). Module ctest 133/133 env-free; 5-config ladder
> green per slice. **PENDING only** (deferred, not blocking): the whole-engine per-slice-check sweep (→ CI) +
> the combined v10-FFT-close + v14 g–z commit (user commits). **NEXT: v15/v16 autodiff — ADR-0097 +
> `docs/phases/phase-3.1.6-v15.md`.** This is the spec; the master phase doc
> (`phase-3.1.6-hesap.md`) carries the one-line roadmap rows + per-slice crush verdicts as they land.
> **ADR-0096 (Accepted 2026-07-02)** pins the architecture; memory `project_v14_v18_planning` pins the scope
> (locked, user-approved, maximal — do NOT re-plan). ~24.1 KLOC / ~880 tests / a–m+z / ~8–10 weeks, multi-session.
>
> **The consumers that set the bar:** v15/v16 autodiff (tape operands = `Tensor<T>` + `EinsumPlan`) · 3.1.11
> batched EKF/sensor fusion · CFD/FEA element batches + POD/HOSVD ROMs · TT-compressed LUTs/ephemerides (the v13
> tie-in) · **certified tiny-ML inference (DO-178C/EASA)** · medical volumes · DAW spectrogram batches · games
> skinning/SH. The moat is timely — deterministic inference is a hot 2025-26 topic, and NumPy-MKL / PyTorch /
> TBLIS ship NONE of: `{1..16}` bit-identical parallel ops, partition-independent reproducible reduction,
> reproducible-by-seed stochastic rounding.

---

## 1. The moat — what v14 has that the incumbents structurally lack

1. **Tier D (default): fixed-order reduction trees, serial ≡ parallel.** Every reduction/contraction is
   bit-identical across `{1..16}` workers (deterministic `parallel_for` partitions, fixed combine order).
   NumPy-MKL and PyTorch give thread-count-dependent bits; their "deterministic" modes are slow opt-ins that
   still don't span thread counts.
2. **Tier R (opt-in): ReproBLAS-class binned summation** — bit-reproducible **independent of partitioning**
   (across machines / core counts / chunkings). Named separately from Tier D — conflating the two claims is the
   honest-scoreboard scar applied to reproducibility. Benched vs ReproBLAS itself.
3. **★Deterministic stochastic rounding** — Philox counter-RNG keyed by `(seed, element linear index)` ⇒ SR is
   order-independent AND reproducible-by-seed. The 2024-26 low-precision frontier; nobody ships deterministic SR.
4. **The v13 certification pillars carry over verbatim** (ADR-0095 §2): caller workspaces / zero heap per call ·
   status-not-exception (`TensorStatus`, `noexcept`, `-fno-exceptions`-clean) · bounded iteration everywhere
   (CP-ALS/HOOI/TT-cross carry `max_iters` + status, never spin) · **dynamic slicing** makes einsum contraction
   memory a hard bound (the WCET pillar applied to einsum).
5. **Integer inference is bit-exact across ALL hardware trivially** — the strongest certification tier, and the
   3.1.14 LLM on-ramp (v14-m).

## 2. Architecture (ADR-0096 — the decisions, not re-litigated here)

- **ONE module `crd-hesap-tensor`**, internal sub-headers (`tensor.hpp` substrate · elementwise/reduce/permute ·
  einsum · sparse · decomp · io · nn). Dense-only consumers dead-strip the rest.
- **Templated compute dtypes** `Tensor<T>` / `TensorView<T>`, `T ∈ {f32, f64, c32, c64}` (+ `i64` index / `u8`
  mask). **Storage dtypes** — f16 · bf16 · FP8 e4m3/e5m2 (OCP) · int8/int4 block-quantized (ggml-compatible:
  per-block scale, block 32 default) — are buffers with explicit convert/quantize/dequantize ops (F16C where
  available); compute always runs f32/f64. Exception: v14-m integer inference computes on int8/int4 natively.
- **NumPy stride-view semantics, rank ≤ 8** (`kMaxRank`): fixed shape/stride arrays (allocation-free metadata,
  WCET-analyzable). Strides in ELEMENTS, signed (negative = flips), stride-0 broadcasting, contiguity tracking.
  `Tensor<T>` owns via `IAllocator*`; `TensorView<T>` non-owning, caller-guaranteed lifetime (Span discipline).
- **einsum**: parser → path optimizer (greedy + optimal-DP) → **`EinsumPlan` build-once/execute-many** (the v13
  precompute lever; NumPy re-plans every call) → TTGT over the OWN v0d GEMM + batched dispatch + direct small/odd
  kernels. GETT-fused kernels ONLY if the profile names the transpose as the wall (SANITY #5).
- **Reuse (SANITY #8)**: v0d GEMM microkernel + `gemm_parallel` · hesap-dense SVD/QR/LU/chol + `interp_decomp` ·
  v12 counter-RNG (fills + SR + **deterministic-randomized** decomp) · crd-jobs deterministic `parallel_for` ·
  crd::math SIMD · v13 workspace/status contracts · ADR-0084 CRDR cook · the v13-z CLI pattern.

```cpp
namespace crd::hesap::tensor
{
inline constexpr crd::u32 kMaxRank = 8U;

enum class TensorStatus : crd::u8 { Ok, BadInput, RankOverflow, ShapeMismatch, NotConverged, Unsupported };

template <typename T> class TensorView   // non-owning; shape/strides by value (fixed arrays)
{
    T*       m_data;
    crd::u32 m_rank;
    crd::u64 m_shape[kMaxRank];
    crd::i64 m_stride[kMaxRank];         // ELEMENTS, signed (negative = flip); 0 = broadcast
    // slice / permute / reshape(view-only) / broadcast_to / is_contiguous / operator()(i0..ik)
};

template <typename T> class Tensor       // owning (IAllocator*), hands out TensorViews
{ /* alloc, shape, view(), zero-copy slice/permute; contiguous canonical row-major */ };

// dtype storage + converts (v14-a): f16/bf16/fp8e4m3/fp8e5m2/int8-block/int4-block buffers,
// convert_to<T>/convert_from<T>, quantize/dequantize, and the deterministic SR variants:
//   sr_convert(dst, src, PhiloxKey{seed})  — SR decision keyed by (seed, element linear index)
}
```

## 3. Verification protocol (every slice)

- **Reconstruct-and-verify-in-python FIRST** for every ported/matched algorithm (fetch the reference's actual
  source via `gh`) — the v13 discipline that caught bugs pre-port.
- **Full peer board per row** (matched threads; install missing peers; N/A stated *with the check*):
  NumPy(+MKL) · PyTorch-CPU · xtensor · opt_einsum + cotengra · **HPTT** · **TBLIS/TCL** · **SPLATT/TACO** ·
  TensorLy / MATLAB Tensor Toolbox / ttpy / tntorch · MATLAB `pagemtimes` · **ReproBLAS** · torch /
  onnxruntime-CPU / llama.cpp-CPU + ggml. A loss or tie = an OPEN bug (SANITY #9).
- **The `{1..16}` moat** on every parallel op + run-twice bit-identity; Tier R additionally gated under forced
  REPARTITION.
- **Per-slice Windows verification from day one** (the v13-z scar) — win-debug green minimum as slices land,
  never linux-gcc-only; full 4-config DoD at slice close.

## 4. The sub-slice table (the contract — from the master rows, locked 2026-07-02)

| Slice | Deliverable | ~LOC | ~Tests |
|---|---|---|---|
| **v14-a** ✅ CLOSED (2026-07-02) | **Substrate + dtypes.** `Tensor<T>`/`TensorView<T>` over `IAllocator`, shape/strides, zero-copy slice/permute/broadcast/reshape views, contiguity tracking; dtypes f32/f64/c32/c64 + i64 index/u8 mask + **f16/bf16 + FP8(e4m3/e5m2) + int8/int4-block QUANTIZED storage** (F16C converts; compute stays f32/f64) + **★deterministic stochastic-rounding converts** (Philox counter-driven, reproducible-by-seed). Gate: NumPy view/stride semantics corpus + convert round-trips + SR unbiasedness + SR run-twice bit-identity. **▶ CLOSE:** c32/c64 + i64/u8 substrate gates (`test_dtype_set.cpp`, 174 asserts), tidy naming fixes, 4-config DoD GREEN (debug+asan+shipping 104,760/28 · permute 1,389/10 · STRICT win-tidy · linux-gcc) + guards. | ~2100 | ~80 |
| **v14-b** ✅ core (2026-07-02) | **Elementwise + broadcasting engine.** Full NumPy broadcast rules; SIMD-fused unary/binary/compare/where/cast over crd::math. Gate: NumPy bit-exact (f64) on a broadcast corpus. **▶ SHIPPED:** `elementwise.hpp` — broadcast_shapes + prepare/collapse (jointly-contiguous dim fusion across all operand streams) + the P0/P1/P2 engine (flat-SIMD / splat-run-SIMD / scalar), ops {add,sub,mul,div,min,max} × {f32,f64} on Vec8f/Vec4d, sign-bit neg/abs (NaN-payload exact), compare→u8, three-way-broadcast where, f32↔f64 casts. **NumPy BIT-EXACT corpus green** (suite 22 cases / 104,415 asserts: win-debug + win-asan-0-err + linux-gcc). **⭐ FULL-BOARD CRUSH** (`docs/bench/2026-07-02-v14b-elementwise-broadcast.md`): broadcast mul **1.50× numpy / 1.68× torch** · strided **1.57× numpy** · row-bcast f64 **1.27×/1.49×** · contiguous 1.05× numpy (DRAM ceiling) / 2.9× torch — zero losses. Pinned: Min/Max = IEEE hardware semantics (NumPy NaN-propagating variants on consumer demand). Parallel path lands with v14-c's Tier-D grain. | ~1200 | ~50 |
| **v14-c** ✅ core (2026-07-02) | **Reductions + scans + ★the reproducible tier.** **▶ SHIPPED** (`reduce.hpp` + `reduce.cpp`): **Tier D** — fixed-order block trees (grain = f(shape) ONLY), sum/prod/min/max/mean/argminmax/cumsum/logsumexp, serial ≡ parallel over crd-jobs with caller workspace, **{1,2,4,8,16} moat GATED bit-identical (f32+f64)**; **Tier R** — faithful fold-3 ReproBLAS v2.1.0 transcription (sticky-bit deposit ladder / renorm / merge / convert) + **12-accumulator SIMD (3 streams × 4 lanes) + speculative single-DRAM-pass with snapshot-rollback**, gated bit-identical under forced REPARTITION ({3,7,16} chunks × fwd/rev merge) + full SHUFFLE + integer-exact + accuracy ≥ naive; **★SR accumulation** into bf16 (3-tuple Philox keying), seed-reproducible + grid-unbiased, gated. **⭐ FULL CRUSH** (`docs/bench/2026-07-02-v14c-reduce-vs-reproblas.md`): Tier-R **1.60× ReproBLAS @1M · 1.01–1.23× @16M** (reproducible sum now cheaper than a naive serial sum); Tier-D 0.120 ns/elem @1M ≈ 3× naive. Suite green debug+asan+gcc. **▶ AXIS REDUCTIONS SHIPPED (2026-07-02):** `reduce_axes.hpp` — general axes_mask over sum/prod/min/max/mean with the shape-adaptive VERTICAL (leading-axes, per-output serial-chain SIMD across outputs) / ROW (trailing-axes, per-output fixed block tree) / GENERAL (strided odometer) dispatch, + argmin/argmax-along-axis (first-wins) + cumsum-along-axis; parallel over disjoint outputs; gates: EVERY mask × EVERY op exact on a rank-4 tensor + strided sources + **{1,2,4,8,16} moat on both parallel paths** (suite now 31 cases / 109,047 asserts, debug+asan+tidy green). Mean = true divide (a reciprocal-multiply differs in the last ulp — caught by the exactness gate). | sum/mean/min/max/prod/argminmax/logsumexp/cumsum with **deterministic fixed-order trees (serial ≡ parallel)** + opt-in **ReproBLAS-class binned summation** (bit-reproducible INDEPENDENT of partitioning) + **★deterministic SR accumulation** (Philox SR into bf16/f16 accumulators — unbiased low-precision reduction, reproducible-by-seed). Gate: NumPy values + `{1..16}` moat + the Tier-R invariant under forced REPARTITION + bench vs ReproBLAS. | ~2000 | ~90 |
| **v14-d** ✅ increment 1 (2026-07-02) | **★HPTT-class transpose/permute.** Blocked SIMD multithreaded permute (deterministic partition), fused scale. Bench: HPTT itself. **▶ SHIPPED (agent-built in a worktree, integrated + re-verified):** `permute.hpp` — `permute_copy(view, order, dst, alpha=1)` with dst-ordered dim collapse (size-1 drop, negative/0-stride safe) → dispatch: memcpy-row / row-loop / **32×32 tiled 2-D transpose on the (src-fastest, dst-fastest) pair** with an outer odometer + **AVX2 8×8 f32 in-register microkernel**; alpha=1 is a pure bit copy (NaN payloads preserved). Gates: 10 cases / **1,389 asserts** — all 24 orders of 5×6×7×8 (f32+f64) bit-exact vs the for_each reference, tile-edge shapes (7×13, 129×65, 257×96), sliced/flipped/broadcast-stride sources, NumPy-corpus bit-exact (`scripts/v14d_permute_corpus.py`), run-twice identity; green **win-debug + win-shipping + win-tidy + linux-gcc**. Timing: 4096² f32 transpose **17.1× vs naive (11.55 GiB/s r+w, shipping)**. REMAINING for the row: the deterministic multithreaded partition + the HPTT head-to-head board — **the oracle is BUILT + baselined** (`external/hptt`, `external/PEER_ORACLES.md`: 1T 4096² f32 0.5505 ns/elem = 13.53 GiB/s · 8T 0.1274 = 58.47 GiB/s, DRAM-bound). **⭐ THE 1T HEAD-TO-HEAD IS A FULL CRUSH (2026-07-02): 1.11×/1.16×/1.31× over HPTT.** **▶ MT PASS SHIPPED (increment 2, same day):** disjoint super-block macro-tasks (bit-identical ANY worker count, {1..16} gated); NT stores WIN at 8T (30-34→42-51 GB/s; the 1T refutation inverts, as predicted) + per-task sfence + inverted MT tile rule + gated prefetch. **8T matched-state A/B: 2D 1.04-1.09× WIN · 4D 1.27-1.45× WIN · ⚠ 3D 0.79-0.84× LOSS — OPEN (SANITY #9)**, levers named in the bench doc; the recorded HPTT oracle numbers proved machine-state-sensitive (~25-45% drift) — matched-state A/B (`build/crd_permute_ab.sh`) is the honest protocol. (`docs/bench/2026-07-02-v14d-permute-vs-hptt.md`; levers: src-locality odometer + stride-aware tile edge + 64B alignment; NT-stores REFUTED at 1T and gated off, kept for the MT increment). En route: the repo-wide `crd-no-non-ascii-test-names` guard violation in the v14-a/b test names was caught (agent report) + fixed. | ~900 | ~30 |
| **v14-e** ✅ core SHIPPED (2026-07-03) | **einsum front-end.** Parser (ellipsis, repeated-index diagonals, output spec) + ★opt_einsum-class path optimizer (greedy + optimal-DP) + **`EinsumPlan` build-once/execute-many**. **▶ SHIPPED `einsum.hpp`:** NumPy-exact parser (incl. the "ii"→trace occurrence rule) · BB-optimal + multi-heuristic greedy + auto (≤7 operands → exact) · `EinsumPlan`. **⭐ Paths ≤ opt_einsum on ALL 33 oracle cases — strictly better where their optimal search's internal inconsistency bites (found + python-verified: it minimizes shared-removed but reports any-removed); planning 9×/41× faster (8.15 µs/plan).** 263 asserts green 4-config. Board `docs/bench/2026-07-03-v14e-einsum-path-vs-opteinsum.md`. | ~1500 | ~60 (263) |
| **v14-f** ✅ core SHIPPED (2026-07-03) | **Contraction execution.** TTGT over the OWN GEMM + batched-GEMM dispatch + direct kernels for small/odd shapes + the **tensor `LinearOp` bridge**. **▶ SHIPPED `einsum_exec.hpp`:** TTGT pipeline (permute-pack → GEMM → unpack) + plan-reuse execution. **Board vs numpy/torch: plan-reuse ×200 CRUSH 3.9×/3.6×; thin-K TN direct kernel flipped its row to 1.53×/1.07× WIN; two GEMM-BOUND cells remain OPEN (0.89–0.96× numpy, 0.82× torch — pinned precisely to the raw f64 GEMM gap, owner v0d/GEMM-asm-reopen, NOT an einsum-layer loss)** — the honest-scoreboard rows carried in the v14-z scoreboard. Board `docs/bench/2026-07-03-v14f-einsum-exec-vs-numpy-torch.md`. | ~1600 | ~55 |
| **v14-g** ✅ core SHIPPED (2026-07-05) | **★cotengra-class hyper-optimizer.** Hypergraph-partitioned contraction trees + simulated annealing + **dynamic slicing** (bounded-memory contraction = the WCET pillar applied to einsum). Gate: tree quality vs cotengra on its published benchmark networks; the slicing memory bound honored exactly. **▶ SHIPPED:** `hyperopt.hpp` — `HyperNet` (pooled-leg SSA hypergraph, saturating cost math) · faithful greedy engine (Boltzmann-Gumbel, batch-index guard, incumbent abort) · `HyperTree` + subtree-reconfigure (exact subset-DP re-solve) · treesa SA (4 rotations, Metropolis-in-log, geometric ladder) · labels-divide partition trees (own label-propagation + best-of glue; no kahypar dependency) · SliceFinder (integer-exact incremental costs, **EXACT memory bound, NotFound-never-best-effort**) · `hyper_optimize` driver (stratified Philox-keyed trials, per-trial reconf, SA finalists; trials = the parallel unit over crd-jobs). **Reconstruct-verified in python FIRST** (cost model bit-match 6/6, T=0 greedy identical, matched-tree slicer parity, oracle quality 5W/1T/0L). **⭐⭐ C++ FULL CRUSH (ctest-enforced corpus gate 6/6 + measured boards):** quality at-or-under cotengra greedy+kahypar @64 matched trials on every network (rand200 12.21 vs 13.08) · **wall-clock 1.81–5.83× (matched 64-trial protocol) faster than their FULL stack while producing better trees** · **1.07–2.2× faster than cotengrust (Rust) at the engine level** · `{1,2,4,8,16}` moat GATED bit-identical + run-twice identity. En route: an exact-value gate caught + root-fixed a pool-reallocation use-after-free in `merge_legs` (reserve-before-spans; SANITY #3 in new code). Boards + protocol: `docs/bench/2026-07-05-v14g-hyperopt-oracle.md`. Suite `[v14g]` 802+ asserts / 11 cases (linux-gcc; win-debug in flight). REMAINING for the row close: win 4-config DoD + tidy + einsum-front-end bridge wiring (>16-operand dispatch) at v14-z integration. | ~1500 (act. ~2900) | ~40 (act. 11 cases / 800+ asrt) |
| **v14-h** ✅ core SHIPPED (2026-07-05) | **Batched linear algebra.** Batched GEMM/Cholesky/LU/solve/small-SVD (the 6×6-by-100k EKF/skinning regime), deterministic batch partition. Bench: PyTorch CPU + MATLAB `pagemtimes`. **▶ SHIPPED `batched.hpp`:** GEMM (register-tiled direct tiny tier w/ the fma-chain bit contract + B-streams-once R∈[5,8] variant; dense-gemm large tier; crd-math gained `load_partial/store_partial`) · Cholesky factor+solve (lane-batched AoSoA, tier bit-identity + poison isolation) · LU factor+solve (per-lane pivoting via PURE-VECTOR argmax, bit-identity incl. pivot sequences) · small-SVD (one-sided Jacobi, masked rotations, bounded sweeps, shared finalize) — all `{1..16}`-moat gated. **⭐⭐ FULL CRUSH vs the FASTEST peers (native MKL + torch, every row):** GEMM 7W+1 DRAM-tie vs `dgemm_batch_strided` · chol 2.5–8.5× vs potrf · LU 1.76–3.81× vs getrf · SVD 1.45–12× vs gesdd; all rows beat torch (to 11.7×); **MATLAB MEASURED same evening: pagemtimes 2.0–8.2× + pagemldivide 1.11–1.14× ALL WON — the scoreboard is complete across MKL/torch/MATLAB.** **The day's 3rd MSVC scar root-caused via standalone repro + flag bisection: /O1+/O2 auto-vectorizes per-lane conditional two-array updates WRONGLY** (3 theories measured-and-killed first; the pure-vector fix is FASTER; SANITY ledger + memory). 5-config ladder green (gcc 3362/8 · win-debug/asan/shipping/tidy; svd.cpp std::sort→crd sort en route). Boards `docs/bench/2026-07-05-v14h-batched-la.md` · session `2026-07-05-v14h-batched-la.md`. | ~1200 (act ~1600) | ~40 (8 cases/3362) |
| **v14-i** ✅ core SHIPPED (2026-07-05, parallel wave) | **★SPARSE tensors.** COO + **★CSF** + **★MTTKRP** (the CP workhorse) + sparse×dense contraction + sparse elementwise/reduce. Peers: **SPLATT** (MTTKRP) + **TACO** (contraction) + scipy/torch.sparse. **▶ SHIPPED `sparse.hpp` + `sparse_mttkrp.hpp`:** `SparseCoo` (canonical sorted+deduped, rank-N TripletBuilder-pattern builder, stable LSD counting sort — compress() bit-reproducible) · `SparseCsf` (SPLATT mode tree, any level order) · MTTKRP (register-fused CSF tree walk; **CSF ≡ COO bit-identity gated**) · TTM/mode-n contract · sparse±/× sparse+dense · reduces (dense semantics) — `{1..16}` moat gated (a real moat violation in TTM's serial path caught + fixed pre-board). **⭐⭐ 10/10 BOARD WINS (1024³/5M nnz, matched 1T, bit-identical inputs, checksums cross-validated): MTTKRP 10.6 ms = 1.5× vs SPLATT v2 BUILT FROM SOURCE (16 ms), 2.3× vs TACO** (their broken `-time` CLI patched on THEIR side for honest numbers)**, 6–10× vs scipy/torch; TTM 2.7–2.8× vs TACO; add/mul/reduce all won.** 3 losing rows SOLVED in-session (register-resident fiber accumulators; staged TTM tile + blocked transpose; branchless packed-key merges — A/B proved branchy selects were the loss). + the **sparse-CP glue** (`sparse_cp.hpp`, integrator-built): v14-j's MTTKRP seam ← CSF functor, seam-parity + run-twice bit-identity gated. 906 asserts/12 cases; 5-config ladder ✓. **MATLAB TTB mttkrp MEASURED same evening: 29-39x WINS** => 13/13 across ALL contracted peers. Board `docs/bench/2026-07-05-v14i-sparse.md`. | ~2500 (act ~2800) | ~80 (906) |
| **v14-j** ✅ core SHIPPED (2026-07-05, parallel wave) | **Decompositions I.** ★CP-ALS (dense + sparse MTTKRP) · Tucker/HOSVD/★HOOI · **★randomized variants** (**deterministic-randomized**: same seed bit-identical `{1..16}`). **▶ SHIPPED `decomp.hpp`:** `cp_als` + **`cp_als_generic` over the MTTKRP SEAM** (the tensor enters ONLY through a functor — the sparse wiring point, now LIVE via `sparse_cp.hpp`) · `hosvd`/`hooi` · `hosvd_rand`/`hooi_rand` (Philox-keyed HMT sketches — every entry f(seed,stream,index) ⇒ bit-identical at any worker count, GATED). Reuse: v14-d permute unfoldings, dense `eig_sym`/QR/gemm, batched chol/Jacobi for the R×R solves. **⭐⭐ 9/9 BOARD WINS vs TensorLy 0.9 at equal-or-better fit: CP-ALS 5.20–5.63× · HOOI 5.06–5.82× · randomized Tucker 1.22–1.71× (strictly better fits).** The FIRST board lost every row (0.10–0.52×) → probed: 117 ms bidiagonal SVD per unfolding → 3 re-gated fixes (Gram G=AAᵀ+eig_sym 117→0.67 ms; AVX2-batched Philox sketch bit-equal to the keyed definition; Gram-operator power iteration) → ALL FLIPPED (rule #9 executed). 191 asserts/8 cases; frozen-tensorly-parity oracle (refs proved to 4e-11 pre-freeze); 5-config ladder ✓. **MATLAB TTB rows MEASURED same evening (fixed budgets via tol=0 - its default early-stops): cp_als 1.67x/1.11x, tucker_als 1.63x - ALL WON.** Board `docs/bench/2026-07-05-v14j-decomp.md`. | ~2000 (act ~2300) | ~70 (191) |
| **v14-k** ✅ core SHIPPED (2026-07-05, parallel wave) | **Decompositions II — TT.** ★TT-SVD + TT-rounding + TT algebra (add/hadamard/contract/**allocation-free eval**) + **★★TT-CROSS (maxvol)**. **▶ SHIPPED `tt.hpp`:** `TtTensor` (cores OWNED contiguous — the borrowed-lifetime scar honored) · `tt_svd` (+ QR-first wide-unfolding fast path) · `tt_round` (err ≤ tol·‖A‖ gated) · add/hadamard/dot/contract · **allocation-free eval** (`tt_eval/_many/_lerp`, workspace 2·max_rank, CountingAllocator-gated) · `maxvol` (Goreinov-Oseledets, transliterated from maxvolpy via tntorch + numpy-verified first) · `tt_cross` (callback-driven — NO materialized tensor; Philox-deterministic, bounded sweeps). **⭐⭐ 8/8 BOARD WINS vs tntorch: tt_svd 2.83–6.32× · add+round 2.45× · eval 13.1× (54 vs 707 ns/pt) · cross 12.6–15.0× at matched eval budgets AND equal-or-better accuracy.** One 0.53× row found + FIXED in-session (QR-first: QR of Cᵀ + small SVD of Rᵀ, no Q formed → flipped every tt_svd row). **★ THE LUT DEMO: 16⁶ two-body kernel TT-crossed from 19,008 evals — 1748× compression (75 KB vs 128 MB), accuracy below the grid's own interp error, 1.50× FASTER than 64-corner interp of the materialized table** (honesty rows: raw grid-index gather still wins at 10.7 ns/pt; the overlapping-domain kernel is a genuine TT-rank wall — tntorch fails it identically, evidence recorded). 280 asserts/9 cases; serial-only v1 stated in-header (determinism gates kept); ttpy N/A-with-check (py3.12/numpy.distutils). 5-config ladder ✓. Board `docs/bench/2026-07-05-v14k-tt.md`. | ~2400 (act ~2600) | ~85 (280) |
| **v14-l** ✅ core SHIPPED (2026-07-05, parallel wave) | **I/O + interop.** **▶ SHIPPED `io.hpp` + `detail/io_zip.hpp` + `dlpack.hpp` + vendored `detail/dlpack.h` (v1.1, Apache-2.0) + `hesap-resources/tensor_artifact.hpp`:** npy r/w (**our v1.0 writer byte-identical to np.save on all 17 corpus cases**; 14 dtypes incl. f16/bf16-bits carriers) · npz (write STORED = np.savez parity; read STORED + **DEFLATE via our own RFC-1951 inflate**, CRC-32 gated; DEFLATE *write* N/A-with-check — no deflate compressor in the engine, numpy reads our STORED bit-exact) · **safetensors r/w** (F32→BF16/F8_E4M3 both directions vs safetensors 0.8.0; spec-complete deterministic JSON parser incl. surrogate pairs) · **DLPack** producer/consumer (pointer-equal zero-copy, versioned+legacy ABIs, READ_ONLY + the deleter lifetime contract per spec) · `philox_fill` (keyed (seed, canonical index) ⇒ order-independent; `{1..16}` + strided≡contiguous gated) · CRDR `'TNSR'` cook/load (HMTX mirror: TNHD 96-B pinned header, FNV-1a-64 hash). **⭐ 12/12 BENCH WINS vs numpy/safetensors-python (512 MB, sync-separated): writes 1.8–3.0×, safetensors read to 6.3× (11.15 GB/s — python's lazy mmap forced honest via .clone()); mechanism = single-pass streaming + in-place rewrite (micro-A/B: 7.2 vs 2.0 GB/s page-cache tax).** 655 asserts/14 cases + resources 78/78 (crdr sort swap); truncation/malformed-JSON adversaries → clean statuses. 5-config ladder ✓. Board `docs/bench/2026-07-05-v14l-io.md`. | ~1700 (act ~2100) | ~70 (655) |
| **v14-m** ✅ core SHIPPED (2026-07-05, parallel wave) | **NN inference pack — ★★the certified tiny-ML demo.** **▶ SHIPPED `nn.hpp`:** conv2d (im2col over the v14-h direct GEMM **+ a bit-chain-identical direct 3×3/s1/p1 kernel**) · max/avg pool · relu/gelu/tanh/sigmoid (ALL via `crd::math` — never `std::`) · layernorm + stable softmax (torch semantics) · linear f32 (two tiers: register-tiled direct at 150 GF/s / `gemm_parallel` above 96 Mflop) · **★the Q8_0 QUANTIZED path** (`q8_dot_tile`: exact i32 lane partials, maddubs/madd + bit-identical AVX-VNNI `dpbusd` alternative, pinned sum tree, scalar mirror) · `NnSequential` (OWNED weights, bounded op list, **relu+maxpool fusion at finalize**, documented workspace formula, **allocation-free infer** — CountingAllocator-gated) · safetensors→MLP/CNN builders (f32 + quantized). **Gates: torch parity 5.96e-8/4.47e-8 (≤1e-6); conv2d+linear BIT-EXACT vs k-ordered fma-chain refs; Q8_0 quantize-on-load BYTE-EXACT vs all six frozen refs; q8 err vs f32 4.56e-3/5.15e-4 argmax-preserved; `{1..16}` moat f32+q8 both models; 4,988 asserts/16 cases.** **⭐⭐ BOARD: f32 8/8 WINS — torch 1.9–9.7×, onnxruntime 1.15–2.1× (initial ort CNN losses PROFILED + FIXED: direct 3×3 kernel, conv+relu+maxpool fusion ≈100 MB traffic killed); quantized 7/8 — torch-int8 6.6–10.2×, ort-int8 1.27–9.3×, and llama.cpp BUILT FROM SOURCE: ggml Q8_0 beaten 1.22–1.48× per layer at native ISA (our whole q8 net 398 ns/row < ggml's three linears alone 460).** The 8th cell was CLOSED same evening: **the per-tensor i8 throughput tier (packed MLAS-structure kernel, u8-asym activations + integer zero-point via weight colsums) = 155.1 ns vs ort-int8 191.1 = 1.23× AT BETTER ACCURACY (4.47e-3 vs their 4.74e-3); full i8 column 1.23–9.6× vs ort-int8, 2.8–15.7× vs torch-int8; the tier sweep also took Q8_0 398→342 ns. EVERY CELL ON EVERY v14-m BOARD WON.** Suite now 18 cases/5,194 asserts (gcc avx2 + avxvnni + ASan/UBSan). **D-v14m-1** (corpus quantizer f16-scale-inversion + half-to-even vs the ggml transcription) documented in-header. 5-config ladder ✓ (gcc both ISA variants + debug/asan/shipping/tidy; module 126/126 env-free). Board `docs/bench/2026-07-05-v14m-nn.md`. | ~2400 (act ~2900) | ~75 (4,988 asserts) |
| **v14-z** | **CLOSE.** CLI `hesap.tensor.*` + system doc + ADR-0096 finalized + the all-peers scoreboard (incl. TBLIS/HPTT/SPLATT/ReproBLAS rows) + the conformance audit + the `{1..16}` moat sweep. | ~900 | ~45 |

**Spine:** a substrate → b elementwise → c reductions (the moat lands here) → d permute → e/f/g einsum
(front-end → execution → hyper-optimizer) → h batched LA → i sparse → j/k decompositions → l I/O → m NN → z close.
b–d unblock e–f; c's Tier D/R machinery is what every later reduction rides. i–k are independent of e–g and can
interleave; l needs only a; m needs b/c/d/l.

## 5. Session log

- **2026-07-02 — kickoff.** ADR-0096 written, arch-reviewed (7 amendments folded in: PRIVATE link edges +
  include-only Philox for lean consumers · StorageTensor-vs-opaque-blocked low-precision split · rank-8 failure =
  runtime `RankOverflow` + plan-build constraint · DLPack = element strides + RAII import owner · Tier-D grain =
  f(shape) never f(num_workers) · SR keyed by canonical destination logical index + 3-tuple for accumulation ·
  `AllocFailed` status + zero-heap scoped to the execute path) + Accepted; this detail doc; ADR index row.
  **v14-a increment 1 SHIPPED: the view substrate** — `engine/hesap-tensor` module (`tensor.hpp`:
  `Tensor<T>`/`TensorView<T>`, kMaxRank=8, element strides signed/stride-0, slice/select/flip/permute/
  broadcast_to/reshape/for_each, NumPy zero-size + size-1-dim contiguity semantics, overflow-safe resize,
  `TensorStatus` incl. AllocFailed) + **the NumPy view-semantics corpus gate** (scripts/v14a_view_corpus.py →
  9 test cases / 255 assertions baked as plain C arrays) — **green on win-debug (MSVC /WX) + linux-gcc-release
  (-Werror)**. Boundary catch: zero-size `{2,0,4}` contiguity (NumPy calls every empty array contiguous).
  (Same session: the CI moat-hang root-cause — `docs/sessions/2026-07-02-jobs-semaphore-lost-wake.md` —
  cleared the gate for this kickoff.)
- **2026-07-02 — v14-a increment 2 SHIPPED: the dtype set** (`dtypes.hpp`). One parameterized bit-exact RNE
  narrower/widener (EBITS/MBITS/fn) ⇒ **f16 + FP8 e4m3fn/e5m2 BIT-EXACT vs ml_dtypes 0.5.4** (the
  JAX/safetensors frontier — the corpus itself pinned e4m3fn overflow→NaN, not saturate) + the bf16 carry-trick;
  **exhaustive 256-pattern fp8 decodes + encode(decode) idempotence**; **ggml Q8_0/Q4_0 BYTE-EXACT**
  (`BlockQ8_0` 34 B / `BlockQ4_0` 18 B — layouts + arithmetic transcribed from the fetched
  `ggml-quants.c` `quantize_row_*_ref`; the v14-m weights-interop on-ramp); **★deterministic SR** for all four
  formats (Philox keyed `(seed, canonical dest index, step)`, include-only edge — no hesap-stats link; exact
  linear-grid SR = add-uniform-below-discarded-bits + truncate; saturates, never manufactures inf/nan);
  `StorageTensor<Dtype>` (element-addressable, stride-header, compute-forbidden) with strided-source converts +
  **SR stride/chunk-independence gated** (permuted view ≡ materialized copy, bit-identical). Tests: **15 cases /
  3,369 asserts green on win-debug /WX + linux-gcc-release -Werror** (gcc caught one sign-compare the MSVC pass
  missed — the pipeline-masked-failure lesson: never trust `cmd | tail` exit codes). **Baseline bench**
  (1M elems, 1 pinned core, vs single-threaded peers): bf16 0.126 ns/elem = **2.9× ml_dtypes / 3.6× torch WIN** ·
  f32→f16 1.17 ns edges numpy (1.28) · e4m3 ≈ parity with ml_dtypes (1.40 vs 1.35) · **OPEN: torch's F16C f16
  path is 0.156 ns (7.5× ahead) — the SIMD/F16C batch pass is the named crush lever and BLOCKS v14-a close**
  (SANITY #9: a measured loss is an open bug; captured in `scripts/run_dtype_bench.sh` output).
  **▶ THE CRUSH PASS LANDED (2026-07-02, same session): batch kernels flipped every loss** — F16C f16 both
  directions + a generic 8-wide AVX2 integer transcription of `narrow_rne` (fp8) + scalar NaN semantics aligned
  to VCVTPS2PH (payload truncate+quiet) so **SIMD ≡ scalar bit-identity is gated over the corpus + 100k Philox
  random patterns × 4 formats** (suite 16 cases / **103,496 asserts**, green win-debug /WX + linux-gcc).
  SR keying PINNED to lane-packed form (block=idx>>2, lane=idx&3). **The board (1M elems, 1 core):
  f32→f16 0.106 ns = 1.29× torch-F16C / 11.5× numpy WIN · f16→f32 0.097 = 6.9× numpy WIN · bf16 0.139 =
  2.5×/3.1× ml_dtypes/torch WIN · e4m3 0.608 = 2.2× ml_dtypes WIN (was parity) · e5m2 0.621.**
  REMAINING in v14-a: Q8_0-vs-ggml-native bench (v14-m per the locked table) · win-tidy at slice close.
- **2026-07-02 — the SR crush + conformance items CLOSED.** **Batch SR shipped**: `narrow8_sr_avx2` in crd-math
  (SIMD SR narrower, saturating, deep-underflow lanes scalar-patched via a lane mask) + draw-agnostic
  `convert_f32_to_{f16,bf16,e4m3,e5m2}_sr(src,dst,rand)` batch APIs (mechanism in crd-math) + tensor-side
  keyed wrappers pulling **32 Philox draws per call from the hesap-stats AVX2 8-block kernel** (its transposed
  u32 output is exactly the pinned block/lane layout; zero heap — 256-elem stack chunks); scalar SR's
  deep-underflow boundary moved to shift>31 so SIMD and scalar share the direct 32-bit form lane-for-lane
  (P(up)=rem/2^shift exact either way). **SR: 7.11 → 1.20 ns/elem (5.9×) — deterministic SR now costs LESS
  than numpy's plain RNE f16 convert (1.22 ns); bf16 SR 0.86 · e4m3 SR 1.19; no external peer ships
  deterministic SR (stated).** Full board (gcc, 1M/1-core): f16 0.099 (1.5× torch) · f16→f32 0.103 (6.4×
  numpy) · bf16 0.120 (3.0×/4.1× ml_dtypes/torch) · e4m3 0.596 / e5m2 0.604 (2.3× ml_dtypes). Batch-SR ≡
  scalar bit-identity gated over corpus+100k randoms ×4 formats (chunk seams + tails exercised).
  **Conformance: the untagged-physical-numeric guard PASSES with NO exemption needed** (it is name-pattern-
  based, not module-based — the arch-review's assumption corrected) · **`smoke_hesap_tensor` shipped = the
  link-isolation gate** (links ONLY core/containers/memory/math + tensor; a future hesap-stats/dense link-drag
  breaks the link) — runs OK · **win-asan: 16 cases / 103,496 asserts, ZERO errors** · win-debug + linux-gcc
  green · `StorageTensor::convert_from{,_sr}` contiguous fast paths ride the batch kernels.
  **Reuse — MIGRATED 2026-07-02 (user direction, ahead of the second-consumer trigger):** the format
  primitives now live in **`crd/math/float_convert.hpp`** (crd-math = the owning module, SANITY #8): scalar +
  F16C/AVX2 batch f16/bf16/FP8 converts and the *random-supplied* SR cores (`std::span` APIs — no containers
  edge). `crd-hesap-tensor/dtypes.hpp` is the POLICY layer: re-exports + the Philox SR keying contract
  (canonical destination index; include-only Philox edge) + ggml Q8_0/Q4_0 blocked storage + `StorageTensor`
  (contiguous fast path rides the crd-math batch kernels). En route: **`-mf16c` added to `crd-simd-flags`**
  (GCC/Clang need it explicitly; MSVC /arch:AVX2 implies it; guarded via `CRD_MATH_HAS_F16C` with scalar
  fallback so no consumer can ever fail to compile) — and the migration made the SIMD≡scalar gate REAL on
  MSVC (the AVX2 flags now reach the test TU), which caught + fixed the widening-side NaN divergence
  (scalar aligned to VCVTPH2PS payload propagation). Suite 16 cases / **103,496 asserts green win-debug +
  linux-gcc, hardware paths active on BOTH**; the crush board re-verified post-migration (f16 0.102 ns =
  1.30× torch · f16→f32 0.106 · bf16 0.122 · e4m3 0.598 = 2.2× ml_dtypes).
