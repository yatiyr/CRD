# 2026-10-28 — SIMD-DISPATCH: f64 and complex permutes on the per-level kernels

<!-- doc-role: session -->
Row: [SIMD-DISPATCH](../ROADMAP.md#slice-simd-dispatch). Decision: [ADR-0134](../decisions/0134-runtime-cpu-dispatch.md).
HEAD before: `59c79f2f`.

## CI first

- Push runs 40581144513 (`59c79f2f`) and 40580987094 (`08da3328`), complete run 40578927592 and push run 40576452112
  failed in Preflight with every job unstarted. GitHub's annotation: "recent account payments have failed or your
  spending limit needs to be increased". No job ran, so there is no log to fix from. The account owner must fix the
  Actions billing; then rerun 40578927592 and the latest push run.
- Billing then came back: attempt 2 of 40581144513 ran, and every GCC and Clang lane (linux-gcc-*, linux-clang-tsan,
  win-clang-cl, win-clang-cl-shipping) failed to compile `cholesky.cpp`: `syrk_microkernel.hpp` and the two
  `supernodal_cholesky.cpp` merges built `GemmBlock` without the `beta` that `fold beta into the first GEMM depth
  group` added (`-Wmissing-field-initializers` / `-Wmissing-designated-field-initializers`; MSVC has no such
  warning). Fixed by giving them `.beta = T{}`; the local clang-cl tree builds crd-hesap-dense, crd-hesap-direct and
  both tensor test programs, whose `[kernels]` and `[permute]` cases pass there too.
- Needs CI audit: no new hosted evidence; DIAG.11c and DIAG.12 stay Needs CI.

## What changed

- The permute kernel requests are per scalar: `PermuteRegion<T>` and `PermuteWalk<T>` (T f32 or f64) carry
  `complex` and alpha's (re, im) parts; the table gains `permute_region_f64` and `permute_walk_f64`. General code
  routes f32, f64, `Complex<f32>` and `Complex<f64>` to them (`PermuteKernelElem`, through a forward declaration of
  `Complex`, so `permute.hpp` includes nothing new) and keeps the scalar tiles for every other type.
- One tile template per element width (4, 8, 16 bytes): the micro transpose is a register-sized square of 32-bit,
  64-bit or 128-bit lanes (avx2 8x8, 4x4 and 2x2; sse2 4x4, 2x2 and one element), the shuffles move bits only, and
  `kBlockRows` is one 64-byte source line per column for every width. The unscaled copy is width-generic.
- Scales: a real element is one multiply; a complex one is `Complex<T>::operator*`'s two products then a true
  subtract (re) and add (im): `addsub` on avx2, both and a lane select on sse2 (an xor-negate then add would flip a
  propagated NaN's sign). The scalar tiles (edges, non-unit strides, portable) run the same operations.
- Stream gate: any kernel element whose dst-fastest row is a multiple of 32 bytes (it was f32 rows of a multiple of
  8), so DRAM-sized multithreaded f64 and complex permutes take non-temporal stores. The tile's near-column test uses
  `|sb|` (a flipped b counted as near).
- No engine code permutes a complex tensor today (`git grep`); decomp, einsum and the CLI permute f64, which now runs
  the vector tiles.

## Tests (hosted CI runs the forced arms)

- `[kernels]`: the region and walk differentials run for f32, f64, `Complex<f32>` and `Complex<f64>` against a scalar
  oracle (`Complex<T>::operator*` for complex), with sNaN payloads, sentinels, a misaligned base, real and complex
  scales (signed zeros, a pure imaginary, infinite over signed zeros) and two tiles below the 16-byte micro square.
  Bits match exactly, except that in a scaled complex product two NaNs agree (the commutative add may meet two NaNs,
  whose payload the compiler's operand order picks; ADR-0134 §7).
- `[permute]`: the 2-D corpus, every order of the rank-4 tiled shapes (scaled too) and the fused-alpha cases gain
  both complex types; a new MT case checks f64 and both complex types at 2, 3, 7 and 8 workers against serial, on a
  transpose the stream gate admits and a reversal whose dst rows are 16 bytes past a 32-byte multiple.
- Local, win-debug: `[kernels]` 13 cases and `[permute]` 14 cases pass; `[permute]` also under forced sse2 and
  portable.

## Board candidates (row-close pass)

The 8- and 16-byte tile edges and MT super-block element counts (sized for f32), non-temporal stores for f64 and
complex, and whether the sse2 16-byte body (no in-register lever) beats the scalar tile.
