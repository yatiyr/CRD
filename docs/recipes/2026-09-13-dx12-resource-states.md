# DX12 resource states across indirect draws, aliasing and repeated frames

<!-- doc-role: reference -->
> Technique reference. Owner: [REPO.3c.5.b](../ROADMAP.md#slice-repo.3c.5.b).

## Parameters and outputs

| Value | Meaning / units | Default and constraint |
|---|---|---|
| Native resource identity | One actual buffer/image, irrespective of API roles | Deduplicate before emitting a transition |
| Indirect roles | Scene/index data, arguments, optional count | Count may be absent or share any other role |
| Parked storage state | State before/after an in-frame indexed draw | UAV; combine only compatible read bits during the draw |
| Image live state | Last native image state | Initialize at creation, retain across execute() |
| Alias slot | Shared placed-resource heap | One active native resource; activation precedes use |
| `no_alias` | Dedicated transient memory | False by default; true prevents reuse in either direction |
| Default optimized clear | Creation hint, normalized color/depth | Opaque black color / reverse-Z depth 0; not a replacement for authored clears |
| PSO identity | Shader pair plus actual pass attachment/state configuration | Create on first actual use; existing bounded cache remains |
| Regression workload | Commands/frames, not timing | Six role arrangements × color/depth × three frames |

## Native model

An API role is not a resource. If one buffer supplies indices and indirect arguments, emit one transition whose
destination is the union of compatible read states. It cannot simultaneously remain a UAV writer.
[Microsoft's state/barrier contract](https://learn.microsoft.com/en-us/windows/win32/direct3d12/using-resource-barriers-to-synchronize-resource-states-in-direct3d-12)
defines compatible read combinations, implicit buffer promotion/decay and the different depth-image rules.
For each distinct resource r, compute read(r) as the bitwise union of its active read roles, transition UAV → read(r),
record the draw, then read(r) → UAV. This balanced interval allows the next compute/readback operation to use the
provider's parked-state contract. It does not claim that native buffers cannot decay at submission boundaries.

Placed memory is separately governed by activation. Microsoft's
[CreatePlacedResource contract](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12device-createplacedresource)
requires alias activation and color/depth metadata initialization. State tracking cannot substitute for activation.
Cerid records previous-active → next-active alias barriers and discards newly activated RT/DS metadata in the writable
state. The authored pass still supplies its clear or content-producing write. Native image state is initialized only
when its resource is created; a new frame does not recreate a live allocation. Resource recreation/reset handles that
lifetime explicitly. Persistent images retain their state in the persistent registry.

## Assembly

1. Allocate storage in COMMON, copy initial contents, then park in UAV. Build placed image slots by disjoint lifetimes;
   honor capacity/alignment and prevent every other resource from reusing a pinned slot.
2. Create images with their actual initial state and default clear hint. Save that state next to native ownership.
3. At pass entry activate any different slot occupant. Initialize attachment metadata, then transition actual state
   to the pass's declared access. Repeated execute() starts from the state left by previous native work.
4. Indexed color/depth draws coalesce role identities and restore each resource once. The next real compute dispatch
   and download must observe the correct contents; a silent log alone cannot establish execution.
5. Materialize a graphics PSO from the actual pass configuration. SV_Depth programs must not be tested during loading
   against an invented color-only PSO. Program-owned shader/root readiness is distinct from each PSO's native result.
6. Imported targets return to COMMON; persistent images use their registry state. Retain alias slot/native resources
   until submission retirement and release them together on rebuild/reset/destruction.

## Traps and evidence

- Resetting a CPU enum does not reset native image state: second-frame error 527 exposed this directly.
- A transition per argument duplicates barriers when scene/args/count share storage. Union roles by identity first.
- Pinning only the incoming resource still permits later allocations to reuse its heap; the slot must retain the pin.
- Newly created placed resources are not automatically active; absent alias barriers can hide behind correct pixels.
- Default clear hints do not qualify nondefault authored clear colors. Their integration and other consumer warnings
  remain owned by REPO.3c.5.d. No warning is filtered to make these tests pass.
- The graphics-program factory no longer creates a speculative color-only PSO. Actual-use PSO creation failure is
  reported through the execution diagnostics; this is not evidence that all shader/attachment combinations are valid.

Implementation: [DX12 raster provider](../../engine/gpu/gpu-context-dx12/src/dx12_raster_context.cpp).
Production-path regressions: [validation tests](../../tests/gpu/gpu-context-dx12/test_dx12_validation.cpp),
[adjacent frame-graph gates](../../tests/gpu/gpu-context-dx12/test_dx12_frame_graph.cpp).
[Session and reproducible qualification](../sessions/2026-09-13-dx12-resource-state-repair.md).
This is correctness work; no performance improvement was measured and no benchmark board is implied.
