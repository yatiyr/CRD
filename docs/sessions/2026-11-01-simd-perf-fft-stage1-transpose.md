# 2026-11-01 — SIMD-PERF: the f32 1K forward's stage-1 transposed store a register a row at avx512

<!-- doc-role: session -->
> Dated session note; no live status. Current work: [ROADMAP](../ROADMAP.md#slice-simd-perf).

Row: [SIMD-PERF](../ROADMAP.md#slice-simd-perf), Left item 1 (fft's f32 forward where MKL leads), at its widest
avx512 gap: 1K (0.79x MKL on this machine). Board:
[2026-11-01-simd-perf-fft-stage1-transpose](../bench/2026-11-01-simd-perf-fft-stage1-transpose.md).

## CI and verification first

- Every push run on main is still cancelled by the next push. Manual dispatch 41269105413 (`b0b1b245`) failed ten
  Windows jobs (win-release among them), all to build on `filesystem.cpp`'s `min`/`max` macro collision or
  `checksum.cpp`'s C4324, both the other tracks' files and both fixed on main; its Linux lanes and win-tidy were
  still running and 41274380605 (`de5e10b0`) still queued. No finished Windows lane has compiled `b0640d0c`'s
  `transpose8x8_halves` yet, which this change now calls from every avx512 codelet unit: the next batch reads the
  first completed Windows lanes and win-tidy for it.
- The last batch's f32 2K to 16K plan changes, on the machine's whole linux-gcc-release tree at `f501fb16` (built
  with `-k 0`, every target): `ctest -R "dsp|stft|cwt|conv|nn|fft"`, 821 of 821 passed.

## Profile first

The [1K profile](2026-11-01-simd-perf-fft-1k-8k-profile.md) had the planned forward (the standalone-hier, 32 x 32)
port-5 bound at 93% busy, stage 1 the hottest symbol, and MKL running 64 x 16.

- **The split (not kept):** 1024 = 16 x 64 runs on existing codelets (the 16-point stage 1, the 64-point leaf), so
  it was tried first. Port 5 rose from 2,191 to 2,223 a transform, stage 1 stayed at 60% of the samples, and the
  board gave 0.920x HEAD [0.908-0.921]: stage 1's cost is its transposed store, which moves every element once at
  any split. Reverted. To board it, `ab_fft_f32.sh` now runs a candidate's named algorithms against HEAD's
  (`CRD_AB_CANDIDATE` with `CRD_AB_ARMS`, arms `cand-<name>`), the mode for a change that alters an algorithm's bits.
- **The bound:** at avx512 `store_transposed` split each group's eight `Vec16f` rows into 256-bit halves (eight
  extracts) and ran Vec8f's 8x8 transpose on each (48 shuffles): 56 port-5 operations a group, 448 a step of the
  32-point stage 1, two steps a transform, about 900 of the 1K forward's 2,190.

## The change

- **`perf(fft)`:** at avx512 `store_transposed` transposes the group's rows with `transpose8x8_halves` (both tiles'
  8x8 transposes on whole registers, 24 operations) and stores each row's halves as its two columns; GCC folds every
  high-half extract into its store (64 memory-destination `vextractf32x8` in the 32-point codelet, none to a
  register). The codelet's shuffles went from 448 to 256. Pure data movement: the same bits at every size, offset
  and level (the board's digests matched at every size of every repetition). The f64 groups (`Vec8d`) and every
  other level keep their path.
- **1K at avx512:** 1.222x HEAD aligned [1.19-1.232] and 1.200x off a line [1.193-1.237]; per transform 2,257 to
  1,766 cycles, port 5 2,190 to 1,718, 6,212 to 5,659 instructions. Against MKL: 0.947x [0.946-0.966] aligned and
  0.965x [0.948-0.966] off a line (HEAD 0.788x and 0.789x); among every offered arm in one process 0.930x and 0.938x.
- **Elsewhere:** the plans without a stage-1 group store (2K to 64K) level; the deep-split-4 (128K to 512K) and the
  tiled four-step (1M) 0.5% to 1.3% below HEAD, inside the 2% band (256K: 4.7% fewer instructions, 10% fewer port-5
  operations, 1.1% more cycles; memory bound, and 1.3x to 1.9x ahead of MKL there).
- avx2, sse2 and portable compile HEAD's path (the branch exists only where a lane is a `Vec16f`), so they were not
  boarded.

## Checks

- On the machine (linux-gcc-release, the whole tree with `-k 0`): `ctest -R "dsp|stft|cwt|conv|nn|fft|crd-check"`,
  831 of 831 (every `crd-hesap-fft-kernels-at-<level>` arm and the repository guards, `crd-check-isa-isolation`
  among them).
- Locally the plan, repository and Allman checks.
- The machine ran 0.63 h in one start and is stopped.

## Left

- The f64 edition of the lever: `Vec8d` groups still split into halves (a `transpose4x4_halves` with its bit probe,
  boarded with `bench_fft_vs_refs` on the f64 standalone-hier and hier sub-FFT sizes).
- The 0.5% to 1.3% at 128K to 1M (fewer instructions, more cycles: a memory-bound store pattern to name with
  store-buffer and fill-buffer counts).
- 1K's remaining 5% to MKL (the final leaf's two-tile block loads and the two deinterleave stages), 8K's and 16K's
  pass-3 transposition against MKL's contiguous first pass (0.89x, 0.88x), the sse2 and portable Default rows on this
  machine, 32K and 64K, avx512 2K's two-pass shape, ARM64's canonical plans on ARM hardware, the i9 re-board of the
  avx2 plan rows, the jobs retune's boards per level, torch and onnxruntime pinned and boarded.
