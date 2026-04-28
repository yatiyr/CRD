# ADR-0015 — Job system shape

**Date:** 2026-04
**Status:** Accepted
**Tags:** [jobs] [arch]

## Decision

- Lands in Phase 2.5, after the renderer / debug / allocator / shader path
  is real enough to inform the work-graph shape.
- Thread pool + fiber tasks from day one. Fibers chosen for stackful
  cooperative tasks (long async chains, deterministic scheduling, easier
  authoring of complex job graphs).
- Work-stealing scheduler. Per-frame allocator for transient task data.
- Gates async asset I/O, GPU upload, parallel command recording, parallel
  shader compilation, and Phase 4.1 parallel solvers.

## References

- `docs/phases/phase-2-graphics.md`
