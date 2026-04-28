# ADR-0012 — Configuration substrate

**Date:** 2026-04
**Status:** Accepted
**Tags:** [config] [arch]

## Decision

- `crd-config` is its own mini-module rather than scattered per-consumer
  parsing.
- Format: TOML via `toml++` (single-header, exceptions-free mode).
  Reasons: comments, deterministic spec, type safety, ASan cleanliness,
  ecosystem.
- Authoring text vs runtime binary rule still applies. Configs are
  authoring data, parsed directly at runtime — small, not hot-path.
- Schema-with-defaults: every `get<T>(key, default)` is non-fatal with a
  logged warning. Strict mode opt-in for production builds.
- ImGui's own `imgui.ini` is NOT replaced. Cerid TOML config sits above
  it (theme, panels, debug toggles); ImGui internal layout state stays
  with ImGui's own format.

## References

- `docs/phases/phase-1.6-config.md`
