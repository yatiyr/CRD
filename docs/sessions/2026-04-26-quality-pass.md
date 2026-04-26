# Session — 2026-04-26 — Phase 1 quality pass

## Goal

Close Phase 1's quality pass before starting `crd-math`:

1. CI matrix
2. benchmark suite + baseline
3. PCH
4. `runtime/examples/` split
5. clang-cl validation
6. clang-tidy preset validation
7. public-header review
8. real assert-bridge test

## What we built / changed

- **CI upgraded** in `.github/workflows/ci.yml`:
  - Windows MSVC matrix: `win-debug`, `win-release`, `win-asan`
  - Separate `win-tidy` job
  - Separate `win-clang-cl` job
- **README gained a CI badge** and minimal project entry-point text.
- **Runtime split landed**:
  - `runtime/src/main.cpp` is now the startup skeleton only
  - `runtime/examples/smoke_log.cpp`
  - `runtime/examples/smoke_memory.cpp`
  - `runtime/examples/smoke_containers.cpp`
- **PCH landed** via `crd/core/pch.hpp` and a root CMake helper applied to
  engine, tests, and runtime targets.
- **Benchmark suite landed** in `tests/bench/test_bench.cpp` with Catch2
  benchmarks for:
  - disabled `CRD_LOG_TRACE`
  - async log push
  - `Array<u32>::push_back` 1k amortised
  - `HashMap<u32,u32>` insert/find/erase at 1M
  - `String` SSO vs heap construct + assign
- **Baseline committed** at `docs/bench/baseline_2026-04.md`.
- **Real assert bridge test landed**:
  - `crd::set_assert_platform_handler(...)` added to core
  - tests install a no-op platform handler
  - trigger real `CRD_ASSERT(false)` and verify `Critical` hits a
    `RingBufferSink`
- **clang-cl portability issue found and fixed**:
  - `/Zc:preprocessor` was MSVC-only and failed under clang-cl with
    `-Wunused-command-line-argument`
  - CMake now only adds it for non-clang MSVC
- **Header review done** and a few core header comments were tightened up.

## Plain-English explanation

This session was not about adding a new subsystem. It was about making the
 existing four subsystems feel like a real foundation rather than a growing
 pile of good code. The result is that the project now has automated build
 coverage, a performance baseline, a cleaner runtime layout, a real
 end-to-end assert proof, and a compiler story that is no longer MSVC-only.

The most useful surprise was clang-cl: it immediately found an assumption we
 had baked in for MSVC (`/Zc:preprocessor`). That is exactly why this session
 happened before math.

## Decisions made

- **PCH stays on** even though the first clean Debug full-build measurement on
  this small tree was slightly slower with PCH than without it.
  - no PCH: `13.61 s`
  - PCH: `14.11 s`
  - Reason: the project is still small, so the upfront PCH build cost hides
    the likely win. We want the substrate in place before math/platform add
    more TUs.
- **Release keeps the real assert bridge test registered but logically
  skips the end-to-end path**, because `CRD_ASSERT` is compiled out when
  `CRD_ENABLE_ASSERTS=OFF`. Debug/ASan still prove the real path.
- **clang-cl is the required cross-compiler bar for this pass.** Linux GCC
  remains desirable, but the roadmap explicitly allowed that to bleed into a
  follow-up if Windows clang-cl surfaced work first.

## Files touched

- `CMakeLists.txt` — default PCH helper, clang-cl-compatible MSVC flag split
- `CMakePresets.json` — added `win-clang-cl`
- `.github/workflows/ci.yml` — CI matrix + tidy + clang-cl
- `README.md` — CI badge and docs pointers
- `engine/core/include/crd/core/pch.hpp` — new
- `engine/core/include/crd/core/assert.hpp` — platform-hook API + comment pass
- `engine/core/src/assert.cpp` — platform-hook implementation
- `engine/core/include/crd/core/{core,platform,types}.hpp` — comment pass
- `engine/*/CMakeLists.txt` — PCH applied to shipped module targets
- `runtime/CMakeLists.txt` — split runtime targets
- `runtime/src/main.cpp` — startup skeleton only
- `runtime/examples/*.cpp` — new smoke executables
- `tests/CMakeLists.txt` + `tests/bench/*` — benchmark target added
- `tests/*/CMakeLists.txt` — PCH applied to test targets
- `tests/log/test_log.cpp` — real assert bridge test
- `docs/bench/baseline_2026-04.md` — new baseline file
- `docs/ROADMAP.md` — quality pass status updated, where-left-off moved to math

## Tests / verification

- `win-debug`: ✅ build clean, `125/125` tests green
- `win-release`: ✅ build clean, `124/124` tests green
- `win-asan`: ✅ build clean, `125/125` tests green, no ASan failures
- `win-tidy`: ✅ config + build clean
- `win-clang-cl`: ✅ config + full build clean

Benchmark baseline captured from `win-release`:

- Disabled `CRD_LOG_TRACE`: `0.191572 ns`
- Async log push: `396.515 ns`
- `Array<u32>::push_back` 1k: `853.786 ns`
- `HashMap<u32,u32>` insert 1M: `45.2836 ms`
- `HashMap<u32,u32>` find 1M: `12.0909 ms`
- `HashMap<u32,u32>` erase 1M: `26.6381 ms`
- `String` SSO construct + assign: `2.9374 ns`
- `String` heap construct + assign: `48.0579 ns`

## Next session starts with

`crd-math` v1 design discussion.

Topics to settle before coding:

1. Vec layout (`.x/.y/.z/.w` storage vs array-only)
2. Quaternion convention
3. Exact column-major storage contract
4. Test count target and split across vec / matrix / primitive passes
