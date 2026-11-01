# 2026-11-01 — SIMD-PERF: the f32 1K and 8K forwards profiled against MKL

<!-- doc-role: session -->
> Dated session note; no live status. Current work: [ROADMAP](../ROADMAP.md#slice-simd-perf).

Row: [SIMD-PERF](../ROADMAP.md#slice-simd-perf), Left item 1 (fft's f32 forward where MKL leads), at the two
unprofiled gaps on the benchmark machine at avx512: 1K (0.82x MKL) and 8K (0.83x).
Board: [2026-11-01-simd-perf-fft-1k-8k-profile](../bench/2026-11-01-simd-perf-fft-1k-8k-profile.md).
Engine code is unchanged: the one lever tried lost and was reverted.

## CI first

Push runs on main are still cancelled by the next push. Run 41262267004 (`8a243a34`, this track's in-place
forward and its 4K plan) survived: win-vs, win-arm64-debug and linux-clang-arm64-debug passed; its Windows repository
checks failed only on `scripts/test-ci-orchestrator-e2e.py`'s end-to-end webhook case, a ci/ test this track has
not touched (the `MemoryError` above it in the log is a case of `test-ci-platform.py`, which passed). Its other lanes,
win-tidy among them, were still queued or running. No run covers `b0640d0c..c93abf76` yet.

## Profile (the bound, with its evidence)

`prof_fft_f32.sh 10` at avx512 for the planned forward (the standalone-hier, 32 x 32), MKL and the copy, with port,
load and store counts. Both are core bound on port 5, the planned forward 93% busy net of the copy and MKL 99% with
28% fewer operations. Each transform runs about 1,280 shuffles against about 2,040 vector FP operations: stage 1's
transposed store into AoSoA rows (448 a step, two steps), its deinterleave, the final leaf's two-tile block loads
(`vinsertf32x8`) and its interleave. Stage 1 also spills (1.9x MKL's stores). MKL runs 64 x 16.

At 8K (`prof_fft_f32.sh 13`) the planned standalone-hier's 128-point final leaf spills: 2.7x MKL's stores, 1.33x its
instructions; MKL runs three passes of 32, 16 and 16.

## The lever tried: in-place-3 at 1K (not kept)

`in-place-3` at n = 16·4·16 (a generated 4-point pass 2, the kernel's B = 4 case, `forward_builds` from 1K) cuts the
transposing shuffles to the four group transposes. It ran 8% fewer cycles in the loop, but at a lower clock (2.93 to
2.96 GHz against the standalone-hier's 3.18 to 3.20, whole-process cycles over task-clock), so the two were level in
time; on the warm board it lost, 0.835x the standalone-hier at avx512 [0.814-0.837] and 0.787x at avx2: pass 2 at
B = 4 reads and writes the whole buffer for a 4-point leaf (1.6x the loads), and a single transform pays the three
passes' store-to-load chains. Reverted.

## Finding: the two measures disagree at 1K and 8K

The board's best warm transform and the loop's mean order the arms oppositely at both sizes (1K: the board ranks the
standalone-hier first, the loop's cycles in-place-3; 8K: the board ranks in-place-3 first at 0.91x MKL, the loop
the standalone-hier, 15.0 against 16.9 microseconds a transform with the copy). The cycle counter does not follow
time across arms either (the clock differed by 8% at 1K). A plan change at either size waits for one measure that
settles both: the loop's per-transform distribution beside its mean, and each arm's clock.

## Checks

The candidate built under `-Werror` on the machine (linux-gcc-release), and its 1K and 8K forwards stayed within the
bound against the FFTW f64 truth (1.1e-07 and 1.3e-07); it was reverted before any commit reached main, so no engine test changed. Locally: the plan, repository and Allman
checks. The machine ran 0.28 h in one start and is stopped.

## Left

- **1K (0.85x MKL on this board at avx512):** the shuffles are the gap (about 1,280 of the 2,150 port-5
  operations, the transposition between the two 32-point passes about 900 of them); a 64 x 16 split with the 16-point pass across a register's lanes, or stage 1
  storing whole 16-column blocks so the leaf loads them without inserts, are the levers the profile names.
- **8K (0.84x):** the 128-point leaf's spills; MKL's 32 x 16 x 16 shape, and the measure that settles the
  board-against-loop split first.
- avx512 2K's two-pass shape (0.90x), ARM64's canonical 2K and 4K plans, the i9 re-board of the avx2 plan rows, the
  jobs retune's boards per level, torch and onnxruntime pinned and boarded.
