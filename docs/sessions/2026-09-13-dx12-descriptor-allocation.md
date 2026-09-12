# DX12 frame descriptor allocation

<!-- doc-role: historical -->
> Dated evidence. Live state: [ROADMAP](../ROADMAP.md); rules: [AGENTS](../../AGENTS.md).

## Scope and implementation

Continued the authorized loop at REPO.3c.5.c after [resource-state qualification](2026-09-13-dx12-resource-state-repair.md).
The [repair contract](../design/dx12-workload-repair.md) requires bounded contiguous reservations for all native frame
descriptor paths, complete table binding, retirement, one submission and explicit failure. This is a generic DX12
provider repair, not RAH-2 acceptance, a new authored algorithm or a geometry/physics change. Advisor unavailable.

Added graph-owned descriptor pages and replaced every frame-heap cursor write with checked range consumption.
Twenty-seven native command/recording entry points reserve their complete ranges before descriptor/root use. Storage
and its texture tables cannot be split by a heap switch. The existing compute-table writer is reused by plain compute,
sampled/indirect compute and RT; UAV clear and blit use the same bounds. Pages append only to unwritten space and may
reuse older tails. Warm frames start at the largest retained page after retirement. Page allocation uses the allocator
passed to the raster factory, which previously ignored that argument. Null allocator is explicitly rejected.

Exhaustion or metadata/native allocation failure records an explicit HRESULT, invalidates the context and cancels the
entire unsubmitted recording. Pages remain alive until the graph's fence has retired. Private recording defaults:
256 initial slots, 65,536 per-page maximum, 262,144 total backing slots per graph. These limits do not alter authored
resource schemas. [Technique, rationale and source references](../recipes/2026-09-13-dx12-frame-descriptors.md).

The existing source synchronizer registered the new private header/TU; its journal, membership overrides and unrelated
prior entries were preserved. No source directories were moved. The pre-edit provider/test snapshot is retained under
ignored `build/research-dev-workflow-20260912/before-descriptor-repair/`. No commit, push or staging.

## Qualification

Human HEAD remains `7201a7b818ee6b363aac6cfbf97f94fa96d8fb85`; these changes are unpublished. One Windows Debug
configuration, MSVC 19.51.36246, two serialized build workers; NVIDIA 4070 Ti SUPER 10de:2705 / 32.0.15.9579.
Ignored evidence root: `build/research-dev-workflow-20260912/`. Wrappers check synchronizer readiness and stable
source/CMake model identities, retain exact commands, bounded native exits, inventories, JUnit and sealed manifests.

- `dx12-resource-states-004525`: new provider integration built; previous four resource-state cases **4/4 pass**.
- `dx12-descriptors-005338`: allocation ranges and queued compute passed. The new graphics fixture used a compute
  BufferLoad in a raster stage and failed compilation; the intentional-OOM fixture incorrectly expected zero errors.
  Corrected the harness to use raster StorageLoad and require the exact expected execution diagnostic.
- `005630`: three cases passed; graphics exposed its incorrectly declared raster/blit pass and an ambiguous UNORM
  midpoint (127 vs 128). Used the proper Transfer pass and distinct 51/102/153/204 byte values. No existing renderer
  oracle, tolerance or diagnostic filter was changed. These failures do not establish an engine state/precision defect.
- `005912`: all four descriptor cases **4 selected/reported/executed/passed**, zero skipped/disabled/failed, exit 0.
  Range/overflow/failed-growth/old-tail case: 47 assertions. Distinct queued compute plus UAV clear: 1,314 assertions,
  32/33/300 dispatch workloads over three deferred frames each. Storage + full bindless tables and blit: 313 assertions,
  exact independent pixels over three frames. Positive captures: zero warnings/errors/instrumentation failures.
  Intentional OOM: 134 assertions, precisely one expected execution-error message, invalid context, zero submissions
  for the cancelled frame, prior output preserved, all allocator pages released.
- `dx12-descriptor-consumers-010235`: five affected targets rebuilt (DX12 tests, scene tests, census, work-graph tests,
  sandbox), **50 selected/reported/executed/passed**, zero skipped/disabled/failed, exit 0. Includes previous 31 cases,
  four new descriptor cases, authored bindless/deferred/tessellation/mesh/RT/indirect/visibility/WBOIT, any-hit and full
  SBT consumers, storage-driven tessellation/mesh and indexed draw consumers. Existing authored consumers do not yet
  assert the new capture; their validation-silence integration remains REPO.3c.5.d.

- `dx12-descriptor-tidy-010601`: LLVM-20 parsed all five changed C++ files with zero diagnostics, exit 0.
- `warp-010748-be31f0`: independent adapter census **1/1 pass**, then validation/state/descriptor regressions
  **16 selected/reported/executed/passed**, zero skipped/disabled/failed, exit 0. Microsoft Basic Render Driver
  1414:008c / 10.0.26100.8972, engine classification Software. Positive workloads emitted zero warnings/errors;
  negative tests required their exact expected diagnostics. Original application list and every device setting
  were restored and verified against the exported pre-run state.

Local qualification is complete. No performance improvement or new platform support is claimed. Full published CI
on the changed revision is still required after the user commits/pushes; the row retains Needs CI. Continue with the
remaining native diagnostics in REPO.3c.5.d while that publication gate remains open.

## CI observation

[Run 34714148010](https://github.com/yatiyr/CRD/actions/runs/34714148010) still tests the human HEAD. At 01:00 local,
all six Linux lanes and both repository checks passed; Windows Shipping was still running. Other Windows test lanes
and tidy failed as recorded in the previous sessions. Release job 103608114996's direct log endpoint returned 404;
the CLI fallback explicitly said logs await run completion. Needs CI output; available local work continues.

## Consumer validation reopened the descriptor owner

The following REPO.3c.5.d capture integration (`dx12-full-consumers-011743`) reproduced five native diagnostic 554
errors in the actual impostor workload: descriptor handles referred to a heap different from the one currently bound.
Its previous output-only hardware oracle still passed, which did not qualify descriptor correctness. The six instrumented
consumer cases all executed and failed their new silence assertions (none skipped). B18 cache-miss warning 971, RT-4
buffer-creation warning 1328, and impostor linkage 660 / clear hints 820–821 remain owned by .d. Return to .c for the
earlier descriptor requirement; retain the successful bounded allocator/state evidence above without claiming closure.

The mixed-table reproducer `dx12-descriptors-012307` selected/executed four cases: three passed, while a sampled draw
followed by bindless growth produced six diagnostic 554 errors despite correct pixels. D3D12 may deduplicate identical
root layouts; selecting the same root again preserves old bindings. Changing heaps invalidates table state, so the
provider now explicitly resets both graphics/compute root signatures on a page switch and each verb rebinds its inputs.
`dx12-descriptors-012439`: **4/4 pass**; expanded graphics case 333 assertions, zero positive warnings/errors.
`warp-012630-973674`: independent census **1/1**, descriptor regressions **4/4**, zero skips/disabled, exit 0; original
application list and all device settings restored and verified. `dx12-consumer-capture-tidy-012745`: all six changed
provider/test/helper files parsed cleanly with LLVM-20. Return .c to Needs CI and resume .d's remaining diagnostics.
The final affected-consumer run belongs to .d after those diagnosed failures are repaired.
