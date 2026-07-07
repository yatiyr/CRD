# crd-hesap-tensor — the N-D tensor substrate (Phase 3.1.6 v14)

> **Status: cores a–m SHIPPED (2026-07-02 → 2026-07-05).** v14-a CLOSED on the full 4-config DoD;
> g/h/i/j/k/l each closed on the 5-config ladder (linux-gcc · win-debug · win-asan ·
> win-shipping/LTCG · win-tidy); b–f green on their recorded per-slice config sets (see the phase
> rows); v14-m's 16-case gate suite is green under BOTH `-mavx2`-only and `-mavxvnni` builds (its
> board) — its phase rows and config-ladder record are not yet written (flagged in the scoreboard
> audit notes). ADR-0096 (+ Amendments 2026-07-05). Plan: `docs/phases/phase-3.1.6-v14.md` (slice
> contract + verdicts); master rows in `phase-3.1.6-hesap.md`. Boards: the consolidated all-peers
> scoreboard `docs/bench/2026-07-05-v14z-scoreboard.md` (links every per-slice board). Sessions:
> `2026-07-05-v14g-hyperopt-crush.md`, `2026-07-05-v14h-batched-la.md`,
> `2026-07-05-v14-parallel-wave.md`. Remaining v14-z integration items (open, stated loud): the CLI
> gate tests (`test_cli.cpp` per the v13-z pattern — the 12 commands + anchor ARE shipped, below) ·
> the >16-operand einsum→hyper-optimizer bridge · the whole-engine per-slice-check sweep over the
> final artifact · the TBLIS/TCL oracle (N/A-with-the-check carried).

## What it is

The engine's N-D array layer — "the engine's NumPy" — and the substrate v15/v16 autodiff, 3.1.11
batched EKF/sensor fusion, CFD/FEA element batches + POD/HOSVD ROMs, TT-compressed LUTs/ephemerides,
certified tiny-ML inference (DO-178C/EASA), medical volumes, DAW spectrogram batches, and games
skinning/SH stand on. One module, ~22 KLOC of headers + two src TUs, dense-to-sparse-to-decomposition
complete. The moat no incumbent ships ANY piece of: **`{1..16}`-worker bit-identical parallel ops
(Tier D)** · an **opt-in partition-independent reproducible reduction tier (Tier R,
ReproBLAS-class)** · **deterministic stochastic rounding** (reproducible-by-seed) ·
**deterministic-randomized decompositions** · **integer inference bit-exact across all hardware** —
carried on the v13 certification pillars (caller workspaces / zero heap on execute, status-not-
exception + `noexcept`, bounded iteration everywhere).

## Architecture (ADR-0096)

- **One module, internal sub-headers**; heavy deps (hesap-dense, hesap-sparse) are consumed
  HEADER-ONLY by deliberately header-only sub-headers, so the built lib carries no dense/sparse link
  edge at all — gated by the link-isolation smoke (`smoke_hesap_tensor` links only
  core/containers/memory/math + tensor). Philox is include-only (never a stats link).
- **The two-tier dtype model.** COMPUTE dtypes are template parameters: `Tensor<T>`/`TensorView<T>`,
  `T ∈ {f32, f64, c32, c64}` (+ `i64` index / `u8` mask tensors) — no runtime-dtype VM. STORAGE
  dtypes are explicit converts: element-addressable f16/bf16/FP8-e4m3/e5m2 ride `StorageTensor<D>`
  (same rank-8 stride header, **compute-forbidden**, reachable only via convert/quantize/io/DLPack);
  int8/int4 block-quantized (ggml-compatible `BlockQ8_0` 34 B / `BlockQ4_0` 18 B, per-block f16
  scale, block 32) is a distinct OPAQUE blocked representation with no element strides. F16C/AVX2
  batch converts; compute always runs f32/f64 — with ONE deliberate exception: v14-m integer
  inference computes on int8 natively (bit-exact across all hardware = the strongest certification
  tier).
- **NumPy stride-view semantics on a bounded-rank header** (`kMaxRank = 8`): fixed shape/stride
  arrays (allocation-free metadata, WCET-analyzable), strides in ELEMENTS, signed (negative = flip),
  stride-0 broadcasting, contiguity tracking. Rank-8 overflow is a runtime `RankOverflow` status,
  never a crash. `Tensor<T>` owns via `IAllocator*` (64-byte aligned); `TensorView<T>` is non-owning
  under the engine-wide Span discipline.
- **The einsum plan pipeline**: parser (ellipsis, repeated-index diagonals, implicit NumPy output
  rule; ≤16 operands / 34 index bits) → path optimizer (exact branch-and-bound ≤7 operands,
  multi-heuristic greedy beyond) → **`EinsumPlan` build-once/execute-many** (8.15 µs/plan; NumPy
  re-plans every call) → TTGT execution over the engine's own deterministic GEMM (`gemm_parallel`,
  ADR-0063 fixed order) + direct kernels for small/odd shapes (register-blocked small-M, thin-K).
  Above that tier sits the **cotengra-class hyper-optimizer** (`hyperopt.hpp`): random-greedy trials
  + labels-divide partition trees (no kahypar dependency) + subtree-reconfigure (exact subset-DP) +
  treesa SA + **SliceFinder with an EXACTLY-honored memory bound** (`NotFound`, never best-effort) —
  dynamic slicing is the WCET pillar applied to einsum. The >16-operand einsum→hyperopt dispatch
  bridge is the open v14-z wiring item.
- **Status contract**: `TensorStatus {Ok, BadInput, RankOverflow, ShapeMismatch, NotContiguous,
  AllocFailed, Unsupported}` — frozen this cluster; iterative ops report non-convergence via per-op
  channels (batched LAPACK-style per-matrix `info` lanes · `DecompStatus`/`CpInfo`/`TuckerInfo` ·
  `MaxvolInfo`/`TtCrossInfo`; ADR-0096 Amendment 1).

## Shipped surface (by sub-header)

- **`tensor.hpp`** (v14-a) — `Tensor<T>`/`TensorView<T>`, zero-copy
  `slice/select/flip/permute/broadcast_to/reshape`, `for_each`, NumPy contiguity incl. zero-size,
  overflow-safe `resize`.
- **`dtypes.hpp`** (v14-a) — the low-precision POLICY layer over `crd/math/float_convert.hpp` (the
  primitives' home module, SANITY #8): f16/bf16/FP8 scalar + F16C/AVX2 batch converts (bit-exact vs
  ml_dtypes; exhaustive fp8 decode/idempotence), ggml `quantize/dequantize_q8_0/q4_0` (byte-exact),
  `StorageTensor<D>`, and **deterministic SR** converts — Philox keyed `(seed, canonical destination
  index, step)`, lane-packed layout pinned; SIMD ≡ scalar bit-gated.
- **`elementwise.hpp`** (v14-b) — full NumPy broadcast rules; all-stream dim collapse + the P0/P1/P2
  engine (flat-SIMD / splat-run-SIMD / scalar) on Vec8f/Vec4d; `ew_binary/ew_unary/ew_compare/
  ew_where/ew_cast`, sign-bit neg/abs (NaN-payload exact). Min/Max = IEEE hardware semantics
  (pinned; NumPy's NaN-propagating variants on consumer demand).
- **`reduce.hpp` + `reduce_axes.hpp`** (v14-c) — Tier-D fixed-order trees
  (sum/prod/min/max/mean/argminmax/cumsum/logsumexp; grain = f(shape) ONLY); **Tier-R**
  `reduce_sum_reproducible` (faithful ReproBLAS v2.1.0 fold-3 transcription + 12-accumulator SIMD +
  speculative single-DRAM-pass); **SR accumulation** `reduce_sum_sr_bf16`; general axes-mask
  reductions with VERTICAL/ROW/GENERAL dispatch + argmin/argmax-axis + cumsum-axis.
- **`permute.hpp`** (v14-d) — `permute_copy(src, order, dst, alpha=1)`: dst-ordered dim collapse →
  memcpy-row / row-loop / 32×32 tiled transpose with AVX2 8×8 f32 microkernel; src-locality
  odometer, stride-aware tile edge; MT = disjoint super-block macro-tasks (NT stores + sfence,
  full-column staged strips for scattered columns); alpha=1 is a pure bit copy.
- **`einsum.hpp` / `einsum_exec.hpp`** (v14-e/f) — `einsum_parse` + `einsum_plan_build` +
  `einsum_execute` (header-only by design — the link-isolation lever); zero-copy stride-sum
  diagonals, private-index pre-sum through the Tier-D reducer, TTGT copy avoidance,
  consumer-aware layouts.
- **`hyperopt.hpp`** (v14-g) — `HyperNet`/`HyperTree`/`SliceFinder`, `hyper_optimize` driver
  (stratified Philox-keyed trials over crd-jobs, per-trial reconfigure, SA finalists),
  `hyper_greedy`/`hyper_labels_divide`/`hyper_slice`.
- **`batched.hpp`** (v14-h) — `batched_gemm` (register-tiled tiny tier with the k-ordered
  single-rounded fma-chain bit contract; dense large tier), `batched_cholesky_factor/solve` +
  `batched_lu_factor/solve` (lane-batched AoSoA; per-lane pivoting via pure-vector argmax — the
  MSVC-autovec scar codified), `batched_svd_small` (one-sided Jacobi, bounded sweeps, per-matrix
  info lanes); tier bit-identity + poison isolation gated.
- **`sparse.hpp` / `sparse_mttkrp.hpp` / `sparse_cp.hpp`** (v14-i) — `SparseCoo` +
  `SparseCooBuilder` (stable LSD counting sort; bit-reproducible `compress()`), `SparseCsf` (SPLATT
  mode tree), `mttkrp` (register-fused CSF walk; CSF ≡ COO bit-identity), `contract_mode` (TTM),
  sparse add/mul (+dense variants, branchless packed-key merges), reductions with dense semantics,
  and the `SparseCpMttkrp` functor + `cp_als_sparse` glue into v14-j's seam.
- **`decomp.hpp`** (v14-j) — `cp_als` + **`cp_als_generic` over the MTTKRP functor seam**,
  `hosvd`/`hooi`, `hosvd_rand`/`hooi_rand` (Philox-keyed HMT sketches ⇒ deterministic-randomized);
  the Gram kernel (G=AAᵀ + `eig_sym`) + Gram-operator power iteration (the 175× factor-kernel
  levers, documented as D(v14j)-1…4).
- **`tt.hpp`** (v14-k) — `TtTensor` (cores OWNED contiguous), `tt_svd` (QR-first wide unfoldings),
  `tt_round`, add/hadamard/dot/norm/contract, **allocation-free eval** (`tt_eval/_many/_lerp`,
  workspace = 2·max_rank), `maxvol` (Goreinov-Oseledets), **`tt_cross`** (callback-driven — no
  materialized tensor; Philox-deterministic, bounded sweeps). Serial-only v1, stated in-header.
- **`io.hpp` / `detail/io_zip.hpp` / `dlpack.hpp`** (v14-l) — npy r/w (writer byte-identical to
  `np.save`), npz (STORED write = np.savez parity; STORED + DEFLATE read via own RFC-1951 inflate,
  CRC-32 gated), safetensors r/w (spec-complete deterministic JSON parser; bf16/fp8 carriers),
  DLPack producer/consumer (zero-copy pointer-equal, versioned + legacy ABIs, the deleter lifetime
  contract; vendored `detail/dlpack.h` v1.1), `philox_fill_uniform` (order-independent keying),
  `IoDtype` (PINNED append-only tag set).
- **`nn.hpp`** (v14-m) — conv2d (direct 3×3/s1/p1 kernel bit-exact to im2col+GEMM, fused
  relu+maxpool), max/avg pool, activations over `crd::math`, layernorm/softmax, f32 + **Q8_0
  quantized linear** (exact i32 lane partials, `dpbusd` under AVX-VNNI bit-identical to AVX2),
  `NnSequential` (owned weights, `workspace_bytes()` formula, zero-alloc infer),
  `build_mlp/cnn_from_safetensors`; D-v14m-1 quantizer divergence documented in-header.
- **`crd-hesap-resources/tensor_artifact.hpp`** (v14-l, the bridge module) — the CRDR `'TNSR'`
  artifact (`'TNHD'` 96-B pinned `TensorFileInfo` header + FNV-1a-64 payload hash; HMTX mirror),
  `cook_tensor`/`cook_tensor_bits`, `TensorResource`.
- **`src/cli_register_tensor.cpp` + `cli_anchor.hpp`** (v14-z) — the CLI (next section).

## The determinism moat (the five named tiers — never conflated)

1. **Tier D (default, zero-cost): fixed-order reduction/contraction trees, serial ≡ parallel** —
   bit-identical across `{1..16}` workers; block size and combine order are f(shape) ONLY, never
   f(num_workers). Gated per op family (13 ctest moat gates — the conformance audit in the
   scoreboard doc names each). Scope: same binary / same SIMD width.
2. **Tier R (opt-in): ReproBLAS-class binned summation** — bit-reproducible INDEPENDENT of
   partitioning; gated under forced repartition ({3,7,16} chunks × fwd/rev merge) + full shuffle;
   measured **faster than ReproBLAS itself** (1.60–1.63× @1M, 1.01–1.23× @16M) — reproducibility now
   costs less than a naive serial sum at DRAM sizes.
3. **Deterministic stochastic rounding** — Philox keyed by (seed, canonical destination index
   [, accumulation step]) ⇒ order/partition-independent AND reproducible-by-seed; stride/chunk
   independence gated; costs LESS than numpy's plain RNE f16 convert. No peer ships deterministic SR
   (checked: numpy/ml_dtypes/torch).
4. **Deterministic-randomized decompositions** — `hosvd_rand`/`hooi_rand` sketches, `tt_cross`, and
   the hyper-optimizer's trials are counter-keyed: same seed ⇒ bit-identical at any worker count.
5. **Integer inference** — Q8_0 path bit-exact across ISAs (AVX2 vs AVX-VNNI gate-verified
   identical), quantize-on-load byte-exact vs frozen refs, run-twice + `{1..16}` gated: the
   certification tier NumPy-MKL/PyTorch/onnxruntime structurally lack.

## Crush summary (details + every caveat: `docs/bench/2026-07-05-v14z-scoreboard.md`)

**203 measured comparison rows (+33 einsum path-quality parity-or-better cases): 194 won, 5
tie/parity, 4 open** (each open cell mechanism-pinned:
2× einsum GEMM-bound numpy cells + 1 torch cell = the v0d f64 GEMM rate row; 1 ort-int8 per-tensor
cell = quantization-scheme cost, per-tensor tier in progress). Headlines: converts to 12.3× numpy /
1.5× torch-F16C · Tier-R 1.6× ReproBLAS · permute beats HPTT 1T 3/3 (8T: 2 wins + 1 parity) · paths
≤ opt_einsum 33/33 at 9–41× lower planning cost · hyperopt beats cotengra's full stack 1.81–5.83×
with BETTER trees 6/6 and cotengrust 1.07–2.17× · batched LA sweeps MKL/torch/MATLAB (chol
2.51–8.48×, SVD to 12×; one documented DRAM-wall GEMM tie) · sparse 13/13 incl. SPLATT 1.5× on its
own kernel and TACO 2.3–2.8× · decomp 12/12 vs TensorLy + MATLAB TTB at equal-or-better fit · TT 8/8
vs tntorch + the 1748×-compression LUT demo that is also 1.50× faster than the table it replaces ·
I/O 12/12 vs numpy/safetensors-py · NN f32 8/8 vs torch+ort, quantized 7/8 + ggml (same format,
native build) beaten 1.22–1.48× on every layer.

## Verification protocol (every slice; the conformance audit lives in the scoreboard doc)

- **Reconstruct-and-verify-in-python FIRST** for every ported/matched algorithm (cotengra internals,
  ReproBLAS fold-3, SPLATT/maxvol/TT-cross, tensorly parity, opt_einsum's optimal-search
  inconsistency — found and exploited); oracles frozen as plain C arrays (`ref_*.inc`, generated by
  tracked `scripts/v14*_*.py`).
- **Full peer board per row** at matched threads, bit-identical inputs where cross-tool, format prep
  excluded on every side; N/A stated WITH the check; a loss or tie = an open bug (three losing rows
  in v14-i, one in v14-k, and all nine first-board v14-j rows were SOLVED in-session, not recorded).
- **The moat gates**: `{1,2,4,8,16}` bit-identity on every parallel op + run-twice everywhere;
  Tier R additionally under forced repartition + shuffle; SR under stride/chunk permutation;
  tier bit-identity (SIMD ≡ scalar, lane ≡ scalar incl. pivot sequences, CSF ≡ COO).
- **Per-slice Windows verification from day one** (the v13-z scar): slices land win-debug-green
  minimum and close on the recorded config ladders (g–l: the full 5-config ladder); per-slice
  verification runs ctest (guards are ctest-registered). The
  whole-engine `per-slice-check.ps1` sweep is deferred to the v14-z close over the final artifact —
  still pending at docs-close time.
- Module guards: the link-isolation smoke · no-std-containers (tests included) · ASCII test names ·
  named allocators in tests.

## Reuse edges (SANITY #8 — consumed, never duplicated)

- **v0d GEMM** (`gemm`/`gemm_parallel`, ADR-0063 bit-locked order) — einsum TTGT, batched large
  tier, NN linear/conv dense tier.
- **hesap-dense** SVD/QR/LU/chol/`eig_sym` — decomp factor kernels, tt unfolding SVDs, batched
  per-matrix tier (header-only consumption; no link edge).
- **hesap-stats Philox counter-RNG** (include-only) — SR keying, `philox_fill_uniform`, decomp
  sketches, tt_cross, hyperopt trial streams; the v12 AVX2 8-block kernel feeds batch SR.
- **crd-jobs** deterministic `parallel_for` — every Tier-D parallel driver (PUBLIC link edge).
- **crd-math** — SIMD (Vec4d/Vec8f, incl. the v14-h `load_partial/store_partial` additions),
  `float_convert.hpp` (the migrated f16/bf16/FP8 + SR-core primitives), deterministic
  transcendentals in NN activations/softmax.
- **crd-hesap-resources** — the `'TNSR'` CRDR artifact (ADR-0084 pattern); resources depends on
  tensor, never the reverse.
- **v13 contracts** (ADR-0095) — caller workspaces / zero-heap execute, status-not-exception,
  bounded iteration; the v13-interp LUT regime is the TT demo's consumer.

## CLI (`hesap.tensor.*`, v14-z — one command per op family, the v13-z pattern)

A smoke/demo surface (proves the op families, does not benchmark them): inputs are SYNTHETIC
(deterministic Philox fills keyed by `seed`) or FILES (io/nn); every command prints a compact
deterministic Text result (shapes + checksums; wall time rides a Hint diagnostic so the Text stays
bit-stable run to run). Commands: `hesap.tensor.einsum.f64` (parse/plan/execute) ·
`hesap.tensor.ew.f64` · `hesap.tensor.reduce.f64` (full or axes bitmask) ·
`hesap.tensor.permute.f64` · `hesap.tensor.batched.f64` (gemm|cholesky|lu|svd) ·
`hesap.tensor.hyperopt` · `hesap.tensor.sparse.mttkrp.f64` · `hesap.tensor.decomp.f64` ·
`hesap.tensor.tt.f64` · `hesap.tensor.io.info` · `hesap.tensor.io.philox.f64` ·
`hesap.tensor.nn.f32` (f32|q8 tiers from .safetensors). Anchor `register_tensor_cli_anchor()` —
the TU's object enters a consumer link ONLY when the anchor is referenced (the static-lib anchor
pattern), so the link-isolation smoke stays intact; consumers referencing it link crd-hesap +
crd-hesap-dense + crd-platform themselves. **CLI gate tests (`test_cli.cpp`) are the open item.**

## Module edges (acyclic)

`crd-hesap-tensor` → PUBLIC: `crd-core` · `crd-containers` · `crd-memory` · `crd-math` · `crd-jobs`;
PRIVATE: `crd-warnings` only. hesap-stats = include-only (PUBLIC include dir; philox.hpp is
header-only constexpr); hesap-dense/hesap-sparse = header-only consumption inside header-only
sub-headers (no link edges — enforced by `smoke_hesap_tensor`); the CLI TU additionally consumes
crd-hesap / hesap-dense / crd-platform include-only, anchor-gated (above). `crd-hesap-resources`
bridges tensor ↔ resources from above (depends on tensor, never the reverse).

## Tests (`tests/hesap-tensor/` — 18 test files, ctest-registered)

Per-slice gate counts as recorded at each close: view corpus 255 asserts/9 cases · dtypes 103,496/16
(+ dtype-set 174) · elementwise (module suite then) 104,415/22 · reduce+axes (module suite then)
109,047/31 · permute 1,389/10 · einsum paths 263 · einsum exec 5,957 (incl. kernel-shape gates) ·
hyperopt 802/11 (+ the frozen cotengra corpus gate) · batched 3,362/8 · sparse 906/12 (incl. the
16-assert sparse-CP glue) · decomp 191/8 · tt 280/9 · io 655/14 (+ resources 78/78) · nn 4,988/16.
Oracle generators + bench harnesses tracked under `scripts/` and `build/crd_*_{gate,bench}.sh`;
peer oracle state in `external/PEER_ORACLES.md`.
