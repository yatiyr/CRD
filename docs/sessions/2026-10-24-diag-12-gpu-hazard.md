# DIAG.12: the same application's GPU hazard, faulty then repaired, 2026-10-24

<!-- doc-role: historical -->
> Dated evidence. Live owner: [DIAG.12](../ROADMAP.md#slice-diag.12). Contract:
> [runtime diagnostics](../design/runtime-diagnostics.md#diag-12),
> [ADR-0133](../decisions/0133-runtime-diagnostics-and-instrumentation.md). Rules: [AGENTS](../../AGENTS.md).
> Previous batch: [authored-program error](2026-10-24-diag-12-authored-program.md).

## CI read at the start

`gh run list --branch main --limit 15`: the runs completed since the previous read (40110939655, 40112350438 and
40113440728) were cancelled by the next push before any job started (zero jobs each). 40103197370 (head `6b214076`)
is still in progress (18 jobs green, 7 running) and 40115109503 (head `371f6d65`) pending; neither was touched.
DIAG.11c (Needs CI) still waits for a completed run that carries its work.

## Change

`crd-diag-host-specimen` has an eighth mode, `gpu`. No new executable, trigger kind or engine code:

- The application creates a headless Vulkan context with Core and synchronization validation, the engine's
  `ValidationCapture`, and a `VulkanComputeContext`. It mints a Pass identity for its upload, prints it, and records,
  inside a debug label carrying it, two overlapping copies of its order ledger into one device buffer through the
  public `ComputeRecorder`. The defect is the missing `barrier()` between them; the repaired run records it.
- The detector is the validation layer's synchronization validation. The application takes the first
  `SYNC-HAZARD-WRITE-AFTER-WRITE` record whose object resolves to a live Resource identity (the compute buffer's
  debug-utils name) and reports it as a `Manual` trigger (code 0x0714): the message names the hazard, the buffer and
  the label's pass; the values carry both identities. `stop()` writes the bundle and the run exits 9.
- A host that cannot run the detector prints `CRD_HOST_GPU_ABSENT` with its reason (`no-vulkan-device`,
  `no-validation-layer`, `no-synchronization-validation`) and exits 10, never clean. A repaired upload that is not
  clean (any error, warning or dropped message) exits 11.
- crd-perf's module declaration gains `gpu-context gpu-context-vulkan` as `TEST_DEPENDS`. The case is tagged
  `[vulkan]` and discovered with `RESOURCE_LOCK crd_gpu_device`, like the GPU suites. The seeded-fault registry's
  `lane-loses-sanitizer` edit anchors on the changed discovery line.

The new `[diag12]` case's oracle for the pass is the identity the application printed before the work, which the
layer's report must resolve back through the label. It checks the trigger's code, the exact message rebuilt from
the two identities the values carry, the end (`Shutdown`), the forty events, this build and the application's
module, and `bundle.inspect`. The repaired run exits 0 with no bundle and no dump. On Linux an absent detector fails
(every Linux lane has lavapipe and the pinned layer); on Windows only `no-vulkan-device` is accepted, with exit 10
and no bundle: the hosted Windows runners have no Vulkan device, the open qualification DIAG.11c names.

DX12 has no route for this class. `Dx12ComputeContext` tracks each buffer's state and inserts the transitions
itself, so the application cannot record a resource-state hazard through the public surface, and DX12 has no
synchronization validation (`BackendHasNoEquivalent`). DX12's correlated Core hazards stay DIAG.7a g-5 and g-6.

## Teeth

Each break was built, run, restored, rebuilt and rerun (`build/diag12gpu/teeth.py`, not committed).

| Break in the application | win-debug (hardware adapter) | linux-gcc-debug (lavapipe) |
|---|---|---|
| the repaired run records no barrier | fails: the repaired run leaves a bundle | fails |
| the trigger is not fired | fails: no bundle | fails |
| the upload is not labelled | fails: the pass oracle and the message | fails |
| synchronization validation not requested | fails: the absence reason is not `no-vulkan-device` | not run |
| no device, reported as exit 0 | fails: exit 10 expected | not run |
| control: no device, exit 10 | passes (the named absence) | not run |

## Verification

- win-debug: `crd-diag-host-specimen` and `crd-perf-tests` built; the 24 `diagnostic host` CTests and the ctest
  guards (15) pass.
- linux-gcc-debug (`wsl-lane.sh build`/`test`): both targets built `-Werror`; the 24 `diagnostic host` CTests pass
  (the two window cases skip headless, as before).
- `tidy-files.py` clean on both files. `clang-format --dry-run`: the added lines' findings are the house hand
  alignment only. `check-allman-braces.py`, `check-master-plan.py` and `check-repository.py` pass.

Hosted once pushed: the GPU case reaches the hazard on every Linux lane and reads `no-vulkan-device` on the hosted
Windows lanes.

## Remaining in DIAG.12

Privacy across every class; removal of the remaining obsolete scaffolding and duplicated services; the re-read of
every child and DG01-DG18 against final source and published CI.
