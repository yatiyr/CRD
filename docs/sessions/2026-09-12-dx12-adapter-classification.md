# DX12 selected-adapter classification

<!-- doc-role: historical -->
> Dated implementation evidence. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

## Purpose and starting state

Continue the approved repository programme at REPO.3c.4. HEAD remained the human-published
`2ed89c487215c91ffc2f667c19b2d97f01f5598c`; preserve the preceding uncommitted workflow fixes. The previous turn made
verified progress, not a blocked wait. No advisor capability is available, and no review is claimed. No commit,
push, geometry/physics algorithm work or renderer implementation is authorized by this increment.

The selected-device CI census reports BasicRender 1414:008c, flags 0, while the current helper tests only the DXGI
software flag. Microsoft's primary sources document this exception. The [recipe](../recipes/2026-09-12-dx12-adapter-classification.md)
records the sources, query/unknown policy and complete assembly. Existing precision bars and workload assertions
remain unchanged. Correct classification alone cannot close the seven reported workload failures.

## Discriminating native experiment

A small ignored CMake/Ninja/MSVC probe enumerated actual DXGI adapters and queried their selected LUID through
`D3DKMTOpenAdapterFromLuid` and `KMTQAITYPE_ADAPTERTYPE`. Open/query/close all returned success:

- NVIDIA RTX 4070 Ti SUPER, 10de:2705, DXGI flags 0, LUID 00000000:0001307b: kernel bits 0000031b,
  SoftwareDevice 0, RenderSupported 1; this was the default device.
- Microsoft Basic Render Driver, 1414:008c, DXGI flags 2, LUID 00000000:00014479: kernel bits 00000105,
  SoftwareDevice 1, RenderSupported 1. Explicit EnumWarpAdapter returned this same LUID on this workstation.

This demonstrates the native query mechanism on both available local providers. It does not reproduce the hosted
flag-0 tuple. The probe compiled warning-free with /W4 /WX and two workers; no engine-wide build was run. Ignored
source, commands and output are under `build/research-dev-workflow-20260912/adapter-query-probe/` and
`adapter-query-observation.log`.

## Implementation increment

The backend now has a three-state `Dx12AdapterKind` query. It combines exact-LUID kernel evidence and DXGI metadata,
retains Unknown on unestablished/conflicting identity, and preserves the existing boolean helper for its callers.
The backend owns kernel handles through RAII and links Gdi32 privately. A private policy has counterfactual cases
for the observed missing flag, unavailable queries and contradictions. The CI probe independently logs kernel
statuses/bits and rejects an unknown engine classification.

Compilation, strict analysis and runtime qualification of this increment remain to be recorded below; source edits
alone do not establish those gates. The watcher was observed starting native regeneration after the source/CMake
edits; subsequent build work must respect its generation handle and finalized baseline.

## First build and oracle observation

The five changed C++ files parsed and passed LLVM-20 strict analysis. Windows Debug's compile database was refreshed
through guarded configure so the new test used its actual private include path. The native census and main DX12 test
targets then built with two workers. All **eight** selected CTests passed on the NVIDIA device: classification policy,
hair BCSDF, fur BCSDF, multiple scattering, Huang microfacet lobe, inner coverage, RT-4 and the native census. Hardware
classification remained Hardware; inner coverage observed white 220/black 92 and hair maximum absolute error 1.788e-6.
No oracle or tolerance changed. These are correctness observations, not a performance benchmark.

The temporary wrapper incorrectly expected seven tests: its B18 pattern also selected the existing Huang case.
It therefore retained an incomplete result despite CTest's eight passes. Evidence inspection then exposed a separate
ordering defect: Path sorting placed directory `ctest/` before file `ctest.xml`, while inspection sorted portable
string names in the opposite order. The actual artifact sets/hashes were equal. The evidence module now sorts the
portable names consistently; a regression covers that prefix plus mixed-case names. Windows passed 46 tooling cases;
Linux passed 44 applicable cases with two Windows-only cases explicitly not applicable. The original result remains
incomplete, not rewritten. The corrected wrapper discovers the exact names and reconciles JUnit through the common
frontend helper; subsequent qualified evidence belongs in a fresh envelope.

## Newly completed published ASan lane

[Job 103572020417](https://github.com/yatiyr/CRD/actions/runs/34700773738/job/103572020417) completed with 12 failures
among 6,763 CTests on the same published revision. Seven repeat the existing census. Five additional failures need
their own memory/lifetime investigation, now REPO.3c.10:

- REN-38 authored any-hit RT, REN-38-F13 intersection/callable, CEIR-19b RT shadows and REN-38-F6 RT pipeline report
  heap-buffer-overflow reads through d3d10warp during state-object/pipeline creation. The first trace reaches
  `Dx12RasterContext::dxr_pipeline` at dx12_raster_context.cpp:2735.
- CEIR-31b-4-b-i frosted-glass mask reports an access violation through d3d10warp during bindless drawing, reaching
  dx12_raster_context.cpp:6491.

Those locations are evidence of the failing call path, not proof that the driver caused it. Inspect actual buffers,
descriptor ranges, ownership and lifetime before attributing the mechanism. No ASan suppression or skip is proposed.
Windows Shipping/Release remained active at this observation; exact published-revision qualification remains open.

## Qualified hardware run and additional identity coverage

Fresh envelope `dx12-classification-203708` passed eight selected/reported/executed tests, with zero skips or failures,
matching source and model identities, and successful evidence integrity inspection. The earlier incomplete envelope
is preserved. This qualifies the default NVIDIA hardware path and its unchanged oracles, not the hosted flag-0 tuple.

The native LUID query was then extracted as a backend-private function, retaining the same public default-device
wrapper. Two additional tests use the production mechanism on the explicitly enumerated WARP adapter and on a LUID
first verified unavailable by DXGI. This covers real software/unknown paths without introducing a forced default
adapter mode. The three affected files passed incremental LLVM-20 analysis again; the expanded ten-test run remains
to be recorded after it completes.

## Subsequent qualification and completed CI census

`dx12-classification-204218` completed the expanded ten-test native Debug run: 10 selected/reported/executed/pass,
zero skips/failures, unchanged source/model and verified envelope integrity. The
[following session](2026-09-12-atomic-abuffer-emitter-repair.md) records controlled WARP oracles, the distinct atomic
pipeline-creation failure, its cross-emitter repair and final 22/11/37-test selections. Existing software-specific
precision bars remain unchanged. The hosted flag-0 provider still needs the human-published repair.

[Published CI run 34700773738](https://github.com/yatiyr/CRD/actions/runs/34700773738) subsequently completed, failed,
at `2ed89c487215c91ffc2f667c19b2d97f01f5598c`. Both repository jobs and all six Linux compiler lanes passed. The six
non-ASan Windows runtime lanes (Debug, SSE2, clang Debug, clang Shipping, Release, Shipping) retained the same seven
DX12 failures; ASan retained the twelve cases above, and strict tidy retained the three frame-cooker diagnostics
already repaired locally. Final Release/Shipping raw logs are `ci-2ed89-release.log` and `ci-2ed89-shipping.log` in the
same ignored diagnostic directory. This completion replaces the earlier observation that those jobs were running;
it does not qualify the unpublished working tree.
