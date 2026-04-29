# ADR-0029 — Shader hot reload

**Date:** 2026-04
**Status:** Draft
**Tags:** [shader] [hot-reload] [runtime]

## Draft decision

- Hot reload lands early, not as late polish.
- Reload uses atomic swap semantics.
- Last-good artifact remains available on compile failure.
- Consumers observe reload events; they are not forced to crash or stall the
  frame because a source file became invalid.

## Forward-fit answers

- Node editor: safe because the reload contract is backend/runtime level, not
  tied to text files alone.
- Per-instance material variation: safe because materials bind to stable
  effect/variant handles.
- New stages: safe if reload remains artifact/handle based.
- GLSL → Slang: safe because the reload semantics live after the frontend seam.
