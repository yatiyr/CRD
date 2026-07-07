# Phase 3.1.6 v17 — GPU compute (`crd-hesap-gpu`) — DETAILED PLAN

> The CPU hesap suite (v5–v16) mirrored onto backend-neutral GPU compute, benchmarked against the portable-GPU gold
> standards, carrying the determinism/reproducibility/certification column no GPU library holds. **ADR-0098** pins the
> architecture (one cluster riding `crd-rhi-compute`; API-agnostic/Vulkan-first; NO float atomics; the T1/T2/T3
> determinism tiers + certificates; portable-first gold standards, honest vendor-gap framing). Master row in
> `phase-3.1.6-hesap.md` (v17); research dossier `docs/research/2026-07-07-v17-gpu-crush.md`.

## Kickoff state (2026-07-07)

- **Environment:** NVIDIA RTX 4070 Ti SUPER (Ada, SM 8.9) · Vulkan SDK 1.4.341.1 · `glslc` on PATH · CUDA toolkit
  **absent** (vendor cuBLAS/cuSPARSE peers conditional; portable-Vulkan peers are the crush axis).
- **Substrate (proven working):** `crd-rhi` (`ComputePipeline`, dispatch) · `crd-rhi-vulkan` (`ValidationCapture`) ·
  `crd-shader` (GLSL→SPIR-V) — all exercised today by `engine/geometry-bvh-gpu` (LBVH build + radix sort on GPU).
- **The rule:** every reduction/scan/scatter uses a FIXED reduction tree (no float atomics) — the GPU moat.
- **Every slice:** GPU result bit/ulp-matches the v5–v16 CPU oracle · `ValidationCapture` 0 errors ·
  `gpu_determinism_check` ×3 (T1) · `CRD_PERF_BUDGET_LE` · `-IncludeRelease`.

## Slice table

| slice | scope | gold standards | oracle (CPU) | LOC | tests |
|---|---|---|---|---:|---:|
| **v17-a** | **GPU foundation + first deterministic kernel + the oracle harness.** The `crd-hesap-gpu` module skeleton over `crd-rhi-compute`: device buffer alloc (ADR-0085 staging/ring), a dispatch helper, the GLSL→SPIR-V kernel-build path, and the **GPU↔CPU bit/ulp oracle harness** + `gpu_determinism_check` (×3, T1). First kernel: a **fixed-tree reduction** (sum/dot, no atomics). Link-isolation smoke (no `vulkan.h` in `crd-hesap-gpu`). | — (harness) | v-any reduce | ~1600 | ~45 |
| **v17-b** | **Reduction + scan primitives.** Fixed-tree reduce (sum/max/min/dot/norm), Blelloch inclusive/exclusive scan (deterministic), segmented reduce/scan. **T2 down-payment:** binned / reproducible-FP (RFA/Kahan-tree) accumulation ⇒ block-size-invariant. | published scan; CCCL reproducible-reduce idea | CPU reduce/scan | ~1800 | ~55 |
| **v17-c** | **★GEMM — the crown.** Tiled + **cooperative-matrix** (`VK_KHR_cooperative_matrix`, Ada) GEMM (f32/f16), deterministic fixed-K reduction. The portable-perf ceiling test. | `vk_cooperative_matrix_perf` methodology · CLBlast · (cuBLAS if CUDA) | v14/hesap-dense GEMM | ~2400 | ~70 |
| **v17-d** | **The v14 tensor surface, device-mirrored.** Device tensors (f32/f16/bf16), elementwise + broadcast + transpose/permute + the **einsum device path** (rides GEMM). Boundary conversions exactly twice (AoSoA lesson). | — (capability) | v14 tensors | ~2200 | ~65 |
| **v17-e** | **★FFT — vs vkFFT.** Stockham/radix FFT (1D/2D, C2C + R2C), deterministic twiddles (crd::math in GLSL). | **vkFFT** (the Vulkan FFT bar) | v10/hesap-fft | ~2400 | ~70 |
| **v17-f** | **Sparse — merge-based SpMV/SpMM.** CSR SpMV (merge-path, load-balanced), SpMM; deterministic fixed-order segment reduction. | published merge-SpMV | v5/hesap-sparse | ~1900 | ~55 |
| **v17-g** | **Dense LA — batched.** Batched Cholesky/LU/QR + triangular solve on GPU. | **MAGMA** batched | v5/hesap-direct | ~2200 | ~60 |
| **v17-h** | **★★crd::math deterministic transcendentals in GLSL (T3).** Port the crd::math surface (exp/log/sin/cos/tanh/…) to GLSL, **cross-vendor bit/ulp-deterministic** via SPIR-V `NoContraction` + a `VK_KHR_shader_float_controls` audit. The T3 foundation — publishable. | libm-on-GPU (driver) | crd::math CPU (ulp tier) | ~1800 | ~55 |
| **v17-i** | **NN inference — vs ncnn / llama.cpp-Vulkan.** Conv/matmul/attention/layernorm/softmax/GELU on GPU (f16), rides GEMM + the tensor mirror. | **ncnn** · **llama.cpp-Vulkan/ggml** | v14 NN ops | ~2200 | ~60 |
| **v17-j** | **Autodiff on GPU (after v16).** The reverse tape on device with the **deterministic fixed-order fold** ⇒ GPU gradients bit-identical across launch config. Gradient of a GPU loss (NN backward). The moat extends to GPU gradients. | PyTorch-CUDA (if CUDA) — else capability | v16 CPU tape | ~2200 | ~60 |
| **v17-k** | **★★The determinism tiers + computation CERTIFICATES (the research crown).** Consolidate T1 (run-to-run) / T2 (cross-architecture reproducible reductions) / T3 (cross-vendor IEEE-only + transcendentals) across the kernels; the **hash-chained computation-certificate** artifact (kernel ids + dispatch dims + IO digests = verifiable-compute evidence). The publishable crown; pairs with the 2025–26 verifiable-inference literature. | CCCL-RFA (T2) · none for certificates | — (novel) | ~2000 | ~55 |
| **v17-z** | **CLOSE.** CLI `hesap.gpu.*` + system doc `docs/systems/hesap-gpu.md` + ADR-0098 finalize + the **full scoreboard** (vkFFT/CLBlast/MAGMA/ncnn + the T1/T2/T3 determinism + certificate columns) + conformance + the determinism-tier sweep across configs. | all | all | ~1400 | ~40 |

**Totals (master-row estimate):** ~23.1 KLOC / ~690 tests / ~9–11 weeks.

## Sequencing rationale

Foundation + oracle (a) → primitives reduce/scan (b) → GEMM crown (c) → tensor mirror (d, rides GEMM) → FFT (e) →
sparse (f) → dense LA (g) → transcendentals/T3 (h, the cross-vendor foundation) → NN (i, rides GEMM+tensor) →
autodiff (j, after the v16 CPU tape) → the determinism crown + certificates (k, consolidates the moat) → close (z).
Each op slice is gated against its CPU oracle (bit/ulp) before any perf claim; determinism (T1 ×3) is a per-slice DoD
gate, not a final afterthought.

## Standing risks (from ADR-0098)

- **Portable-vs-vendor perf gap** — named honestly on every board; we win on portable-peer parity + the moat, not on
  out-FLOPping hand-tuned PTX.
- **T3 cross-vendor bit-exactness** depends on driver `float_controls` support — audited per kernel, not assumed.
- **Slang / CUDA ports are SEAMED, not built** (ADR-0098 §2) — a bounded, recorded scope line, not a silent
  reduction; the `crd-rhi-cuda` kernel-ABI seam is pinned so the port is a later drop-in.

## Session log

- **2026-07-07 — v17 KICKOFF.** ADR-0098 proposed (architecture + determinism tiers + gold standards + honest vendor
  gap). Kickoff check recorded (RTX 4070 Ti SUPER + Vulkan 1.4.341 + glslc; CUDA absent ⇒ portable peers; substrate
  proven via geometry-bvh-gpu). Detailed a–z plan locked (this doc). **NEXT: v17-a** — the module skeleton over
  rhi-compute + the GPU↔CPU oracle harness + the first fixed-tree deterministic reduction (T1 ×3).
