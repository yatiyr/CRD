# 2026-10-30 — SIMD-DISPATCH: fft codelet frames under MSVC's optimization limit

<!-- doc-role: session -->
> Dated session note; no live status. Current work: [ROADMAP](../ROADMAP.md#slice-simd-dispatch).

Row: [SIMD-DISPATCH](../ROADMAP.md#slice-simd-dispatch). Before:
[glibc's software fma](2026-10-30-simd-dispatch-glibc-soft-fma.md).

## CI first

- Push run 41021189146 (4ab1cd90, in progress, not cancelled) failed win-shipping, linux-gcc-release and
  linux-gcc-shipping. All three failed the sparse LinearOps' picks case on NaN payloads; 530d0f4e already compares
  every site of it with payloads aside (its real f64 lines 156, 157 and 166 included).
- win-shipping also failed five fft cases at avx2 and avx512 (the batched paths per column, the f32 forwards from 1K
  to 64K and the two `[kernels]` arms) on `SIGSEGV - Stack overflow`, reproduced in the local win-shipping tree.
- No completed run has another failure. Needs CI audit: DIAG.11c and DIAG.12 still wait on a green complete-tier
  run; the win-asan green run of the guard is not in yet (41021189146's win-asan is still running).

## Cause

cdb at the overflow: `codelet256_batched_part0<Vec4d>` asked `__chkstk` for 0x15A9B0 bytes (1.35 MB) of the 1 MB
main stack. Prologues of every whole codelet (d8475439) in the shipping binary:

| Whole body | Frame |
| --- | --- |
| every codelet of at most 3,594 lines | 0.5 to 33 KB |
| N=128 stage-1 fused sh_csf, 3,787 and 3,851 lines | 0.87 to 1.77 MB |
| the eight N=256 codelets, 6,407 to 8,459 lines | 1.38 to 3.93 MB (avx512 the larger) |

cl stops optimizing a function past a size and gives each value its own slot; it says so only through the
off-by-default C4883 (`/w14883` on unit 4 at avx2 named both of its large codelets). GCC and Clang are unaffected.
`/d2OptimizeHugeFunctions` lifts the limit, but unit 4 had not finished after 18 min at 2.8 GB against 136 s without
it, so it was stopped and not kept.

## Fix

- `fft_codegen_split.py`: the whole edition holds each codelet in parts of at most `WHOLE_BUDGET` (3,000) lines,
  the parts' slot loads and stores counted (a cut of an N=256 codelet carries about 500 values). 71 of 85 codelets
  stay whole; the N=256 ones take 3 or 4 parts and six N=128 ones 2, the largest part 3,006 lines. The split edition
  and the hierarchical and twiddle units are byte-identical; the batched units were regenerated on WSL.
- `CrdSimd.cmake`: every cl kernel unit builds with `/we4883`, so no kernel function past the limit compiles.
- Same operations in the same order, so every level keeps its bits.

## Checks

win-shipping crd-hesap-fft-tests rebuilt with `/we4883` on its kernel units: the largest frame is now 26 KB at avx2
(a part) and 49 KB at avx512 (a driver's slot array); the three failing cases, `[kernels]` at a forced avx2 and all
45 cases passed. win-debug built. Generators `--check`, check-generated, the tooling tests (the splitter's case: a
part bounded with its slots; a budget no part fits refused), Allman braces, the plan and repository checks.

## Left

- The fft avx2 and avx512 columns re-run on this edition in the next board session (the f64 acceptance measured
  unsplit bodies under GCC).
