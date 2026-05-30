# ADR-0088 — GEMM hand-tuned asm: INVESTIGATED → REVERTED (intrinsics vindicated)

**Date:** 2026-05-30
**Status:** **REVERTED 2026-05-30** — the asm was built end-to-end, measured cleanly, and found **~2%
SLOWER** than the inlined intrinsic. **All asm code (spike + f64 kernel + runtime dispatch + dual-syntax
build + the v0d-asm-0 direct-to-C framework change) was reverted.** ADR-0082 (intrinsics-first) is
**VINDICATED and STANDS.** This ADR is kept as the durable *investigation record* so the asm path is not
re-tried without new evidence. **Does NOT supersede ADR-0082** — the investigation CONFIRMED it.
**Tags:** [arch] [hesap] [math] [simd] [blas3] [microkernel] [asm] [perf] [reverted]
**Supersedes:** (none — investigated re-opening ADR-0082, then confirmed ADR-0082 by measurement)

> ⚠ The sections below were written when the asm was being ADOPTED (the premise that the asm kernel
> would be the perf lever). They are preserved verbatim as the record of what was attempted; the
> **2026-05-31 final amendment at the bottom** is the binding outcome — read it first.

## Context

ADR-0082 (2026-05-19) chose pure-intrinsics microkernels and deferred per-µarch asm indefinitely
behind a locked hot-swap point (`gemm_microkernel<T>` signature + `CRD_HESAP_MICROKERNEL_BACKEND`
switch), with a three-condition revisit gate.

The 2026-05-29/30 CHOLMOD crush measured the gate firing for the sparse-direct workload:
- **C1 (GEMM-bound measured workload, N>1000):** bmwcra_1 factor is cmod+cdiv-gemm-bound; CHOLMOD is
  a real measured peer. ✓
- **C2 (intrinsics <70% of asm peak):** on square-NN we are ~71% of OpenBLAS (borderline). But on the
  **NT-ColMajor cmod (~0.82×) and especially cdiv (0.40–0.45×) shapes that DOMINATE the sparse
  factor**, we are well below 70% of OpenBLAS asm peak. ✓ for the real shapes.
- **C3 (no better lever):** scheduling (tree/node/2D-hybrid), symbolic (the O(nnz) rewrite + slead),
  and the cdiv ColMajor→RowMajor reroutes were all banked this cluster; the residual is the gemm
  per-flop rate. GPU offload doesn't fit the determinism-moat sparse factor. ✓

Decisive measurement: **bmwcra Cerid flops == CHOLMOD flops** (1.286e11 ≈ 1.287e11) — the prior
"Cerid's lower fill ⇒ fewer flops ⇒ matching efficiency wins via the fill margin" thesis is FALSE
(fill ≠ flops). So bmwcra factor's ceiling == the gemm per-flop ratio; raising it is the ONLY ≥1.0×
path for bmwcra AND a ~20% lift for every gemm-bound hesap op (eig/SVD/LU/dense-Chol/AMG). The user
elected to make the gemm floor a **permanent, multiplatform foundation** before building more hesap on
top of it. See `docs/sessions/2026-05-30-hesap-v5a-6-*` and `docs/phases/hesap-v0d-asm-microkernel-plan.md`.

## Decision

Re-open asm. Specifically:

1. **Framework-first, then asm vs the measured residual.** Portable C++ wins first — direct-to-C
   microkernel store (kills the `gemm_packed_inner` temp-micro + scalar-merge ~10%), Mc/Kc/Nc retune.
   These are WASM-safe, bit-identical, raise the floor for every shape, and are on the asm critical
   path anyway. Then asm targets the *measured* residual + the NT/ColMajor/skinny shapes.

2. **Runtime CPUID → function-pointer dispatch (NEW, reusable engine facility).** One binary detects
   AVX2/AVX-512/NEON/scalar at startup and selects the best kernel; WASM uses its fixed SIMD path at
   compile time. The existing compile-time `CRD_SIMD_BACKEND` becomes the build-time *floor/fallback*,
   not the selector. This dispatch + the dual-syntax build + the bit-identity test harness live in a
   **reusable home** (crd-math::simd / a small leaf), NOT buried in hesap — GEMM is the first consumer.

3. **Dual-syntax asm so the primary ship compiler gets it too:** MASM `.asm` (ml64) for MSVC + GAS
   `.S` for gcc/clang/clang-cl. Both bit-identical to each other AND to the intrinsic kernel.

4. **Bit-identity is the ABI contract — cross-compiler AND cross-ISA.** Every selectable backend
   (intrinsic-f64, asm-f64 MASM≡GAS, future AVX-512/NEON) produces bit-identical output: single-rounded
   FMA (`vfmadd231`-equivalent), the SAME p-order, the SAME NR/accumulation grouping (6×8 f64, 8×8 f32).
   AVX-512's wider tile MUST hold the accumulation grouping or it breaks the determinism moat (ADR-0063).
   A direct microkernel-vs-intrinsic bit-compare test gates every selectable backend.

5. **Incremental µarch coverage (REVISES ADR-0082's "≥6 µarchs day 1").** Runtime dispatch + the
   intrinsic fallback make partial asm coverage SAFE — an uncovered µarch falls back to the bit-identical
   intrinsic kernel. Start with **AVX2** (the dev/CI hardware; the 14900K has NO AVX-512 — Raptor Lake
   consumer). Add AVX-512 (server runner) / NEON (ARM/Apple) when a runner or workload justifies. No
   all-or-nothing bar.

6. **Infrastructure general; kernels measured.** The dispatch + dual-syntax build + bit-identity harness
   are a general engine capability. Transforms / rendering / geometry may add THEIR OWN asm kernels to
   the same infrastructure WHEN a measured frame-budget bottleneck justifies it — NOT speculatively. The
   GEMM register-tile kernel does NOT transfer to fixed-size 4×4 transforms or batched geometry ops
   (different pattern); each hot loop that earns asm writes its own kernel. "Real workload before
   optimization" + the engine-itis guard (elite-no-shortcuts is a shipping risk) stay in force.

## Rationale

- The ADR-0082 gate is **met on the real sparse-factor shapes** (cdiv ~0.45× asm peak), not overridden.
- Equal-flop measurement proves the gemm rate is the *only* ≥1.0× lever for bmwcra; it also lifts all
  gemm-bound hesap — a foundation investment, not a one-matrix chase.
- Runtime fallback dissolves ADR-0082's biggest deterrent (the "≥6 µarchs day-1 forever-debt"):
  incremental coverage is now safe, so the maintenance commitment scales with measured need.
- WASM/multiplatform is *protected*: native asm is never selected for WASM/unknown µarchs — they use
  the intrinsic/WASM-SIMD path, the universal fallback. The hybrid serves portability, not against it.
- Keeping the infra reusable (not hesap-internal) makes future transform/geometry kernels cheap when
  they're measured — without paying for them speculatively now.

## Consequences

**Positive:** the gemm floor rises for all hesap; one shipped binary auto-selects the best kernel; the
asm-kernel facility becomes engine-wide; WASM/ARM stay clean via fallback.

**Negative / costs:** per-µarch asm maintenance (now incremental + fallback-safe, not 42-files-day-1);
CI matrix grows per µarch shipped; the bit-identity ABI surface must be audited per backend; dual-syntax
(MASM+GAS) ≈ 2× asm source per µarch. The asm must replicate the intrinsic FMA p-order exactly.

**Determinism (ADR-0063):** the cross-compiler/cross-ISA bit-identity contract (decision §4) is the new
moat-failure mode; the bit-compare gate is mandatory on every selectable backend.

## Slice plan
`docs/phases/hesap-v0d-asm-microkernel-plan.md` — v0d-asm-0 (this ADR + framework C++ wins) → v0d-asm-1
(toolchain spike: dual-syntax no-op + runtime dispatch, green on MSVC/clang-cl/gcc, WASM/ARM fall back)
→ v0d-asm-2/3 (f64 6×8, f32 8×8 AVX2 asm) → v0d-asm-4 (integrate + re-run CHOLMOD crush + dense benches
+ full sweep). Slots BEFORE v5b sparse LU. Out of scope (land first): v5a-5 checkpoint; bmwcra routing+cap fix.

## 2026-05-30 update — MEASURED: the asm kernel does NOT crush bmwcra (kept opt-in)

v0d-asm-1 (toolchain spike) + v0d-asm-2 (f64 6×8 asm kernel) shipped + verified: the dual-syntax asm
(MASM/GAS) assembles+links+runs on MSVC+clang-cl+gcc, and the kernel is **bit-identical to the
intrinsic** (240-assert `[asm]` gate). Microbench: the asm kernel **is faster than the intrinsic**
(direction robust; magnitude untrustworthy — a ~9% cross-run turbo confound).

BUT the **decisive in-situ A/B** (bmwcra factor, same-binary env-toggle `CRD_HESAP_ASM_KERNEL`, 8T, 3×
each) shows asm-on vs asm-off **statistically indistinguishable** (medians ~2174 vs ~2194ms, full
overlap; asm-off posted the fastest run). **The kernel win washes out at the bmwcra scale** — that
factor is memory/scaling-bound (the 2.01×@8T plateau), not kernel-IPC-bound.

**So this ADR's core premise — "the asm kernel is THE bmwcra lever" — is REFUTED at scale.** Disposition:
the asm kernel is KEPT (correct + faster-at-kernel-level + the permanent multiplatform foundation +
the runtime-dispatch infrastructure, all reusable), but **OPT-IN** (`CRD_HESAP_ASM_KERNEL`, default
= intrinsic) — NOT engine-default, because it gives bmwcra no benefit, adds Win64 prologue overhead on
small-K tiles, and regressed one micro-shape's raw throughput. Flipping the engine-wide default is its
own slice needing a *kernel-bound* consumer's A/B (e.g. moderate-N dense eig/SVD/LU, which the bmwcra
washout does not speak to). **bmwcra's crush needs the SCALING lever (block-DAG / 2D-within-front), not
the kernel** (confirms the v5a-6 diagnosis: bmwcra = two walls, the scaling plateau being the binding
one at 8T). The build-integration + dispatch + bit-identity-ABI machinery this ADR established stand;
the perf thesis was narrowed by measurement.

## 2026-05-30 FINAL — same-process A/B: asm is ~2% SLOWER; cluster REVERTED; ADR-0082 vindicated

After the bmwcra washout, the open question was whether the asm helps KERNEL-bound work (the dense
solvers). Two measurements settled it:
- **Interleaved cross-process dense A/B** (kills the turbo confound): asm ~tied-to-+3%, mixed sign,
  buried in the box's ~13% turbo noise — no resolvable win. The earlier "+10–16% IPC" was the clock
  artifact (the %-of-peak normalization assumed startup-clock==GEMM-clock, which doesn't hold on a
  turboing 14900K).
- **DECISIVE same-process A/B** (asm vs intrinsic timed microseconds apart in ONE process ⇒ identical
  clock, zero confound): **asm/intrinsic ≈ 0.98 in every round — the asm is ~1–2% SLOWER.**

**Root cause (structural, not tunable):** the intrinsic microkernel **inlines** into `gemm_packed_inner`'s
panel loop (no call overhead, already ~98% of peak); the asm is a **hard function call** (call/ret +
prologue per microkernel invocation). For Cerid's inlined-microkernel usage the inlined intrinsic wins,
and no asm tuning closes it (can't inline hand-asm across TUs). OpenBLAS/BLIS asm wins only because their
*whole framework* is asm and not competing with an inlined-98%-peak intrinsic; Cerid is.

**Decision (user, 2026-05-30): REVERT the whole asm cluster.** Removed: the f64 asm kernel (MASM+GAS),
`asm_backend.{hpp,cpp}` (CPUID + dispatch), the `src/asm/` dir, the gemm_microkernel.hpp wiring, the
CMake asm-language integration, `test_asm_backend.cpp`, the bench kernel-A/B, AND the v0d-asm-0
direct-to-C framework change (gemm_pack.hpp/blas3.cpp — it was only ~3% within-noise too). The tree is
back to the committed v5a-5 state. **ADR-0082 (intrinsics-first) STANDS, vindicated by clean
measurement.** The hot-swap point ADR-0082 reserved remains for a genuine future need (an asm-only ISA
feature like AMX that intrinsics can't express — NOT a generic perf lever). **bmwcra's crush is the
SCALING lever (block-DAG / 2D-within-front), not the kernel.**

## References
- **ADR-0082 (STANDS, vindicated)** — intrinsics-first; the hot-swap point + the three-condition gate
- ADR-0063 — determinism contract (bit-exact across SIMD widths) — now cross-compiler+cross-ISA
- ADR-0078 §5 — two-layer typed architecture (raw SIMD inner kernels)
- ADR-0065 §13 — hesap elite-tier (D4 task-DAG, D6 modern hardware)
- `docs/sessions/2026-05-30-hesap-v5a-6-perlevel-gate-refuted-bmwcra-diag.md` — the equal-flop / kernel-ceiling finding
- Goto/Van de Geijn 2008; Van Zee/Smith 2014 (BLIS) — framework + per-µarch microkernel model
- `feedback_reference_implementations_are_the_floor`, `feedback_quality_bar`, `project_browser_wasm_deployment_goal`
