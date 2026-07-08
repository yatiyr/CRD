# 2026-07-07 — v17-g: TF32 tensor-core GEMM (wmma) vs cuBLAS-TF32 — WORKS + CORRECT, crush OPEN (needs mma.sync)

The v17-g crown, first cut: a **TF32 tensor-core GEMM** via `nvcuda::wmma` (m16n16k8), naive + a **cp.async multi-stage
pipelined** variant, benchmarked against cuBLAS at MATCHED TF32 precision (`crd_v17g_gemm_tensorcore.cu`). This is the
high-FLOP reduced-precision tier — distinct from the bit-exact-f32 tier (the v17-b CUDA-core kernel). 3-run data.

## Result (GFLOP/s; min/typical across 3 runs)

| N | wmma naive | wmma PIPELINED (cp.async) | cuBLAS-TF32 | PIPE / cuBLAS-TF32 | maxrel vs f32 |
|--:|--:|--:|--:|--:|--:|
| 512  | ~3.8k | ~5.1–5.8k | 21–23k | **0.24–0.28×** | 2e-5 |
| 1024 | ~16k | ~19–23.6k | 34–40k | **0.55–0.64×** | 2e-5 |
| 2048 | ~23k | ~23.7–25.9k | 37–42k | **0.56–0.69×** | 4e-5 |

**Correct + deterministic:** maxrel vs the true-f32 reference is 2e-5–4e-5 (TF32 truncation is tiny here because the
inputs are ~0.05-magnitude and accumulation is f32). The kernel runs, the numbers are right — it's a *perf* gap.

## Honest read — crush OPEN, and WHY

- **cp.async pipelining is the right lever and it helped** (+30–40% at N=1024) — and unlike the CUDA-core case, cp.async
  is a CLEAN fit for tensor cores: `wmma::load_matrix_sync` reads from shared via a leading dimension, so the row-major
  cp.async layout needs no transpose (the problem that sank the CUDA-core cp.async).
- **But `wmma` tops out at ~0.6–0.7× cuBLAS-TF32.** cuBLAS-TF32 (37–42 TF at N=2048; the 4070 Ti SUPER's TF32 tensor
  peak) is a flagship kernel that uses **raw `mma.sync` + `ldmatrix` PTX** — below the `wmma` C++ API, which has real
  per-fragment abstraction overhead (the `load_matrix_sync`/fragment-shuffle path). Matching cuBLAS-TF32 needs the
  lower-level kernel: `ldmatrix` for conflict-free fragment loads, `mma.sync.m16n8k8` (finer than wmma's m16n16k8),
  register-level (not just shared) software pipelining, and larger warp tiles. That's the CUTLASS-class kernel — the
  real depth of the v17-g crown (~85 tests for a reason).
- **Reported head-on (⛔ no false victory):** this is honest progress on the tensor-core tier, NOT a cuBLAS-TF32 crush.

## Where the crush actually stands (the whole picture)

| tier | vs vendor | status |
|---|---|---|
| CUDA-core **true-f32**, N=1024 | **1.06× BEAT** cuBLAS-f32 (6/6 runs) | ✅ + **wired into the CKIR compiler** (v17-b) |
| **Fused** GEMM+bias+SiLU/ReLU, N=1024 | **1.13× / 1.20× BEAT** | ✅ reproducible (SiLU off cublasLt's menu) |
| CUDA-core true-f32, N=2048 | 0.90× | OPEN (swizzled cp.async) |
| **TF32 tensor-core**, all N | 0.56–0.69× cuBLAS-TF32 | OPEN (needs mma.sync + ldmatrix) |

## Gotchas (→ hints §Perf)

- **`wmma` C++ API ≈ 0.6–0.7× of hand-tuned `mma.sync`/`ldmatrix`** — the fragment abstraction (esp. `load_matrix_sync`
  from shared) has overhead; the vendor kernels don't use it. For the crush, drop to the PTX mma intrinsics.
- **cp.async + tensor cores is a CLEAN combo** (no transpose problem) — wmma loads from shared by leading-dim, so the
  contiguous row-major cp.async layout just works. This is why cp.async helped here but hurt the CUDA-core kernel.
- **Redundant `__float_to_tf32` after `load_matrix_sync` into a tf32 fragment** — the load already truncates; the manual
  loop is pure overhead (removed in the pipelined variant).
- **wmma pipelined tiles ≥3 stages exceed 48KB → dynamic shared + Ada opt-in** (same `cudaFuncSetAttribute` pattern).

## Next

The `mma.sync` + `ldmatrix` kernel (register-pipelined, m16n8k8) is the path to the cuBLAS-TF32 crush — the deep v17-g
work. The reproducible wins (N=1024 f32 + fused) are already banked in the compiler; this tier is the remaining climb.

---

## UPDATE — raw `mma.sync.m16n8k8.tf32` PTX kernel (2026-07-07, "go all the way down")

Built the lowest-level kernel the user demanded: `crd_v17g_gemm_mma.cu` — raw `mma.sync.aligned.m16n8k8.row.col.f32.
tf32.tf32.f32` PTX with manual shared→register fragment loads following the exact m16n8k8 lane layout, `cvt.rna.tf32`
inputs, f32 accumulate; single-buffer + cp.async multi-stage pipelined variants (config search over k-tile/stages).

| N | mma single | mma PIPELINED (cp.async) | cuBLAS-TF32 | PIPE/cuBLAS-TF32 | correct? |
|--:|--:|--:|--:|--:|:--:|
| 512  | ~4.4k | ~6.6k | 21–24k | 0.28–0.30× | maxrel 2e-5 ✓ |
| 1024 | ~17k | ~24–26k | 34–40k | **0.62–0.74×** | 2e-5 ✓ |
| 2048 | ~26k | ~26–28k | 40–42k | **0.63–0.67×** | 4e-5 ✓ |

**mma.sync BEATS wmma** (0.67–0.74× vs 0.56–0.69×) — confirms the PTX path is the right one; the `wmma` fragment
abstraction was leaving ~10% on the table. Correct + deterministic.

**But the honest structural ceiling:** cuBLAS-TF32 measures **~40 TFLOP/s**, and the RTX 4070 Ti SUPER's TF32 tensor
peak is **~44 TFLOP/s** — **cuBLAS is at ~90% of peak.** You cannot *crush* a 90%-of-peak flagship kernel; **parity is
the ceiling**, and reaching it needs the full CUTLASS stack (bigger warp tiles for load amortization, register-level
double-buffering of fragments, `ldmatrix`-class loads, reduced per-element cvt). My pipelined mma.sync at 0.74% is
solid progress toward parity, reported head-on — NOT a crush of the TF32 tier, because there isn't one to be had.

**⇒ The crush at the tensor-core tier is FUSION, same as at f32.** Even at raw-GEMM parity, a FUSED tensor-core
GEMM+bias+activation beats cuBLAS-TF32 + a separate elementwise kernel (the vendor pays the C round-trip; SiLU is off
cublasLt's epilogue menu). That's the winnable, consistent crush strategy across both precision tiers — and it's
already proven at f32 (@1024: SiLU 1.13×, ReLU 1.20×).

## Gotchas (→ hints §Perf)

- **`mma.sync` TF32 operands are `.b32` (u32 registers, `"r"` constraint), NOT `.f32`.** `cvt.rna.tf32.f32` outputs a
  `.b32` bit-pattern; the accumulator is `.f32` (`"+f"`). Mixing them up ⇒ ptxas "Arguments mismatch for instruction".
- **cuBLAS-TF32 is ~90% of the TF32 tensor peak** on Ada consumer cards — the tier's honest target is PARITY, and the
  crush is FUSION (epilogue in the C write), not raw-GEMM throughput.
- **mma.sync > wmma by ~10%** — the C++ fragment API is worth dropping for the PTX intrinsic in the hot GEMM.

---

## UPDATE — the "do all 3 moves" pass (2026-07-07): fusion across the fleet + fused-TC + the parity dependency

**Move 1 — FUSION IS NOW A COMPILER PROPERTY ACROSS ALL 4 LOCAL BACKENDS (the big win).** `detect_fuse` moved to the
shared `ckir_tile.hpp`; per-language fused emitters (`emit_epi_clike` shared GLSL+HLSL, `emit_epi_wgsl`, +
`emit_contract_fused_{glsl,hlsl,wgsl}` = naive `precise` matmul + epilogue in the store, and the CUDA warp-tiled
fused). Every backend's `run()` tries fusion first. **GATED ON REAL GPUs:** CUDA (68407) + Vulkan (4929) + DX12 (2866)
+ WebGPU (2866) each fuse `SiLU(A@B+bias)` into ONE kernel, correct vs the oracle. Author `activation(GEMM+bias)` once
→ every backend emits the fused crush.

**Move 2 — fused tensor-core (mma.sync + bias+SiLU in the store) vs cuBLAS-TF32 + a separate pass.** Measured
(`crd_v17g_gemm_mma.cu`, `gemm_mma_pipe<...,FUSED=1>`): **0.32–0.88× — it LOSES.** The reason is the SAME structural
truth as the f32 win, seen from the other side: **fusion only tips a win when the raw GEMM is at parity.** At f32 my
warp-tiled GEMM was at parity (@1024 1.0×), so fusing the epilogue tipped it to 1.13–1.20×. At TF32 my mma GEMM is
~0.7× (not parity), and the saved epilogue (≈2·N² memory ops, small vs the N³ GEMM at large N) can't overcome a
30%-slower GEMM. **⇒ the fused-TC crush is GATED on Move 3 (a parity mma kernel).**

**Move 3 — the parity-track mma kernel.** Established: the raw mma.sync foundation (0.67–0.74× cuBLAS-TF32, correct,
cp.async-pipelined). Reaching cuBLAS-TF32 PARITY (the honest ceiling — cuBLAS is ~90% of the TF32 tensor peak) needs
the full CUTLASS stack: bigger warp tiles (load amortization), register-level fragment double-buffering, `ldmatrix`,
reduced per-element cvt. That's genuine multi-session kernel engineering; **parity — not a blowout — is the ceiling,
and only THEN does fusion tip the TF32 tier into a crush.** Documented head-on: no false victory claimed on the TF32
tier. The crush that IS real and banked lives at f32 + fused, across all backends (Move 1).

---

## PARITY SLICE (2026-07-07): research + counters-first + the bank-conflict fix -> 0.85x (N=2048), and the honest map

**Research (the user's "do every research"):** the roadmap to cuBLAS parity is Alex Armbruster's from-scratch
tensor-core GEMM (reaches 96% of cuBLAS): **vectorized loads (3x) -> shared-memory swizzling to kill fragment-read bank
conflicts (2x, the biggest lever) -> async prefetch overlap -> tile tuning (256x256x32) -> double-buffering (->96%)**.

**Counters-first (the VTune doctrine):** tried to profile with **Nsight Compute (ncu)** -- **blocked by
`ERR_NVGPUCTRPERM`** (GPU perf counters need admin / the driver "allow non-admin counters" setting; the GPU analog of
the WSL2-perf-unavailable gotcha). Fell back to a hand bank-conflict analysis: my fragment read `As[(rb+gid)*BK+ks+tig]`
with BK=16 collapses `gid*16` -> 8 rows onto 2 banks = **4-way conflict** (B likewise). Exactly the swizzle bottleneck.

**Fix applied (swizzle-lite via padding + drop the cvt):** pad the shared strides **BK->20, BN->132** (both `==0 mod 4`
so cp.async's float4 stays 16B-aligned, and `gid*20`/`tig*132` land on distinct banks -> conflict-free reads) and
**drop the per-element `cvt.rna.tf32`** (mma reads `.b32` and truncates the low 13 bits anyway => `__float_as_uint` is
free and correct -- maxrel held at 2e-5). **Result: N=2048 0.68x -> 0.85x (best; 0.71-0.85x across runs).**

**The honest, size-dependent finding:** the padding helped the **compute-bound** regime (N=2048, 256 blocks) but
**N=1024 stayed 0.69x** -- because at N=1024 there are only **64 blocks (8x8) on 66 SMs ~= one wave**, so it's
**occupancy/tail-bound, not bank-conflict-bound**. Different regimes, different limiters.

**The precise remaining map to parity (named, not hand-waved):**
1. **N=2048 (0.85->0.96x):** bigger block tile (128x256 / 256x128) + register-level fragment double-buffering
   (prefetch next-k fragments during the current mma) -- the blog's Kernel-5/6 steps.
2. **N=1024 (occupancy-bound):** smaller tiles OR **split-K** (more blocks -> fill the SMs) -- a per-size autotuner call.
3. XOR-swizzle (vs padding) frees the pad bytes for slightly larger tiles.

Parity remains the honest ceiling (cuBLAS-TF32 ~= 90% of peak); this slice advanced the parity track **0.68->0.85x with
research + a counters-informed fix**, and maps the last mile exactly. No false victory -- it's the parity track, en route.

Sources: Alex Armbruster, "How To Write A Fast Matrix Multiplication From Scratch With Tensor Cores" (2024);
NVIDIA CUTLASS `examples/14_ampere_tf32_tensorop_gemm`.

---

## PARITY GRIND ROUND 2 (2026-07-07): templated autotuner + every lever -> ~32 TF HARD CAP, bottleneck isolated

Built a **templated TF32 mma.sync kernel** (`crd_v17g_parity.cu`) + a **CUTLASS-lite config sweep** over
{block 128x128..256x256, warp, k-tile 16/32, stages 3-5} with **register double-buffering** of fragments, and ground it:

| lever tried | result |
|---|---|
| bank-conflict padding (BK->20, BN->132) | 0.68 -> 0.85x (N=2048) ✓ the one that moved it |
| no-cvt raw-bit tf32 operands | free, correct (maxrel 2e-5) ✓ |
| cp.async multi-stage (3-5 deep) | in place |
| register fragment double-buffer (prefetch next-k during mma) | ~0 (not the limiter) |
| bigger tiles 128x256 / 256x128 / 256x256 | best config, but still capped |
| **smaller high-occupancy tiles (64x64, 64x128)** | **did NOT help -> NOT occupancy-bound** |
| deeper stages (s4/s5) | ~0 |

**HARD CAP: my kernel is stuck at ~31-32 TFLOP/s across EVERY config** (best 0.79-0.89x cuBLAS-TF32 warm, 0.66-0.75x
cold). Systematically eliminated: bank conflicts (fixed), fragment-load pipelining (double-buffer did nothing),
occupancy (small tiles did nothing). **The one remaining suspect: the fragment loads are 32 scalar `LDS.32`/k-step, and
that's saturating the load-store pipe.**

**Why I can't cheaply fix it (the research-confirmed wall):** cuBLAS loads fragments vectorized, but for TF32
**`ldmatrix` can't help — it can't transpose 32-bit data**, so both cuBLAS and I use plain 32-bit shared loads. To
vectorize them (LDS.64/128) the 4 scattered fragment elements must be made contiguous via an **XOR fragment-swizzle**
that is *simultaneously* cp.async-store-friendly (contiguous) AND fragment-read-friendly — the deepest CUTLASS
technique. cp.async's contiguity constraint fights the fragment gather; reconciling them is the crux CUTLASS spends
enormous machinery on.

**A measurement confound that matters:** without `nvidia-smi -lgc` (admin), **cuBLAS-TF32 itself swings 34->44 TF
(±30%) on clock variance** while my kernel is stable at 32. So the *same* kernel measures 0.73x (cuBLAS cold-clocked
high) to 0.94x (cuBLAS warm-clocked low) run-to-run. A reliable >=1.0x can't even be *defined* without clock-locking.

**Honest conclusion:** exhaustive research-and-measurement-driven grinding took the hand kernel to ~0.85-0.9x
(matched-clock estimate), capped at ~32 TF (73% of peak), **LDS-bound on scalar fragment loads**. Absolute parity
(>=1.0x) requires (a) the **XOR fragment-swizzle for vectorized fragment loads** (CUTLASS-class, a dedicated deep
slice) AND (b) **clock-locked measurement** (admin) to define parity at all. Every reasonable hand-kernel lever has
been applied and measured; the last mile is precisely characterized, not hand-waved. **No false victory: it is not at
parity, and I've isolated exactly why.**

---

## PARITY GRIND, PROFILER-UNLOCKED (2026-07-07, Fable): 30.5 -> 34.5 TF (0.86-0.91x), method + remaining map

User enabled GPU counters + locked the clock (2610 MHz) -> the grind went counters-first for real. Sequence
(each step ncu-verified, all in `crd_v17g_parity.cu` + probes):

| step | evidence | fix | TF (N=2048 best) |
|---|---|---|---|
| baseline | — | — | 30.5 |
| ncu: 8.4M bank conflicts REMAIN | B-frag reads stride by `tig` ⇒ BN+4 pad wrong | **BN+8 (stride%32==8)** ⇒ 0 conflicts | ~31.5 |
| ptxas -v: 388-516B SPILLS on 512-thr configs | `__launch_bounds__` reg cap | drop/fix those configs | (unmasked sweep) |
| ABLATION: structure 5.5 / feed 4.4 / LDS 2.5 TF | the decisive decomposition | attack each | — |
| feed 4.4 TF | hoisted cp.async addressing + single-barrier loop (S>=3 proof) + early feed issue | +2 TF | ~32.7 |
| serial per-tile ldf | **CROSS-KT fragment prefetch** (S>=4, wait_prior(S-3)) | +1 TF | 33.4-33.7 |
| single-block phase-lock | **multi-block residency** (64x128, 2 blk/SM) | best config | **34.5** |
| pure-mma ceiling probe | 43 TF reachable (16 indep chains) — instruction not the limit | calibration | — |
| PROFILED CUBLAS ITSELF | 128x256/PBK16/s3/8w/73.73KB UNPADDED (XOR swizzle)/220regs — same budget, hmma 48.3% vs my 37.5% | the endgame is swizzle + SASS | — |

**Honest verdict: 0.86-0.91x cuBLAS-TF32 (clock-locked, N-dependent). ABSOLUTE PARITY NOT YET REACHED.** The
remaining ~10-15% has exactly two named levers left: (1) **XOR-swizzled shared layout** (removes padding => deeper
stages fit; enables vectorized fragment loads; cuBLAS's unpadded smem proves it) — implementable, the next slice;
(2) SASS-level instruction scheduling — cuBLAS is hand-scheduled SASS, structurally beyond nvcc/CUDA-C. CUTLASS-class
public kernels land 90-97% of cuBLAS; this hand kernel is now AT the low end of CUTLASS-class. All techniques feed
the CKIR tuning DB + emitter as reusable schedule knowledge.
