# ADR-0087 — Large-scale deterministic simulation: environmental fields, surface-integral coupling, moving-frame agents, and player-count-scaled networking

**Status:** Proposed (design direction from a 2026-05-30 design session; ratify per-slice when eylem + the marine/aero + networking clusters resume after hesap Phase 3.1.6)
**Date:** 2026-05-30
**Tags:** [arch] [physics] [eylem] [networking] [determinism] [lod] [environment] [mmo]
**Related ADRs:** ADR-0086 (eylem unified motion model — refines its D10 networking), ADR-0035 (networking architecture — extends Layer 3), ADR-0062/0063 (eylem physics + determinism), ADR-0067 (force-field architecture — environmental fields), ADR-0073 (aerospace substrate — atmosphere/aero is an instance of this model), ADR-0065 (hesap — deterministic FFT = the spectral ocean), ADR-0076 (geometry — hull collision).
**Phase:** Phase 3.1 — Eylem (large-scale + environmental + many-human-networking direction for resume).

---

## Context

Worked scenario (the user's stress test): a **1700s naval-battle MMO** — a dynamic sea, dozens of ships colliding and under constant physics, thousands of soldiers on heaving decks trying to keep balance, **roughly half of them real human players** — under three **unbreakable rules**: *incredible animation + physics, deterministic networking, >60fps.* The general problem this exemplifies: **large-scale deterministic simulation with heavy environmental coupling and many concurrent human players.** The user explicitly rejected the point-probe buoyancy approach (many unseen floaters sampling the water surface — costly and hard to make deterministic/networked, the pain point in their shipped War Sails work).

This ADR records the *method* that generates the solution (so it transfers to cities, cavalry, destruction, weather — any large env-coupled many-human sim), plus the specific decisions. It **refines ADR-0086's D10** (which leaned toward lockstep) and **extends ADR-0035**.

## The method (six transferable principles)

These are the durable part; the decisions below are them applied.

1. **Replace state with deterministic fields.** Anything expressible as a closed-form function of (position, time, seed) is computed, not simulated, and is **never networked** — every client/server evaluates it identically.
2. **Integrate the real physics over a surface; don't sample probes.** A net force/torque from an exact surface integral beats hundreds of point samples — cheaper, smoother, exact, deterministic.
3. **Authoritative-coarse vs cosmetic-fine, everywhere.** The cheap thing that decides outcomes is deterministic + networked; the gorgeous thing is local, LOD'd, never on the wire.
4. **Detail follows attention.** Full fidelity only where a human is looking.
5. **Scale the network model to interaction-locality and human-player count.** There is no single correct netcode — there is the right one per regime.
6. **Solve in the right reference frame.** A hard sub-problem (a soldier balancing on a rolling deck) is trivial in the carrier's frame and a nightmare in the world's.

## Decision (Proposed)

**D1 — Environmental media are DETERMINISTIC FIELDS, not simulated or networked state (Principle 1).** Ocean, wind, atmosphere, current are procedural functions — `field(x, t, seed)` → value + gradient/velocity — evaluated identically on every machine. **Zero environmental state crosses the network.** Split per Principle 3:
- **Physics field** — cheap, low-frequency, deterministic (e.g. a few Gerstner waves, or the hesap deterministic FFT spectrum); this is what moves bodies and must be bit-identical across platforms.
- **Render field** — rich, GPU, cosmetic (full FFT ocean, foam, chop); may differ per client; never affects outcomes. They agree on the low frequencies that move ships.

**D2 — Body↔fluid coupling by SURFACE INTEGRAL over a low-poly proxy, not point-probe floaters (Principle 2).** Give each body a low-poly proxy hull (~dozens of faces). Each step, over submerged/exposed faces, sum the **exact hydrostatic pressure force** (ρ·g·depth·area·normal at the face centroid) plus **hydrodynamic** terms (anisotropic quadratic drag, added mass, field-velocity coupling) — the Kerner boat model. One deterministic sum yields float, **righting moment** (center-of-buoyancy vs center-of-mass → roll/pitch/bob for free), realistic anisotropic turning/drag, and wave-riding — cheaper than floaters, exact, and trivially deterministic (it reads D1's field). This **generalizes to aerodynamics** (ADR-0073): atmosphere is a D1 field, aero force is the same surface integral. Buoyancy fidelity LODs (Principle 4): focused ships get the full hull integral; distant ships get a single buoyancy force + precomputed righting curve + a wave-driven bob/roll oscillator.

**D3 — Agents on moving carriers are solved in the carrier's LOCAL non-inertial frame (Principle 6).** Parent the agent to the carrier's frame; the carrier's acceleration enters as a **pseudo-force** so the agent physically feels the deck heave. A **balance controller** drives recovery — inverted-pendulum center-of-mass over the support polygon, using ankle → hip → stepping (foot-IK) strategies in escalation, and beyond recovery the **powered ragdoll** (ADR-0086 D3) takes over via the continuous gain ramp (stagger, rail-grab via IK reach, fall). Near agents get the full controller; distant agents get the carrier's motion as a procedural sway over animation. Generalizes to characters on any vehicle/platform/elevator.

**D4 — Networking scales with human-player count; determinism is the bedrock for every regime (Principle 5). This refines ADR-0086 D10.**
- **Few humans + AI crowd** (e.g. a coop siege) → **deterministic lockstep** (inputs-only, everyone bit-identical), per ADR-0086 D10.
- **Many humans (MMO scale, e.g. the naval battle)** → **server-authoritative, spatially-partitioned DETERMINISTIC simulation regions** (server meshing — one node per ship-cluster/zone, seamless handoff as players cross boundaries) + **interest management** (a client receives only state near its player → bandwidth flat as the battle grows) + **client prediction & rollback** that is *tight because client and server run the identical deterministic code* (predictions rarely diverge) + the **authoritative/cosmetic split** (server simulates only the cheap authoritative coarse layer for a region; each client computes the gorgeous balance/ragdoll detail locally for its own view — never simulated server-side, never networked). Lockstep is **not** used here — waiting on the slowest of hundreds + the desync surface make it untenable.
- **Determinism's dividends apply in both regimes:** full replay (battle re-runs from inputs+seed — spectating, killcams), and anti-cheat (server re-derives deterministic truth, rejects impossible client claims). "Deterministic networking" at MMO scale means *deterministic authoritative + deterministic prediction*, not lockstep.

**D5 — Detail-follows-attention extends to environment coupling and crowds.** The frame budget is spent where a human looks: focused ships get full hull integrals, focused soldiers get full balance + powered ragdoll; everything else degrades along the ADR-0086 D7 LOD continuum (distant ships = oscillators, distant soldiers = GPU-skinned animation + procedural sway). >60fps is a *consequence* of D1–D5, not a separate optimization pass — the costly things were replaced by fields (D1), integrals (D2), and local/cosmetic work (D4), and detail exists only where seen.

## Consequences

**Good**
- The three unbreakable rules are met *by construction*: incredible physics (real hull hydrostatics + powered-ragdoll deck balance), deterministic networking (fields-not-state + deterministic region sims + prediction → replay + anti-cheat), >60fps (detail-follows-attention + fields + local cosmetic).
- The method generalizes: the same six principles design a city crowd, a cavalry charge, a collapsing building, a weather system. This ADR is a *pattern*, not a naval feature.
- Unifies marine + aerospace coupling (ADR-0073) under one field + surface-integral model; environmental fields slot alongside ADR-0067 force fields.

**Bad / costs / risks (the honest frontier)**
- **Server meshing with deterministic handoff** (migrating a deterministic region between server nodes as agents cross boundaries) is bleeding-edge (Star Citizen is still fighting it). Realistic target: ~100–200 humans per seamless region, meshed to *feel* continuous, not 1,000 in one undivided melee.
- **Cross-platform FP determinism** (so client prediction matches server bit-for-bit) is the deep dependency — deterministic transcendentals, strict FP env, per-platform validation (extends ADR-0063). Invest early; the whole prediction model rests on it.
- **Balance-controller robustness** (a biped reliably self-catching on a stochastic heaving deck without looking drunk or rigid) is research-grade *feel* tuning; the architecture is right, the polish is iteration.
- **Physics-field/render-field consistency** must be managed so ships visibly sit on the rendered sea (low-freq agreement + the render adding only cosmetic high-freq).

## Relationship to existing ADRs

- **Refines ADR-0086 D10** — D10 leaned toward lockstep; D4 here establishes that lockstep is the *few-human* regime and many-human MMO uses server-authoritative deterministic regions + prediction. Same determinism bedrock, delivery matched to player count. (0086 D10 carries a forward-pointer here.)
- **Extends ADR-0035** — concretizes Layer 3 (client-server + rollback + extrapolation) with server-meshing, interest management, and the deterministic-prediction rationale for large player counts.
- **Generalizes ADR-0073 (aerospace)** — atmosphere = a D1 field; aero forces = the D2 surface integral. Marine and aero coupling share one model.
- **Relates to ADR-0067 (force fields)** — environmental fields (ocean/wind/atmosphere/current) are deterministic field sources alongside the FieldFormula catalog.
- **Builds on ADR-0065 (hesap)** — the deterministic FFT is the spectral-ocean generator; the per-step constrained solve is the eylem solver backbone. **ADR-0076 (geometry)** — hull collision.

## Alternatives rejected

- **Point-probe buoyancy (floaters sampling the water + per-point forces)** — costly, noisy, and hard to make deterministic/networked (the user's shipped War Sails pain). Replaced by the D2 surface integral.
- **Networking environmental state (streaming the water/wind)** — unnecessary and unscalable; D1 makes it a deterministic field instead.
- **Full deterministic lockstep for many human players** — wait-on-slowest + desync surface make it untenable past a handful of humans; D4 scales the model instead.
- **World-frame free-body agents on moving platforms** — fragile and expensive; D3 solves in the carrier frame.

## References

- ADR-0086 — eylem unified motion model (powered ragdoll, LOD continuum, authoritative/cosmetic; this ADR refines its D10).
- ADR-0035 — networking layers (extended here for MMO scale).
- ADR-0073 — aerospace (the aero instance of D1+D2); ADR-0067 — force fields.
- ADR-0065 — hesap (deterministic FFT ocean + the constrained-solve backbone); ADR-0076 — geometry.
- Kerner, "Water interaction model for boats in video games" (the surface-integral buoyancy basis for D2).
- Memory: `project_eylem_crush_physx_jolt_with_determinism` — the performance bar all of this must clear.
