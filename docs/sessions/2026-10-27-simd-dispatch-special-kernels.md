# 2026-10-27 — SIMD-DISPATCH: the hesap-special Lanczos kernels

<!-- doc-role: session -->
Row: [SIMD-DISPATCH](../ROADMAP.md#slice-simd-dispatch). Decision: [ADR-0134](../decisions/0134-runtime-cpu-dispatch.md).

## CI read

- No completed run since the last read has a failed job: 40458470799 (`c65799a6`), 40451010209 and 40445742611 were
  cancelled before any job failed. Push run 40455017769 (`09ba6a3a`) is in progress (five jobs green, none failed)
  and 40462897802 (`12fbcb1a`) is pending. Nothing was dispatched.
- Needs CI audit: no complete run finished since 40434280766, so [DIAG.11c](../ROADMAP.md#slice-diag.11c) and
  [DIAG.12](../ROADMAP.md#slice-diag.12) stay as recorded on the threefry note; they follow
  [DIAG.11b](../ROADMAP.md#slice-diag.11b)'s user re-record (8).

## Mechanism

hesap-special is the fourth family and the smallest of the five the row lists (two routines, one consumer header); fft,
dense, sparse and geometry stay.

- `crd_add_cpu_kernels(crd-hesap-special NAME special LEVELS portable avx2)`: `kernels/special_kernels.cpp` defines
  `SpecialKernels` (`lgamma_lanczos`, `tgamma_lanczos`, whole ranges), `special_kernels()` resolves it once, and
  `lgamma_batch`/`gamma_batch` make one call per job range. No sse2 body: the core's two `crd_log1` run ten `fma`
  each, so an SSE2 body needs the exact two-lane `fma` the row already lists for the stats log core.
- `detail/lanczos_core.hpp` holds the half-plane core (`x >= 0.5`, finite) in the level namespace; it calls only
  `crd_log1`. The reflection, poles, NaN and infinities are out of line in general code (`src/lanczos.cpp`): they need
  `crd::math::sin` and `floor`, inline functions of plain `crd::math`, which a kernel unit must not compile. Both
  levels call them for the lanes outside the core, so every level gives their bits. `simd_lanczos.hpp` is deleted;
  `special.hpp` reaches no intrinsics header on GCC and Clang (new include-budget entry).
- The kernel objects define nothing outside their level namespace but the table: `nm -C` on both GCC objects (plus
  `DW.ref.__gxx_personality_v0`) and `llvm-nm` on both MSVC objects.
- `bench_bessel_batch` became `bench_special_batch` with the Lanczos rows; `bench_special_vs_refs.cpp` (Boost, no
  target builds it, REPO.3) times each level's table instead of calling the AVX2 twins.

Defects fixed on the way, in the public `lgamma`/`gamma` and the Lanczos pair alike: `lgamma(+inf)` and
`gamma(+inf)` were NaN (`inf - inf` in the Stirling and Lanczos sums), and `gamma(+-0)` was NaN; now `+inf`, `+inf`
and `+-inf`, as C's `lgamma` and `tgamma` give.

## Verification

The differential case runs every level the host executes over lengths 0 to 41 and on to 210 at four offsets, with 24
edge arguments (both half-planes, the 0.5 boundary, signed zeros, poles, subnormals, overflow, both infinities, NaN)
spaced seven apart so each meets every lane, and a guard word past the end. A new case checks the edge values of the
public and Lanczos functions. The batch moat case compares with `lanczos_lgamma`/`lanczos_tgamma`.

| Tooth | win-debug | linux-gcc-debug |
|---|---|---|
| AVX2 sum starts at the second coefficient | differential (both values) | the same |
| AVX2 lanes at +inf count as inside the half-plane | differential (both values) | the same |
| AVX2 tail writes one element past the range | differential (guard word) | the same |
| portable body takes +inf into the core | differential (lgamma) | the same |
| `lanczos_lgamma` gives -inf at the poles | edge case (4 assertions) | - |
| `gamma(-0)` loses the zero's sign | edge case | - |
| public `lgamma(+inf)` back to NaN | edge case | - |
| `lgamma_batch` calls the tgamma kernel | batch moat (every thread count) | the same |

Each restore was rebuilt and passed. Both lanes pass crd-hesap-special-tests (42 cases) and the five
`crd-hesap-special-kernels-at-<level>` arms; the include budget fails on GCC when `batch.hpp` includes
`simd/transcendental.hpp`; strict tidy sees a finding planted in the AVX2-only body; clang-format (no finding above
HEAD's), Allman and the guards are clean.

## Board

linux-gcc-release, `taskset -c 4`, seven interleaved rounds (medians), host idle. New `88f1a31c` at forced
`CRD_CPU_LEVEL`; HEAD's header-only path compiled into the same bench source with the lane's flags (`a4dff2e9`) and
with the sse2 lanes' (no ISA flags, `CRD_SIMD_TARGET=1`, `196f772f`). Arguments in [0.1, 50], ns/elem.

| Routine | portable, 4 Mi / L2 | HEAD sse2 lane | avx2, 4 Mi / L2 | HEAD avx2 | MATLAB-1T | scipy 1.17 |
|---|---|---|---|---|---|---|
| lgamma | 35.72 / 34.81 | 35.79 / 35.04 | 3.986 / 3.815 | 3.974 / 3.810 | 7.59 | 8.64 |
| tgamma | 55.88 / 55.34 | 56.83 / 56.81 | 5.108 / 4.891 | 5.091 / 4.895 | 14.52 | 14.82 |

Both levels are at HEAD parity (within 1.7%). avx2 leads MATLAB R2026a single-thread 1.90x (lgamma) and 2.84x
(tgamma). The portable level is 4.7x and 3.8x behind it: its twenty `fma` per lgamma are libm calls; that is the
two-lane `fma` item. On the full pool (32 workers, WSL) lgamma takes 0.69 ns against MATLAB-MT's 0.65 (0.94x) and
tgamma 0.85 against 1.16 (1.36x); HEAD gives the same, so the loss is older than this batch.

## Left in the row

The fft, dense, sparse and geometry kernels as per-level units first, then the rest the row lists.
