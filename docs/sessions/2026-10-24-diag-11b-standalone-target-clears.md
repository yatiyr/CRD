# DIAG.11b (9): the standalone D3D12 targets' clear values, 2026-10-24

<!-- doc-role: historical -->
> Dated evidence. Live owner: [DIAG.11b](../ROADMAP.md#slice-diag.11b). Contract:
> [runtime diagnostics](../design/runtime-diagnostics.md#diag-11b),
> [ADR-0133](../decisions/0133-runtime-diagnostics-and-instrumentation.md). Rules: [AGENTS](../../AGENTS.md).
> Previous batch: [overlapping batch uploads](2026-10-24-diag-11b-batch-upload-overlap.md).

## CI read at the start

`gh run list --branch main --limit 15`: the one failed job is linux-gcc-public-checks (120819184913) of the scheduled
run 40075929537, still running, head `420aaf1a`: `held.fibers[0].high_water == 9U` read 8, the reading that `d96eae30`
repairs ([hosted flakes](2026-10-24-diag-11b-hosted-flaky-readings.md)). The completed runs after it were cancelled
with no failed job.

## Finding

The device validation batch left the other standalone targets' clear values as a follow-up. A new case draws into
every standalone kind at the public draws' default clear under `qualify_dx12_workload` (debug layer, every message
captured). At HEAD on this host's adapter it read 4 warnings and 1 error:

- **820 on the MSAA, G-buffer (both attachments) and visibility targets**: created with no clear value, so D3D12
  clears them slowly and warns on every clear, the default included. They are now created for it: opaque black for
  the RGBA8 targets (`standalone_clear()`, as the colour and colour-depth targets already were) and id 0 for the
  R32_UINT visibility target, the value `draw_visbuffer` and `clear_scope` clear it to (an MRT draw's authored id
  stays legal). The MSAA resolve texture is not a render target and keeps none.
- **615, a depth draw on the D24S8 target bound the D32 pipeline**: `Dx12RasterProgram::pso_for` keyed a depth PSO on
  whether a DSV exists and its compare, not its format, so a program that drew into the colour-depth target first
  reused that PSO for the stencil target. The DSV format joins the cache identity, compared exactly like the other
  members. The Vulkan raster context draws with shader objects and dynamic rendering, so it bakes no attachment
  format and has no such cache.

The VRS target was already silent (a colour target first). Clears of other values stay legal and keep their value
(the raster header's contract); a frame graph's own images carry their CEIR-derived hints and `@output` its
`output_clear()`.

## Teeth

win-debug, on this host's adapter: the MSAA target created without its clear value and the DSV compare removed, the
case fails with one 820 and the 615; restored, rebuilt and silent (0 warnings, 0 errors).

## Verification

win-debug: crd-gpu-context-dx12-tests (216 cases), crd-ceir-gpu-dx12-tests (47), crd-gpu-context-encoder-gpu-tests,
crd-scene-render-tests (115, `CRD_ASSETS_DIR` set), crd-render-graph-gpu-tests, crd-gpu-context-dx12-work-graph-tests
and the 21 `crd-diag-overhead` CTests pass; the rest of `dev.py plan`'s targets build. `dev.py check` refused the tree
(project sync's "Generation needs reconciliation"), so the suites ran directly. No Linux lane: both files are
D3D12-only. The ctest guards, `tidy-files.py` on both files, `clang-format --dry-run` (the added test lines clean; the
engine file's count below HEAD's), `check-allman-braces.py`, `check-master-plan.py`, `check-repository.py`.

Hosted once pushed: the Windows lanes pass the new case on WARP with the debug layer, and crd-gpu-context-dx12-tests
on every Windows lane.
