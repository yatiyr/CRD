# ADR-0098 — crd-hesap-gpu: the GPU compute cluster — the CPU hesap suite mirrored onto backend-neutral compute, the determinism moat's hardest test (T1/T2/T3 + certificates), and the reserved cross-vendor kernel seam

- **Status:** Proposed (2026-07-07) — v17 kickoff. Per the ADR-at-slice-time pattern (ADR-0080/0096/0097 precedents), the base text is Proposed-state framing; implementation revisions fold into "Amendments at v17-close" at the bottom.
- **Phase:** 3.1.6 v17 (GPU compute) — one cluster, `crd-hesap-gpu`, mirroring the v14–v16 CPU surface onto the GPU
- **Tags:** `hesap` `gpu` `vulkan` `rhi-compute` `determinism` `reproducibility` `certificates` `slang` `portability` `architecture` `substrate`
- **Plan:** `docs/phases/phase-3.1.6-v17.md` (v17 DETAILED PLAN — a–z); master row in `phase-3.1.6-hesap.md` (v17); research dossier `docs/research/2026-07-07-v17-gpu-crush.md`; memory `project_v14_v18_planning`.
- **Depends:** ADR-0080 (crd-rhi-compute — the backend-neutral compute interface, Accepted/shipped) · crd-shader (GLSL→SPIR-V, HLSL; `compile.hpp`) · ADR-0085 (streaming staging/ring allocators) · the v14 tensor surface (device mirror) · v16 autodiff (v17-j GPU tape, after v16). **Precedent proven:** `engine/geometry-bvh-gpu` already dispatches real compute (LBVH build + radix sort via `.comp` shaders through rhi-vulkan) — the dispatch/buffer/shader path works today.

## Context

v14 gave the engine its tensor/NN surface, v15+v16 its differentiation layer — all on the CPU, all crushing their gold standards at matched accuracy with the `{1..16}`-worker bit-identical determinism moat. v17 mirrors that suite onto the GPU: the same GEMM/FFT/reduce/scan/sparse/dense-LA/NN/autodiff operations, dispatched through the backend-neutral `crd-rhi-compute` interface, benchmarked against the portable-GPU gold standards (vkFFT, CLBlast, MAGMA, ncnn/llama.cpp-Vulkan) — and, uniquely, carrying the determinism/reproducibility/certification column no GPU library holds.

**Kickoff-check findings (2026-07-07, recorded per the plan):**
- **Host GPU:** NVIDIA GeForce RTX 4070 Ti SUPER (Ada, SM 8.9). **Vulkan SDK 1.4.341.1** present; `glslc` (GLSL→SPIR-V) on PATH; `vulkaninfo` present.
- **CUDA toolkit ABSENT** (`nvcc` not found). ⇒ the vendor-CUDA peers (cuBLAS/cuSPARSE/CUTLASS/CUB-RFA) are **conditionally out** unless CUDA is later installed; the crush axis is **portable-Vulkan gold standards** (vkFFT/CLBlast/MAGMA/ncnn) + the determinism column. The RTX card still lets us bench the portable Vulkan path on real Ada silicon and (if CUDA is installed mid-phase) add the vendor column.
- **Substrate maturity:** `crd-rhi` (`ComputePipeline`, `CommandBuffer::bind_compute_pipeline`, dispatch), `crd-rhi-vulkan` (`vulkan_backend.cpp`, `ValidationCapture`), `crd-shader` (`compile.hpp` GLSL→SPIR-V + HLSL) all present and exercised by `geometry-bvh-gpu`. v17 does **not** bootstrap the backend — it builds kernels on a working one.

The hard problems v17 owns: (1) **portable perf** — matching vendor-tuned kernels through a portable API; (2) **the determinism moat on the GPU** — where the CPU's "fixed reduction order, no atomics" rule meets warp-nondeterminism, atomic scatter-adds, and cross-vendor FP; (3) **verifiable compute** — hash-chained execution transcripts as DO-178C-grade evidence.

## Decision

### 1. ONE cluster module, `crd-hesap-gpu`, riding `crd-rhi-compute` — API-AGNOSTIC BY CONSTRUCTION
One module mirrors the CPU hesap surface (sub-areas: `gemm/` · `reduce/` · `fft/` · `sparse/` · `la/` · `nn/` · `autodiff/` · `determinism/`). **hesap-gpu never `#include`s Vulkan** — every kernel launches through the backend-neutral `crd-rhi-compute` interface (buffers, pipelines, dispatch, barriers) and every kernel *source* is GLSL compiled by `crd-shader`. This keeps the compute logic backend-portable and is gated by a link-isolation smoke (no `vulkan.h` symbol in `crd-hesap-gpu`).

### 2. Vulkan-first; the Slang track SEAMED, not built; the `crd-rhi-cuda` kernel-ABI reserved
Kernels are authored in **GLSL → SPIR-V** now (`glslc` present, the `geometry-bvh-gpu` precedent). We **reserve** — but do not yet build — (a) the **Slang kernel-language track** (one kernel source → SPIR-V now, CUDA/PTX + Metal + DXIL later; Khronos-governed) and (b) the **`crd-rhi-cuda` kernel-ABI seam** (a pinned descriptor/dispatch/memory contract) so a CUDA or Metal backend is a pure drop-in. The ADR pins the seam's shape; the port is a later phase. Rationale: shipping GLSL-first on the proven path beats blocking v17 on a kernel-language migration, while the seam keeps the door open (ADR-0082-class portability honesty).

### 3. NO float atomics anywhere — deterministic fixed-tree reductions as the STANDING RULE
Every reduction/scan/scatter uses a **fixed reduction tree** (deterministic order, independent of launch/warp scheduling), never `atomicAdd` on floats. This is the GPU expression of the CPU moat (`batch_gradient`'s fixed-order fold). It is a *correctness* rule, not a tuning knob — a kernel that float-atomics is a bug.

### 4. THE GPU DETERMINISM TIERS (the research crown) + certificates
- **T1 — run-to-run, same device:** fixed trees, no atomics ⇒ bit-identical across launches. The full moat; gated `gpu_determinism_check` ×3 every slice.
- **T2 — cross-ARCHITECTURE reproducible reductions:** binned / reproducible-floating-point (RFA/Kahan-tree) accumulation ⇒ bit-identical regardless of block size / occupancy / GPU generation. Benched against NVIDIA CCCL's reproducible-reduction idea.
- **T3 — cross-VENDOR bit-exact for IEEE-only kernels:** SPIR-V `NoContraction` + a `VK_KHR_shader_float_controls` (rounding-mode / denorm / FMA-contraction) audit, **plus `crd::math` deterministic transcendentals PORTED TO GLSL** (the `geometry-shader-helpers` GLSL-emit precedent) ⇒ cross-vendor deterministic transcendentals on the GPU. Publishable; no GPU library has it.
- **Computation CERTIFICATES:** a hash-chained transcript of the deterministic execution (kernel ids, dispatch dims, input/output digests) = a verifiable-compute artifact ("this exact result from this exact computation"). DO-178C evidence + trustless-compute; pairs with the 2025–26 bit-exact-inference-verification / verifiable-training literature.

### 5. Gold standards — portable-first, honest
**vkFFT** (THE Vulkan FFT bar) · **CLBlast** (portable BLAS) · **MAGMA** (batched dense LA) · **ncnn** + **llama.cpp-Vulkan/ggml** (NN inference) · published **merge-based SpMV** numbers · the **`vk_cooperative_matrix_perf`** methodology for the GEMM ceiling. Vendor CUDA (cuBLAS/cuSPARSE/CUTLASS/CUB-RFA) **only if CUDA is installed** (currently absent — noted per slice). The crush axes: **vkFFT-class portable perf** + **the determinism/certification column no GPU library carries**. The portable-Vulkan vs vendor-CUDA gap is named up front on every board (ADR-0082-class honesty) — we do not pretend a portable kernel beats a hand-tuned PTX kernel; we win on portable-peer parity + the moat.

### 6. DoD §6 GPU discipline — every slice
`ValidationCapture` 0 errors · **bit/ulp GPU-vs-CPU oracle** (the v14–v16 CPU result is the ground truth) · `gpu_determinism_check` ×3 (T1) · `CRD_PERF_BUDGET_LE` · `-IncludeRelease` in the sweep. A GPU slice is not done until its result bit/ulp-matches the CPU oracle AND replays bit-identically ×3.

## Consequences

- **Positive:** the whole compute suite becomes GPU-resident on a portable API; the determinism moat gets its hardest and most publishable test; the reserved seams keep CUDA/Metal a drop-in; every Cerid consumer (eylem physics, rendering, ML) gets GPU compute with a CPU-bit-exact oracle.
- **Negative / risks:** portable Vulkan will trail hand-tuned vendor CUDA on raw FLOPs (named honestly, not hidden); cross-vendor bit-exactness (T3) depends on driver `float_controls` support (audited, not assumed); the Slang/CUDA ports are deferred (seamed, not built) — a real but bounded scope line, recorded, not silent.
- **Calendar:** ~9–11 weeks / ~23 KLOC / ~690 tests (master-row estimate). Accepted at kickoff with full visibility.

## Alternatives considered

- **Per-domain GPU modules** (hesap-gpu-fft, hesap-gpu-gemm, …) — rejected: they'd duplicate the buffer/dispatch/determinism plumbing; one cluster with sub-areas shares it (the v16 one-module precedent).
- **CUDA-first** — rejected: MSVC-incompatible toolchain friction, non-portable, and CUDA is absent on the host; Vulkan-first + the reserved CUDA seam gets portability now and the vendor path later.
- **Float atomics for speed** — rejected outright: breaks the moat (the whole point of v17's determinism crown). Fixed trees are the rule.
- **Skip determinism, chase FLOPs** — rejected: FLOP-parity with vendor CUDA on a portable API is not achievable and not the differentiator; the determinism/certification column is what no one else has.
