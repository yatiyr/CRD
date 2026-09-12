# DX12 validation and command lifetimes

<!-- doc-role: reference -->
> Technique reference. Owner: [REPO.3c.5.a](../ROADMAP.md#slice-repo.3c.5.a). [Repair contract](../design/dx12-workload-repair.md).

## Parameters first

| Parameter or result | Meaning and units | Default / bounds |
|---|---|---|
| Allocator | Owns capture state and records; must outlive the capture | Required `IAllocator`; failed allocation is explicit |
| Capacity | Retained diagnostic records | 256; 1–4,096; no callback allocation or growth |
| Message text | Copied native text, bytes including terminator | 1,024; truncation is counted and prevents qualification |
| Device registrations | Distinct live native devices, shared across contexts | 32; exhaustion prevents qualification |
| Startup replay | Maximum stored native notifications / bytes per notification | 4,096 / 8,192; overflow or unreadable records are explicit |
| Wait timeout | Maximum native fence wait, milliseconds | 30,000; finite; tests use 5 for intentional timeout |
| Readiness | Whether the capture witnessed validated startup | Ready, invalid capacity, allocation failure, unavailable layer or late start |
| Counts | Notifications, drops, truncations, instrument/execution failures and context lifetimes | Monotonic; first instrument issue retains operation, HRESULT and detail |

No benchmark or performance improvement is claimed. [Session evidence](../sessions/2026-09-12-dx12-validation-foundation.md)
records correctness runs and the actual hardware/software tuples.

## Why this exists

A black image or an empty callback log cannot establish a successful GPU run. Validation may have started too late,
failed registration, lost messages or missed teardown. A failed command-list close must never be submitted, and a
signalled fence after device removal is not successful completion.

[EnableDebugLayer](https://learn.microsoft.com/en-us/windows/win32/api/d3d12sdklayers/nf-d3d12sdklayers-id3d12debug-enabledebuglayer)
must precede device creation. [Message callbacks](https://microsoft.github.io/DirectX-Specs/d3d/MessageCallback.html)
can run on arbitrary threads, must not call D3D, and remain active until deregistration has waited out callbacks.
[GetCompletedValue](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12fence-getcompletedvalue)
returns UINT64_MAX on device removal. These are API invariants, not rendering algorithms or numerical approximations.

## Invariants and assembly

1. Construct `Dx12ValidationCapture` before native devices. Allocate its bounded state once. A process-local mutex
   serializes managed device creation and debug enablement. A late capture is explicitly ungated; it never enables
   the layer on a live device. There is no persistent DirectX setting change in this implementation.
2. Every factory in `gpu-context-dx12` creates its device through `Dx12DeviceScope`. Declare that member before native
   resources so it retires after them. Shared native device identities share one callback; distinct devices get
   separate registrations. The program/default-adapter probe, compute, raster, RT and work-graph contexts participate.
3. Register the callback before replaying stored startup messages. This closes the asynchronous handover gap;
   overlap may conservatively count a notification twice. Counts are observations, not guaranteed unique native events.
   Callbacks ignore native filters. Startup replay inspects actual filters and rejects anything that could suppress
   warnings/errors; the SDK's informational-only deny filter is acceptable. No filter is changed.
4. Copy messages under a records mutex. Reports and individual messages are returned by value. Removing an observer
   takes the same mutex before freeing its allocator-owned storage. Native callback removal holds no records lock.
   A failed native deregistration quarantines the bounded slot and retains its queue ownership; later captures retain
   that instrument failure rather than reusing potentially live callback storage.
5. Check allocator/list Reset, Close, Execute's device state, Signal and fence completion. Advance fence values only
   after successful Signal; reject overflow into UINT64_MAX. `submitted` counts actual Execute calls, not completion.
   A failed Close yields zero submissions. An early event wakeup still requires the requested completed fence value.
6. A failed wait or post-submit retirement cannot free potentially in-flight resources. Remove the failing logical
   device using [ID3D12Device5](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12device5-removedevice),
   then retire resources; this does not reset the OS adapter. An older runtime unable to perform that safe retirement
   terminates through the engine fatal path. No infinite wait or successful readback is fabricated.
7. Destroy participating resources/contexts, then assert `complete_and_silent()`: Ready, started > 0, started = finished,
   active = 0, and zero warnings/errors/drops/truncations/instrument/device-creation/execution failures. Separately assert
   real workload execution and its unchanged output oracle. Silence alone does not qualify a feature.

## Reproduce and diagnose

Build `crd-gpu-context-dx12-tests` using the scoped [BUILDING](../BUILDING.md) workflow, then select
`-R '^DX12 validation' --timeout 180 --no-tests=error`. Eight CTests exercise deliberate bad close, late capture,
removed device, timeout/premature wakeup, bounded concurrent callbacks, observer/device lifetimes and a real production
compute copy. Expected negative cases assert their errors; the positive copy requires validation silence through all
five context lifetimes. Creating RT/work-graph contexts does not claim actual ray or work-graph dispatch qualification.

For software reproduction use the fully restored [WARP diagnostic protocol](2026-09-12-dx12-adapter-classification.md#controlled-warp-workload).
Capture exact selected identity/driver and reconcile CTest inventory, JUnit execution counts and exit status.

Traps: do not enable validation after creating even an unmanaged native device; do not equate a nonzero native filter
size with suppression of errors; do not hold a records lock while unregistering; do not confuse information with
warnings; do not reuse descriptors/resources merely because a wait returned. Capacity loss is an instrument failure,
not a reason to silently omit diagnostics. Test all failure exits as well as the valid data path.

## Code and explicit boundary

[Public capture](../../engine/gpu/gpu-context-dx12/include/crd/gpu/dx12_validation_capture.hpp),
[device scope](../../engine/gpu/gpu-context-dx12/src/dx12_device_scope.hpp),
[capture implementation](../../engine/gpu/gpu-context-dx12/src/dx12_validation_capture.cpp),
[checked execution](../../engine/gpu/gpu-context-dx12/src/dx12_execution.cpp),
[regressions](../../tests/gpu/gpu-context-dx12/test_dx12_validation.cpp).

The standalone [kir-dx12 backend](../../engine/gpu/kir-dx12/src/backend_dx12.cpp) creates a separate native device and
does not participate in this registry. External devices are likewise unmanaged. They must exist after layer enablement
and have their own instrument; this capture cannot prove their lifetime coverage. Integrating that provider without
a dependency cycle belongs to [RAH-6.b](../ROADMAP.md#slice-rah-6.b), not an invented all-provider qualification here.
