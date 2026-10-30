# 2026-10-30 — SIMD-DISPATCH: the fft board's method, board 2, and the tiled four-step at f32 1M

<!-- doc-role: session -->
> Dated session note; no live status. Current work: [ROADMAP](../ROADMAP.md#slice-simd-dispatch).

Row: [SIMD-DISPATCH](../ROADMAP.md#slice-simd-dispatch) (the boards; fft's Left: f32 64K and 512K, the forced levels'
method). Before: [push run 40993784639's fixes](2026-10-30-simd-dispatch-nan-aside-arms-snapshot-dumps.md).
Board: [fft, board 2](../bench/2026-10-30-simd-dispatch-fft-board.md#board-2-one-program-on-both-engines).

## CI first

No completed run has a failed job. Push run 41021189146 (4ab1cd90) was in progress with no failed job, 41021307508
pending; the cancelled runs before them list no jobs. Nothing cancelled or dispatched. Needs CI audit: DIAG.11c and
DIAG.12 still wait on a green complete-tier run, and SIMD-DISPATCH on a green win-asan run; nothing flips.

## The f32 gap was the board's method

HEAD's `bench_fft_f32` timed `reset()`, the input copy, inside each transform's timing (Cerid's and MKL's);
pre-row's f32 program copied outside it. MKL lost the same 0.007 ms at 64K and 0.13 ms at 512K between the two
programs. Pre-row's f32 program built against HEAD's lane tree ran 64K at 1.00 and 512K at 0.95 of pre-row. Both
fft programs now copy before each timed call (`time_best_ms` takes the untimed step in `bench_fft_vs_refs`).

Board 2 runs pre-row's f32 and f64 programs on both engines, then HEAD's programs at every level, five passes. Every
avx2 range overlaps pre-row's; f32 and f64 are accepted at avx2.

## The forced levels' method

- **MKL** runs AVX2 on this host (`MKL_VERBOSE`), not AVX-512 as the first board said, and a peer at the host's ISA
  at every forced level is ADR-0134 §8's method. Not a defect.
- **glibc:** `run-lane-bench.sh` hides the features above a forced sse4.2 or sse2 level from glibc's ifuncs
  (`GLIBC_TUNABLES=glibc.cpu.hwcaps=...`, the hidden list printed in the level's header line; checked with
  `ld.so --list-diagnostics`: the leaf-1 and leaf-7 active bits drop, no glibc-hwcaps library directory exists, so
  the peers' libraries do not change). fft's sse4.2 and sse2 times did not move. Portable keeps the host's glibc:
  hidden, its f32 4M forward took 3,357 ms against 109 (glibc's software `fma`) on a machine no build targets.
  ADR-0134 §8 records it.

## f32 1M: the tiled four-step

The user's named target. On the board's one binary the tiled four-step arm ran 2.105 ms against the planned
deep-split's 2.554 at avx2 and within 1% to 2% of it at sse4.2, sse2 and portable. `planned_forward<f32>(1M)` now
returns it on every level, as its 2026-10-29 note planned ("the winner becomes the planner's only path"); the
cross-level bits stay (its leaves run on the lanes). One run after: 1.955 ms, MKL 1.857 (0.95x; before, 0.73x).

## Checks

win-debug: crd-hesap-fft-tests built; the planner table, every band (f32), the forced forwards 128K to 1M, the
specials and the f32 four-step oracle cases passed (2,258 assertions). linux-gcc-release: both fft programs and
`run-lane-bench.sh` at detected and sse2 (the level lines and the hidden list printed). Tidy, clang-format, Allman
braces, the ctest guards, the plan and repository checks.

## Left

- fft: the user's decision on one plan for every level (above).
- The other families' boards.

Follow-up for an extra batch: on GCC and Clang below avx2, `libm::fma` calls glibc's `fma`, whose software body a CPU
without FMA runs (portable f32 4M 30x slower with FMA hidden); MSVC's soft path is crd's own `detail::fma_f64`.
Measure both under the sse2 mask and use the faster on every compiler.
