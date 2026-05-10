# Phase 6 — Native physics

**Status:** ⚠ **Folded into Phase 3.1 (Eylem) on 2026-05-10.**

This phase originally planned to replace a PhysX-first binding with a
Cerid-native backend. ADR-0062 (2026-05-10) collapsed the two-phase
plan into one: Cerid builds **eylem** native from day 1, no PhysX wrap
step. ADR-0018's bind-then-replace lifecycle is superseded.

**Where to look now:**

- **`docs/phases/phase-3.1-eylem.md`** — the full slice plan
  (~30 slices over v0–v9; v0 = `crd-math` SIMD substrate; v1 = rigid 3D
  substrate; v2 = rigid 2D specialisation; v3 = XPBD soft / cloth / rope;
  v4 = maximal-coord articulations; v5 = vehicles; v6 = CCD +
  Featherstone reduced-coord articulations; v7 = FEM mesh deformation;
  v8 = GPU acceleration; v9 = differentiable + 9-config replay-hash CI).
- **`docs/decisions/0062-eylem-physics-architecture.md`** — module
  split, AoSoA-8 layout, broadphase / solver / 2D-3D-codebase /
  threading / determinism choices, ECS-native integration model.
- **`docs/decisions/0063-eylem-determinism-contract.md`** — FP contract,
  Cerid-internal trig / sort / hash substitutions, cross-thread merge
  discipline, snapshot-replay CI matrix.
- **`docs/research/cerid-eylem.md`** — industry survey + algorithm
  catalogue + the *why* behind every architectural choice.

The Phase 6 number is retained in the ROADMAP for legacy linking but no
new work lands here.
