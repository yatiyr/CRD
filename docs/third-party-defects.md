# Third-party defects register

<!-- doc-role: reference -->
> Defects reproduced outside Cerid code in the providers, runtimes and tools the hosted lanes depend on. Rules:
> [AGENTS](../AGENTS.md); tracker: [ROADMAP](ROADMAP.md); gate: [check-registered-failures.py](../scripts/check-registered-failures.py).

## Policy

An entry is admitted only with, in this order: a reproduction that contains no Cerid code, or a faulting stack with
no Cerid frame on memory the engine did not allocate; the exact affected tests and lanes; the engine handling that
keeps every oracle intact; and a retirement trigger. Registered failures are never suppressed, filtered or skipped.
The tests run, sanitizers stay strict, the reports stay in the lane log, and only the lane's final step consults this
register: it compares the failing set with the registered set in both directions. An unexpected failure turns the lane
red; so does a registered test that passes, is skipped or does not run, which is how an entry is forced out of the
register when the provider is fixed. Every hosted lane, Windows and Linux, ends in the gate; lanes without entries
require zero failures and propagate CTest's exit code.
Each entry names the ROADMAP row that closed with this register as its evidence. An entry never widens by itself:
adding a test name is a documented decision with a reproduction, like the entry it joins.

## Registered failures

Lane keys are the CI preset names; test names are exact CTest names. The gate reads this block.

<!-- registered-failures -->
```json
{
  "lanes": {
    "linux-gcc-asan": [
      {"test": "assetio: OUR tangents vs the REFERENCE mikktspace.c ORACLE", "defect": "TP-5"}
    ]
  }
}
```

<a id="tp-1"></a>
## TP-1: the OS WARP reads 8 bytes past the runtime's DXIL library copy in CreateStateObject

| Field | Value |
|---|---|
| Component | Microsoft Basic Render Driver (WARP), inbox `d3d10warp.dll` 10.0.26100.8972 on the workstation and 10.0.26100.33296 on hosted `windows-latest`; vendor 1414, device 008c |
| Symptom | AddressSanitizer heap-buffer-overflow: a READ of size 8 by `memmove` called from `d3d10warp.dll`, 0 bytes past a heap block allocated by `D3D12Core.dll` whose size equals a DXIL library blob (2,924 and 2,568 bytes in the gates, 2,272 in the SDK-only probe) |
| Trigger | `CreateStateObject` for a raytracing pipeline, before any GPU work; the debug layer reports nothing |
| Reproduction outside Cerid | `dxr-asan-probe` (2026-09-12): a standalone MSVC ASan program linking only D3D12 and DXGI reproduces the same read at the same WARP offset ([investigation](sessions/2026-09-12-dx12-asan-investigation.md#sdk-only-rt-reduction)) |
| Provider isolation | Identical ASan executables pass all four gates when the signed Microsoft.Direct3D.WARP 1.0.20 DLL is placed beside them, with 32, 36, 31 and 11 assertions ([evidence](sessions/2026-09-13-inner-coverage-route-and-pinned-warp.md)) |
| Affected | No lane since 2026-10-23 (engine handling below). Before it, `win-asan` only: the four RT gates and, since DIAG.7a, the DXR pipeline-identity test, each creating a raytracing state object. Which library copy WARP read past varied between runs of the same binaries on hosted WARP 10.0.26100.33648 ([39746853196](https://github.com/yatiyr/CRD/actions/runs/39746853196) failed all five, [39923363836](https://github.com/yatiyr/CRD/actions/runs/39923363836) four, the identity test passing), so a registered set could not be stable ([session](sessions/2026-10-23-scheduled-runs-thread-probe-reap-race.md#win-asan-the-library-tail-keeps-warps-read-inside-the-runtimes-copy)) |
| Hosted evidence | [Run 34757652779](https://github.com/yatiyr/CRD/actions/runs/34757652779) at `ae44264`: `win-asan` fails exactly the four registered names, census `driver=10.0.26100.33296`, four reads 0 bytes past D3D12Core regions of 2,568, 2,924, 2,924 and 3,788 bytes; every other lane green. [Run 34766787633](https://github.com/yatiyr/CRD/actions/runs/34766787633) at `a0419cf`: first gated run, "registered failures for this lane: 4; observed failures: 4; results: 6789; ctest exit: 8", `gate: PASS`, `win-asan` green with the sanitizer unsuppressed [Run 37191910186](https://github.com/yatiyr/CRD/actions/runs/37191910186) at `660a085` (nightly, 2026-10-04): the DXR pipeline-identity test added by DIAG.7a fails with the same signature, a READ of size 8 by `memmove` in `d3d10warp.dll` 0 bytes past a 2,952-byte region `D3D12Core.dll` allocated, reached from `Dx12RasterContext::dxr_pipeline` through `CreateStateObject`, census `driver=10.0.26100.33438`; registered here the same day. Still current on 2026-10-17 and 2026-10-18 ([39013990584](https://github.com/yatiyr/CRD/actions/runs/39013990584), [39108972918](https://github.com/yatiyr/CRD/actions/runs/39108972918) and [39124869209](https://github.com/yatiyr/CRD/actions/runs/39124869209), driver 10.0.26100.33438): the same five tests, each a READ of size 8 at `d3d10warp.dll+0x180132995` 0 bytes past a `D3D12Core.dll` region of 2,952, 2,924, 2,568, 2,924 and 3,788 bytes from `dxr_pipeline` (`dx12_raster_context.cpp:3379`); the gate named them `registered TP-1: failed`, and in 39013990584 the one unexpected failure was the allocator corpus's process-start fault, which the nonreproduced-failure register records ([session](sessions/2026-10-18-diag-11b-build-speed-compile-time.md#tp-1-sightings)). |
| Engine handling | `Dx12RasterContext::dxr_pipeline` hands each DXIL library to `CreateStateObject` with 8 zero bytes after its container (`BytecodeLength` = container + 8); the runtime copies them, so the provider's 8-byte read stays inside the runtime's copy. The SDK-only probe with that tail is ASan-clean with the debug layer silent; a provider that reads further still reaches the sanitizer. The work-graph state object takes no tail: its test is ASan-clean on hosted WARP. Not adopted: the 1.0.20 replacement, which fails 13 bit-exact compute gates ([TP-3](#tp-3)); ASan suppressions, message filters and skips, by rule |
| Retirement | When the SDK-only probe without the tail is ASan-clean on the current WARP build, the tail and this entry are removed. Re-run the probe after each Windows runtime update |
| Row | [REPO.3c.10](ROADMAP.md#slice-repo.3c.10) |

<a id="tp-2"></a>
## TP-2: WARP reports a false native SV_InnerCoverage bit

| Field | Value |
|---|---|
| Component | Both WARP builds, inbox 10.0.26100.* and package 1.0.20; conservative rasterization Tier 3 is reported |
| Symptom | The native inner-coverage bit is set for partially covered pixels: 312 interior and 0 edge pixels against an exact oracle of 220 and 92 |
| Reproduction outside Cerid | SDK-only D3D12 program with a two-line pixel shader, DXIL and DXBC ([recipe](recipes/2026-09-13-dx12-inner-coverage.md)) |
| Affected | No lane since 2026-09-13: the documented software adapter takes the barycentric pixel-corner route and matches the oracle exactly; the native conformance case is a genuine skip on that provider |
| Engine handling | Route contract `inner_coverage_route()`; the native bit stays the contract on qualified providers |
| Retirement | When a WARP build passes the native conformance case; the route selection then stops treating that provider as software-unqualified |
| Row | [REPO.3c.7](ROADMAP.md#slice-repo.3c.7) |

<a id="tp-3"></a>
## TP-3: Microsoft.Direct3D.WARP 1.0.20 diverges from hardware and the OS build on bit-exact compute

| Field | Value |
|---|---|
| Component | NuGet package Microsoft.Direct3D.WARP 1.0.20, `d3d10warp.dll` SHA-256 `2a08692c…`, census `driver=1.0.20.0` |
| Symptom | 12 of 177 `crd-gpu-context-dx12-tests` cases and 1 of 46 `crd-ceir-gpu-dx12-tests` cases that compare bit-exactly with the CPU oracle fail: eleven `precise` FP kernels without subgroup ops and two wave-shaped kernels. The package reports `wave_max=128` and `rt_tier=12` where the OS build reports 4 and 11 |
| Reproduction | Whole executables on `win-debug` with and without the package, same binaries ([evidence](sessions/2026-09-13-inner-coverage-route-and-pinned-warp.md#whole-suite-qualification-and-withdrawal)); the mechanism is unmeasured |
| Affected | No lane: the package is not staged on hosted lanes |
| Engine handling | Withdrawn from CI; the installer and staging module stay as an opt-in harness ([recipe](recipes/2026-09-13-dx12-pinned-warp.md)) |
| Retirement | If a later package passes the whole DX12 executables, it may replace the TP-1 provider on hosted lanes after the same qualification |
| Row | [REPO.3c.10](ROADMAP.md#slice-repo.3c.10) |

<a id="tp-4"></a>
## TP-4: D3DConfig export fails with C00CEE2C on the workstation

| Field | Value |
|---|---|
| Component | `C:\Windows\System32\d3dconfig.exe` on the 2026-09-13 workstation image |
| Symptom | `d3dconfig --export` returns error C00CEE2C before any setting is changed; every other verb works |
| Affected | No lane; local reversible WARP diagnostics only |
| Engine handling | The wrapper variants record the export failure and keep the textual apps and device snapshots as the restoration proof; every run still reports `restoration: verified` |
| Retirement | When the export verb works again the variants can require the XML export as before |
| Row | none; recorded in the [route and provider session](sessions/2026-09-13-inner-coverage-route-and-pinned-warp.md) |

<a id="tp-5"></a>
## TP-5: MikkTSpace's edge-sort pivot PRNG shifts a 32-bit value by 32

| Field | Value |
|---|---|
| Component | MikkTSpace (pinned commit `633eb1ff`, MIT), the tangent-generation reference oracle, `mikktspace.c` |
| Symptom | UBSan: `mikktspace.c:1667:21: runtime error: shift exponent 32 is too large for 32-bit type 'unsigned int'`. The pivot PRNG in `QuickSortEdges` computes `t = uSeed & 31` then `uSeed >> (32 - t)`; when `t == 0` the shift is by 32, undefined for a 32-bit type. Non-recovering UBSan aborts the process |
| Trigger | Any `genTangSpaceDefault` call whose edge sort takes the pivot branch with `t == 0`; the assetio oracle hits it deterministically on its fixtures |
| Reproduction outside Cerid | Faulting stack is entirely MikkTSpace, no engine frame: `QuickSortEdges` (`:1667`) ← `BuildNeighborsFast` (`:1517`) ← `InitTriInfo` (`:1056`) ← `genTangSpace` (`:307`) ← `genTangSpaceDefault` (`:226`). The UB is on MikkTSpace's own local `uSeed`; the engine only supplies mesh data |
| Affected | `linux-gcc-asan` only (the one lane with UBSan), the one registered test. The same test passes with correct tangents on every non-sanitized lane and on `win-asan` (ASan without UBSan). The oracle is the unmodified upstream file, `#include`d as the reference; patching it would defeat the bit-exact comparison it exists for, so the engine keeps it pristine and registers the sanitizer-only finding |
| Hosted evidence | [Run 34821419392](https://github.com/yatiyr/CRD/actions/runs/34821419392) at `2b6dcdb0`: the first complete-tier run, `linux-gcc-asan` reports the shift at `mikktspace.c:1667` on this test; local reproduction on the `linux-gcc-asan` preset (WSL2, GCC 13.3) matches |
| Engine handling | None: the UB is in the reference oracle's own PRNG, our tangent implementation is separate and validated against it on every non-UBSan lane. No sanitizer exemption: a never-matching `$<CONFIG:Asan>` one was removed ([2026-10-05](sessions/2026-10-05-tp5-dead-exemption.md)) |
| Retirement | When a later MikkTSpace pin masks the shift (`>> ((32 - t) & 31)` or equivalent) or the oracle is dropped; the gate then fails on the unexpected pass and this entry is removed |
| Row | [REPO.3d](ROADMAP.md#slice-repo.3d); evidence [first complete-tier run repairs](sessions/2026-09-14-first-complete-tier-run-repairs.md) |

## The registered-failure gate

`scripts/check-registered-failures.py --lane <preset> --junit <ctest --output-junit file> --ctest-exit <code>` reads
this document's block and the lane's JUnit output. Exit 0 means the failing set equals the registered set for that
lane (or, for a lane without entries, that nothing failed and CTest exited 0). Exit 1 names every unexpected failure
and every registered test that did not fail; exit 2 means the register or the JUnit evidence is unusable; any other
code is CTest's own, propagated. The Windows matrix job runs it as the last line of its Test step, after the census
probe and the full CTest run. Unit tests live in `scripts/test-repository-tools.py`.
