# DIAG.11c: one native diagnostic workflow per tuple, 2026-10-24

<!-- doc-role: historical -->
> Dated evidence. Live owner: [DIAG.11c](../ROADMAP.md#slice-diag.11c). Contract:
> [runtime diagnostics](../design/runtime-diagnostics.md#diag-11c),
> [ADR-0133](../decisions/0133-runtime-diagnostics-and-instrumentation.md). Rules: [AGENTS](../../AGENTS.md).
> Previous batch: [DIAG.11c doctor modes](2026-10-24-diag-11c-doctor-modes.md).

## CI read at the start

`gh run list --branch main --limit 15`: the only failed job on a completed run is still linux-gcc-public-checks
(120819184913) of the scheduled run 40075929537, repaired by `d96eae30`. Every push run since 40079623101 was
cancelled by the next push with no failed job; 40080021621 is still running and was left alone.

## Change

The workflow already existed as separate cases; nothing marked them as one route or counted them per lane. Now:

- Seven cases carry `[diag-workflow]`: `doctor: reports the schema, host tuple and the diagnostic modes`; `diagnostic
  host: an application that fails on purpose ...` (profile, fatal, bundle with build and module identity,
  `bundle.inspect`); `symbolize (windows): a real dump ...`; the two `crash capture` wild-write cases (Windows dump,
  Linux record); `DIAG.7b(h1)` (DX12 device fault on WARP); `DIAG.7c(g)` (a Vulkan loss written as a bundle).
- crd-perf-tests, crd-core-tests and the two GPU test targets discover twice, `TEST_SPEC "~[diag-workflow]"` and
  `"[diag-workflow]"` with `LABELS "diag;diag-workflow"`: each case is registered once (the win-debug inventory
  counts equal each executable's `--list-tests`: 298, 77, 216, 313), and `results.labels` in every lane's
  `conclusion.json` counts the route beside the toolchain, modes and adapters. No script or CMake helper was added:
  `crd_discover_tests` already forwards `TEST_SPEC`, and `ci-evidence.py` already counts labels.
- `check-diag-runbooks.py` resolves a `-L <regex>` against the labels of the tree's tests, as it does `-R` against
  their names; the runbook's mode entry names `ctest --test-dir build/win-debug -L diag-workflow`.
- The `lane-loses-sanitizer` seed in `tests/support/diag/seeded-faults.json` anchors on the new discovery line
  (`crd-diag-seeded-faults` failed on the old one: the seeded text occurred 0 times).

## Tuples run here

| Tuple | Lane | Adapter | Workflow cases |
|---|---|---|---|
| Windows 11 26200 x64, MSVC 19.51.36246, Debug | win-debug | NVIDIA GeForce RTX 4070 Ti SUPER, driver 32.0.16.1088 (Vulkan: device fault enabled); DX12 case on WARP | 6 of 6 passed |
| Linux (WSL) x64, GCC 13.3, Debug | linux-gcc-debug | llvmpipe (LLVM 20.1.2), VK_EXT_device_fault absent | 4 of 4 passed |

Linux has no minidump route, so its symbol step is the bundle's GNU build id matched to the file (the host case).
Hosted Windows runners have no Vulkan device (run 39239067340, win-debug: `DIAG.7c(g)` skipped, adapter Microsoft
Hyper-V Video): Vulkan on Windows is qualified on this host's hardware only.

## Teeth

- Without the doctor case's tag (win-debug, rebuilt) the label selects 5 cases and the doctor case is still
  registered once; restored, rebuilt: 6.
- `check_regex` resolving `-L` against test names: the new tooling case fails; restored: passes.

## Verification

win-debug: the four targets build; `ctest -L diag-workflow` 6 passed; `ci-evidence.py inventory` and `bundle` on that
run record `diag-workflow` 6 executed, 6 passed. linux-gcc-debug (`wsl-lane.sh build` and `test -L diag-workflow`):
4 passed. `test-repository-tools.py DiagRunbooks`, the ctest guards (`crd-check-diag-runbooks` included),
`check-allman-braces.py`, `check-master-plan.py`, `check-repository.py`.

Hosted once pushed: `diag-workflow` in each lane's `conclusion.json`: 6 cases on the Windows lanes (`DIAG.7c(g)`
skipped for want of a Vulkan device), 4 on the Linux lanes, none failed.

## Not done in this batch

DIAG.11c: the audit of public schemas and host adapters for x64-only or native-filesystem-only assumptions.

Follow-up for an extra batch: none.
