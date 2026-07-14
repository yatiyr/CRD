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

## Next (remaining CKIR-port increments)

- Fan the forward emitter out to DX12/HLSL WaveMatrix, Metal simdgroup_matrix, WebGPU subgroup-matrix.
- Port the **backward** pass into CKIR (dW reduction + on-chip da chain), with a fixed-order deterministic dW
  reduction (no atomics) so the training half also holds the `{1..16}` moat.

These two standalone .cu files remain the gold-standard references the port must match.
