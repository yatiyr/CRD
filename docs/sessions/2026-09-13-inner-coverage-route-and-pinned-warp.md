# Inner-coverage route contract and the pinned WARP provider

<!-- doc-role: historical -->
> Dated evidence. Live owners: [REPO.3c.7](../ROADMAP.md#slice-repo.3c.7) and [REPO.3c.10](../ROADMAP.md#slice-repo.3c.10).
> Rules: [AGENTS](../../AGENTS.md). Preceding batch: [guard/tidy repairs](2026-09-13-ci-guard-tidy-repairs.md).

Both rows were the last hosted failures outside strict analysis at revision `0b858a6`
([run 34726528230](https://github.com/yatiyr/CRD/actions/runs/34726528230)): B1-f inner coverage on every Windows lane
and four DXR gates on the ASan lane. Both had already been isolated to the hosted software provider by SDK-only
reductions; this session turns those isolations into qualified engine contracts. Same user direction as the preceding
batch; no commit or push; NVIDIA 4070 Ti SUPER 10de:2705 / 32.0.15.9579 and WARP 1414:008c on this workstation.

## Census extension

`crd-dx12-device-info` now prints OPTIONS3 barycentrics and the highest shader model, probing from 6.6 downwards
because the runtime rejects an unknown model. Hardware: `barycentrics=1 highest=0x66`. WARP 10.0.26100.8972 through the
reversible D3DConfig wrapper (`warp-134815-684fe7`): `conservative_tier=3 barycentrics=1 highest=0x66`, restoration
verified. The wrapper's XML export step fails on this host today with `C00CEE2C` before any change; the variant
`run-warp-diagnostics-noexport.py` records that failure instead of aborting and keeps the textual before/after
snapshots as the restoration proof. Every run below reports `restoration: verified`.

## REPO.3c.7 — the route contract

`IRasterContext::inner_coverage_route()` reports `Native`, `Barycentric` or `Unsupported`; `supports_inner_coverage()`
is true for either route. DX12 decides per device in `dx12_inner_coverage_route`: conservative Tier 3 on a provider that
is not the documented software adapter keeps the native `SV_InnerCoverage`; the software adapter, and any provider with
Tier 1 or 2, takes the barycentric route when OPTIONS3 barycentrics and shader model 6.1 are available; otherwise the
program is refused. The HLSL emitter lowers `KBuiltin::InnerCoverage` on that route to `noperspective float3 :
SV_Barycentrics` and one helper: screen-space-linear barycentrics are affine, so `ddx`/`ddy` are exact and the minimum
over the four pixel corners is `b - 0.5*(|ddx b| + |ddy b|)`; the pixel is fully inside iff every component stays
non-negative. The program compiles as `ps_6_1` on that route only. A process-wide test override forces a route the
default device can run. Vulkan keeps its native `FullyCoveredEXT`; the default accessor derives the route from the bool.

The public test is now three cases with one CPU oracle. The oracle classifies every 64×64 pixel by separating axes
against the shared triangle: fully covered, touched but partial, or untouched, with corners within 1/64 px of an
edge or extreme vertex excluded as ambiguous. White, red and black must match exactly; any other colour fails.

| Case | Hardware | WARP 10.0.26100.8972 (`warp-140336-4089bf`) |
|---|---|---|
| public route | native, 17 assertions pass | barycentric, 17 assertions pass |
| forced barycentric route | 16 assertions pass | 16 assertions pass |
| native SV_InnerCoverage conformance | pass | genuine SKIP: native route unqualified on this provider |

Both providers and both routes render interior 220, edge 92, background 3,784, ambiguous 0, mismatches 0. The
previous provider result (312 interior, 0 edge) is therefore replaced by an exact match on the software provider
without touching the oracle, without a software exception and without reporting a different tier. The
[recipe](../recipes/2026-09-13-dx12-inner-coverage.md) records the contract and the remaining native-provider trap.

## REPO.3c.10 — the over-read lives in the OS WARP build

Rebuilt `crd-gpu-context-dx12-tests`, `crd-scene-render-tests` and the census under `win-asan`, then ran the four gates
through the wrapper twice with the same executables:

| Arm | Provider (census) | REN-38, REN-38-F13, CEIR-19b, REN-38-F6 (DX12) |
|---|---|---|
| A `warp-134948-07eef4` | OS WARP `driver=10.0.26100.8972` | all four fail: 8-byte memmove read past a D3D12Core allocation inside d3d10warp |
| B `warp-135107-ac7699` | app-local signed package `driver=1.0.20.0` | all four pass with 32, 36, 31 and 11 assertions |

Arm B placed the retained `warp-package-1.0.20/d3d10warp.dll` (SHA-256 `2a08692c…`) beside the two ASan executables and
removed it afterwards; the census driver version proves which module executed. The frosted-glass access violation
named in the row already passes at `0b858a6` on the ASan lane (CEIR-31b-4-b-i GATE (DX12)).

The candidate engine-side resolution was a verified provider pin, not a suppression: `scripts/install-warp.py` downloads
the official NuGet package with the pinned SHA-256, extracts exactly `build/native/bin/x64/d3d10warp.dll` and verifies
the DLL hash; `cmake/CrdWarp.cmake` copies it beside every ALL test executable when `CRD_WARP_DLL` is set
(`crd-warp-stage`, content-checked copies, fatal on a missing file, `EXCLUDE_FROM_ALL` targets skipped). The four
Windows compiler jobs were wired to install it and configure with the variable; the whole-suite qualification below
withdrew that wiring the same day, so hosted lanes keep the OS WARP and the harness stays opt-in.
`test-repository-tools.py` gained three cases (client identification with byte checks, package and member checksum
enforcement, staging beside every ALL executable and none beside two excluded ones in a synthetic project) —
**16 tests pass**. The installer reproduced the recorded hashes and the 8,500,576-byte DLL from the retained package.
The [pinned-WARP recipe](../recipes/2026-09-13-dx12-pinned-warp.md) records the acquisition, the result and its limits.

## Whole-suite qualification and withdrawal

Staging 1.0.20 beside every test executable changes the provider for every DX12 test on every Windows lane, so the
three DX12 executables ran whole (CTest working directory and `CRD_ASSETS_DIR` parity) through the reversible D3DConfig
wrapper variant `run-warp-diagnostics-pinned.py`, once with the package staged and once without, same `win-debug`
binaries, restoration verified both times:

| Executable | 1.0.20 app-local (`warp-145345-ab068e`, census `driver=1.0.20.0`) | OS WARP 10.0.26100.8972 (`warp-145917-2e693c`) |
|---|---|---|
| `crd-gpu-context-dx12-tests` | 164 passed, **12 failed**, 1 genuine skip of 177 | 176 passed, 0 failed, 1 genuine skip of 177 |
| `crd-scene-render-tests` | 113 of 113 passed | 113 of 113 passed |
| `crd-ceir-gpu-dx12-tests` | 45 passed, **1 failed** of 46 | 46 of 46 passed |

The census differs beyond the driver field: the package reports `wave_min=4 wave_max=128` and `rt_tier=12` where the
OS build reports `wave_max=4` and `rt_tier=11`. The thirteen failures are all "== CPU oracle bit-exact" gates that
hardware and the OS build pass: eleven `precise` floating-point kernels whose CKIR graphs contain no subgroup op
(NRC fused-MLP forward and backward, Stockham FFT, 2-D FFT pipeline, batched strided inverse FFT, fused FFT
convolution, R2C fused convolution, the CEIR 6-dispatch 2-D FFT asset, both B17-c A-buffer composites at
|Δ| = 5.96e-08, the B4-vis software rasterizer at 11 pixels) and two wave-shaped kernels (B11 wave/subgroup ops, the
radix sort). The mechanism is not established here: no probe of contraction, rounding or the executed wave width was
run, and the decision does not need one. A first direct-execution run (`warp-144250-bac603`) is superseded: it ran
the executables from the repository root without `CRD_ASSETS_DIR`, which skipped or failed scene-render cases for
environmental reasons, and a tracked file changed during it.

Decision: the pin trades four red ASan gates for thirteen red tests on all seven Windows lanes, so the CI wiring is
removed; `CRD_WARP_DLL` stays empty on hosted lanes and the installer, staging module and tooling tests remain as
the opt-in harness that reproduces this experiment. REPO.3c.10 returns to Partial with the over-read isolated
(8-byte memmove inside the OS `d3d10warp.dll`, 0 bytes past D3D12Core-owned regions of 2,924 and 2,568 bytes) and
the four hosted ASan DXR gates open; the order rule returns REPO.3c and the audited REPO.DEV rows to Partial behind
it. Separately observed, not acted on: `Dx12ComputeContext::subgroup_size()` returns `WaveLaneCountMin`, which is only
the executed width when min equals max. During this work `build/win-debug` was regenerated through the `win-debug`
preset after a preset-less configure attempt had left `CMAKE_MAKE_PROGRAM` unset; no tracked file changed by that.

## Local qualification

- `crd-gpu-context-dx12-tests` rebuilt after the interface change; the three B1-f cases pass on hardware and behave
  as tabulated on WARP; `crd-scene-render-tests` and `crd-gpu-context-vulkan-tests` rebuild as consumers of the
  changed raster interface. The whole DX12 executable ran on hardware afterwards (result appended below).
- Hygiene guard PASS (96 modules); tooling tests 16/16; the workflow parses (the WARP steps were added, then removed again).
- Incremental LLVM-20 tidy of the changed headers/TUs is recorded in the close-out below with the 258-TU sweep.

## Close-out

- Whole `crd-gpu-context-dx12-tests` executable on hardware after every change: **177 test cases, 10,189 assertions,
  exit 0**; the only diagnostic lines are the intentional descriptor-failure probe and Catch2 WARN summaries.
- Incremental LLVM 20.1.8 tidy on the twelve changed headers/TUs: clean after replacing one nested conditional in
  the oracle comparison; the three B1-f cases re-ran 3/3 on hardware after that repair.
- Validator PASS (863 rows, 1,029 documents, 8,863 local links); hygiene 96 modules; tooling 16/16; `git diff --check` clean.
- After the withdrawal: the workflow parses with only the clang-tidy comment left changed; tooling 16/16 including the
  `EXCLUDE_FROM_ALL` staging cases; validator PASS (863 rows, 1,029 documents, 8,867 local links); `git diff --check`
  clean; D3DConfig restored (`<no apps>`, `force-warp=false`); no `d3d10warp.dll` left under `build/win-debug`.
- State: REPO.3c.7 held Needs CI with this evidence until [run 34757652779](https://github.com/yatiyr/CRD/actions/runs/34757652779)
  at `ae44264` passed B1-f on every Windows lane through the barycentric route on the OS WARP (census
  `driver=10.0.26100.33296`); it is Done. The ASan lane failed exactly the four DXR gates, which the
  [third-party register](2026-09-13-third-party-defect-register.md) now owns under REPO.3c.10. No commit or push by the agent.
