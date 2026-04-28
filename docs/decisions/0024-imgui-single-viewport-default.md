# ADR-0024 — ImGui single-viewport default

**Date:** 2026-04
**Status:** Accepted
**Tags:** [imgui] [ui] [vulkan]

## Decision

- ImGui debug overlay ships with **docking enabled** by default.
- ImGui **multi-viewport stays off by default**.
- Multi-viewport remains configurable through `crd-config`, but the default
  policy is single-viewport docking because Vulkan multi-viewport introduces
  extra platform/backend rough edges without being necessary for the current
  debug-overlay role.

## References

- `docs/phases/phase-2-graphics.md`
