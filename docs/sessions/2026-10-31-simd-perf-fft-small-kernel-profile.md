# 2026-10-31 — SIMD-PERF: the f32 forward's 2K to 16K kernel against MKL, profiled

<!-- doc-role: session -->
> Dated session note; no live status. Current work: [ROADMAP](../ROADMAP.md#slice-simd-perf).

Row: [SIMD-PERF](../ROADMAP.md#slice-simd-perf), Left item 1 (fft's f32 forward where MKL leads), at its worst
remaining peer gap: after [the four-step's whole-line stores](2026-10-31-simd-perf-fft-four-step-lines.md) the 2M and
4M forwards sit at 0.97x to 1.18x of MKL, while 1K to 64K sat at 0.44x to 0.89x
([first board](../bench/2026-10-31-simd-perf-fft-f32-xeon.md)). Board:
[2026-10-31-simd-perf-fft-small-kernel-profile](../bench/2026-10-31-simd-perf-fft-small-kernel-profile.md).

## CI first

No mirror run has reported on this track's commits: push run 41220996700 (`d91c77f1`) was pending without jobs, the
earlier pushes were cancelled by later ones. The last run with results, 41204961252 at `06f099ce` (before this
track's commits), failed every build lane on three tests, none from this track: `crd-check-isa-isolation` (crd-math's
scalar table lambdas outside the level namespace, fixed on main by `bcaed0e1`), `crd-generated-sources` and
`crd-repository-tools`'s provenance case (both the fft codelet headers' stale manifest hashes, current on main since).
`crd-repository-tools` then failed on main for another cause: `5eea2fa9` registered `crd-fuzz-net-negotiation`
without a `scripts/fuzz.py` TARGETS entry. This session's fix met main's own at the rebase (`d476d628` added the same
entry beside crd-crypto's), so main's stands and this one was dropped.

## Profile (the bound, with its evidence)

`prof_fft_f32.sh` at 2K, 4K, 8K and 16K, every side its own process (planned, standalone-hier, ip4, Stockham, MKL,
the copy), avx512 and avx2, with the decoder, store and L1 events; `perf record` on the store-buffer, RFO and L1
replacement events at 4K; the codelet bodies' stores counted from the disassembly. Counts per transform net of the
copy:

- **4K (0.74x MKL):** the bound is the out-of-place structure's working set. standalone-hier streams the input, its
  AoSoA intermediate and the full stage-1 twiddle table, 96 KiB through a 48 KiB L1d; it issues 1,434 RFOs for 1,024
  written lines, and stage 2's stores to the caller's lines, queued behind 285 spill stores a step, stall the store
  buffer for 4,122 of its 15,233 cycles (MKL 11; stage 2 holds 54.5% of the stall samples). The radix-64 bodies spill
  two stack stores per data store at avx512 (313 to 407 per stage-1 step against 128 data stores) and run from the
  legacy decoder (MITE 24,976 of 28,522 uops). MKL runs three in-place passes over the 32 KiB data (32, 16 and 8
  points): 1,869 stores against 4,568, 21,828 instructions against 27,894.
- **2K (the plan):** standalone-hier is level with MKL here (its 48 KiB working set fits the L1; MKL runs the same
  64 x 32 split), but the planned forward is ip4, at 1.46x its cycles (avx512) and 1.34x (avx2).
- **8K:** standalone-hier level with MKL at avx512 (both out of the L1, MKL 65% memory bound); 0.87x at avx2.
- **16K:** the planned ip4 level with MKL in cycles; standalone-hier frontend bound (27%).

## Levers tried (each reverted)

Three candidates, each from the profile, each boarded against HEAD's standalone-hier in one interleaved board:

- **deep-split-3 at 1K to 4K** (16·8·8, 16·16·8, 16·16·16, whose radix-16 and radix-8 bodies spill 0 to 30 stores a
  step): avx512 2K 1.027, 4K 0.995; avx2 2K 1.039, 4K 0.963. The third pass costs what the smaller bodies save.
- **The factored stage-1 twiddle from n2 = 64** (the 4K table 10 KiB in place of 32 KiB, other bits): 4K avx512 1.067,
  every other cell level, unresolved or the other way (2K avx512 0.969), and the i9 had measured it 3% to 6% slower at
  these n2.
- **Stage 2's destination lines prefetched before each step** (the same bits): 4K avx512 1.066, avx2 0.959.

None closes more than a sixth of the 4K gap, two split by level, one changes the bits; none is kept. Intervals in the
board.

## The 2K plan

standalone-hier runs the 2K forward 1.3x faster than the planned ip4 at both levels on this machine. The i9's July
A/B chose ip4 there by 12% (`2026-07-03-v10-fft-remeasure-and-midband.md`), and its October arms
(`2026-10-30-simd-dispatch-fft-f32-arms.json`) hold 0.001 ms for both at avx2: `bench_fft_f32` printed three
decimals of a millisecond at that board, so its data cannot rank 1K to 8K (it prints five since `3e596ed5`; the data
file is the logs' values, not a rounding of the generator). The plan stays under the machine-dependent plan item until
an i9 board at that resolution exists.

## Checks

No engine change landed. On the machine: the lane tree's `bench_fft_f32` at HEAD and three experimental builds, every
side passing the ADR-0092 §5 class against FFTW's f64 truth; the prefetch build printed HEAD's digest at every 4K
repetition (36 of 36). Locally, on the rebased tree: `test-repository-tools.py` (198 cases, all but the generated
model case, which needs Ninja and this shell has none, as before), the plan, repository and Allman checks. The machine ran 0.52 h in one start and is stopped.

## Left

- The in-place f32 forward for 2K to 8K (the 4K gap's lever): a row pass whose lanes are rows through the register
  transpose (`transpose_rows`), twiddled, then column-batched passes, every table a few KiB and no n-sized
  intermediate, bodies that keep their points in registers (radix 16 at avx512 and radix 8 at avx2, from the spill
  counts). Generator work in `gen_fft_batched.py`, an `FftAlgorithm` of its own, boarded against standalone-hier and
  MKL at every level.
- The 2K plan (above), with the machine-dependent plan item.
- The sse2 f64 forward at 1M and 2M (4.9x and 6.0x its own inverse), then the four-step's P2 at 2M, the 4M
  six-step against the four-step, the cnn rows against pre-row and the jobs retune's boards.
