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

- **D-001 — Memory infrastructure for elite-tier allocator coverage** (opened 2026-05-07). Pauses Phase 3.0 v1d. TLSF allocator (D-001-a) + GrowablePool + ChunkAllocator refactor (D-001-b). See `D-001-memory-infrastructure.md`.

## Closed detours

(none yet)
