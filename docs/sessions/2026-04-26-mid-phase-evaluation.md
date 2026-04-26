# Session — 2026-04-26 — mid-phase quality evaluation

## Goal

No code this session. Step back, look at the project as a whole, decide
what state it's in, and plan a dedicated **quality session** to land
*before* `crd-math` work begins. The point: math is a 3-session block;
starting it on a shaky foundation is worse than spending one session
hardening what's already there.

## What we built / changed

- `docs/sessions/2026-04-26-mid-phase-evaluation.md` (this file).
- `docs/ROADMAP.md` — Phase 1 "finishing" step expanded from a single
  bullet into eight concrete sub-steps (7a–7h), and the order moved:
  the quality pass now lands **before** math, not after platform.
  Added a new decision-log entry capturing the mid-phase evaluation.
- No engine code touched. All test counts are still 119/119 Debug,
  118/118 Release, 119/119 ASan from the previous session.

## Plain-English explanation

Right now, `crd-containers` v1 just shipped. The engine has four
modules done, every test green in three build flavours, a clean
one-way module dependency graph, and an extensive doc history. That
sounds great — but a few things are missing or weak that will become
expensive if we wait until Phase 1 closes:

- **No CI.** Every session ends with a manual three-flavour test run.
  Skipping one (because we're tired or in a hurry) is a real risk.
- **No performance baseline.** Our claim that "disabled `CRD_LOG_TRACE`
  costs literally zero" is verified by reading the Release binary's
  symbol table, not by measurement. We don't know how fast `Array::
  push_back`, `HashMap::find`, or `String` SSO actually are.
- **Compiler coverage is one (MSVC).** The POSIX code paths in
  `crd-memory` and `crd-log` exist but have never been compiled.
  `__VA_OPT__` needed `/Zc:preprocessor`; other compilers may have
  their own surprises.
- **Header compile cost is unmonitored.** Templates everywhere, no
  PCH, no unity build. Full builds take ~30+ seconds.
- **`runtime/main.cpp` is creeping toward 200 lines** of "demo every
  module" code. Hard to read, hard to extend.
- **Logger still uses `std::deque`/`std::mutex`/`std::condition_variable`
  for its async queue, and `std::string` for stored records.** This is
  fine, but the "we use our own containers everywhere" story isn't yet
  true. Becomes refactor pressure when streaming arrives in Phase 2.

None of these are emergencies. All of them are easier to fix now,
when the surface area is small (~3900 LoC engine code, ~120 tests),
than later when more modules pile on.

The decision: spend **one full session** before math doing a quality
pass. The session's deliverables become Phase 1 step 7a–7h.

## Decisions made

- **Quality session moves to BEFORE math, not after platform.**
  Original ROADMAP put "finishing" at the end of Phase 1 (step 7).
  We're moving it forward to *between* containers and math. Reasoning:
  - Math is bug-prone (numerical correctness, alignment for SIMD
    later). Catching MSVC-only assumptions in `crd-math` headers is
    much better with multi-compiler CI in place.
  - Performance regressions are easier to bisect when the baseline is
    drawn early.
  - `runtime/examples/` split should happen before math adds yet
    another smoke section.
- **The quality session has eight scoped sub-tasks** (7a–7h, see ROADMAP).
  Not all are guaranteed to finish in one calendar session — some
  (clang-cl, Linux GCC) might surface issues that take longer. The
  goal is to *land* CI + benchmarks + PCH + runtime split + Doxygen
  review confidently, even if cross-compiler matrix takes a follow-up.
- **Phase 1 step numbering rewritten** to put the quality pass between
  containers (step 4) and math (now step 6, was step 5). Platform is
  now step 7. Original "finishing" step 7 becomes step 8.

## Quality scorecard (subjective)

| Dimension | Score | Notes |
| --- | ---: | --- |
| Architecture | 9/10 | Allocator pattern, dep graph, streaming-ready interfaces — production-quality decisions held under scrutiny so far. |
| Test coverage | 8/10 | 119 tests; key invariants pinned (Robin Hood backshift, SSO discriminant, sub-budget exhaustion, heterogeneous hash equality). Gaps: multi-thread Robin Hood, very-large heap allocations, formatter locale edge cases. |
| Documentation | 9/10 | Sessions + ROADMAP + deep-dives + systems overviews are all in sync. Missing: API reference (Doxygen). |
| Performance | 5/10 | Works, looks reasonable, never measured. No baseline. |
| Build infrastructure | 6/10 | Three working presets, but no CI, no PCH, no unity build. Manual three-flavour pass per session. |
| Cross-platform | 3/10 | Windows MSVC only. POSIX paths written, never compiled. |
| C++ quality | 9/10 | Modern C++20 idiomatic, no exceptions, fail-fast asserts, type-safe APIs, `[[nodiscard]]` where it matters. |
| Discipline | 10/10 | ROADMAP fidelity, decision log, session-by-session honesty about what was hard and what got deferred. |

**Aggregate: ~7.4/10.** "Serious project on a good track, hardening
work due before the surface area grows."

## Risks (categorised)

### Low risk
- **Module architecture won't need refactoring.** The
  allocator-as-constructor-arg pattern, the `IAllocator*` interface
  with `reallocate`/`allocation_size` defaults, and the one-way module
  dependency graph have all held up across four modules. Streaming
  allocator integration in Phase 2 is genuinely a drop-in change.
- **Documentation system stays.** Sessions folder, ROADMAP, deep-dives
  — all maintained per session, no rot.

### Medium risk
- **Logger STL dependency.** `std::deque` async queue + `std::string`
  message ownership. Fine for v1, but when job system arrives in
  Phase 2 the SPSC lock-free path needs custom containers. Plan: do
  it as part of the job system migration, not now.
- **No performance baseline.** Six months from now, "the engine got
  slower" will be hard to attribute without numbers from today.
  Mitigated by step 7b (benchmark suite) in the upcoming quality
  session.
- **Single-compiler.** Code may not even compile under clang-cl or
  GCC. Mitigated by step 7e (cross-compiler matrix), though that
  step has the most surprises potential.

### High risk
- **None, honestly.** The largest project-level risk is **scope
  creep** — jumping ahead to Phase 2 graphics before math + platform
  finish. So far this discipline has held; the quality-pass-before-math
  decision in this session is consistent with that discipline.

## Quiet gaps (worth knowing, not yet acted on)

1. **Real `CRD_ASSERT(false)` bridge test is absent.** The handler is
   exercised manually (because Windows MessageBox would block the
   test runner). End-to-end "real assert + log captured" is unproven.
2. **Force-link anchor pattern is manual.** Each new module-level
   `.cpp` that doesn't otherwise get referenced needs an anchor in a
   consumer's umbrella header. Easily forgotten on the next module.
3. **`std::format` locale edge cases untested.** Format specifiers
   like `{:.3f}` may behave differently under non-en_US.UTF-8
   locales. Could surprise users on Turkish locales (decimal comma).
4. **`default_allocator()`'s leak status is unverified.** The
   function-local-static `MallocAllocator` is never destroyed; the OS
   reaps the memory on process exit. ASan typically catches this kind
   of "leaked at exit" but our runs have stayed silent — either ASan
   is suppressing or we're truly clean. Should explicitly verify.
5. **`static_assert(sizeof(String) == 32)` is MSVC-specific.** Other
   ABIs may align the `IAllocator*` field differently inside the
   struct after the 24-byte union. Will fire on non-MSVC if so —
   surfaced by step 7e.

## Phase 1 progress

```
[ DONE ]  crd-core
[ DONE ]  crd-log
[ DONE ]  crd-memory
[ DONE ]  crd-containers (v1a + v1b + v1c + v1d)
[ NEXT ]  Quality pass (7a–7h, one session, possibly two)
[      ]  crd-math v1 (Vec)
[      ]  crd-math v2 (Mat + Quat + Transform)
[      ]  crd-math v3 (AABB / Sphere / Ray / Plane / Frustum)
[      ]  crd-platform v1 (Window + Timer + Input via GLFW)
[      ]  crd-platform v2 (Filesystem + DynamicLibrary + threading)
[      ]  Phase 1 closeout sweep (CONTEXT.md, retrospective)
```

Estimated **8–10 sessions** to Phase 1 close (assuming the quality
pass takes one or two and cross-compiler shakeout is bounded).

## Files touched

- `docs/sessions/2026-04-26-mid-phase-evaluation.md` — new (this file)
- `docs/ROADMAP.md` — Phase 1 step list rewrite, new decision-log
  entry, "Where I left off" updated to point at quality session
- `CONTEXT.md` — no change

## Tests / verification

Nothing changed. Test counts unchanged from previous session:
- Debug: 119/119
- Release: 118/118
- ASan: 119/119

## Next session starts with

**Quality pass session (Phase 1 step 7a–7h).** Detailed breakdown in
ROADMAP, but at a glance:

1. **7a — CI.** GitHub Actions workflow with matrix
   `(Debug | Release | ASan)` × MSVC. Trigger on push and PR.
   Cache CPM downloads.
2. **7b — Benchmark suite.** Catch2 benchmarks under
   `tests/bench/`. At minimum: disabled `CRD_LOG_TRACE` cost,
   async log producer push cost, `Array::push_back` 1k amortised,
   `HashMap` insert/find 1M. Capture results in a baseline file
   committed to the repo.
3. **7c — PCH.** `crd-core` types + asserts + platform precompiled.
   Apply to all engine + test + runtime targets. Measure full-build
   delta.
4. **7d — `runtime/examples/` split.** One executable per module
   smoke (`smoke_log`, `smoke_memory`, `smoke_containers`).
   `runtime/main.cpp` becomes the canonical engine startup
   skeleton, not a kitchen sink.
5. **7e — clang-cl on Windows + Linux GCC stretch.** Probably the
   most surprising step. Likely catches non-portable include paths,
   `__VA_OPT__` dependencies, alignment assumptions, etc.
6. **7f — clang-tidy preset.** Run `win-tidy` and triage. Either
   fix or explicitly suppress every category.
7. **7g — Doxygen-friendly review.** Walk public headers, ensure
   doc comments are in shape. Don't generate yet — just confirm the
   substrate is good.
8. **7h — Real `CRD_ASSERT(false)` bridge test.** A way to invoke
   the assert path without blocking on MessageBox (likely:
   temporarily install a no-op handler that bypasses the platform
   UI for tests only).

End-of-session goal: CI green, benchmark baseline captured,
PCH measurably faster, runtime split, clang-cl at minimum compiling.
Cross-compiler matrix may bleed into a follow-up if Linux GCC
surfaces a lot.

Then — and only then — math v1 design discussion.
