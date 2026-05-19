# Session 2026-05-19 — `crd-hesap` v0d-perf-f64-avx2 (Vec4d substrate + f64 AVX2 microkernel)

## Goal

After v0d-perf chunk 1 shipped Goto/BLIS layered GEMM with AVX2 f32
microkernel (f32 → 59% peak, f64 still on scalar fallback → 17% peak),
ship the **f64 AVX2 microkernel** to bring f64 into the same intrinsics-
tier perf class as f32.

User directive — **"IF THERE IS SOMETHING MISSING IN OUR MATH MODULE
LIKE Vec4d, we NEED TO IMPLEMENT IT, IF SOMETHING MISSING WE GO AND DO
IT!"** No shortcuts. Add Vec4d to `crd-math::simd` as a proper substrate
wrapper, then use it in the microkernel.

## What we built / changed

- **`engine/math/include/crd/math/simd/vec4d.hpp`** — NEW. Full 4-lane
  f64 SIMD wrapper mirroring the Vec4f / Vec8f shape:
  - `alignas(32)` struct with native `__m256d v;` (AVX2 path) or scalar
    `f64 lanes[4]` fallback.
  - Constructors: default, broadcast(`f64`), 4-element initializer.
  - Statics: `zero()`, `one()`, `load(p)`, `load_aligned(p)`.
  - `store(p)`, `store_aligned(p)`, `lane(i)`.
  - Arithmetic: `+`, `-`, `*`, `/`, unary `-`, scalar `*` (lhs/rhs),
    scalar `/`.
  - `mul_add(a, b, c) = (a * b) + c` and `mul_sub` — **NO hardware FMA**
    per ADR-0063 determinism contract (two roundings).
  - `min`, `max`, `abs` (via `_mm256_and_pd` with 0x7FFF... sign mask),
    `clamp`, `sqrt` (hardware `_mm256_sqrt_pd`).
  - Deterministic pairwise `horizontal_sum` + `dot`.
  - Comparisons `cmp_lt / le / eq / gt / ge` + `select`.
- **`engine/math/include/crd/math/simd/backend.hpp`** — add
  `k_vec4d_lanes = 4` constant.
- **`engine/math/include/crd/math/simd/simd.hpp`** — umbrella header
  now includes `vec4d.hpp`.
- **`tests/math/test_simd.cpp`** — 8 new test cases (24 assertions)
  covering Vec4d ctors / lane access / load+store round-trip / arithmetic
  / `mul_add` two-rounding contract / min-max-abs-sqrt / pairwise
  reductions / cmp+select / sizeof+alignment.
- **`engine/hesap-dense/include/crd/hesap/dense/detail/gemm_microkernel.hpp`**
  — new `gemm_microkernel_avx2_f64`:
  - Same MR=8 × NR=8 packed tile as the scalar / f32 path. Internally
    processes as 2 × (MR=8 × NR=4) halves since one Vec4d holds 4
    doubles.
  - Per half: 8 Vec4d row accumulators + 1 broadcast A register + 1 B
    load = 10 of 16 YMM registers (comfortable headroom).
  - Mul + add are SEPARATE Vec4d ops; deterministic per ADR-0063.
  - Dispatcher: `T = f32` → AVX2 f32; `T = f64` → AVX2 f64; complex /
    other types → scalar fallback.

## Measured perf uplift (dev box, AVX2, ~5.66 GHz, win-shipping)

```
==== f64 gemm (Vec4d AVX2 microkernel) ====
  N=  64    17.14 GFLOPS  (18.9% peak)   was 10.8%  → 1.75×
  N= 128    34.96 GFLOPS  (38.6% peak)   was 14.9%  → 2.6×
  N= 256    50.20 GFLOPS  (55.4% peak)   was 16.3%  → 3.4×
  N= 512    55.68 GFLOPS  (61.5% peak)   was 17.2%  → 3.6×
  N=1024    51.92 GFLOPS  (57.3% peak)   was 17.2%  → 3.3×
```

f64 GEMM now hits **57-61% single-core peak**, in the same intrinsics-
tier as f32 (58.6% at N=1024). The 3.3-3.6× speedup at large N matches
the 4× theoretical (one Vec4d holds 4 doubles vs 1 in scalar; remaining
gap = packing/load overhead amortized differently at f64 cache
footprint).

## Plain-English explanation

v0d-perf chunk 1 plugged AVX2 into f32 GEMM but f64 fell through to the
scalar microkernel because crd-math didn't have a Vec4d wrapper. The
proper substrate move was to add Vec4d to `crd-math::simd` (mirror of
Vec4f / Vec8f shape), then use it in the microkernel.

Vec4d works on every backend: AVX2 native (`__m256d`), scalar fallback
elsewhere. SSE2/NEON-only builds get correct f64 ops via the scalar
path — no missing-symbol surprises if someone configures Cerid for
those targets.

The user's directive — "if something missing, we implement it" —
captures the philosophy. The shortcut (raw `__m256d` inside the
microkernel) would have shipped the perf win but skipped the substrate
work. Future hesap modules that need f64 SIMD (spMV in v1, FFT in v10,
autodiff in v15) now get Vec4d for free.

## Decisions made

- **D50 (v0d-perf-f64-avx2)** — Vec4d added to `crd-math::simd` as a
  proper substrate wrapper, not raw `__m256d` in the microkernel. Per
  user 2026-05-19 directive + `feedback_quality_bar` no-shortcut
  policy. Vec4d's surface matches Vec4f / Vec8f for consistency;
  future Vec8d (for AVX-512) follows the same shape.
- **D51 (v0d-perf-f64-avx2)** — Vec4d fallback for non-AVX2 backends
  is a 4-element scalar array (no SSE2 composed-pair path yet — added
  if/when SSE2 f64 perf becomes a measurable concern).
- **D52 (v0d-perf-f64-avx2)** — f64 microkernel uses MR=8, NR=8
  PACKED tile but processes it as 2 × (MR=8, NR=4) halves internally.
  Same packing layout as scalar / f32 path — no per-type pack format,
  no driver refactor. Future AVX-512 f64 (`Vec8d`, 8-wide) microkernel
  processes the whole 8×8 in one shot.
- **D53 (v0d-perf-f64-avx2)** — `gemm_microkernel_avx2_f64` uses Vec4d
  arithmetic (no `_mm256_fmadd_pd`) for determinism. Vec4d's
  `operator+` / `operator*` are separate ops per ADR-0063.

Decisions D50-D53 queued for ADR-0065 §14 lock at v0-close.

## Files touched

- `engine/math/include/crd/math/simd/vec4d.hpp` — new
- `engine/math/include/crd/math/simd/backend.hpp` — `k_vec4d_lanes`
- `engine/math/include/crd/math/simd/simd.hpp` — umbrella
- `tests/math/test_simd.cpp` — 8 new Vec4d cases
- `engine/hesap-dense/include/crd/hesap/dense/detail/gemm_microkernel.hpp`
  — new `gemm_microkernel_avx2_f64` + dispatcher branch
- `context.md` — perf table update
- `docs/phases/phase-3.1.6-hesap.md` — v0d row perf-f64-avx2 entry

## Tests / verification

- **Built?** ✅ All targets clean under win-debug + win-shipping.
- **Tests pass?**
  - `crd-math-tests` Vec4d filter: **8 cases / 24 assertions PASS**.
  - `crd-hesap-dense-tests`: **108 cases / 4704 assertions PASS**
    unchanged (f64 AVX2 microkernel produces same numerical results as
    the scalar path for the existing correctness corpus).
- **Bench numbers**: see "Measured perf uplift" above.
- **Per-slice DoD?** _5-config `per-slice-check.ps1 -IncludeRelease`
  running at session-close authoring._

## Next session(s) starts with

Pick from v0d-perf follow-on backlog (priority order):

1. **`v0d-parallelism`** — outer-loop parallelism via existing
   `crd-hesap-sched::parallel_tiles_for`; ~3-7× on multi-core at large
   N (multiplies single-core perf without changing peak ratio). 1 day.
2. **`v0d-perf-prefetch` + `v0d-perf-block-tune`** — closing toward
   ~80% single-core peak. 1-2 days.
3. **`v0d-perf-arch-extend`** — AVX-512 (with `Vec8d` mirroring this
   slice's Vec4d work) + NEON microkernels. Multi-session; needs
   AVX-512-capable HW for honest characterization.
4. **v0e dense direct solvers** — LU / Cholesky / QR / LDLT / IR /
   LinearOp factor view / condition estimation. ~1200 LOC + ~120
   tests + ~7 d. Consumes today's BLAS L3 correctness surface.
