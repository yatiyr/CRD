# 2026-11-01 — SIMD-PERF: the f32 4K forward as three in-place passes

<!-- doc-role: session -->
> Dated session note; no live status. Current work: [ROADMAP](../ROADMAP.md#slice-simd-perf).

Row: [SIMD-PERF](../ROADMAP.md#slice-simd-perf), Left item 1 (fft's f32 forward where MKL leads), at the row's widest
peer gap: the f32 4K forward at 0.73x MKL at avx512 and 0.69x at avx2 ([wide-rows
session](2026-11-01-simd-perf-cnn-wide-rows.md)), whose bound the [2K to 16K
profile](2026-10-31-simd-perf-fft-small-kernel-profile.md) had named: the out-of-place working set.
Board: [2026-11-01-simd-perf-fft-in-place-4k](../bench/2026-11-01-simd-perf-fft-in-place-4k.md).

## CI first

No push run has reported on this track's commits since `58e6399d`: every push run from 41246085460 on was cancelled
by a later push while queued, and 41257137149 (`590071f1`) was pending without jobs. The one run with results,
41247045494 (a dispatch at `67977fda`), failed seven Windows lanes, `win-tidy` among them, on one cause outside this
track: MSVC's C2797 in `tests/tools/ceridc/test_ceridc_screen.cpp` (the serial workflow's file, fixed on main by its
`fix(ceridc)` commit). `win-tidy` stopped on that unit before the nn kernel units, so this track's avx2 and avx512 nn
code (`4e5f41f0` to `590071f1`) still has no clang-tidy result; 41257137149, which holds the fix, is the run to read.

## Profile (the bound, with its evidence)

From the 2K to 16K profile, re-read on this start (`prof_fft_f32.sh 12`, net of the copy side): at 4K the planned
standalone-hier streams the 32 KiB input, a 32 KiB AoSoA intermediate and the 32 KiB full stage-1 twiddle table
through the 48 KiB L1d, memory bound 45.9% of the slots at avx512 (16,988 cycles a transform against MKL's 10,114),
and its radix-64 bodies spill two stack stores a data store. MKL runs 4K as three in-place passes over the 32 KiB
data. The lever: three in-place passes of radix 16, every table a few KiB, no n-sized intermediate.

## The change

- **`FftAlgorithm::InPlace3`** (`in-place-3`): the f32 forward of n = 16·B·16 (B 8, 16, 32: 2K, 4K, 8K), offered by
  `forward_builds` there, so every forced-forward test, `bench_fft_f32` and the boards walk it. x[i1 + 16 i2 + 16B i3]
  becomes X[ka + 16 kb + 16B kc]: pass 1 the 16-point leaf over i3 at column t = i1 + 16 i2, twiddled by the
  factored W_n^{t·ka} (lo over t & 15 and hi over t >> 4, 2 KiB and B/8 KiB); pass 2 the B-point leaf over i2,
  twiddled by W_{16B}^{i1·kb}; both store where they loaded, as AoSoA blocks (a tile of 8 complex and its block are
  the same 64 bytes). Pass 3 takes each kb's 16 x 16 group, transposed by the engine into a 2 KiB scratch, and stores
  its 16-point leaf over i1 interleaved into the group's own elements. The data stays in the caller's buffer.
- **The generator:** `gen_fft_batched.py` emits the four in-place codelets (pass 1 interleaved in and its blocks-in
  edition, pass 2 at B 8, 16 and 32, pass 3) through one `_emit_leaf` that the tiled four-step's leaves now share; it
  asserts that no store of an in-place step precedes its last load (every DFT output reads every input). Regenerated
  on WSL, the reference host, with the manifest refreshed.
- **A column a step:** the portable level and unoptimized builds run a codelet a column at a time, and a column's
  block floats are other columns' interleaved floats, so an in-place deinterleave is safe only for a step of whole
  tiles. Where `codelet_lanes.hpp`'s tile lane is a scalar the engine deinterleaves the buffer a tile at a time and
  runs pass 1's blocks-in edition; elsewhere pass 1 deinterleaves as it loads. Two earlier cuts: one converted the
  layout in the codelets and transposed each group in place, which the portable level failed (a relative error of
  1.1e3 at 8K, its column steps overwriting unread neighbours); the next ran the separate deinterleave pass on every
  level, which with the group transposes held 30% of the avx512 samples.
- **The plan builder** shares `build_factored_stage1` (the standalone-hier's and deep-split's factored table, the
  same bits: board 2) and `build_notr_twiddles` (the deep-split's last notr table) with the new builder.
- **The planner:** the canonical f32 4K forward is now in-place-3 (avx512 runs it: the per-level table has no avx512
  row). The avx2 row of the i9's table keeps the standalone-hier at 4K and 8K: its data has no in-place-3 arm and its
  0.001 ms resolution cannot rank them.
- **`ab_fft_f32.sh`:** `CRD_AB_ARMS` names algorithms of one build timed in one process a repetition (the full
  board), with `CRD_AB_REFERENCE`; `bench_fft_f32`'s header lists the new arm.

## Results (board)

- **4K avx512:** 1.38x the standalone-hier [1.24-1.39], level with MKL (0.985x, 0.95x to 1.02x; was 0.73x); 10,328
  cycles a transform against MKL's 10,114, memory bound 16.5% (45.9%).
- **4K avx2:** 1.17x the standalone-hier [1.16-1.18], 0.75x MKL (0.64x for the standalone-hier on this board).
- **8K:** level at avx512, 1.10x at avx2; **2K:** the standalone-hier 1.10x and 1.16x faster (its working set fits the
  L1d), so neither plan changes.
- **Planned forwards elsewhere:** the same digests in HEAD and the candidate at every other size at both levels, all
  level.

## Checks

On the machine (linux-gcc-release, no warning under `-Werror`): `crd-hesap-fft-tests` with the fft, dsp and wavelet
fatal cases (94), `crd-hesap-dsp-tests`, `crd-hesap-comms-tests` and `crd-hesap-wavelet-tests`, all passing; the
forced in-place-3 forward bit-identical at portable, sse2, avx2 and avx512 and within the f32 class. Locally: the
generated-source manifest, the plan, repository and Allman checks. The machine ran 0.66 h in one start and is
stopped.

## Left

- **avx512 4K, the rest to MKL:** 1.32x MKL's instructions and 2.6x its stores; the group transposes (eight `Vec8f`
  8 x 8 transposes a group, 22% of the samples) and the radix-16 bodies' spills. Levers to profile: avx512's
  `transpose16x16`, and at avx512, where one step holds a whole group, pass 3 loading the group transposed in
  registers.
- **avx2 4K and 8K (0.75x and 0.65x MKL):** every radix-16 body spills on 16 `ymm`; a four-pass radix-8 edition is
  the avx2 candidate, boarded against in-place-3.
- **The avx2 plan rows at 4K and 8K:** in-place-3 runs 1.17x and 1.10x the standalone-hier on this machine's avx2;
  the per-level table comes from the i9's board, which needs a re-run with the new arm (the machine-dependent plan
  item, with the 2K plan).
- The jobs retune's boards per level; torch and onnxruntime pinned and boarded against the mlp and cnn rows.
