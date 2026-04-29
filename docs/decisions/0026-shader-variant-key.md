# ADR-0026 — Shader variant key

**Date:** 2026-04
**Status:** Draft
**Tags:** [shader] [cache] [arch]

## Draft decision

- Variant identity is a typed tuple of **structural axes only**.
- The tuple hashes to a stable content-addressed key.
- The include graph and effective preprocessed source participate in the hash.
- Numeric specialization values do not pollute the structural variant key by
  default; they are represented separately in compile requests.

## Forward-fit answers

- Node editor: safe if the frontend produces the same structural tuple.
- Per-instance material variation: safe because per-instance data is not part
  of the structural key.
- New shader stages: safe if stage participates in the typed tuple.
- GLSL → Slang: safe because the key is structural/content based, not tied to
  GLSL text alone.
