# Phase 3.1.13 — `crd-cam`: manufacturing — toolpath generation

**Status:** 📋 planned (ADR-0077 §3.1.13)
**ADR:** `docs/decisions/0077-multi-domain-expansion-vision.md`
**Slot:** after Phase 3.1.8 (`crd-brep`) and Phase 3.1.9 (`crd-cad-feature`) close.

## Why this exists

CAM (Computer-Aided Manufacturing) converts a CAD model + manufacturing intent into machine-executable instructions. It's a distinct workflow from CAD (manufacturing engineer vs design engineer) with its own algorithms:

- **Toolpath generation** — offset curves, medial axis, voronoi-based finishing, scallop-height calculation.
- **Post-processing** — converting generic toolpaths to vendor-specific G-code dialects.
- **Material removal simulation** — boolean-difference per tool position over time (high-frequency B-rep ops).

The CAM substrate completes the CAD → manufacturing pipeline.

## Scope

### Milling (subtractive — primary scope)

- **3-axis milling**: pocketing (parallel offsets, zigzag, spiral), contouring, parallel finishing, scallop finishing, pencil tracing, rest machining (high-residual after roughing).
- **5-axis milling**: indexed (4+1, 3+2), continuous 5-axis (sinusoidal/Sinumerik patterns), swarf machining (cut with side of cutter), 5-axis roughing.
- **Tool library**: end mill (flat / ball / bull-nose / chamfer / V-mill), drill, tap, reamer, lollipop (T-slot), face mill.
- **Workholding**: vises, chucks, fixtures (collision avoidance during toolpath).

### Turning (lathe)

- Roughing, finishing, threading, grooving, cut-off, drilling on rotational axis.
- Live tooling (mill operations on a lathe — turn-mill machines).

### Additive manufacturing (slicing)

- **FDM** (fused deposition modeling) — layer-by-layer; infill patterns (rectilinear / grid / honeycomb / gyroid); support generation; raft / brim / skirt.
- **SLA** (stereolithography) — supports with tree-structure optimization; orientation for minimal supports.
- **SLS** (selective laser sintering) — no supports needed (powder bed).
- **DMLS / SLM** (direct metal laser sintering / selective laser melting) — supports for thermal management.
- **Bound metal deposition** (Markforged Metal X style).

### Post-processing

- **G-code output** with configurable post-processors per machine controller:
  - Fanuc (industry-standard).
  - Siemens Sinumerik.
  - Heidenhain TNC.
  - Mazak (proprietary).
  - LinuxCNC (open source).
  - Generic ISO 6983 / RS-274.
- Post-processor language: declarative configuration (mapping toolpath ops → G-code), not embedded scripting.

### Material removal simulation

- **High-frequency B-rep boolean** — subtract tool envelope from stock at each toolpath step.
- **Z-map / voxel** — faster than B-rep boolean, less accurate; the trade-off is visualization quality vs simulation correctness.
- **Collision detection** — between tool / holder / fixture / stock at each step.
- **Optimization** — flag toolpath segments with bad cutter engagement angle, excessive feed-rate per chip-load.

### Sheet metal

- **Flat patterns** — unfold a 3D sheet-metal part to its flat blank.
- **Bend allowance** — material-specific bend deduction tables.
- **K-factor** computation.
- **Punch / laser / waterjet** nesting (2D part packing).

### PCB

- **Stack-up** — layer geometry, dielectric materials, copper thickness.
- **Routing constraints** — trace width, via spacing, impedance control.
- **Gerber output** for fabrication.
- **Pick-and-place** files for assembly.

## Dependencies

- `crd-brep` (Phase 3.1.8) — exact geometry, tool-stock boolean ops.
- `crd-cad-feature` (Phase 3.1.9) — feature info (drilled holes, threaded features, pocket features for feature-recognition CAM workflows).
- `crd-geometry-mesh` — stock-as-mesh, fixture-as-mesh.
- `crd-geometry-decomposition` (V-HACD) — for collision-detection convex decomposition of fixtures.
- `crd-geometry-bvh` — proximity queries for collision detection.
- `crd-hesap-stats` — nesting / packing optimization.

## Sub-modules (planned)

- `crd-cam-milling-3axis`
- `crd-cam-milling-5axis`
- `crd-cam-turning`
- `crd-cam-additive`
- `crd-cam-postprocess`
- `crd-cam-sim` (material removal simulation)
- `crd-cam-sheetmetal`
- `crd-cam-pcb`

## Reference reading

- Smid "CNC Programming Handbook" (2008) — practical reference.
- Suh, Kang, Chung & Stroud "Theory and Design of CNC Systems" (2008).
- Hood-Daniel & Kelly "Build Your Own CNC Machine" (for understanding the controller side).
- FreeCAD Path workbench architecture (open-source CAM reference).
- OpenSCAM / CAMotics architecture (open-source machine simulator).
- Wright & Bourne "Manufacturing Intelligence" (1988) — feature-recognition CAM history.
- ASME B5.59 — machine tool standards.
- ISO 6983 / RS-274 — G-code standard (loosely followed by manufacturers).

## Out of scope

- CAD geometry itself (Phase 3.1.8 / 3.1.9).
- Manufacturing simulation beyond toolpath (production scheduling, ERP integration).
- Robotics-specific CAM (`crd-control` Phase 3.1.11 handles trajectory; CAM-for-robot would be a Phase 8 integration).
- Generative design — defer (could overlap with `crd-fea` topology optimization).

## Open questions

- **Vendor licensing** — high-end CAM (Mastercam, NX CAM, PowerMill) has proprietary toolpath algorithms protected by IP. Defaulting to published-algorithm subset (Mastercam-90s-era + open-source FreeCAD Path) avoids any IP exposure.
- **Verification standard** — CAM output must match what the machine actually does. NIST has standard test parts (AMRC, NCDMM). Validation corpus is half the substrate.
- **Cloud CAM** — modern trend (Onshape CAM, Fusion 360 CAM) runs toolpath generation in the cloud. Cerid's substrate-first approach naturally supports this — toolpath generation is a pure function of geometry + tool data.

## Revisit triggers

This stub becomes a full phase plan when:
- The research dossier (`docs/research/cerid-cam.md`) ships.
- `crd-brep` close.
- A specific consumer (machine shop, manufacturing partner, 3D printing service) makes CAM an active priority.
