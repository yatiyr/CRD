# DIAG.11b (7): the authored rendering workload, 2026-10-24

<!-- doc-role: historical -->
> Dated evidence. Live owner: [DIAG.11b](../ROADMAP.md#slice-diag.11b). Contract:
> [runtime diagnostics](../design/runtime-diagnostics.md#diag-11b),
> [ADR-0133 ID-5](../decisions/0133-runtime-diagnostics-and-instrumentation.md#implementation-decisions). Rules:
> [AGENTS](../../AGENTS.md). Previous batch: [file readers' zero pass](2026-10-24-diag-11b-file-reads-zero-pass.md).

## CI read at the start

`gh run list --branch main --limit 15`: no completed run has a failure without a fix on main. The completed runs
since 40032224637 (green) were cancelled with no job. 40052612807 (head `90e9a299`, still running) has 18 failed jobs:
each lane fails only `crd-diag-coverage` and the Windows repository job only `test_committed_registry_describes_the_tree`
(its `HOTSPOT_NOT_VISIBLE` and `PERMISSION_DENIED` lines are the passing sampling tests' negative controls), which
`9583f26b` repairs. 40067359705 (head `9ce438da`) was pending, so `9583f26b` and later have no hosted run yet.

## The workload

The row's "sandbox headless" is the sandbox's own frame path without its window: the scene renderer, the authored
frame graph and a headless device, driven by the harness so `render` shares its modes, record, CTests and judge. No
new file or script: `render` is one more class in `tests/support/diag-overhead/workloads.cpp`.

- The scene: 3072 quads in a grid in front of the camera and 1024 behind it, drawn through
  `engine://frame/forward_csm` (from `assets/`, shadow cascades on) into a 512 x 512 colour and depth target; D3D12 on
  Windows, Vulkan elsewhere. Every frame moves the first row (64 instances), syncs and renders. The renderer, the
  world and the resource manager allocate from one heap, which basic and full hand over through the host's
  allocator watch. One site per frame.
- The answer: setup's first frame must draw exactly the grid (3072 instances), cull exactly the 1024 behind the
  camera, run its passes on the device (6 timed) and cover at least 50,000 pixels unlike the background (it covers
  124,032); its counts are every frame's signature, and a frame whose sync re-extracted nothing or whose device ran
  no pass fails. Anything short refuses the run (exit 3) with the counts it saw.
- The mesh is `tests/gpu/gpu-shared/quad_scene_fixture.hpp`'s quad and pack writer, now framework-free
  (`write_one_pack` returns false instead of a Catch2 `REQUIRE`; the frosted-glass caller requires it).
- `Workload::warm` fills a workload's process-lifetime caches before the ledger's window opens: the Vulkan backend
  loads shaderc once and keeps it for the process, so without the warm compile every Linux `render` CTest read
  `library-handle` outstanding. The record now names the unbalanced classes (`lifetime.unbalanced`).
- Ownership: the harness moved from the `ceir-host` module to `scene-render`, whose test dependencies already hold
  `ceir-host`, `ceir-cook`, `jobs`, `perf` and both device backends (`crd_verify_modules` refused the renderer under
  `ceir-host`). The three `render` CTests take `RESOURCE_LOCK crd_gpu_device`.

## Teeth

Each on win-debug, rebuilt and rerun, then restored, rebuilt and green:

- `sites_per_iteration` 2: basic fails "not every site and frame was filtered", full "not every payload was evaluated
  once" (off counts no site, so it cannot see it).
- The movers left in place: all three modes fail "a frame computed the wrong answer".
- `covered()` counting nothing: every mode refuses the run, "0 of 50000 pixels covered".
- The warm left out (Linux, linux-gcc-debug): the three `render` CTests fail on the ledger, `library-handle`.

## Boards

`diag-overhead.py run --workloads render --iterations 200 --warmup 20 --affinity 5555`, host idle
(`build-bench.py host`). Frame time and CPU per frame, worse verdict decides:

| Tuple (SHA-256 prefix), processes | Disabled (1%) | Basic (2%) | Full (reported) | Basic storage |
| --- | --- | --- | --- | --- |
| win-shipping-profile `58a639e9aec5` vs win-shipping `c8d8900352c8`, D3D12 hardware adapter, 192 | -0.27% (-1.00 .. +0.09), -0.13% (-1.11 .. +0.38): within | +0.55% (-0.02 .. +1.22), +1.11% (+0.24 .. +1.69): within | +1.08%, +1.53% | 152 KiB ring, +0.33 MiB |
| linux-gcc-shipping with profiling `97295e32fec3` vs linux-gcc-shipping `9113cd013eef`, llvmpipe, 96 | -0.11% (-0.54 .. +0.41), -0.00% (-0.22 .. +0.23): within | -0.18% (-0.49 .. +0.13), -0.01% (-0.36 .. +0.22): within | +0.07%, +0.05% | 152 KiB ring, -0.26 MiB |
| linux-gcc-relwithdebinfo `15ea22ca19ad`, llvmpipe, 96 | n/a | +0.15% (-0.57 .. +0.55), -0.16% (-0.45 .. +0.28): within | +0.03%, +0.32% | 152 KiB ring, -0.13 MiB |

Frame p50 / p99: Windows 353 / 577 µs compiled-out, 352 / 585 off, 353 / 600 basic, 354 / 595 full (CPU per frame
about 670,000 cycles: the frame waits on the device's fence); Linux 2,865 / 3,332 µs, CPU 10.8 ms a frame, since
llvmpipe rasterises on the process's own threads. Every arm delivered its captures (576 on Windows, 288 on Linux).

A first Windows board at 48 processes read disabled and basic within on the frame and unresolved on the CPU
(intervals of about ±1.4% and ±2.2%: the driver's threads add their own spread); 192 resolves both. The Linux
tuples measure the modes against a software-rasterised frame, which dilutes a fixed cost eightfold; the hardware
tuple is the strict one. No instruction board counts `render` under cachegrind: llvmpipe's compiled rasteriser is
the frame, so the Linux disabled verdict rests on the frame and CPU above.

## Verification

win-debug: `crd-diag-overhead` and `crd-scene-render-tests` built; the 18 `crd-diag-overhead-*` CTests, the two
script CTests and the 18 frosted-glass device gates (`CEIR-31b-4-b`, Vulkan and D3D12) pass; the ctest guards.
linux-gcc-debug through `wsl-lane.sh`: both targets build with no `warning:` line; the 18 harness CTests and the
frosted-glass Vulkan gates pass on llvmpipe. win-shipping, win-shipping-profile, linux-gcc-shipping, its profiling
configuration and linux-gcc-relwithdebinfo build the harness with no `warning:` line. `tidy-files.py` clean on the
changed sources (Windows database; the WSL host has no clang-tidy 20, item (o)), clang-format on the added lines (the
remaining findings are the files' hand-aligned groups), `check-allman-braces.py`, `check-master-plan.py`,
`check-repository.py`.

## Not done in this batch

- (l) zstd 1.5.7 and (o) clang-tidy 20 on the WSL host are downloads; this run's only user message approves none.
- (8) the user's elevated recording with Visual Studio closed; (9) dev.py/CI ownership and conclusion artifacts, the
  hosted sanitizer/race/corpus/negative-control audit with each separate mode's cost, and the fuzz/soak cadence.
