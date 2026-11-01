# 2026-11-01 — HW-RESILIENCE.3: known-answer self-check of hesap-sparse

<!-- doc-role: session -->
> Dated session note; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [HW-RESILIENCE.3](../ROADMAP.md#slice-hw-resilience.3). Before:
[the direct and rigid3d self-checks](2026-11-01-hw-resilience-3-direct-rigid3d-self-checks.md).

## CI read

- Completed runs: push runs up to 41277265371 were cancelled by concurrency and have no failed jobs.
- Running complete-tier runs: 41254049879 (`31404ec4`) and 41269105413 (`b0b1b245`) failed only on causes already
  fixed on main (`filesystem.cpp`'s `min`/`max` by 9723ac75, C4324 by 921e19cc, the clear/copy race by a93cf0ca, the
  coordinator race by aa05dd37, deflate by 95ec6965, tidy by a40af1bf). 41274380605 (`de5e10b0`, queued) holds every
  one of those fixes, so it carries HW-RESILIENCE.2's ARM64 clause and the DIAG.11c/DIAG.12 complete-tier evidence;
  none was dispatched.
- Account-backed lanes: only the FTL names exist; no AMD or Mac names. DIAG.11b clause (8): no new recording under
  `build/diag-overhead-wpr/`.

## Self-check

`hesap.sparse` (`sparse_self_check`, `src/sparse_kernel_dispatch.cpp`):

- One SELL-C-σ matrix per slice height (4: f64, c64, c32; 8: f32), laid out at compile time as `to_sell` lays one out
  and held to its invariants by a `static_assert`: two U32 slices that pair, two Delta16 slices that pair, a narrow
  Delta16 slice and a last one with an empty row and a pad row; every slice but one has masked columns, the pairs share
  full columns with columns of each left after them, one gap needs the high byte, and the height-4 matrix is permuted.
- Values are non-dyadic ratios, so a level that fused a step would round differently; the answers are the declared
  order (two roundings a step from +0, then the scalar write) evaluated at compile time. No row stores column 0 and
  x[0] is +infinity, so a padded lane that read x instead of a zero gives NaN.
- Three picks per type in two slice ranges each: the resident policy (a U32-only range, then both forms), the streaming
  policy at pair width 2 with beta 0 into a NaN-filled y (beta 0 never reads y), and scalar loads with no pairs.
- SDDMM: a 4-row mask with an empty row at depth 23 (every stripe and tail), rows padded with NaN past the depth, then
  depth 0 (`alpha * +0`, here -0).

## Tests and evidence

- `sparse self-check:` cases: every table the host runs passes; a wrong routine (each of the six, one answer a bit
  off) fails in that routine. The process-level case requires the family's resolution `Passed`.
- win-debug: `[kernels],[sparse]` (116 cases) and the 87 forced `-kernels-at-` arms pass; SDE 10.13.1 `-icx` with
  `CRD_CPU_LEVEL=avx512` passes while running avx512. linux-gcc-debug: the arms and kernel cases (102) pass;
  linux-gcc-asan: 90 pass.
- Teeth: an empty-dot answer changed failed every table; a portable body that read x for padded lanes failed it;
  a portable step fused with `std::fma` failed portable. All restored, rebuilt and passing.
- Guards: 21 of 21. Strict tidy and clang-format are clean on the changed files.

## Local fault on the Linux lane

At `ctest -j 4` the CSR spmv cases of the forced arms died with SIGSEGV or SIGBUS in the portable SELL step about one
run in twelve, at HEAD without this change too. The program is single-threaded and, under gdb with address
randomisation off, still crashed only sometimes, with impossible argument values; serial runs (0 of 24) and the ASan
build under the same four-way load (0 of 40) were clean, and so were 12 rounds of `tsan-host-check.py`. This matches
the host's load-dependent misexecution (MEMORY "This host misexecutes"); the hosted Linux lanes, which run these cases
in parallel on every push, are the evidence.

## Left on the row

Known-answer checks for hesap dense, fft and tensor, the device path, `ceridc doctor --hardware` and the state-hash
divergence check.
