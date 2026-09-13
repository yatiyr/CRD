# CI repair batch one: guards, tidy names and the pooled test allocator

<!-- doc-role: historical -->
> Dated evidence. Live owners: [REPO.3c.3](../ROADMAP.md#slice-repo.3c.3) and the accepted rows
> [REPO.3c.4.a](../ROADMAP.md#slice-repo.3c.4.a) through [REPO.3c.6](../ROADMAP.md#slice-repo.3c.6). Rules: [AGENTS](../../AGENTS.md).

## User direction

On 2026-09-13 the user assigned new implementation work after a review-only pause: "fix all the CI errors first. And
then finish all the in progress parts that are in between and left. And drive until the slice that is truly open and
all the slices behind it must be completed. Setup a loop for it with 60 seconds wakeup time." This resumes work at
the earliest unfinished row in table order; it is not a future-slice override. The user still owns every commit and
push; agents change the working tree only. The session loop is a per-minute scheduler inside the agent session.

## Published census at the human revision

Human HEAD `0b858a63bf62893e19024b162216bab76a189fa9` ("repository hardening") is tested by
[run 34726528230](https://github.com/yatiyr/CRD/actions/runs/34726528230). Compared with the previous
[run 34714148010](https://github.com/yatiyr/CRD/actions/runs/34714148010) at `7201a7b`:

| Lane | Failures at 7201a7b | Failures at 0b858a6 | Remaining failures at 0b858a6 |
|---|---:|---:|---|
| linux-gcc debug, release, relwithdebinfo, asan, debug-sse2, shipping | 0 | 1 each | `crd-no-malloc-allocator` |
| win-debug, win-release, win-debug-sse2, clang-cl, win-shipping, clang-cl-shipping | 6 each | 2 each | `crd-no-untagged-physical-numeric`, B1-f inner coverage |
| win-asan | 10 | 6 | the two above plus REN-38, REN-38-F13, REN-38-F6 and CEIR-19b RT gates |
| clang-tidy | build failed | build failed | one naming diagnostic, stopped at TU 1700 of 2057 |
| Repository checks, both OS | pass | pass | |

The published revision is the first hosted run of the DX12 repairs recorded in the 2026-09-12/13 sessions. On every
Windows lane, including ASan and Shipping, B18-a, B18-b, B18-c, RT-4 and the CEIR-18p impostor gate pass with 6, 6,
11, 8 and 19 assertions and silent validation captures; the B17-c atomic A-buffer case passes with 3,083 assertions.
The device census reports `engine_kind=2 (software)` with `documented_basic_render=1` for the flagless
1414:008c / 10.0.26100.33296 provider, where the previous revision reported hardware. Those are the published outcomes
REPO.3c.4.a, REPO.3c.4, REPO.3c.5.a through REPO.3c.5.d, REPO.3c.5 and REPO.3c.6 were waiting for; the remaining failures
belong to REPO.3c.3 (strict analysis and guards), REPO.3c.7 (inner coverage) and REPO.3c.10 (ASan RT gates).

## Root causes

1. **Linux guard.** The validation test introduced 17 `MallocAllocator` uses. Windows passed only because
   `check_no_malloc_allocator.ps1` computed its default root inside the `param` block, where `$PSScriptRoot` is empty:
   `Resolve-Path "/.."` became `D:\`, every scope was skipped and the guard printed PASS without scanning. The same
   defect was in `check_no_std_math.ps1` and `check_no_std_sort.ps1`; `check_no_non_ascii_test_names.ps1` had already
   recorded and avoided it on 2026-05-13. Only the malloc guard hid a violation; the other two also pass with an
   explicit root.
2. **Windows guard.** The new `FgClearHint` declared `float depth`. Linux passed because the bash regex required one
   character before the quantity token, so a field named exactly `depth` could never match; the PowerShell sibling
   uses `\w*` and is the stricter one.
3. **Strict tidy.** Hosted LLVM 20.1.8 stopped on the function-local `constexpr crd::u32 kN` in
   `test_work_smoke_dx12.cpp`; the previous run's three diagnostics were already repaired in the published revision.
   The pinned binary contains no `LocalConstexprVariable` or `StaticConstexprVariable` option string, and a probe
   compiled with the repository `.clang-tidy` reports a non-static function-local `constexpr` as `local constant`
   (lower_case) while namespace, class and `static` local constants pass as `kCamelCase`. The two dead keys and the
   comment claiming a granular category were wrong; CODING now states the convention the tool enforces.

## Repairs in the working tree

- `scripts/check_no_malloc_allocator.ps1`, `check_no_std_math.ps1`, `check_no_std_sort.ps1`: resolve `RepoRoot` in
  the body when the parameter is empty, with the reason recorded beside it.
- `scripts/check_no_untagged_physical_numeric.sh`: name prefix `[a-zA-Z_0-9]*`, matching the `.ps1` test.
- `scripts/check_no_malloc_allocator.sh`: the comment filter strips an optional drive letter before the line number, so
  the script gives the same answer on a Windows host as on Linux.
- `engine/gpu/gpu-context/include/crd/gpu/frame_graph.hpp`: `FgClearHint::depth` carries the
  `crd-lint-allow-untagged-physical` marker; a normalized [0,1] depth-clear value has no unit.
- `tests/gpu/gpu-context-dx12/test_dx12_validation.cpp`: every capture, context and descriptor-page allocator is a
  64 MiB or 4 MiB `TlsfAllocator`. The capture allocates only at construction and teardown under the main thread, so
  the single-threaded pool is safe for the concurrent-callback case.
- `tests/execution/ceir-gpu-dx12/test_work_smoke_dx12.cpp`: `expected_count`, mirroring the Vulkan twin.
- `tests/rendering/scene-render/test_scene_render.cpp`: `kExpectedIds` (static) and `expected_count` (local); the
  local helper had found both in a TU hosted tidy never reached.
- `.clang-tidy`: dead `LocalConstexprVariable*` and `StaticConstexprVariable*` keys removed; corrected comment.
- `docs/CODING.md`: function-local constants are lower_case whether `const` or `constexpr`; namespace/class-scope
  constexpr and `static` constants are `kCamelCase`. The preserved lesson records are unchanged; new records in the
  build/verification corpus carry the correction and the guard-parity rule.

## Local evidence, Windows Debug, MSVC, two workers

- CTest guards on `build/win-debug`: `crd-no-std-math-check`, `crd-no-std-transcendental-check`,
  `crd-no-std-sort-check`, `crd-no-non-ascii-test-names`, `crd-no-untagged-physical-numeric`,
  `crd-no-malloc-allocator` **6/6 passed** with the guards now scanning; both bash siblings pass on this host, and the
  corrected bash regex flags the unmarked `depth` field when the marker is removed from a probe copy.
- Rebuilt `crd-gpu-context-dx12-tests`, `crd-scene-render-tests` and `crd-ceir-gpu-dx12-tests` (32, 9 and 6 build
  steps, including the frame-graph header consumers). `DX12 validation|resource states|descriptors|stage interfaces`
  plus `CEIR-34 E4` **18/18 passed** on the NVIDIA 4070 Ti SUPER; `ceir 20b` DX12 work smoke **1/1 passed**.
- Pinned LLVM 20.1.8 helper: the probe file and the three renamed constants reproduce and clear the hosted
  diagnostics. The 258 translation units hosted tidy never reached are being analysed with the same helper; the
  result is appended below before REPO.3c.3 changes state.
- Documentation validator PASS after every documentation change; `git diff --check` clean.

## Sweep and close-out

The pinned LLVM 20.1.8 helper analysed the 257 C++ translation units the hosted tidy job never reached (the compile
database minus the 1,206 objects it had built, minus assembly and CUDA sources): **257 clean, none ungated**, so the
three renamed constants are the only naming diagnostics in the tree the hosted lane will meet. Every C++ header and
TU changed today parses clean under the same helper after one repair of a nested conditional in the new oracle.
Close-out: validator PASS (863 rows, 1,029 documents, 8,863 local links), hygiene guard 96 modules, tooling tests
16/16, `git diff --check` clean.

REPO.3c.3 held Needs CI with this evidence until the user published `ae44264`: [run 34757652779](https://github.com/yatiyr/CRD/actions/runs/34757652779)
completed the clang-tidy lane, passed every Linux lane's guards and every non-ASan Windows lane, and closed REPO.3c.3,
3c.4.a, 3c.4, 3c.5.a through 3c.5.d, 3c.5, 3c.6, 3c.7, 3c.8 and 3c.9 as Done in order. The pointer moved on through the
[route/provider session](2026-09-13-inner-coverage-route-and-pinned-warp.md), the [REPO.DEV audit](2026-09-13-repo-dev-audit.md)
and the [third-party register](2026-09-13-third-party-defect-register.md).
