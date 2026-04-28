# ADR-0021 — Animation architecture

**Date:** 2026-04
**Status:** Accepted
**Tags:** [animation] [arch]

## Decision

- Skeletal first, then blends, then IK, then cinematics. Each is a slice
  inside Phase 3.2 with its own quality pass.
- GPU skinning path from day one; CPU fallback for tools / cooker.
- Cinematic timeline is a separate layer with deterministic playback —
  reused by simulation / medical use cases for replay.

## References

- `docs/phases/phase-3-simulation.md`
