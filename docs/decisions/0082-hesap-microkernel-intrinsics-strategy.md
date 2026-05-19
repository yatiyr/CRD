# ADR-0082 — Hesap GEMM microkernel: intrinsics-via-Vec8f/Vec16f, ASM deferred

**Date:** 2026-05-19
**Status:** **Accepted**
**Tags:** [arch] [hesap] [blas3] [microkernel] [perf] [simd]
**Supersedes:** (none — refines ADR-0065 §13 D4 microkernel discipline + ADR-0078 §5 two-layer architecture)

## Context

Phase 3.1.6 v0d ships BLAS L3 (gemm / syrk / herk / syr2k / her2k /
trmm / trsm). The phase-doc target is "≥70% AVX-512 peak GEMM" per
ADR-0065 §13 D6 (modern hardware support). The natural elite question:
**do we hand-roll inline assembly microkernels per micro-architecture, or
do we stay in C++ intrinsics via `crd-math::simd`?**

The user surfaced this at v0d-foundation close 2026-05-19, asking "do
we really need hand-rolled inline asm per arch ... think as an elite
system architect."

## Decision

**Cerid ships pure C++ intrinsics microkernels via `crd-math::simd`.**
Hand-rolled inline-asm per-µarch is **deferred indefinitely** and lives
behind a documented hot-swap point that future architects can flip on
if a measured Cerid workload ever justifies the engineering cost.

Concretely for v0d-perf:

- AVX2 microkernel via `crd::math::simd::Vec8f` (8×8 register tile).
- AVX-512 microkernel via `crd::math::simd::Vec16f` (added in this slice).
- NEON microkernel via `crd::math::simd::Vec4f` (4×4 register tile for ARM).
- Scalar microkernel as the universal fallback (works for every T
  including `Complex<U>`).
- Runtime CPU detection picks the widest available kernel at startup.

**Target**: 80-85% of theoretical SIMD peak for f32 GEMM on the dev box.
This is the achievable ceiling for pure intrinsics; the last 5-10% gap
to MKL/BLIS-asm is documented as expected.

## Rationale

### What inline asm actually buys

The MKL-95%-peak vs BLIS-asm-92%-peak vs BLIS-intrinsics-85-88%-peak
spread is real but comes from specific compiler limitations:

1. **Register-allocation control** — 32 AVX-512 registers, want exactly
   24 holding C accumulators, 4 holding B columns, 4 for prefetch; the
   compiler often spills one or two. Asm doesn't.
2. **FMA chain scheduling** — Skylake-X / Sapphire Rapids have 2 FMA
   ports; saturating both needs interleaving that compilers reorder.
3. **Manual prefetch placement** — `vprefetch1` 8 cache lines ahead.
   Compilers don't.
4. **No stack spills** in tight inner loops.

Net: **5-10% on Intel µarchs, 3-5% on AMD Zen 4, ~2% on modern compilers
with PGO**. The gap is shrinking over compiler generations, not growing.

### What inline asm actually costs

Hand-rolled asm for GEMM is a fundamentally different engineering
practice from "more lines of code":

| Cost | Impact for Cerid |
|---|---|
| **Per-µarch maintenance** | BLIS ships separate kernels for Haswell / Skylake-X / Sapphire Rapids / Granite Rapids / Zen 2/3/4/5 / Apple M1/M2/M3/M4 / Neoverse N1/V1/V2 / A78. **42 hand-maintained files** for Cerid's 7 BLAS L3 ops × 6 µarchs. |
| **Toolchain pain** | MSVC doesn't support x86-64 inline asm (only intrinsics or external `.asm` via MASM). Supporting MSVC + clang-cl + GCC needs `.S` (AT&T) for GCC/clang-Linux, `.S` (Intel) for clang-cl, `.asm` (MASM) for MSVC. Three syntaxes per arch. |
| **Debugging asymmetry** | A typo in asm gives "wrong answer at C[37,42] only." Costs a day to bisect. Intrinsics give compile errors or clean failures. |
| **Determinism (ADR-0063)** | Cerid promises bit-exact across SIMD widths. Each asm variant gets audited separately. Intrinsics through Vec8f / Vec16f inherit the determinism contract. |
| **Forever-debt** | Every asm file is a maintenance item the project carries indefinitely. Cerid is ~30 modules already; adding 42 asm files is 2.4× the surface-area cost of a single new module. |

### Who needs asm (and who doesn't)

**Need it:**
- **Intel MKL / OneMKL** — Intel's business is selling the last 5%.
- **BLIS** — academic project; asm IS the research output.
- **HPC sites** — $50M supercomputer, 90% GEMM workload → 5% peak = $2.5M.
- **HFT firms** — every cycle counts.

**Don't need it (and ship pure intrinsics):**
- **Eigen** — 75-85% peak. Used by Google, TensorFlow Lite, ROS.
- **Faer (Rust)** — 80-85% AVX-512 via portable-simd.
- **Highway (Google)** — backs JAX/XLA CPU paths.
- **xtensor, Armadillo, mlpack, Stan-math** — all pure intrinsics.

These projects aren't lacking ambition; they made the elite-engineering
trade-off in the same direction Cerid does here.

### Why this is right for Cerid specifically

1. **Cerid is a general-purpose engine substrate, not a BLAS library.**
   The actual hesap consumers — eylem FEM (GEMV-dominated), geometry
   (small const-size matrix ops), iterative solvers (spMV-dominated),
   future ML training (GPU not CPU) — none are GEMM-FLOP-bound at scale
   where 5% peak matters.
2. **The strategic direction (ADR-0081, 2026-05-19) is agent-native +
   C++ hot-reload**, not "dethrone MKL." The 42-asm-file forever-debt
   doesn't fit the strategic spending plan.
3. **The `feedback_reference_implementations_are_the_floor` mandate
   is about workload measurement, not engineering class.** Cerid
   hitting ~85% peak with intrinsics matches the intrinsics-class
   reference (Eigen / Faer / Highway). That IS the floor. Asm-class is
   a different sport (different engineering practice).
4. **Maintenance asymmetry**: a Vec8f / Vec16f kernel gets compiler
   upgrades for free. LLVM 21's FMA scheduler is measurably better than
   19's; Cerid intrinsics get faster automatically when the user bumps
   LLVM. Asm kernels age the opposite way.

## Hot-swap architecture (open door for future ASM)

The decision keeps a **clean swap point** so a future architect can flip
on asm-class kernels without rewriting the rest of hesap-dense.

### Signature locked

The `gemm_microkernel<T>` dispatcher signature is the swap point and
is locked by this ADR:

```cpp
template <typename T>
void gemm_microkernel(crd::usize k,
                      const T* a_packed,    // packing layout per-backend
                      const T* b_packed,    // packing layout per-backend
                      T*       c_tile,      // strided output tile
                      crd::usize ldc) noexcept;
```

Same signature for every future asm variant. Tests run against the
dispatcher, not against the intrinsics implementation directly, so asm
variants drop in without changing test code.

### Compile-time backend selection

Cerid ships ONE compile-time switch:

```cpp
// engine/hesap-dense/include/crd/hesap/dense/detail/microkernel_backend.hpp
#define CRD_HESAP_MICROKERNEL_BACKEND_INTRINSICS 1  // default
#define CRD_HESAP_MICROKERNEL_BACKEND_ASM        2  // reserved
```

`gemm_microkernel.hpp` consults this via `#if`. The intrinsics branch is
always present and built. The asm branch is reserved-but-empty — when a
future architect ships the asm files, they add the .S sources + flip the
default. Existing intrinsics path remains as a fallback for arches
without asm coverage.

### Packing format is backend-coupled

Packed-layout (Ac panel + Bc panel format) is defined per-backend.
Today's intrinsics microkernel expects:
- `a_packed`: MR × K row-major (8 contiguous A rows per microkernel iter)
- `b_packed`: K × NR row-major (K rows, NR=8 cols)

When asm lands, the BLIS-convention packing (MR-wide column slabs for A,
NR-wide row slabs for B) becomes a per-backend variant. The packing
function lives next to the microkernel and the two ship together; the
DRIVER doesn't care which packing format is in use.

### Conditions to revisit this ADR

This ADR should be superseded by a new ADR only when **all three** hold:

1. **A measured Cerid workload** sustains >50% of total solve time on
   GEMM at N > 1000 (i.e. GEMM is the bottleneck, not a constant-N
   inner-loop concern). AND
2. **The intrinsics microkernel measures < 70% of MKL/BLIS-asm peak**
   on that workload's hardware. AND
3. **No other optimization** (GPU offload via `crd-rhi-compute`, switch
   to a sparse algorithm, different solver) has a better cost-benefit
   ratio than per-arch asm.

If all three are true, the new ADR commits to the per-µarch asm
maintenance plan (≥6 µarchs from day 1; CI matrix expansion; ABI
guarantees for the extern asm symbols) and lands a `v0d-asm-microkernel`
slice that ships:
- `engine/hesap-dense/src/asm/` directory with per-arch .S files
- `CRD_HESAP_MICROKERNEL_BACKEND_ASM` flipped to default
- New CI matrix entries per arch

Until that day comes, the intrinsics path is the elite path **for Cerid's
engineering economics**.

## Consequences

**Positive:**
- v0d-perf ships in 3-4 days (intrinsics) not 8-12 days (asm).
- Forever maintenance cost stays small; compiler upgrades benefit hesap
  automatically.
- Cross-platform from day 1: MSVC + clang-cl + GCC + Linux + Windows
  + future Mac/ARM CI all work with the same source.
- Determinism contract (ADR-0063) auto-inherits via crd-math::simd.

**Negative:**
- Final 5-10% of peak performance not achievable until asm path lands.
- Some HPC benchmark scenarios will show Cerid hesap behind MKL.
- The intrinsics path can't use exotic asm-only features (e.g. AMX tiles
  on Sapphire Rapids) — those gates land when the asm path opens.

**Insertion points:**
- v0d-perf (this slice) ships the hot-swap point architecture + the
  intrinsics microkernel family.
- Future v0d-asm-microkernel (deferred) flips the backend switch.
- ADR re-evaluation criteria documented above; no time-bounded review
  (Cerid does measured re-evaluation, not calendar-based).

## 2026-05-20 update — FMA acceptance + reference-class shootout outcome

The original 2026-05-19 ADR called for two-rounded `mul_add` (a*b + c
with two roundings) per ADR-0063's determinism contract. After the
reference-class shootout vs Eigen-MT (see
`docs/sessions/2026-05-19-hesap-vs-reference-shootout.md`), we ratified:

- **Hesap microkernels use `crd::math::simd::fma(a, b, c)` — single-
  rounded IEEE 754 FMA** via `_mm256_fmadd_ps` / `_mm256_fmadd_pd`
  (AVX2 path) with `std::fma` scalar fallback. This is ~2× the throughput
  of the two-rounded `mul_add` path on AVX2-FMA hardware.
- **Determinism contract for hesap**: bit-exact across SIMD widths AND
  scalar paths because `std::fma` and hardware FMA are IEEE 754-2008
  mandated to produce identical results.
- **ADR-0063 (eylem determinism contract) continues to use `mul_add`**
  with two roundings — that's a physics-replay requirement and a
  different contract. The two coexist: hesap is numerical computing
  (perf-critical, single-rounded FMA OK), eylem is physics replay
  (bit-exact-across-recompile required, two-rounded mul_add).
- **The asm-microkernel revisit gate continues to NOT be triggered** —
  intrinsics + FMA achieves 70-100% of single-core peak on the dev box
  (i9-14900K AVX2) per the L3 shootout. We BEAT Eigen-MT at all 10 GEMM
  sizes (f32 + f64) with the intrinsics path alone.

The reference-class GEMM shootout result (10/10 WINS over Eigen-MT)
validates the intrinsics-first decision empirically. The remaining
5-25% gap on L2 ops (gemv small-N, symv mid-N, trsv large-N) is
filed but does NOT trigger the asm gate either (gap is < 30%, and
no consumer slice yet bottlenecks on those L2 ops).

## References

- ADR-0065 §13 — hesap elite-tier amendments (D4 task-DAG, D6 modern hardware)
- ADR-0063 — determinism contract (bit-exact across SIMD widths)
- ADR-0078 §5 — two-layer typed architecture (raw SIMD inner kernels)
- Goto / Van de Geijn 2008 — "Anatomy of high-performance matrix multiplication" (foundational)
- Van Zee / Smith 2014 — BLIS framework paper
- Frison / Diehl 2018 — BLASFEO small-matrix specialization
- Heinecke 2016 — libxsmm JIT
- `feedback_reference_implementations_are_the_floor` — hardware-floor mandate
- `feedback_quality_bar` — single-path elite
- `feedback_ship_at_consumer_template_from_day_one` — substrate proactively, speculative defer
