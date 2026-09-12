# DX12 ASan pipeline and bindless investigation

<!-- doc-role: historical -->
> Dated evidence. Live owner: [REPO.3c.10](../ROADMAP.md#slice-repo.3c.10). Rules: [AGENTS](../../AGENTS.md).

## Purpose and initial evidence

Continue the approved repository programme after the [atomic-emitter repair](2026-09-12-atomic-abuffer-emitter-repair.md).
The previous turn made verified progress. HEAD remains human-published `2ed89c487215c91ffc2f667c19b2d97f01f5598c`;
preserve its uncommitted fixes. No commit/push, unrelated algorithm changes or renderer implementation. Advisor is
unavailable; no review claimed. The source synchronizer reported a finalized baseline and no active transaction.

The published ASan lane contains five additional failures. Four RT-pipeline cases report an 8-byte memmove read
immediately beyond D3D12Core allocations of 2,924, 2,568, 2,924 and 3,788 bytes respectively, through d3d10warp during
`CreateStateObject`. The frosted-glass panel-mask case reports an access violation through d3d10warp at
`OMSetRenderTargets` inside `record_bindless`. These are distinct observations, not proof of a common mechanism or
driver fault. The full published trace remains in ignored `build/research-dev-workflow-20260912/ci-2ed89-asan.log`.

Initial source inspection: the DXR subobject array holds at most six libraries plus four other entries; pointers refer
to live local descriptors and caller-owned program bytes during synchronous creation. This arithmetic alone does
not establish valid shader content or all lifetime contracts. Inspect actual inputs and validation evidence before
patching. The existing local ASan executable predates current fixes and cannot be used as fresh evidence without
rebuilding the affected target. Reproduce the five exact cases plus the selected-device census under ASan, then
compare local hardware with controlled WARP using the existing reversible diagnostic procedure.

## Reproductions completed before the strict-order amendment

Evidence is under ignored `build/research-dev-workflow-20260912/`; original failed/incomplete envelopes remain intact.
The first bounded ASan wrapper built the DX12 tests/census but selected eight cases where six were expected. The broad
F6 regex also included tessellation/culling, and three intended cases belong to `crd-scene-render-tests`. The selection
guard stopped before CTest; this was an instrument-scope error, not eight executed failures. The corrected wrapper
used six exact names and rebuilt all three executable owners.

`dx12-asan-baseline-214544` selected/executed/passed **6/6**, zero skipped, on the default NVIDIA RTX 4070 Ti SUPER
under current MSVC ASan. Source and model identities remained unchanged. The selected tests were the native census,
REN-38 any-hit, REN-38-F13 intersection/callable, CEIR-19b RT shadows, REN-38-F6 RT pipeline and CEIR-31b frosted mask.

`warp-214805-230d76` used the existing reversible D3DConfig diagnostic with the three exact executable paths. All six
executed: census passed and the five ASan cases failed, zero skipped; CTest exit 8. Original app/device settings were
restored and verified. Four RT cases reproduced an 8-byte read immediately beyond D3D12Core allocations (2,924,
2,568, 2,540 and 2,568 bytes) through d3d10warp at offset 0x180137745. Frosted-glass reproduced the access violation
at d3d10warp offset 0x1801db071 during `OMSetRenderTargets`. Local runtime/driver 10.0.26100.8972 differs from hosted
10.0.26100.33296; reproduction does not make the tuples interchangeable. No suppression, skip or oracle change occurred.

## SDK-only RT reduction

`dxr-asan-probe/run-215336` contains a tiny standalone CMake/MSVC ASan program linking only Direct3D/DXGI SDK APIs,
with no Cerid engine, scene or frame graph. It compiles three lib_6_3 shaders into owned DXC blobs, keeps seven state
subobjects and all descriptors alive, enables the D3D12 debug layer and registers an unfiltered InfoQueue1 callback.
Immediately before synchronous CreateStateObject the instrument reported zero errors/warnings. Library sizes were
2,272, 2,484 and 2,860 bytes.

Explicit WARP creation reproduced the same 8-byte read exactly beyond a 2,272-byte D3D12Core allocation at the same
WARP offset, before GPU dispatch. Configure/build passed; the bounded probe exited 1 with the retained ASan trace.
This isolates that observation outside Cerid's IR/lifetime implementation. It is evidence for further runtime/provider
comparison, not a verified general driver diagnosis or permission to suppress ASan. The hardware arm and a newer
app-local WARP arm were considered but not run. Source/log: `dxr-asan-probe/main.cpp`, `dxr-sdk-probe.log`.

## Source concerns retained under REPO.3c.10

Inspection found no engine DX12 debug-layer/InfoQueue capture mechanism; the frosted-glass test explicitly relies on
pixels and no crash. The frame graph creates overlapping placed resources for disjoint lifetimes but emits no aliasing
barriers in `dx12_raster_context.cpp`. Its existing transient-alias test builds and checks memory sizes without executing
the graph. These are concrete source/coverage observations; their causal connection to the frosted crash is unproven.
When this row becomes eligible, establish proper validation evidence and inspect transitions, activation/initialization,
buffer/depth handling and repeated execution through real consumers before choosing a fix. Do not replace the authored
algorithm path. No source implementation change for these concerns was made in this investigation.

The user's [strict-order amendment](2026-09-12-strict-slice-order.md) subsequently parked this row. Its retained scope
and evidence are owned by the sole master table; this historical note cannot authorize out-of-order continuation.
