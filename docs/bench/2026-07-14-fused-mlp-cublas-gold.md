# Fused MLP vs cuBLAS — the NRC moat, forward + backward (2026-07-14)

**Board — RTX 4070 Ti SUPER, fp16 in / fp16 accumulate (both sides identical math).**
Benches: `bench/gpu-compute/mlp_fused_bench.cu` (forward), `bench/gpu-compute/mlp_backward_bench.cu` (backward).
nvcc `-O3 -arch=sm_89`, min-of-7×50-iter, 3 warmups, **correctness-gated** (fused vs cuBLAS/CPU-oracle must match).

MLP: input 64 → 5 hidden layers of 64 (ReLU) → 64 linear out = **6 weight matrices 64×64**, batch **1 048 576**.

| Pass | vendor (cuBLAS, best algo) | **fused (CKIR-class)** | ratio |
|---|---|---|---|
| Forward (inference) | 2.68 ms · 19.3 TF | **1.13 ms · 45.6 TF** | **2.37× CRUSH** |
| Backward (train)    | 9.60 ms · 10.7 TF | **5.06 ms · 20.4 TF** | **1.90× CRUSH** |
| **Full training step** | **12.28 ms** | **6.19 ms** | **1.98×** |

Correctness: forward `max_abs_diff = 0.00000` vs cublasLt (identical fp16 accumulate). Backward `dW[L-1]`
fused rel = `5e-5`, cuBLAS rel = `2e-4` — both vs an independent fp32 CPU oracle (`a[L-1]^T·gout`).

## Why it is a genuine structural crush (the NRC moat)

cuBLAS is a *per-call* library: every `cublasLtMatmul` reads its activation input from DRAM and writes its
output to DRAM. A 6-layer MLP pays **~6× the activation DRAM traffic** on the forward pass, and on the
backward pass an entire elementwise ReLU′-mask kernel per layer (**4.12 ms of pure DRAM traffic**, see below)
plus every dz/da intermediate round-tripping DRAM. The fused kernel reads the input tile **once**, ping-pongs
activations/gradients in shared through all 6 layers, and writes only the final result. cuBLAS *cannot*
express this — fusion across GEMM calls is off its menu. This is exactly the tiny-cuda-nn / Neural Radiance
Cache fully-fused-MLP technique (NVIDIA's own NRC is built on tiny-cuda-nn for precisely this reason). We
reproduce it and beat vendor on **both** the render-time (forward) and train-time (backward) halves.

## ⚠ The fairness correction — the 8× that WASN'T (mandate: bench the strongest peer, never strawman)

The first backward run showed **8.14×** — but that was cuBLAS running its *default* algorithm on the dW
weight-gradient GEMM, a pathological **huge-K (1 048 576) skinny 64×64 reduction**. Breakdown at default:
`mask 4.12 + dW 37.29 + dA 2.67 ms` — the dW GEMM alone was 37 ms at 1.4 TF because the default heuristic
does **not** pick split-K. Giving cuBLAS a 256 MB workspace + `cublasLtMatmulAlgoGetHeuristic` (which selects
split-K) dropped dW **37.29 → 2.64 ms (14×)**. Honest backward breakdown: `mask 4.12 + dW 2.64 + dA 2.69 ms
= 9.60 ms`. **The real crush is 1.90×, not 8×.** The 8× is recorded here only as the strawman it was.

## Forward lever ladder (serial measure→change→measure)

| Step | change | fused ms | ratio |
|---|---|---|---|
| 0 | 1 warp/16 rows, shared weights, ping-pong | 1.94 | 1.41× |
| 4 | **2 row-fragments / warp (ILP)** | 1.29 | 2.11× |
| 5 | **4 row-fragments / warp, 2 warps** | **1.13** | **2.37×** |

Decisive lever = **register ILP**: 16 independent wmma accumulator chains per warp (4 row-frags × 4 col-frags)
keep the tensor pipe saturated; a single 16×16 chain stalls on its own dependency. In-place activation store
(drop the ping-pong) was tried and **reverted** — wmma `store_matrix_sync`→`load_matrix_sync` of the same
shared region scatters/gathers across lanes and a bare `__syncwarp()` does not make it correct.

## Backward levers + the atomic wall

The fused backward is **2 GEMMs/layer** (dW = a^T·dz reduced over batch → fp32; da = dz·W^T → on-chip g) plus
a ReLU′ mask folded in-register. Correctness needed **per-warp** dW scratch (`sdw[WARPS][256]` — a block-shared
scratch raced the two warps → rel 0.54 bug). The dW output is a global reduction → **200 M atomic adds** to a
64×64 fp32 target; a `NOATOMIC` probe measured that scatter at **1.89 ms (35 %)** of the backward. Split-K
partial-accumulation (NGROUP copies + reduce) was tried to relieve contention: **NGROUP=256 (25 MB) was WORSE
(5.88 ms)** — it blew the atomic target out of the 48 MB L2 into DRAM latency; the cost is atomic *count*, not
contention, and the small single target is L2-resident. `NGROUP ∈ {1,8,32,64}` all land ~5.07 ms (L2-resident);
**NGROUP=8 marginally best (1.90×)**. Cutting the atomic count further needs a persistent-block design holding
all 6 layers' dW (98 KB fp32) in shared alongside activations — does not fit in Ada's 100 KB opt-in. 1.90× is
the honest floor for this design.

## Determinism (the crush we hold that cuBLAS does not)

Forward is **deterministic** — no atomics, fixed wmma schedule (gates `max_abs_diff = 0` run-to-run and vs
cuBLAS). Backward's **dW uses fp32 `atomicAdd`** → not yet run-to-run bit-exact (float atomic order varies);
cuBLAS split-K is *also* non-deterministic. A deterministic dW (fixed-order tree reduction over the NGROUP
partials, no atomics) is a CKIR-port item — the port must deliver the `{1..16}` determinism moat this codebase
holds everywhere else.

## ✅ CKIR PORT — increment 1: CUDA forward authored in CKIR (2026-07-14)

The forward fused MLP is now **authored in CKIR** — not hand-written. `engine/kir/include/crd/kir/ckir_mlp.hpp`
(a monolithic per-backend recipe emitter, the same pattern as the coopmat2 GEMM tensor tier) turns one `MlpConfig`
(width 64, 6 layers, tile 128, 2 warps) into the wmma forward kernel via `emit_fused_mlp_fwd_cuda`, plus a CPU
reference oracle `mlp_forward_ref`. Test `tests/kir/test_ckir_mlp.cpp` (oracle vs hand-computed 2-layer =
bit-identical; emit well-formedness) + the `[.emit-cuda-mlp]` tool writes `bench/gpu-compute/ckir_mlp_fwd_gen.cu`;
the driver `ckir_mlp_bench.cu` compiles it with nvcc and duels cuBLAS:

| | ms | TF | ratio |
|---|---|---|---|
| cublasLt-MLP | 2.83 | 18.2 | 1.00× |
| **CKIR-authored fused MLP** | **1.17** | **43.9** | **2.41× CRUSH** |

`max_abs_diff = 0.00000` vs cuBLAS; **determinism: 0 / 67 108 864 halves differ run-to-run (BIT-IDENTICAL)** — the
`{1..16}` determinism pillar holds for the forward (fixed schedule, no atomics). crd-kir suite 706/125 green on
MSVC + clang-cl; both new files tidy-clean.

## ✅ CKIR PORT — increment 2: Vulkan coopmat2 forward → PORTABLE (2026-07-14)

The forward is now authored in CKIR on **both primary compute backends** from the same `MlpConfig`.
`emit_fused_mlp_fwd_glsl` (ckir_mlp.hpp) emits a `VK_NV_cooperative_matrix2` (workgroup-scoped) kernel: one
workgroup owns a batch tile, activations ping in **shared** across all layers (never touch global between layers
— the fusion), the per-layer matmul runs on the tensor units, and the fp32 accumulator round-trips a shared
scratch for the ReLU / linear + fp16 repack. Test `tests/kir-vulkan/test_backend_vulkan.cpp` (`[mlp]`) emits,
compiles (shaderc), dispatches through the unified `VulkanComputeContext`, and gates the fp16 result vs the CPU
oracle:

| backend | tensor primitive | vs oracle | determinism |
|---|---|---|---|
| CUDA | wmma (16×16×16) | `max_abs_diff=0` vs cuBLAS | 0 / 67M bit-identical |
| **Vulkan** | **coopmat2 (workgroup-scope)** | **rel = 0.0021 (fp16 tol)** | **0 / 524288 bit-identical** |

Both from one config, both matching the same oracle, both deterministic. crd-kir 706/125 + crd-kir-vulkan GPU
33027/32 green on MSVC **and** clang-cl; all touched files tidy-clean.

**⚠ Oracle correction found en route:** the Inc-15 CPU oracle read weights **column-major** (`w[k + W·n]`) — a
transpose of what the kernels actually compute. A host convention-probe (both layouts vs the live CUDA kernel)
proved the true layout is **row-major `w[k·W + n]`** (column-major was 95× wrong). The oracle test had only
checked self-consistency, not that it matched the emitter. Oracle fixed → it is now the true shared reference
both backends match.

## ✅ CKIR PORT — increment 3: forward fanned out to ALL backends via the FP32-precise tier (2026-07-14)

The tensor cooperative-matrix primitives the fan-out originally named are **phantom on this toolchain** (measured, not
assumed): HLSL **WaveMatrix** is rejected by DXC 1.8.2502 (`WaveMatrixLeft` undeclared — never shipped in mainline DXC;
the DX12 backend also compiles at `cs_6_0`); **WGSL subgroup-matrix** is Dawn-experimental, absent from the `wgpu-native`
this repo uses; **MSL simdgroup_matrix** is mature on Apple but there is no Metal runtime on Windows. Emitting them would
be theater. Instead, the forward is delivered on every backend via the **FP32-precise fused tier** — the strongest moat:
FP32, no FMA, so it is **bit-exact across ALL backends**, which the fp16 tensor tier structurally cannot be.

`build_mlp_fwd_fp32` (ckir_mlp.hpp) builds the fused MLP on the **CKIR statement tier** (one workgroup = one sample, one
thread = one output feature, activations ping-pong in shared across all layers — the fusion; ascending `acc = acc + a·w`
fold, precise ⇒ no FMA). ONE builder → the *generic* per-backend emitters lower it to all five:

| backend | how verified here | result |
|---|---|---|
| CPU oracle (`eval_cpu_kernel`) | vs `mlp_forward_ref` | **bit-exact** (512 `==`) |
| **Vulkan** | emit GLSL → shaderc → dispatch → compare | **bit-exact vs oracle** |
| **DX12** | emit HLSL → DXC → dispatch → compare | **bit-exact, identical to Vulkan** |
| **CUDA** | emit → nvcc `-arch=sm_89 -c` | compiles clean |
| **WGSL** | emit → landmark-validate | well-formed (no imperative WebGPU ctx here) |
| **MSL** | emit → landmark-validate | well-formed (no Metal on Windows) |

Vulkan == DX12 == oracle **bit-for-bit** — the `{1..16}` cross-backend determinism moat, now on the fused MLP. crd-kir
1234/127 + gpu-context-vulkan/dx12 `[mlp]` green; all touched files tidy-clean. Tensor tier (the *crush*) stays CUDA
wmma + Vulkan coopmat2; FP32-precise tier is the *portable+bit-exact* companion.

## ✅ CKIR PORT — increment 4: the BACKWARD pass, DETERMINISTIC dW (2026-07-14)

The training half is now in CKIR, statement-tier, and holds the determinism moat the standalone could not. Two builders
(ckir_mlp.hpp), no atomics:

- **Kernel A `build_mlp_bwd_dz`** — the per-sample activation-gradient chain. One workgroup = one sample; walk layers
  backward computing `dz[l]` on-chip (ReLU′ mask via `select(a[l+1]>0, g, 0)`), `da = dz·Wᵀ` → next g; write each `dz[l]`
  to `dz_all`.
- **Kernel B `build_mlp_bwd_dw`** — the **deterministic** weight-gradient reduction: `dW[l][k][n] = Σ_r a[l][r][k]·dz[l][r][n]`
  summed in **ascending sample order** (fixed order, NO atomics). The standalone's fp32-`atomicAdd` dW was non-deterministic;
  this is bit-exact **and run-to-run bit-identical**.

| check | result |
|---|---|
| Kernel A dz vs CPU oracle (`mlp_backward_ref`) | **bit-exact** |
| Kernel B dW vs CPU oracle | **bit-exact** |
| **Vulkan** dz + dW vs oracle | **bit-exact** (`bad==0`) |
| **Vulkan** dW run-to-run | **BIT-IDENTICAL** (`det==0`) |

Both built on the CKIR statement tier ⇒ the generic emitters lower them to every backend (same as the forward FP32 tier).
Tests: `test_ckir_mlp.cpp` (backward CPU, 28k `==`), `test_vulkan_context.cpp` `[mlp]` (Vulkan run + determinism). crd-kir
28883/128 green on MSVC + clang-cl; tidy-clean.

## ✅ CKIR PORT — increment 5: DX12 backward + CUDA tensor backward (2026-07-14)

- **Backward on DX12** — the FP32-precise statement-tier backward (both builders) now runs on DX12 too:
  `bad_dz==0`, `bad_dw==0`, **bit-identical to Vulkan + the oracle**. The deterministic dW reduction is portable across
  both runnable statement-tier backends.
- **CUDA tensor backward** — `emit_fused_mlp_bwd_cuda` (ckir_mlp.hpp) emits `reduce_dw` + the fused wmma backward
  (per-sample dz mask + on-chip da wmma chain + dW = aᵀ·dz wmma reduced over the batch → fp32 atomic into NGROUP
  partials), the CKIR mirror of the forward tensor recipe. Generated `ckir_mlp_bwd_gen.cu` compiles (nvcc) and, via
  `ckir_mlp_bwd_bench.cu`, **reproduces the crush: 1.94× vs cuBLAS** (cuBLAS at its best split-K algo), dW correct
  (`rel=5e-5`). ⚠ fp32-atomic dW ⇒ the *crush* tier (per-backend, not bit-exact); the FP32 statement-tier backward is the
  bit-exact/deterministic companion.
- **Vulkan coopmat2 backward** — not built: a coopmat2 batch-reduction dW is disproportionately hard, and Vulkan already
  has the deterministic FP32 backward. Optional follow-on.

## Status — the full NRC moat in CKIR

| pass | tensor tier (CRUSH) | FP32-precise tier (PORTABLE + BIT-EXACT) |
|---|---|---|
| Forward | CUDA wmma **2.41×** · Vulkan coopmat2 | statement-tier: Vulkan+DX12 **run bit-exact**, CUDA nvcc, WGSL/MSL emit |
| Backward | **CUDA wmma 1.94×** (CKIR) | statement-tier: **Vulkan + DX12 run bit-exact + deterministic dW** |

The full NRC moat — forward inference crush **and** backward training crush **and** deterministic bit-exact training — is
now authored in CKIR. The two standalone .cu files remain the gold-standard references the tensor path matches.
