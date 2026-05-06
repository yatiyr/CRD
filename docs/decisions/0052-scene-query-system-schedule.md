# ADR-0052 — Scene/ECS L4: Query · System · Schedule

**Status:** Accepted
**Date:** 2026-05-06
**Tags:** scene, ecs, arch, layer-4, query, scheduler

---

## Context

Layers 1–3 (Entity, Storage, Relations) describe the *data model*. Layer 4 describes how application code reads, writes, and orchestrates that data: the **query DSL**, the **system contract**, and the **schedule** that dispatches systems each frame.

The scale targets (millions of entities, jobified systems, scripts as bulk-iterated agents) constrain three design choices:

1. The query API must be composable (relation filters, change filters, spatial filters, history queries) and chunk-aware (`par_each` is a first-class operator, not an afterthought).
2. Systems must declare their component reads/writes so the scheduler can auto-parallelise without manual race-checking.
3. Mutation during parallel iteration must be safe — the canonical answer is **command buffers** with a phase-sync flush.

This ADR locks Layer 4 of the eight-layer architecture.

---

## Decision

### 1. Query DSL

Queries are expression-built ranges. The composer chains filters; the result is iterated via range-for or visited via `par_each`:

```cpp
auto q = world.query<Transform, Renderable>()    // base: archetype-walking pair query
            .with<Visible>()                     // require tag presence
            .without<Hidden>()                   // require tag absence
            .with_relation<ChildOf>(parent_id)   // relation filter (ADR-0051)
            .changed<Transform>()                // ChangeDetect index (ADR-0053)
            .skip_pending<Renderable>()          // AsyncAware index (ADR-0053)
            .in_aabb(camera_frustum);            // SpatialBVH index (ADR-0053; impl deferred)

// Serial iteration
for (auto [entity, transform, renderable] : q)
{
    submit(entity, transform, renderable);
}

// Parallel iteration: jobs pool dispatches one chunk per worker fiber
q.par_each(jobs, [](EntityId e, Transform& t, Renderable& r) {
    process(t, r);
});

// Time-tunneled query (History index, ADR-0053; impl deferred)
const Transform& prev = world.view<Transform>().at(e, -3);
```

The query type is opaque to callers — the chain returns `Query<...>` whose template parameters carry the filter set. C++20 concepts validate that `with_relation<X>` is only callable when `X` is a registered relation, etc.

### 2. Components and entities accessed via accessors, not raw pointers

```cpp
template <typename T>
class ComponentRef
{
    EntityId        entity;
    ComponentMask*  chunk_version;  // for ChangeDetect (incremented on write)
    T*              data;
public:
    operator const T&() const noexcept { return *data; }
    void set(const T& v) { *data = v; ++(*chunk_version); }
    // ... write methods bump chunk_version automatically
};
```

Reads are a plain dereference. Writes go through `set()` (or operator=, etc.) so the chunk version counter (Layer 5 ChangeDetect, ADR-0053) is bumped automatically. Manual bump available when needed (`commit_change(t)`); raw `T*` access is also exposed but documented as bypassing change tracking.

### 3. System contract

```cpp
class ISystem
{
public:
    virtual ~ISystem() = default;

    // Declared dependencies. Reads are co-readable across systems; Writes are exclusive.
    using Reads  = ComponentSet<>;   // overridden by derived class
    using Writes = ComponentSet<>;

    // Phase the system runs in (see §4).
    [[nodiscard]] virtual SchedulePhase phase() const = 0;

    // The actual work. May submit jobs to the passed counter for parallel work.
    virtual void run(World& world, jobs::Counter& fence) = 0;

    // Diagnostics
    [[nodiscard]] virtual crd::containers::StringView name() const = 0;
};
```

Concrete example:

```cpp
class TransformPropagation : public ISystem
{
public:
    using Reads  = ComponentSet<HierarchyNode, Relation<ChildOf>>;
    using Writes = ComponentSet<Transform>;

    SchedulePhase phase() const override { return SchedulePhase::PreRender; }

    void run(World& world, jobs::Counter& fence) override
    {
        auto roots = world.query<Transform>().without_relation<ChildOf>();
        roots.par_each(jobs::pool(), &fence, [&](EntityId root, Transform& t) {
            propagate_subtree(world, root, t.local_matrix());
        });
    }

    crd::containers::StringView name() const override { return "TransformPropagation"; }
};
```

### 4. Schedule phases

Five fixed phases run in order each frame:

```cpp
enum class SchedulePhase : crd::u8
{
    PrePhysics,     // input, AI decisions, physics-event preparation
    Physics,        // physics step (Phase 3.1+)
    PostPhysics,    // physics result reactions, contact-driven gameplay
    Update,         // general gameplay logic
    PreRender,      // transform propagation, animation pose, culling, GPU upload prep
    RenderExtract,  // build draw lists from current world state
    PostRender,     // editor diagnostics, debug overlay, replay capture
};
```

Within a phase, systems run in registration order in v1. Auto-parallel scheduling within a phase (dispatch systems whose Reads/Writes don't conflict in parallel) is reserved API; implementation defers to Phase 3.5 when system count justifies it. Until then the analysis runs but only validates correctness — not parallelisation.

### 5. Command buffers for parallel mutation

A system iterating in parallel must not mutate the entity-component set directly (other workers may be reading the same data). The standard pattern:

```cpp
void run(World& world, jobs::Counter& fence) override
{
    auto deads = world.query<Health>().filter([](const Health& h) { return h.value <= 0; });

    deads.par_each(jobs::pool(), &fence, [&](EntityId e, const Health&) {
        // CANNOT call world.destroy(e) here — would race with other workers.
        world.commands().destroy(e);          // queued; flushed at phase end
        world.commands().add_component<Corpse>(e, {});
        world.commands().spawn_relation<EmittedFrom>(spawn_particle_system(), e);
    });
}
```

Each worker fiber has a thread-local command buffer. At phase sync (after `fence.wait()`), the World drains all command buffers in registration order and applies them serially. Single-threaded apply is fine — command-buffer apply is fast (it's just storage mutations) and rarely the bottleneck.

Operations supported in command buffers:
- `spawn(...)` / `destroy(e)`
- `add_component<T>(e, value)` / `remove_component<T>(e)`
- `set_component<T>(e, value)` — overwrite
- `add_relation<Tag>(src, target)` / `remove_relation<Tag>(src)`

### 6. View into the world

`World` exposes:

```cpp
class World
{
public:
    // Query construction
    template <typename... Cs> auto query();
    template <typename T>     auto view();              // shorthand for single-component

    // Single-entity ops (sequential, immediate)
    EntityId spawn();
    void     destroy(EntityId e);                       // deferred to flush
    void     destroy_immediate(EntityId e);             // assert no parallel iteration

    template <typename T> void add_component(EntityId, T);
    template <typename T> void remove_component(EntityId);
    template <typename T> bool has_component(EntityId) const;
    template <typename T> T*   get_component_mut(EntityId);
    template <typename T> const T* get_component(EntityId) const;

    // Bulk ops (jobified)
    Commands& commands();                               // thread-local handle

    // Schedule control
    void register_system(std::unique_ptr<ISystem>);
    void step(crd::f64 dt);                             // runs all phases once
    void step_fixed(crd::f64 fixed_dt, crd::u32 max_substeps); // determinism

    // Slot map
    [[nodiscard]] bool is_alive(EntityId) const noexcept;
    [[nodiscard]] crd::u32 entity_count() const noexcept;
};
```

### 7. Determinism mode

`world.step_fixed(dt, max_substeps)` runs systems with a fixed-point delta time. Relevant for physics (Phase 3.1), networking rollback (Phase 4.2), and simulation replay (Phase 8). Variable-rate cosmetic systems (camera smoothing, animation interpolation) use the variable `dt`; fixed-step systems opt in via `phase()` annotation:

```cpp
class PhysicsStep : public ISystem
{
    SchedulePhase phase() const override { return SchedulePhase::Physics; }
    bool          fixed_step() const override { return true; }   // opt-in
    crd::f64      fixed_dt()   const override { return 1.0 / 60.0; }
    // ...
};
```

`step_fixed` accumulates real-time delta and runs fixed-step systems N times where N is `floor(accumulator / fixed_dt)`. Variable systems run once. Standard Bevy `FixedTimestep` / Unity `FixedUpdate` shape.

---

## Rationale

### Why declare Reads/Writes at the type level

Auto-parallel scheduling needs to know which systems conflict. Declaring at the type level (vs runtime registration) gives compile-time validation: a system's `run()` body can be checked against its declared sets in debug builds (assert-only — no runtime cost in release). It also makes the dependency graph statically inspectable for tooling.

### Why phase-fixed schedule

Fully data-driven scheduling (auto-extract phases from system dependencies) is what Bevy does today and is genuinely powerful at 50+ systems. Cerid will have ~5–15 systems through Phase 3.5; the cost-benefit doesn't pay off until later. Fixed phases match Unity / Unreal mental model and ship in two days, not six. The Reads/Writes machinery is in place so we can switch to auto-extract in Phase 3.5 without API changes.

### Why command buffers

The only safe alternative to command buffers in parallel iteration is locking, which serialises everything we just parallelised. Command buffers are the universal answer — Bevy, Unity DOTS, EnTT, Flecs all use them. Per-worker buffers eliminate contention; serial flush is a non-issue at our scale (drains in <0.1 ms for typical workloads).

---

## Consequences

- The query DSL is the single point of contact between systems and the storage layer. New filter operators (added later as Indexes are implemented — `at_frame`, `in_aabb`, `gpu_resident_only`) extend the DSL without breaking existing queries.
- Systems are virtual classes. Their dispatch overhead is ~5 ns per system per frame — invisible.
- The schedule is opaque: callers register systems and call `step(dt)`; phase order is fixed; ordering within a phase is registration order. v1 ships without auto-parallel; the API is in place for v2 to enable it without a breaking change.
- Command buffers add ~24 bytes per worker for the buffer, plus N * sizeof(deferred-op) per frame's queued commands. Negligible.
- `step_fixed` provides deterministic-mode entry. Networking rollback and sim-replay (Phase 4.2 / Phase 8) consume this directly.

---

## References

- ADR-0020 — Scene & ECS hybrid (cornerstone)
- ADR-0035 — Networking architecture (consumes determinism mode)
- ADR-0049 — Entity identity (referenced)
- ADR-0050 — Storage backends (chunks are the par_each work unit)
- ADR-0051 — Relations (relation operators in DSL)
- ADR-0053 — Component index slot framework (filters are index-driven)
- Bevy ECS scheduler documentation (Reads/Writes auto-parallel)
- Unity DOTS Job Components / IJobChunk
