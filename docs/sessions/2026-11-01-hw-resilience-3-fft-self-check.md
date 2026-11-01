# 2026-11-01 — HW-RESILIENCE.3: known-answer self-check of hesap-fft

<!-- doc-role: session -->
> Dated session note; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [HW-RESILIENCE.3](../ROADMAP.md#slice-hw-resilience.3). Before:
[the dense self-check](2026-11-01-hw-resilience-3-dense-self-check.md).

## CI read

- Completed runs: push runs up to 41286401310 were cancelled by concurrency and have no failed jobs.
- Running complete-tier run 41274380605 (`de5e10b0`) failed five jobs. win-tidy (`kUnixNs`, 4b9bc837) and the GCC
  release and shipping links (`test_multivariate.cpp`'s `o[3]`, 76330344) were fixed on main already.
  win-clang-cl and win-clang-cl-shipping failed to link `crd-perf-tests`: the divide-fault sweep of
  `test_instruction_access.cpp` (1f053102) divides 128-bit integers, which clang-cl allows (`__SIZEOF_INT128__`)
  but leaves `__udivti3` and `__divti3` unresolved, since it links no compiler runtime. 41269105413 shows the same
  two failures. Fixed in 7cfed26c: on clang-cl every image links the builtins library the compiler names for itself
  (`/clang:-print-libgcc-file-name /clang:--rtlib=compiler-rt`), and configure fails if there is none. Teeth on
  win-clang-cl: without the option the link fails on both symbols; with it the divide cases pass (600,024 assertions).
- Needs CI audit: both complete-tier runs still going end red on causes fixed on main, so HW-RESILIENCE.2, DIAG.11c and
  DIAG.12 stay Needs CI; a complete-tier run is dispatched once neither is in progress.
- Account-backed lanes: the three FTL names exist (no AMD or Mac names); enabling that lane is the operator's CI work.

## Self-check

`hesap.fft` (`fft_self_check`, `src/fft_self_check.cpp`) runs the table's four routines, f64 then f32:

- One Stockham plan per size from 2 to 128, built in general code through `FftPlan`'s by-name constructor on a 12 KiB
  stack arena (`LinearAllocator`) and run through the candidate table, so nothing reaches `fft_kernels()`.
- execute: every size both ways (the leaf codelets, f64's lane-trick codelets at 8, 16 and 32, the Stockham passes
  with odd and even log2), and a forward at 16 on inputs scaled to 2^-1040 and 2^-140 whose results are subnormal.
- execute_batched: f64's register-resident N = 8 and N = 16 codelets (batch 8), the generated leaf codelets (a
  forward over whole tiles), the radix-8 passes with each remainder stage over 11 (f64) or 19 (f32) transforms, a
  whole vector of the widest level and a scalar tail, and an inverse at 64.
- Inputs are small nonzero integers. A plan gives the same bits on every level, so each routine's answer is one
  FNV-1a digest (core's `fnv1a_64_le`, which the capability-owner guard asked for) of the portable table's outputs.
  Every output was checked once against an mpmath DFT at 200 bits: within 4.1e-16 (f64) and 1.0e-7 (f32) of its
  transform's largest output, the subnormal forward within 5.1e-12 and 9.2e-5. Every output has a NaN guard after it.

## Tests and evidence

- `fft self-check:` cases: every table the host runs passes; one result bit off in execute or in a batch's last
  transform, and a write past the batch, fail under the routine's name. The process-level case requires `Passed`.
- win-debug: the cases and the 12 forced arms pass (portable, sse2, sse4.2, avx2); the three tables gave identical
  digests. SDE 10.13.1 `-icx` passes while running avx512. linux-gcc-debug: the plain cases and forced arms pass.
- Teeth: the f32 batched digest one bit off failed every table (`f32 execute_batched`) and the selection case's
  `Passed`; restored, rebuilt and passing.
- Guards: 21 of 21. Strict tidy and clang-format are clean on the changed lines.
- The first linux-gcc-debug build hit two GCC segmentation faults in the sse2 and avx2 codelet units; the rerun built
  both, so it is recorded as this host's known fault, not a code defect.

## Left on the row

The tensor known-answer check, the device path, `ceridc doctor --hardware` and the state-hash divergence check.
