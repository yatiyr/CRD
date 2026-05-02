# Session: crd-jobs v1k — integration smoke + crd-app wiring

**Date:** 2026-05-02  
**Slice:** v1k  
**Branch:** main

---

## What was built

Phase 2.5 completion slice: `smoke_jobs` rewritten as a full public API demo and
`Application::run()` wired to initialize and shut down the job system.

### Application wiring

`ApplicationDesc` gains `crd::jobs::Config jobs_config{}` (public field, exposed in the header).
`Application::run()` calls:
```cpp
void Application::run()
{
    if (!m_valid)
        return;
    crd::jobs::init(m_desc.jobs_config);
    while (tick())
    {
    }
    crd::jobs::shutdown();
}
```

`crd-app` CMakeLists gains `crd-jobs` as a PUBLIC link target (required because `ApplicationDesc`
exposes `crd::jobs::Config` in the public header).

`application.hpp` gains `#include <crd/jobs/jobs.hpp>`.

### smoke_jobs rewrite

Complete replacement of the v1a raw fiber demo. Five sections, each printing a result line:

1. **run + wait** — single job via `make_job<F>()`, `run()`, `wait()`; verifies `done==1`.
2. **run_and_wait** — convenience overload; verifies `done==1`.
3. **parallel_for** — splits `[0,1000)` across 4 jobs; atomic sum verified against `999*1000/2 = 499500`.
4. **H/N/L priorities** — 10 High + 20 Normal + 40 Low jobs submitted via `run(std::span(...))`;
   all-ran counts verified (no ordering assertion — ordering is unit-tested).
5. **frame_alloc + frame_reset** — three allocs with alignment checks, `frame_reset()`, then a
   fourth alloc from cursor 0; all pointers `[[maybe_unused]]` to silence release-mode warnings.

Exit code 0 on pass; `CRD_ASSERT` kills process on failure.

---

## Files changed

- `engine/app/include/crd/app/application.hpp` — added `#include <crd/jobs/jobs.hpp>` and `jobs_config` field
- `engine/app/src/application.cpp` — `run()` calls `jobs::init()` / `jobs::shutdown()` with `!m_valid` guard
- `engine/app/CMakeLists.txt` — `crd-jobs` added to PUBLIC `target_link_libraries`
- `runtime/examples/smoke_jobs.cpp` — complete rewrite (v1a raw fiber demo → v1k public API demo)
- `docs/phases/phase-2.5-jobs.md` — v1k row marked ✅
- `context.md` — updated to Phase 2.5 COMPLETE, v1a–v1k shipped, test counts, session log

---

## Decisions

- **PUBLIC link for crd-jobs**: `ApplicationDesc` exposes `crd::jobs::Config` in the public header,
  so consumers of `crd-app` transitively need `crd-jobs` headers. PUBLIC link is correct.
- **`!m_valid` guard in run()**: Prevents `jobs::init()` from running if window/context creation
  failed. Matches the existing pattern in `tick()`.
- **`[[maybe_unused]]` on frame_alloc pointers**: In release mode `CRD_ASSERT` is a no-op, so
  variables only used in asserts become unused. `[[maybe_unused]]` is cleaner than `(void)`.
- **`std::span(jobs, count)` CTAD**: Used instead of `crd::containers::ConstSpan<JobDecl>{...}`
  because MSVC had trouble parsing brace-init of a template alias with angle brackets inside a
  function argument. CTAD works identically and matches the pattern in test_jobs.cpp.

---

## Smoke results

```
=== smoke_jobs (crd-jobs v1k — public API integration) ===
[smoke_jobs] 1. run+wait          OK  (done=1)
[smoke_jobs] 2. run_and_wait       OK
[smoke_jobs] 3. parallel_for       OK  (sum=499500, expected=499500)
[smoke_jobs] 4. H/N/L priorities   OK  (High=10, Normal=20, Low=40)
[smoke_jobs] 5. frame_alloc/reset  OK
[smoke_jobs] PASS
```

`smoke_renderer` also ran clean (exit 0) with the job system now initialized inside `Application::run()`.

---

## Six-configuration results

| Config | Tests |
|---|---|
| win-debug | 355/355 ✅ |
| win-release | 352/352 ✅ |
| win-relwithdebinfo | 355/355 ✅ |
| win-asan | 355/355 ✅ |
| win-clang-cl | 355/355 ✅ |
| win-tidy | 355/355, exit 0 ✅ |

---

## Phase 2.5 — COMPLETE

All 11 slices (v1a–v1k) shipped. Definition of done satisfied:

1. ✅ All 11 slices with unit tests.
2. ✅ `smoke_jobs` runs: main thread fiber conversion, parallel work dispatch, wait-on-counter, priority ordering all-ran check.
3. ✅ Six-configuration green.
4. ✅ `crd-resources` (Phase 2.6) can call `jobs::run()` without modification.
5. ✅ Pinned-job mechanism tested (unit tests in v1g).
6. ✅ ADR-0033 filed and cross-referenced.

---

## Next

**Phase 2.6** — `crd-resources` + asset cooker.
