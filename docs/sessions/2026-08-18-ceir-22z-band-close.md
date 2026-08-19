# CEIR-22 band close — numerical GPU workflows through CEIR (the §137 GEMM→FFT→reduction→viz-prep proof)

**Date:** 2026-08-18 · **Mold:** the 20z band-close discipline (advisor at band-close → fresh family×config sweep →
row-per-claim / row-per-config tables → deferral ledger → tracker BAND row).

## What the band proves

CEIR-22 routes **numerical GPU workflows through CEIR** (§52/§59/§137). The crown proof (§137): a **single authored
`.ceir` asset** — `assets/ceir/tensor_pipeline.ceir` — that MIXES high-level tensor ops (`linalg.gemm`, `tensor.fft`,
`tensor.reduce`) with **two authored CKIR compute dispatches** (`compute.dispatch @viz_magnitude` / `@viz_normalize`) is
**parse-loaded**, **planned** (an inspectable device-resident memory plan), and **executed as ONE device-resident submit**
(no CPU round-trip between stages), on **NVIDIA (Vulkan) + D3D12 + Mesa lavapipe**, validated against an INDEPENDENT
composed f64 reference — end-to-end AND per-stage — with a structural **profile** and a positive submit wall-time.

## Row-per-claim

| Slice | Claim | Proof |
|---|---|---|
| **22a** | `ceir.linalg` (gemm) + `ceir.tensor.fft` declared (opgen) + verifiers | `find_linalg_misuse` / `find_tensor_misuse` reject every contract misuse with the exact kind; text+binary roundtrip |
| **22b** | the CEIR→CKIR **native provider** `synth_gemm/synth_reduce/synth_fft` | 6 device gates (Vk+DX12): each synth graph runs bit-exact vs `eval_cpu`/`eval_cpu_kernel` AND an INDEPENDENT ref (triple-loop / serial / naive-DFT); 12 typed `SynthReject` kinds |
| **22c-1** | `plan_tensor_pipeline` — the PURE device-free memory plan | roles/bytes/def-use wiring/reshape-alias + typed rejects + genericity (a re-shaped asset plans differently) |
| **22c-2** | `execute_tensor_pipeline` + the C++-built §137 Vulkan proof | one device-resident submit (graph-tier gemm/reduce + kernel-tier fft into ONE recorder) vs a composed ref |
| **22c-3a** | plan handles `compute.dispatch`→VizDispatch + the Output-rule fix | 5-stage design-B plan; `DispatchOutputsNotTrailing`; ⛔ resultless-dispatch ⇒ Output = the final stage's trailing binds |
| **22c-3b** | the two authored viz `.ckir` kernels + device-free oracle | `ckir_read`→`eval_cpu_kernel` == closed-form magnitude / normalize (rank-0 max read) |
| **22c-3c** | the authored `.ceir` asset + parse-load gate | `parse()`→verify(all walks)→plan(5 stages)→anti-drift vs builder→roundtrip→`collect_dependencies`==[viz_magnitude,viz_normalize] |
| **22c-3d** | the parse-loaded 6-stage Vulkan proof + per-stage oracle + determinism | device-resident vs composed ref; every stage vs its independent f64 ref; uninstrumented terminal == instrumented (bit-exact) |
| **22c-3e** | the DX12 leg | same asset, HLSL emitters + the portable copy-readback contract |
| **22c-3f** | the lavapipe leg | same Vulkan binary on Mesa's software Vulkan (cross-vendor) |
| **22c-3g** | `TensorPipelineProfile` (the §137 "profiling") | 5 coherent per-stage rows (kind/grid/workgroups/bytes) + caller-stamped `wall_ns>0`; NO time threshold (soft-perf) |

## Row-per-config (the fresh close sweep — never inherited)

| Config | CEIR-22 result | Notes |
|---|---|---|
| **win-debug** (MSVC) | **29/29** ✅ | the full family incl. DX12 |
| **win-asan** (MSVC /fsanitize=address) | **27/27** ✅ | no ASan errors; asserts active ⇒ also re-verifies the dialect.cpp fix |
| **win-release** (MSVC /O2 /GL) | **26/26** ✅ + clean `/WX` compile | ⭐ the release compile CAUGHT + fixed a latent bug (see Scars) |
| **lavapipe** (linux-gcc-debug, Mesa software Vulkan) | **25/25** ✅ | the full non-DX12 family; cross-vendor |

⛔ **Ledgered sweep gaps** (config-invariant, covered elsewhere — NOT proof holes): DX12 tests are Linux-skipped by design
(no D3D12). The 2 `crd-kir-tests` viz **evals** (22c-3b) did not enumerate on win-release/win-asan (a Windows
`catch_discover_tests`-on-that-config tooling gap — the exe built; pure-CPU `eval_cpu_kernel`, GREEN on win-debug +
lavapipe).

## Zero-builder audit (the everything-authorable rule)

- The ALGORITHMS are AUTHORED assets: `tensor_viz_{magnitude,normalize}.ckir` + the `tensor_pipeline.ceir` graph.
- `synth_gemm/synth_reduce/synth_fft` = the native **PROVIDER** — a declared-op→CKIR **lowering mechanism** (like a compiler
  backend; it reuses the existing `ckir_*` primitives), NOT a hand-coded algorithm. Established + accepted in 22b.
- `build_pipeline_b` (test) = the deliberately-KEPT **anti-drift oracle** the parse-load gate compares the committed `.ceir`
  against (the builder-vs-loader reclassify rule; documented in the `.ceir` authoring recipe memory). No `delete` step —
  a `.ceir` asset has no cook-time author-then-delete path yet.
- ✅ No disallowed C++ algorithm KGraph builders introduced.

## Deferral ledger (all typed-rejected — never a silent subset)

- **gemm**: plain-contract only (α=1 ∧ β=0 ∧ !transA ∧ !transB) → α/β/trans = `SynthReject::GemmEpilogueUnsupported`.
- **reduce**: {sum,prod,max,min} synth; `mean` (declared in the tensor vocab) → `SynthReject::ReduceFnUnsupported`.
- **fft**: rank-1 pow2 fwd/inv, F32-only → r2c/c2r/2d/batched + non-pow2 + non-F32 = typed rejects; name-forward.
- **compute.dispatch**: `r`/`w` bindings; `rw` in-place → name-forward (VizDispatch counts only `w` as outputs).
- **kernel_interface**: both viz refs UNPINNED (legal per the compute.dispatch TOML) — pin when a cook path consumes the asset.
- **viz grid-coverage**: no bounds guard by design (grid·local_size == numel is an authoring invariant; the per-stage oracle
  catches a violation for THIS asset; a structural check is name-forward).
- **profiling**: per-stage GPU-timestamp queries → name-forward (the structural profile + a caller wall-time is the gold floor).
- **hesap cross-oracle**: NOT added — the composed triple-loop/naive-DFT/closed-form PER-STAGE reference is already
  synthesis-independent (shares zero code with the synth path); a 2nd independent ref is redundant belt-and-braces.
- **12d memory planner**: the plan uses distinct buffers (no interval-coloring aliasing) — a ledgered slice.

## Scars this band (the release/Linux legs earned their keep)

- ⛔ **release/gcc -Werror catch what MSVC-debug hides** (twice): a set-but-unused `fr_ref` (22c-2 test, gcc `-Werror`) and a
  `dialect.cpp` `EffectRecord& e` read only by `CRD_ASSERT_MSG` (C4189 unused-but-set under NDEBUG `/WX`, latent since the
  release config wasn't built recently). Both fixed; the second vindicates the advisor forcing the release config.
- ⛔⛔ **`eval_cpu_kernel` is SCALAR** — a uvec3 `Builtin`+`Swizzle` ASSERTS → a Windows debug-assert HANG (empty-output ctest).
  Use the scalar `LocalInvocationIndex` (== gid.x for one workgroup); store rounds to F32 ⇒ f32-relative tol. (memory written)
- ⛔ **resultless `compute.dispatch` breaks a `last_result` Output rule** — Output = the final stage's trailing `n_out` binds.
- ⛔ **`.ckir` fft radix hang** (from 22b): `build_fft1d_batched` picks radix by log2(n); a fixed radix-2 caller must use
  `build_fft1d_radix2`. (memory written)
- 📌 **the `.ceir` authoring recipe** (parse/print, no comments, bootstrap-via-print, keep the builder as anti-drift oracle) —
  memory written for the next band that authors a `.ceir`.

## Commits (proposed — user commits; NO AI co-author trailer)

- ㉖ `feat(ceir-gpu): CEIR-22c-2 execute_tensor_pipeline + the §137 Vulkan proof gate`
- ㉗ `feat(ceir): CEIR-22c-3 the authored tensor_pipeline.ceir + viz .ckir kernels + the §137 proof (Vulkan/DX12/lavapipe) + profiling; fix a latent release-config /WX unused-var in dialect.cpp`

**CEIR-22 → BAND ✅.** Next: CEIR-23 (advisor at band-open; drive toward CEIR-35).
