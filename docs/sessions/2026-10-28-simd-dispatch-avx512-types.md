# 2026-10-28 — SIMD-DISPATCH: the 512-bit types

<!-- doc-role: session -->
Row: [SIMD-DISPATCH](../ROADMAP.md#slice-simd-dispatch). HEAD before: `1ccc57de`.

## CI first

Push run 40623100798 (`bedfc2bb`) is still in progress. Its finished jobs show four failure groups:

- Every Windows tooling job: the SDE installer's `pin.ld.so` link. This was fixed in `21ccf7c2`.
- win-tidy: `misc-confusable-identifiers` in tensor_kernels.cpp. Also fixed in `21ccf7c2`.
- The Windows release, shipping and profile builds: C1002 in the fft units. Fixed in `3e849d96`. Two ceir test
  objects on the same lanes ran out of heap beside them (C1060); complete run 40649898318 at `3e849d96` is expected
  to show that they were collateral.
- linux-clang-tsan: races on fiber dispatch in four tests. This group is new, and `38b982f2` fixes it
  ([note](2026-10-28-simd-dispatch-debug-frames-eig-fixture.md)).

Complete run 40649898318 (`3e849d96`, in progress) lost linux-gcc-debug-scalar, one of the cold lanes, because a
read of the Vulkan validation layer archive stalled past `pins.download`'s 120 s timeout. The download now retries a
failure in the network up to three times, after 5, 15 and 45 s. It does not retry a 4xx status or a checksum mismatch.
The tooling suite covers this case.

The linux-gcc release, relwithdebinfo and shipping builds ended with exit 143 ("the runner has received a shutdown
signal") while the fft units compiled. No row is Needs CI. DIAG.11c and DIAG.12 still wait on a finished complete-tier
run.

## Vec16f, Vec8d, Vec16i

`vec16f.hpp`, `vec8d.hpp` and `vec16i.hpp` live in the level namespace. Each type is one zmm register at avx512 and
two 256-bit halves below that: `Vec8f`, `Vec4d` and `Vec8i`, which are 128-bit pairs or arrays on the lower levels.
They are not part of the `simd.hpp` umbrella, so no general-code header reaches them.

- **API:** Vec16f has Vec8f's arithmetic, compare and select operations, plus the convert.hpp conversions, rounding
  and bit operations at 16 lanes. Vec8d has Vec4d's arithmetic, compare and select operations. Vec16i has Vec8i's
  operations. The complex helpers (`addsub`, `fmaddsub`, `swap_pairs`, the (de)interleaves, the transposes, the
  `*_c_*` loads and stores) are not included: no zmm body is planned for fft or the complex sweeps, so they come with
  the first body that needs them.
- **Masks:** a comparison gives all-ones lanes (`movm`), and `select` reads each lane's sign bit (`movepi*_mask`), as
  the 256-bit blend does.
- **min and max:** on a NaN or two zeros they return the second operand, as `minps` does.
- **horizontal_sum:** the low half's pairwise sum plus the high half's. It never uses `_mm512_reduce_*`.
- **Partial loads and stores:** they mask with `(1 << count) - 1`. Masked-off lanes load as +0, and nothing outside
  `count` is read or written.
- **`truncate` and `round_nearest`:** `roundscale` with scale 0, which is `roundps`.
- **`convert_truncate`:** uses the GCC fold barrier.

`CRD_SIMD_HAS_AVX512` is a new rung above `HAS_AVX2`, which stays true at avx512. At avx512, `backend_name()` is
"AVX-512".

## The probe

- **Inputs:** the probe's inputs are now 64-byte aligned, and its result buffer is 8 × 64 words.
- **New Vec16f and Vec16i operations:** 33 operations of the shared f32 and i32 templates. Vec16f also gets `fnmadd`,
  every partial count from 1 to 15 (the store's buffer keeps its guard lanes), and a halves round trip.
- **Shared f64 template:** the Vec4d operations moved into one f64 template that Vec8d shares (partial counts 1 to 7,
  halves).

Results:

- win-debug: the probe matches portable bit for bit at portable, sse2, sse4.2 and avx2 (crd-math-tests, 540,490
  assertions).
- Under SDE `-skx`, the avx512 level runs too and matches: 92,881 assertions, against 74,305 without it.

## The eight-lane log and exp twins

`crd_log8` and `crd_exp8` (transcendental.hpp, avx512 only) run crd_log4's and crd_exp4's operations in the same
order on one zmm register. The blends are masked multiply and add, the floor is `roundscale`, and the exponent
injection is the single one. Abnormal and edge lanes take the scalar core. The scaling is not done with `scalef`, which
rounds once where the core injects the exponent in two steps. The probe adds an eight-lane twin operation over the
twins' inputs: at avx512 it runs the new twins, and elsewhere it runs the level's own twins. Under SDE `-skx` it gives
crd_exp1's and crd_log1's bits (`[simd-levels]`: 667,778 assertions with avx512, against 534,378 natively).

## CI read 2

Complete run 40649898318 (`3e849d96`, in progress) has two finished failures; the push runs after it were cancelled
before any job failed.

- linux-gcc-debug-scalar: the Vulkan validation download stall above, fixed in `137b89f1`.
- win-clang-cl-shipping, one test of 7,863: `fiber keep: while tiny jobs stream the readings stay exact and bounded,
  and an idle pool holds nothing` (targeted wake) failed `samples > 0`. The 200 frames took a few milliseconds and
  ended before the reader thread was first scheduled, so it took no reading. The pool was not at fault. The stream now
  runs at least 200 frames and until the reader has finished 16 samples that it began after the stream started,
  within 10 s. `ran` is checked against the frames actually run (win-debug, `[fiber_keep]` 126 assertions).

Needs CI audit: DIAG.11c keeps its evidence. DIAG.12's last open clause, the race case on linux-clang-tsan at or after
`7eec1bf7`, passes in push run 40623100798 at `bedfc2bb`. That job failed only on the fiber-switch hooks fixed in
`38b982f2`. Both rows follow SIMD-DISPATCH and stay Needs CI.

## Left

Each family's zmm body.
