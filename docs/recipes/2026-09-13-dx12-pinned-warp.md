# Pinning the WARP software provider for DX12 test lanes

<!-- doc-role: reference -->
> Technical recipe. Live owner: [REPO.3c.10](../ROADMAP.md#slice-repo.3c.10). Rules: [AGENTS](../../AGENTS.md).

## Parameters

| Input | Meaning | Pinned value |
|---|---|---|
| Package | Microsoft.Direct3D.WARP on nuget.org, flat container URL | version 1.0.20, `microsoft.direct3d.warp.1.0.20.nupkg` |
| Package SHA-256 | Identity of the downloaded archive; the only trusted acquisition proof | `e5fe5de661ce98b58ef9cfb736e73c0a7a2623d3bbf5f14839b2d55566d87e40` |
| Member | The single file extracted; archive paths never choose output locations | `build/native/bin/x64/d3d10warp.dll` |
| DLL SHA-256 / size | Verified after extraction, output deleted on mismatch | `2a08692cba4c130593329255627fb915d666c90cda53d284594bc8438fd4f49d`, 8,500,576 bytes |
| Signer | Authenticode recorded at first acquisition (`warp-package-1.0.20/acquisition.json`) | Microsoft Corporation, Valid |
| Consumer | CMake cache variable read by `cmake/CrdWarp.cmake` | `CRD_WARP_DLL=<destination>/d3d10warp.dll`, empty by default |
| Proof of use | `crd-dx12-device-info` driver field of the selected adapter | `driver=1.0.20.0` (OS build reports `10.0.26100.*`) |

## Contract

The D3D12 runtime loads `d3d10warp.dll` from the application directory before System32. Staging the verified DLL
beside a test executable therefore selects the pinned build for that process only: nothing is installed system-wide,
no registry or D3DConfig state changes, and processes without the copy keep the OS build. Hosted lanes do not set
the variable: the 1.0.20 package was qualified on the whole DX12 suites on 2026-09-13 and withdrawn from CI (see
Findings). The harness stays as the reproducible provider experiment; `CRD_WARP_DLL` is empty by default and the
reversible D3DConfig diagnostic qualifies the OS build on a workstation.

## Assembly

1. `python scripts/install-warp.py --destination <dir>` downloads (or reuses) the package, verifies both hashes and
   prints the `CRD_WARP_DLL` line. `--archive` accepts an already downloaded official package; the checksum is still
   required. Unit tests: client identification with byte checks, wrong package hash, wrong member hash.
2. `include(CrdWarp)` in `tests/CMakeLists.txt`; `crd_stage_warp_dll(<tests root>)` after every subdirectory. With the
   variable set it collects every ALL executable below the root (targets or directories marked `EXCLUDE_FROM_ALL`
   are skipped, never forced into hosted builds), creates the ALL target `crd-warp-stage` depending on them and copies
   the DLL into each output directory with `copy_if_different`; a missing file is a configure error. The
   synthetic-project test builds two executables in nested directories beside two excluded ones and expects exactly
   two staged copies and no excluded build.
3. CI: not wired. To reproduce the experiment, configure a Windows preset with `-DCRD_WARP_DLL=<destination>/d3d10warp.dll`,
   build, and run the DX12 executables (or the reversible D3DConfig wrapper variant `run-warp-diagnostics-pinned.py`
   under ignored `build/research-dev-workflow-20260912/`); the census driver field must read `1.0.20.0`.

## Findings and traps

- The OS build 10.0.26100.8972/33296 reads 8 bytes past a D3D12Core allocation inside `CreateStateObject`; ASan reports
  it in every DXR gate and in an SDK-only program with no Cerid code. Package 1.0.20 passes the same four gates under
  ASan with the same executables (`warp-135107-ac7699`): a provider fix for that defect, not a sanitizer suppression.
- The pinned build shares the inner-coverage defect of the OS build; that contract is handled by the
  [route recipe](2026-09-13-dx12-inner-coverage.md), not by the pin.
- Whole-suite qualification on `win-debug` (`warp-145345-ab068e` pinned, `warp-145917-2e693c` OS build, CTest parity):
  1.0.20 fails 12 of 177 `crd-gpu-context-dx12-tests` cases and 1 of 46 `crd-ceir-gpu-dx12-tests` cases that the OS
  build and hardware pass bit-exactly (eleven `precise` FP kernels without subgroup ops: MLP forward/backward, five
  FFT pipelines, two A-buffer composites, the visibility-buffer rasterizer; two wave-shaped kernels: B11 wave ops and
  the radix sort); `crd-scene-render-tests` passes 113/113 on both. The package also reports `wave_max=128` (OS: 4)
  and `rt_tier=12` (OS: 11). It is therefore not a drop-in provider for every lane: the pin is withdrawn from CI and
  the four hosted ASan DXR gates stay open ([session](../sessions/2026-09-13-inner-coverage-route-and-pinned-warp.md#whole-suite-qualification-and-withdrawal)).
- Cache hits still pass through the hash checks: the installer re-verifies the cached package and DLL every run.
- Do not add the DLL to PATH: safe DLL search order consults System32 before PATH, so only the application directory
  overrides the OS build. Do not ship the DLL with products; it is a test-lane provider.

## Code and reproduction

- [Installer](../../scripts/install-warp.py), [staging module](../../cmake/CrdWarp.cmake),
  [tooling tests](../../scripts/test-repository-tools.py); the workflow no longer references them.
- Local arms and the acquisition record live under ignored `build/research-dev-workflow-20260912/` (`warp-134948-07eef4`,
  `warp-135107-ac7699`, `warp-145345-ab068e`, `warp-145917-2e693c`, `warp-package-1.0.20/`); see the
  [dated session](../sessions/2026-09-13-inner-coverage-route-and-pinned-warp.md).
