# crd-hesap-motion — trajectory generation (the mission-critical motion primitives)

> Phase 3.1.6 v13 (n–q). ADR-0095. Plan: `docs/phases/phase-3.1.6-v13.md`.
> Status: **SHIPPED (2026-07-01)** — linux-gcc-green + Windows 4-config; motion suite 37289 assertions / 11 cases.
> Session: `2026-07-01-v13-motion-ruckig-otg.md`, `2026-07-02-v13z-windows-close.md`.
> SQUAD + quaternion cubic B-spline · clothoid + NURBS · minimum-jerk/snap QP · S-curve/trapezoidal + TCB · **the full
> arbitrary-state Ruckig-class OTG** (single-DoF + multi-DoF time-sync) · CLI `hesap.motion.*`.

## What it is

The motion *primitives* that planning/control (Phase 3.1.11, one-way consumer) sits on: attitude interpolation,
continuous-curvature paths, optimal polynomial trajectories, and jerk-limited online trajectory generation for
drones/robots/self-driving/games. Every generator honors the v13 certification moat (ADR-0095): **deterministic by
construction** (`crd::math`, fixed FP order), **allocation-free** (all stack arrays), **WCET-bounded** (a finite
candidate set + fixed-iteration Newton — no unbounded loops, no `malloc`, no exceptions). That is the DO-178C /
ISO-26262-ASIL-D property GSL (mallocs) / Boost (throws) / Reflexxes-class incumbents lack — preserved *while beating
Ruckig's throughput*.

## Shipped surface

- **v13-n — attitude trajectories.** **★SQUAD** (C² spherical cubic via nested SLERP) · **★quaternion cubic B-spline**
  (Kim-Kim-Shin cumulative basis, C² manifold-correct). Added `quat_log`/`quat_exp` to **crd-math** (SANITY-8 — the
  capability's home module; unit-norm preserved to 1e-16).
- **v13-o — continuous-curvature paths.** **★clothoid / Euler spiral** (curvature linear in arc length, closed-form via
  `crd-hesap-special` Fresnel) · **★NURBS** (Cox-de Boor rational B-splines — exact unit circle to 1e-12).
- **v13-p — optimal polynomial trajectories.** minimum-jerk quintic + minimum-snap septic BVP + **★minimum-snap
  MULTI-SEGMENT** (`min_snap_trajectory` — the Mellinger `pᵀQp` QP: minimize ∫snap² s.t. waypoints + C³ + BCs, via the
  equality-constrained KKT linear system over **crd-hesap-dense** LU).
- **v13-q — jerk-limited real-time motion (the headline).** S-curve / trapezoidal profiles (`plan_scurve`) ·
  Kochanek-Bartels TCB · **★★rest-to-rest multi-DoF time-sync** (`plan_synchronized`, matches `ruckig.duration`) ·
  **★★★the FULL arbitrary-state Ruckig-class OTG** — `otg.hpp` `plan_otg` (single-DoF min-time from any state: the
  7-phase profile + monic-quartic solver + ≤2-phase brake) and `otg_sync.hpp` `plan_otg_timed` /
  `plan_synchronized_otg` (reach-in-exactly-tf + multi-axis synchronization). A faithful reimplementation of Ruckig's
  third-order position solver (Berscheid & Lien 2021, MIT).

## Crush — beats Ruckig's own C++

Benchmarked vs `libruckig.a` (Release, `-O3 -march=native`), reconstruct-verified in Python first (1934/1934 step1 +
2474/2474 step2 vs the `ruckig` package), then ported and gated on baked references:

| capability | correctness vs Ruckig C++ | speed vs Ruckig C++ |
|---|---|---|
| single-DoF arbitrary-state (`plan_otg`) | **0/2000** duration-mismatch (bit-exact) | **1.94×** (188.6 vs 365.8 ns) |
| multi-DoF arbitrary-state sync (`plan_synchronized_otg`) | **0/2000** tsync-mismatch (bit-exact) + 0 reach-fail | **1.26×** (1615.7 vs 2041.0 ns) |

Levers: velocity-limited candidates checked first (optimal when valid ⇒ long moves skip the quartic solves);
closed-form quartic (no bracketing) + Newton-shrink + Ruckig's `up_first` direction heuristic. Bit-for-bit AND faster,
plus the determinism + WCET + allocation-free moat Ruckig lacks.

## Determinism

`crd::math` + fixed evaluation order + stack-only storage ⇒ bit-identical across compilers/opt-levels/threads; the
generators are `noexcept` and status/`valid`-flagged (the `crd-hesap-v13-no-exceptions` guard holds). A finite
candidate set + fixed-iteration Newton = an analyzable WCET.

## CLI (`hesap.motion.*`)

Kinematic state + limits are scalars, so the agent reaches the generators directly.

| command | params | output |
|---|---|---|
| `hesap.motion.otg.f64` | `p0,v0,a0, pf,vf,af, vmax,amax,jmax`, `samples` | blob `[valid,duration,nsamp,(t,p,v,a)*]` |
| `hesap.motion.scurve.f64` | `p0,pT, vmax,amax,jmax` | blob `[valid,total,tj,ta,tc]` |

Anchor `register_motion_cli_anchor()`; `test_cli.cpp` drives the OTG to its target and cross-checks that a rest-to-rest
OTG duration equals the jerk-limited S-curve time.

## Module edges (acyclic)

`crd-hesap-motion` → core/containers/memory · **math** (quaternion slerp/log/exp) · **crd-hesap** (CLI) ·
**crd-hesap-special** (Fresnel → clothoid) · **crd-hesap-dense** (the minimum-snap KKT LU). Planning (RRT/A*/MPC) is
Phase 3.1.11 control, a one-way consumer.

## Tests

`tests/hesap-motion/` — `test_motion` · `test_cli`. 37289 assertions / 11 cases (incl. 12 baked Ruckig single-DoF + 9
sync references), green on win-debug + the full win-tidy check set. Bench harnesses: `scratchpad/ruckig_lib` +
`scratchpad/otgbench`.
