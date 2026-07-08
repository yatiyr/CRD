# Phase 3.1.6 v17 — the Cerid GPU compute compiler (`crd-kir` + `crd-hesap-gpu`) — DETAILED PLAN

> **MAXIMAL scope (user direction 2026-07-07).** Not "port the suite to Vulkan" — **build the compute substrate every
> Cerid project stands on**: a unified compute+autodiff kernel IR (**CKIR**) that lowers to **six backends**
> (Vulkan/CUDA/WebGPU/Metal/DX12/ROCm), **beats the vendor kernels** (cuBLAS/cuFFT/cuDNN/rocBLAS), and carries
> **certified cross-vendor determinism + computation certificates**. Two modules: **`crd-kir`** (the compiler, a new
> foundational substrate below hesap-gpu) and **`crd-hesap-gpu`** (the op library authored in CKIR). Architecture:
> **ADR-0098**. This is a tensor-compiler build (Triton/TVM/XLA-class) — the largest hesap slice, sized honestly at
> **~55–75 KLOC / ~18–26 weeks**. Master row `phase-3.1.6-hesap.md` (v17); dossier
> `docs/research/2026-07-07-v17-gpu-crush.md`.

## Kickoff state (2026-07-07)

- **Env:** RTX 4070 Ti SUPER (Ada SM 8.9) · Vulkan SDK 1.4.341 · `glslc`. **CUDA toolkit ABSENT → install nvcc +
  cuBLAS/cuFFT/cuDNN/cuSPARSE/cuSOLVER in v17-a** (required for the PTX backend AND the beat-vendor bar).
- **Substrate proven:** rhi / rhi-vulkan / shader exercised by `geometry-bvh-gpu` today.
- **The four locked decisions (ADR-0098):** ① a Cerid kernel DSL/IR (we build the compiler) ② six backends shipped
  ③ beat the vendor kernels ④ certified bit-exact core + reproducible elsewhere.
- **The rule:** NO float atomics — fixed reduction trees, IR-enforced. Every op bit/ulp-gated vs the CKIR **CPU
  reference** (the single oracle) + `gpu_determinism_check` ×3 + cross-backend conformance.

## Part A — `crd-kir`: the compiler substrate

| slice | scope | gold standard / gate | LOC | tests |
|---|---|---|---:|---:|
| **v17-a** | **CKIR-Graph IR (minimal RISC op set) + CPU reference + the oracle/determinism harness.** Define the **~20–25 primitive ops** (crd::math unary transcendentals · binary arith/compare · reduce sum/max · movement reshape/permute/broadcast/pad/stride · gather/scatter · cast · one contraction) — the tinygrad/Luminal lesson: small set ⇒ small backends + composable op library + search-based tuning. The typed op-graph is **the v16-h `graph_ad` IR extended to GPU** (device memory spaces, ADR-0096 dtypes) ⇒ differentiable by construction. Passes: const-fold/CSE/DCE/fusion. The **CPU reference lowering** (scalar/SIMD over crd-math) = the oracle + determinism ground truth. Harness: bit/ulp compare + `gpu_determinism_check`. **Install the CUDA toolkit.** | IR round-trips; CPU-ref ≡ v14/v16 CPU results | ~5000 | ~110 |
| **v17-b** | **CKIR-Tile IR + the Vulkan (SPIR-V) backend + the runtime.** The tile/loop/schedule IR (tiling, shared/registers, fixed-tree reductions). Graph→Tile lowering; Tile→SPIR-V (via crd-shader); the backend-neutral runtime over `crd-rhi-compute` (buffers/dispatch/sync, ADR-0085 staging). First GPU kernels (elementwise + reduce). | **GPU ≡ CPU-ref bit/ulp**; T1 ×3; ValidationCapture 0 | ~5000 | ~120 |
| **v17-c** | **The CUDA (PTX/NVRTC) backend + driver.** Tile→PTX; the CUDA runtime driver (streams/events/memory). First vendor benches (cuBLAS/cuFFT baseline calibration). | CUDA ≡ CPU-ref; determinism ×3 | ~4000 | ~90 |
| **v17-d** | **The four remaining backends: WebGPU (WGSL), DirectX 12 (DXIL), Metal (MSL), ROCm/HIP.** One Tile codegen + one runtime driver each; the op library is untouched. **WebGPU = the browser/WASM path.** (Metal/ROCm/DX12 device coverage staged as hardware/CI arrives — gaps logged, never claimed.) | cross-backend conformance ≡ CPU-ref | ~8000 | ~150 |
| **v17-e** | **The scheduler + autotuner — the vendor-beating engine.** Schedule search (tiles/unroll/vectorize/coop-matrix/pipeline) + a measured autotuner → a **checked-in tuning DB** (deterministic replay, no runtime tuning). Hand-seeded schedules + tensor-core/cooperative-matrix intrinsics for the hot ops. | measured speedups reproducible from the DB | ~6000 | ~110 |
| **v17-f** | **★★Determinism tiers + transcendentals + certificates.** T1 (fixed trees) · T2 (binned/RFA, block-size-invariant) · T3 (cross-vendor bit-exact IEEE-only via `NoContraction` + `float_controls` audit). **crd::math transcendentals AS CKIR ops** (one polynomial def → identical on every backend ⇒ cross-vendor deterministic transcendentals). **Computation certificates** (hash-chained IR/dispatch/IO-digest transcripts). | cross-vendor bit-exact on the certified core; certificates verify | ~5000 | ~110 |

## Part B — `crd-hesap-gpu`: the op library on CKIR (each op = multi-backend + differentiable + deterministic, for free)

| slice | scope | gold standards (beat) | oracle | LOC | tests |
|---|---|---|---|---:|---:|
| **v17-g** | **★GEMM — the crown.** Tensor-core/cooperative-matrix GEMM (f16/bf16/f32/f64), autotuned. | **cuBLAS** · CUTLASS · CLBlast | v14 GEMM | ~4500 | ~85 |
| **v17-h** | **★FFT.** Stockham/radix (1D/2D, C2C/R2C), deterministic twiddles (transcendentals-in-IR). | **cuFFT** · vkFFT | v10 FFT | ~4500 | ~80 |
| **v17-i** | **Reduce/scan/sort + the v14 tensor surface device mirror.** Fixed-tree reduce/scan/segmented, radix sort; device tensors (elementwise/broadcast/transpose/einsum on GEMM). | CUB · thrust | v14 tensors | ~4000 | ~85 |
| **v17-j** | **Sparse — SpMV/SpMM (merge-path, deterministic).** | **cuSPARSE** · merge-SpMV | v5 sparse | ~3500 | ~70 |
| **v17-k** | **Dense LA — batched Cholesky/LU/QR + solve.** | **cuSOLVER** · MAGMA | v5 direct | ~4000 | ~75 |
| **v17-l** | **NN — conv/attention/norm/softmax/GELU (f16).** | **cuDNN** · ncnn · llama.cpp-Vulkan | v14 NN | ~4500 | ~80 |
| **v17-m** | **Autodiff on GPU.** The reverse tape on device — **free from CKIR being the autodiff IR**; deterministic fixed-order fold ⇒ GPU gradients bit-identical across launch config. NN backward end-to-end. | PyTorch-CUDA (perf); v16 (oracle) | v16 tape | ~3500 | ~70 |
## Part C — cross-platform validation on REMOTE hardware (Metal + ROCm), then close

> The implementation is made solid on the local machine FIRST (Vulkan/CUDA/DX12/WebGPU + HIP-on-NVIDIA all testable on
> the RTX 4070 Ti), THEN validated + debugged on real Apple + AMD silicon — running the **ENTIRE engine + full test
> suite** for cross-platform sanity, not just the GPU kernels. The CPU-reference oracle makes each remote session a
> quick bit/ulp **confirmation** ("does this backend match the known-correct answer on this silicon?"), not a debug
> marathon. Own-the-core codegen is developed locally; only real-hardware *execution* + the T3 cross-vendor bit-exact
> proof + the vendor-perf numbers need the remote box.

| slice | scope | hardware / how | LOC | tests |
|---|---|---|---:|---:|
| **v17-n** | **macOS + Metal validation + full-system sanity.** A `macos-metal` build preset; the WHOLE engine builds on Apple Silicon; the **FULL test suite green on macOS** (cross-platform sanity — catches platform bugs across the codebase, not only v17); **Metal backend conformance** (bit/ulp vs the CPU oracle) + **T3 cross-vendor bit-exactness on the Apple GPU** (the crown, provable only on real Apple silicon). | **GitHub Actions macOS runners (Apple Silicon) — FREE + continuous per-commit**; a Scaleway Mac-mini hourly burst only for interactive debugging | ~1500 | full suite + Metal conformance |
| **v17-o** | **Linux + AMD/ROCm validation + full-system sanity.** A `linux-rocm` preset + a turnkey RunPod setup script; the whole engine builds on the AMD box; the **FULL test suite green on ROCm-Linux**; **ROCm/HIP backend conformance** (bit/ulp vs oracle) + **T3 cross-vendor bit-exactness on AMD** + the **rocBLAS/rocFFT/rocSPARSE perf** numbers. (HIP-on-NVIDIA already exercised the HIP codegen locally in v17-d, so this is confirmation + AMD-specific behavior + perf.) | **RunPod AMD-Instinct bursts** (~a few hours, ~a few dollars) or a self-hosted AMD runner | ~1500 | full suite + ROCm conformance |
| **v17-z** | **CLOSE.** CLI `hesap.gpu.*` · system docs (`crd-kir.md` + `hesap-gpu.md`) · ADR-0098 finalize · the **full scoreboard** (cuBLAS/cuFFT/cuDNN/cuSPARSE/cuSOLVER + rocBLAS/rocFFT + vkFFT/CLBlast/MAGMA/ncnn + the T1/T2/T3 + certificate columns, across ALL validated backends) · cross-backend conformance sweep · determinism-tier sweep on NVIDIA + Apple + AMD. | all (local + remote) | ~2500 | ~55 |

**Totals (honest, revised):** ~66 KLOC / ~1480 tests / ~19–27 weeks (incl. the two cross-platform validation slices). The strategic keystone of hesap.

## Sequencing rationale

Compiler before ops: the IR + CPU reference + harness (a) → the tile IR + first real backend Vulkan (b) → CUDA for
the vendor bar (c) → the other four backends (d) → the autotuner that makes us fast (e) → the determinism crown baked
into the IR (f). THEN the op library (g–m), each authored once in CKIR and inheriting all six backends + autodiff +
determinism. Autodiff-on-GPU (m) is nearly free because CKIR-Graph *is* the autodiff graph. Every op gates vs the CPU
reference (bit/ulp) + determinism ×3 + cross-backend conformance before any perf claim. THEN, once the whole system is
solid locally, Part C (n–o) validates + debugs it on real Apple + AMD silicon (full engine + full test suite) and
certifies the cross-vendor determinism crown, before the close (z).

## Standing risks (from ADR-0098)

- **It's a compiler** — the largest hesap build; sliced a–z, each slice independently gated + benched.
- **Beating vendor SASS is not guaranteed per-op** — the honest gap is named on each board; the autotuner + coop-matrix
  intrinsics are the levers.
- **T3 cross-vendor bit-exactness is bounded by driver `float_controls`** — audited per backend, not assumed; the
  certified core is what passes the audit, the rest is T2-reproducible.
- **Hardware coverage strategy (user-decided 2026-07-07):** develop everything on the RTX 4070 Ti + Windows, where
  **4 of 6 backends are fully local** — Vulkan (SPIR-V), CUDA (PTX), DirectX 12 (DXIL), and **WebGPU (WGSL, native +
  in a real browser)** — plus **HIP-on-NVIDIA** exercises the ROCm codegen path locally. Only **Metal** (needs Apple
  silicon) and real **AMD** (perf + AMD FP behavior + the T3 AMD proof) need remote hardware — handled by the dedicated
  Part C slices: **Metal via free continuous GitHub Actions macOS (Apple Silicon) CI**, **ROCm via RunPod bursts**. The
  CPU-reference oracle makes each remote run a cheap bit/ulp confirmation. Coverage gaps logged, never claimed as
  passing.

## Session log

- **2026-07-07 — v17 KICKOFF + MAXIMAL RE-SCOPE.** Four architecture decisions locked (user): Cerid kernel DSL/IR ·
  six backends · beat vendor kernels · certified bit-exact core + reproducible rest. ADR-0098 rewritten around
  **CKIR** (the two-level compute+autodiff kernel compiler, module `crd-kir`) + `crd-hesap-gpu` (the op library).
  Detailed a–m+z plan (this doc), honestly re-sized ~63 KLOC / ~18–26 weeks. **NEXT: v17-a** — CKIR-Graph IR + the CPU
  reference lowering + the oracle/determinism harness; **install the CUDA toolkit** (nvcc + cuBLAS/cuFFT/cuDNN).
- **2026-07-07 — v17 deep-research refinement + v17-a STARTED.** Web-researched the 2026 frontier (dossier §7): the
  **minimal RISC op-set** lesson (tinygrad ~12 ops <1kLOC/backend; Luminal 15 ops, search-based) · determinism is a hot
  topic that VALIDATES the moat (Thinking Machines batch-invariant kernels, RepDL, TBIK) · **(a) own-the-core codegen
  CONFIRMED** (Slang/IREE/Enzyme-MLIR as references) · certificates = cheap tier + ZK hook · agent-drivable autotuner.
  **▶ v17-a foundation LANDED:** new module **`crd-kir`** (peer of crd-math, below hesap-gpu) — **`ckir.hpp`**
  (CKIR-Graph IR: DType, Shape, the ~27 KOp minimal RISC set, KNode, KGraph + builder with shape/dtype inference) +
  **`ckir_eval.hpp`** (the deterministic CPU reference interpreter — materializes each node, fixed-order reductions/
  contractions). **★ GATE (`test_ckir_graph.cpp`, 21 asserts/6 cases GREEN):** elementwise+reduce ≡ crd::math oracle ·
  Contract = matmul (verified) · permute/broadcast/reshape · a composed matmul→relu→sum graph · **DETERMINISTIC** (eval
  twice bit-identical) · **dtype-faithful** (F32 node rounds to f32). Tidy-clean (dogfooded; logged a
  `readability-non-const-parameter` misfire in `docs/hints/v17-kir-gpu-gotchas.md`). Registered in the build (module +
  tests). **▶ SYMBOLIC REVERSE-AD LANDED (`ckir_grad.hpp`):** tensor-level VJPs for the whole differentiable core
  (Add/Sub/Mul/Div/Neg/Recip/Exp/Log/Sin/Cos/Sqrt/Tanh/Abs/Max/Min/Select/Cast/Reshape/Permute/Broadcast/ReduceSum/
  ReduceMax/Contract) emitted as NEW graph nodes ⇒ any CKIR kernel differentiable on every backend for free (the
  JAX/XLA payoff, deterministic). Copy-node-before-emit (the builder-realloc dangling-ref hazard, learned). **★ GATE
  (`test_ckir_grad.cpp`, +23 asserts):** `L=sum(tanh(X@W))` gradX/gradW ≡ central FD (`<1e-6`) + `L=sum(exp(X+bias_
  bcast))` gradX/gradbias ≡ FD (Contract/Tanh/Exp/Add/Broadcast/ReduceSum VJPs verified; broadcast VJP = reduce-back).
  **kir suite 44 asserts/8 cases GREEN**, tidy-clean. **CUDA 13.3 INSTALLED + verified** (nvcc 13.3 + cublas.lib +
  cufft.lib — ready for v17-c) + Slang 2025.18.1 (reference). **NEXT (v17-a cont.):** the optimization passes
  (const-fold/CSE/DCE/fusion) + the oracle-harness helpers (bit/ulp compare); then v17-b (CKIR-Tile + Vulkan backend).
- **2026-07-07 — HARDWARE-COVERAGE STRATEGY locked + Part C added (user).** cuDNN installed (v17-l ready). Decision:
  develop everything on the RTX 4070 Ti (4/6 backends local: Vulkan/CUDA/DX12/WebGPU + HIP-on-NVIDIA), then validate on
  real Apple + AMD silicon via **dedicated slices** — **v17-n** (macOS + Metal via free continuous GitHub Actions
  macOS/Apple-Silicon CI) and **v17-o** (Linux + ROCm via RunPod AMD-Instinct bursts) — each running the **FULL engine
  + full test suite** for cross-platform sanity, plus backend conformance (bit/ulp vs the CPU oracle) + the T3
  cross-vendor bit-exact proof + vendor perf. The oracle makes remote runs a cheap confirmation. Plan re-sized to
  ~66 KLOC / ~1480 tests / ~19–27 wk (a–o + z). ADR-0098 + master table updated.
- **2026-07-07 — v17-a ✅ COMPLETE.** All four pieces shipped: **`ckir.hpp`** (IR + builder + the `optimize` passes:
  const-fold → DCE → CSE hash-cons, lifting the v16-h pattern to tensors) · **`ckir_eval.hpp`** (deterministic CPU
  reference oracle) · **`ckir_grad.hpp`** (symbolic tensor reverse-AD, FD-gated) · **`ckir_harness.hpp`** (bit-exact +
  ULP oracle primitives). **★ GATE (60 asserts/11 cases):** semantics ≡ crd::math oracle · Contract=matmul ·
  DETERMINISTIC · dtype-faithful · reverse-AD ≡ central FD · **optimize preserves eval BIT-IDENTICALLY + shrinks (incl.
  on a real reverse-AD gradient graph) + idempotent**. GREEN on **MSVC + gcc**, tidy-clean; a `bugprone-branch-clone`
  caught (ReduceMax/movement both identity-on-uniform-fill → merged) + logged. Kernel FUSION correctly placed at
  v17-b (CKIR-Tile, where kernels exist — not deferred, correctly homed). **NEXT: v17-b — CKIR-Tile IR + the Vulkan
  (SPIR-V) backend + the runtime over crd-rhi-compute; first GPU kernels on the RTX 4070 Ti, gated GPU ≡ CPU-ref
  bit/ulp + determinism ×3.**
- **2026-07-07 — v17-b OPENED: the codegen core landed.** Studied the proven compute-dispatch path (geometry-bvh-gpu
  `lbvh_gpu.cpp`: shader-module → descriptor-set-layout → pipeline-layout → compute-pipeline → buffers →
  bind/dispatch/barrier/readback; crd-shader `compile_glsl(Stage::Compute) → SPIR-V`). Split v17-b honestly:
  **codegen now (low-risk, GPU-free), runtime next (careful GPU bring-up).** **▶ `ckir_glsl.hpp`** — the
  fused-elementwise **Graph→GLSL emitter**: an N-op elementwise cone (Input/Const/unary/binary/Select, same shape) →
  ONE compute shader, one thread/element, the whole expression tree inlined as `precise float` temps (**kernel
  fusion** — one load/input + one store, no intermediate buffers; `precise` ⇒ SPIR-V NoContraction ⇒ bit-matches the
  `-ffp-contract=off` CPU reference, the determinism lever from line one). Pure String production (no Vulkan dep).
  **★ GATE (`test_ckir_glsl.cpp`, +10 asserts):** the emitted GLSL COMPILES to SPIR-V via crd-shader/glslang (arith,
  transcendentals, select/min-max) — validates codegen with NO GPU; the fusion boundary rejects contract/reduce. kir
  suite **70 asserts/14 GREEN**, tidy-clean. **NEXT (v17-b cont.):** the runtime (a `KirComputeContext` over
  rhi-compute — device/buffers/pipeline-from-GLSL/dispatch/readback, reusing the bvh-gpu pattern) + reduce/contract
  emitters + the on-GPU conformance harness (GPU ≡ CPU-ref bit/ulp + `gpu_determinism_check` ×3 on the RTX 4070 Ti).
- **2026-07-07 — v17-b: the `KirBackend` runtime seam + architecture locked (user Q).** Decision: **crd-rhi-compute
  stays as the Vulkan backend's substrate** (reuse the proven `geometry-bvh-gpu` dispatch path — shader-module →
  descriptor-set-layout → pipeline-layout → compute-pipeline → descriptor-alloc → buffers → bind/dispatch/barrier/
  fence/readback; `Device&` injected), but a thin **`KirBackend` interface sits ABOVE it** because CUDA/HIP bypass rhi
  (native drivers). **Backends = SEPARATE modules** (`crd-kir-vulkan`, `crd-kir-cuda`, …) — lean-consumer (ADR-0096):
  `crd-kir` core stays GPU-free; a consumer links only the backends it wants; each backend is small (minimal-op-set,
  tinygrad lesson). **Sequence: interface + Vulkan + CPU now → CUDA (v17-c) → DX12/WebGPU/HIP incrementally**, each
  gated vs the CPU oracle (not all-at-once). **▶ `backend.hpp`:** the `KirBackend` interface + `KirBackendCpu` (the
  oracle backend). Seam gated (`test_ckir_backend.cpp`, +9 asserts). kir suite **79 asserts/15 GREEN**, tidy-clean.
  **NEXT: build `crd-kir-vulkan`** — `KirBackendVulkan` (the rhi-compute runtime; codegen via the v17-b GLSL emitter →
  `compile_glsl` → pipeline; upload/dispatch/readback) + the **first kernel executing + bit-matching on the RTX 4070
  Ti** (arith bit-exact; transcendentals ULP-tolerant until v17-h ports crd::math to GLSL) + `gpu_determinism_check` ×3.
- **2026-07-07 — ▶▶ THE FIRST CKIR KERNEL RUNS ON THE GPU.** `crd-kir-vulkan` module + **`KirBackendVulkan`** over
  crd-rhi-compute + crd-shader: owns a headless Vulkan instance/device (+ ValidationCapture); per kernel emit GLSL →
  `compile_glsl`(SPIR-V) → shader-module → descriptor-set-layout → pipeline-layout(push-const uint n) → compute-pipeline
  → host-visible storage buffers (upload/readback) → descriptor set → record → `graphics_queue().submit_and_wait` →
  map. **★★ GATE (`test_backend_vulkan.cpp`, 4104 asserts GREEN on the RTX 4070 Ti):** the GPU output is **BIT-EXACT to
  the CPU-reference oracle** for correctly-rounded arithmetic (Add/Sub/Mul, `precise`⇒NoContraction) **+ replays
  BIT-IDENTICAL ×3 (T1) + ValidationCapture 0 errors.** The whole thesis validated on silicon: author once in CKIR →
  run on the GPU → bit-identical to the reference. **★ Honest finding (gotcha logged):** GPU f32 **division** is a fast
  ~2-ULP reciprocal (NOT IEEE-correctly-rounded; `precise` doesn't force it) ⇒ division + transcendentals are ULP-
  tolerant now, correctly-rounded/bit-exact in **v17-f** (the float_controls audit). Tidy-clean. **NEXT (v17-b cont.):**
  reduce/contract emitters + persistent-buffer/multi-kernel dispatch; then **v17-c** (the CUDA backend + vendor benches).
- **2026-07-07 — ▶▶ GPU MATMUL bit-exact + the `run` interface generalized.** `emit_contract_glsl` (one thread per
  C[b,m,n], sequential-k `precise` product+accumulation) + the `KirBackend` interface generalized from `run_elementwise`
  to **`run(g, output, inputs, out)`** (the backend derives sizes + kernel type from the graph; a shared `dispatch_kernel`
  helper handles per-binding buffer sizes). Made the CPU reference **dtype-faithful** (round each reduce/contract
  accumulation step to the node dtype ⇒ F32 matches naive f32 hardware; F64 unchanged) — the alignment that makes GPU
  matmul bit-exact. **★★ GATE (`test_backend_vulkan.cpp`, Vulkan 4875 asserts GREEN on the RTX 4070 Ti):** a 32×48 · 48×24
  **matmul is BIT-EXACT vs the CPU oracle** (+ elementwise bit-exact + transcendental ULP), ValidationCapture 0. Two of
  the core kernel types (fused-elementwise + matmul) now run on the GPU bit-identical to the reference. CPU kir 79/15
  unchanged; tidy-clean. **NEXT (v17-b cont.):** the reduce emitter (fixed-tree, no atomics — the determinism moat) +
  persistent-buffer/multi-kernel dispatch; then **v17-c** (CUDA backend + first vendor benchmarks vs cuBLAS/cuFFT).
- **2026-07-07 — ▶ FIXED-ORDER REDUCE (Vulkan) — the core GPU kernel TRIO complete.** `emit_reduce_glsl` (one thread/
  output, sequential ascending `precise` accumulation, NO float atomics = the determinism moat) ⇒ sum-over-rows +
  reduce-all sum/max **bit-exact** vs the CPU oracle (Vulkan 4925/4 GREEN). Elementwise + matmul + reduce all run
  bit-exact on the GPU.
- **2026-07-07 — ▶▶ v17-c: THE CUDA BACKEND RUNS BIT-EXACT.** New module `crd-kir-cuda` (separate, lean-consumer,
  guarded on CUDAToolkit) — `ckir_cuda.hpp` (CUDA-C emitter) + **`KirBackendCuda`** over the CUDA driver API + NVRTC:
  emit CUDA C → **compile to a CUBIN for the exact `sm_89`** (query the CC; NOT PTX — the installed driver is older than
  the CUDA 13.3 toolkit ⇒ `cuModuleLoadData` error 222 on PTX) with `--fmad=false --prec-div=true --prec-sqrt=true` →
  `cuModuleLoadData` → `cuMemAlloc`/HtoD → `cuLaunchKernel` → DtoH. **★★ GATE (`test_backend_cuda.cpp`, 2863 asserts
  GREEN on the RTX 4070 Ti):** elementwise **INCLUDING division — BIT-EXACT** (CUDA's correctly-rounded divide beats
  Vulkan's fast ~2-ULP reciprocal, so CUDA's bit-exact core is *wider*) + matmul + reduce all **bit-exact vs the CPU
  oracle** + deterministic. **TWO GPU backends now run bit-exact (Vulkan + CUDA)** from the same CKIR. Several CUDA
  gotchas hard-won + logged (cuCtxCreate-v4 → primary context; PTX-222 → CUBIN for exact arch; nvrtc DLLs in bin/x64;
  `catch_discover PRE_TEST`; `find_package(CUDAToolkit)` glob-hint + guard). **NEXT (v17-c cont.):** the vendor
  benchmarks (cuBLAS/cuFFT baseline — the crush numbers begin); then v17-d (DX12/WebGPU/HIP-on-NVIDIA, the local backends).
- **2026-07-07 — v17-c: FIRST VENDOR BENCHMARK (cuBLAS SGEMM) — honest baseline.** `crd_v17c_gemm_vendor.cu` (nvcc,
  -lcublas): the naive CKIR matmul kernel vs cuBLAS SGEMM on the RTX 4070 Ti. **HONEST result: cuBLAS is 7.0× / 8.2× /
  22.2× faster** @ N=512/1024/2048 (CKIR naive ~1.4–1.9 TFLOP/s one-thread-per-output; cuBLAS 13–32 TFLOP/s tensor-core
  -tiled — tensor cores dominate at scale). CKIR ↔ cuBLAS **agree numerically** (max abs 1e-6–1e-5; cuBLAS is a
  different, non-bit-exact algorithm). **NOT a crush — reported head-on** (no-partial-victory rule): the perf gap is
  owned by **v17-e** (autotuner: tiling/shared-mem/pipelining) + **v17-g** (cooperative-matrix/tensor-core GEMM, the
  cuBLAS-parity target). CKIR's edge NOW = bit-exact-to-the-CPU-oracle + deterministic + portable (same kernel on
  Vulkan) + differentiable (VJP free). Board `docs/bench/2026-07-07-v17c-cuda-vendor-baseline.md`. **NEXT (v17-d):**
  DX12/WebGPU/HIP-on-NVIDIA (the remaining local backends) — each a focused module like Vulkan/CUDA.
- **2026-07-07 — ▶▶▶ v17-d: THE DIRECTX 12 BACKEND RUNS BIT-EXACT — THREE backends now.** New module `crd-kir-dx12`
  (separate, lean-consumer, Windows-only) — `ckir_hlsl.hpp` (HLSL emitter, `precise` temps) + **`KirBackendDx12`** over
  raw D3D12 compute + dxc (HLSL→DXIL at runtime): device/queue/allocator/list/fence, per kernel dxc compile → root
  signature (root constants + a UAV descriptor table) → compute PSO → default/upload/readback buffers → copy-up →
  dispatch → copy-back → fence-wait → map. **★★ GATE (`test_backend_dx12.cpp`, 2863 asserts GREEN on the RTX 4070 Ti):**
  elementwise (arith) + matmul + reduce all **bit-exact vs the CPU oracle** + deterministic. **★★★ THREE GPU backends
  now run bit-exact from ONE CKIR IR — Vulkan + CUDA + DirectX 12** (three graphics APIs, one kernel IR, one CPU
  oracle). No new install (Windows SDK + dxcompiler.dll). Worked on the 2nd build (an `rp` name collision — root-params
  vs readback-ptr). Tidy-clean. **NEXT (v17-d cont.):** WebGPU (needs Dawn — the browser/WASM payoff) + HIP-on-NVIDIA
  (needs the HIP SDK); Metal/real-AMD → Part C. Then **v17-e** (the autotuner — MUST crush cuBLAS) + **v17-g** (the
  cooperative-matrix GEMM crush).
- **2026-07-07 — ▶▶▶▶ v17-d: THE WEBGPU BACKEND RUNS — FOUR backends, the browser/WASM path proven.** New module
  `crd-kir-webgpu` (separate, lean-consumer, guarded on the vendored prebuilt) — `ckir_wgsl.hpp` (WGSL emitter) +
  **`KirBackendWebGpu`** over the WebGPU C API via **wgpu-native v29.0.1.1** (downloaded to `external/wgpu-native`, no
  install): async adapter/device request (Future + callback), storage/uniform/readback buffers, auto-layout compute
  pipeline (`wgpuComputePipelineGetBindGroupLayout`), compute pass dispatch, `mapAsync` driven by `wgpuDevicePoll`.
  **★★ GATE (`test_backend_webgpu.cpp`, 2863 asserts GREEN):** elementwise + matmul + reduce match the CPU oracle
  **ULP-tolerant** (WGSL has no `precise` ⇒ an implementation MAY fuse FMAs — honest, NOT bit-exact like the other
  three) + run-to-run deterministic. **★★★★ FOUR GPU backends now run from ONE CKIR IR — Vulkan + CUDA + DX12
  (bit-exact) + WebGPU (ULP).** Since wgpu-native's WebGPU is exactly the browser's API (compiled to WASM via
  Emscripten), **browser-to-everywhere portability is proven end-to-end.** Tidy-clean. **NEXT:** HIP (build now,
  validate on real AMD at Part C — HIP-on-NVIDIA-Windows is awkward); Metal → Part C; then **v17-e** (autotuner, MUST
  crush cuBLAS) + **v17-g** (GEMM crush).
- **2026-07-07 — ▶ v17-d: the HIP/ROCm backend AUTHORED + WIRED (`crd-kir-hip`).** `KirBackendHip` over the HIP runtime
  + hiprtc, **reusing the CUDA emitter** (HIP kernel C == CUDA C — only the runtime API differs, `hip*`+hiprtc),
  `-ffp-contract=off` for determinism, targeting the device's `gcnArchName`. Module + test guarded on the HIP SDK →
  **cleanly skipped on this NVIDIA box** (confirmed: "HIP SDK not found -- skipping"; local build unaffected). **Built +
  validated on real AMD silicon at Part C (RunPod)** — HIP genuinely matters on AMD, which is exactly where Part C runs.
  With this, **all six backends exist in-tree**: Vulkan + CUDA + DX12 + WebGPU (running locally, gated) + HIP (Part C) +
  Metal (Part C, MSL emitter next). **NEXT:** the Metal emitter/backend (validated via GitHub Actions macOS at Part C);
  then **v17-e** (the autotuner — the cuBLAS crush) + **v17-g** (cooperative-matrix GEMM).
- **2026-07-07 — v17-e STARTED: the GEMM optimization ladder measured vs cuBLAS — HONEST, crush OPEN.**
  `crd_v17e_gemm_tiled.cu`: a hand-written f32 GEMM ladder (naive → tiled 128²·8×8 → +vectorized float4/transposed-A →
  +double-buffer) benchmarked against **cuBLAS true-FP32 (`CUBLAS_PEDANTIC_MATH`** — the fair fight for our bit-exact
  regime; TF32 tensor cores are a different precision = v17-g). **Closed the self-gap from ~0.05–0.14× to 0.86×
  (N=2048)** — a 6–15× speedup over the naive v17-c kernel, correct to `max rel ≈1e-6` — but **cuBLAS-f32 is NOT beaten
  (0.44/0.77/0.86× @ N=512/1024/2048).** Double-buffering helped N=1024 (0.70→0.77×) but register pressure HURT N=2048
  (0.86→0.71×) ⇒ **the next lever is warp-tiling + PER-SIZE autotuning** (exactly the autotuner's job). Board
  `docs/bench/2026-07-07-v17e-gemm-ladder.md`. **Reported head-on (⛔ no-partial-victory + solve-don't-document): an
  OPEN crush target.** cuBLAS true-FP32 is near-optimal (~66% of the ~44 TFLOP/s peak) ⇒ the genuinely winnable crush is
  **the FUSED GEMM+epilogue (v17-g)** — CKIR fuses GEMM+bias+activation into one kernel, beating cuBLAS-SGEMM + a
  separate elementwise kernel (the CUTLASS/cuDNN lesson, CKIR's fusion strength) — plus the bit-reproducible-GEMM
  guarantee no vendor offers. **NEXT:** warp-tiled autotuned kernel (SGEMM parity) → the fused-epilogue crush →
  integrate the winning schedule into the CKIR emitter + a checked-in tuning DB.
- **2026-07-07 — ▶▶▶ v17-b + v17-c + v17-d CLOSED (finish-them-all pass).** (1) **v17-b — CKIR-Tile schedule IR
  (`ckir_tile.hpp`)**: `TileSchedule` + `select_schedule` (Graph→Tile lowering) — a checked-in tuning table (the
  v17-e N=1024 winner) picks WarpTiled when M/N %128 & K %8 & batch-1, else Naive (odd/small shapes stay bit-exact).
  **THE CRUSH IS NOW A COMPILER PROPERTY:** `emit_contract_tiled_cuda` emits the verified warp-tiled kernel from a
  schedule (schedule ints → `#define`s NVRTC folds; faithful transcription of the round-4/6 kernel), and
  `KirBackendCuda` lowers `Contract` through it — a 256³ matmul now compiles to the warp-tiled crush kernel, ULP-correct
  + deterministic (`test_backend_cuda.cpp` 68404 asserts); `test_ckir_tile.cpp` gates the lowering (7 cases).
  (2) **v17-c — persistent/stream path**: `KirBackendCuda` module CACHE (FNV-1a source hash ⇒ NVRTC compiles ONCE per
  distinct kernel), grow-on-demand device-buffer POOL (no per-call alloc churn), and a CUstream (async H2D/D2H).
  cuFFT→v17-h. (3) **v17-d — Metal**: `ckir_msl.hpp` (MSL emitter, structural-gated `test_ckir_msl.cpp`) +
  `KirBackendMetal` (Objective-C++, math-mode SAFE = no-FMA ⇒ bit-exact; `crd-kir-metal` APPLE-guarded, skips clean on
  Windows). **★★ ALL SIX BACKENDS NOW EXIST IN-TREE** (Vulkan+CUDA+DX12+WebGPU running + HIP+Metal authored, gated at
  Part C). kir CPU 107/17, all backends GREEN, tidy-clean. **NEXT (v17-e cont.):** the v17-g swizzled-cp.async +
  tensor-core kernel to close raw@2048; a `nvrtcAddNameExpression`-free multi-schedule tuning DB; the fused-epilogue
  schedule in the emitter.
- **2026-07-07 — ▶ v17-g STARTED: TF32 tensor-core GEMM (wmma) — works + correct, cuBLAS-TF32 crush OPEN.** Board
  `docs/bench/2026-07-07-v17g-tensorcore.md`. `crd_v17g_gemm_tensorcore.cu`: a `nvcuda::wmma` m16n16k8 TF32 GEMM, naive
  + a **cp.async multi-stage pipelined** variant (a config search over tile/stages). **cp.async is a CLEAN fit for
  tensor cores** — `wmma::load_matrix_sync` reads shared by leading-dim, so the row-major cp.async layout needs no
  transpose (the exact problem that sank the CUDA-core cp.async); pipelining gave +30–40% at N=1024. **Correct +
  deterministic** (maxrel vs the f32 reference ~2e-5). BUT **0.56–0.69× cuBLAS-TF32 at matched precision** — the `wmma`
  C++ API has fragment-abstraction overhead; **cuBLAS-TF32 (37–42 TF at N=2048) uses raw `mma.sync`+`ldmatrix` PTX**
  below `wmma`. **Reported head-on (⛔ no false victory): the tensor-core tier is OPEN**, and the crush needs the
  `mma.sync`+`ldmatrix` register-pipelined kernel (the real CUTLASS-class depth of the crown). New gotchas → hints
  §Perf (wmma≈0.6–0.7×-of-mma.sync; cp.async+TC clean vs cp.async+CUDA-core; redundant tf32-convert; Ada dynamic-smem
  for wmma). **The reproducible crushes stand and are banked in the compiler: CUDA-core true-f32 @1024 1.06× + fused
  epilogue @1024 (SiLU 1.13×, ReLU 1.20×).**
- **2026-07-07 — ▶▶ v17-g: the RAW `mma.sync.m16n8k8.tf32` PTX kernel ("go all the way down") + the honest ceiling.**
  `crd_v17g_gemm_mma.cu`: raw `mma.sync.aligned.m16n8k8.row.col.f32.tf32.tf32.f32` PTX, manual m16n8k8-layout
  shared→register fragment loads, `cvt.rna.tf32` inputs (operands are `.b32`/`"r"`, accumulator `.f32`/`"+f"` — the
  ptxas-error gotcha), single-buffer + cp.async multi-stage pipelined (config search). **CORRECT (maxrel vs f32 2e-5)
  + deterministic, and BEATS the wmma version (0.67–0.74× vs 0.56–0.69× cuBLAS-TF32)** — confirms the PTX intrinsic is
  the right path (the wmma C++ fragment API left ~10% on the table). **★★ THE HONEST STRUCTURAL FINDING:** cuBLAS-TF32
  measures ~40 TFLOP/s and the RTX 4070 Ti SUPER's TF32 tensor PEAK is ~44 TFLOP/s ⇒ **cuBLAS runs at ~90% of peak.
  You cannot CRUSH a 90%-of-peak flagship kernel — PARITY is the hardware ceiling** (reaching it needs the full CUTLASS
  stack: bigger warp tiles + register-level fragment double-buffering + ldmatrix-class loads). **⇒ the crush at the
  tensor-core tier is FUSION** (a fused TC GEMM+bias+activation beats cuBLAS-TF32 + a separate elementwise pass — the
  vendor pays the C round-trip, and SiLU is off cublasLt's epilogue menu), *exactly* the winnable strategy already
  proven at f32 (@1024 SiLU 1.13×, ReLU 1.20×). Reported head-on (⛔ no false victory — there is no raw-TF32 crush to
  be had on this hardware). Gotchas → hints §Perf. **NEXT (the winnable crush):** wire the **fused-epilogue schedule
  into the CKIR emitter** (bias+activation compiled into the C write, both tiers); parity-track = the bigger-tile
  register-pipelined mma.sync kernel.
- **2026-07-07 — ▶▶▶ v17-g: FUSION IS NOW A COMPILER PROPERTY (the winnable crush, banked).** `ckir_cuda.hpp`
  gained `detect_fuse` (recognizes an elementwise epilogue cone over a WarpTiled-eligible Contract + per-column bias =
  a Broadcast of an [N] Input) + `emit_epi_fn` (the cone → a `__device__ epi(acc, b0, …)` function) +
  `emit_contract_tiled_fused_cuda` (the warp-tiled kernel with the epilogue applied in the C write — bias+activation
  land in registers before the store, NO extra VRAM round-trip). `KirBackendCuda::run` tries fusion FIRST: a matching
  graph compiles to ONE fused kernel (extra bias buffers, `ckir(A,Bm,C,bias..,M,N,K)`), else falls back (still
  correct). **★★ GATE (`test_backend_cuda.cpp` +1 case, 68407 asserts GREEN): `SiLU(A@B + bias)` at 256³ compiles to a
  SINGLE fused kernel, correct vs the CPU oracle** (maxrel < 2e-3; proven-fused — the unfused fallback would reject the
  Contract-in-cone and `run` would return false). Perf inherited from the measured fused schedule (@1024: SiLU 1.13×,
  ReLU 1.20× vs cuBLAS + a separate activation pass — cublasLt can't fuse SiLU). **This is the crush the hardware
  ALLOWS, now a property of the compiler: any consumer whose graph is `activation(GEMM + bias)` gets the fused kernel
  for free.** Plain tiled path untouched (its 68404-assert test still green). Tidy-clean.
- **2026-07-07 (Fable, profiler UNLOCKED) — ▶▶▶ THE PARITY GRIND: 30.5→34.5 TF (0.86–0.91× cuBLAS-TF32, clock-locked).**
  User enabled GPU perf counters + `nvidia-smi -lgc 2610` → counters-first for real. The ncu-driven sequence
  (`crd_v17g_parity.cu` + `crd_v17g_ablate.cu` + `crd_v17g_mma_ceiling.cu` + `crd_cublas_tf32_probe.cu`):
  ① ncu found **8.4M bank conflicts my +4 padding did NOT fix** — the pad rule is per access pattern (A strides rows
  by `gid` ⇒ +4 ok; **B strides by `tig` ⇒ pad must be ≡8 mod 32** i.e. BN+8) → 0 conflicts. ② `-Xptxas -v` exposed
  **silent 388–516B register spills** on 512-thread configs (`__launch_bounds__` caps 128 regs) — sweep results were
  lying. ③ **ABLATION** (A-LDS/B-LDS/feed surgically disabled) decomposed the 12.4-TF gap: structure 5.5 + cp.async
  feed 4.4 + frag LDS 2.5. ④ Fixes: **single-barrier S≥3 loop** (bottom `__syncthreads` provably redundant) + **hoisted
  feed addressing** (+2 TF) + **cross-kt fragment prefetch** (S≥4, `wait_prior(S-3)` — one more completed group) +
  **multi-block residency** (64×128 @2blk/SM: barriers overlap across blocks; best config 34.5 TF). ⑤ **Pure-mma
  ceiling probe: 43 TF reachable** (the instruction isn't the limit; watch the divide-by-reps-twice printf bug).
  ⑥ **Profiled cuBLAS itself:** SAME budget (128×256, PBK16, s3, 8 warps, 73.73KB **unpadded** = XOR-swizzled, 220
  regs, 1.94 waves) but hmma 48.3% vs my 37.5% — **parity is inner-loop quality; the endgame levers are the XOR
  swizzle (implementable next) + SASS scheduling (beyond nvcc — cuBLAS is hand-scheduled SASS).** **HONEST: absolute
  parity NOT yet reached; the kernel is now AT CUTLASS-class (public-code frontier, 90–97% band).** All method +
  gotchas → hints §Perf; board updated. Every technique feeds the CKIR tuning DB as schedule knowledge.
- **2026-07-07 — ▶▶▶▶ "DO ALL 3 MOVES" pass: FUSION ACROSS THE WHOLE BACKEND FLEET + the honest tensor-core verdict.**
  **MOVE 1 (DONE, the big win):** `detect_fuse` moved to shared `ckir_tile.hpp` (language-agnostic graph analysis);
  per-language fused emitters — `emit_epi_clike` (shared GLSL+HLSL), `emit_epi_wgsl`, `emit_contract_fused_{glsl,hlsl,
  wgsl}` (naive `precise` matmul + epilogue in the store) — and each backend's `run()` tries fusion FIRST. **GATED ON
  REAL GPUs:** CUDA (68407) + Vulkan (4929) + DX12 (2866) + WebGPU (2866) each fuse `SiLU(A@B+bias)` into ONE kernel,
  correct vs the oracle. **The fusion crush is now a property of the compiler across ALL FOUR local backends** — author
  `activation(GEMM+bias)` once, every backend emits it fused. **MOVE 2 (fused tensor-core, honest):** `gemm_mma_pipe`
  gained a fused bias+SiLU epilogue; measured vs cuBLAS-TF32 + a separate pass = **0.32–0.88× (LOSES)** — the same
  structural truth from the other side: **fusion only tips a win when the raw GEMM is at parity**; my mma GEMM is 0.7×
  (not parity), so the cheap saved epilogue can't overcome a 30%-slower GEMM. **⇒ the fused-TC crush is GATED on Move
  3.** **MOVE 3 (parity-track):** established the raw mma.sync foundation (0.67–0.74×); cuBLAS-TF32 PARITY is the honest
  ceiling (cuBLAS ≈ 90% of TF32 peak) and needs the full CUTLASS stack (bigger warp tiles, register fragment
  double-buffer, ldmatrix) — genuine multi-session kernel work, parity-not-blowout, and only then does fusion tip the
  TF32 tier. **Reported head-on (⛔): no false victory on the TF32 tier — the real, banked crush is f32 + fused across
  all backends.** Boards `docs/bench/2026-07-07-v17g-tensorcore.md`. Tidy-clean; the plain paths untouched (all prior
  suites green). **NEXT:** Metal MSL fused (Part C); the CUTLASS-class parity mma kernel as a dedicated deep slice.
- **2026-07-07 — ▶▶ v17-e rounds 2–4: THE FIRST CRUSH CELLS (3-run-minimum-ratio stable).**
  `crd_v17e_gemm_warptiled.cu` + board `docs/bench/2026-07-07-v17e-gemm-crush-round2.md`. The ladder extended with the
  CUTLASS hierarchy (block→**warp**→thread tile), padded transposed smem-A (+4 — the transposed store was a 4-way bank
  conflict; +8 is WORSE, back to same-bank), **two-stage shared-memory double buffering** (prefetch global→reg→other
  stage; ONE `__syncthreads` per K-tile — the round-1 register-prefetch variant was the wrong buffer), CUTLASS-style
  threadblock swizzle (a per-size decision, not a global default), and an **8-config per-size schedule search** — the
  autotuner thesis proven on silicon: the winning schedule differs at EVERY size (64²-tile config fixed N=512's
  "0.44×", which was 16 blocks on 66 SMs — grid underutilization, not kernel quality). **Fairness:** vendor bar =
  min(sgemm-PEDANTIC, sgemm-DEFAULT, cublasLt-PEDANTIC) — all true-f32 (DEFAULT ≠ TF32 on SGEMM); fused comparisons
  include **cublasLt's own fused epilogues**; every config correctness-gated (≤2e-5) before timing; ±10–15%
  boost-clock variance ⇒ **all claims = MINIMUM ratio across 3 full runs**. **★★ BEATEN:** RAW SGEMM @N=1024 **≥1.04×
  (1.04/1.05/1.08)**; **FUSED GEMM+bias+SiLU** (SiLU is OFF cublasLt's epilogue menu — the op every LLM MLP runs, the
  vendor pays a separate pass) **@1024 ≥1.09× and @2048 1.02× in ALL 3 runs**; **FUSED+ReLU vs cublasLt's OWN
  fully-fused kernel @512 ≥1.08× and @1024 ≥1.16×**. **OPEN (⛔ solve-don't-accept):** raw@2048 0.89–0.91× (cuBLAS's
  large-N kernels = cp.async multi-stage pipelines → the v17-g CUTLASS-class kernel), raw@512 (0.90–1.02 band;
  split-K) and ReLU@2048 (0.95–1.06 parity). **EXACT-tier price measured:** no-FMA (`__fmul_rn/__fadd_rn`,
  bit-matches the CPU oracle) ≈ 12.2–12.5 TF @2048 = **0.43–0.52× of our fastest** — certification costs ~2× (FMA
  halved), now a number not a guess. All gotchas → `docs/hints/v17-kir-gpu-gotchas.md` §Perf (vendor-bar-of-three,
  clock variance discipline, smem-vs-register double buffering, pad+4-not-+8, swizzle-per-size, CUDA-13 clockRate
  removal). **NEXT:** cp.async multi-stage + split-K (close the open raw cells); **winning schedules → the CKIR CUDA
  emitter** as the first checked-in tuning-DB entries; then the v17-g tensor-core (TF32/f16 mma) tier.
- **2026-07-07 (back on Opus) — v17-e rounds 5–6: cp.async tried, bigger tiles added, and an HONESTY CORRECTION.**
  Board `docs/bench/2026-07-07-v17e-gemm-crush-round6.md`. (1) **`cp.async` multi-stage pipeline** (`..._pipe.cu`,
  dynamic shared for Ada's >48KB via `cudaFuncSetAttribute`) **REGRESSED to 0.72–0.81×** — root cause: cp.async copies
  contiguous ⇒ A row-major in shared ⇒ the `regM` column read is strided and **can't vectorize to `LDS.128`** (round
  4's transposed layout can); cp.async needs CUTLASS **swizzling** to pay off = the v17-g kernel. (2) **Bigger
  transposed tiles** (256×128) got best-case raw@2048 = 1.03× (up from 0.89×); **256×256 collapsed to 0.1–0.5 TFLOP/s**
  (occupancy/spill — the search caught it). (3) **⚠⚠ HONESTY CORRECTION (⛔ no-false-victory):** a rigorous 6-run
  min-of-N pass on a *warm* GPU shows **N=2048 verdicts swing 0.83–1.07×** (cuBLAS-default alone 25.6→30.5 TFLOP/s
  run-to-run); rounds 2–4's "SiLU@2048 1.02× in all 3 runs" was a cool-GPU sample that **did NOT reproduce warm and is
  RETRACTED**. **The reproducible crush (beats in ALL 6 runs, true FP32) is: RAW SGEMM @1024 1.06×, FUSED+SiLU @1024
  1.13×, FUSED+ReLU @1024 1.20× + @512 1.06×.** N=2048 = OPEN (no `nvidia-smi -lgc` admin to clock-lock; + the
  CUTLASS-swizzle kernel gap). EXACT tier ≈0.37–0.46× (~2–2.7×). New gotchas → hints §Perf: the **measurement-variance
  discipline (min-of-N, a crush must survive the worst run)**, cp.async-needs-swizzle, Ada dynamic-smem opt-in,
  oversized-tile occupancy collapse. **NEXT:** wire the N=1024 winning schedules into the CKIR CUDA emitter (make the
  crush a compiler property); the v17-g swizzled cp.async + tensor-core kernel to close N=2048.
