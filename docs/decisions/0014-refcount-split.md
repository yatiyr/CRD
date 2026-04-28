# ADR-0014 — Reference counting split

**Date:** 2026-04
**Status:** Accepted
**Tags:** [memory] [resources]

## Decision

- Generic shared-lifetime primitives belong in `crd-memory` (intrusive
  ref-counting, atomic variants later).
- Resource-facing shared references, eviction, lazy loading, and
  hot-reload ownership belong in `crd-resources`.

## References

- `docs/phases/phase-2-graphics.md`
