# Cerid — Live Context

> Short-term memory: "where are we now?" The master plan lives in `docs/ROADMAP.md`; the doc map in `docs/README.md`.
> This is a **DASHBOARD, not a changelog.** Each milestone's detail lives in its session log (`docs/sessions/YYYY-MM-DD-*.md`); this file summarises the *current* state and points there. Keep it lean (≤ 300 lines) — prune stale snapshots, don't stack them.

---

## Current focus — Phase 3.1.6 `crd-hesap` (numerical stack)

> **🎯 STANDING MANDATE (2026-05-31): don't stop until `crd-hesap` HONESTLY + COMPLETELY crushes EVERY gold-standard solver for the simulation targets (cloth/deformation/CFD/Navier-Stokes/FEA) — Eigen + CHOLMOD + UMFPACK + PARDISO/MUMPS/SuperLU_DIST.** HONEST = fair same-class peer at its best, matched accuracy, no parallel-vs-serial asterisks. The cross-thread bit-determinism **moat** is the differentiator (beat speed AND keep determinism). Full directive: memory `feedback_full_victory_beat_all_gold_standards`.

**Committed state: v6 done (`4fd0b84` "v6 finally finished!").** v5 sparse-direct + v6 sparse-eigenvalue are shipped and committed.

**▶ Active: v7 — the optimisation domain (WIP, uncommitted).** New module `crd-hesap-opt` (ADR-0090; unconstrained + constrained merged per user direction). Done through **v7-e-2-bench**: substrate + line searches + L-BFGS/BFGS/SR1 + faithful L-BFGS-B + nonlinear-LS/Levenberg-Marquardt (dense + sparse) + the CHOLMOD head-to-head crush vehicle. L-BFGS matches liblbfgs/scipy on eval-count; sparse-LM carries the `{1..16}` moat through the supernodal factor. **NEXT = the next v7 subslice** (first-order v7-f / Newton v7-g / trust-region v7-h, or the constrained spine v7-j → v7-k QP → v7-n NLP). Plan + per-slice detail: `docs/phases/phase-3.1.6-hesap.md`; memory `project_v7_optimization_plan`, `project_v7e2_lattice_cholmod_perf`.

**Also WIP this session (uncommitted):** engine-core sanity hardening — root-caused + fixed the `TlsfAllocator::init_pool` end-sentinel bug (the multifrontal-LU flaky AV; memory `project_mf_lu_frontparallel_flaky_uaf`) + added a bite-verified boundary regression test; wrote the **sanity doctrine** (`docs/SANITY.md`) + the **doc map** (`docs/README.md`) and consolidated the doc system.

**Open follow-ons (post-v5/v6, not blocking — track in `docs/debt.md`):** CI 18-config sweep on the WIP; `docs/systems/hesap-direct.md` system doc; ADR-0065 §27 D(direct) lock; all-families `{1..16}` moat audit; raefsky3 DELAYED-PIVOT frontier (the one matrix within-front pivoting couldn't take to full f64).

---

## Coming up next (Phase 3.1 eylem — ⏸ PAUSED at v1b)

Phase 3.1 eylem is paused at v1b close per the ADR-0076 §12 sequencing pivot (2026-05-11): `crd-geometry` ships FULL before eylem v1c resumes, so eylem v1c+ consumes geometry from day 1 (no deferred-refactor debt). Resume order after the hesap cluster: v1c broadphase → v1c-sensor → v1d narrowphase + filter + callback + mesh + hf → v1e SI solver + material → v1f joints + articulation-filter + fields → v1g islands + contactmodify → v1h scene queries → v1i character controller → v1j snapshot/replay → v1k sandbox → v1l close. Full slice plan in `docs/phases/phase-3.1-eylem.md`.

---

## Active detour

_none — D-001 closed 2026-05-07; D-002 (concurrent containers) 2026-05-12; D-003 (crd-perf) + D-006 (crd-time) 2026-05-15._

> When a detour opens, this section names it and the main roadmap pauses until it closes. Detour files: `docs/detours/D-NNN-<slug>.md`. Queue rules: `docs/detours/README.md`.

---

## Last shipped milestone

**2026-06-07 — v6 (sparse eigenvalue) CLOSED + committed (`4fd0b84`).** New module `crd-hesap-eigen`: matrix-free Lanczos · thick-restart Lanczos (≡IRLM) · Arnoldi/Krylov-Schur (≡IRAM) · shift-invert · LOBPCG (+generalized +preconditioned) · Jacobi-Davidson · FEAST · IRLBA sparse SVD; CLI `hesap.eigen.*`; the `{1..16}` determinism moat on every method. ADR-0089 + `docs/systems/hesap-eigen.md`. Honest verdict: AMG-LOBPCG crushes direct ARPACK 9.5× wall + 40× mem, parity vs PRIMME; sparse SVD competitive at matched accuracy + moat.

**2026-06-05 — v5 (sparse direct) CLOSED + committed (`f0ae6db`).** The full family: v5a Cholesky · v5b LU · v5c QR · v5d LDLᵀ · v5e HSS/BLR · v5f mixed-precision IR + GMRES-IR + within-front partial pivoting — all with the cross-thread determinism moat. SPD Cholesky beats CHOLMOD (hood/ldoor) via a profiled serial-symbolic fix; LU beats MUMPS on af23560; honest losses recorded where MUMPS/UMFPACK win. Memory `project_v5f_mixed_precision`, `project_symbolic_is_the_cholmod_gap`.

For the full hesap story (v0 BLAS → v1/v2 sparse substrate + reorderings → v3 dense eig/SVD → v4 iterative + AMG → v5 sparse-direct → v6 eigen → v7 opt), see `docs/phases/phase-3.1.6-hesap.md` and the session logs.

### Recent slice history (one line per cluster — full detail in the linked session log / phase doc)

- **v7 (optimisation, `crd-hesap-opt`) — WIP** — a→e-2-bench shipped (L-BFGS/L-BFGS-B/LM + sparse-LM + CHOLMOD bench); ADR-0090; uncommitted. `docs/phases/phase-3.1.6-hesap.md`.
- **v6 (sparse eigenvalue, `crd-hesap-eigen`) ✅ committed `4fd0b84`** — Lanczos/Arnoldi/LOBPCG/JD/FEAST/IRLBA + CLI + moat; ADR-0089.
- **v5 (sparse direct) ✅ committed `f0ae6db`** — Cholesky/LU/QR/LDLᵀ/HSS-BLR/mixed-IR family + moat.
- **v4 (iterative solvers + preconditioners + AMG) ✅** — `docs/sessions/2026-05-27-hesap-v4z-close.md`; ADR-0065 §26.
- **v3 (dense eig + SVD + least-squares) ✅** — symmetric + non-symmetric (balance/Hessenberg/Francis/AED) + SVD + lstsq/pinv/NNLS/TLS; ADR-0065 §24.
- **v0 (dense BLAS L1/L2/L3 + microkernels) ✅** — ADR-0082 (intrinsics-via-Vec8f microkernel; asm deferred).
- **2026-05-19 — Phase 3.1.7 `crd-geometry` substrate CLOSED** — all 11 sub-modules (primitives→bvh→convex→mesh→spatial→polygon→mesh-processing→delaunay→gpu-lbvh→decomposition→shader-helpers); ADR-0076. `docs/sessions/2026-05-19-geometry-v9-close.md`.
- **2026-05-15 — Phase 3.1.7.5 `crd-units` CLOSED** — two-layer typed architecture; ADR-0078; `crd-no-untagged-physical-numeric` guard.
- **2026-05-10 — Phase 3.0 scene/ECS CLOSED** — 8-layer slot ECS; ADRs 0049–0061.

For complete chronology see `docs/sessions/`; for the ADR index `docs/decisions/README.md`; for the active phase plan `docs/phases/phase-3.1.6-hesap.md`.

> Older milestones intentionally truncated — they live in session logs.
