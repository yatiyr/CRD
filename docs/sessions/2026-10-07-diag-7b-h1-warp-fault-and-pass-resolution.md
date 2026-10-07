# DIAG.7b(h1) WARP real-fault measurement and DRED pass resolution, 2026-10-07

<!-- doc-role: historical -->
> Dated evidence. Live owners: [DIAG.7b](../ROADMAP.md#slice-diag.7b), [DIAG.7a](../ROADMAP.md#slice-diag.7a).
> Contract: [runtime diagnostics](../design/runtime-diagnostics.md#diag-7b). Rules: [AGENTS](../../AGENTS.md).
> Decisions: [2026-10-05 user decisions](2026-10-05-user-decisions.md) (section 5 splits (h) into (h1) and (h2);
> section 4 gates the DX12 pass-to-fault proof on it). Preceding: [DIAG.7b census](2026-10-04-diag-7b-census.md).

## Goal

The user's decision for DIAG.7b(h) was: first, (h1), a real page-fault device removal on WARP in a child process,
labelled as software evidence, with "whether WARP reports DRED page-fault data is measured first"; then (h2), one
opt-in hardware page-fault run that the user starts. DIAG.7a's DX12 pass label proof rides the same fault: the core
`BeginEvent` marker around every frame-graph pass is visible only in DRED breadcrumb contexts, and those are
populated only by a genuine removal.

## Measured first: WARP produces no real device fault

The workload, in a child process on the explicit WARP adapter (`EnumWarpAdapter`, vendor 0x1414 device 0x008C) with
DRED breadcrumbs, contexts and page faults requested (all `Set`, readable) and no validation layer:
two passes bracketed by the production `Dx12PassEventScope`, the second storing 16 KiB through a root UAV, submitted
with `dx12_submit` and waited with the bounded `dx12_wait`.

| Mode | Root UAV address | Result on WARP |
|---|---|---|
| offset | 1 GiB past a live buffer | wait `S_OK`, removal reason `S_OK`: the stores are discarded |
| freed | a buffer released after recording, before submission | wait `S_OK`, removal reason `S_OK` |
| wild | `0x10000` | wait `S_OK`, removal reason `S_OK` |
| hang | a live buffer, a store loop that never ends | no removal by WARP within 20 s, nor within 55 s; the engine's bounded wait times out (`0x800705B4`) and forces the removal (`EngineForced`) |

After the forced removal in the hang mode, DRED reports `DXGI_ERROR_DEVICE_REMOVED`, device state `Unknown`, the
breadcrumb query `Ok` with **0 nodes** (although the list was executing) and the page-fault query `Ok` with VA 0.
That matches DIAG.7b(e) on hardware: only a genuine removal populates DRED's lists.

So WARP cannot supply the real fault: it treats an unmapped GPU address as a discarded store, and it has no watchdog.
The real-fault evidence, and with it the DIAG.7a pass-to-fault correlation, rides (h2) entirely. The hang probe was
a measurement only; it is not kept (it spins a CPU core until the bound).

## What changed

- **DRED reader, bundle version 2** (`engine/gpu/gpu-context-dx12/src/dx12_dred.{hpp,cpp}`). Each breadcrumb node
  now also keeps the first 64 ops of its history, up to four breadcrumb context strings (narrowed to ASCII, with the
  Cerid identity parsed from each) and the **in-flight pass**: of the `BeginEvent` markers still open at op
  `last_completed`, the innermost whose context carries a Pass identity. A tool's own marker nested inside a pass does
  not hide the pass; a pass whose marker carried no string, a closed pass and a finished list resolve to none; nesting
  deeper than 16 resolves to none rather than to an outer pass. The walk reads DRED's own lists, so it is exact when the
  history or contexts exceed what the report stores. `kDx12RemovalBundleVersion` is 2, so a version-1 bundle is
  refused as `BadVersion` instead of misread.
- **Breadcrumb value semantics, unsettled.** DRED's last breadcrumb value is read either as the count of completed
  ops or as the index of the last completed one. A `BeginEvent` at the stopping op therefore counts as open, which
  resolves the pass of a stopped draw, dispatch or copy under both readings. The (h2) run prints the raw value and
  history, which settles it.
- **`tests/gpu/gpu-context-dx12/test_dx12_device_fault.cpp`** (new, in `crd-gpu-context-dx12-tests`):
  - a hidden child case (`[.dx12-device-fault-child]`) runs the workload, installs crash capture into a directory the
    parent passes in the environment, and leaves with `_Exit`; a real removal goes through the production
    `stop_failed_device` path, which records it as `Observed` and writes the `gpu_*.dmp` bundle;
  - **(h1)** `[dx12][validation][dred][device-fault]`: the WARP child must end `completed` (the measured software
    limit, and then no bundle may exist) or `recorded` (then the full bundle is checked). A missing DRED, a timeout, a
    spawn failure or a crash fails it. It is the CI-capable run of the whole path, labelled software evidence;
  - **(h2)** `[.dx12-hardware-device-fault]`, hidden and skipped unless `CRD_DX12_HARDWARE_FAULT_OPT_IN=1`: the same
    child on the default device, which the production classifier must call `Hardware`. It requires an `Observed`
    removal, a failure removal reason, DRED state `PageFault` or `Fault`, breadcrumbs `Ok` with the in-flight pass
    resolved to the second pass (`fault pass`, kind Pass), and the page-fault query `Ok` with the released buffer
    among the recently freed allocations under its Cerid identity. The bundle directory is kept as evidence;
  - a deterministic resolver case in `[dred]` reads one constructed history at seven stopping points and a crowded
    node with six contexts (four stored).

## Evidence

- **win-debug** (workstation, RTX 4070 Ti SUPER present; the fault legs use WARP only): `[dred]` 7 cases, 142
  assertions; the whole `crd-gpu-context-dx12-tests` 203 cases, 10,634 assertions (twice, before and after the
  formatting pass). The WARP child prints `WARP: no device fault (software-provider limit)`.
- **win-shipping**: builds; `[dred],[validation]` 37 cases, 4,332 assertions; `[dred]` 7 cases.
- **win-clang-cl-shipping**: the first build hit a clang-cl frontend crash (`0xC0000005` in the `dse` pass on the new
  test file) while other TUs compiled alongside; the next build compiled and linked it clean, and the final build was
  clean too. This is the known non-deterministic LLVM 20.1.8 crash under memory pressure, not a source diagnostic.
  `[dred]` 7 cases, 142 assertions.
- **win-asan**: builds; `[dred]` 7 cases, 142 assertions, no AddressSanitizer report (the WARP child included).
- **Hidden cases:** `--list-tests` shows neither child nor the (h2) case; the (h2) case without the opt-in reports a
  visible SKIP.
- **Teeth:**
  - Resolver: with the `EndEvent` pop disabled, the stop inside the context-less pass C resolves to pass B and the
    resolver case fails (1 of 34 assertions, exit 42). Restored and rebuilt: green.
  - Child: with DRED not requested, the child exits 20 and the (h1) case fails. Restored and rebuilt: green.
- **Checks:** the strict tidy gate is clean on both C++ files; `clang-format --dry-run` reports no new findings on
  `dx12_dred.{hpp,cpp}` (35 and 42, as at HEAD) and none on the new test; the Allman check passes. The repository
  guards (`ctest -R "^crd-no-|^crd-check"`, 8 tests) pass; the first run caught a `std::string` in the child, now a
  plain buffer. `check-master-plan.py` and `check-repository.py` pass.
- **Linux (WSL):** not applicable. `gpu-context-dx12` and its tests are Windows-only, and no shared `gpu-context`
  header changed (`dx12_dred.hpp` is private to the DX12 module).

## Remaining

- **(h2), user-run.** On an idle machine, from a Developer Command Prompt at the repository root, after building
  `crd-gpu-context-dx12-tests` on win-debug:
  `set CRD_DX12_HARDWARE_FAULT_OPT_IN=1` then
  `build\win-debug\tests\gpu-context-dx12\crd-gpu-context-dx12-tests.exe "[.dx12-hardware-device-fault]" -s`.
  The GPU takes one contained page fault in a child process; Windows resets the adapter (TDR) and the desktop
  recovers. The result is evidence for this GPU only. It closes DIAG.7b's real-fault clause and DIAG.7a's DX12
  pass-to-fault correlation, and settles the breadcrumb value semantics.
- **Hosted CI:** the `[dred]` cases, the (h1) WARP case included, must pass on the hosted Windows lanes (win-debug,
  win-shipping, win-clang-cl-shipping, win-asan).
