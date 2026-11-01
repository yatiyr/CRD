# 2026-11-01 — SIMD-PERF: the in-place forward's plans at every level, pass 1's twiddles as one table, and the f32 8K plan

<!-- doc-role: session -->
> Dated session note; no live status. Current work: [ROADMAP](../ROADMAP.md#slice-simd-perf).

Row: [SIMD-PERF](../ROADMAP.md#slice-simd-perf), Left item 1 (fft's f32 forward): the 8K plan board the
[16K session](2026-11-01-simd-perf-fft-in-place-16k.md) named, widened to every level the canonical plan runs on.
Board: [2026-11-01-simd-perf-fft-pass1-table](../bench/2026-11-01-simd-perf-fft-pass1-table.md).

## CI first

Every push run on main is still cancelled by the next push; nothing covering this track's last batch completed. Run
41262267004 (8a243a34) finished most lanes, none failing in this track's code: linux-gcc-release and
linux-gcc-shipping on `test_deflate.cpp`'s LTO null-dereference warning (fixed on main by 95ec6965), win-tidy on
`test_arguments.cpp`'s raw-literal findings (fixed by a40af1bf), win-shipping's and the Windows repository checks'
`test-ci-orchestrator-e2e.py` webhook case and win-release's diagnostic-host wild-write case (the serial workflow's
rows). The manual dispatches 41269105413 and 41274380605 were still queued.

## Profile first

- **The lower levels had never been boarded.** The canonical plan runs under the deterministic modes on every level,
  and the 2K, 4K and 16K flips to in-place-3 were judged at avx512 and avx2 only. On the sse2 unit (which an sse4.2
  selection runs) in-place-3 lost to the plans it replaced by 6% at 2K and 4% at 4K, and at portable by 7% at 2K
  and 12% at 16K: a regression of this track's own, found here.
- **Why:** on the sse2 unit pass 1 was 61% of the 2K forward and 47% of the 8K one. Pass 1 is the only pass that
  forms its twiddles, W_n^{t·ka} from the factored roots (two products and two fused operations an output beside the
  multiply's two), and the sse2 unit computes each fused operation exactly from dozens of SSE2 instructions.
- **8K at avx512:** in-place-3 35,600 cycles a transform aligned against MKL's 30,900 (1.29x MKL's instructions;
  pass 1 37%, the pass-3 transposition 9%, 22% off a line); MKL runs 32·16·16 with its first pass over contiguous
  rows of 32 and no separate transposition.

## The change

- **Pass 1's twiddles as one table** (`perf(fft)`): the plan also holds the products, 15 rows of the 16B columns
  (15 KiB at 2K to 120 KiB at 16K), each rounded as the lane rounds it (a rounded product, then one fused
  operation), so loading them gives the bits the lane computed. The generator emits `_tw` editions of pass 1 beside
  the factored ones; the sse2 unit and the portable level load the table at every size, avx2, avx512 and NEON at 2K
  only (`kInPlace3TabledTo`): there the table and the data share the L1d, while at 4K the table lost 0.956x at
  avx512 off a line and from 8K the two were level. The bits are unchanged at every size, level and offset (every
  board repetition compared them). sse2 1.26x to 1.38x HEAD's in-place-3, portable 1.19x to 1.28x, avx512 2K 1.05x
  aligned, avx2 2K 1.07x, the rest level; in-place-3 now leads every other arm at every size on the sse2 unit and
  the portable level, so the flips gain there too (sse2 1.30x to 1.37x the plans they replaced).
- **The f32 8K plan** (`perf(fft)`): in-place-3, 1.06x the standalone-hier at avx512 and 1.08x at avx2 aligned,
  level off a line, 1.15x on the sse2 unit and 1.40x at portable with the table. avx512 runs it under Default:
  0.894x MKL aligned and 0.914x off a line, from 0.843x and 0.907x. avx2's Default row keeps the standalone-hier.
  The planner's f32 8K exclusion from the deep-split, unreachable now, went.
- **`prof_fft_f32.sh`** (`fix(bench)`): a side's files carry the offset (`-off16`), so two offsets' runs into one
  directory no longer overwrite each other (this batch's first 8K profile lost its aligned files that way).
- **A main breakage on the way** (`fix(tests)`): the whole linux-gcc-release tree stopped at `crd-hesap-stats-tests`'
  link, GCC 13's LTO flagging `test_multivariate.cpp`'s sample buffers as maybe uninitialized (rvs writes all k,
  which LTO cannot see); they are value-initialized.

## Checks

- On the machine (linux-gcc-release): the fft suite, 146 of 146 (every `crd-hesap-fft-kernels-at-<level>` arm and the
  new test that the table holds the factored products bit for bit); the multivariate cases, 9 of 9; the whole tree
  built with `-k 0` and the 20 repository guards passed, `crd-check-isa-isolation` among them (a partly built tree
  had failed it on other targets' missing objects).
- The codelets regenerated on WSL, the reference host (`gen_fft_batched.py hybrid2 --check` clean), their manifest
  entries refreshed (`check-generated.py`); locally the plan, repository and Allman checks.
- The machine ran 1.53 h in one start and is stopped.

## Left

- **The sse2 and portable Default rows:** Stockham leads in-place-3 on the sse2 unit at every size from 2K to 16K
  (1.03x to 1.34x) while the i9's rows plan the four-step at 2K and 8K (in-place-3 1.09x to 1.11x it now): a re-board
  of those rows on this machine.
- **8K and 16K against MKL (0.89x, 0.88x):** MKL's first pass over contiguous rows of 32 with no separate
  transposition (32·16·16 at 8K, 16·32·32 at 16K) against the pass-3 group transposition (9% aligned, 22% off a line).
- 1K's transposition (0.79x), 32K and 64K, avx512 2K's two-pass shape, ARM64's canonical 2K to 16K plans on ARM
  hardware, the i9 re-board of the avx2 plan rows, the jobs retune's boards per level, torch and onnxruntime pinned
  and boarded.
