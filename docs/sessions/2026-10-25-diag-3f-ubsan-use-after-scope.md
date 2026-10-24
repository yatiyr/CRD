# DIAG.12: UBSan and use-after-scope specimens for DIAG.3f, 2026-10-25

<!-- doc-role: historical -->
> Dated evidence. Live owner: [DIAG.12](../ROADMAP.md#slice-diag.12). Contract:
> [runtime diagnostics](../design/runtime-diagnostics.md#diag-3f),
> [ADR-0133](../decisions/0133-runtime-diagnostics-and-instrumentation.md). Rules: [AGENTS](../../AGENTS.md).
> Previous batch: [the asset reader on the I/O seam](2026-10-25-diag-10b-asset-read-seam.md).

## CI read

No completed run has a failed job. 40140261458, 40139956989, 40138646327, 40136412382, 40136198933, 40134042881,
40131739936, 40131452484, 40130228499, 40130140033, 40128640993, 40128305793, 40127079573 and 40126129075 were
cancelled with no failed job; 40140275224 (head `01562de5`) is pending.

## Change

DIAG.3f's design names UBSan and use-after-scope; the row left both "not claimed", and the re-check
([residual limitations](2026-10-25-diag-12-owner-rows.md)) gave their specimens to DIAG.12.

- `crd-diag-use-after-scope-specimen` reads a block-scoped local through an escaped pointer after the block ends, in
  `crd_diag_read_dead_scope`. GCC and MSVC ASan both report `stack-use-after-scope` naming that frame, so the route is
  claimed on every ASan lane and judged on `lane.asan`; no new lane field.
- `crd-diag-signed-overflow-specimen` adds `INT_MAX + 1` in `crd_diag_add_overflow`, with
  `__ubsan_default_options` printing the stack. GCC defines no macro for `-fsanitize=undefined`, so the target gets
  `CRD_DIAG_UBSAN_BUILD` from `CRD_ENABLE_UBSAN`; the UBSan-class label wins over ASan's, so win-asan reports it
  absent.
- The runner knows the `ubsan` tag (a report is a `runtime error: ` line) and `LanePromise::ubsan`
  (`CRD_DIAG_LANE_UBSAN`).
- Two `diag 3f:` cases in `test_diag_harness.cpp` require the catch, the site and the class name on the promising
  lanes and `InstrumentAbsent` elsewhere.
- Both activation seeds (`specimens-lose-sanitizer`, `lane-loses-sanitizer`) strip the new specimens too. Stripped,
  the scope specimen reports absent; the overflow one keeps its definition, runs the add unchecked and exits 0, so its
  case fails on `verdict=Unexpected`.

Two specimen files are new: the harness runs one specimen per class, and a specimen's sanitizer label is fixed at
compile time, so neither class fits an existing specimen.

## Teeth

- win-asan, the read moved inside the scope: the scope case fails (`verdict=Unexpected`). Restored, rebuilt, passes.
- linux-gcc-asan, the add made `INT_MAX + 0`: the overflow case fails (`exit 1 without an UndefinedBehaviorSanitizer
  report`). Restored.
- linux-gcc-asan, the runner's `ubsan` report check made false: the same failure. Restored, rebuilt, the 17
  `diag 3f:`/`diag harness:` cases pass.

## Verification

- win-debug: crd-perf-tests builds; the 17 `diag 3f:`/`diag harness:` cases pass, both new ones `InstrumentAbsent`.
- win-asan: the five `diag 3f:` cases pass; the scope report names `crd_diag_read_dead_scope`, the overflow is
  absent.
- linux-gcc-asan (`wsl-lane.sh`): the 17 cases pass; the scope report and the UBSan report (`signed integer overflow:
  2147483647 + 1`, frame `crd_diag_add_overflow`) name their sites.
- `diag-seeded-faults.py`: on linux-gcc-asan both activation seeds are DETECTED with six tests each, the two new
  cases among them; on win-asan `specimens-lose-sanitizer` is DETECTED with three (heap overflow, use after free, use
  after scope). The win-asan cases pass again after the runner's restore.
- The ctest guards, tidy-files (six files clean), Allman, `test-diag-seeded-faults.py`, `diag-coverage.py
  --validate`, check-master-plan and check-repository pass; clang-format finds nothing in the new files and no more
  than HEAD in the changed ones.

Hosted once pushed: the two `diag 3f:` cases pass on every lane (caught on win-asan and linux-gcc-asan as above,
absent elsewhere), and the "Seeded detector failures" step reports both activation seeds DETECTED.
