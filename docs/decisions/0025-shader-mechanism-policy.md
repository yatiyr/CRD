# ADR-0025 — Shader mechanism policy

**Date:** 2026-04
**Status:** Draft
**Tags:** [shader] [renderer] [arch]

## Draft decision

- Permutations are for structural axes only: pass type, skinned/static,
  alpha mode, forward/deferred, and any other axis that changes shader
  structure or pipeline compatibility.
- Specialization constants are for numeric compile-time axes: cascade count,
  light caps, loop bounds.
- Bindless / UBO / push constants are for material parameters.
- Dynamic branching is allowed only for cheap, low-frequency runtime choices.

## Rejection criteria

- If an axis does not change shader structure, it must not become a
  permutation by default.
- If an axis is high-cardinality and frequently data-driven, it must not be a
  permutation axis.
- Material authoring convenience is not enough reason to push data into the
  permutation key.

## Forward-fit answers

- Node editor: safe, because the policy is mechanism-based, not frontend-based.
- Per-instance material variation: safe, because material parameters stay out
  of the structural permutation key.
- Compute / mesh / ray tracing: safe, same mechanism rule applies.
- GLSL → Slang swap: safe, mechanism policy does not depend on frontend.
