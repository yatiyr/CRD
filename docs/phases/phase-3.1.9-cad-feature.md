# Phase 3.1.9 — `crd-cad-feature`: parametric features + drafting + GD&T

**Status:** 📋 planned (ADR-0077 §3.1.9)
**ADR:** `docs/decisions/0077-multi-domain-expansion-vision.md`
**Slot:** after Phase 3.1.8 (`crd-brep`) close.

## Why this exists

`crd-brep` is the **geometric kernel** (B-rep solids, NURBS surfaces, exact operations). Modeling *workflow* — parametric history, sketches with constraints, drafting drawings, GD&T — is a distinct layer that consumes B-rep and adds the user-facing CAD experience.

Splitting them is the same pattern as `crd-shader` (compilation kernel) vs `crd-material` (authoring layer) in Phase 2.3/2.8.

## Scope

### Feature trees (parametric history)

A feature is an operation on the model: "extrude this sketch 50mm", "fillet these edges with radius 3", "shell with wall thickness 2". The feature tree is a directed acyclic graph of these operations; rebuilding from the tree produces the current B-rep.

- Feature types: sketch-derived (extrude / revolve / sweep / loft) + edit (fillet / chamfer / draft / shell / pattern / mirror / split / combine) + reference (datum plane / axis / point).
- Rebuild on edit: changing a sketch dimension propagates downstream; if a feature breaks (referenced edge no longer exists), surface the error to the user.
- Suppression / unsuppression of features (toggle without delete).
- Configuration variations (Solidworks pattern — multiple configurations sharing a feature tree with different dimension values).

### 2D sketching with constraints

A sketch is a 2D drawing on a plane; constraints (geometric + dimensional) drive its shape. The constraint solver is the core algorithm.

- Geometric constraints: horizontal / vertical / parallel / perpendicular / tangent / concentric / collinear / coincident / equal length / symmetric.
- Dimensional constraints: distance / angle / radius / diameter / parametric expressions.
- Solver: typically Newton-Raphson over the constraint Jacobian (`crd-hesap-iterative`), with a degree-of-freedom counter to detect over/under-constrained.
- Sketch entities: line / arc / circle / spline / ellipse / point / construction geometry.

### Drafting / dimensioning / GD&T

Engineering drawings from 3D models.

- 2D drawing views (front / top / right / isometric / section / detail / break / aligned / projected) from a 3D B-rep model.
- Dimensioning per ISO 128 / ASME Y14.5: linear / angular / radial / diameter / arc length / chamfer / chain / baseline / ordinate.
- GD&T (Geometric Dimensioning and Tolerancing) per ASME Y14.5: form (flatness / straightness / circularity / cylindricity), orientation (parallelism / perpendicularity / angularity), location (position / concentricity / symmetry), runout, profile.
- Surface finish symbols (Ra / Rz), weld symbols, bill of materials (BOM).
- Drawing template system (title block, border, revision history).

## Dependencies

- `crd-brep` (the underlying geometric kernel — Phase 3.1.8)
- `crd-hesap-iterative` (constraint solver Newton-Raphson + sparse linear solve)
- `crd-hesap-opt` (over/under-constrained system handling)
- `crd-scene` (feature tree as ECS entities — each feature is an entity, edges are relations)
- `crd-renderer` (drawing view rendering — orthographic projection + hidden-line removal)

## Sub-modules (planned)

- `crd-cad-feature-tree` — feature DAG types, rebuild engine, error recovery.
- `crd-cad-sketch` — 2D sketch entities + constraint solver.
- `crd-cad-drafting` — 2D drawing view extraction, dimensioning, hidden-line removal.
- `crd-cad-gdt` — GD&T tolerance authoring + validation.

## Reference reading

- Solidworks API / Onshape API / Fusion 360 API documentation (parametric workflow patterns).
- Roller "Constraint-based 2D Geometric Solving" (1987) — original sketch solver paper.
- ASME Y14.5-2018 — GD&T standard.
- ISO 128 — engineering drawing standard.
- Hoffmann & Joan-Arinyo "On user-defined features" (1998).

## Out of scope

- Geometric kernel itself (Phase 3.1.8 `crd-brep`).
- CAM toolpath generation (Phase 3.1.13 `crd-cam`).
- FEA analysis (Phase 3.1.12 `crd-fea`).
- 3D animation / mocap (Phase 3.2).

## Open questions

- **Constraint solver scaling** — Solidworks scales to hundreds of constraints per sketch; Onshape to thousands. Newton-Raphson + sparse direct (`crd-hesap-direct`) should handle 10⁴; beyond that may need iterative + preconditioning.
- **Hidden-line removal algorithm** — Appel 1967 / Weiler-Atherton 1977 / Athan 1993; modern GPU-rasterizer approaches (depth-peeling + edge detection) may be simpler.
- **Drawing serialization** — DWG (proprietary AutoCAD) vs DXF (open) vs PDF/A vs SVG vs PNG. Default to DXF + PDF.

## Revisit triggers

This stub becomes a full phase plan when:
- The research dossier (`docs/research/cerid-cad-feature.md`) ships.
- `crd-brep` (Phase 3.1.8) v0 ships (this phase consumes B-rep heavily).
- A specific consumer (Solidworks-class CAD partner, design tool app) makes parametric modeling an active priority.
