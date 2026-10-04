# DIAG.7a (g-6): the DX12 program route under Core validation

<!-- doc-role: historical -->
> Dated evidence. Live owner: [DIAG.7a](../ROADMAP.md#slice-diag.7a); contract:
> [DIAG.7a](../design/runtime-diagnostics.md#diag-7a). Rules: [AGENTS](../../AGENTS.md).
> Preceding: [7a census and (g-1)..(g-5)](2026-09-16-diag-7a-gpu-validation-identity-census.md); same day:
> [nightly CI repairs](2026-10-04-nightly-ci-repairs.md).

## Purpose

The (g) route ledger left one claimed route without a correlated proof: DX12 Program under Core, bucketed "feasible but
deferred". This work does not depend on the user's open DIAG.7a closure question, so it ran while CI qualified the
nightly repairs.

## Measurement

A temporary experiment in `test_dx12_validation.cpp` (removed afterwards) Cerid-named a compute PSO as a Program
through `dx12_attach_identity` and provoked every Core misuse that involves a pipeline state, on the workstation debug
layer:

| Misuse | Message | Severity | Names the PSO |
|---|---|---|---|
| Dispatch with a root signature other than the PSO's | 953 "The currently set Root Signature doesn't match the currently set Pipeline State Object" | Error | no |
| Dispatch with no root signature | 952 "No Root Signature has been set" | Error | no |
| SetPipelineState on a COPY list | 933 "Invalid API called ... not valid for D3D12_COMMAND_LIST_TYPE_COPY" | Error | no |
| SetPipelineState of a graphics PSO on a COMPUTE list | none | none | no |
| Draw without the graphics PSO's declared render target | none | none | no |
| Draw with a compute PSO bound | 951 "The current Pipeline State (0x...:'[crd:prog:...] name') is a compute Pipeline State" | Warning | **yes** |

The capture counters confirmed the two silent cases were silent, not dropped (`dropped == 0`, error count unchanged).

## Result

The DX12 program route correlates at **warning** severity: the one Core record that names a pipeline state carries the
parsed Program identity. No Core **error** names a pipeline state, so an error-severity program correlation is a
mechanism limit of the Core layer, not a missing Cerid step. GPU-based validation and DRED, the DIAG.7b territory, are
where an error-severity program report could come from.

`DIAG.7a(g-6)` (`[dx12][validation][identity][hazard]`) lands the measured route with the g-5 shape:
(a) a named compute PSO under the Draw hazard yields a correlated warning carrying its Program id, alive in the
registry; (b) the same hazard on an unnamed PSO still warns but parses no identity; (c) a valid dispatch of a named PSO
with its own root signature produces no record naming it at any severity. The strict gate also required splitting two
multi-declarations in the existing DXR identity test.

## Verification

- `crd-gpu-context-dx12-tests` built alone on `win-debug` (two jobs); `[hazard]` 2 test cases, 38 assertions, passed
  three consecutive runs; `[identity]` 15 test cases, 185 assertions, passed.
- Strict LLVM 20.1.8 gate on `test_dx12_validation.cpp`: clean.
- Hosted lanes qualify the rest with the next push.

## For the DIAG.7a decision

The ledger becomes: PROVEN resource (Vulkan g-1/g-2, DX12 g-5), program (Vulkan g-3), pass (Vulkan g-4); MEASURED
program on DX12 at warning severity (g-6, error severity unavailable from Core); MECHANISM-BLOCKED pass on DX12 (no
PIX); OPEN lifetime class. Whether a warning-severity correlation satisfies "a correlated error on each claimed route"
for DX12 programs, or the claim moves to DIAG.7b, is part of the user's open DIAG.7a closure decision.
