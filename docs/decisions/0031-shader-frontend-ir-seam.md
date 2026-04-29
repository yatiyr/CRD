# ADR-0031 — Shader frontend → IR seam

**Date:** 2026-04
**Status:** Draft
**Tags:** [shader] [arch] [ir]

## Draft decision

- All frontends lower to SPIR-V in the current design.
- The seam is explicit so a future Cerid-IR can be inserted later without
  changing backend or consumer-facing handles.
- GLSL via shaderc is the first frontend.
- The future node-editor frontend must target the same seam instead of growing
  a second incompatible backend path.

## Forward-fit answers

- Node editor: this ADR exists specifically to keep that future path open.
- Per-instance material variation: unaffected; frontend choice does not change
  handle/material semantics.
- New stages: safe if the seam is stage-agnostic and IR-level.
- GLSL → Slang: safe because frontend replacement happens before the seam, not
  after it.
