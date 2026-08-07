# ADR-0075 — Eylem testing rigor and conservation-law CI

> Status: **Accepted** (2026-05-11)
> Companions: [ADR-0062](0062-eylem-physics-architecture.md) (eylem
> architecture), [ADR-0063](0063-eylem-determinism-contract.md) (determinism
> contract), [ADR-0067](0067-eylem-force-field-architecture.md) (force
> fields), [ADR-0068](0068-eylem-body-types-collision-filtering-callbacks.md)
> (body / filter / callback substrate). Coverage source:
> [`docs/research/cerid-eylem-coverage-audit.md`](../research/cerid-eylem-coverage-audit.md) §3.7.

## Context

Phase 3.1 v1 ships ~10K LOC of physics across solver, broadphase,
narrow phase, joints, articulations, fields, contact filtering,
callbacks, and snapshot/replay. Without a disciplined test contract,
the **scientific computing** mandate domain is unsubstantiated: a
games engine can ship "looks right" — a scientific engine cannot.
Robotics RL training, aerospace verification simulation, and
cinematic deterministic playback all depend on the engine's numerical
behaviour being **measurable** and **regression-protected**, not just
"passes a smoke test."

Three categories of risk are live without this ADR:

1. **Numerical drift** — solver impulse application accumulates rounding
   error. Energy slowly drifts up (numerical injection) or down (numerical
   damping). Without conservation-law tests this is invisible until a
   medical-sim user reports their pendulum loses 5% of swing per minute.
2. **Algorithmic regression** — a contact-cache refactor changes the
   solver convergence subtly. Existing tests pass, but the canonical
   10-box stack now jitters. Without closed-form regression tests the
   change ships, the next sandbox demo looks slightly worse, nobody
   knows why for three weeks.
3. **Cross-engine performance comparability** — Cerid's "elite tier"
   claim is empty without "elite tier" measurements. PhysX / Bullet /
   Box2D all publish their canonical-scene numbers; Cerid's are not on
   the same axis without a comparison harness.

The coverage audit (`cerid-eylem-coverage-audit.md` §3.7) marked this
as **P0 — must close before v1l**. CI infrastructure should land WITH
the v1j snapshot-replay harness, not after, because each new solver
slice (v1e SI, v6c Featherstone, v6d Nonsmooth Newton, v3a XPBD-soft)
needs its conservation invariants asserted at the slice DoD — not
retroactively.

## Decision

Cerid eylem ships **six categories of test discipline**, each with CI
assertions, all integrated with the existing 14-config sweep
(`scripts/full-sweep.ps1`).

### 1. Conservation-law tests

For every solver shipped, a frictionless + gravityless scene asserts
the physical conservation laws within bounded epsilon over a long
integration window:

```cpp
// tests/eylem-rigid3d/conservation/test_si_solver.cpp
TEST_CASE("SI solver conserves kinetic energy in frictionless free flight",
          "[eylem][solver][conservation]")
{
    PhysicsConfig cfg;
    cfg.gravity = {0, 0, 0};               // disable gravity
    cfg.fixed_dt = 1.0F / 240.0F;          // 240 Hz for tight integration

    auto scene = make_si_physics_scene(cfg);
    BodyId b = scene->add_body(/*mass=1, vel=(1,2,3), pos=origin*/);

    const auto e0 = compute_kinetic_energy(*scene);
    for (int step = 0; step < 14400; ++step) // 60 s @ 240 Hz
    {
        scene->step(cfg.fixed_dt);
    }
    const auto e1 = compute_kinetic_energy(*scene);

    REQUIRE_THAT(e1, Catch::Matchers::WithinRel(e0, kEpsEnergy)); // 1e-5 default
}
```

Six conservation invariants per solver (SI / TGS / XPBD-rigid /
Featherstone / Nonsmooth Newton / etc.):

1. **Kinetic energy** in frictionless gravityless free flight (no
   external forces → energy strictly conserved within FP rounding).
2. **Angular momentum** in frictionless gravityless free spin (no
   torque → angular momentum strictly conserved).
3. **Linear momentum** in two-body frictionless elastic collision
   (the canonical pool-ball test — pre-collision and post-collision
   linear momenta sum identical).
4. **Total energy** in pendulum free-swing (KE + PE constant; tests
   gravity integration accuracy).
5. **Constraint violation** in stacked-box scene (penetration depth
   stays under `eps_penetration` for 60 s — tests stabilisation).
6. **Sleep-wake idempotency** — sleeping bodies woken without state
   change reproduce identical trajectory (tests sleep restoration).

Per-solver epsilon defaults defined in `eylem-tuning.md` (companion
doc); per-test override via `WithinRel(value, eps)` argument.

### 2. Closed-form regression tests

Scenes with known analytic solutions; assert simulator output matches
analytic prediction within tolerance. This catches "the simulator is
self-consistent but wrong" failure mode.

| Scene | Analytic prediction | Tolerance |
|---|---|---|
| Pendulum (small angle) | Period `T = 2π√(L/g)` | ≤ 1% over 30 s |
| Projectile (no drag) | Range `R = v² sin(2θ) / g`; max height `H = v² sin²(θ) / 2g` | ≤ 0.5% (FP-limited) |
| Two-body Kepler orbit (point masses) | Period from Kepler's third law `T² = 4π²a³/μ` | ≤ 1% per period |
| Simple harmonic oscillator (spring) | Period `T = 2π√(m/k)`; amplitude constant | ≤ 0.5% over 100 cycles |
| Free-fall with quadratic drag | Terminal velocity `v_t = √(2mg / ρCₐA)` | within 1% after 5τ |
| Uniform circular motion | Centripetal accel `a = v²/r` | within 1% over 60 s |
| Rolling ball (no slip) | Energy conservation `KE_trans + KE_rot = const` | within 0.5% over 30 s |

Failures here mean the solver is producing wrong physics — bug, not
imprecision. Fail loud. Tag `[eylem][regression][closed-form]`.

### 3. Long-duration drift tests

Run a canonical scene (10-box stack, 30-link humanoid ragdoll, simple
pendulum) for 60 minutes of sim time. Snapshot every 10 s. Verify
state-hash divergence grows no faster than `O(t / dt)` accumulated
rounding error. A drift ratchet (state hash changes by more than
expected between two snapshots) fails the build.

The snapshot system (v1j) already exists; this test consumes it.

### 4. Cross-engine comparison benchmark

Canonical scenes simulated in eylem **and** Box2D **and** Bullet (both
MIT-licensed; brought in as test-only deps under
`tests/eylem-rigid3d/cross_engine/`):

| Scene | Metric | Cerid budget |
|---|---|---|
| 10-box stack | Sleep time + max penetration depth | within 2× Box2D-3D-equivalent / Bullet |
| Newton's cradle (5 spheres) | Energy retention after N collisions | within 2× best-of-three |
| Swept-rotor pendulum | Phase-space drift over 60 s | within 2× best-of-three |
| 30-link humanoid ragdoll | Settle time + final pose stability | within 2× best-of-three |
| 100-body planar stack | Solver iterations to converge | within 2× best-of-three |

The "within 2× best-of-three" assertion is the elite-tier bar — beats
the worst engine, comparable to the median, may or may not lead. CI
fails the build only on >2× regression. Reference implementations
inform algorithm choice — not source code reuse — per the
[ADR-0065 §3](0065-hesap-numerical-substrate.md) posture
(used here for tests only, not shipping engine).

### 5. Property-based tests

Random scenes with random seeds; assert universal invariants regardless
of scene content:

- **No body penetrates static ground** when initialised above ground +
  any dynamic body's `pos.y >= 0` after 60 s of sim
- **Collision count is bounded** — N bodies in unit cube generate
  ≤ N(N-1)/2 contact pairs per substep
- **Sleep state is deterministic** — same seed → same sleep schedule
- **Force magnitude bounded** — apply `force = 1` impulse, body's
  resulting velocity bounded by `1 / mass` (no impulse amplification)

Catch2's [Generators](https://github.com/catchorg/Catch2/blob/devel/docs/generators.md)
drive the random seed; CI runs 100 random scenes per test, fails on
any single failure. Pinned RNG seed for deterministic CI.

### 6. Stress tests

| Scene | Spec | Assertion |
|---|---|---|
| 1000-box stack | 1000 dynamic boxes, 60 s settle | sub-frame budget on Zen 4 / Raptor Lake |
| 100k particles | XPBD particle cloud + 100 rigids | 60 Hz step on AVX2 |
| 30-link humanoid | Single ragdoll, 60 s flop | <2 ms per step |
| 1000 bodies in single broadphase cell | All co-located in 1m³ | broadphase doesn't degrade O(n²) |

Stress tests are **bench-style** — they fail the build on regression vs
recorded baseline (same model the Phase 2.5 jobs benchmarks use).

## CI integration

All six categories integrate with `scripts/full-sweep.ps1`:

- **Conservation + closed-form + property-based** run on every config
  in the 14-config sweep (debug, asan, release, etc.) — they're cheap
  unit tests + don't depend on hardware perf.
- **Cross-engine comparison + stress** run only on `win-release` and
  `linux-gcc-release` to avoid 14× the bench wall time. Configs that
  don't measure performance still build the test binaries to catch
  build regressions.
- **Long-duration drift** runs in a nightly CI job (10 minutes wall
  time per config); failures open a tracked detour.

The v1j replay-hash test (snapshot stable across MSVC/clang/gcc ×
x64/ARM × Windows/Linux) becomes the v9b CI hard-block — landing
within this ADR's bench-budget framework.

## Slice plan (locked)

| Sub-slice | Scope | LOC | Tests |
|---|---|---|---|
| **v1l-test-conservation** | Conservation-law test infrastructure (kinetic energy, angular momentum, linear momentum, constraint violation). Runs on SI solver in v1l; expanded as new solvers ship. | ~300 | ~12 |
| **v1l-test-closedform** | Closed-form regression tests (pendulum, projectile, SHO, drag terminal velocity, circular motion, rolling, Kepler orbit). | ~250 | ~10 |
| **v1l-test-stress** | 1000-box stack + ragdoll + broadphase-co-located + XPBD-particle stress benches. | ~200 | bench |
| **v9b-test-cross-engine** | Box2D + Bullet integration (test-only deps) + canonical-scene comparison harness. | ~400 | ~6 |
| **v9b-test-drift** | 60-minute long-duration drift CI jobs across 14 configs. | ~100 | nightly |
| **v9b-test-property** | Property-based random-scene invariants via Catch2 generators. | ~200 | 100/scene |

**Total ~1450 LOC + ~30 tests + 5 benches + nightly CI infra.** Lands
across v1l (initial categories) + v9b (cross-engine + drift +
property) — same split the audit recommended.

## Rationale

### Why six categories, not one "good enough" suite

Each category catches a different failure mode the others miss:
- Conservation tests catch numerical drift but not algorithmic bugs
  (a wrong-but-self-consistent solver passes conservation).
- Closed-form tests catch algorithmic bugs but not perf regressions.
- Cross-engine comparison catches "we are 5× slower than Bullet but
  haven't noticed" — neither conservation nor closed-form sees this.
- Property-based tests catch corner-case crashes (random seeds find
  what designers forget to write tests for).
- Stress tests catch O(n²) regressions invisible to small unit tests.
- Drift tests catch slow accumulation invisible to short tests.

Six suites are roughly the discipline standard at the elite tier
(Box2D Catto's posture + Drake's CI + MuJoCo's regression tracking
all overlap with these six categories).

### Why "within 2× best-of-three" is the elite-tier bar

A games-engine bar would be "within 5× of leader"; a scientific bar
would be "matches leader within 5%". Cerid commits to the
**games-engine bar PLUS strict floor** — within 2× of the best of
three published-comparable references on canonical scenes. This is the
provable claim that backs "beyond industry standard": measurably
not-much-slower than the reference engines, with measurably better
determinism + multi-domain coverage.

### Why test-only Box2D + Bullet, not source code reuse

Per ADR-0065 §3 ("reference implementations inform algorithm choice —
not source code"). The cross-engine benchmarks consume Box2D + Bullet
as black boxes, identical to how a customer would integrate them. Cerid
ships zero Bullet/Box2D code — the test framework links them only to
generate comparison numbers.

### Why CI integration on every config, not just release

Conservation + closed-form + property-based tests are unit tests —
their cost is dominated by Catch2 setup, not the actual physics.
Running on all 14 configs catches platform-specific FP differences
(MSVC vs clang vs gcc; x64 vs ARM; sanitizer effects on solver
stability) that a release-only bar would hide.

## Consequences

**Positive:**
- Scientific computing domain claim is substantiated by measurable
  conservation invariants on every solver.
- Algorithmic regressions caught at the ADR-0075 test slice DoD,
  not by users three weeks later.
- Cross-engine comparison provides the "elite tier" objective claim.
- Drift tests guarantee long-running sims (medical, aerospace
  verification, robotics RL) don't degrade silently.
- Property-based tests find corner-case crashes designers forget to
  write tests for.

**Negative:**
- Test-only Box2D + Bullet integration adds external dependency
  surface (~3 days work to wire up CPM.cmake fetch + cross-engine
  scene authoring).
- Long-duration drift tests are nightly-only; fast-iteration developers
  don't see them. Mitigation: failures open tracked detours;
  `scripts/full-sweep.ps1 --include-drift` opt-in for local pre-PR.
- Cross-engine comparison numbers can become stale (Box2D / Bullet
  versions move). Mitigation: pinned versions in CPM; refresh quarterly
  in a tracked detour.

**Neutral:**
- ~1450 LOC + ~30 tests + 5 benches added across v1l + v9b. Material
  but proportional to the scientific-computing claim.
- The ADR-0075 test framework is reused by ADR-0067 fields
  (conservation under field-applied work) and ADR-0068
  (callback-storm bench for destruction scenes) — net infra value
  exceeds the new lines.

## References

**Coverage source:** [`docs/research/cerid-eylem-coverage-audit.md`](../research/cerid-eylem-coverage-audit.md) §3.7

**Companion ADRs:**
- [ADR-0062](0062-eylem-physics-architecture.md) §1 (snapshot-replay deliverable centrality)
- [ADR-0063](0063-eylem-determinism-contract.md) — determinism contract (the *why* behind drift tests)
- [ADR-0065](0065-hesap-numerical-substrate.md) §3 — reference-implementation posture (informs cross-engine bench)

**Reference implementations (test-only deps):**
- [Box2D v3](https://box2d.org/) — MIT, primary 2D reference
- [Bullet 3](https://github.com/bulletphysics/bullet3) — Zlib, primary 3D reference

**Test framework:**
- [Catch2 v3](https://github.com/catchorg/Catch2) — already shipped (Phase 1)
- [Catch2 Generators](https://github.com/catchorg/Catch2/blob/devel/docs/generators.md) — property-based-test driver
