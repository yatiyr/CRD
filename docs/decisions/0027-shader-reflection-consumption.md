# ADR-0027 — Shader reflection consumption model

**Date:** 2026-04
**Status:** Draft
**Tags:** [shader] [reflection] [rhi]

## Draft decision

- `spirv-reflect` is the source of truth for:
  - descriptor bindings
  - push-constant ranges
  - vertex attribute layout
  - material parameter discovery
- `crd-shader` consumes reflection eagerly; manual descriptor authoring is not
  the primary path.

## Forward-fit answers

- Node editor: safe if it also lowers to SPIR-V and therefore yields the same
  reflection path.
- Per-instance material variation: safe because reflection discovers the
  parameter surface rather than hard-coding one authoring model.
- New stages: safe as long as reflection supports them and the consumer model
  remains table-driven.
- GLSL → Slang: safe because reflection happens after lowering to SPIR-V.
