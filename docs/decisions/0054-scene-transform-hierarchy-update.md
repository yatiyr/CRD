# ADR-0054 — Scene/ECS: Transform hierarchy update model

**Status:** Accepted
**Date:** 2026-05-06
**Tags:** scene, ecs, math, performance

---

## Context

`Transform` is the most-iterated component in the engine. Every renderable, light, audio source, physics body, UI node has one. The hierarchy update — propagating local transforms through parent-child relations to produce world transforms — runs every frame in `SchedulePhase::PreRender`.

At million-entity scale, this update must be:
1. **Cheap** for static entities (don't recompute what didn't change).
2. **Cache-coherent** in iteration (don't pointer-chase through hierarchy).
3. **Jobifiable** later when system count justifies it (don't bake serial assumptions in v1).
4. **Composable** with animation, physics, and authoring tools (don't lock the layout).

This ADR locks the transform layout, the dirty-propagation model, and the update timing.

---

## Decision

### 1. `Transform` component layout

```cpp
struct Transform
{
    crd::math::Vec3f translation = {0, 0, 0};
    crd::math::Quatf rotation    = crd::math::Quatf::identity();
    crd::math::Vec3f scale       = {1, 1, 1};

    // Cached world matrix, valid when world_dirty == false.
    // Updated by TransformPropagation system in SchedulePhase::PreRender.
    crd::math::Mat4f world       = crd::math::Mat4f::identity();
};
// 12 + 16 + 12 + 64 = 104 bytes. 64-byte aligned via Storage layer's chunk alignment.
```

- TRS authored separately (not as a single Mat4f). Lerp / blend / parent-rebase operate on TRS, not matrices. Animation interpolation, physics integration, networking interpolation all need TRS form.
- World matrix cached. Most queries that need world-space data (rendering, culling, audio attenuation) read the cached value; recomputing on demand would multiply work by the number of consumers.
- Local matrix is *not* cached. Reconstructing local matrix from TRS is cheap (3 multiplies, 1 quat-to-mat). The world matrix is the expensive product across the hierarchy and is the one worth caching.

### 2. Dirty flag stored separately, not in Transform

```cpp
// Stored as a parallel SoA array in the same archetype chunk as Transform.
// One byte per entity; iteration is cache-coherent.
struct TransformDirtyFlag
{
    crd::u8 self_dirty   : 1;   // local TRS modified since last propagation
    crd::u8 ancestor_dirty : 1; // some ancestor's transform changed; cascade required
    crd::u8 _reserved    : 6;
};
```

The flag lives outside `Transform` so the read path (`get world matrix`) doesn't have to read the dirty byte. Single-byte padding in the chunk is acceptable; SoA places dirty flags contiguous with each other across the chunk for fast bulk-clear.

### 3. Push-based dirty propagation

Setters mark `self_dirty` immediately. The `TransformPropagation` system, running once per frame in `SchedulePhase::PreRender`, walks the `ChildOf` reverse index (ADR-0051) in topological order:

```cpp
class TransformPropagation : public ISystem
{
public:
    using Reads  = ComponentSet<Relation<ChildOf>, TransformDirtyFlag>;
    using Writes = ComponentSet<Transform, TransformDirtyFlag>;

    SchedulePhase phase() const override { return SchedulePhase::PreRender; }

    void run(World& world, jobs::Counter& fence) override
    {
        // Walk roots, propagate down. Single-threaded in v1 (see §6).
        auto roots = world.query<Transform>().without_relation<ChildOf>();
        for (auto [e, t] : roots)
        {
            propagate_subtree(world, e, t, /*parent_world=*/Mat4f::identity(),
                              /*parent_dirty=*/false);
        }

        // Bulk-clear dirty flags after propagation.
        world.query<TransformDirtyFlag>().par_each(jobs::pool(), &fence,
            [](EntityId, TransformDirtyFlag& f) { f.self_dirty = 0; f.ancestor_dirty = 0; });
    }
};
```

The `propagate_subtree` recursion: if `parent_dirty || self_dirty`, recompute world matrix from `parent_world * local`, set `ancestor_dirty` on children. Otherwise skip the recompute.

### 4. Local matrix construction

```cpp
[[nodiscard]] crd::math::Mat4f Transform::local_matrix() const noexcept
{
    return crd::math::Mat4f::trs(translation, rotation, scale);
}
```

Computed on demand inside `propagate_subtree`. Not cached.

### 5. Setter API marks dirty

```cpp
class TransformAccessor   // returned by ComponentRef<Transform>
{
public:
    // Reads — no dirty mark
    const Vec3f& translation() const;
    const Quatf& rotation()    const;
    const Vec3f& scale()       const;
    const Mat4f& world()       const;   // returns cached value; UB if dirty (debug asserts)

    // Writes — mark dirty
    void set_translation(Vec3f);
    void set_rotation(Quatf);
    void set_scale(Vec3f);
    void set_local(Vec3f t, Quatf r, Vec3f s);   // bulk
    void set_world(const Mat4f& w);              // re-decomposes to TRS, marks dirty
    void translate(Vec3f delta);
    void rotate(Quatf delta);
    // etc.
};
```

All write paths flip `self_dirty = 1` and bump the chunk version counter (ChangeDetect, ADR-0053). Read of `world()` in debug asserts `!dirty` — caller wrote a setter and queried world in the same frame, before propagation. Standard pattern: query world after `PreRender` phase, not before.

### 6. Single-thread serial v1; jobified v2

`propagate_subtree` is recursive and trivially job-parallel: each subtree is independent. v1 ships single-threaded — the API is the same; the recursion is just sequential. Jobification (Phase 3.5+) splits subtrees across fibers when measurement shows transform propagation has crossed a budget threshold.

The transform propagation budget at 1 M entities of which ~10% change per frame (100 K dirty transforms): single-threaded is ~5 ms. Jobified across 8 cores: ~0.7 ms. Worth doing later; not worth the complexity now.

### 7. Interaction with `History` (ADR-0053)

`Transform` is registered with `History{60}` from day one. The history ring buffer records (translation, rotation, scale) — *not* the cached world matrix, which is recomputed from history during a rewind. Rewinding 3 frames means restoring TRS from `history[-3]` and marking all transforms `self_dirty`; next frame's `TransformPropagation` recomputes world matrices.

This means the history ring is `60 * (12 + 16 + 12) = 2400 bytes per Transform with history` — at 1 M entities, 2.4 GB if every transform tracks 60 frames. In practice we'll register a smaller window (`History{8}` is enough for typical interpolation; `History{60}` is for replay-debugging and gameplay rewind). Configurable per registration.

---

## Rationale

### Why TRS authored separately

Animation needs to lerp Vec3f translation, slerp Quat rotation, lerp Vec3f scale — none of which work on Mat4f form. Networking needs to send compressed TRS; matrix form bloats the payload. Physics needs to read translation + rotation as-is. Editor authoring shows TRS in inspectors, not 16-float matrices. Picking matrix-as-source costs every consumer extra work; picking TRS-as-source costs only the world-matrix consumers a small recompute, which is amortised by caching.

### Why cache world matrix

100 ms of vector math per frame at 1 M entities (recomputing world on every read) is too expensive. Caching trades 64 bytes per Transform for O(1) world-matrix reads. Memory cost is acceptable (64 MB at 1 M entities).

### Why push (eager) over pull (lazy) propagation

- Pull (recompute on first read): introduces query-time hitches; resists jobification (recursive on-demand vs pre-staged); harder to track which entities had a real change for downstream consumers (ChangeDetect breaks).
- Push (mark on write, propagate at sync point): eager; predictable cost; jobifies trivially; integrates cleanly with ChangeDetect.

Standard Unity, Unreal, Bevy choice.

---

## Consequences

- `Transform` is 104 bytes. Per-entity hierarchy overhead is `Transform + TransformDirtyFlag + Relation<ChildOf>` = 104 + 1 + 8 = 113 bytes.
- The `TransformPropagation` system runs once per frame in `SchedulePhase::PreRender`. Single-thread cost is roughly proportional to dirty subtree count, not total entity count.
- Animation systems write TRS via the setter API; `self_dirty` is set; next propagation pass recomputes world. No animation-specific propagation code needed.
- Physics systems write translation + rotation directly; same flow.
- History registration (`History{N}`) records TRS only. Rewinding restores TRS and marks `self_dirty` on the affected entities; the next propagation handles cascading.
- Editor uses `Transform.world()` for gizmo display — same caching path. Editor writes TRS, not world.

---

## References

- ADR-0020 — Scene & ECS hybrid (cornerstone)
- ADR-0049 — Entity identity (Layer 1)
- ADR-0051 — Relations (ChildOf provides hierarchy)
- ADR-0053 — Component index slot framework (ChangeDetect, History consumers)
- ADR-0021 — Animation architecture (TRS source-of-truth shared)
- `crd-math` Quat / Mat4 / Transform types (already shipped in Phase 1)
