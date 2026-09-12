# Bounded DX12 frame descriptor allocation

<!-- doc-role: reference -->
> Technique reference. Owner: [REPO.3c.5.c](../ROADMAP.md#slice-repo.3c.5.c).

## Parameters and outputs

| Value | Meaning / units | Default and bounds |
|---|---|---|
| Initial page | First shader-visible CBV/SRV/UAV heap, descriptors | 256; positive and no larger than maximum page/budget |
| Maximum page | Largest individual heap, descriptors | 65,536; no larger than native tier-1 heap limit |
| Total slots | Maximum backing capacity owned by one graph, descriptors | 262,144; includes unused page tails |
| Reservation | Complete native command's contiguous range, descriptors | Positive, fits one page; zero commands need no reservation |
| Storage/texture table | Slots actually addressed by the fixed native layout | 1 per single view; indexed storage uses UAV + SRV |
| Compute/RT table | Complete fixed UAV table | 8, padding unused entries with the existing first binding |
| Bindless table | Complete existing texture-array layout | 1,024; storage-plus-bindless reserves 1,025 together |
| Sampled compute | UAV table plus sampled SRV | 9 together |
| Allocator | Page metadata owner | Factory allocator; outlives context/graphs, `try_allocate` can fail |
| Result | Native HRESULT plus checked execution diagnostic | Exhaustion/OOM cancels the unsubmitted frame and makes context invalid |
| Retirement | Earliest legal overwrite/reuse | After the graph's prior submitted fence completed |

These are private provider recording limits, not a new authored binding schema or RAH-2 resource-table implementation.
Native descriptor stride is queried; byte footprint is capacity × that stride, plus allocator-owned page metadata and
driver overhead. There is no timing/performance claim associated with the chosen defaults.

## Why ranges and retirement matter

The old frame heap had 256 slots and unchecked increments, while one existing bindless table addressed 1,024.
A larger constant leaves both exhaustion and lifetime bugs intact. Microsoft's
[descriptor heap overview](https://learn.microsoft.com/en-us/windows/win32/direct3d12/descriptor-heaps-overview)
requires descriptors referenced by queued work to remain intact. A complete draw/dispatch reserves its range before
any root binding, and every subsequent view write consumes only that reservation's checked bounds.

[Setting descriptor heaps](https://learn.microsoft.com/en-us/windows/win32/direct3d12/setting-descriptor-heaps)
invalidates descriptor-table state when a different heap is selected. A range spanning two heaps is therefore invalid:
allocating storage, switching heaps for textures, then using the old storage handle is not a valid draw. Cerid switches
before the command and binds all tables required by that command afterward, including its sampler tables.

The allocator doubles page capacity up to the configured maximum, clamped to remaining backing budget. It can append
to an older page's unwritten tail, but never replaces its published entries. Pages survive recording and submission;
the graph waits for retirement before resetting cursors. Warm frames start in the largest retained page. Native
[shader-visible heap limits and switching costs](https://learn.microsoft.com/en-us/windows/win32/direct3d12/shader-visible-descriptor-heaps)
motivate bounded pages/reuse; reduced stall time has not been measured here.

## Assembly and failure handling

1. Construct graph-owned descriptor storage using its raster factory allocator. Native objects stay inside DX12.
2. At execute() entry, retire any previous graph submission and the selected recording-ring slot before resetting
   descriptor cursors. A failed retirement makes the context invalid and prevents reuse.
3. Validate the operation's existing inputs; reserve the whole native-command range before its first descriptor/root
   use. Graphics, compute, RT, UAV clear and blit paths all use the same checked writer.
4. On heap change bind the resource and sampler heaps together. Materialize views into the reserved range and bind
   the current command's full tables. Padded existing table entries remain initialized.
5. Submit the graph once. A range/allocation failure records its explicit HRESULT, latches context failure and prevents
   submission of the partially recorded frame. No skipped draw is reported as successful execution.
6. Keep pages until retirement, then reuse them. Destroy page metadata through its originating allocator after native
   heap destruction; graph teardown first retires any outstanding submission.

## Regressions and traps

- Test exact capacity, one over, huge/zero requests, failed growth with unchanged counters, and an older page tail.
- Distinct compute buffers expose overwritten queued descriptors; final pixels alone can miss earlier dropped work.
- A storage-plus-bindless draw must reserve both tables together, including padding to the full native table size.
- Exercise clear/blit and deferred repeated frames; changing heaps invalidates inherited tables even if resource state
  remains correct. Pass declarations must match their operations: the blit fixture is a Transfer pass.
- An intentional OOM is expected to produce one explicit execution error. Assert its exact message and zero submissions;
  expecting a silent capture would defeat the failure-reporting contract.
- The native raster CKIR storage node differs from compute BufferDecl/BufferLoad. Use the production stage's vocabulary.
- Choose unambiguous normalized pixel values when testing bindings; UNORM rounding midpoints are not descriptor oracles.

Code: [arena](../../engine/gpu/gpu-context-dx12/src/dx12_frame_descriptors.cpp),
[limits/range contract](../../engine/gpu/gpu-context-dx12/src/dx12_frame_descriptors.hpp),
[native consumers](../../engine/gpu/gpu-context-dx12/src/dx12_raster_context.cpp),
[regressions](../../tests/gpu/gpu-context-dx12/test_dx12_validation.cpp).
[Dated qualification](../sessions/2026-09-13-dx12-descriptor-allocation.md) records actual tuples, counts and limits.
No benchmark board is implied: this is correctness and bounded-allocation work.

## Mixed shader bindings when changing pages

The actual impostor capture exposed stale tables from a previous shader even though its pixels passed on hardware.
A focused sampled→bindless fixture reproduced diagnostic 554 with distinct page sizes. On each heap change, explicitly
clear both root signatures before the next command establishes its required roots. Equal layouts may share a native
signature; redundantly setting that signature preserves bindings. Microsoft defines [root binding lifetime](https://learn.microsoft.com/en-us/windows/win32/direct3d12/using-a-root-signature)
and [table invalidation after heap changes](https://learn.microsoft.com/en-us/windows/win32/direct3d12/setting-descriptor-heaps).
No descriptor contents are overwritten, no extra submission is added, and no diagnostic is filtered. The expanded
graphics test verifies the earlier sampled pixel plus every bindless/blit pixel through three frames on hardware/WARP.
