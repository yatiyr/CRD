# Detour Queue

Side missions that interrupt the main roadmap. Use this when you need to
pause Phase X to do something else (a bug, a refactor, an experiment, an
external request).

## Rules

- Each detour gets its own file: `D-NNN-<slug>.md`.
- A detour pauses the main roadmap. `context.md` records "Active detour:
  D-NNN" so future-you knows.
- Each detour has: title, why, scope, exit criteria.
- Run detours as their own mini-pipeline (research → coder → tester →
  reviewer → docs-keeper). Same DoD applies.
- When done: `@docs-keeper` closes the detour. If it changed architecture,
  it produces a new ADR; otherwise just a session log entry. The main
  roadmap then resumes.
- Detours that grow beyond their exit criteria become real phase slices —
  promote them, don't let them quietly take over.

## Active detours

(none — main roadmap resumes at Phase 3.0 v1d)

## Closed detours

- **D-001 — Memory infrastructure for elite-tier allocator coverage** (opened + closed 2026-05-07). Shipped `TlsfAllocator` (canonical Conte/Masmano TLSF, arbitrary alignment, `try_allocate` non-throwing path) + `GrowablePoolAllocator` (auto-growing pages of fixed-size aligned blocks) + refactored `crd::scene::ChunkAllocator` to wrap GrowablePool. Closed the v1c1 O(N) `ChunkAllocator::free` perf debt. See `D-001-memory-infrastructure.md` (closed) and the two session logs.
