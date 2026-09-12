# Ordered atomic A-buffer and coverage verification

<!-- doc-role: historical -->
> Dated evidence. Live owners: [REPO.3c.6](../ROADMAP.md#slice-repo.3c.6) and [REPO.3c.7](../ROADMAP.md#slice-repo.3c.7).
> Rules: [AGENTS](../../AGENTS.md).

Continue the authorized repository loop after [DX12 consumer local qualification](2026-09-13-dx12-consumer-diagnostics.md).
The preceding children retain Needs CI, not a local implementation blocker. Begin with REPO.3c.6: reconcile the retained
[five-emitter repair](2026-09-12-atomic-abuffer-emitter-repair.md) with published provider results, qualify the actual
atomic workload through the new native capture and preserve its bit-exact/non-background oracle. One primary Windows
Debug configuration, two workers, focused affected consumers and incremental LLVM-20; no full local sweep.

Human HEAD is 7201a7b818ee6b363aac6cfbf97f94fa96d8fb85. Its completed Windows test logs report the atomic A-buffer case
passing, including Shipping (`ci-103608115021-7201.log`, 4,585/6,681). The missing Release attempt was a lost hosted
runner, and its isolated retry 103635907447 is running. That retry does not publish local fixes. No advisor capability
is available; no review is claimed. Preserve earlier work, renderer design gates and human-only commit/push ownership.

## Atomic local qualification

The five-emitter implementation is already in the human revision. Atomic output passes in all six available published
Windows logs (Debug, SSE2, Shipping, clang-cl Debug/Shipping and ASan); Release is still building in the isolated retry.
The new local test wraps the actual compute context through teardown in the shared native validation capture, requires
successful pipeline creation before dispatch, checks all 3,072 RGB values are finite, and retains exact equality and
non-background checks. This prevents a NaN from escaping the maximum-error comparison. No kernel/oracle was weakened.

Evidence under `build/research-dev-workflow-20260912/`, Windows Debug, MSVC 19.51, two workers:

- `atomic-native-close-022111`: rebuilt the actual DX12 executable; exact/static and atomic cases **2/2**, no skips.
- `atomic-native-tidy-022636`: LLVM-20 parsed the changed compute test; **1/1 clean**.
- `warp-022726-e2b23e`: independent Software census, then atomic **1/1**, 3,083 assertions, zero native warnings/errors,
  no dropped/truncated messages or instrumentation/execution failures. Original D3DConfig apps and all settings restored
  and verified. WARP 10.0.26100.8972; NVIDIA proof uses 4070 Ti SUPER / 32.0.15.9579.

REPO.3c.6 has completed available local work and needs CI output for the unpublished capture/assertion strengthening.
The earlier five-emitter proof remains in its linked session; this does not rerun unchanged compute emitters locally.

## Coverage investigation contract

REPO.3c.7 starts next. Verify the encoder's conservative-state route, queried Tier 3, emitted builtin and actual pixels
before blaming the provider. The old software-only `black > 0` exception is not evidence that Tier-3 edge semantics
are optional. Retain real fully-covered and partial-pixel assertions, use native validation, and report unavailable
capabilities as actual skips. Add a deterministic interior/edge case away from raster snapping uncertainty if needed.
Reference: [Microsoft conservative-raster specification](https://microsoft.github.io/DirectX-Specs/d3d/ConservativeRasterization.html).

## Coverage reproduction and native reduction

The strict software-independent oracle reproduces a real gap. The first `coverage-baseline-023004` inventory guard
rejected a Vulkan executable accidentally matched by the regex before CTest; the corrected DX12-only selection
`coverage-baseline-023048` rebuilt and passed 2/2 on hardware. Conservative raster increased coverage 242→312 pixels;
inner coverage identified 220 interior / 92 edge pixels. `warp-023135-1cfba7` passed overestimate and failed inner
coverage: 312 interior / 0 edge. The original software exception had concealed that part of the feature contract.

The test now starts native capture before contexts, uses genuine SKIP for unavailable device/compiler or Tier 3,
checks program creation, masks bit 0, and compares exact interior/edge/background pixels. Two sections compare CKIR
with a direct native HLSL fragment; both use the public production encoder. Black clear / red edge avoids an unrelated
optimized-clear warning while preserving the geometric oracle. `coverage-baseline-023416` rebuilt the actual owner and
passed 2/2 hardware cases, including both sections, with zero native warnings/errors. `warp-023552-d10146` failed both
sections at the edge-count and pixel (32,20) assertions; capture was complete/silent (42 info messages, 2/2 contexts in
each section). No mask, compiler route or nonzero-count weakness explains the failure.

`coverage-new-warp-023751` / `warp-023751-7d3055` tested Microsoft's signed WARP 1.0.20 package beside the executable.
The census reported driver 1.0.20.0, Tier 3, wave maximum 128 and RT tier 1.2. Both shader sections reproduced the same
failure, with zero native warnings/errors. The exact temporary DLL was removed; every D3DConfig experiment restored
and verified the original app list and all settings. No global SDK/runtime installation or shipping dependency changed.

The official NuGet package SHA-256 is `e5fe5de661ce98b58ef9cfb736e73c0a7a2623d3bbf5f14839b2d55566d87e40`;
its x64 DLL SHA-256 is `2a08692cba4c130593329255627fb915d666c90cda53d284594bc8438fd4f49d`. Authenticode was Valid,
signer Microsoft Corporation. Acquisition URL, full SHA-512 and entry inventory are in `warp-package-1.0.20/acquisition.json`.

Finally `coverage-sdk-probe/run-024152` built a standalone /W4 /WX SDK-only executable, with no Cerid libraries. It
explicitly creates the adapter, queries Tier 3, uses a conservative graphics PSO and R32_UINT target, reads raw values
after a checked finite fence, and registers unfiltered native diagnostics. All six arms ran:

- NVIDIA hardware, DXIL and DXBC: **2/2 pass**; raw interior=1, edge=0, background=2.
- OS WARP 10.0.26100.8972, DXIL and DXBC: **2/2 fail**; raw interior=1, edge=1, background=2.
- App-local WARP 1.0.20, DXIL and DXBC: **2/2 fail**, same incorrect edge value. Loaded module paths prove which DLL ran.

All arms reported zero native errors/warnings. These observations isolate a native SDK/provider mismatch outside
Cerid; they do not identify its internal vendor-code cause. No external issue/message was submitted. A provider/runtime
fix or an explicitly designed and qualified capability/fallback contract remains necessary. REPO.3c.7 must not be Done
or Needs CI: its remaining native failure is reproduced locally. The [recipe](../recipes/2026-09-13-dx12-inner-coverage.md)
preserves the geometry, native assembly, shaders, primary references and public test route.

## User stop and reviewer handoff

During this task the user instructed: "finish the loop after finishing your current task. I will use you as a reviewer
from now on". This supersedes the unattended continuation grant for this Codex task. Finish the coverage investigation
and its checks/documentation, stop the repository heartbeat, and do not advance to REPO.3c.8. The roadmap retains the
actual unfinished gate rather than hiding it. This task is review-only afterward; future implementation needs a new
user assignment. The renderer design gates and human-only publication rule remain unchanged.

Final CI inspection: Release retry job 103635907447 in
[run 34714148010](https://github.com/yatiyr/CRD/actions/runs/34714148010) still has Configure passed, Build in progress,
Test pending. It runs human revision 7201a7b, not the unpublished working tree. No new tests or hosted passes are inferred
from that status. Earlier local repairs remain Needs CI. No commit, push, stage, benchmark claim or advisor review occurred.

Final LLVM-20 evidence `coverage-final-tidy-024545`: changed raster test parsed, **1/1 clean**. The scheduler update
returned PAUSED for `cerid-repository-hardening-loop`, and its saved TOML also reports PAUSED. Synchronizer status has
an intact baseline, no incomplete transaction and no generation in progress; its normal source/IDE watcher remains
running. That developer synchronizer is separate from the stopped agent implementation loop.

Close-out checks passed: master-plan/document validator **863 rows, 1,025 documents, 8,802 local links**, all 210 D-007
and 16 v17 routes retained; repository guard **96 modules**; `git diff --check` exit 0. The first documentation check
caught an AGENTS byte-budget overrun from a duplicated stop rule; compacting the existing historical-grant sentence
preserved the rule and restored the budget. No implementation row is left In progress after this user-requested stop.
