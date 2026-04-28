# ADR-0022 — Open-world streaming pipeline

**Date:** 2026-04
**Status:** Accepted
**Tags:** [memory] [resources]

## Decision

Streaming is a *pipeline*, not just an allocator. Required pieces:

1. Allocator architecture (Phase 1, shipped)
2. Job system (Phase 2.5)
3. Async filesystem I/O (`crd-platform` + Phase 2.5 jobs)
4. Resource manager / streamer (`crd-resources`, Phase 2.6)
5. Streaming allocator implementation (Phase 2.2)

Doing the streaming allocator alone, without 2–4, is wasted work.

## References

- `docs/phases/phase-2-graphics.md`
- `docs/phases/phase-3-simulation.md`
