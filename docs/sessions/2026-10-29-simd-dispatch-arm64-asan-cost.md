# 2026-10-29 — SIMD-DISPATCH: the two ASan lanes at the job cap

<!-- doc-role: session -->
> Dated session note; no live status. Current work: [ROADMAP](../ROADMAP.md#slice-simd-dispatch).

Row: [SIMD-DISPATCH](../ROADMAP.md#slice-simd-dispatch) (Left: the ARM64 lanes' and the guard's first green runs).
Before: [the SELL job cut](2026-10-29-simd-dispatch-sell-job-cut.md).

## CI first

Scheduled run 40849112570 (fea7ee0a) completed at 15:23. Every push run since was cancelled by the next push and
none of their jobs failed. The two jobs still running at the last read both reached GitHub's 6 h job cap:

- **win-asan** (job 123363959205): 6,693 of 8,100 tests in 5 h 38 min of Test step. Three failures:
  crd-generated-sources and crd-repository-tools on the transcendental refs' provenance (fixed at 74a95125, after
  fea7ee0a), and **crd-check-isa-isolation** (373 s): `cpu_features.cpp.obj` "defines the per-level symbol
  `L__asan_global_??_C@_04FNEJMADF@sse2@`". That name is MSVC ASan's descriptor of the string literal `"sse2"`. The
  guard skipped literals by their bare `??_C@` prefix, so it missed the descriptor and matched the level in the
  literal's bytes. The slowest cases: B19-a4 binning 1,960 s, the AMG W-cycle 1,367 s, the five fft forced-level arms
  620 to 1,121 s each, the hair beauty frame 747 s.
- **linux-gcc-arm64-asan** (job 123363959097): 2,283 of 2,474 tests; the build took 3 min, the Test step 5 h 56 min.
  Two failures: GCC's UBSan reported a signed overflow in `vaddq_s32` from NEON `Vec4i::operator+` (simd level
  probe, the level-applied and neon arms), and two cases timed out: "fft: four-step path (large n, f32)" at 3,600 s
  and crd-fuzz-ceir-differential-corpus at 300 s.

The GCC ARM64 ASan cost is not ASan's ordinary cost. Over the 2,281 cases all five ARM64 and x64 lanes ran:

| Lane | Median case | Sum of the cases |
|---|---|---|
| linux-gcc-arm64-asan | 0.25 s | 5.21 h |
| linux-clang-arm64-asan | 0.03 s | 1.52 h |
| linux-gcc-asan (x64) | 0.06 s | 1.34 h |
| linux-gcc-arm64-debug | under 0.01 s | 0.39 h |
| linux-clang-arm64-debug | under 0.01 s | 0.50 h |

Against each compiler's own debug lane, the sanitizers cost Clang at most 41 times on a case and GCC up to 1,924
times (15 cases above 280), plus about 0.2 s per process. The worst ratios are allocation-heavy parsers and fuzz sweeps (ceir's
binary deserializer 77 s against 1.5 s, the CEIR-18p impostor round trip 21 s against 0.1 s), the drop-frame
timecode day (637 s against 6 s; x64 GCC ASan 7 s) and the large fft cases (3,600 s and over against 121 s). x64
GCC ASan, from the same GCC 13, runs them at Clang's speed, so the cost belongs to GCC's AArch64 sanitizer build or
runtime. More test parallelism would hide it and leave the two timeouts, so the presets stay as they are until the
cause is measured.

Needs CI audit: DIAG.11c and DIAG.12 wait on a green complete-tier run; the scheduled run is red, and none was
dispatched (it would only repeat the two caps).

## What landed

- **5cf0e54e, the guard**: `described()` strips MSVC ASan's `L__asan_global_` prefix before every judgement (kernel
  objects, general objects, image maps), so a literal's descriptor passes like the literal and a level table's or
  level symbol's fails like it; a table's descriptor is not counted as a second table. Three fixtures in
  `IsaIsolation`; each fails with the stripping removed.
- **bd6623a2, NEON `Vec4i`**: GCC's `arm_neon.h` spells `vaddq_s32` and `vsubq_s32` as C++ `+` and `-` on the vector
  type, and GCC's UBSan instruments them. Both operators now add and subtract unsigned lanes, which wrap by
  definition, as the scalar fallback's `wrap_add` does. A clang `--target=aarch64-pc-windows-msvc` syntax probe
  compiled the NEON branch; the lane is the evidence.
- **The measurement**: `host-check.yml` takes an `experiment` input (all, fuzz, tsan, arm64-sanitizer-cost). The
  new legs, one per ARM64 ASan preset on `ubuntu-24.04-arm`, build the preset and a copy without UBSan, then time
  five cases through ctest, two interleaved rounds, under the defaults, `detect_stack_use_after_return=0`,
  `malloc_context_size=0`, `quarantine_size_mb=0`, `detect_leaks=0` and ASan alone, plus one `verbosity=1` start.
  The arms separate UBSan from ASan, and within ASan the fake stack, the allocation stack traces, the quarantine and
  the leak scan. The fix goes where the cost is; an option that weakens detection on one lane only is not a fix.

## Left

- Read the dispatched host check, fix the cause, then the two timeouts and linux-gcc-arm64-asan's first green run.
- win-asan's cap: 1,407 cases did not run. The forced fft arms each run portable and their own level
  (5fbf494d); the rest is per-case ASan cost on the CPU-rendered GPU cases.
