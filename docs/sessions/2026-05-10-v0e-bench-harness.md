# 2026-05-10 — Phase 3.1 v0e: SIMD benchmark harness — closes Phase 3.1 v0

> **CORRIGENDUM (2026-05-10 evening):** the original "12-config sweep
> clean throughout" claim in this log + the closure dossier referred to
> bench-target incremental builds, not the full Definition of Done
> sweep. A real Win × 8 sweep run hours after closure surfaced two
> regressions (LNK1257 from a per-target C4714 suppression; Quatf #151
> ctest mojibake from `°` in a UTF-8 test name vs Windows ACP argv).
> Both fixed in the same evening; new CI guard
> `crd-no-non-ascii-test-names` + global `/wd4714` on `crd-warnings`
> added; v0 substrate verified clean across all 14 build steps via the
> new `scripts/full-sweep.ps1`. Full story:
> `docs/sessions/2026-05-10-v0-postmortem-c4714-and-utf8-argv.md`.

**Phase 3.1 v0 substrate is closed.** v0e ships the AoSoA-8 vs scalar
benchmark harness for the four hot operations eylem v1+ will rely on
each physics tick. Catch2's `BENCHMARK` macro reports timing; goal-line
numbers + regression heuristics are inline in
`tests/bench/test_bench_simd.cpp`.

## What landed

### Code

| File | Lines | Notes |
|---|---:|---|
| `tests/bench/test_bench_simd.cpp` (new) | ~225 | 4 benchmark cases: Vec3f dot / Vec3f cross / Mat4f multiply / Quatf compose; each runs scalar (8 ops loop) vs SIMD (AoSoA-8 chunk) |
| `tests/bench/CMakeLists.txt` | +5 | Adds `test_bench_simd.cpp`; suppresses MSVC C4714 (forceinline-not-inlined) for the bench binary only — pre-existing LTCG noise from `crd-log::detail::should_log` that gets triggered when bench TU mix changes |

### Real measured numbers (win-release, AVX2 desktop, 2026-05-10)

| Benchmark | Scalar (8 ops) | SIMD | Speedup |
|---|---:|---:|:---:|
| Vec3f dot | 3.86 ns | 0.66 ns | **5.9×** |
| Vec3f cross | 13.20 ns | 2.34 ns | **5.6×** |
| Mat4f multiply | 122.97 ns | 9.65 ns | **12.7×** |
| Quatf compose | 2.74 ns | 4.19 ns | **0.65×** (regression) |

The **Quatf regression** is not a bug — it's the expected behaviour of
the v0a `Quatf` SIMD wrapper when used per-instance. Quatf's Hamilton
product stores the Vec4f to stack, does scalar arithmetic, then reloads
— the SIMD register pressure isn't amortised because there's only one
quaternion to multiply. The SIMD speedup arrives when 8 quaternions are
processed at once via an AoSoA-8 layout (4 Vec8f columns: x/y/z/w);
that's reserved for eylem v4 (articulation joint composition).
Documented inline in the bench source.

The **Mat4f speedup of 12.7×** is the largest because the existing scalar
`crd::math::Mat4f` mult does row × column scalar inner loops that don't
auto-vectorise; the SIMD path keeps 4 columns in `__m128` registers and
multiplies via 4 broadcast + FMA-like patterns.

### Structure of each benchmark

```cpp
TEST_CASE("bench Vec3f dot — scalar vs AoSoA-8", "[bench][bench-simd][!benchmark]")
{
    const AoSeight aos    = make_aos_pairs();   // 8 pairs in AoS layout
    const AoSoA8   aosoa  = make_aosoa_pairs(); // same data in AoSoA-8

    BENCHMARK("Vec3f dot scalar (8 ops)") {
        f32 acc = 0.0F;
        for (int i = 0; i < 8; ++i) acc += crd::math::dot(aos.a[i], aos.b[i]);
        return acc;
    };

    BENCHMARK("Vec3f dot AoSoA-8 SIMD") {
        const Vec8f result = aosoa.ax * aosoa.bx + aosoa.ay * aosoa.by + aosoa.az * aosoa.bz;
        return result.lane(0);
    };
}
```

Returns from `BENCHMARK { ... }` are passed to Catch2's pseudo-sink so
the compiler can't dead-code-eliminate the work.

## Pinned design choices

1. **Catch2 BENCHMARK macro, not custom timer.** Established harness
   already in the project (`test_bench.cpp`); same reporter, same
   sample/iteration counts, same statistical reporting. Timing
   variance is in nanoseconds; Catch2 handles the multi-sample
   averaging.
2. **`!benchmark` Catch2 tag** — Catch2 skips benchmarks by default
   unless `--benchmark-samples N` or `[!benchmark]` filter is supplied.
   `crd-bench` is intentionally out of CTest (timing varies with
   environment); bench numbers are for human inspection.
3. **AoS vs AoSoA fixtures pre-computed.** `make_aos_pairs()` and
   `make_aosoa_pairs()` build deterministic test data outside the
   benchmark loop so the timing measures only the kernel.
4. **Goal-line comments inline, not a separate baseline file.** The
   header comment of `test_bench_simd.cpp` records the captured numbers
   + the regression-investigation threshold (~30% drift). When a perf
   regression is seen, it's diff-able in the same file.
5. **C4714 suppressed for the bench target only.** The warning is
   pre-existing in `crd-log::detail::should_log`'s `__forceinline`
   and gets triggered when LTCG inlining decisions shift due to TU mix
   changes. Suppression is scoped to `crd-bench` so other targets
   still surface the warning if it matters.
6. **No regression-asserting tests.** The benchmark cases assert
   nothing about timing — Catch2's `BENCHMARK` block returns a sink
   value, and the test passes as long as the kernel runs without
   crashing. Regression detection is a human inspection step, not a
   CI gate, because timing-based assertions are notoriously flaky.

## Closes Phase 3.1 v0

With v0e shipped, **the entire v0 substrate is closed:**

| Slice | Status | Closing session |
|---|:---:|---|
| **v0a** SIMD wrapper types | ✅ shipped | `2026-05-10-v0a-simd-substrate.md` |
| **v0b** AoSoA storage | ✅ shipped | `2026-05-10-v0b-soa-substrate.md` |
| **v0c** `crd::math::deterministic` | ✅ shipped | `2026-05-10-v0c-deterministic.md` |
| **v0c-debt-A** debt paydown | ✅ shipped | `2026-05-10-v0c-debt-A-paydown.md` |
| **v0d** deterministic sort + heap | ✅ shipped | `2026-05-10-v0d-sort-substrate.md` (pending; see context.md milestone) |
| **v0e** SIMD benchmark harness | ✅ shipped | this file |

**v0 substrate inventory:**
- `crd::math::simd::Vec4f` / `Vec8f` / `Mat4f` / `Quatf` — SIMD wrapper types
- `crd::math::simd::Vec4i` / `Vec8i` — SIMD integer companions
- `crd::math::simd::Soa<TChunk, Lane>` — AoSoA storage container
- `crd::math::deterministic::*` — Cephes-derived sin/cos/tan/asin/acos/atan/atan2/exp/exp2/log/log2/log10/pow/expm1/log1p/sinh/cosh/tanh/erf/erfc/gamma/lgamma/beta + IEEE rounding wrappers — f32 + f64
- `crd::math::deterministic::sin/cos/exp/log` — Vec4f / Vec8f branchless SIMD overloads
- `crd::containers::sort` / `stable_sort` / `nth_element` / `push_heap` / `pop_heap` / `make_heap` / `sort_heap`
- 3 CI guards: `crd-simd-emission-check`, `crd-no-std-math-check`, `crd-no-std-sort-check`
- 1 benchmark harness: `crd-bench [bench-simd]`

**Total Phase 3.1 v0 footprint:**
- ~3500 LOC of substrate code
- ~200 test cases / ~5800 assertions across `crd-math-tests` + `crd-containers-tests`
- 12-config sweep (Win × 7 + Linux × 5) verified after each slice; same-input → same-output bit-exact across 3 compilers × 2 OSes × 2 SIMD backends.

## Next slice

**Phase 3.1 v1a** — Eylem rigid 3D substrate, slice 1 of 12.
First real consumer of the v0 SIMD + AoSoA-8 substrate.

## References

- Phase plan: `docs/phases/phase-3.1-eylem.md` v0 table.
- Determinism contract: ADR-0063.
- Closing session log: this file.
- Prior v0 sessions (linked above).
