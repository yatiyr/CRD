# ADR-0028 — Shader cache hierarchy

**Date:** 2026-04
**Status:** Draft
**Tags:** [shader] [cache] [vulkan]

## Draft decision

- Four cache tiers:
  1. source
  2. preprocessed
  3. SPIR-V
  4. VkPipeline
- `VkPipelineCache` sits under the fourth tier as the Vulkan-native helper.
- Cache keys are content-addressed and include the include graph.
- Invalidations are dependency-aware, not timestamp-only hacks.

## Forward-fit answers

- Node editor: safe if it feeds the same canonical IR/cache seam.
- Per-instance material variation: safe because material data is not the same
  thing as shader artifact identity.
- New stages: safe if stage and structural axes participate in keys.
- GLSL → Slang: safe because the cache contract is at/after the canonical IR seam.
