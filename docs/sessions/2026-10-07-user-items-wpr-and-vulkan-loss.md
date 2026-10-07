# User items: the elevated WPR capture and the Vulkan real-loss decision, 2026-10-07

<!-- doc-role: historical -->
> Dated evidence. Live owners: [DIAG.6c](../ROADMAP.md#slice-diag.6c), [DIAG.7c](../ROADMAP.md#slice-diag.7c),
> [DIAG.7b](../ROADMAP.md#slice-diag.7b), [DIAG.7a](../ROADMAP.md#slice-diag.7a). Contract:
> [runtime diagnostics](../design/runtime-diagnostics.md). Rules: [AGENTS](../../AGENTS.md).

## DIAG.6c: the elevated WPR capture

The user ran `python scripts/sample-cpu-wpr.py --stacks` once from an elevated PowerShell. WPR recorded
`%TEMP%\crd_hotspot.etl` (462 MB, 11:04). The script then printed a failed export, not a verdict:
- the `--stacks` export ran `xperf -i <etl> -o <txt> -symbols -a stack`;
- xperf's stack action needs an activity, so it answered `error: stack: no option specified` with exit 5;
- it left an empty `crd_hotspot_profile.txt`.

The recorded trace itself was sound. Re-analysed without elevation:

| Measure | Value |
| --- | --- |
| Specimen process | `crd-diag-cpu-hotspot-specimen.exe`, pid 26256 |
| Samples in that process | 18,194 |
| Walked stack to the hotspot | `__scrt_common_main → invoke_main → main → crd_diag_unscoped_hotspot_burn` |
| `crd_diag_unscoped_hotspot_burn` | 4,093 inclusive samples (22.5 %), 2,635 exclusive |

Changes to `scripts/sample-cpu-wpr.py`:
- **Stacks export:** it now uses `-a stack -butterfly -process <specimen>`, checked against `xperf -help stack`.
- **Frame check:** it accepts the butterfly report's `module ! symbol` form, so a stack report without the hotspot
  reads as `HOTSPOT_NOT_VISIBLE`, not `MISSING_STACKS`.
- **`--from-etl <trace>`:** it runs only the analysis on a recorded trace, which needs no elevation. Only recording
  needs the kernel sampler.

With the fixed script, both verdicts were reproduced from the user's trace:
- `--from-etl <trace> --stacks` gives `HOTSPOT_VISIBLE` in the sampled call stacks (clause 2, external half);
- `--from-etl <trace>` gives `HOTSPOT_VISIBLE` in the flat sampled profile (clause 3).

`scripts/test-sample-cpu-wpr.py` now has 15 tests. Six are new: the exact export arguments for both modes, an export
failure never reading as visible, butterfly frames, `--from-etl` running no `wpr`, and a missing trace. All six fail
against the old script; the old tests mocked xperf as always succeeding, which is why the defect went unseen.

The row's other blocker, the clang-cl `-Winfinite-recursion` fix, is confirmed by the green complete-tier runs
37445373995, 37456628683 and 37502078114 (`win-clang-cl-shipping` builds `crd-perf-tests`). DIAG.6c therefore has no
work left beyond its hosted tooling check.

## DIAG.7c(h): the Vulkan real device loss

Item (h) was the proof that a real Vulkan device loss produces `VK_ERROR_DEVICE_LOST` and a meaningful fault report:
- a software leg on lavapipe (h1), never measured;
- an opt-in hardware leg on the user's GPU (h2), with no test written.

Two loop steps stopped before building (h1) ([handover](2026-10-07-diag-7c-h-handed-to-user.md)).

**User decision:** (h) is recorded as an explicitly unqualified route. No real-loss test is built:
- the loss path stands on the injected-loss, `VK_EXT_device_fault`-report and loss-bundle proofs (c) to (g);
- whether a physical GPU's real loss matches them stays unproven, and is listed as unproven rather than claimed;
- the DX12 counterpart, DIAG.7b(h2), runs on the user's hardware (below).

## DIAG.7b(h2) and DIAG.7a: the DX12 hardware fault

The user agreed to one opt-in hardware run, after the explanation of what it does:
- a contained page fault in a child process;
- possibly a brief TDR, a GPU reset with a few seconds of flicker;
- GPU-heavy applications closed first;
- the serial workflow paused so no GPU test runs at the same time.

The WARP leg (h1) was rebuilt and passes on win-debug. The hardware result is recorded below once the run is done.
