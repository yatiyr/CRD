# Phase 3.1.16 — `crd-sciviz`: scientific visualization

**Status:** 📋 planned (ADR-0077 §3.1.16)
**ADR:** `docs/decisions/0077-multi-domain-expansion-vision.md`
**Slot:** after Phase 3.1.10 (`crd-cfd`) and Phase 3.1.12 (`crd-fea`) (the consumers).

## Why this exists

Engineering / scientific workflows live in **2D plots as much as 3D viewports**. A FEA stress analysis is judged by both the colored 3D stress map AND the convergence-vs-iteration plot AND the von Mises distribution histogram. A CFD analysis needs streamlines + slice planes + vector arrows + scalar field heatmaps. A control-system tuning session needs Bode plots, step responses, root-locus diagrams.

Bundling plotting into `crd-ui` (Phase 5) would be wrong:
1. sciviz quality is the FEA / CFD validation tool — it ships before the editor.
2. The ParaView / VTK / matplotlib-quality functionality is its own discipline.
3. The substrate consumers (CFD, FEA, control, robotics, medical viz) all need it.

## Scope

### 3D scalar / vector / tensor field visualization

- **Isosurface extraction** — Marching Cubes + Dual Contouring. Partially in `crd-geometry-mesh-processing` v7; sciviz adds quality knobs (smoothing, decimation per quality target).
- **Slice plane** rendering — interactive cutting planes through volumetric data with color-mapped scalar field on the slice.
- **Volume rendering** — direct ray-marching of volumetric data (CT scans, CFD fields). Transfer function authoring (opacity + color per scalar value).
- **Streamlines** — RK4 integration of vector fields; line + tube + ribbon visualizations.
- **Pathlines / streaklines** — time-varying flow visualization.
- **Vector field arrows** — sparse glyph placement.
- **LIC** (Line Integral Convolution) — dense vector field visualization on a 2D plane or surface.
- **Tensor visualization** — superquadric / ellipsoid glyphs for stress / diffusion tensors.

### 2D / 3D plotting

In-engine plotting (Phase 7 editor will wrap UI; substrate provides the rendering):

- **Line plots** — time series, x-y plots, multiple series, log scales.
- **Scatter plots** — 2D / 3D scatter with color / size mapping.
- **Histograms** — fixed-bin and Freedman-Diaconis adaptive binning.
- **Box plots** — quartile distributions.
- **Surface plots** — 3D surface from 2D scalar field.
- **Contour plots** — 2D isolines from scalar field.
- **Heatmaps** — 2D scalar field as colored grid.
- **Bode plots** — magnitude + phase vs frequency (control theory).
- **Root locus** — pole/zero plots in s-plane.
- **Polar plots** — angular data.

### Color mapping

- **Perceptually-uniform color maps**: viridis, plasma, magma, cividis (the modern matplotlib default set).
- **Diverging maps**: RdBu, BrBG, PiYG (for signed data with neutral center).
- **Sequential maps**: blues, greens, ... (traditional, deprecated in favor of viridis-class).
- **Categorical maps**: tab10, Set1 (for discrete categories).
- Custom color map authoring + `.cube` LUT import.

### Annotation + measurement

- **Distance** measurement between two 3D points / two surfaces.
- **Angle** measurement between two edges / surfaces.
- **Area** measurement on a face / projected area.
- **Volume** measurement of a closed B-rep solid (consumes `crd-brep`).
- **Cross-section** properties (centroid, moment of inertia) — useful for engineering review.
- **Text annotation** anchored to 3D space (consumes `crd-font`).
- **Dimension lines** with tolerances.

### Comparison views

- **Side-by-side** rendering of two simulation results.
- **Difference field** computation (A − B with color mapping).
- **Overlay** with adjustable opacity.
- **Time-step scrubbing** for transient simulations.

## Dependencies

- `crd-geometry-primitives` (basic shapes, plane math).
- `crd-geometry-mesh` (surface meshes for slice cuts, isosurfaces).
- `crd-geometry-mesh-processing` (isosurface extraction via Marching Cubes).
- `crd-hesap-stats` (histogram binning, statistical summaries).
- `crd-renderer` (the rendering substrate — sciviz is mostly rendering ops on data).
- `crd-font` (Phase 3.3) — for plot axis labels, annotations.
- `crd-cfd` (Phase 3.1.10) — first major consumer.
- `crd-fea` (Phase 3.1.12) — second major consumer.

## Sub-modules (planned)

- `crd-sciviz-field` — scalar / vector / tensor field visualization (isosurface / slice / streamline / volume).
- `crd-sciviz-plot` — 2D / 3D plotting (line / scatter / histogram / surface / contour).
- `crd-sciviz-colormap` — color maps + transfer function authoring.
- `crd-sciviz-measure` — annotation + measurement tools.

## Reference reading

- Schroeder, Martin & Lorensen "The Visualization Toolkit: An Object-Oriented Approach to 3D Graphics" (2006) — VTK reference.
- ParaView architecture (open-source sciviz, built on VTK).
- Matplotlib design (the de-facto Python plotting reference).
- "Visualization Analysis and Design" (Munzner 2014).
- "Information Visualization: Perception for Design" (Ware 2020).
- Crameri, Shephard & Heron "The misuse of colour in science communication" (2020) — perceptual color map argument.
- Cabral & Leedom "Imaging Vector Fields Using Line Integral Convolution" (1993) — LIC paper.

## Out of scope

- 2D UI framework (Phase 5 `crd-ui`).
- General-purpose 3D scene graph (Phase 3.0 `crd-scene`).
- Image processing (`crd-image-processing` doesn't exist; not in scope of the multi-domain expansion).
- DICOM medical image import — Phase 8 medical viz integration.

## Open questions

- **Plot library integration** — should sciviz wrap an existing plotting library (matplotlib via C++ bindings? plplot?) or write from scratch? Probably from scratch for engine consistency (no external dependency for runtime sciviz).
- **GPU vs CPU rendering** — high-quality streamlines (millions of seeds, ribbon geometry) is a GPU win. Plot rendering can be CPU-side rasterized into a TextureResource. Mixed strategy likely.
- **File format** — does sciviz output exportable images / videos / interactive HTML? Default to images (PNG / SVG); export-to-video and export-to-HTML as later slices.

## Revisit triggers

This stub becomes a full phase plan when:
- The research dossier (`docs/research/cerid-sciviz.md`) ships.
- `crd-cfd` or `crd-fea` is approaching production use (consumers).
- A specific consumer (engineering simulation user, scientific computing workflow) needs sciviz at substrate level.
