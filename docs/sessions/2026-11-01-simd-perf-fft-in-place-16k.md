# 2026-11-01 — SIMD-PERF: the f32 16K forward as the in-place three passes, with an aligned work buffer off a line

<!-- doc-role: session -->
> Dated session note; no live status. Current work: [ROADMAP](../ROADMAP.md#slice-simd-perf).

Row: [SIMD-PERF](../ROADMAP.md#slice-simd-perf), Left item 1 (fft's f32 forward where MKL leads): 16K, the widest
f32 gap from 1K to 64K (0.69x MKL aligned, 0.74x misaligned, the
[alignment board](../bench/2026-11-01-simd-perf-fft-alignment.md)). Board:
[2026-11-01-simd-perf-fft-in-place-16k](../bench/2026-11-01-simd-perf-fft-in-place-16k.md).

## CI first

Every push run on main is still cancelled by the next push. Run 41262267004 (8a243a34, the first in-place passes)
survives as a dispatch-like run: win-vs, win-arm64-debug, linux-gcc-relwithdebinfo, linux-clang-arm64-debug and
linux-clang-arm64-asan passed; its Windows repository checks failed in `scripts/test-ci-orchestrator-e2e.py` (the
webhook build's `plan` job left `scheduled`), the serial workflow's CI orchestrator, not this track's code; win-tidy,
the clang-cl lanes and the rest were still running when this batch ended. The manual dispatch 41269105413 (b0b1b245)
was still queued.

## Profile first (avx512, aligned, per transform with the input copy)

MKL runs 16K as three passes over the caller's buffer, leaves 16, 32 and 32 (`rowbatch_twidl_fwd_016`,
`colbatch_twidl_fwd_32`, `colbatch_plain_fwd_32`): 62,800 cycles, 98,800 instructions, 8,200 stores. The planned
IP4-AoS runs seven radix-4 passes: 89,300 cycles, 1.7x the instructions, core bound (41.6%). The out-of-place arms
stream intermediates: deep-split-3 (32·32·16) 85,700 cycles, memory bound 51%, 2.4x the stores; the standalone-hier
40% frontend bound on its 128-point bodies from the legacy decoder. The gap is the pass shape, not a kernel body.

## The change

- **In-place-3 at 16K = 16·64·16:** `gen_fft_batched.py` emits pass 2's 64-point leaf (`codelet64_inplace_mid`),
  `execute_in_place3` names each B, and `forward_builds` offers in-place-3 from 2K to 16K, so every forced-forward
  test, `bench_fft_f32` and the boards walk it. Its profile: 73,200 cycles (memory bound 55%, store-buffer stalls
  34,900 cycles against MKL's 20,700; the 64-point leaf spills 293 stack stores a step beside its 128 data stores).
  1.31x IP4-AoS at avx512 aligned and 1.09x off a line, 1.10x at avx2 aligned but 0.81x off a line.
- **The aligned work buffer:** off a 64-byte boundary every 64-byte block straddles two lines, and the three passes
  in the caller's buffer took 6,900 split loads and 6,200 split stores where MKL takes one read and one write of the
  buffer (3,100 and 2,100). From 8K (`kInPlace3WorkFrom`), a buffer off a line runs pass 1 out of place
  (`codelet16_inplace_first_to`, the same leaf and twiddles, interleaved in from the caller's buffer, blocks out to the
  plan's aligned work buffer after pass 3's 2 KiB scratch), passes 2 and 3 on the work buffer and pass 3's stores into
  the caller's. The portable level deinterleaves into the work buffer before pass 1's blocks edition. Same operations
  in the same order, so the same bits at every address (ADR-0063): 1.17x to 1.25x the in-place passes off a line at
  8K and 16K, level on one. At 4K the work buffer lost 0.68x (its 64 KiB beside 32 KiB of data leaves the L1d that
  holds the in-place passes), so 2K and 4K run in place at every address, as before.
- **The plan:** the canonical f32 16K forward is in-place-3 (`planned_forward`; avx512 runs it under Default, every
  level under the deterministic modes); avx2's Default row from the i9's board keeps IP4-AoS. With the change it is
  1.26x IP4-AoS at avx512 at both alignments, 1.08x at avx2 aligned and level off a line; **0.884x MKL aligned and
  0.912x misaligned at avx512**, from 0.686x and 0.744x. The plan scratch grows by n complex from 8K (128 KiB at 16K).
- **8K stays the standalone-hier** this batch: in-place-3 now runs 1.06x it aligned at both levels and level off a
  line at avx512, 0.96x off a line at avx2; each arm's worst cell favours in-place-3 (0.959 against 0.940), so a
  dedicated 8K board decides the plan next.

## Checks

- A new test, "the in-place three passes give the same bits at every offset of the buffer": 2K to 16K, every level
  the host runs, each 4-byte offset of a 64-byte line (both routes from 8K): within the bound against the reference,
  the guards kept, and the bits of the offset on the line. The planner test's f32 16K forward is in-place-3.
- On the machine (linux-gcc-release): `crd-hesap-fft-tests` and every `crd-hesap-fft-kernels-at-<level>` arm, 95 of
  95 passed; every board checked the FFTW f64 truth, and the route board checked the two builds' in-place-3 bits equal
  every repetition.
- The codelets regenerated on WSL, the reference host (`gen_fft_batched.py hybrid2 --check`), and their manifest
  entries refreshed; `gen_fft_level_plans.py --check`; locally the plan, repository and Allman checks.
- A follow-up fix branches pass 1 on whether the work buffer is the caller's (a 2K or 4K buffer off a line had run
  the out-of-place edition with its two buffers the same, which the shared schedule made safe but its contract did
  not cover); the fft tests passed again, 95 of 95.
- The machine ran 0.70 h in one start and 0.06 h in a second for the fix's tests, and is stopped.

## Left

- **8K (0.83x MKL aligned as planned, in-place-3 0.88x):** the in-place-3 plan on a dedicated board at both levels
  and alignments.
- **16K (0.88x):** pass 1's 38% (16 rows 8 KiB apart share an L1d set) and pass 2's 64-point spills; MKL's 16·32·32
  shape (a 32-point pass 3) keeps every leaf at 32.
- **1K (0.79x):** the shuffles between the 32 x 32 passes.
- 32K and 64K (0.83x and 0.76x aligned), avx512 2K's two-pass shape, ARM64's canonical 2K and 4K plans, the i9
  re-board of the avx2 plan rows, the jobs retune's boards per level, torch and onnxruntime pinned and boarded.
