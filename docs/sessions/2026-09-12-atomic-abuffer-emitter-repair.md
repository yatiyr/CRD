# Atomic A-buffer and compute-emitter repair

<!-- doc-role: historical -->
> Dated evidence. Live ownership: [ROADMAP](../ROADMAP.md). Rules: [AGENTS](../../AGENTS.md).

## Scope and mechanism

Continue the authorized repository/CI programme at REPO.3c.6, following the
[classification session](2026-09-12-dx12-adapter-classification.md). HEAD remained the human-published
`2ed89c487215c91ffc2f667c19b2d97f01f5598c`; previous uncommitted work was preserved. No commit/push, new renderer
programme or geometry/physics algorithm work occurred. The advisor capability is unavailable; no review is claimed.

The actual atomic A-buffer case failed locally under WARP with Windows exception `0xc00000fd` (stack overflow).
Observed CPU progress ruled out a fabricated hang. Temporary stage markers showed capture pipeline creation completed
and resolve pipeline creation failed before any dispatch. The static, non-atomic A-buffer baseline passed separately.
Four-layer resolve HLSL expanded to 413,859 bytes/8,966 loads because the shared gather/sort graph was recursively
inlined. Explicit canonical `Materialize` statements reduced that diagnostic artifact to 7,859 bytes/24 loads and
the same exact-output WARP test passed. Markers were removed using this session's saved preimage, not Git HEAD.

Cross-emitter qualification then found CUDA/MSL/WGSL still hoisted consumers before the new snapshots. GLSL also used
a storage scalar type for a Boolean local, producing invalid compilation followed by a null-pipeline failure in the
existing Vulkan harness. Extracted the existing GLSL/HLSL ordering analysis into a shared helper used by all five
compute emitters; corrected local Boolean types and duplicate declaration tracking. No buffer layout, math order,
provider precision bar, capture barrier or image oracle changed. The
[recipe](../recipes/2026-09-12-kernel-snapshot-ordering.md) teaches the full mechanism and regression contract.

The new source-size case covers 1/4/8 layers on all five emitters and fails before continuing into an oversized case.
A new real CUDA case reuses the existing shared capture/resolve and CPU oracle, verifies all 3,072 RGB values exactly,
and checks actual non-background output. Missing CUDA is an explicit CTest skip, never a qualification pass. Its target
now links the existing kir interface and shared test include; the pre-existing Linux sanitizer policy was preserved.

## Evidence, including failed instruments

All paths in this section are under ignored `build/research-dev-workflow-20260912/`. They are local diagnostics,
not portable published artifacts. Dated conclusions are retained here; exact-revision CI remains necessary.

| Envelope / log | Observation |
|---|---|
| `dx12-classification-204218` | Final preceding native Debug classification run: 10 selected/executed/pass, zero skips, matching source/model identities; integrity verified |
| `warp-205047-7a9dc9` | Ten classification/B18/coverage/RT oracles passed under local WARP. D3DConfig returned success while printing an export error and leaving truncated XML; original evidence retained |
| `warp-205212-ec3d28` | Static exact-reference A-buffer baseline passed; distinct from the failing atomic case |
| `warp-205319-76fec6` | Atomic case reproduced stack overflow. Native settings restoration ran, but the supervisor encountered a transient status-file PermissionError; retained restore-failure outcome, with final independent settings comparison confirming restoration |
| `warp-205750-3938ec` | Stage instrumentation localized stack overflow to resolve pipeline creation; no dispatch yet |
| `warp-210326-f3af50` | Explicit snapshots made the actual WARP atomic test pass its unchanged exact reference |
| `atomic-final-211005` | Missing/stale CMake File API metadata blocked qualification; wrapper then requested the canonical query and configured |
| `atomic-final-211052` | Intermediate 21-test run: 19 passed, source-size regression and Vulkan atomic case failed; this drove shared ordering and Boolean-type repairs |
| `atomic-final-212043` | Guard rejected overlap with a live source-synchronizer writer before build; waited for finalized baseline, without bypass |
| `atomic-final-212512` | Five affected targets built; **22/22** selected/reported/executed/pass, zero skips/failures; source/model unchanged and envelope integrity verified |
| `warp-212842-e1b539` | Final Ninja Debug executables: **11/11** selected/reported/executed/pass, zero skips/failures, including atomic resolve; source/model/executable hashes unchanged, full original app/device settings restored and integrity verified |
| `emitter-consumers-213027` | **37/37** adjacent consumer cases passed, zero skips/failures; source matched the qualified build, model unchanged, integrity verified |

The 22-test selection covered all five source emitters, Vulkan and DX12 B17-b/c variants, three classification cases,
B18 hair/fur/scattering/Huang, inner coverage, RT-4, the device census and CUDA atomic output. The adjacent 37 cases
covered shared-memory CPU oracles, snapshot-overwrite semantics, five-emitter kernels, serialization/reflection,
arithmetic completeness, GLSL/HLSL compilation, Vulkan shared-memory/transpose dispatch and scattering normalization.
Selections overlap; do not sum them as unique tests. Hidden benchmark/generator cases were excluded.

Hardware: NVIDIA GeForce RTX 4070 Ti SUPER (10de:2705), Windows driver 32.0.15.9579; classifier Hardware, DXGI flags 0,
kernel bits 0x31b/SoftwareDevice 0. Local WARP: BasicRender 1414:008c, driver/D3D12Core/d3d10warp 10.0.26100.8972,
flags 2, kernel bits 0x105/SoftwareDevice 1, classifier Software, wave width 4 and RT tier 1.1. Hosted CI's
flag-0 BasicRender driver 10.0.26100.33296 is a different tuple, still awaiting the human-published repair.

The primary build was existing `win-debug`, MSVC 19.51.36246, CMake 4.3.2, two workers, scoped CTest with 180-second
per-case budgets. LLVM-20 incremental analysis parsed and passed all changed emitter/order/OIT headers and the kir
and CUDA test TUs. Earlier classifier files had already passed. Logs: `kernel-order-tidy-final.log`, preceding
`kernel-order-tidy.log`, `cuda-atomic-tidy.log`, and `atomic-tidy-final.log`. No whole-repository/configuration sweep ran.
Artifact sizes and diagnostic durations are not performance claims; no benchmark speedup is asserted.

## Supervisor status-read repair

The atomic diagnostic exposed a transient PermissionError reopening the atomically published native status file.
The source of that transient denial was not established. The supervisor now retries only that read for at most one
second inside the existing overall budget, recording retries and the error. It never reruns the native command.
Persistent denial remains instrument failure 125 with process-tree cleanup. Two adversarial regressions verify a
command executes exactly once and retains exit 7 through transient denial, and persistent denial cannot become green.
Windows tooling: **48/48** passed. WSL Ubuntu/Python 3.12.3: **46 applicable passed**, two Windows-only skips, out of
48 declared cases. Logs: `status-read-windows.log`, `status-read-linux.log`. This does not close remaining external
build coordination or portable C++ analysis obligations under REPO.DEV.3b.

## Handoff

REPO.3c.6 retains its exact failing-provider/publication gate; local source/runtime proof alone cannot close it.
REPO.3c.4.a, B18/RT/coverage and the wider repository gates similarly retain their full obligations. Metal/WebGPU,
other hardware and native Linux device evidence are not implied by emission or this workstation's results.
The next discriminating repository work is the separate REPO.3c.10 ASan pipeline/bindless failures and the remaining
impostor case, followed by remaining developer-workflow/CI children. Do not reopen renderer implementation before
RAH-0/ADR-0107 review. User alone publishes; then inspect the exact resulting CI revision and every affected lane.

## Documentation and workspace close-out

Updated the sole master row, context, compact memory, recipe index, shader-system entry, developer contract and
research/census references. Corrected the stale atomic-buffer binding comment and avoided attributing the observed
status-file access denial to an unproven external process; these final source edits were comments only.
AGENTS, START_HERE, PRINCIPLES, SANITY, BUILDING and CODING were inspected; their current rules remained applicable.
The first documentation check caught a wrong ADR filename and an over-budget context pointer; both were corrected.
Final check: **859 rows, 1,011 documents, 8,619 local links, 210 D-007 and 16 v17 routes passed**. Repository hygiene
checked all 96 module layouts/ignore/test contracts and passed; all six repository-tool fixtures and `git diff --check`
passed. Synchronizer status showed a live watching process, finalized baseline, no incomplete transaction and no
active generation. MEMORY remained below its 3,000-byte budget. Original failed envelopes were preserved.
