# DIAG.7b census: DX12 GPU-based validation, DRED and device removal

<!-- doc-role: historical -->
> Dated evidence. Live owner: [DIAG.7b](../ROADMAP.md#slice-diag.7b); contract:
> [DIAG.7b](../design/runtime-diagnostics.md#diag-7b); [ADR-0133](../decisions/0133-runtime-diagnostics-and-instrumentation.md)
> DG12. Rules: [AGENTS](../../AGENTS.md). Preceding: [DIAG.7a g-6](2026-10-04-diag-7a-dx12-program-route.md).

## Position

DIAG.7b requires DIAG.7a, which is Open on the user's closure decision; this census changes no row status and starts
no device-removal reproduction. It ran while hosted CI qualified the nightly repairs, per the user's 2026-10-04
direction to carry on serially.

## Acceptance clause (verbatim)

> Qualify debug layer, optional GPU-based validation and DRED setup before device creation. Capture supported
> breadcrumbs/page-fault allocations, actual HRESULT/removal reason, recent resource retirement and program mapping.
> Preserve last-known diagnostic state through failed query calls and provider shutdown. Report available capabilities
> rather than assuming every adapter can page-fault-report.
>
> Acceptance: controlled device-removal/fault handling and safe isolated invalid workloads produce readable bundles;
> separate simulated error-path coverage from an actual adapter/device-loss reproduction. Never intentionally hang the
> desktop GPU outside a contained qualified test. Software-provider evidence is labelled as such; real hardware gates
> stay visible.

## What exists

- **Debug layer and GPU-based validation before device creation.** `detail::Dx12DeviceScope::request_validation`
  records core/sync/GBV requests; `create()` applies the process-global `EnableDebugLayer` and an explicit
  `ID3D12Debug1::SetEnableGPUBasedValidation` under the device mutex before `D3D12CreateDevice`, then resolves the common
  `ValidationActivation` (core proven through the info-queue query; GBV "enabled, not proven instrumenting"; sync has no
  D3D12 equivalent) (DIAG.7a f-3, `dx12_validation_capture.cpp`).
- **Removal observation on every completion path.** `dx12_submit`, `dx12_signal` and `dx12_wait` read
  `GetDeviceRemovedReason` before and after the queue operation, treat a fence value of `UINT64_MAX` as removal, bound
  every wait, report through `dx12_execution_failure` (counted per capture as `execution_failures`), and force
  `ID3D12Device5::RemoveDevice` when completion fails so memory still referenced by queued work is never freed
  (`dx12_execution.cpp`). Context `valid()` checks the removal reason.
- **Identity for mapping.** DIAG.7a names resources, programs and frame-graph passes with Cerid tokens through
  `dx12_attach_identity`, keeps retired provenance in the identity registry ((e)), and pre-parses tokens from messages.
- **Crash bundles.** DIAG.5 writes evidence streams into minidumps (`capture_dump` with a `DumpNote`) and imports
  symbol bundles (5d); a device-removal bundle can ride the same container.

## What is missing

- No `ID3D12DeviceRemovedExtendedDataSettings` setup: breadcrumbs and page-fault reporting are never enabled.
- No DRED read after removal: no breadcrumb history, no page-fault address or allocation names, no
  `ID3D12DeviceRemovedExtendedData2::GetDeviceState`.
- No capability report for DRED per runtime/adapter, and no preserved last-known state when a query fails.
- No mapping of breadcrumb command lists/queues or page-fault allocations to Cerid program/pass/resource identities.
- No readable removal bundle; no labelled simulated-versus-real evidence split; GBV is enabled but never qualified by
  an instrumented invalid workload.

## Design points for review

1. **DRED is not a validation mode.** The common `ValidationActivation` has exactly core/sync/GPU-assisted. DRED is a
   DX12 device-removal diagnostic, so it gets a DX12-local request and activation report on `Dx12DeviceScope`, not a
   fourth common mode (DG11/DG12: one collector; the common vocabulary stays backend-neutral).
2. **Process-global setup.** Like GBV, DRED settings are process-wide and must precede device creation; they belong
   under the same device mutex with an explicit set so they never leak into a later context.
3. **Simulated removal is safe.** `ID3D12Device5::RemoveDevice` removes a device without GPU work and is already the
   engine's own failure response, so it qualifies the read and bundle paths on WARP and hardware without hanging the
   desktop GPU. A real fault (page fault, TDR) stays a separately labelled, contained, hardware-gated reproduction.

## Sub-unit plan (order re-confirmed per step)

| Unit | Work | Gate |
|---|---|---|
| (b) | DRED request on `Dx12DeviceScope`: breadcrumbs and page faults set through `ID3D12DeviceRemovedExtendedDataSettings1` before creation; DX12-local activation with unsupported reasons | win-debug unit tests on hardware and WARP |
| (c) | DRED reader after removal: removal HRESULT, device state, breadcrumb history, page-fault VA and allocations; last-known state kept when a query fails | simulated removal via `RemoveDevice` |
| (d) | Simulated removal end to end through the production completion path (`dx12_wait` failure forcing removal), labelled simulated | win-debug, WARP labelled software |
| (e) | Map breadcrumb lists/queues and page-fault allocations to Cerid identities, including retired provenance | identity tests |
| (f) | Readable removal bundle in the DIAG.5 evidence container | bundle round-trip |
| (g) | GBV qualification: an isolated invalid descriptor workload captured only with GBV on | win-debug hardware |
| (h) | Real device-loss reproduction: contained, hardware-gated, never on the desktop GPU outside a qualified harness | stays visible as a hardware gate |

## (b) landed: DRED setup before device creation

SDK check first: the workstation and the hosted Windows image both use Windows SDK 10.0.26100 (no Agility SDK), whose
`d3d12.h` declares `ID3D12DeviceRemovedExtendedDataSettings`/`Settings1`/`Settings2`, `ID3D12DeviceRemovedExtendedData1`/
`Data2`, `D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1`, `D3D12_DRED_PAGE_FAULT_OUTPUT2` and `D3D12_DRED_DEVICE_STATE`.

- `Dx12DeviceScope::request_dred(breadcrumbs, page_faults)` records the request; `create()` applies it to the
  process-global DRED settings under the device mutex before `D3D12CreateDevice`, through `apply_dred_settings`. Each
  feature is set only when it differs from the last applied state, explicitly `FORCED_ON` or `FORCED_OFF`, so a scope
  that requests nothing turns an earlier scope's breadcrumbs back off instead of inheriting them; with nothing
  requested and nothing applied it makes zero calls, as before. Breadcrumb context strings (`Settings1`) follow
  breadcrumbs when the runtime offers them.
- `Dx12DredActivation` (DX12-local, not a common validation mode) reports the creation-time half per feature:
  `NotRequested`, `Set` or `SettingsAbsent`, whether contexts were enabled, and whether the device exposes
  `ID3D12DeviceRemovedExtendedData1` for the removal-time read. The query outcome at removal is (c)'s half.
- `dx12_dred_process_state()` exposes the applied process-global state for the no-inheritance test.
- The strict gate also required rewriting the nested ternaries in the 7a (f-3) activation reasons as if-chains;
  behaviour unchanged.

Verification on the workstation (`win-debug`, hardware adapter): `[dred]` 1 case, 22 assertions, the `Set` path (not
the `SettingsAbsent` fallback); the whole `[validation]` set 29 cases, 4,162 assertions, passed after the change;
strict gate clean on the three files. Hosted lanes qualify the rest.

## (c) landed: the removal-time DRED read

`dx12_dred.hpp`/`.cpp` add `detail::dx12_read_dred(ID3D12Device*)`, a no-allocation, never-throwing reader into a
fixed-capacity `Dx12DredReport`: the removal HRESULT, the `Data2` device state (Unknown/Hung/Fault/PageFault, or
NotQueried when the interface is absent), the auto-breadcrumb outcome with up to 8 nodes (command list and queue debug
names, op count, last completed op), and the page-fault outcome with the faulting VA and up to 8 existing or recently
freed allocations (debug name, allocation type). Each query keeps its own outcome and raw HRESULT: `Ok`,
`NotAvailable` (`DXGI_ERROR_NOT_CURRENTLY_AVAILABLE`, `DXGI_ERROR_UNSUPPORTED`, `E_NOINTERFACE`) or `Failed`, so one
failed query never erases the others and "unavailable" is never reported as "empty". D3D12 types stay in the module.

Measured on the workstation hardware adapter with a simulated removal (`ID3D12Device5::RemoveDevice` after one named,
completed submission; no GPU hang):

| Device | Removal reason | Breadcrumbs | Page fault |
|---|---|---|---|
| DRED requested, removed | `DXGI_ERROR_DEVICE_REMOVED` | Ok, 0 nodes (nothing in flight) | Ok, VA 0, no allocations |
| DRED not requested, removed | `DXGI_ERROR_DEVICE_REMOVED` | NotAvailable (`DXGI_ERROR_UNSUPPORTED`) | NotAvailable |
| DRED requested, live | S_OK | NotAvailable (`NOT_CURRENTLY_AVAILABLE`) | NotAvailable |

The first two rows differ only in the (b) request, which proves the pre-creation settings reach the runtime. The
`[dred]` test asserts exactly this classification, never breadcrumb content, plus the null-device contract; it passed
three consecutive runs, and the `[validation]` set passed (30 cases, 4,179 assertions). Strict gate clean. Software
evidence (WARP) and a real fault reproduction remain separate, labelled steps ((d), (h)).

## (d) landed: removals recorded on the production completion path

`stop_failed_device` (the failure response of `dx12_submit`, `dx12_signal` and `dx12_wait`) now records every stopped
device through `dx12_record_removal`, with its origin: `Observed` when the runtime or driver had already removed the
device (the classification an actual device loss takes) and `EngineForced` when the engine itself called
`RemoveDevice` after a failed completion (the simulated error path). The `Dx12RemovalRecord` (origin, 1-based
sequence, the full `Dx12DredReport`) lives in a process-lifetime store outside every context, so the last-known removal
survives provider shutdown; the DRED read happens outside its lock.

`[dx12][validation][dred]` (d): a DRED-enabled device, a fence nobody signals, `dx12_wait` with a 50 ms bound. The
timeout yields an `EngineForced` record with `DXGI_ERROR_DEVICE_REMOVED` and both DRED queries `Ok`; a second wait on the
removed device yields an `Observed` record; a live `Dx12ValidationCapture` counted both execution failures; the record
outlives the device and its scope. `[dred]` 3 cases, 55 assertions, three consecutive passes; `[validation]` 31 cases,
4,195 assertions. Strict gate clean on the four files. Remaining: (e) identity mapping, (f) bundle, (g) GBV
qualification, (h) the hardware-gated real fault.

## (e) landed: DRED lists map to Cerid identities

Measured first: on the workstation hardware adapter a forced `RemoveDevice` leaves DRED's breadcrumb list **empty**,
even with a Cerid-named list holding a recorded operation (a UAV barrier) in flight behind a queue-side wait on a fence
nobody signals (the queue idles; no GPU hang). Real breadcrumb and page-fault content therefore needs a genuine device
fault, which is the hardware-gated (h). That end-to-end attempt was replaced, not kept as a test that cannot fail.

The mapping is proven deterministically instead. The read is split: `dx12_dred_fill_breadcrumbs` and
`dx12_dred_fill_page_fault` turn DRED's output lists into the report, and `dx12_read_dred` calls them. Each breadcrumb
node carries `list_identity` and `queue_identity`, and each allocation an `identity`, parsed with the public
`crd::gpu::parse` from the native debug names DIAG.7a stamps. The `[dred]` (e) case feeds constructed lists: ten nodes
(the first a Cerid-named Pass list on an unnamed queue, one over-long name, one without a last-completed value) and a
page fault with one live and one retired Cerid-named resource. It checks that all ten are counted and eight stored,
that the named list maps to its Pass identity and the unnamed queue to none, that the long name is truncated and
terminated, and that the existing allocation maps to a live identity while the freed one keeps a valid, retired
identity (registry `alive` false): the "recent resource retirement" clause. `[dred]` 4 cases, 82 assertions;
`[validation]` 32 cases, 4,222 assertions; strict gate clean.
