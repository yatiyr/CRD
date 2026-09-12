# 2026-09-04 — CEIR-25 `ceir.autodiff` correctness scoreboard (reverse-mode AD as a compiler transform)

<!-- doc-role: evidence -->
> Dated evidence; counts, results and Next paragraphs are historical. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

CEIR-25 makes **reverse-mode differentiation a compiler transform** over the high-level tensor vocab (`build_gradient`
differentiates `linalg.gemm` / `tensor.reduce(sum)` / `ml.mlp` BEFORE expansion, emitting backward ops in the existing op
vocab). This is a **correctness** band, not a perf band — the meaningful measurement is whether the transform's emitted
backward gradient matches a **dialect-independent reference**, and whether the whole backward pass runs **device-resident**
on every backend. So this board scores each gate by *what reference it matched* and *on which backend*, not by wall-clock.

> The perf/crush home for the AD *math* is the v15 (forward) / v16 (reverse) hesap-autodiff boards — those already crushed
> torch/JAX. Here the **hesap reverse-mode AD is the analytic reference** the CEIR *compiler transform* must reproduce; a
> match means the transform is correct, independently of the (separately-benchmarked) speed of the math it lowers to.

## Config

- **Host:** i9-14900K (builds host-capped per the 14900K doctrine). **win-debug** functional build (MSVC), and **WSL2
  linux-gcc-debug** (gcc 13.3, `-Werror`) for the cross-compiler leg.
- **Devices:** Vulkan (`VK_EXT_shader_object`) + DX12 (FL12) on **RTX 4070 Ti SUPER** (REAL devices); **WSL2 llvmpipe**
  (software Vulkan — the CI/portability exposure that proves a gate RAN, not skipped).
- **Harness (re-runnable):**
  - device-free — `crd-ceir-gpu-tests` (`[autodiff]`, `[tensor-pipeline]`, `[ckir-synth]`), both compilers.
  - authored-asset — `crd-kir-tests` (`[asset]`, the `relu_vjp.ckir` reading gate).
  - device — `crd-ceir-gpu-vulkan-tests` + `crd-ceir-gpu-dx12-tests` (Windows real; WSL llvmpipe for the Vulkan TU).
  - build: `scripts/build-target.bat <build-dir> <target>`; run: `scripts/run-ctest.bat <dir> <name-regex>`; WSL via
    `wsl bash <script>` (preset `linux-gcc-debug`). Every gate is also `clang-tidy` LLVM-20 clean (warnings=errors).
- **References (every gate scored against a dialect-independent oracle):**
  - **hesap** `crd::hesap::autodiff::reverse::nn::matmul_vjp` / `relu_vjp` — analytic f64, a **SEPARATE codebase** from CEIR
    (a shared row-major indexing bug cannot pass both — the dialect-independence).
  - **`gradient_check::grad_fd`** — central-difference FD witness on the C++ forward (a third independent oracle; invalid at
    relu kinks, so device corpora are kept off-kink).
  - **`eval_cpu_kernel`** — the CKIR scalar oracle (for the authored `relu_vjp.ckir`).

## Board — every CEIR-25 gate × backend × reference (assertions as-of each slice's close, cited to D-007)

Two evidence forms appear: **gate** = the gate's OWN run (by-name ctest cases / the device ctest #IDs / the gate's own
assertion count) — this is what scores the gate; **·rode** = the larger regression bucket the gate ran inside without breaking
it (a tag total or the full suite), evidence of no-collateral-damage, NOT the gate's own strength.

| Slice | Gate | Backend(s) | Evidence — gate (·rode: regression) | Reference matched | What it discriminates (incl. the negative gate) |
|---|---|---|---|---|---|
| **25a-1** | gemm VJP registry + `vjp_gemm` structural | device-free (MSVC + gcc) | win-debug 45/45 by-name; gcc 13.3 | structural: `dA=gemm(dC,Bᵀ)`, `dB=gemm(Aᵀ,dC)` op+shape | the transpose + 2-gemm emission; NEG: `lookup→nullptr` (`MissingVjp`) for an unregistered op / a dispatch w/o registered kernel / w/o kernel attr |
| **25a-2** | `vjp_reduce(sum)` + `build_gradient` orchestrator | device-free (MSVC + gcc) | 5 cases / 89 assertions | STRUCTURAL + PARTIAL numeric (`eval_cpu`) | seed→reshape→broadcast→2 transpose→2 gemm→elementwise{add}; NEG: an on-path `tensor.reduce` with NO rule → `MissingVjp`, PURE (op count UNCHANGED) |
| **25b-1** | `ckir_synth` transpose/broadcast/elementwise | device-free | 3/3 by-name (·rode `22b\|25a\|25b` 8/8) | synth-mold structure + 4 typed rejects | the graph-tier synth wrappers; NEG: 4 synth-reject constructs (bad perm / shape) |
| **25b-2a** | `synth_elementwise` on a REAL device | Vulkan + DX12 (+ llvmpipe) | Vk #4714 · DX12 #4790 by-name (·rode llvmpipe `[ckir-synth]` 2149/4) | GPU == independent index-map ref | the EXISTING fused-elementwise emitter consumes synth output |
| **25b-2b** | broadcast + permute root emitters | Vulkan + DX12 (+ llvmpipe) | Vk #4715/#4716 · DX12 #4793/#4794 by-name (·rode llvmpipe 2272/6) | GPU == index-map ref | NEW `emit_broadcast_nd`/`emit_permute` both backends |
| **25b-3** | StageKind widen `{Transpose,Broadcast,Elementwise}` plan | device-free (gcc `-Werror`) | 2/2 by-name (·rode tensor-pipeline 24/24; gcc 340/28) | plan routing + TYPED reject | the 3 NEW planner stages route the backward vocab; NEG: verifier-legal broadcast-compat `[3,2]+[3,1]` elementwise → `SynthRejected` (the same-shape envelope) |
| **25b-4a** | resolvers + multi-stage device | Vulkan + DX12 (+ llvmpipe) | Vk #4729 · DX12 #4804 (2/2 by-name) (·rode llvmpipe `[ckir-synth]`+`[tensor-pipeline]` 2951/12) | GPU == `e[r,c]=in0[c*3+r]+in1[r]` (transpose+broadcast+add, ONE submit) | resolver wiring + plan def-use REACHED the executor |
| **25b-4b·1** | WHOLE `sum(gemm(A,A))` backward plans (device-free) | device-free (gcc `-Werror`) | test #821 by-name (·rode `[autodiff]`+`[tensor-pipeline]` 344/25) | 8-stage plan, order pinned; single `Output` = `grads[0]` | the entire backward is ONE device-resident pipeline |
| **25b-4b·2** | that backward runs device-numeric | Vulkan + DX12 (+ llvmpipe) | Vk #4731 · DX12 #4807 by-name; gate 24 assertions/1 case (·rode llvmpipe `[tensor-pipeline]` 703/7) | **hesap `matmul_vjp`** + `grad_fd` — tol **1e-4 rel** (3-elt f32 dot, \|ref\|~O(6)) | reverse-pass TOPOLOGY on device (a dropped transpose IS caught) |
| **25c-0** | `relu_vjp.ckir` authored + permanent reading gate | device-free asset (MSVC + gcc) | 36 assertions (·rode `[asset]` 542/29) | **hesap `relu_vjp`** via `eval_cpu_kernel` — EXACT over 32 neg/zero/pos threads | authored `Select(CmpGt(x,0),gy,0)` exact at `x==0` (strict `>0`) |
| **25c-1** | `vjp_mlp` composite rule structural + 2 typed rejects | device-free (gcc `-Werror`) | `[autodiff]` 192/9 (·rode full device-free 1157/87) | structural (op counts, `dW` shapes) + IDENTITY (`@relu_vjp` reads `z1`, not `h1`) | the composite-before-expansion reframe; NEG: `tanh` activation → `MlpActivationUnsupported`; `x[8,8]·W1[8,8]` (M·hidden=64≠32) → `MlpBakedShapeUnsupported` — both assert ZERO backward vocab emitted |
| **25c-1b-1** | `relu_vjp` on device + readonly-input codegen | Vulkan + DX12 (+ llvmpipe) | Vk #4734 · DX12 #4811 · llvmpipe #4565 by-name (·rode device regression 817/817) | **hesap `relu_vjp`**; GLSL `readonly buffer` / HLSL `RWByteAddressBuffer` asserted BOTH ways | the FIRST authored kernel with readonly inputs — codegen path proven TAKEN, not silently dropped |
| **25c-1b-2a** | write-through-declare plan + two-output + forward-liveness | device-free (gcc) | `[tensor-pipeline]` 213/18; gcc `[autodiff]` 223/10 (·rode full device-free 1192/88) | plan roles (`Intermediate`/`Output`) + submit-order edge | the non-SSA `dispatch→declare` edge; NEG: an UNERASED dead forward composite → plan `UnsupportedOp` (`reject_op==mlp`) |
| **25c-2** | WHOLE MLP backward device-numeric (dW1 + dW2) | Vulkan + DX12 (+ llvmpipe) | **Vk #4736 · DX12 #4814 · llvmpipe #4567** by-name (·rode 258/258 Vk + 223+8 DX12 regression) | **hesap `matmul_vjp`+`relu_vjp` composed** + `grad_fd` `MlpLoss` — nonzero blocks **1e-4 rel** (4-elt f32 dot chain thru relu); zero blocks **EXACT (==0)** | the numeric **CROWN**: non-uniform seed `M` + zero-block proof (half-dead relu ⇒ `dW1[:,c≥2]==0` ∧ `dW2[c≥2,:]==0` exact, c<2 blocks \|·\|>0.05) |
| **25c-3** | checkpointing | — (no new gate) | — | recompute **IS** full gradient checkpointing — proven end-to-end by 25c-2 | (ledger: the store-hybrid has no consumer this band) |

## Verdict

- **Every CEIR-25 gate is GREEN on every backend it targets — NO losses.** A failing correctness gate would be an open bug
  (SANITY #6/#9); none exists. There is no external "peer" to crush here (this is a compiler-transform *correctness* band,
  scored vs oracles) — the external-peer column is **N/A, with the check**: the AD *math* peers (torch/JAX) live on the
  v15/v16 hesap boards, and hesap is the reference this transform is scored against.
- **The device numeric crown is complete.** The whole MLP backward (`x[8,4]·W1[4,4]·relu·W2[4,4]`, backward → dW1+dW2) runs
  **device-resident** — one submit, intermediates GPU-only — on **Vulkan + DX12 + llvmpipe**, matching a dialect-independent
  hesap reference (itself FD-cross-validated before it judges the device). The `sum(gemm(A,A))` backward (25b-4b·2) is the
  same crown for the plain tensor vocab.
- **Dialect-independence is real:** hesap `nn_reverse` is a separate codebase from CEIR; `grad_fd` (central difference) is a
  third, orthogonal witness. A shared indexing mistake cannot pass all three.
- **Cross-compiler + cross-device:** every device-free gate runs under BOTH MSVC (win-debug) and gcc (`-Werror`, WSL); every
  device gate runs on real hardware AND llvmpipe (proving the gate RAN, not skipped). All gates `clang-tidy` LLVM-20 clean.
- **This board is NOT exhaustive over `GradError`.** Any-rank loss is exercised only *implicitly* (25b-4b loss=`[3]`, 25c
  loss=`[8,4]`, both differentiate) — there is no dedicated gate because `LossNotScalar` is a **dead enum member** (never
  returned; the "scalar loss" contract was never enforced and is wrong as shipped — established 25z-1). No gate ⇒ no row.

> **Provenance:** assertion counts are recorded **as-of each slice's close** (source=scoreboard, cited to the D-007-ceir-tracker
> rows written at each measurement time — the README-sanctioned consolidation form). The **25z-5** band-close suite run
> re-measures the whole band in one pass and reconciles any drift in this board **in place** (below).

## Band-close reconciliation (25z-5, fresh single-pass re-measure)

The per-exe re-run at band close (each leg recorded as it lands; ⛔ this is also the FIRST recompile of the grad.hpp header
edits from 25z-1 — a comment-only sweep, so a clean build here confirms no syntax damage):

| Leg | Exe / config | Result | Notes |
|---|---|---|---|
| device-free (MSVC) | `crd-ceir-gpu-tests`, win-debug | ✅ **1192 assertions / 88 cases**, exit 0 | matches the last-recorded 1192/88 (no drift); `grad.cpp` recompiled clean (BUILD_EXIT=0), all CEIR-25 device-free gates #802–838 green |
| device-free (gcc) | `crd-ceir-gpu-tests`, WSL `linux-gcc-debug` `-Werror` | ✅ **1192 assertions / 88 cases**, exit 0 | gcc 13.3.0; `grad.cpp` recompiled clean cross-compiler — identical count to MSVC (no drift) |
| asset | `crd-kir-tests` (`relu_vjp.ckir` reading gate #4077) | _pending_ | |
| device (Vulkan) | `crd-ceir-gpu-vulkan-tests`, real + llvmpipe | ✅ **7/7 real** (#4720–4736) + **7/7 llvmpipe** (#4551–4567), exit 0 | ctest auto-applied the run-env (assets+shaderc); tests loaded `relu_vjp.ckir` + compiled GLSL fine |
| device (DX12) | `crd-ceir-gpu-dx12-tests`, real | ✅ **4/4 (D3D12 #4802–4811) + 3/3 (DX12 #4812–4814)**, exit 0 | two single-term ctest calls (name split D3D12/DX12, no cmd-pipe); #4813 exercises the HLSL readonly→UAV drop |
| asset (reading gate) | `crd-kir-tests` (`relu_vjp.ckir`) | ✅ **1/1 win (#4077) + 1/1 gcc (#4059)**, exit 0 | the committed `.ckir` reads + `eval_cpu_kernel` == analytic ReLU-VJP, cross-compiler |

**Reconciliation verdict (25z-5):** all six legs GREEN, zero failures, zero drift from the per-slice closes. Every CEIR-25 gate
passes on every backend/compiler it targets: device-free (MSVC + gcc `-Werror`) 1192/88, Vulkan (real + llvmpipe) 7/7, DX12 (real)
7/7, asset reading gate (win + gcc) 1/1. The grad.hpp header sweep from 25z-1 recompiled clean everywhere. The board above is
confirmed against a fresh single-pass re-measure — source=scoreboard, not memory.
