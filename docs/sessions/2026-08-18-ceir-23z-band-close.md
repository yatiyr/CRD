# CEIR-23 band close — ML/scientific numerics through CEIR (quantization + sparse, §53/§54)

**Date:** 2026-08-18 · **Mold:** the 20z/22z band-close discipline (advisor at band-close → fresh family×config sweep →
row-per-claim / row-per-config tables → zero-builder audit → deferral ledger → tracker BAND row).

## What the band proves

CEIR-23 adds **quantization** (§54) and **sparse linear algebra** (§53) as first-class CEIR — declared dialects with
verifiers, authored `.ckir`/`.ceir` assets, and cross-vendor device proofs. Two crowns:

- **Quantized MLP** (§54): a 2-layer uniform-width MLP as the parse-loaded `assets/ceir/quant_mlp.ceir` — every weight Q8
  (int8), each `quant.dequantize`→`linalg.gemm` pair **FUSED** into a `StageKind::QuantGemm` (a PLAN 2→1 collapse; the
  dequantized weight is never materialized), `relu` between layers — runs device-resident on **NVIDIA (Vulkan) + D3D12 +
  lavapipe**, matching a float MLP oracle BIT-EXACT.
- **CSR SpMV** (§53): `assets/ckir/spmv_csr.ckir` — `y[i] = Σ_{k∈[row_ptr[i],row_ptr[i+1])} values[k]·x[col_idx[k]]` — the
  FIRST CKIR asset with **data-dependent control flow** (runtime `For` + per-thread `ForBreakIf` + the `col_idx`
  indirection), proven on Vulkan + DX12 + lavapipe vs a CPU CSR SpMV ref.

## Row-per-claim

| Slice | Claim | Proof |
|---|---|---|
| **23a** | `ceir.quant` (quantize/dequantize) declared (opgen) + `find_quant_misuse` | 9 kinds; scale/zp rank-0 per-tensor or rank-1 per-axis OPERANDS; value(float)/storage(int) element roles; scheme vocab; no new TypeKind |
| **23b-1** | the unfused Q8 dequant `.ckir` (u32-pack + FLOAT sign-extend) | `ckir_read`→`eval` == `(int8−zp)·scale`; Vk+DX12 device gates (the HLSL/GLSL emitter-agreement check) |
| **23b-2** | the **fused** `dequantize→gemm` window (`StageKind::QuantGemm`, plan 2→1 collapse) | ONE shared `fusable_dequant_into_gemm_weight` (single-use ∧ weight-slot ∧ symmetric ∧ rank-0 ∧ α=1,β=0,no-trans) + `UnsupportedQuantScheme` typed reject; 6 plan tests; the 155-node `quant_gemm_q8.ckir` (eval oracle); Vk+DX12 gates (fused == unfused Dequant+Gemm == float oracle) |
| **23c** | the **quantized-MLP crown** (parse-loaded `quant_mlp.ceir`) | device-free parse/verify(+find_quant)/plan(QuantGemm→relu→QuantGemm)/roundtrip; Vk+DX12 (composed MLP == float oracle, bit-exact); `relu.ckir` + a standalone `quantize_q8_0` rounding cross-check |
| **23d** | `ceir.sparse` (`sparse.spmv`, CSR) declared (opgen) + `find_sparse_misuse` | 7 kinds; the CSR triple + dense x as explicit rank-1 tensor OPERANDS (no new TypeKind); `row_ptr==y+1` via `.count` arithmetic; the For-loop runtime-bound micro-gate |
| **23e** | the **CSR SpMV** `.ckir` (runtime For + ForBreakIf + col_idx indirection) | bootstrap-via-`ckir_write`→committed→builder DELETED (18a-1); device-free eval == CPU CSR SpMV; Vk+DX12 gates |
| **23z** | the BAND-23 composing gate + sym-dequant reading gate | ONE module carries quant + sparse ops → `find_quant`+`find_sparse`+structure all None (verifiers COMPOSE) + TEXT AND BINARY round-trip byte-clean (the FIRST serializer crossing for these ops); `quant_dequantize_q8_sym.ckir` eval oracle |

## Row-per-config (the fresh close sweep — never inherited)

| Config | CEIR-23 result | Notes |
|---|---|---|
| **win-debug** (MSVC) | **28/28** ✅ (+ 22c blast 12/12) | full family incl. Vk+DX12 device gates |
| **win-release** (MSVC /O2 /GL /WX) | **28/28** ✅ (+ 22c 12/12) | clean `/WX` compile — `sparse.cpp` + the 155-node kernel + all 23 tests under release opt |
| **WSL linux-gcc** (`-Werror`) | device-free **12/12** ✅ (+ 22c 2/2) | ⭐ `sparse.cpp` + generated `sparse_ops.cpp` + `test_sparse`/`test_band23_gate` compile clean under gcc `-Werror` (first gcc exposure) |
| **lavapipe** (linux-gcc-debug, Mesa software Vulkan) | **16/16** ✅ | ⭐ the divergent-loop **SpMV** (For + ForBreakIf) + fused QuantGemm + MLP run on software Vulkan (llvmpipe's 3-defect history did NOT repeat) |
| **win-asan** (MSVC /fsanitize=address) | device-free **12/12** + plan **9/9** ✅ | no UAF/leak/OOB in the verifiers/oracles/band-gate + `plan_tensor_pipeline`/the fusable predicate |

## Zero-builder audit (the everything-authorable rule)

- ALGORITHMS are AUTHORED assets: `quant_dequantize_q8{,_sym}.ckir`, `quant_gemm_q8.ckir`, `relu.ckir`, `spmv_csr.ckir` (.ckir)
  + `quant_mlp.ceir`. Each of the 6 has a committed-asset **reading gate** (the 19z inventory rule).
- SpMV bootstrap builder: **DELETED** (the 18a-1 write→commit→read→delete mold; the marker comment survives).
- `engine/ceir/src/sparse.cpp` = a pure VERIFIER (no KGraph builders). `plan_tensor_pipeline`'s QuantGemm collapse is a PLAN
  transform, not a KGraph algorithm builder.
- KEPT test-only builders (verification infrastructure, NOT production algorithm sources): the 23d-1 segmented-sum builder
  (machinery-verification, the prefix-scan class) + `build_quant_mlp` (the sanctioned anti-drift oracle for `quant_mlp.ceir`).
- ✅ No disallowed C++ algorithm KGraph builders introduced.

## Deferral ledger (all typed-rejected or chartered name-forwards — never a silent subset)

- **quant asymmetric / per-axis PLAN path**: `PlanReject::UnsupportedQuantScheme` (the plan-path Q8 kernels are symmetric
  per-tensor; the asymmetric kernel is proven standalone at 23b-1, just not through the plan). Name-forward.
- **quant formats**: INT4/INT2 sub-byte packing, FP8 element, calibration metadata → name-forward (the 12a rule).
- **general-dims fused QuantGemm**: `quant_gemm_q8.ckir` is baked at [M4·K8·N8] (uniform-width MLP reuse) → a spec-/push-const
  parameterized fused kernel is name-forward.
- **`quantize_q8_0` provenance**: the crown uses DIRECT int8 (block-32/f16-scale Q8_0 doesn't fit the per-tensor rank-0 plan
  path); the rounding cross-check is DECOUPLED (a standalone device-free hesap gate).
- **sparse forms**: SpMM (sparse×dense-matrix) / batched / mixed-precision / CSC / COO / block-sparse / SpGEMM → future ops,
  name-forward (the op name IS the format; no `format` attr).
- **general max-row-length SpMV**: `spmv_csr.ckir`'s uniform For bound is baked (MAX=3 = the gate matrix's max row); a
  spec-/push-const bound is name-forward.
- **sparse.spmv is NOT a tensor-pipeline plan op** this band → a module containing it hits `PlanReject::UnsupportedOp` by
  construction (zero plan-code touched).
- **kir-cuda native-provider leg**: chartered at band-open as a LEDGER row with a home — the ⛔ CUDA-required mandate surfaces
  here; the CUDA native provider for the quant/sparse ops is a future band (the device proofs this band ran Vk + DX12 +
  lavapipe, the portable path).
- **the divergent-count For is DOCTRINE, not debt**: a uniform max bound + a per-thread `ForBreakIf` IS the portable/lockstep
  form (a bare divergent `stmt_for` reads as thread-0's count in eval AND diverges from the GPU masking model).

## Scars this band (memories written)

- ⛔⛔ **`eval_cpu_kernel` (and the GPU) is LOCKSTEP** — a For loop's trip count is UNIFORM (eval reads `active[0]`'s count for
  ALL threads); a bare divergent `stmt_for(per-thread count)` SILENTLY runs thread-0's trips (the 23d-1 segmented-sum gave
  `3,7,13,15` for `3,12,6,24` — every segment ran 2 iterations). Divergent loop = uniform max bound + per-thread `ForBreakIf`.
  (memory: `feedback_ckir_eval_for_bound_is_uniform_use_max_plus_forbreakif`)
- ⛔⛔ **a fusion / specialized-kernel selection predicate must check EVERY semantic attribute the kernel assumes** — not just
  the structural trigger. The fused `quant_gemm_q8` is symmetric + per-tensor + α=1,β=0,no-transpose; the predicate gates ALL
  of those (advisor caught scheme+scale-rank; I caught α/β/trans by applying the principle). ONE shared predicate gates the
  fusion AND the unfused-selection, with a TYPED reject for the unhandled variants. (memory written)
- ⛔ **em-dash in a `TEST_CASE` name** re-bit at 23c-e: `ctest -R <name>` reports NotRun/exit-8 (codepage round-trip) while
  direct-by-tag passes. ASCII test names at WRITE time (the guard exists; I skipped it).
- 📌 **bootstrap-via-`ckir_write` with EVAL-VERIFY-BEFORE-COMMIT** (23e-a extended the 18a-1 mold): build the kernel, EVAL it
  vs the oracle, THEN `ckir_write`→commit→delete-builder — so the committed asset is verified-correct before the builder is
  gone. The advisor's For micro-gate (build→round-trip→eval→emit) is the prerequisite that de-risked the authoring.

## Commits (proposed — user commits; NO AI co-author trailer)

- `feat(ceir): CEIR-23a/23d declare ceir.quant + ceir.sparse (opgen) + find_quant_misuse/find_sparse_misuse + the band23 composing gate`
- `feat(ceir-gpu): CEIR-23b the fused dequantize→gemm QuantGemm collapse (plan 2→1) + the authored Q8 dequant/gemm .ckir kernels + Vulkan/DX12 device gates`
- `feat(ceir): CEIR-23c the quantized-MLP crown (quant_mlp.ceir + relu.ckir) parse-loaded + Vulkan/DX12 (composed == float oracle); CEIR-23e the CSR SpMV .ckir (runtime For + ForBreakIf + col_idx) + Vulkan/DX12 gates`

**CEIR-23 → BAND ✅.** Next: CEIR-24 (`ceir.ml` + provider partitioning → MLP + attention; advisor at band-open; drive toward CEIR-35).
