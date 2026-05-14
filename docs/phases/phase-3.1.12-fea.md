# Phase 3.1.12 — `crd-fea`: engineering FEA (static + modal + buckling + fatigue)

**Status:** 📋 planned (ADR-0077 §3.1.12)
**ADR:** `docs/decisions/0077-multi-domain-expansion-vision.md`
**Slot:** after Phase 3.1.6 (`crd-hesap`) close (likely parallel with `crd-cfd`).

## Why this is separate from eylem v7 FEM

| | eylem v7 (dynamic FEM) | `crd-fea` (engineering FEA) |
|---|---|---|
| Time domain | Real-time (60–240 Hz) | Offline (analysis runs minutes-hours) |
| Solve type | Explicit time-stepping (Verlet, RK4, IMEX) | Static (Newton-Raphson load steps) or modal (eigenvalue) |
| Element library | Reduced (tet linear / quad linear typical) | Full (linear + quadratic + cubic; shell; beam; gasket; rigid; etc.) |
| Material models | Hyperelastic (Neo-Hookean, Mooney-Rivlin) typical | Linear elastic → plasticity (J2 / Drucker-Prager / Cam-Clay) → hyperelasticity → viscoelasticity → damage |
| Correctness bar | Plausible-looking, real-time-stable | Mesh-independence proven, convergence shown |
| Validation | Game-style "good enough" | Industry codes (ASME BPVC, ASME B31, AISC 360) require traceable solver behavior |

These are **fundamentally different solvers** with different consumers. Eylem v7 ships first as part of physics; `crd-fea` ships as the engineering-analysis substrate.

## Scope

### Static structural analysis

- **Linear static** — `[K]{u} = {f}` with sparse direct solver (consumes `crd-hesap-direct`).
- **Nonlinear static** — Newton-Raphson load stepping with line-search; tangent stiffness recomputation; arc-length method for snap-through / snap-back.
- **Geometric nonlinearity** — large displacement, small strain (corotational) or large strain (total Lagrangian / updated Lagrangian).
- **Material nonlinearity** — plasticity (return mapping algorithms), hyperelasticity (Neo-Hookean, Mooney-Rivlin, Yeoh, Arruda-Boyce, Ogden), viscoelasticity (Maxwell / Kelvin-Voigt / generalized).
- **Contact analysis** — penalty / Lagrange / augmented-Lagrangian; node-to-surface and surface-to-surface; frictional contact (consumes `crd-geometry-bvh` for proximity).

### Modal analysis

- **Natural frequencies + mode shapes** — generalized eigenvalue problem `[K]{φ} = ω²[M]{φ}` (consumes `crd-hesap-eig` Lanczos / Arnoldi).
- **Frequency response** — `(K − ω²M){u(ω)} = f(ω)`; FRF computation.
- **Random vibration** — PSD response, Miles equation.
- **Component mode synthesis** (Craig-Bampton, Hurty) — substructuring for large assemblies.

### Buckling analysis

- **Linear buckling** — eigenvalue problem `[K] + λ[Kσ] = 0` where `Kσ` is the stress-stiffness matrix.
- **Nonlinear buckling** — Riks arc-length method for tracing post-buckling response.

### Fatigue analysis

- **High-cycle fatigue** — S-N curves, Goodman / Soderberg / Gerber mean-stress corrections; Miner's rule cumulative damage.
- **Low-cycle fatigue** — strain-life (Coffin-Manson), Neuber's rule.
- **Multiaxial fatigue** — critical plane, equivalent stress.
- **Crack propagation** — Paris law, NASGRO.

### Element library

- Solids: tet (linear, quadratic, 10-node), hex (linear 8-node, quadratic 20-node, 27-node), wedge (6/15-node), pyramid (5/13-node).
- Shells: Mindlin-Reissner (4/8-node quad, 3/6-node tri), MITC.
- Beams: Euler-Bernoulli, Timoshenko.
- Special: rigid, gasket, spring, damper, mass.

### Coupled multiphysics

- **Thermal-structural** — temperature → thermal strain → stress; sequential or fully coupled.
- **Thermal-electric** — Joule heating.
- **Acoustic-structural** — partitioned coupling with `crd-audio` modal acoustics (defer).

## Dependencies

- `crd-hesap-sparse` (sparse matrix assembly)
- `crd-hesap-direct` (sparse direct factorization for linear static — multifrontal LU/Cholesky/QR)
- `crd-hesap-iterative` (large problems beyond direct-solver memory)
- `crd-hesap-eig` (modal + buckling eigenvalue solvers — Lanczos / Arnoldi / LOBPCG)
- `crd-geometry-mesh` (FEA mesh import + topology)
- `crd-geometry-bvh` (contact proximity queries)
- `crd-brep` (CAD geometry → mesh tessellation pipeline) — Phase 3.1.8
- `crd-cad-feature` (GD&T tolerance → analysis loading) — Phase 3.1.9

## Reference reading

- Bathe "Finite Element Procedures" (2014) — comprehensive industry reference.
- Hughes "The Finite Element Method: Linear Static and Dynamic Finite Element Analysis" (2000).
- Zienkiewicz, Taylor & Zhu "The Finite Element Method" 3-volume set (2013).
- Belytschko, Liu, Moran & Elkhodary "Nonlinear Finite Elements for Continua and Structures" (2014).
- Crisfield "Non-linear Finite Element Analysis of Solids and Structures" (1991).
- Wriggers "Computational Contact Mechanics" (2006).
- Cook, Malkus, Plesha & Witt "Concepts and Applications of Finite Element Analysis" (2002).
- ANSYS Mechanical / Abaqus / Nastran theoretical manuals (industry-standard solver behaviors).

## Sub-modules (planned)

- `crd-fea-element` — element library (solids / shells / beams / special).
- `crd-fea-material` — material models (linear elastic → plasticity → hyperelastic → viscoelastic).
- `crd-fea-assemble` — global stiffness/mass assembly from elements.
- `crd-fea-solver-static` — linear + nonlinear static solvers.
- `crd-fea-solver-modal` — natural frequency + mode shape + FRF.
- `crd-fea-solver-buckling` — linear + nonlinear buckling.
- `crd-fea-contact` — penalty / Lagrange / augmented-Lagrangian contact.
- `crd-fea-fatigue` — S-N / strain-life / multiaxial / crack propagation.
- `crd-fea-import` — `.cdb` (ANSYS), `.inp` (Abaqus), `.bdf` (Nastran) input deck reader.

## Out of scope

- Linear algebra primitives (`crd-hesap`).
- Dynamic time-domain FEM (eylem v7).
- CFD (Phase 3.1.10).
- Mesh generation / quality (`crd-geometry-mesh-processing`).
- Topology optimization — defer (could be a `crd-topopt` future substrate).
- Acoustic-structural coupling — defer.

## Open questions

- **Determinism** — engineering FEA is validated against benchmarks; bit-exact replay is desirable for regression but the iterative tolerance bounds the reproducibility. Per-config replay (same OS + same SIMD) should be sufficient.
- **GPU acceleration** — sparse direct solvers have GPU variants (cuSPARSE / cuSOLVER); modal eig solvers can be GPU-accelerated. Defer to v8+.
- **Validation suite** — engineering FEA requires NAFEMS benchmark verification, Roark's Formulas comparisons, and standard textbook problems. The validation corpus is half the substrate.

## Revisit triggers

This stub becomes a full phase plan when:
- The research dossier (`docs/research/cerid-fea.md`) ships.
- `crd-hesap` close + `crd-geometry-mesh` close.
- A specific consumer (engineering / manufacturing / aerospace structural analysis customer) makes FEA an active priority.
- Eylem v7 dynamic FEM closes (the two solvers share material models and element libraries; we want to avoid duplication).
