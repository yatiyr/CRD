# Phase 3.1.10 — `crd-cfd`: computational fluid dynamics

**Status:** 📋 planned (ADR-0077 §3.1.10)
**ADR:** `docs/decisions/0077-multi-domain-expansion-vision.md`
**Slot:** after Phase 3.1.6 (`crd-hesap`) close.

## Why this exists

CFD is one of Cerid's stated eight domains. The data structures and solver patterns are sufficiently different from triangle-soup graphics meshes that they need their own substrate:

- **Unstructured grid topology** with face connectivity for flux computation — `crd-geometry-mesh` is triangle-soup BVH-indexed, optimized for ray/closest-point queries. CFD needs cell-to-face-to-neighbor traversal in inner solver loops.
- **Stiff solvers** with specific preconditioners (multigrid, block ILU) — eylem's iterative solvers are for contact dynamics, not flow.
- **Multiphase + turbulence** — requires their own algorithms, not borrowed from anywhere.

`crd-hesap` provides the linear algebra (sparse, iterative, direct, eig, ODE). CFD is the **discretization + physics-model layer** on top.

## Scope

### Grid framework

- **Unstructured grid topology** — cell-centered (FVM) and vertex-centered (FEM) variants.
- **Face connectivity** — every internal face owns its two adjacent cells; boundary faces own one + a boundary-condition tag.
- **Ghost cells** — for parallel domain decomposition (HPC integration via Phase 6 MPI).
- **Polyhedral cells** — modern CFD (OpenFOAM, Star-CCM+) supports arbitrary polyhedra, not just tets/hexes. Cost: more complex flux schemes; benefit: better mesh quality control.
- **AMR (Adaptive Mesh Refinement)** — octree-based (block-structured AMR) or anisotropic (per-cell refinement). Use case: shock capturing, boundary-layer resolution.

### Discretization schemes

- **FVM (Finite Volume Method)** — primary; flux-balance form, conservative, naturally handles shocks.
  - Convection schemes: upwind / central / TVD / MUSCL / WENO.
  - Diffusion schemes: central / over-relaxed correction.
  - Time integration: explicit (RK4) / implicit (Crank-Nicolson, BDF) / IMEX.
- **FEM (Finite Element Method)** — for problems where FVM struggles (incompressible flow, complex boundary conditions).
  - Galerkin / SUPG (streamline upwind Petrov-Galerkin) / DG (discontinuous Galerkin).
- **Spectral methods** — for high-accuracy on simple geometries (defer to v9+).

### Physics models

- **Compressible Navier-Stokes** — Euler equations + viscous terms; conservative variables (ρ / ρu / ρE).
- **Incompressible Navier-Stokes** — SIMPLE / PISO / fractional step / projection methods.
- **Heat transfer** — conduction (Laplacian), convection (advective), radiation (P1 / discrete ordinates / Monte Carlo).
- **Turbulence models** —
  - **RANS** (steady-state averaged): k-ε, k-ω, k-ω SST (industry default), Spalart-Allmaras, Reynolds-stress.
  - **LES** (large-eddy simulation): Smagorinsky / dynamic Smagorinsky / WALE / Vreman sub-grid models.
  - **DNS** (direct numerical simulation): no model, full resolution. v9+ only.
- **Multiphase** —
  - **VOF** (Volume of Fluid) — interface as a step function in a color field.
  - **Level-set** — interface as the zero level of a signed distance field (builds on `crd-sdf`).
  - **Eulerian-Lagrangian** — fluid Eulerian, droplets Lagrangian particles.
- **Combustion** — premixed (G-equation), non-premixed (mixture fraction + chemistry tables / Flamelet Generated Manifolds), detailed chemistry (CHEMKIN-style mechanism integration).
- **MHD** (magnetohydrodynamics) — coupled Navier-Stokes + Maxwell; for plasma simulation, fusion research. v9+ stretch.

### Solver framework

- **Steady-state solve** — pseudo-time stepping or Newton's method with implicit linear systems (consumes `crd-hesap-direct` / `crd-hesap-iterative`).
- **Transient solve** — explicit RK / implicit BDF / IMEX (consumes `crd-hesap-ode`).
- **Convergence monitoring** — residual norms, conservation checks, monitor variables.
- **Restart files** — checkpoint + resume long-running simulations.

## Dependencies

- `crd-hesap-sparse` (sparse matrix assembly)
- `crd-hesap-iterative` (CG / PCG / BiCGSTAB / GMRES for pressure-Poisson; AMG preconditioner for elliptic problems)
- `crd-hesap-direct` (sparse direct factor — multifrontal LU/Cholesky for stiff implicit solves)
- `crd-hesap-ode` (transient time integration)
- `crd-sdf` (level-set methods, immersed boundary)
- `crd-geometry-mesh` (surface meshes for boundary representation, BVH for ray-traced radiation)
- `crd-geometry-spatial` (octree / R-tree for AMR)

## Reference reading

- Versteeg & Malalasekera "An Introduction to Computational Fluid Dynamics: The Finite Volume Method" (2007) — best practical intro.
- Ferziger, Perić & Street "Computational Methods for Fluid Dynamics" (2020) — comprehensive textbook.
- Anderson "Computational Fluid Dynamics: The Basics with Applications" (1995) — compressible focus.
- Wilcox "Turbulence Modeling for CFD" (2006) — turbulence reference.
- Hirsch "Numerical Computation of Internal and External Flows" (2007) — 2-volume comprehensive.
- OpenFOAM architecture (open-source CFD reference).
- SU2 architecture (open-source CFD/multiphysics, NASA/Stanford).
- ANSYS Fluent / Star-CCM+ trade publications.

## Sub-modules (planned)

- `crd-cfd-grid` — unstructured grid topology + AMR.
- `crd-cfd-flux` — FVM flux schemes.
- `crd-cfd-fem` — FEM discretization (defer if v0–v3 are FVM-only).
- `crd-cfd-turbulence` — k-ε / k-ω / SST / LES sub-grid models.
- `crd-cfd-multiphase` — VOF / level-set / Eulerian-Lagrangian.
- `crd-cfd-combustion` — chemistry table integration, flame models.
- `crd-cfd-radiation` — P1 / DO / Monte Carlo radiation transfer.
- `crd-cfd-import` — CGNS / VTU / OpenFOAM case import.

## Out of scope

- Linear algebra primitives (`crd-hesap` covers).
- Mesh generation / quality improvement (`crd-geometry-mesh-processing` covers).
- 1D system simulation (Modelica-class, defer).
- Quantum / DSMC (rarefied gas) — defer.

## Open questions

- **Polyhedral vs simplex-only cells** — polyhedral is modern best-practice but more complex; tet/hex/prism/pyramid is the classical compromise. v0 likely simplex-only; polyhedral as v5+.
- **GPU acceleration** — CFD on GPU is challenging (irregular memory access, divergent control flow). NVIDIA Modulus, AMReX-Hydro show it's possible. Likely defer to v8+ after CPU substrate stabilizes.
- **Determinism contract** — CFD is harder than physics: nonlinear convergence, iterative tolerance. Bit-exact replay across SIMD widths is probably not feasible; per-config replay (same SIMD width + same OS) should be.
- **Visualization** — CFD output (vector fields, scalar fields, streamlines, isosurfaces) consumes `crd-sciviz` (Phase 3.1.16). The two phases land in tandem.

## Revisit triggers

This stub becomes a full phase plan when:
- The research dossier (`docs/research/cerid-cfd.md`) ships.
- `crd-hesap` close (sparse + iterative + direct + ODE all need to be available).
- A specific consumer (CFD partner, aerospace / automotive / HVAC customer) makes CFD an active priority.
- `crd-sciviz` (Phase 3.1.16) is at least scoped, since CFD output is the primary sciviz consumer.
