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
