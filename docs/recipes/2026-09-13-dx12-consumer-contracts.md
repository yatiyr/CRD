# DX12 consumer contracts: shader interfaces, caches and attachment clears

<!-- doc-role: reference -->
> Implementation reference. Live owner: [REPO.3c.5.d](../ROADMAP.md#slice-repo.3c.5.d).
> Rules: [AGENTS](../../AGENTS.md). Measured correctness evidence: [session](../sessions/2026-09-13-dx12-consumer-diagnostics.md).

## Parameters and limits

| Input | Meaning / units | Default and bounds |
|---|---|---|
| CKIR stage outputs | Authored location and interpolation per output | At most `kMaxStageOutputs`; stable increasing-location declaration order in HLSL VS/DS/MS |
| `FgImageDesc.optimized_clear.color` | Expected attachment clear, four floats in view-format value units | Opaque black for direct graph callers; authored plans derive their first known Clear |
| `optimized_clear.depth` | Expected depth clear, normalized depth | 0 for direct graph callers; authored plans carry the actual depth, including conventional 1 or reversed-Z 0 |
| Fullscreen `clear_color` | Actual authored RGBA clear operation | Opaque black; forwarded through cooking, CEIR and execution |
| Pipeline-cache key count | Number of successfully stored native PSOs | At most 65,536; subsequent new pipelines run uncached |
| Native cache payload | Opaque provider data, bytes | Nonempty and at most 256 MiB on import; unsupported serialization returns empty |
| Cache envelope | Four little-endian u64 fields, then keys and native bytes | 32-byte header; exact lengths, version, duplicate-key and checksum checks |
| Cache ownership | Context caller allocator and serialized recording owner | Owned input copy; cache/pipeline calls on one context require caller serialization |

## Why these changes exist

The real impostor workload initially produced correct hardware pixels while emitting shader-linkage and descriptor
errors. Four B18 numerical tests passed their math but emitted cache lookup warnings. RT-4 emitted scratch-state
warnings. A pixel or numerical oracle and a native validation oracle prove different requirements; both must pass.
The shared capture helper spans device creation through teardown and reports counts even when a test assertion throws.
The [descriptor repair](2026-09-13-dx12-frame-descriptors.md) owns heap rebinding; this recipe owns the other repairs.

## Shader interfaces and RT scratch

The velocity vertex cook appends a flat fade at location 4 after smooth clip outputs at locations 5 and 6. The fragment
emitter already orders inputs by location. One bounded stable insertion sort now orders HLSL output declarations in
the vertex, domain and mesh emitters. It does not reorder graph arithmetic, change locations, alter interpolation or
replace an authored shader. The exact-pixel regression deliberately uses mixed interpolation and noncontiguous
locations in a different declaration order. The observed native linkage diagnostics, rather than a visual guess,
established the repair requirement.

RT scratch buffers start COMMON and explicitly transition to UAV before each acceleration-structure build. The AS
result remains in its required acceleration-structure state. This follows the application's responsibility for
[resource states and barriers](https://learn.microsoft.com/en-us/windows/win32/direct3d12/using-resource-barriers-to-synchronize-resource-states-in-direct3d-12).

## Indexed native pipeline libraries

Microsoft's [LoadComputePipeline contract](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12pipelinelibrary-loadcomputepipeline)
reports a missing name or incompatible description as an error. The observed SDK emits diagnostic 971 for an ordinary
cold lookup. Cerid now keeps an allocator-owned set of successful stores: known key → load and validate the description;
absent key → create and store. A failed indexed load remains an explicit diagnostic. No warning is filtered.

The key mixes DXIL FNV-1a-64 with binding count and push-constant byte size. The version-1 envelope contains magic
`CRDXPC01`, key count, native byte count and an FNV-1a-64 checksum of the payload, followed by u64 keys and native bytes.
All integers are explicitly little-endian; no padded C++ struct is copied. This is disposable derived cache data,
not an authenticated or portable engine asset. Legacy raw-native blobs are rejected and regenerated.

Import validates bounded lengths, exact consumption, checksum and unique keys before constructing a replacement.
Malformed data or native driver/adapter incompatibility returns false while preserving the working cache. Empty input
explicitly resets to a new empty library. The [native API borrows its input storage](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12device1-createpipelinelibrary):
Cerid owns a copy, destroys the old library before replacing its blob, and declares members so native destruction
precedes backing-storage destruction. Tests exercise cold/warm/new-key execution, repeated replacement, eight malformed
inputs, retained usable pipelines and explicit reset. A checksum does not authorize untrusted input.

## Clear values from authored CEIR

A [D3D12 creation clear value](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ns-d3d12-d3d12_clear_value)
is an optimization hint with the attachment view's typed format. It is not an initialization command. The native
clear still executes with its requested value even when the hint differs; that can emit a performance warning.

1. Cooked fullscreen `clear_color` now fills the existing `FullscreenBuildDesc.clear`, and the builder emits all four
   CEIR attributes. Both links had previously dropped the value.
2. Pure color/depth attachment decoders are shared by execution and load-time resource planning. No second parser of
   clear attributes exists in the frame runtime.
3. `FramePlans.clear_hints` follows resource indices. It inspects the actual BeginRender attachments once at load,
   follows existing color/depth/shared-depth slot routing, ignores Load/DontCare and keeps the first known Clear for
   each component. Buffer writes do not consume color slots. Multiplicative MRT blends retain their identity clear;
   uint clear hints follow native value conversion. Unknown/custom commands do not invent a hint.
4. The recorder passes those hints into transient and persistent image descriptions. DX12 uses them for placed and
   committed resource creation and graph-owned companion depth. Standalone color/depth targets use their public
   opaque-black/depth-1 defaults. Other APIs may ignore optimization metadata; semantic clear operations are unchanged.
5. A persistent image retains its history and original creation hint when only the hint changes. Later different
   clears remain legal; do not destroy temporal history just to make a performance diagnostic disappear.

The cooked-asset GPU regression draws a deliberately partial triangle and checks both the rendered center and the
exact authored corner color on transient and persistent images through three frames. The real impostor gate also
retains its nonzero cull/draw counts and visible-pixel contrast checks. No clear command, failure or skip is hidden.
There is no claim that every possible sequence of changing clears can match one immutable creation hint.

## Source boundary and qualification

The separate `Dx12ComputeContext::Impl::bind_compute` still has a fixed 8,192-descriptor heap with unchecked cursor
growth. This is a **source finding**, not a reproduced failure of these workloads. Its dynamic bounds/diagnostics
owner is [RAH-6.b](../ROADMAP.md#slice-rah-6.b); it is distinct from the now-checked raster frame descriptor allocator.
Standalone provider validation and wider typed-attachment migration retain their existing owners and design gates.

Code: [HLSL interfaces](../../engine/gpu/kir/include/crd/kir/ckir_hlsl.hpp),
[pipeline cache](../../engine/gpu/gpu-context-dx12/src/dx12_compute_context.cpp),
[RT scratch](../../engine/gpu/gpu-context-dx12/src/dx12_ray_tracing_context.cpp),
[native images](../../engine/gpu/gpu-context-dx12/src/dx12_raster_context.cpp),
[attachment decoders](../../engine/execution/ceir-gpu/src/render_materialize.cpp),
[fullscreen CEIR](../../engine/execution/ceir-gpu/src/render_fullscreen_build.cpp),
[plan/record bridge](../../engine/assets/frame-cook/src/frame_runtime.cpp).
No speedup or benchmark victory is claimed. Hardware, runtime versions, real exits, test counts and remaining
published-provider evidence are recorded in the linked session; emitter compilation alone does not qualify a platform.
