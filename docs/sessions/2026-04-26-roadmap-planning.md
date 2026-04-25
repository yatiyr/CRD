# Session — 2026-04-26 — roadmap & docs structure

## Goal

No code this session. Decide the order of the remaining Phase 1 modules,
agree on a memory strategy that survives the open-world goal, and set up a
docs structure so future-me can pick up where we left off without scrolling
through chat history.

## What we built / changed

- **`docs/ROADMAP.md`** — single source of truth: phase plan, status table,
  decision log (chronological), glossary, "where I left off" pointer.
- **`docs/sessions/SESSION_TEMPLATE.md`** — template for one-file-per-session
  notes. Filename convention: `YYYY-MM-DD-short-topic.md`.
- **`docs/sessions/2026-04-26-log-module.md`** — backfill for the previous
  session that built `crd-log`.
- **`docs/sessions/2026-04-26-roadmap-planning.md`** — this file.
- **`docs/systems/`** directory created (overviews to land alongside).
- No engine code touched.

## Plain-English explanation

We didn't ship a system today. We shipped a *map*. The roadmap file tells
you the order of things, what's done, what's next, and why every weird
decision was made (so we don't re-litigate them in three months). The
sessions folder tells the story of the project one file at a time —
each session = one file = a short writeup of what changed and why.

The reason for the split: the roadmap is supposed to stay short and
high-level (you read it to know "where am I"), while the session files
hold the actual narrative ("what did past-me do, and what was she
thinking"). When a system is brand-new, its session note doubles as a
plain-English explanation. When a system is old and stable, sessions
just track tweaks and commits.

## Decisions made

- **Memory: two-phase strategy.** Phase A (now): `IAllocator` interface +
  4 simple allocators (Malloc / Linear / Stack / Pool). Phase B (during
  graphics): TLSF, BuddyAllocator, RingAllocator, StreamingAllocator,
  GPUAllocator. Real streaming workload-driven, not theory-driven.
- **`IAllocator` exposes `reallocate` and `allocation_size`** with default
  implementations from day one, so Phase B can override without breaking
  the interface.
- **Containers take `IAllocator*` as a constructor argument**, not a
  template parameter. Keeps types stable across allocator changes
  (EA STL / Bitsquid pattern). This is the single decision that makes
  open-world streaming a drop-in change later, instead of a refactor.
- **Streaming is a pipeline, not just an allocator.** It needs allocator +
  job system + async filesystem + resource manager. Doing the allocator
  alone today is wasted work; doing the architecture correctly today is
  the whole game.
- **Math: column-major matrices, radians everywhere, scalar first.**
- **Phase 1 order:** core ✅ → log ✅ → memory → math → containers → platform.
- **Docs structure:** one ROADMAP, one file per session, one overview per
  system. Long deep-dives (like `docs/log/LOG_FILE.md`) live under their
  module's folder.

## Files touched

- `docs/ROADMAP.md` — created.
- `docs/sessions/SESSION_TEMPLATE.md` — created.
- `docs/sessions/2026-04-26-log-module.md` — backfilled.
- `docs/sessions/2026-04-26-roadmap-planning.md` — this file.
- `docs/systems/` — directory created (empty for now).

## Tests / verification

- No code changed → no tests run. Build state unchanged from end of
  previous session (13/13 Catch2 tests still green).

## Next session starts with

1. Open `docs/ROADMAP.md`, re-read the "Where I left off" section.
2. Wire `crd-core` assert handler → `crd-log` Critical. Small bridge:
   `assert.hpp` exposes a `set_assert_handler()` callback; `crd-log::init()`
   registers a default handler that emits a Critical record and flushes.
   No reverse dependency from core to log.
3. Add a test verifying that `CRD_ASSERT(false)` (in a way that doesn't
   abort the test runner) lands in a `RingBufferSink` as Critical.
4. Begin `crd-memory` v1: create `engine/memory/` skeleton, write
   `alignment.hpp` (`align_up`, `is_pow2`, `kDefaultAlignment = 16`,
   `kCachelineSize = 64`), write `memory_stats.hpp` (debug-only counters),
   write `allocator.hpp` (the agreed `IAllocator` interface with
   `reallocate` and `allocation_size` defaults), write `MallocAllocator`
   on top of `_aligned_malloc` / `aligned_alloc`.

End-of-session goal: `crd-memory` library links, `MallocAllocator` works,
5–8 Catch2 tests green covering alignment correctness and stats counters.
