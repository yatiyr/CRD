# Classifying DX12 execution adapters

<!-- doc-role: reference -->
> Technique reference. Live ownership: [REPO.3c.4](../ROADMAP.md#slice-repo.3c.4).

## Parameters and outputs

| Value | Meaning | Default / valid states |
|---|---|---|
| Device LUID | Identity of the device actually selected by `D3D12CreateDevice(nullptr, ...)` | Queried from that device; never inferred from a display-controller name |
| DXGI evidence | Whether the descriptor query succeeded and its software flag | Unavailable / known flag; negative flag alone cannot establish hardware |
| Kernel evidence | Query availability, SoftwareDevice and RenderSupported bits | Unavailable / rendering hardware / software / display-only |
| BasicRender identity | Exact successfully queried vendor 0x1414 **and** device 0x008c | Documented software provider, including its flagless primary-display form |
| `Dx12AdapterKind` | Public backend classification | Unknown, Hardware, Software |
| Compatibility boolean | Existing `dx12_default_adapter_is_software` result | True only for Software; Unknown retains stricter behavior |
| Native status | HRESULT or NTSTATUS returned by the corresponding query | Log the raw status; zero-initialized output after failure is not evidence |

These values are identifiers and categorical evidence, not physical quantities. There is no precision/timing knob.

## Why the flag is insufficient

A displayed adapter, a default D3D12 device and a software implementation are different observations. Resolve the
device's LUID before classifying it. Microsoft's [DirectStorage implementation](https://github.com/microsoft/DirectStorage/blob/main/GDeflate/GDeflateDemo/main.cpp#L245-L266)
documents that Microsoft Basic Render Driver may omit `DXGI_ADAPTER_FLAG_SOFTWARE`. The
[BasicRender documentation](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/microsoft-basic-display-driver)
identifies its role exposing WARP through a kernel adapter. Cerid's published CI reproduced flags 0 for that provider.

The [kernel adapter type](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmthk/ns-d3dkmthk-_d3dkmt_adaptertype)
contains separate SoftwareDevice and RenderSupported bits. The [published 7201 census](../sessions/2026-09-12-dx12-validation-foundation.md#published-ci-observation)
proved that both software flags can be zero on a display-backed BasicRender device. A negative SoftwareDevice bit
therefore cannot establish hardware rendering. The [DXGI overview](https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/d3d10-graphics-programming-guide-dxgi)
documents BasicRender's exact 1414:008c identity and its flagless primary adapter. Use that full identity from the selected
LUID; a vendor alone or a display name is insufficient. This corrects the original kernel-bit-only policy. It does not
establish any rendering feature, numerical accuracy or hardware performance result.

## Assembly and evidence policy

1. Create the same default D3D12 device used by the current backend contexts, then read `GetAdapterLuid`.
2. [Open the kernel adapter from that LUID](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmthk/nf-d3dkmthk-d3dkmtopenadapterfromluid).
   An RAII owner closes every successfully opened handle on all paths.
3. [Query adapter type](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmthk/nf-d3dkmthk-d3dkmtqueryadapterinfo)
   with `KMTQAITYPE_ADAPTERTYPE`, recording availability separately from SoftwareDevice and RenderSupported.
4. Resolve DXGI's descriptor for the same LUID. Pass both evidence sources to the backend-private policy.
5. A successfully queried exact BasicRender identity means Software even if both software flags are absent. Otherwise,
   positive kernel software means Software. Kernel hardware requires RenderSupported and a negative SoftwareDevice bit;
   contradictory positive DXGI software then leaves Unknown. Without hardware evidence, a positive DXGI software flag
   can establish Software. Display-only or unavailable evidence otherwise leaves Unknown; never infer hardware from absence.
6. The compatibility boolean tests equality with Software. The independent CI census prints native query/status bits,
   rejects Unknown and compares the engine result with the observed device. Query failure cannot silently qualify hardware.

The kernel query uses Windows' Gdi32 import library privately inside the DX12 backend. No Windows structure enters
a shared public interface; other backends retain their existing dependencies and platform guards.

## Traps and verification

- A name or vendor alone is not an implementation oracle. The documented exact BasicRender pair is a provider rule;
  qualify nearby vendor/device values as counterexamples, and never trust an unqueried descriptor.
- Failure plus a zero-initialized structure does not mean hardware. Preserve the separate availability bit.
- The explicitly enumerated WARP adapter and kernel BasicRender adapter need not be assumed to share a LUID on
  every system. Query the device that executes the workload.
- A repaired classifier can activate an existing software-specific test contract. It must not enlarge that contract,
  remove workload assertions, convert skips to hardware passes or explain an unrelated crash without evidence.
- Counterfactual policy tests cover both missing flags, display-only adapters, exact/nearby identities, failed queries
  and conflicting reports. The native census and
  affected compute/raster/RT tests are separate gates; a synthetic descriptor is not a dispatch proof.
- The backend-private LUID query is shared by default-device classification and explicit native adapter tests.
  Exercise a real WARP identity and a verified-unavailable identity without changing the application's default GPU.

The [session](../sessions/2026-09-12-dx12-adapter-classification.md) records prototype, implementation and actual test
evidence as obtained. No performance benchmark or speedup is claimed.

Implementation: [public query](../../engine/gpu/gpu-context-dx12/include/crd/gpu/dx12_context.hpp),
[backend](../../engine/gpu/gpu-context-dx12/src/dx12_context.cpp),
[policy](../../engine/gpu/gpu-context-dx12/src/dx12_adapter_classification.hpp),
[policy tests](../../tests/gpu/gpu-context-dx12/test_dx12_adapter.cpp),
[native census](../../tests/gpu/gpu-context-dx12/device_info.cpp).

## Controlled WARP workload

Microsoft's [D3DConfig tool](https://devblogs.microsoft.com/directx/d3dconfig-a-new-tool-to-manage-directx-control-panel-settings/)
controls DirectX settings for its registered application list. This allows a diagnostic on the real production
default-device path without adding an engine-only forced-adapter branch. The control is persistent user state:
`force-warp` normally remains false; register only the exact census/test executable paths for this experiment.

1. Capture `apps`, complete `device` settings and an XML export before mutation. The recorded experiment requires
   an empty app list and `force-warp=false`; otherwise stop and preserve the existing user profile.
2. Validate both command output and exported XML. D3DConfig was observed returning exit zero while printing an export
   error and writing incomplete XML. Exit zero alone is insufficient preparation for changing settings.
3. Write durable recovery instructions first. Add only the two exact executable paths, then enable force-WARP.
4. Run the native census and require actual Software classification before the scoped CTests. Record selected LUID,
   kernel bits, driver, feature tiers, compiler, source/model/executable identities and complete test counts.
5. In guaranteed cleanup, disable the override and remove only those added paths. Read back the full original app
   and device state and validate the final export. Never clear/reset/import over unrelated user settings.
6. Preserve a failed cleanup/instrument result even when independent readback establishes restoration. A later
   successful run gets a fresh envelope; it cannot rewrite the earlier failure.

The [atomic-emitter session](../sessions/2026-09-12-atomic-abuffer-emitter-repair.md) records final hardware and WARP
results, original-setting restoration and the distinct hosted provider still requiring CI evidence. WARP is a useful
software-device diagnostic; it does not qualify another driver version or replace hardware testing.
