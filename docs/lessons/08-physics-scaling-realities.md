# Lesson 08 — Physics scaling realities: what "millions of particles" actually means

> **The question that motivated this lesson:** "Once we have the full GPU pipeline, we'll simulate physics of millions of particles at very small cost, right?"

The intuition is half right. The GPU LBVH cluster (`v9a-*`) makes one stage of physics — broadphase + spatial queries — scale spectacularly. But "physics" is a 3-stage pipeline, and **the LBVH is not the bottleneck for most realistic million-scale scenarios**. This lesson decomposes the question honestly so future architectural decisions don't overpromise based on broadphase numbers.

## TL;DR

Physics has three stages per frame:
1. **Broadphase** — find candidate collision pairs via spatial acceleration structure. LBVH/BVH refit. **This is what our v9a-* cluster accelerates.**
2. **Narrowphase** — compute exact contact points for each candidate pair (GJK, EPA, etc.).
3. **Constraint solve** — resolve all contacts simultaneously, iteratively converge to a penetration-free, momentum-conserving state.

**The constraint solver is the wall**, not the broadphase. For:
- **Particle effects** (no inter-particle contact): trivial, millions at 60fps. Our work doesn't matter here.
- **Fluid sim (SPH/MPM)**: ~1M particles at 30-60fps with our work directly enabling.
- **Rigid bodies, contact-rich**: real-time tops out at ~100K-200K dynamic bodies on consumer GPUs *regardless of broadphase speed*. 1M is research territory.
- **Static-dominated scenes** (1M static + 10K dynamic): **fully tractable** — this is the sweet spot our work unlocks.

## Part 1 — The three-stage physics frame

A modern rigid-body physics frame does:

```
For each timestep:
  Stage 1 — BROADPHASE
    Update each dynamic body's world AABB.
    Refit (or rebuild) the BVH over all body AABBs.
    Query the BVH for overlapping pairs.
    Cost: O(N log N) rebuild, O(N) refit, O(K) per pair where K = overlapping pairs.
    Our v9a-* cluster: handles up to 1M AABBs in ~5 ms on consumer GPU.

  Stage 2 — NARROWPHASE
    For each candidate pair from broadphase, compute exact contact info:
      contact_point, normal, penetration depth, friction surface
    Algorithms: GJK + EPA for convex-convex, mesh-vs-everything walks the BVH.
    Cost: O(K) where K = candidate pair count, with per-pair cost ~100-500 ns.
    Cerid: crd-geometry-convex (GJK/EPA shipped), crd-geometry-mesh (closest-point + raycast shipped).

  Stage 3 — CONSTRAINT SOLVE
    Assemble all contact constraints + joint constraints + friction constraints.
    Solve the linear complementarity problem (LCP) iteratively (e.g., Projected
    Gauss-Seidel, 8-16 iterations) until contact velocities + penetration converge.
    Cost: O(C × iters) where C = constraint count, iters = ~10-30.
    Cerid: eylem v1e (Sequential Impulse, not yet shipped).
```

Each stage scales differently with N (dynamic body count) and C (constraint count). The killer fact: **C scales super-linearly with N in contact-rich scenes**.

## Part 2 — Why constraint count grows faster than body count

Pile-up analogy: drop 1000 balls into a box. At rest, each ball touches ~6 neighbors plus the floor/walls. Contact count ~ 7000 = 7× the body count.

Drop 1M balls in. Each ball still contacts ~6 neighbors plus boundaries. Contact count ~ 7M = 7× body count.

Linear scaling? Looks like it. But in practice, sand-pile-class scenes have:
- Densely-packed contact regions where each particle has 10-12 contacts
- "Stacking towers" where each block has 4-8 contacts (top, bottom, 2-4 sides)
- Friction constraints (each contact contributes 1-2 friction directions)

Effective C/N ratios:
- Sparse scene (objects mostly not touching): C ≈ N × 0.5-2
- Mixed scene (some piles, some flying): C ≈ N × 2-5
- Dense pile (sand, gravel, debris): C ≈ N × 8-15

For 1M bodies in a dense scenario: 8-15M constraints. PGS solver at 10-30 iterations = 80-450M constraint updates per frame.

## Part 3 — The frame-budget math

At 60 Hz: 16.6 ms total budget per frame. Breakdown for a 1M-dynamic-body **dense scene**:

| Stage | Algorithm | 1M dense (8-15M constraints) | 1M sparse (1-2M constraints) |
|---|---|---|---|
| Broadphase rebuild | GPU LBVH (v9a-*) | ~5 ms | ~5 ms |
| Pair query | Tree walk + AABB tests | ~3-5 ms | ~1-2 ms |
| Narrowphase | GJK+EPA | ~5-10 ms | ~1-2 ms |
| Constraint solve | PGS, 15 iters | **~80-200 ms** | **~15-30 ms** |
| **Total** | | **~95-220 ms** (3-13× over budget) | **~22-39 ms** (1.3-2.4× over budget) |

Even with our broadphase reduced to zero, you can't fit a 1M dense-scene physics frame in 16.6 ms with current state-of-the-art constraint solvers. The solver is the wall.

**This is independent of our work.** The fastest known GPU rigid-body simulators (NVIDIA PhysX 5 GPU, AMD FleX, Bullet GPU) all hit the same wall around 100K-200K dynamic bodies for real-time on consumer hardware.

## Part 4 — What our work actually enables

The picture changes dramatically when the scene is **not dense rigid-body contact** but rather one of:

### Pattern A — Particle effects (no inter-particle contact)
Smoke, fire, sparks, sand-fall-without-pile-up. Each particle is updated independently:
- Position += velocity × dt
- Velocity += gravity × dt + external forces
- Maybe collide with environment SDF or a small kinematic set
- **NO constraints between particles**

This trivially runs millions of particles at 60fps. Our work doesn't matter — there's no spatial query needed if particles don't interact.

### Pattern B — Fluid simulation (SPH / MPM)
Per particle, find your K nearest neighbors, sum their kernel contributions, update velocity. Pure spatial-query workload. No constraints (well-posed with proper kernels).

- Frame cost = dominated by neighbor-finding = dominated by spatial sort
- Our LBVH-equivalent (Morton + radix on GPU) **is** the spatial query
- 1M particles at 30-60fps is realistic on a desktop GPU
- This is the workload our v9a-* cluster most directly accelerates

### Pattern C — Static-heavy scenes (1M static + 10K dynamic)
Open-world game: a million pieces of level geometry (walls, props, terrain triangles), 10K characters/projectiles/debris.

- Static BVH builds **once** at scene-load (parallel CPU radix → CPU LBVH → upload, or GPU LBVH if scene is GPU-resident).
- Static BVH is **never refit** — geometry doesn't move.
- Per-frame work: refit only the 10K-element dynamic BVH (~0.1 ms), then query the static BVH for each dynamic body's AABB (~0.5 ms total).
- Constraint count: 10K × ~3 contacts = ~30K — easily within solver budget.
- **Total physics frame: ~5 ms.** Fits 60fps with room.

This is the architecturally exciting case our LBVH unlocks. "Open-world physics with massive static geometry and ordinary dynamic counts" was effectively impossible before fast GPU BVH builds. Now it's just an implementation problem.

### Pattern D — Sparse rigid-body scenes (~100K dynamic, mostly not touching)
Asteroid field, flying debris, lightly-populated levels. C ≈ N × 1-2 = manageable.

- Broadphase: ~1-2 ms (our work)
- Narrowphase: ~1 ms
- Constraint solve: ~5-10 ms
- **Fits 60fps easily.**

100K-200K dynamic bodies in real-time. The current state-of-the-art ceiling, now achievable with our broadphase substrate.

## Part 5 — Where Cerid's eylem physics plan slots in

The eylem module (Phase 3.1, paused at v1b) is designed for multi-domain physics: games + robotics + medical viz + cinematic + DAW. Each domain has different scaling targets:

| Domain | Typical body count | Bottleneck | Real-time? |
|---|---|---|---|
| Games (typical) | 1K-10K dynamic + level geometry | Solver | Yes |
| Games (ambitious) | 100K dynamic + 1M static | Solver | Yes with our work |
| Robotics simulation | 100-1000 articulated | Articulation solver | Yes |
| Medical (soft tissue) | 100K-1M vertices | PBD / FEM iteration | Yes (offline-quality real-time) |
| Cinematic offline | 1M-10M dynamic | Solver | No (offline only) |
| DAW | 0 | n/a | n/a |

Our LBVH cluster (Phase 3.1.7 v9a) directly serves:
- ✅ Games (level broadphase, kinematic-vs-dynamic queries)
- ✅ Robotics (collision queries between robot links and environment meshes)
- ✅ Medical (sliding-contact queries between deformable tissue and tools)
- ⚠️ Cinematic (broadphase is fast; solver is still slow, but offline tolerates it)

What eylem v1c (broadphase) will look like, given our pipeline:

```cpp
// At scene-load:
StaticBvh static_bvh = build_static_bvh_gpu(level_geometry);   // once, ~50ms
DynamicBvh dyn_bvh   = empty();

// Per frame:
for (auto& body : dynamic_bodies) {
    body.update_world_aabb();
}
bvh_refit(dyn_bvh, dynamic_aabbs);                              // O(N), ~0.1ms for 10K

// Broadphase queries:
auto dyn_static_pairs = static_bvh.query_overlap_pairs(dyn_bvh); // ~0.5ms
auto dyn_dyn_pairs    = dyn_bvh.query_self_overlap();            // ~0.2ms

// Hand to narrowphase + solver (eylem v1d / v1e):
solve_constraints(dyn_static_pairs, dyn_dyn_pairs, dt);          // ~3-8ms
```

Total physics frame: ~5-10 ms. Fits 60fps. **Our work makes this composition possible** — without GPU LBVH or parallel CPU radix, the broadphase step alone would eat most of the frame budget.

## Part 6 — The honest verdict

When you say "millions of particles at very small cost," the answer depends on what you mean:

| You mean... | Reality |
|---|---|
| Particle effects, no inter-particle | ✅ Trivial, but our work isn't needed (no spatial query) |
| GPU fluid (SPH/MPM), 1M particles | ✅ 30-60fps achievable; our work directly enables |
| Million-body rigid-body simulation, dense contacts | ❌ Solver wall, not broadphase wall — research territory |
| Million **static** colliders + 10K dynamic | ✅ Our work unlocks this; modest physics frame budget |
| Million-vertex deformable (cloth/soft body) | ✅ PBD or XPBD on GPU; well-known territory |

**The most exciting realistic target** for our work is Pattern C: open-world physics with 1M+ static geometry and 10K dynamic bodies, all GPU-accelerated, at 60fps. That's a real architectural unlock — engines that don't have GPU LBVH cap out at ~100K static colliders before broadphase rebuild eats the frame.

What we're explicitly **not** unlocking with the v9a cluster: real-time million-rigid-body simulation. That waits on a new generation of GPU constraint solvers (PBD-style with Jacobi relaxation rather than Gauss-Seidel) which is its own research area and isn't on Cerid's roadmap.

## Part 7 — Why this matters for architectural decisions

When designing for a workload, ask:
1. **What's the dominant cost in this domain's frame?** (Not always what you'd guess.)
2. **What's the scaling ceiling regardless of how fast you make the bottleneck?**
3. **What's the realistic target for the next 12 months of consumer hardware?**

For Cerid's eylem v1c broadphase (planned next physics consumer):
- Dominant cost in typical game workload: constraint solver, NOT broadphase
- Scaling ceiling for real-time rigid-body: ~100K dynamic bodies (solver-bound)
- Realistic target: 10K dynamic + 1M static at 60fps (extends ambitious-game ceiling by ~10×)

The v9a-* GPU LBVH cluster's purpose is **to take broadphase off the bottleneck list**. After it lands, the next physics slice that's worth doing is the solver — not "more broadphase optimization." This is the architectural reason the v9a cluster exists.

## What to read next

- [Lesson 01 — Morton codes, radix sort, and the LBVH pipeline](01-morton-codes-and-lbvh-pipeline.md) — what we built and where it slots in.
- [Lesson 05 — CPU vs GPU performance tiers](05-cpu-vs-gpu-perf-tiers.md) — when GPU acceleration helps vs hurts.
- [Lesson 07 — Using radix and Morton in real consumers](07-using-radix-and-morton.md) — the eylem v1c broadphase recipe, concretely.
