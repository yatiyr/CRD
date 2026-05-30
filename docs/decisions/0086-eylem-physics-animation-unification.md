# ADR-0086 — Eylem unified motion model: physics-animation as one solve, LOD fidelity continuum, crowd-scale deterministic networking

**Status:** Proposed (design direction from a 2026-05-30 design session; to be ratified + reconciled per-slice when eylem resumes after hesap Phase 3.1.6)
**Date:** 2026-05-30
**Tags:** [arch] [physics] [eylem] [animation] [networking] [determinism] [lod]
**Related ADRs:** ADR-0062 (Eylem physics architecture — base), ADR-0063 (Eylem determinism contract), ADR-0021 (Animation architecture), ADR-0074 (Eylem cinematic bridge), ADR-0035 (Networking architecture), ADR-0065 (hesap numerical substrate — the solver backbone), ADR-0076 (geometry substrate — broad/narrow phase), ADR-0020 (scene ECS hybrid — the articulated hierarchy), ADR-0078 (units).
**Phase:** Phase 3.1 — Eylem (currently paused for geometry + hesap; this is the motion-model direction for resume).

---

## Context

The requirement, stated by the user: movement, animation, and everything physical must be **exceptionally smooth, elegant, and completely real** — explicitly **NOT** the fragmented model of Unity/most engines, where animation and physics are separate peers that fight (kinematic vs ragdoll with a discrete switch, IK as a post-process bolt-on, a capsule character controller divorced from the sim). The target workloads include ragdolls, entity-tying constraints, robots, IK/FK, character controllers blending many keyframe + procedural animations, **thousands-troop siege battles**, and **coop/multiplayer with deterministic physics shared by all players**.

The foundation now exists to do this without the fragmentation: a world-class deterministic sparse solver (**hesap**, ADR-0065 — beats CHOLMOD on structured cases, the heart of stiff/constraint solving), the geometry substrate (ADR-0076 — GJK/EPA/SAT, BVH), the ECS + scene tree (ADR-0020), the fiber job system, the determinism moat (cross-thread bit-identical), and units (ADR-0078).

**Honest note on existing decisions.** ADR-0062 already pins eylem's v1–v5 solver as **Sequential Impulses (maximal-coordinate articulations)**, with TGS in v2, XPBD for soft in v3, and **reduced-coordinate Featherstone only in v6/v7**; it also notes "v9 may unify rigid into XPBD." The current animation↔physics integration is ADR-0074 (cinematic bridge: animation curves driving *kinematic* bodies) plus ragdoll-on-death — which is close to the fragmented model the user wants to avoid. This ADR therefore **refines and evolves** that integration; it is Proposed and carries an explicit reconciliation section (below).

## Decision (Proposed)

**D1 — One solver, one state; animation proposes, physics disposes.** There is no separate "animation transform" and "physics transform." The rendered pose of a physical entity is *always* the output of the physics solve. Animation never writes final transforms — it produces **targets**. Physics is the sole arbiter that resolves every influence (animation intent, contacts, joints, IK, balance) into one consistent pose. This kills the fragmentation at the root: there is only one place a pose is decided.

**D2 — Constraints are the universal primitive.** Collision (unilateral non-penetration), joints (bilateral/limited — ragdoll, robot actuator, "tie entity A to B", doors), IK (end-effector = target), and animation (motor/drive toward a keyframed target at gain *k*), balance, look-at, foot-plant — are all **constraint types in one constraint system solved together each step**. New behaviors are new constraint types, not new subsystems.

**D3 — Powered/active ragdoll: animation = per-joint motor targets + a *continuous gain*.** Every joint has a motor (PD/actuator) driving it toward the animation target at a controllable gain. High gain → tracks animation tightly (a hard impulse still perturbs it → real reaction); zero gain → pure ragdoll. The continuous gain ramp, per-joint and per-moment, **is** the blend between animated and physical — so a hit produces a real stagger/recoil, and death is a gain ramp with **no discrete kinematic↔ragdoll switch**. *(This refines ADR-0021 and evolves ADR-0074 — see Reconciliation.)*

**D4 — Reduced-coordinate articulated bodies (Featherstone) are the end-state for characters/robots.** Joints exact by construction (no drift), O(n), stable — the robotics/MuJoCo-class basis for *real* articulated motion. **FK** is the forward pass (free); **IK** is constraints solved against the same Jacobians (no separate IK pass fighting the pose). *(ADR-0062 schedules Featherstone at v6/v7 after maximal-coords+SI v1–v5; this is the end-state direction, with a possible earlier pull for hero characters — see Reconciliation.)*

**D5 — Character controller = a motor-driven physical body + responsive locomotion targets**, not a kinematic capsule sweep. Locomotion targets come from responsive animation (motion matching); the physical body + contacts + balance give real reaction (push → stagger, slope/stairs → IK + balance adapt). Responsive *and* physical.

**D6 — The solver is pluggable over the constraint graph.** The substrate is the constraint graph over the articulated state; the engine underneath is chosen by regime: SI/TGS (ADR-0062 v1/v2), XPBD for soft (v3), reduced-coords + **hesap sparse solve** for exact/robotics fidelity (v6/v7), converging toward the unified position-based or hybrid solver ADR-0062 already gestures at for v9. The per-step constrained system is a large sparse solve — this is precisely why hesap mattered: it lets us solve more constraints accurately and stably (smooth under load) versus mushy sequential-impulse-only.

**D7 — LOD is a fidelity continuum, not decoupled clocks.** A single per-agent "simulation level" scales the *whole* pipeline (animation sample rate + physics fidelity together): near/hero = full unified solve; mid = fewer iterations + simplified contact + visible-limb IK; far = motors at near-infinite gain ≈ kinematic (cheap) ; culled = analytic/asleep. Lower animation tick rate for distant agents survives as a **target-refresh-rate LOD parameter on one system** (refresh less, interpolate), NOT as a second clock that structurally splits animation from physics.

**D8 — Authoritative-coarse / cosmetic-fine split.** The **authoritative** layer (position, controller capsule, collision, "did the blade connect / is it dead", gameplay constraints) runs full-rate and deterministic for every gameplay-relevant agent, regardless of visibility. The **cosmetic** layer (per-bone articulated detail, active-ragdoll nuance, foot-IK) is LOD-gated by visibility and is non-authoritative. This split is the key enabler for BOTH LOD scaling and networking (D10), and it preserves determinism (visibility-based LOD can never desync the authoritative sim).

**D9 — Crowd scale = detail-follows-attention.** Thousands of agents run the cheap tier (authoritative capsule + animation/skinning, GPU-batched — ADR-0062 v8 `crd-eylem-gpu`). Full-fidelity physical reactions are reserved for the focused/visible subset (~dozens to a low hundred) as transient, contact-localized bursts (the impulse applied at the actual contact point; impacted limbs respond; secondary motion + foot-IK; then settle back to animation). Brute-force "thousands fully physical simultaneously" is neither affordable nor needed; the gold standard spends the budget where the player perceives it.

**D10 — Networking: deterministic lockstep for the shared authoritative sim + rollback/prediction for local players; cosmetic layer local.** State-replication does not scale to thousands of agents; **lockstep sends inputs, not state**, and every client computes the identical battle because the sim is bit-deterministic — bandwidth is independent of agent count. Local-player responsiveness comes from rollback (GGPO-style, per ADR-0035 Layer 3) or input-delay; the crowd advances on confirmed lockstep. Only the cheap **authoritative** layer (D8) needs determinism + networking; the expensive **cosmetic** reactions are computed locally per client (each renders detail for its own camera) and are reproducible from the deterministic triggers. This requires **cross-PLATFORM** FP determinism — strict IEEE-754, no x87/fast-math, deterministic reductions (have these via ADR-0063), **plus deterministic transcendentals** (eylem uses `crd-math`'s own deterministic `sin`/`cos`/`sqrt`, not platform libm) and per-platform bit-identity validation. This extends ADR-0035 + ADR-0062 §5 (snapshot-replay) + ADR-0063. **For MANY human players (MMO scale), lockstep does NOT hold — see ADR-0087 for the server-authoritative deterministic-region + client-prediction model that scales the network model to player count.**

## Consequences

**Good**
- The fragmentation the user dislikes is gone by construction: one solver, one pose, animation-as-targets, physics-as-arbiter; behaviors added as constraint types.
- Smooth (continuous gain blends, no state-switch pops; real solver, no jitter under load), elegant (one model), real (exact articulation + active ragdoll + IK-grounded contact).
- A thousand-troop coop siege that is **physically reactive *and* perfectly replayable + lockstep-networkable** — something neither PhysX/Unity (non-deterministic) nor Euphoria (non-reproducible) can claim. The determinism moat cashes out here; this is a genuinely unusual capability.
- The authoritative/cosmetic split unifies the LOD story and the networking story under one idea.

**Bad / costs / risks**
- Full-fidelity per-agent physics is affordable for ~dozens to a low hundred at once, not thousands simultaneously — the focused-subset budget is a real design constraint (large all-on-screen full-reaction counts need a cheaper near-tier solver or a cap).
- Cross-platform FP determinism (D10) is a deep, ongoing per-platform validation effort (deterministic transcendentals + strict FP env), beyond the cross-thread moat already held.
- Craft risk: invisible LOD transitions (no "pop" to detail), GPU-crowd ↔ CPU-focused coordination, and bounded rollback scope around player↔crowd interactions.
- GPU bit-determinism (ADR-0062 v8) is genuinely hard — solved with deterministic GPU reductions, not by relaxing the moat.

## Relationship to existing ADRs / reconciliation needed

- **ADR-0062 (Accepted).** Aligns: determinism-by-construction, ECS-native, submit-to-`crd-jobs`, snapshot-replay as core deliverable, AoSoA-8, the v8 GPU module, the "v9 may unify rigid into XPBD" aspiration (D6 fleshes out that end-state). **Reconcile:** D4 reduced-coords-Featherstone is scheduled v6/v7 in 0062 (maximal-coords + SI for v1–v5); this ADR proposes it as the *character/robot end-state* and asks whether a *powered-articulation* path should be pulled earlier for hero characters, or whether maximal-coords-SI with motor joints (D3) suffices through v5. To be decided when the articulation slices are planned.
- **ADR-0074 (cinematic bridge) + ADR-0021 (animation).** This ADR's **powered/active-ragdoll** model (D3) evolves the current "animation curves drive kinematic bodies + ragdoll-on-death" integration toward a unified motor-driven model. **Reconcile the boundary:** cutscene/cinematic playback may stay kinematic-curve-driven (ADR-0074) where determinism + exact authored motion is wanted; *gameplay* characters use powered ragdoll (D3). Clarify which path each use case takes when the animation-physics slices are planned.
- **ADR-0035 (networking).** D10 extends Layer 2/3 with the *crowd-scale lockstep* (inputs-not-state) rationale, the *authoritative/cosmetic network split*, and the *cross-platform transcendental* determinism requirement. No conflict; this is the eylem-specific elaboration of 0035's "lockstep for strategy/simulation."
- **ADR-0063 (determinism contract).** D10 adds the cross-platform transcendental + per-platform-validation requirement as an extension of the contract.

## Alternatives rejected

- **Unity-style fragmentation** — animation/physics as fighting peers, discrete kinematic↔ragdoll switch, capsule-hack character controller, bolt-on IK. The thing we are explicitly not building.
- **Maximal-coordinate ragdolls as the permanent character model** — drift/jitter; fine as the v1–v5 starting point (ADR-0062) but not the end-state for "completely real" articulation.
- **State-replication networking** — does not scale to thousands of agents (bandwidth); lockstep (inputs) is the only model that does.
- **Server-authoritative-only (no shared deterministic sim)** — loses the "every player sees the identical battle" property and the replay/authored-realism payoff; client-prediction is added *on top of* the deterministic substrate (D10), not instead of it.

## References

- `docs/research/cerid-eylem.md` — eylem research backing.
- ADR-0062 / ADR-0063 — eylem physics + determinism (base).
- ADR-0021 / ADR-0074 — animation + cinematic bridge (reconcile).
- ADR-0035 — networking layers (extend).
- ADR-0065 — hesap (the per-step constrained-solve backbone).
- Memory: `project_eylem_crush_physx_jolt_with_determinism` — the performance bar this motion model must hit (beat PhysX + Jolt, head-to-head per slice, without sacrificing determinism).
