# v17 CKIR / GPU-compute gotchas

Banked landmines from building `crd-kir` + `crd-hesap-gpu` (ADR-0098). Read before re-hitting a matching symptom;
append the moment a new one bites (per the standing rule: log gotchas as you optimize).

## Tooling / build

- **`clang-tidy readability-non-const-parameter` misfires on non-template header-only functions** — it flagged
  `crd::i64* coord` (written via `coord[i]=…`), `f64* out` (written via `out[e]=…`), and `IAllocator* scratch`
  (used for the non-const `allocate`/`deallocate` calls) as "can be pointer to const" in `ckir_eval.hpp` — yet
  `kan.hpp` passed clean with the identical `f64* y` write pattern. The check is **inconsistent** on inline
  header-only functions compiled as their own TU (the `tidy-files.ps1` mode). Fix: these params are *provably*
  non-const, so suppress with a reasoned `NOLINT(readability-non-const-parameter)` (NOLINTNEXTLINE for a one-line
  signature; NOLINTBEGIN/END around a multi-line one) — don't waste time trying to satisfy it, and never actually
  make a written/non-const-method pointer `const`. (2026-07-07, v17-a.)

## Determinism / bit-exactness

- **⛔ GPU f32 DIVISION is NOT IEEE-correctly-rounded by default (~2 ULP).** The first CKIR GPU kernel (v17-b) was
  bit-exact vs the CPU reference for **Add/Sub/Mul** (IEEE-correctly-rounded + `precise` ⇒ no FMA fusion) but ~2 ULP
  off on any element with a **division** — NVIDIA (and most GPUs) implement f32 `OpFDiv` as a fast reciprocal +
  refinement, NOT correctly-rounded, and `precise` does not force it. So the honest v17-b split is: **bit-exact for
  correctly-rounded ops (add/sub/mul, and sqrt which IEEE requires), ULP-tolerant for division + transcendentals.**
  Bit-exact division is **v17-f** work (the `VK_KHR_shader_float_controls` audit / correctly-rounded-divide path, or
  emit a Newton-refined correctly-rounded divide). Do NOT gate division as bit-exact before then. (2026-07-07, v17-b —
  first GPU kernel: 4076/4104 bit-exact, the 28 were all the division term.)
- **`precise float` temps ⇒ SPIR-V NoContraction ⇒ bit-matches the `-ffp-contract=off` CPU reference** for the
  correctly-rounded ops. The determinism lever works; emit every kernel temp `precise`.
- Host-visible storage buffers (`MemoryUsage::CpuToGpu`/`GpuToCpu` + `BufferUsage::Storage`) + `graphics_queue()`
  (compute-capable, shares the command pool ⇒ no queue-family mismatch) + `submit_and_wait` + `map()` = the simplest
  correct dispatch/readback; add a `buffer_barrier(ComputeShaderWrite → TransferSrc)` before readback (ValidationCapture
  stays 0). (2026-07-07, v17-b.)

## CUDA backend (v17-c, driver API + NVRTC)

- **⛔ `cuModuleLoadData` fails with error 222 (CUDA_ERROR_UNSUPPORTED_PTX_VERSION) when the toolkit is NEWER than the
  driver.** NVRTC 13.3 emits PTX whose ISA version the older installed driver can't JIT. **Fix: compile straight to a
  CUBIN for the GPU's EXACT arch** — query `CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MAJOR/MINOR`, pass
  `--gpu-architecture=sm_89` (real, not `compute_89`), and use `nvrtcGetCUBIN`/`nvrtcGetCUBINSize` (NOT
  `nvrtcGetPTX`). A CUBIN loads directly, no PTX-JIT, no version mismatch. (2026-07-07, v17-c.)
- **⛔ CUDA 13 `cuCtxCreate` is v4 (4 args incl. a params struct) — the 3-arg call won't compile.** Use the primary
  context: `cuDevicePrimaryCtxRetain(&ctx, dev)` + `cuCtxSetCurrent(ctx)` (retain/release, recommended anyway).
- **NVRTC DLLs live in CUDA `bin/x64/`, NOT `bin/`** (`nvrtc64_130_0.dll` + `nvrtc-builtins64_133.dll`) — put
  `.../CUDA/vX.Y/bin/x64` on PATH or the test exits 53 (DLL-not-found, no output). `nvcuda.dll` (driver) is in System32.
- **GPU tests: `catch_discover_tests(... DISCOVERY_MODE PRE_TEST)`** — the default POST_BUILD discovery runs the exe at
  *build* time, which fails when the CUDA/Vulkan DLLs aren't on the build PATH. PRE_TEST defers enumeration to ctest.
- **CMake `find_package(CUDAToolkit)` needs `CUDA_PATH` (set by the installer, may need a shell restart) or a hint** —
  glob `C:/Program Files/NVIDIA GPU Computing Toolkit/CUDA/v*` and set `CUDAToolkit_ROOT` to the latest. Guard the whole
  module on `CUDAToolkit_FOUND` so machines/CI without CUDA still build.
- **★ CUDA `--fmad=false --prec-div=true --prec-sqrt=true` ⇒ bit-exact incl. DIVISION** (correctly-rounded), where
  Vulkan's fast reciprocal is ~2 ULP. So CUDA's bit-exact core is *wider* than Vulkan's (add/sub/mul/div/sqrt all
  bit-match the CPU reference; only transcendentals are ULP until v17-h). (2026-07-07, v17-c.)

## Perf (v17-e GEMM crush campaign, 2026-07-07 — RTX 4070 Ti SUPER, 66 SMs, ~44.9 TF f32 peak)

### Benchmark fairness + measurement
- **⛔ The vendor bar is THREE entry points, not one:** `cublasSgemm` PEDANTIC, `cublasSgemm` DEFAULT, and
  `cublasLtMatmul` PEDANTIC pick **different kernels** and any of them can be fastest at a given size (Lt beat sgemm
  by ~8% at one 2048 run; default beat pedantic at another). All three are true-f32 numerics for SGEMM — **DEFAULT
  math does NOT mean TF32** (TF32 needs explicit opt-in via `CUBLAS_TF32_TENSOR_OP_MATH`/GemmEx compute type). The
  honest vendor number = min(all three) per size.
- **⛔ Boost-clock/thermal variance is ±10–15% run-to-run** (no clock lock without admin `nvidia-smi -lgc`). A
  same-config kernel measured twice minutes apart can differ 20% ("+SiLU faster than raw" artifacts). **Never claim a
  crush off one run — take the MINIMUM ratio across ≥3 full runs** (most conservative for us).
- **cuBLAS PEDANTIC vs DEFAULT is a kernel-selection difference too**, not just an arithmetic contract — don't
  attribute a pedantic-vs-default delta to "IEEE cost".
- **CUDA 13 removed `cudaDeviceProp::clockRate`** — query `cudaDeviceGetAttribute(&khz, cudaDevAttrClockRate, 0)`.

### Schedule levers (measured effects, GEMM f32)
- **⛔ Small-N "slowness" was GRID UNDERUTILIZATION, not kernel quality:** 128×128 tiles at N=512 = 16 blocks on 66
  SMs (0.44× cuBLAS). A 64×64-tile config → 64 blocks → 0.90×+. **The optimal schedule is size-dependent — a per-size
  config search (the autotuner) is mandatory, not a nicety.** Winning config differed at EVERY size tested.
- **⭐ Register-prefetch double-buffering HURT at N=2048** (0.86→0.71×: prefetch regs on top of an 8×8 accumulator
  cut occupancy) **but two-stage SHARED-MEMORY double buffering WON** (prefetch global→reg→other-smem-stage, compute
  current stage, ONE `__syncthreads` per K-tile instead of two): +10–30% at N=1024. Buffer in smem stages, not in
  extra live registers.
- **Padded transposed shared-A: pad rows by +4** (`BMP=BM+4`). The transposed store pattern
  `As[(icA*4+j)*BM + irA]` with BM=128 is a 4-way bank conflict (stride 512B ≡ bank 0); +4 makes consecutive column
  stores land 16 banks apart (2-way residual). **+8 is WORSE than +4** (136×4B ≡ 0 mod 128B — back to same-bank).
  Padding must keep the row stride ≡ 0 mod 4 floats or float4 reads of the tile mis-align.
- **Threadblock swizzle (CUTLASS column-group rasterization) is a per-size decision:** helped 1024/2048-SiLU rows,
  *hurt* D2@512 and some ReLU rows. Autotuner input, never a global default.
- **Fused epilogue = the structural crush lever:** bias+activation applied in registers before the C store. The
  vendor pays a full extra C read+write (≈11% at N=2048, ≈23% at 1024) whenever the op is **off cublasLt's epilogue
  menu — SiLU (every LLM MLP!) is off-menu**; RELU_BIAS/GELU are on-menu, so there the fight is raw-kernel parity.
- **The EXACT tier (no-FMA, `__fmul_rn`/`__fadd_rn`, bit-matches the `-ffp-contract=off` CPU oracle) costs ~2×**
  (12.2–12.5 vs ~26 TF at N=2048) — FMA disabled halves the FLOP rate. Fast tier keeps FMA and stays fixed-order
  run-to-run deterministic (T1); EXACT is the T3/certification tier. Price is now measured, not guessed.
- **Still open at N=2048 raw (0.89–0.91×):** cuBLAS's large-N kernels pipeline with `cp.async` multi-stage +
  bigger tiles — that's the v17-g CUTLASS-class kernel. N=512 raw parity band (0.90–1.02) wants split-K.

### Round 5–6 (2026-07-07, back on Opus): cp.async, bigger tiles, and a measurement-honesty correction
- **⛔⛔ N=2048 is TOO CLOCK-VARIABLE to claim a crush without `nvidia-smi -lgc` (needs admin).** Across 6 runs the
  N=2048 verdicts swung: RAW 0.90–1.03×, FUSED-SiLU 0.83–1.02×, FUSED-ReLU 0.86–1.07× — cuBLAS-default alone swings
  25.6→30.5 TFLOP/s run-to-run, and the GPU throttles as a long bench session heats it. **A "1.02× in all 3 runs"
  claim from an early cool-GPU session did NOT reproduce warm.** Rule: **a crush cell must beat under the WORST
  (min-of-N) conditions, and 2048 can't be measured that way here.** The reproducible crush is **N=1024** (RAW 1.06×,
  SiLU 1.13×, ReLU 1.20× — beaten in ALL 6 runs) **+ N=512 fused-ReLU (1.06×)**. N=2048 = OPEN, honestly, until
  clock-locked or the v17-g kernel lands. Small-N fused timings (~0.02 ms) are also too noisy to trust (SiLU@512
  read 0.64× once purely from a bad clock sample).
- **⛔ Naive `cp.async` multi-stage REGRESSED (0.72–0.81× at 1024/2048) — worse than the round-4 transposed
  double-buffer.** Root cause: `cp.async` copies *contiguous* global→shared, so A lands **row-major**; the `regM`
  column read is then *strided* (`As[row*AW+dot]`) and **can't be vectorized to `LDS.128`**. Round 4's *transposed*
  shared-A (`As[dot*BM+row]`) makes that read unit-stride → `LDS.128`. cp.async can't produce the transposed layout
  without CUTLASS-style **swizzling** (the real work, v17-g). So cp.async is NOT a free win — it needs the swizzle to
  pay off. Bigger *transposed* tiles (256×128) beat naive cp.async and got the best-case 2048 numbers.
- **⭐ Ada (sm_89) >48KB shared-per-block needs DYNAMIC shared + explicit opt-in:** `extern __shared__` +
  `cudaFuncSetAttribute(k, cudaFuncAttributeMaxDynamicSharedMemorySize, bytes)` before launch, and pass `bytes` as
  the 3rd `<<<grid,blk,bytes>>>` arg. Static `__shared__` hard-caps at 48KB (ptxas "uses too much shared data
  0xc000 max"). Dynamic shared base is 16B-aligned (safe for float4/cp.async).
- **⛔ Oversized tiles COLLAPSE occupancy — measured, not theoretical:** 256×256 (512 threads, huge register +
  shared) → **1 block/SM or register spill → 0.1–0.5 TFLOP/s** (100× slower). The config search caught it (correctness
  passed, timing tanked). Sweet spot on Ada for f32: 128×128 / 256×128 tiles, ≤256 threads, 8×8 register tile.
- **⭐ 256×128 (tall) tile is the best raw@2048 config in the good runs** (best-case 1.03×) — more rows per block →
  better A-reuse at large N. But it doesn't hold under warm-clock variance (see the honesty gotcha above).

### v17-g tensor cores (TF32 wmma vs cuBLAS-TF32, 2026-07-07)
- **⛔ The `wmma` C++ API tops out at ~0.6–0.7× of cuBLAS-TF32** (measured: 0.56–0.69× at N=2048, pipelined). The
  fragment abstraction — especially `load_matrix_sync` from shared + the fragment register shuffle — has real overhead
  the vendor kernels avoid. **cuBLAS-TF32 uses raw `mma.sync` + `ldmatrix` PTX** (finer m16n8k8 tiles, conflict-free
  fragment loads, register-level pipelining). To CRUSH cuBLAS-TF32 you must drop below `wmma` to those PTX intrinsics —
  that's the CUTLASS-class kernel, the real depth of the v17-g crown. (TF32 correctness was fine: maxrel vs f32 ~2e-5.)
- **⭐ cp.async + tensor cores is a CLEAN combo (unlike cp.async + CUDA-core).** `wmma::load_matrix_sync` reads shared
  by leading-dimension, so the contiguous row-major cp.async layout needs NO transpose — the exact problem that sank
  the CUDA-core cp.async (strided non-vectorizable regM read) doesn't exist here. cp.async pipelining gave +30–40% on
  the wmma kernel (N=1024). So: cp.async is the right lever for tensor-core GEMM, the wrong one for the CUDA-core
  register-tiled GEMM.
- **Redundant `wmma::__float_to_tf32` after `load_matrix_sync` into a `precision::tf32` fragment** — the load already
  truncates to TF32; the manual per-element conversion loop is pure overhead. Drop it.
- **wmma pipelined tiles at ≥3 stages exceed 48KB shared → dynamic shared + Ada opt-in** (`extern __shared__` +
  `cudaFuncSetAttribute(k, cudaFuncAttributeMaxDynamicSharedMemorySize, bytes)`, same as the CUDA-core case).

### v17-g PARITY GRIND with the profiler UNLOCKED (2026-07-07, Fable — counters-first for real)

**Setup that made this possible:** the user enabled GPU perf counters (admin: NVIDIA dev settings / `RmProfilingAdminOnly=0`)
and locked the core clock (`nvidia-smi -lgc 2610`, admin terminal; `-rgc` to undo). **Memory clocks stay unlocked**
(`-lmc` separately if needed) — residual cuBLAS variance ±10% comes from mem-clock ramping.

- **⛔⛔ ncu IS the method.** Every hand-theory I had was wrong until profiled: occupancy 16.7% (smem-limited, 8
  warps/SM) + `math_pipe_throttle` dominant + **8.4M shared bank conflicts that my "+4 padding" did NOT fix**. The
  padding rule is PER ACCESS PATTERN: A-frag reads stride rows by `gid` ⇒ pad so `(stride*gid)%32` spreads (BK+4=20
  works: gid*20 hits all banks); **B-frag reads stride rows by `tig` ⇒ need `stride%32==8`** (BN+8=264: tig*8+gid
  covers all 32) — **BN+4 left a 2-way collision**. After the +8 fix: conflicts = 0 (verified).
- **⛔ `__launch_bounds__` register-caps SILENTLY SPILL:** 512-thread configs cap at 128 regs/thread and ptxas spills
  388–516 B/thread to local — the "more warps" configs were secretly 10× slower from spills. **Always compile
  `-Xptxas -v` and check "spill stores" per instantiation** before trusting a sweep result.
- **⭐⭐ ABLATION is the definitive profiler-adjunct:** same kernel with A-LDS/B-LDS/global-feed surgically disabled
  (numerically wrong, structurally identical). Decomposed the 12.4 TF gap exactly: loop structure+barriers 5.5 TF,
  cp.async feed 4.4 TF, fragment LDS 2.5 TF. Each got its own fix.
- **⭐⭐ PROFILE THE VENDOR:** ncu on a minimal cuBLAS-TF32 launcher read its schedule directly — **128×256 tile,
  PBK=16, 3 stages, 8 warps/256 threads, 73.73KB smem (UNPADDED ⇒ XOR-swizzled), 220 regs, 1.94 waves, 16.6%
  occupancy — the SAME budget as my best config**, but hmma 48.3% vs my 37.5%. Parity is inner-loop quality, not
  schedule shape.
- **⭐ The pure-mma ceiling probe: `mma.sync.m16n8k8.tf32` DOES hit the full 43 TF peak at locked 2610** (16
  independent accumulator chains, no loads) — the instruction is not the limit; the load/compute interleave is.
  (Probe bug that cost an hour: dividing by reps twice in the GFLOP/s printf → "4.3 TF" panic. Sanity-check
  ms/launch, not just the final rate.)
- **⭐ Single-barrier cp.async loop:** with STAGES≥3 the bottom `__syncthreads` is REDUNDANT (a warp drifts ≤1 kt past
  the top barrier; its cp.async writes target stage `(kt+S-1)%S` ≠ readers' `kt%S`). Also issue the next tile's
  cp.async feed RIGHT after the top barrier (lands during the mma burst), and **hoist the feed addressing** (per-thread
  base pointers advancing by constants — kills per-tile div/mod + 64-bit IMAD chains). Together +2 TF.
- **⭐ CROSS-KT fragment prefetch (software pipeline):** with STAGES≥4 and `wait_prior(S-3)` (NOT S-2 — the prefetch
  reads stage kt+1, one more completed group than the plain loop), the NEXT tile's ks=0 fragments load during the
  current tile's last mma burst → the per-tile serial `ldf` disappears. Safe: the kt+1 writer targets stage `kt%S`,
  prefetch reads `(kt+1)%S`.
- **⭐ MULTI-BLOCK RESIDENCY is the sleeper winner:** small tiles (64×128, ≤41KB smem, ≤96 regs) at 2 blocks/SM keep
  the tensor pipe busy THROUGH barriers (blocks don't share them) — `64x128x8 w32x64 s3 2blk` = 34.5 TF, the best
  N=2048 config, beating every big-tile 1-block config. The phase-lock of a single resident block is a real tax.
- **State of the grind (locked 2610): 30.5 → 34.5 TF (0.86–0.91× cuBLAS-TF32) — parity NOT yet reached.** The
  remaining named lever: **XOR-swizzled shared layout** (kills padding ⇒ fits deeper stages, enables vectorized frag
  loads) — cuBLAS's unpadded 73.73KB proves it's the endgame technique. Beyond that: SASS-level scheduling (nvcc
  can't express it; cuBLAS is hand-scheduled SASS).
- **⛔ Deep-pipelining (more cp.async STAGES) did NOT help + the cross-kt prefetch path REGRESSES small-PBK configs to
  ~10 TF.** PROFILED THE WINNER (64×128×8 w32×64 s3, 2 blk/SM): hmma **40.7%**, occupancy 28.9%, bank conflicts ~0 —
  dominant stalls are **`math_pipe_throttle` (10.2/issue) + `barrier` (4.0/issue)**. The barrier stall is STRUCTURAL:
  every tile needs `__syncthreads` for cross-thread cp.async visibility, and the mma warps stall on it. More stages
  can't hide a per-tile barrier. **XOR-swizzle won't fix this either** (it addresses conflicts/occupancy, not the
  barrier). ⇒ **The synchronized-tile structure has a CUDA-C ceiling here (~0.86–0.91×). The decisive remaining lever
  is WARP SPECIALIZATION** — dedicate producer warps to cp.async + consumer warps to mma, synced via named barriers
  (`bar.arrive`/`bar.sync`) / mbarrier, so the CONSUMER (mma) warps NEVER stall on the load barrier. That's cuBLAS's
  structural edge (hmma 48% vs my 40%). It's a distinct ~100-line rewrite (correct output-tile coverage by the
  consumer warp count + producer/consumer ring sync) — the right focused next slice, best started with fresh context.
  SASS hand-scheduling is the tier below that. Public CUTLASS-class kernels land 90–97% of cuBLAS; this hand kernel is
  at the low end of that band, profiler-confirmed.
- **⛔⛔ WARP SPECIALIZATION does NOT help TF32 GEMM on Ada (sm_89) — DEFINITIVE NEGATIVE (`crd_v17g_warpspec.cu`,
  correct, maxrel 5e-4).** Built the full producer/consumer kernel (producer warps run cp.async → named-barrier ring
  `bar.arrive`/`bar.sync` → consumer warps do ONLY mma, never a load barrier). Both splits REGRESSED: 4-prod/4-cons
  = 0.63–0.81× (dedicating half the warps to loading halves mma throughput), 2-prod/8-cons = 0.55–0.82× and DROPS at
  N=4096 (2 producer warps can't feed 8 consumers — starvation). **Root cause: warp specialization is a HOPPER
  technique — it pays off only when producers are near-free via TMA (`cp.async.bulk`/tensor-map hardware). Ada has NO
  TMA, so cp.async still burns thread cycles, and any warp spent producing is mma throughput lost.** On Ada the
  SYNCHRONIZED multi-stage kernel (all warps load AND mma) is optimal; the barrier stall is real but cheaper than
  losing warps. **⇒ The CUDA-C ceiling for TF32 GEMM on Ada is ~0.86–0.90× cuBLAS; every structural lever is
  exhausted, proven by measurement.** The SOLE remaining path is **SASS-level instruction scheduling** (cuBLAS is
  hand-scheduled SASS; nvcc/CUDA-C cannot express the interleave that keeps hmma at 48% vs our 40%). That's a distinct
  tool — inline SASS / CuAssembler / a maxas-style assembler — effectively its own research project, NOT a CUDA-C
  edit. The honest verdict: **absolute parity requires SASS; from CUDA C this kernel is at the public frontier.**
- **⛔ Source-level mma/LDS INTERLEAVE does NOT help — it HURTS (measured A/B, same clock state: 0.86× non-interleaved
  → 0.79× interleaved @2048).** NVIDIA's dev-forum advice ("interleave independent instructions between MMAs so they
  dual-issue mma-pipe + LSU-pipe") is a **SASS-scheduling** operation — ptxas does NOT reproduce it from CUDA-C source
  interleaving; forcing it in source just disrupts ptxas's register allocation. This is the FINAL confirmation that the
  10% gap is pure SASS instruction scheduling (which is exactly what cuBLAS hand-writes).
- **⛔⛔ MEASUREMENT UNRELIABILITY: consumer Ada POWER-THROTTLES tensor GEMM even with `nvidia-smi -lgc` locked.** The
  SAME kernel measured 21→34 TF across runs at a locked 2610 MHz SM clock (memory at full 10501). `-lgc` locks the
  clock *request*; the GPU still throttles below it under sustained tensor-core power draw. So even the "clock-locked"
  parity ratios carry ±20% noise. On this consumer card a stable cuBLAS-TF32 parity number is not measurable without a
  power/thermal-controlled datacenter part. **Conclusion (exhaustive): full TF32-tensor parity on consumer Ada is
  blocked by THREE independent walls — the tensor nerf (~1:1 vs 8:1 datacenter), SASS-only scheduling (ptxas can't emit
  it), and power throttling. The meaningful crushes are already ours: CUDA-core FP32 (1.02–1.06× vs cuBLAS-FP32) here,
  and Ootomo 3×TF32 (FP32-accurate, surpasses FP32 1.7×) on datacenter.**

### v17-g raw mma.sync PTX (2026-07-07, "all the way down")
- **⛔ `mma.sync.m16n8k8.tf32` operands are `.b32` (u32 regs, `"r"` constraint), NOT `.f32`.** `cvt.rna.tf32.f32`
  outputs a `.b32` tf32 bit-pattern (use `unsigned` + `"=r"`); the C/D accumulator stays `.f32` (`"+f"`). Passing the
  tf32 operands as `"f"` ⇒ ptxas "Arguments mismatch for instruction 'cvt'/'mma'". The m16n8k8 lane layout: groupID =
  lane>>2, tig = lane&3; A(row) a0(gid,tig) a1(gid+8,tig) a2(gid,tig+4) a3(gid+8,tig+4); B(col) b0(tig,gid)
  b1(tig+4,gid); C(row) c0(gid,tig*2) c1(gid,tig*2+1) c2(gid+8,tig*2) c3(gid+8,tig*2+1).
- **⭐ raw `mma.sync` beats the `wmma` C++ API by ~10%** (0.67–0.74× vs 0.56–0.69× cuBLAS-TF32, both pipelined) — drop
  wmma for the PTX intrinsic in the hot GEMM.
- **⛔⛔ cuBLAS-TF32 runs at ~90% of the TF32 tensor PEAK** on Ada consumer (measured ~40 TF vs ~44 TF peak on the 4070
  Ti SUPER). **You cannot CRUSH a 90%-of-peak flagship kernel — parity is the ceiling**, and it needs the full CUTLASS
  stack (bigger warp tiles, register-level fragment double-buffering, ldmatrix-class loads). **The crush at the
  tensor-core tier is FUSION** (fused GEMM+bias+activation beats cuBLAS-TF32 + a separate elementwise pass — the vendor
  pays the C round-trip), exactly as at f32. Don't chase a raw-throughput blowout that the hardware forbids.
