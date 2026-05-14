# Phase 3.2 — Animation substrate

**Status:** 📋 planned (architectural shape captured by ADR-0021; scope expanded per ADR-0077 §4)
**ADRs:** ADR-0021 (animation architecture), ADR-0077 §4 (multi-domain expansion)
**Slot:** after Phase 3.1.7 close + Phase 3.1 eylem v9 close + Phase 3.1.6 hesap close.

## Scope

### Skeletal animation

- **Skinning** — linear blend skinning (LBS) for performance, dual quaternion skinning (DQS) for quality (avoids candy-wrapper artifact).
- **Skeleton hierarchy** — `crd-scene` tree of joint entities; transforms as `crd-math::Transform`; bones as relations.
- **Animation clips** — keyframe sequences with cubic Hermite interpolation; cooker emits CRDR `'ANIM'` artifacts.
- **Skeleton retargeting** — map mocap from one skeleton topology to another (T-pose alignment, bone-name correspondence, length adaptation).

### Blending + state machines

- **Animation blend trees** — weighted blending across N clips; layered blending (additive layers for procedural overlays).
- **State machines** — finite-state machines with transitions, blend windows, parameter-driven transitions. Authoring in node graph.
- **Motion matching** — modern AAA pattern (Holden 2018+); replaces hand-authored state machines with feature-based clip retrieval. Consumes `crd-ml-inference` for learned matching (Phase 3.1.14).

### Inverse kinematics (IK)

- **Two-bone IK** — analytic solver (arm / leg).
- **FABRIK** (Forward And Backward Reaching IK) — iterative; arbitrary chain length.
- **Full-body IK** — body-IK with center-of-mass + footprint constraints; Aristidou et al. patterns.
- **Look-at IK** — head / eye tracking targeting.
- **Foot IK** — ground alignment via geometry raycasts (`crd-geometry-bvh`).

### Procedural animation

- **Look-at** — head, eye targeting.
- **Lean** — character body lean on slopes / turns.
- **Foot-IK** — adjustment to terrain via raycasts.
- **Hand-IK** — hand placement on environment objects (door handles, ledge grabs).
- **Pose corrections** — apply small adjustments to baked animations based on dynamic context.

### Morph targets / blendshapes

- **Vertex deltas** — per-target delta arrays; cooker compresses redundant vertices.
- **Activation weights** — multiple targets blend additively.
- **GPU evaluation** — compute shader applies blendshapes pre-skinning.
- **Facial animation** (FACS) — action units mapped to blendshape activations.

### Facial animation

- **FACS** (Facial Action Coding System) — 46 action units, mapped to blendshape targets.
- **Visemes** — speech-driven mouth shapes; consumes audio analysis or ML lip-sync (Phase 3.1.14).
- **ML-driven facial** — audio-to-blendshape neural networks (Apple Animoji-class) via `crd-ml-inference`.

### Crowd simulation

- **RVO** (Reciprocal Velocity Obstacles) — multi-agent steering avoiding collisions.
- **Behavior trees** — hierarchical decision-making per agent.
- **Path planning** via `crd-control` (Phase 3.1.11) — A* / RRT for crowd navigation.
- **Group dynamics** — formation, herding behaviors.

## Dependencies

- `crd-scene` — skeleton hierarchy as ECS entities + relations.
- `crd-math` — transform math, quaternion interpolation.
- `crd-resources` — animation clip loading via `'ANIM'` CRDR.
- `crd-renderer` — skinning shader integration.
- `crd-geometry-bvh` — foot-IK raycasts, hand placement.
- `crd-eylem` — physics-driven ragdoll, motion ragdoll blending.
- `crd-ml-inference` (Phase 3.1.14) — motion matching, facial ML.
- `crd-control` (Phase 3.1.11) — crowd path planning.

## Sub-modules (planned)

- `crd-anim-core` — skeleton / clip / playback runtime.
- `crd-anim-skin` — LBS / DQS skinning.
- `crd-anim-blend` — blend trees + state machines.
- `crd-anim-ik` — two-bone / FABRIK / full-body IK.
- `crd-anim-morph` — blendshapes + facial.
- `crd-anim-motion-match` — motion matching (depends `crd-ml-inference`).
- `crd-anim-crowd` — RVO + behavior trees.

## Reference reading

- Parent "Computer Animation: Algorithms and Techniques" (2012) — comprehensive.
- Kavan, Žára "Spherical Blend Skinning" (2005) and DQS papers.
- Aristidou et al. "Inverse Kinematics: a review of existing techniques" (2018).
- Holden, Komura, Saito "Phase-Functioned Neural Networks for Character Control" (2017) — motion matching foundations.
- van den Berg, Lin, Manocha "Reciprocal Velocity Obstacles for real-time multi-agent navigation" (2008).
- Ekman & Friesen "Facial Action Coding System" (1978).

## Out of scope

- **Mocap capture pipeline** — Phase 8 cinematic integration.
- **Animation authoring UI** — Phase 7 editor.
- **Hardware skinning extension** (compute / mesh shader skinning) — Phase 3.5 modern rendering prologue.

## Open questions

- **Skinning at scale** — for crowd scenes (thousands of characters), GPU compute skinning is necessary. Whether this lands in this phase or Phase 3.5's GPU-driven amendment is a sequencing question.
- **Mocap retargeting determinism** — retargeting algorithms have known divergence across mocap pipelines (Maya HumanIK, Motionbuilder, Auto-rig Pro). Match `crd-eylem` v6's URDF/SDF/MJCF importer pattern: read source pose data deterministically; deliberate algorithmic choice (which retargeter to use) per-clip.
- **Animation file format** — proprietary (FBX, BVH, glTF) or custom? `crd-resources` cooker pattern says: import all, normalize to `'ANIM'` CRDR.

## Revisit triggers

This stub becomes a full phase plan when:
- Phase 3.1 eylem closes (for ragdoll / motion physics blending).
- `crd-ml-inference` (Phase 3.1.14) scoped (for motion matching).
- A specific consumer (game project, cinematic pipeline) makes animation an active priority.
