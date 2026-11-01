# 2026-11-01 — SIMD-PERF: the in-place forward's pass-3 group transpose at avx512

<!-- doc-role: session -->
> Dated session note; no live status. Current work: [ROADMAP](../ROADMAP.md#slice-simd-perf).

Row: [SIMD-PERF](../ROADMAP.md#slice-simd-perf), Left item 1 (fft's f32 forward where MKL leads), at the widest
planned gap on the benchmark machine: the f32 2K forward at 0.80x MKL at avx512, then 4K level with it.
Board: [2026-11-01-simd-perf-fft-group-transpose](../bench/2026-11-01-simd-perf-fft-group-transpose.md).

## CI first

No completed run covers this track's commits since `58e6399d`: push runs on main are still cancelled by the next
push, and the mirror's run for `95ec6965` was pending. Dispatch 41254049879 at `31404ec4` had failed two lanes,
linux-gcc-shipping and linux-gcc-release, both on GCC's LTO `-Werror=null-dereference` at
`tests/foundation/compression/test_deflate.cpp:755-756` (another track's file, fixed on main by `1e689d55` and
`95ec6965`); its `win-tidy` lane was still running, so the avx2 and avx512 nn units and the in-place codelets still
have no clang-tidy result.

## Profile (the bound, with its evidence)

`prof_fft_f32.sh 11` and `12` at avx512 for in-place-3 and MKL, with `uops_dispatched.port_*` counted. Port 5 is the
bound: 88% busy at 2K (4,827 uops a transform over 5,467 cycles) against MKL's 76%; port 0 57%, port 1 9%. While
512-bit operations are in flight port 1 takes no vector operation, so every shuffle, the 256-bit ones of the group
transpose included, and half the 512-bit FMAs share port 5. The 2x loads against MKL are a symptom, not the bound
(the load ports are 28% busy). The group transpose (eight `Vec8f` 8 x 8 tiles, 192 shuffles a group) held 24.8% of the
2K samples and 19.5% at 4K.

## The change

- **`transpose8x8_halves`** (`vec16f.hpp`): Vec8f's 8 x 8 transpose on both halves of eight `Vec16f` rows at once; at
  avx512 the unpack and in-lane shuffle stages on whole registers and the 128-bit lane stage as one merge-masked
  `shuffle_f32x4` a row, 24 operations for both tiles. The SIMD-type probe holds it to portable's bits on every
  level (`vec16f transpose8x8 halves`).
- **The group transpose at avx512** (`fft_kernels.cpp`): an AoSoA block row (8 real then 8 imaginary parts) is one
  register, so both parts of a tile transpose together, 96 shuffles a group, and each transposed row is stored whole
  into the unchanged scratch. Other levels keep the `Vec8f` tiles.
- **Measured and not kept:** split scratch rows (16 real, 16 imaginary parts) loaded whole by pass 3 (a generator
  change) saved pass 3's inserts but its whole loads spanned two half stores and could not forward
  (`ld_blocks.store_forward` 289 a 2K transform, about 1.6 kept); the kept edition ran 2.0% to 2.6% fewer cycles in
  four alternating profile rounds.
- **Result:** at avx512 1.129x HEAD at 2K [1.113-1.151] and 1.123x at 4K [1.103-1.134], 8K level (its plan is the
  standalone-hier), avx2 level at both sizes (its code unchanged), the same bits at every size of every repetition.
  The planned forward against MKL at avx512: 0.90x at 2K (was 0.80x), 1.12x at 4K (was 1.00x). In the profile loop
  2K takes 4,847 cycles (was 5,467) and 4K 11,928 (was 13,304).

## Checks

On the machine (linux-gcc-release, no warning under `-Werror`): `crd-hesap-fft-tests` (52 cases) at forced portable,
sse2, avx2 and avx512, all passing; `crd-math-tests` `[simd],[kernels]` (88 cases) and the probe case "every level
the host executes gives the portable level's bits", passing. Locally: the plan, repository, generated-source and
Allman checks.
The machine ran 0.48 h in two starts and is stopped.

## Left

- **avx512 2K, the rest to MKL (0.90x):** MKL runs 2K as two passes of 64 x 32 (`rowbatch_twidl_fwd_064` 57% of its
  samples, `colbatch_plain_fwd_32` 20%) against in-place-3's three of 16 x 8 x 16, at 1.28x its instructions and
  1.77x its loads, still holding port 5 89% busy: a two-pass 2K with a larger leaf is the structural lever, and pass
  1's deinterleave and pass 3's interleave (two `permutex2var` a value each, port 5) the local one.
- **1K:** the standalone-hier at 0.82x MKL at avx512 and 0.63x at avx2, not yet profiled.
- **8K:** in-place-3 level with the standalone-hier at avx512 on the warm board's minimum; the plan stays.
- **ARM64:** the canonical 2K and 4K plans (in-place-3) are unmeasured on ARM hardware.
- The avx2 per-level rows at 2K to 8K with the i9 re-board, the jobs retune's boards per level, torch and onnxruntime
  pinned and boarded.
