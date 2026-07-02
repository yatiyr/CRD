# Phase 3.1.6 — v14: the TENSORS cluster (`crd-hesap-tensor` — the engine's N-D substrate)

> **Status: OPEN — v14-a in progress (kickoff 2026-07-02).** This is the spec; the master phase doc
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
| **v14-a** | **Substrate + dtypes.** `Tensor<T>`/`TensorView<T>` over `IAllocator`, shape/strides, zero-copy slice/permute/broadcast/reshape views, contiguity tracking; dtypes f32/f64/c32/c64 + i64 index/u8 mask + **f16/bf16 + FP8(e4m3/e5m2) + int8/int4-block QUANTIZED storage** (F16C converts; compute stays f32/f64) + **★deterministic stochastic-rounding converts** (Philox counter-driven, reproducible-by-seed). Gate: NumPy view/stride semantics corpus + convert round-trips + SR unbiasedness + SR run-twice bit-identity. | ~2100 | ~80 |
| **v14-b** | **Elementwise + broadcasting engine.** Full NumPy broadcast rules; SIMD-fused unary/binary/compare/where/cast over crd::math. Gate: NumPy bit-exact (f64) on a broadcast corpus. | ~1200 | ~50 |
| **v14-c** | **Reductions + scans + ★the reproducible tier.** sum/mean/min/max/prod/argminmax/logsumexp/cumsum with **deterministic fixed-order trees (serial ≡ parallel)** + opt-in **ReproBLAS-class binned summation** (bit-reproducible INDEPENDENT of partitioning) + **★deterministic SR accumulation** (Philox SR into bf16/f16 accumulators — unbiased low-precision reduction, reproducible-by-seed). Gate: NumPy values + `{1..16}` moat + the Tier-R invariant under forced REPARTITION + bench vs ReproBLAS. | ~2000 | ~90 |
| **v14-d** | **★HPTT-class transpose/permute.** Blocked SIMD multithreaded permute (deterministic partition), fused scale. Bench: HPTT itself. | ~900 | ~30 |
| **v14-e** | **einsum front-end.** Parser (ellipsis, repeated-index diagonals, output spec) + ★opt_einsum-class path optimizer (greedy + optimal-DP) + **`EinsumPlan` build-once/execute-many**. Gate: FLOP-count parity-or-better vs opt_einsum, python-verified BEFORE the port. | ~1500 | ~60 |
| **v14-f** | **Contraction execution.** TTGT over the OWN GEMM + batched-GEMM dispatch + direct kernels for small/odd shapes + the **tensor `LinearOp` bridge** (into hesap-iterative/eig). Bench: NumPy/PyTorch/TCL/**TBLIS** matched-thread; GETT-fused ONLY if the profile names the transpose (SANITY #5). | ~1600 | ~55 |
| **v14-g** | **★cotengra-class hyper-optimizer.** Hypergraph-partitioned contraction trees + simulated annealing + **dynamic slicing** (bounded-memory contraction = the WCET pillar applied to einsum). Gate: tree quality vs cotengra on its published benchmark networks; the slicing memory bound honored exactly. | ~1500 | ~40 |
| **v14-h** | **Batched linear algebra.** Batched GEMM/Cholesky/LU/solve/small-SVD (the 6×6-by-100k EKF/skinning regime), deterministic batch partition. Bench: PyTorch CPU + MATLAB `pagemtimes`. | ~1200 | ~40 |
| **v14-i** | **★SPARSE tensors.** COO + **★CSF** + **★MTTKRP** (the CP workhorse) + sparse×dense contraction + sparse elementwise/reduce. Peers: **SPLATT** (MTTKRP) + **TACO** (contraction) + scipy/torch.sparse. Reuse: hesap-sparse TripletBuilder/format patterns. Real-data gates (FROSTT-class tensors cooked via CRDR). | ~2500 | ~80 |
| **v14-j** | **Decompositions I.** ★CP-ALS (dense + sparse MTTKRP) · Tucker/HOSVD/★HOOI · **★randomized variants** (rSVD/`interp_decomp`-based over the v12 counter-RNG ⇒ **deterministic-randomized**: same seed bit-identical `{1..16}`). Gates: TensorLy + MATLAB Tensor Toolbox (fit + factors up to sign/perm) + randomized-vs-exact error bounds. | ~2000 | ~70 |
| **v14-k** | **Decompositions II — TT.** ★TT-SVD + TT-rounding + TT algebra (add/hadamard/contract/**allocation-free eval**) + **★★TT-CROSS (maxvol)** — build a TT from FUNCTION EVALUATIONS, no materialized tensor ⇒ compress v13 interp LUTs / ephemerides 100–1000× and evaluate in the hot path. Gates: ttpy/tntorch + analytic compression-error + the v13-interp LUT demo. | ~2400 | ~85 |
| **v14-l** | **I/O + interop.** v12 counter-RNG tensor fills (reproducible-by-construction) + `.npy`/`.npz` r/w + **★safetensors r/w** (real ML weights incl. bf16/f16) + **★DLPack** producer/consumer (zero-copy interchange — bit-exact buffer sharing with the NumPy/PyTorch bench harnesses) + CRDR `'TNSR'` cook (ADR-0084 pattern). Gate: NumPy/torch round-trips bit-exact. | ~1700 | ~70 |
| **v14-m** | **NN inference pack — ★★the certified tiny-ML demo.** conv2d (im2col over the own GEMM) + pooling + activations (crd::math) + layernorm/softmax + **★the QUANTIZED path** (int8/int4-block weights, ggml/llama.cpp-class — integer inference bit-exact across ALL hardware) + the end-to-end demo: **safetensors → MLP/CNN deterministic allocation-free inference, bit-identical `{1..16}`** (the DO-178C/EASA certified-inference niche). Gate: torch-CPU value parity (≤1e-6 f32) + quantized-vs-ggml parity + the moat + inference bench vs torch/onnxruntime-CPU/llama.cpp-CPU. | ~2400 | ~75 |
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
