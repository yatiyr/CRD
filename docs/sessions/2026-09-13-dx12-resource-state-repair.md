# DX12 resource-state repair and continued CI review

<!-- doc-role: historical -->
> Dated evidence. Live state: [ROADMAP](../ROADMAP.md); rules: [AGENTS](../../AGENTS.md).

## Scope and order

Continued the user's requested repository loop after the [validation foundation](2026-09-12-dx12-validation-foundation.md).
REPO.3c.5.b owns generic buffer/image/alias lifetime repairs under the [repair contract](../design/dx12-workload-repair.md).
No renderer algorithm, tolerance, authored asset, geometry or physics change. No advisor capability was available.
Only the user commits/pushes; all new repairs remain unpublished at human HEAD
`7201a7b818ee6b363aac6cfbf97f94fa96d8fb85`. Two build workers; one Windows Debug configuration.

## Reproduced defects and implementation

The indexed color indirect path left scene/argument/count buffers in read states before the next compute access.
Its depth counterpart duplicated transitions when roles shared a resource. Both now coalesce by native resource,
combine compatible read bits and restore the distinct resources once. New real dispatch/readback tests cover no
count, distinct count, args=count, scene=count, scene=args and all shared, on both color and depth paths.

Repeated execute() reset image tracking to COMMON while live placed images remained in COPY_SOURCE. State now lives
until resource recreation. Placed resources activate through native alias barriers; color/depth activation initializes
attachment metadata by discard before authored writes. Pinning now also prevents later resources reusing the pinned
heap. Storage-buffer creation uses COMMON; default color/depth resources carry matching default clear hints.

A sampled-depth test exposed two warning-680 PSOs: program construction prematurely invented a color-only pass for
an SV_Depth shader. Graphics PSOs now materialize against the actual pass attachments/state. Owned program bytes/root
readiness is distinct from attachment-specific PSO qualification; creation failures enter the checked diagnostic path.
Mesh/task/tessellation factory behavior is not claimed as newly qualified by this change. Nondefault clear hints and
remaining consumer diagnostics retain their REPO.3c.5.d owner. [Technique](../recipes/2026-09-13-dx12-resource-states.md).

## Qualification evidence

Ignored evidence root: `build/research-dev-workflow-20260912/`. Guarded wrappers check native synchronizer readiness,
retain source/model identity, commands, bounded exit codes, selected/JUnit counts and sealed manifests.
Same workstation/runtime tuple as the validation-foundation session: NVIDIA 4070 Ti SUPER 10de:2705, driver
32.0.15.9579; local WARP 1414:008c / 10.0.26100.8972. No performance benchmark or broader platform result is claimed.

- `dx12-resource-states-000055`: reproduced native state error 527 on repeated indirect access.
- `000245`: output oracles passed but 36 native creation/clear warnings kept validation red. Creation fixes followed.
- `000730`: reproduced the second-frame color image state mismatch; `000954` passed color alias and pin sections.
- `001245`: three cases passed; depth outputs passed but two premature-PSO warnings failed capture silence.
- `001629`: all four cases passed after actual-pass PSO creation.
- `002330`: final expanded color/depth role coverage, **4 selected/reported/executed/passed**, zero skips/failures,
  exit 0. Three repeated frames per workload, exact compute values, visible color/sampled-depth oracles, one graph
  submission, alias memory checks; capture through teardown reported zero warnings/errors/instrument failures.
- Incremental LLVM-20 `dx12-resource-and-work-tidy-002807` parsed the raster implementation and both work-graph tests
  clean. It rejected eight grouped declarations in the new validation test; these were split before final checks.

- `dx12-resource-consumers-003159`: five affected DX12 targets rebuilt, **31 selected/reported/executed/passed**,
  zero skipped/disabled/failed, exit 0. Includes the validation/state suite, both existing actual impostor cases,
  REN-40-A indirect, REN-3.1/3.2 sampled depth, persistent reset, compute, present/resize, authored copies and RT-1/3/6.
  Existing impostor/adjacent cases do not yet assert native capture silence; that consumer integration remains .5.d.
- `dx12-resource-final-tidy-003334`: corrected validation test and the frame-graph comment update both parsed clean;
  exit 0. Combined with the earlier clean raster/work-graph parses, all five changed C++ files are qualified.
- `CERID_DIAGNOSTIC_BUILD=build/win-debug`, `run-warp-diagnostics.py '^DX12 validation|^DX12 resource states'` →
  `warp-003518-667f70`: **12/12 pass** plus independent software census **1/1 pass**, zero skips/failures, exit 0.
  Positive workloads report zero warnings/errors/instrument failures through teardown; deliberate negative tests
  still detect their expected faults. Original application list and every DirectX device setting were restored and
  independently read back, with valid before/after XML and a durable recovery journal.

Earlier failing envelopes remain evidence. REPO.3c.5.b has completed available local work and needs CI after human
publication. The next available row is REPO.3c.5.c, checked descriptor allocation; .5.d still owns full consumer closure.

## Newly completed published CI

[Run 34714148010](https://github.com/yatiyr/CRD/actions/runs/34714148010) still tests the human HEAD, not these changes.
At 00:20 local, repository checks on both OSes and five Linux lanes passed. Windows Release/Shipping and Linux ASan
were running. Four Windows non-ASan lanes retain six failures: B18-a/b/c, inner coverage, RT-4 and actual impostor.
Windows ASan job 103608115002 reports **10 failed / 6768 total**: the six plus REN-38 any-hit, full intersection/callable,
CEIR-19b RT shadow and RT-pipeline state-object failures. Its atomic A-buffer passes, as in all four completed non-ASan
Windows lanes. The old frosted-glass AV does not fail this run; that isolated pass does not close its intermittent
REPO.3c.10 gate. [Prior native ASan reproduction](2026-09-12-dx12-asan-investigation.md) remains relevant.

Strict tidy job 103608115020 newly failed at `test_dx12_work_graph.cpp:110` and
`test_work_smoke_vulkan.cpp:99,164`: two local `kN` names and a pointer-handle exception comment on the wrong line.
REPO.3c.3 was reopened and took priority. Renamed locals to `expected_count`; placed the existing narrow ABI cast
annotation on the cast, documenting caller-owned pointer transport. No new diagnostic category was disabled.
The two touched no-device branches now use actual Catch SKIP, replacing warning-plus-success returns.

`work-graph-tidy-consumers-002637` rebuilt both owners and **2/2 actual device cases passed**, zero skips/failures,
exit 0: DX12 DispatchGraph (10 assertions) and Vulkan indirect work execution (20 assertions), same expected count 5.
LLVM-20 parsed both clean in `dx12-resource-and-work-tidy-002807`. REPO.3c.3 therefore needs published CI; local work
resumes at REPO.3c.5.b. Initial local naming-edit and overbroad discovery-regex mistakes were corrected; their failed
envelopes `002440` and `002533` remain. Raw CI logs are `ci-<job-id>-7201.log` in the evidence root.


At 00:34 local the Linux ASan lane also passed. Windows Release job 103608114996 reported failure, but its job-log
endpoint returned HTTP 404 on two reads; its exact failure contents need CI output. Windows Shipping remained running.
These are observation limits, not inferred causes. The loop continues available local work and retains publication gates.
