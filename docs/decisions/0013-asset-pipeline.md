# ADR-0013 — Asset pipeline

**Date:** 2026-04
**Status:** Accepted
**Tags:** [resources] [arch]

## Decision

- Asset pipeline is a separate executable (`crd-tools/asset_cooker`).
- Runtime never imports source assets. glTF / PNG / HDR / WAV → Cerid
  binary formats (`.crd_mesh`, `.crd_tex` BC-compressed, `.crd_envmap`,
  etc.).
- Editor will eventually drive the cooker.

## References

- `docs/phases/phase-2-graphics.md`
