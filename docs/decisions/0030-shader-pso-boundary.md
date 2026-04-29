# ADR-0030 — Shader / PSO boundary

**Date:** 2026-04
**Status:** Draft
**Tags:** [shader] [rhi] [renderer]

## Draft decision

- `crd-shader` owns effect identity, reflection, variant identity, cache keys,
  and the shader-side inputs to PSO creation.
- `crd-rhi` / backend own the actual pipeline object creation and native PSO
  representation.
- PSO hash ownership must be explicit: shader variant identity and fixed
  pipeline state both participate, but pipeline objects are not owned by the
  frontend.

## Forward-fit answers

- Node editor: safe because frontend does not own native PSO objects.
- Per-instance material variation: safe because material data does not have to
  own PSO creation.
- New stages: safe if PSO construction remains backend-owned.
- GLSL → Slang: safe because the seam between effect metadata and native PSO
  creation remains intact.
