# 2026-11-07: snapshot interpolation and rewind history, bit-exact with networked-physics

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [HESAP-NET](../ROADMAP.md#slice-hesap-net) (stays Partial). Track C batch 105.

## CI first

Push run 42319502866 (23e5b36b, the first to carry batches 102-103 on these lanes) failed from this track in three ways, fixed:
- `test_clock.cpp` did not compile on any clang lane (double to long double, `-Wdouble-promotion`), so linux-clang-tsan and
  linux-clang-arm64-debug never built. The whole test target now builds on linux-clang-tsan here.
- "clock tracker: exchanges at the ends of the clock range" overflowed in the test's own addition under UBSan
  (linux-gcc-arm64-asan). It steps inward now; the engine's code at those ends then ran clean on linux-gcc-asan.
- `crd-ci-platform` on every lane: the libwebrtc install step was missing from `.github/ci-tiers.json`, so the orchestrator's
  lane skipped it and the parity table was stale. That test now joins this track's pre-push checks when ci.yml changes.

Not this track's, recorded: the mikktspace oracle on linux-gcc-asan shard 2 of 42298265296 (registered TP-5). Dispatch
42328203363 is still queued.

## What landed, capability `numerics.motion-sampling`, kernel family `hesap.motion`, no allocation

- `motion.hpp`: lerp, cubic Hermite and extrapolation over flat f32 and f64 arrays of any layout; nlerp and slerp over
  the x y z w of `smallest_three_decode`. nlerp answers the identity rather than a NaN for any input.
- slerp is Eberly's series in cos(angle), lane arithmetic only, so it needs no scalar libm and every level gives one
  answer. Eight factors, as remembered from his listing, measured 2.6e-5 here; sixteen with the balance constant searched
  afresh (1.902) stay within 2.7e-8.
- `rewind.hpp`: `RewindHistory<N>` holds strictly increasing stamps and answers a slot pair and `t`, moving a time past
  the declared window and saying so, and refusing to interpolate a span wider than the declared gap. The distance is taken
  unsigned, so the whole i64 range works.
- `crd/math/simd/interleave4.hpp`: four channels against lane registers on every level, added to the math suite's level
  probe; `hesap.quantize`'s encoder uses it too.

## Evidence

- `crd-hesap-motion-peer-tests` (linux-gcc-public-checks, LTO linux-gcc-release): every level's positions and nlerp bit for
  bit with `InterpolateSnapshot_Linear`, `_Hermite`, `_Hermite_WithExtrapolation` and vectorial's `nlerp`, cut from the
  pinned Snapshot.h and quat4f.h. slerp's worst component error is 1.77e-7 against the peer's 1.56e-4.
- `crd-hesap-net-tests` `[motion]`, 178,000 assertions in 16 cases, on win-debug, linux-gcc-debug and linux-gcc-asan, compiled by
  linux-clang-tsan: every level against restated expressions on lengths 0 to 70, guards past each batch, in place, hostile
  values against portable, closed-form paths, the history against a linear search, a rewound steady motion.
- Forced-level arms pass on linux-gcc-debug, avx512 under SDE skx and icx. The NEON paths pass an aarch64 clang syntax check
  only: audit `crd-hesap-net-kernels-at-neon` and the math probe on the ARM64 lanes.
- `crd-fuzz-hesap-motion`, 15 seeds: 1,352,456 libFuzzer executions clean under ASan and UBSan; replay clean on win-debug. The
  first run's one finding was the target's own unit-length oracle, too strict for a chord that underflows.
- 31 teeth, 29 caught. The two missed are equivalent: the tail's zero padding (each lane is independent, and it is kept so
  no uninitialised lane is read) and `max_rewind <= 0` (the same as `< 0`, which the code now says).
- clang-tidy clean: LLVM 20 on win-debug's files and the peer lane's.
- [Board](../bench/2026-11-07-hesap-net-motion-vs-networked-physics.md): 2.7x to 2.8x the peer on SSE2 and 5.0x to 5.2x on AVX2
  for a whole body, 1.2x and 1.9x on nlerp. Only the portable level's nlerp is behind, 2.2x.

## Next

IEEE 1278.1 dead reckoning: nine algorithms in f64 on quaternions, per-entity sincos through the scalar table and lanes for the
rest, against KDIS's positions, Open-DIS's integrals restated in long double and an integrated path; projective velocity
blending. Then NET-DEBUG's Philox distributions and RaptorQ.
