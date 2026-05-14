# Phase 3.1.8 — `crd-brep`: NURBS / B-rep core (the CAD/manufacturing substrate)

**Status:** 📋 planned (ADR-0077 §3.1.8)
**ADR:** `docs/decisions/0077-multi-domain-expansion-vision.md` (overarching)
**Slot:** after Phase 3.1.7 (`crd-geometry`) close.

## Why this exists

Triangle meshes are a viewing / export format; real CAD users (Solidworks, Onshape, Fusion 360, Catia, NX, Inventor customers) work with **exact parametric geometry**: NURBS surfaces, Bézier patches, and B-rep solid topology with explicit face / edge / vertex relations plus geometric tolerances.

Without a B-rep substrate, Cerid cannot serve manufacturing / CAD / aerospace / mechanics customers in the way they need. STEP / IGES / Parasolid files are the industry interchange and they encode B-rep, not meshes. Fillets, chamfers, exact booleans, parametric features, GD&T, manufacturing drawings — all of these require B-rep.

This is the **CAD-substrate** decision-point. ADR-0077 §3.1.8 explains the strategic context.

## What ships here vs elsewhere

| Capability | This phase (`crd-brep`) | Elsewhere |
|---|---|---|
| Parametric surface evaluation | ✅ NURBS, Bézier patches | — |
| B-rep solid topology | ✅ face/edge/vertex with tolerances | — |
| Exact boolean operations (parametric) | ✅ | (Mesh booleans are `crd-geometry-polygon` v6 — approximate) |
| Fillet / chamfer / sweep / loft | ✅ | — |
| STEP / IGES / Parasolid import-export | ✅ | — |
| Tessellation B-rep → mesh | ✅ (outputs to `crd-geometry-mesh`) | — |
| Parametric features (feature tree) | — | `crd-cad-feature` (Phase 3.1.9) |
| 2D sketching + constraints | — | `crd-cad-feature` |
| Drafting / dimensioning / GD&T | — | `crd-cad-feature` |
| Toolpath generation (CAM) | — | `crd-cam` (Phase 3.1.13) |
| Static structural FEA on B-rep | — | `crd-fea` (Phase 3.1.12, with tessellation step) |

## Dependencies

- `crd-geometry-primitives` (`Vec3` / `Plane` / closest-point predicates)
- `crd-geometry-mesh` (tessellation output target)
- `crd-hesap-iterative` (Newton-Raphson for surface-surface intersection)
- `crd-math` (vector / matrix / SIMD)
- `crd-containers` (Array / ConstSpan)
- `crd-memory` (`IAllocator*`)

## Sub-modules (planned — pending research dossier)

The exact split lands when the research dossier ships. Likely:

- `crd-brep-core` — surface evaluation (NURBS basis functions, Bézier de Casteljau), B-rep topology types (Face / Edge / Vertex / Loop / Shell / Solid), tolerance model.
- `crd-brep-intersect` — surface-surface intersection (SSI), curve-surface intersection (CSI), curve-curve intersection (CCI). Newton-Raphson + subdivision hybrid.
- `crd-brep-bool` — exact boolean operations (union / difference / intersection) on B-rep solids.
- `crd-brep-features` — fillet / chamfer / sweep / loft / draft-angle / shell.
- `crd-brep-import` — STEP / IGES / Parasolid format readers/writers.
- `crd-brep-tessellation` — B-rep → `crd-geometry-mesh::TriangleMesh` for visualization.

## Slice list — locked at research-dossier time

Placeholder. The full slice list lands when the research dossier closes (estimated 6–9 months of work end-to-end).

Indicative slices (NOT committed):

- **v0** core types (NURBS surface evaluation, Bézier de Casteljau, B-rep Face/Edge/Vertex/Loop/Shell/Solid types, tolerance model).
- **v1** parametric surface ops (point projection, closest point, normal evaluation, curvature).
- **v2** intersection (SSI/CSI/CCI with Newton-Raphson + subdivision).
- **v3** exact boolean ops on B-rep solids.
- **v4** fillet + chamfer + draft.
- **v5** sweep + loft + shell.
- **v6** STEP import-export.
- **v7** IGES import-export.
- **v8** Parasolid import (read-only — write requires licensing).
- **v9** tessellation B-rep → `crd-geometry-mesh::TriangleMesh` with quality control (chord deviation, edge length, angle tolerance).
- **v10** GPU evaluation (compute-shader NURBS for real-time CAD viewport rendering).
- **v-close** validation corpus + performance bench (vs OpenCASCADE on standard CAD test parts) + full N-config sweep.

## Substrate decisions (TBD — locked at research-dossier time)

To pin at v0:
- Tolerance model — explicit per-edge, per-vertex, per-face? Or single global model tolerance?
- Knot vector representation — `crd::containers::Array<T>` of f64 — does `crd-math` need a knot-vector helper?
- Trim-curve representation — 2D parametric curves on a 3D surface? Or 3D curves with parameter-space cache?
- Determinism contract — does B-rep need bit-exact reproducibility (ADR-0063 pattern)? Probably yes for replay-friendly CAD diffing.
- Threading model — surface evaluation is embarrassingly parallel (`crd-jobs::parallel_for` over patches); boolean operations are inherently sequential per pair.

## Reference reading

- Piegl & Tiller "The NURBS Book" (1997) — the canonical NURBS reference.
- Mäntylä "An Introduction to Solid Modeling" (1988) — B-rep topology foundations.
- Hoffmann "Geometric and Solid Modeling" (1989).
- Mortenson "Geometric Modeling" (2006).
- Parasolid kernel design papers (Siemens publications, partial).
- OpenCASCADE source + documentation (open-source CAD kernel, reference architecture).
- ACIS kernel (Spatial Corp; design discussions in CAD trade journals).
- Patrikalakis & Maekawa "Shape Interrogation for Computer Aided Design and Manufacturing" (2002) — SSI/CSI algorithms.

## Out of scope (for this phase)

- Parametric feature trees (Phase 3.1.9).
- 2D sketching with constraints (Phase 3.1.9 — constraint solver).
- Drafting / dimensioning / GD&T (Phase 3.1.9).
- CAM toolpath generation (Phase 3.1.13).
- Reverse engineering (point cloud → NURBS) — defer.
- Mesh-to-CAD reconstruction — defer.
- Subdivision surfaces (Catmull-Clark) — `crd-geometry-mesh-processing` v7 (Loop subdivision is already there for triangle meshes; if subdivision surface SURFACE evaluation is needed, it lands here).

## Open questions

- **Licensing for Parasolid write** — Parasolid is owned by Siemens; write access requires a commercial license. Read-only via the published format may be sufficient for round-tripping; revisit when a CAD partner appears.
- **STEP AP242 vs AP203 vs AP214** — STEP has multiple application protocols. AP242 is the modern integrated protocol (covers GD&T); AP214 is automotive; AP203 is the original mechanical. Default to AP242 with backward read.
- **GPU evaluation** — NURBS basis functions are well-suited to compute shaders (de Boor / de Casteljau algorithms parallelize cleanly). Whether GPU eval is part of v0 or a later slice depends on viewport performance need.

## Revisit triggers

This stub becomes a full phase plan when:
- The research dossier (`docs/research/cerid-brep.md`) ships.
- A specific consumer (CAD partner, manufacturing customer, etc.) makes B-rep an active priority.
- Phase 3.1.7 `-mesh` v4 closes (B-rep tessellates *into* `crd-geometry-mesh::TriangleMesh`, so the target format needs to exist first).
