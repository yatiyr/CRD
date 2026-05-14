# Phase 3.1.15 — `crd-procgen`: procedural content generation

**Status:** 📋 planned (ADR-0077 §3.1.15)
**ADR:** `docs/decisions/0077-multi-domain-expansion-vision.md`
**Slot:** after Phase 3.1.7 v7 (`crd-geometry-mesh-processing`) close.

## Why this exists

Procedural generation is cross-cutting:
- **Games** — terrain generation, cave systems, dungeon layouts, infinite worlds.
- **Cinematic** — environment population, vegetation, urban scenes.
- **CAD test data** — synthetic geometry for validation, stress-testing, demos.
- **Scientific visualization** — synthetic field data for algorithm development.

The substrate belongs at the math/geometry tier, not bolted onto editor tools.

## Scope

### Noise primitives

The `crd-geometry-primitives::formulary.hpp` (v0e) already has Perlin-class smooth-min / smooth-max combinators. This phase extends noise:

- **Perlin noise** (1D / 2D / 3D / 4D) — gradient-based, classic.
- **Simplex noise** — Perlin's improvement, less directional bias.
- **Worley noise** (cellular noise) — distance-to-Nth-nearest-point; for veins, scales, cracks.
- **Voronoi noise** — full Voronoi cell info (centroid + cell index).
- **Fractal noise** (fBm — fractional Brownian motion) — multi-octave sum.
- **Ridged multifractal** — for terrain.
- **Curl noise** — divergence-free; for fluid-like motion.

### Wave Function Collapse (WFC)

- **Tile-based WFC** (overlapping or simple tiled) — input pattern → output tile placement.
- **Constraint propagation** — efficient backtracking on contradiction.
- **Use cases**: dungeon generation, texture synthesis, level layout.

### L-systems

- **Deterministic L-system** — string rewriting rules.
- **Stochastic L-system** — probabilistic rule selection.
- **Parametric L-system** — rules with parameters.
- **Context-sensitive L-system** — rules depending on neighboring symbols.
- **Use cases**: plants, vegetation, fractals, abstract structures.

### Terrain generation

- **Heightmap synthesis** — multi-octave fBm + ridged multifractal + warping.
- **Hydraulic erosion simulation** — particle-based water transport (Mei et al. 2007); produces realistic gullies, rivers, deposits.
- **Thermal erosion** — material slumping below repose angle.
- **River network simulation** — Voronoi-based watersheds, flow accumulation, drainage networks.
- **Geological layering** — strata, faults, intrusions.

### City / building generation

- **City layout** — Voronoi blocks, road graphs, zoning.
- **Procedural buildings** — split-grammar (Müller et al. 2006), shape grammars.
- **Interior generation** — room placement, door positioning, furniture distribution.

### Procedural materials (Substance-style)

- **Node graph** — sources (noise / pattern / image) → filters (blur / warp / blend / quantize) → output (albedo / normal / roughness / metallic / AO maps).
- **Tile-able** — output textures wrap seamlessly.
- **Parametric** — exposed parameters drive material variation.
- **Cooked output** — bake to `TextureResource` at material-build time.

## Dependencies

- `crd-geometry-primitives` (formulary, noise base — Phase 3.1.7 v0e ✅).
- `crd-geometry-mesh-processing` (subdivision / remesh / hole-fill — Phase 3.1.7 v7).
- `crd-hesap-stats` (random distributions, PCG / Xoshiro256\*\* RNGs).
- `crd-hesap-fft` (frequency-domain noise synthesis).
- `crd-renderer` (procedural material cooking, texture baking).

## Sub-modules (planned)

- `crd-procgen-noise` — Perlin / simplex / Worley / Voronoi / fBm / ridged / curl.
- `crd-procgen-wfc` — Wave Function Collapse.
- `crd-procgen-lsys` — L-systems.
- `crd-procgen-terrain` — heightmap synth + erosion + rivers.
- `crd-procgen-urban` — city / building / interior.
- `crd-procgen-material` — Substance-style procedural materials.

## Reference reading

- Ebert, Musgrave, Peachey, Perlin & Worley "Texturing & Modeling: A Procedural Approach" (2003) — comprehensive reference.
- Perlin "An Image Synthesizer" (1985) — original Perlin noise.
- Worley "A cellular texture basis function" (1996) — Worley noise.
- Gumin "WaveFunctionCollapse" (https://github.com/mxgmn/WaveFunctionCollapse) — WFC reference implementation.
- Prusinkiewicz & Lindenmayer "The Algorithmic Beauty of Plants" (1990) — L-systems.
- Müller et al. "Procedural Modeling of Buildings" (SIGGRAPH 2006).
- Mei et al. "Fast Hydraulic Erosion Simulation and Visualization on GPU" (2007).
- "Substance Designer" documentation (Adobe; node-based procedural material reference).

## Out of scope

- **AI-driven content generation** (text-to-image, text-to-mesh) — Phase 3.1.14 `crd-ml-inference` handles the inference; the procedural pipeline integration is here.
- **Animation procedural** (gait synthesis, crowd behavior) — Phase 3.2 animation.
- **Speech synthesis** — Phase 3.4 audio.

## Open questions

- **Determinism** — procgen is often used in user-content pipelines where same-seed-same-output is critical. Match `crd-hesap-stats` PCG / Xoshiro256\*\* contract (ADR-0063 determinism for splittable random).
- **GPU evaluation** — many procgen kernels (noise, terrain erosion, material graphs) are GPU-friendly. v0 CPU-only; GPU mirror v8+.
- **Editor integration** — procedural workflows need node graph authoring. Defer the UI to Phase 7 editor; the substrate exposes the graph evaluator.

## Revisit triggers

This stub becomes a full phase plan when:
- The research dossier (`docs/research/cerid-procgen.md`) ships.
- `crd-geometry-mesh-processing` (Phase 3.1.7 v7) closes.
- A specific consumer (terrain-heavy game, procedural CAD, infinite-world demo) needs procgen at substrate level.
