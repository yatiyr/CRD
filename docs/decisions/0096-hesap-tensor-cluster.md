# ADR-0096 — crd-hesap-tensor: the N-D tensor substrate — templated compute dtypes over stride views, the two-tier deterministic-reduction contract, deterministic stochastic rounding, and the certified-inference lane

- **Status:** Accepted (2026-07-02)
- **Phase:** 3.1.6 v14 (the TENSORS cluster)
- **Tags:** `hesap` `tensor` `einsum` `determinism` `reproducibility` `quantization` `ml-inference` `safety-critical` `architecture` `substrate`
- **Plan:** `docs/phases/phase-3.1.6-v14.md` (the v14 DETAILED PLAN — sub-slice table a–m+z + architecture); master rows in `phase-3.1.6-hesap.md`; memory `project_v14_v18_planning`.

## Context

v14 gives the engine its N-D array layer — the substrate v15/v16 autodiff, the 3.1.11 batched-EKF/sensor-fusion work, CFD/FEA element batches + POD/HOSVD ROMs, TT-compressed LUTs/ephemerides (the v13 tie-in), certified tiny-ML inference (DO-178C/EASA), medical volumes, DAW spectrogram batches, and games skinning/SH all stand on. Scope was locked 2026-07-02 (maximal, user-approved, re-scoped from the ~2800-LOC sketch): a–m+z, ~24.1 KLOC / ~880 tests. The moat is timely — deterministic inference is a hot 2025-26 topic, and NumPy-MKL/PyTorch/TBLIS ship **none** of `{1..16}` bit-identical parallel ops, partition-independent reproducible reduction, or reproducible-by-seed stochastic rounding. Five things had to be settled before line one.

1. **One module or several** — dense ops, sparse tensors, decompositions, and NN inference could each be a module.
2. **The dtype architecture** — how f16/bf16/FP8/int4/int8 storage coexists with f32/f64/c32/c64 compute without a NumPy-style runtime-dtype VM.
3. **The view/ownership model** — NumPy stride semantics vs owning containers, and where DLPack interop fits.
4. **What "deterministic" means for reductions** — thread-count-invariant is not the same claim as partition-invariant; naming them separately is the honest-scoreboard scar applied to reproducibility.
5. **The reuse boundary** — v14 must ride the v0d GEMM, hesap-dense factorizations, the v12 counter-RNG, crd-jobs deterministic `parallel_for`, and the v13 workspace/status contracts, never duplicate them (SANITY #8).

## Decision

1. **ONE module, `crd-hesap-tensor`, with internal sub-headers** (`tensor.hpp` substrate · `elementwise/reduce/permute` · `einsum` · `sparse` · `decomp` · `io` · `nn`). The capabilities share one substrate type and one dtype story; module-splitting them would force the substrate public in fragments. The lean-consumer lever is **CMake link-edge visibility, not dead-strip**: TU-local heavy deps (hesap-sparse for `sparse`, hesap-dense for `decomp`) link **PRIVATE**, gated by a link-isolation smoke proving a dense-only consumer drags none of them; the Philox counter-RNG is consumed **include-only** (`philox.hpp` is header-only constexpr with leaf includes — the units→math precedent), so the substrate's SR never link-drags the hesap-stats→special stack (module-isolation cornerstone honored *within* the module).

2. **Templated compute dtypes; storage dtypes are explicit converts — no runtime-dtype VM.** `Tensor<T>`/`TensorView<T>` are templated on the COMPUTE dtype `T ∈ {f32, f64, c32, c64}` (+ `i64` index / `u8` mask tensors), the hesap house pattern. The low-precision set splits in two: **element-addressable storage (f16, bf16, FP8 e4m3/e5m2 per OCP)** rides a `StorageTensor` carrying the SAME `kMaxRank` shape/stride header — compute-forbidden, reachable only via convert/quantize/dequantize/io/DLPack (safetensors + DLPack carry bf16/f16 as N-D *strided* tensors, so a flat buffer is too weak) — while **int8/int4 block-quantized (ggml-compatible: per-block scale, block 32 default)** is a distinct OPAQUE blocked representation that explicitly does NOT ride element strides. F16C converts where available; compute always runs f32/f64. A runtime-dtype dispatch VM (NumPy's) buys generality the engine doesn't need; integer inference (v14-m) runs on int8/int4 directly — the one deliberate exception, because **integer compute is bit-exact across ALL hardware trivially: the strongest certification tier**.

3. **NumPy stride-view semantics on a bounded-rank header.** Shape/strides are fixed arrays of **rank ≤ 8** (`kMaxRank`) — allocation-free view metadata, WCET-analyzable, covers every named consumer (NCHW conv = 4, batched EKF = 3, TT cores = 3–4, volumes = 3–5). **The rank-8 failure mode is a runtime status, never a crash**: data-driven paths (einsum intermediates — a pairwise contraction of rank-8 operands can exceed 8 — reshape/broadcast, npy/safetensors/DLPack import) return `RankOverflow`; the einsum path optimizer treats `kMaxRank` as a **hard plan-build constraint** (prefers orders keeping intermediates ≤ 8, rejects at build, never at execute); the variadic accessor static_asserts. NumPy-conformance + import gates are scoped to rank ≤ 8. Strides are in ELEMENTS (typed `T*` arithmetic), signed (negative = flips), stride-0 broadcasting, contiguity tracking; `Tensor<T>` owns via `IAllocator*`, `TensorView<T>` is non-owning with caller-guaranteed lifetime (the engine-wide Span discipline — no refcounted aliasing). DLPack (v14-l) maps **element-for-element (DLPack strides are elements; zero conversion — byte↔element applies only to the NumPy buffer/.npy path)**, with an RAII single-owner import wrapper holding the `DLManagedTensor` deleter (not a refcount) handing out views; gates run a NumPy view/stride-semantics corpus bit-exact.

4. **The two-tier deterministic-reduction contract (named tiers — never conflated).**
   - **Tier D (default): fixed-order reduction trees, serial ≡ parallel** — bit-identical across `{1..16}` workers. The grain that makes this true: **block/tile size and combine order are a function of SHAPE ONLY, never `num_workers`** (block-partials combined in block-index order); scope = same binary / same SIMD width across thread counts. Zero-cost claim; every reduction/contraction ships this.
   - **Tier R (opt-in): ReproBLAS-class binned summation** — bit-reproducible **independent of partitioning** (across binaries/vector widths/machines/chunkings), benched vs ReproBLAS itself.
   - **Deterministic stochastic rounding**: Philox counter-RNG keyed by the **canonical row-major logical index within the destination view's own shape** (from the multi-index — never a traversal/iteration counter, so the key is independent of strides and worker partition); SR into broadcast (stride-0) or aliasing destinations is forbidden. **SR accumulation (v14-c) keys the 3-tuple `(seed, canonical_element_index, accumulation_step)`** where `step` is the position in the Tier-D fixed-order tree (a single reused draw would be a fixed dither, not unbiased SR). ⇒ SR converts and SR accumulation into bf16/f16 are order-independent AND reproducible-by-seed — deterministic SR is a 2024-26 low-precision frontier nobody ships.

5. **einsum = build-once plans over the OWN kernels.** Parser (ellipsis, repeated-index diagonals, output spec) → ★opt_einsum-class path optimizer (greedy + optimal-DP) → **`EinsumPlan` build-once/execute-many** (the v13 precompute lever — NumPy re-plans every call) → execution via **TTGT over the v0d GEMM** + batched-GEMM dispatch + direct kernels for small/odd shapes; HPTT-class blocked SIMD permute (v14-d) feeds it. The **cotengra-class hyper-optimizer with dynamic slicing** (v14-g) makes contraction memory a hard bound — the WCET pillar applied to einsum. GETT-fused kernels land ONLY if the profile names the transpose as the wall (SANITY #5). Python-verified FLOP-count parity vs opt_einsum BEFORE the port.

6. **The v13 pillars carry over, scoped honestly** (ADR-0095 §2): **zero heap on the EXECUTE/hot path** — construction (Tensor alloc, `EinsumPlan` build, slice-intermediate allocation) uses `IAllocator*`; build-once/execute-many + dynamic slicing is precisely what makes execute zero-heap. Status-not-exception (`TensorStatus` incl. `AllocFailed` — an `IAllocator` OOM has no legal `bad_alloc` channel under `-fno-exceptions`; shape-product overflow is defensively `BadInput`), `noexcept`, bounded iteration everywhere (CP-ALS/HOOI/TT-cross carry `max_iters` + status). The einsum planner / cotengra-class partitioner / COO-CSF builders are the classic owning-STL creep points (the v11/v12 scar) — `crd::containers::{Array,HashMap}` only. Sparse (COO/★CSF/★MTTKRP) reuses hesap-sparse TripletBuilder/format patterns; decompositions ride hesap-dense SVD/QR/LU/chol + `interp_decomp`; **randomized decompositions run the v12 counter-RNG ⇒ deterministic-randomized** (same seed, bit-identical `{1..16}` — a moat oxymoron nobody ships); I/O = `.npy`/`.npz` + ★safetensors (real ML weights incl. bf16/f16) + ★DLPack + CRDR `'TNSR'` cook (ADR-0084 pattern).

7. **Per-slice Windows verification from day one** — the v13-z scar: every slice lands with win-debug green minimum, never linux-gcc-only.

## Consequences

- v14-a ships the substrate: `Tensor<T>`/`TensorView<T>` + shape/stride/view machinery + the dtype convert set incl. deterministic SR; gates = the NumPy view-semantics corpus + convert round-trips + SR unbiasedness + SR run-twice bit-identity. v14-a close items: add `crd-hesap-tensor` to the `crd-no-untagged-physical-numeric` exemption exactly as hesap-dense/hesap-fft (raw `T` public surface is the ADR-0078 lower layer) + the link-isolation smoke.
- v15/v16 tape operands are OWNING `Tensor<T>` (ADR-0097 must pin: saved-for-backward intermediates are arena/Tensor-owned, never bare `TensorView`s — backward runs after forward's frames unwound).
- The gold-standard board (full-board rule, matched threads, reconstruct-verify-in-python-first): NumPy(+MKL) / PyTorch-CPU / xtensor (ops) · opt_einsum + cotengra (paths) · **HPTT** (transpose) · **TBLIS/TCL** (contraction) · **SPLATT/TACO** (sparse) · TensorLy / MATLAB Tensor Toolbox / ttpy / tntorch (decomp) · MATLAB `pagemtimes` (batched) · **ReproBLAS** (reproducible tier) · torch / onnxruntime-CPU / llama.cpp-CPU + ggml (inference/quantized).
- v15/v16 autodiff consume `Tensor<T>` + `EinsumPlan` as their tape operands (einsum VJP = einsum with permuted spec); v14-m's op set becomes trainable in v16-c; the v16-i deterministic-training demo trains the v14-m certified controller.
- Extends ADR-0065 (hesap substrate); consumes ADR-0078 (two-layer typed/raw) + ADR-0084 (CRDR cook); sibling to ADR-0094/0095. ADR-0097 (autodiff pair) writes at v15 kickoff. Cluster closes at v14-z (CLI `hesap.tensor.*` + system doc + the all-peers scoreboard + conformance audit + the `{1..16}` moat sweep).

## Amendments (2026-07-05 — the v14-z close; appended, history above unchanged)

**Shipped as designed.** All six decisions held through a–m with no re-litigation: ONE module — and
the §1 lean-consumer lever shipped in a STRONGER form than its wording: the heavy deps (hesap-dense,
hesap-sparse) are consumed HEADER-ONLY by deliberately header-only sub-headers (einsum_exec / batched /
decomp / tt / sparse), so the built lib carries NO dense/sparse link edge at all (PUBLIC edges:
core/containers/memory/math/jobs; the only PRIVATE edge is crd-warnings) — gated by the link-isolation
smoke (`runtime/examples/smoke_hesap_tensor.cpp`, which links ONLY core/containers/memory/math +
tensor and breaks on any future link-drag); Philox include-only as designed (hesap-stats include dir,
never a link);
templated compute dtypes with `StorageTensor` (element-addressable f16/bf16/FP8) split from the opaque
blocked int8/int4 (ggml byte-exact); the rank-8 stride-view header with `RankOverflow`-not-crash;
the two named reduction tiers + deterministic SR, each ctest-gated exactly as specified (Tier-D
`{1..16}` moat per parallel op · Tier-R under forced repartition + shuffle · SR seed/stride/chunk
independence); einsum as build-once plans over the own kernels with the cotengra-class hyper-optimizer
honoring the slicing memory bound EXACTLY; the reuse boundary (v0d GEMM, hesap-dense factors, v12
counter-RNG, crd-jobs, v13 contracts) intact; per-slice Windows verification held (win-debug green
minimum as slices landed; g–l each closed on the full 5-config ladder). Boards: `docs/bench/2026-07-05-v14z-scoreboard.md` (the consolidated
all-peers scoreboard + conformance audit). One Consequences item dissolved rather than executed: the
`crd-no-untagged-physical-numeric` exemption proved UNNECESSARY — the guard is name-pattern-based, not
module-based, and passes with no exemption (the arch-review's assumption corrected at the v14-a close).

**Deviations from the letter of the decision (each deliberate, recorded at the slice that made it):**

1. **`TensorStatus` shipped FROZEN without a `NotConverged` member** (the Decision-block sketch carried
   one). The shipped enum is `{Ok, BadInput, RankOverflow, ShapeMismatch, NotContiguous, AllocFailed,
   Unsupported}`; iterative ops report non-convergence through per-op channels instead — batched LA
   carries LAPACK-style per-matrix `info` lanes (poison isolation: a bad matrix flags its own lane),
   `decomp.hpp` carries its own `DecompStatus` (incl. `NotConverged`) + `CpInfo`/`TuckerInfo`, `tt.hpp`
   carries `MaxvolInfo`/`TtCrossInfo` — the batched/tt/decomp precedent. The bounded-iteration pillar
   is unweakened (budgets + status on every loop). Adding `TensorStatus::NotConverged` is flagged for
   whenever `tensor.hpp` next opens (2026-07-05 parallel-wave log, "agent HOME→ flags").
2. **The CRDR `'TNSR'` artifact lives in `crd-hesap-resources`** (`tensor_artifact.hpp`, HMTX-mirror
   TNHD 96-B pinned header + FNV-1a-64 hash), not in `crd-hesap-tensor` — ADR-0084's layering puts
   CRDR cook/load with resources, and it keeps this module free of a resources link edge (§1's lean
   consumers unharmed).
3. **D-v14m-1 (paper/reference divergence, documented in `nn.hpp`):** the v14-m frozen corpus
   quantizer rounds **half-to-EVEN on the f16-ROUNDED inverse scale**, where `dtypes.hpp`'s ggml
   `quantize_row_q8_0_ref` transcription (round-half-away on the direct scale) differs on 150/16,416
   q values over the corpus. The NN loader implements the frozen-ref semantics so the byte-exact
   weight gate is against pinned bytes; the dtypes-layer transcription remains byte-exact vs ggml
   itself. Both contracts are separately gate-enforced.
4. **A vendored third-party header, by exemption:** `detail/dlpack.h` — the DLPack v1.1 ABI header
   (Apache-2.0), vendored VERBATIM as the interop source of truth (the ABI struct layouts must be
   theirs, not a transcription). House-style rules deliberately not applied to it; our RAII import
   owner + versioned/legacy ABI handling live in our own `dlpack.hpp` on top.
5. **The sparse-CP wiring is a glue header over a functor seam, not a decomp edit:** `cp_als_generic`
   (v14-j) takes the tensor ONLY through an MTTKRP functor; `sparse_cp.hpp` (v14-i/j glue) supplies
   the CSF-backed functor. Seam parity (CSF functor ≡ `DenseMttkrp` fits, identical iteration counts)
   + run-twice bit-identity are ctest-gated. This keeps `decomp.hpp` dense-only (its header-only
   hesap-dense consumption) while sparse CP ships complete.

**Still open at the v14-z docs close (loud, per the honest-scoreboard rule):** the CLI
`hesap.tensor.*` registration TU is SHIPPED (12 commands, one per op family;
`register_tensor_cli_anchor()` static-lib anchor with include-only crd-hesap/hesap-dense/platform
edges, so the link-isolation smoke stays intact) but its gate tests (`test_cli.cpp`, the v13-z
pattern) are not yet written; the >16-operand einsum→hyper-optimizer bridge (`einsum.hpp`
`kEinsumMaxOperands = 16`) and the whole-engine per-slice-check sweep over the final artifact are
the other remaining v14-z items; the TBLIS/TCL contraction oracle named in the gold-standard board
was never built (N/A-with-the-check carried forward); the ort-int8 per-tensor cell's answer — a
per-tensor int8 tier — is IN PROGRESS as a new dtypes/storage decision (it would amend §2's
storage-dtype set when accepted).
