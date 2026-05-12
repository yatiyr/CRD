# crd-scene — concurrency contracts

> Detour D-002 v1. The spec the D-002 v6 stress matrix tests against, and the
> rule sheet for anyone writing parallel code over the scene. Status quo as of
> Phase 3.0 close (v1h serial dispatch); the "future" notes track what v1h+1 /
> Phase 3.5 auto-parallel scheduling will change.

## The one-paragraph model

`crd-scene` is **single-writer**. There is no internal locking anywhere in
`World` or its storages, and none is planned — the design (ADR-0050/0052/0053)
puts structural change behind a deferral boundary instead. Concretely, at any
instant exactly one agent may *structurally mutate* the scene (spawn/destroy,
add/remove component, add/remove relation, register component/relation,
re-archetype, resize a pool). What runs in parallel is **iteration**: a system
walks chunks single-threaded and dispatches *one job per chunk*; each job reads
its chunk freely and may write the component values of *its own* entities
in-place — but may not change which entities exist or which archetype they're
in. Anything a job "wants to change structurally" it records into `Commands`,
which is replayed serially at the next phase boundary. TSan run against
arbitrary concurrent scene mutation will light up on UB that is simply illegal
use — the stress matrix tests the legal patterns only.

`World::for_each_chunk` says it plainly: *"Threading: not thread-safe. par_each
across yielded chunks is the expected parallel path (the visitor dispatches one
job per chunk)."*

## Legal concurrent patterns (today)

1. **Parallel read-only iteration.** Many fibers, each handed a distinct
   `ChunkView` by `Query::for_each_chunk` / a future `par_each`, reading
   component SoA arrays. Zero synchronisation. Precondition: no structural
   mutation is in flight (guaranteed inside a phase because mutations are
   deferred to the phase boundary).
2. **Parallel disjoint in-place writes.** Same shape, but jobs also write
   component *values* — each job only touches the entities in *its* chunk, and
   the chunks are disjoint memory. No archetype change, no insert/remove, no
   relation change. (Version-counter semantics for parallel `get_mut` are a
   v1h+1 detail — see "Open contract questions".)
3. **Recording into `Commands` from the running system.** A system body queues
   `destroy` / `add_component` / `remove_component` / `set_component` /
   `add_relation` / `remove_relation`; they're applied in registration order at
   the phase boundary, on the draining thread. Legal because **only one system
   runs at a time** (v1h serial dispatch).
4. **Concurrent reads of frozen setup data.** After the registration phase,
   `ComponentRegistry` (`info(id)`, masks, trait flags) is immutable — read it
   from anything.
5. **Per-fiber/per-job private allocators.** Allocators are
   single-threaded-by-contract (see D-002 detour doc); a job that needs scratch
   allocation uses its own arena (`jobs::frame_alloc`, or a worker-local
   `TlsfAllocator`), never a shared one.

## Illegal today (do not test these as if they should pass)

- Two agents structurally mutating concurrently — `spawn`/`destroy`,
  `add_component`/`remove_component`, `add_relation`/`remove_relation`,
  `register_component`/`register_relation`, or any path that re-archetypes,
  swap-removes, resizes a dense buffer, or rehashes an internal map.
- Calling `World::*` mutators from inside a `par_each` job. Queue into
  `Commands` instead — and note even *that* is single-`Commands`-per-`World`
  today (concurrent `Commands` append is **not** safe; v1h+1 adds per-fiber
  stripes).
- `World::destroy_immediate(e)` while any parallel iteration is in flight (its
  own doc-comment asserts this).
- Registering components/relations after the setup phase, or from a worker
  thread.
- Mutating an index's state (`AsyncAwareIndex::mark_loaded` / `mark_failed`,
  etc.) concurrently with a query that reads it — see "Open contract questions".

## Per-structure contract

| Structure | Owner | Concurrent reads | Concurrent structural mutation | Notes |
|---|---|---|---|---|
| `SlotMap` | `World` | OK while no allocate/free runs (`is_alive`, iteration) | **No.** `allocate`/`free` are single-writer. | `Commands::spawn()` calls `allocate()` *immediately* even from a system body — race-free **only because v1h dispatch is serial**. v1h+1 parallel `par_each` switches `spawn` to a deferred placeholder resolved at flush. |
| `ArchetypeChunkStorage` | `World` | `for_each_chunk` over disjoint chunks: OK. `has`/`get` (read): OK while no mutation. | **No.** `insert`/`remove`/`get_mut` re-archetype, move entities, swap-remove, bump version counters, fire the event sink. | Each `for_each_chunk` job owns one chunk → disjoint memory → in-place value writes across jobs are race-free. Archetype membership must not change during the walk. |
| `ArchetypeGraph` | `ArchetypeChunkStorage` | OK while no `insert` can create a new archetype/edge | **No.** New archetype/edge creation resizes/rehashes internal maps. | Inside a phase, `Commands` deferral guarantees no new archetypes appear mid-walk, so lookups of existing archetypes are safe in parallel. |
| `SparseSetStorage` | `World` | Read of a stable pool: OK while no mutation | **No.** `insert`/`remove` swap-remove (reshuffle dense order) and grow the dense buffer ×2. | Same "owns its pool / one writer" rule as the archetype backend. |
| `ComponentRegistry` | `World` | **Always OK after the setup phase** (`info`, masks, flags are frozen) | Setup-phase only; single-threaded; never during a parallel phase. | Uses `&ComponentTypeTag<T>::value` as the type key — no RTTI on the hot path. |
| `SharedComponentPool` | `SparseSetStorage::Pool` | Read of a shared entry: OK while no share-break | **No.** refcount inc/dec is a plain `u32` (not atomic; `CRD_ASSERT` on overflow); CoW break copies bytes into an owned slot — a structural mutation, goes through the single-writer path. | One pool per `InheritPolicy::Inherit` component. |
| Relations (`World::m_relations` / `RelationInfo` reverse indexes) | `World` | `traverse_relation` / reverse-lookup: OK while no relation mutation | **No.** `add_relation`/`remove_relation`/destroy mutate `HashMap<EntityId, Array<EntityId>>`. | Cycle checks (`would_form_cycle`) and on-target-destroyed cascades also touch these maps. |
| `Commands` | `World` (one buffer) | n/a (write-mostly) | Appended on the calling thread; **single producer today.** `spawn()` is immediate; the rest append `Array<Command>` + `Array<u8>` payload bytes. | Drained at every phase boundary in registration order on the draining thread. v1h+1: per-fiber stripes for parallel `par_each`. |
| `ChangeDetectIndex`, `AsyncAwareIndex` (and future `IComponentIndex`es) | `World` (registered) | Query operators (`.changed<T>()`, `.skip_pending<T>()`) read index state during iteration: OK while no mutation | Index state (`HashMap`s) is mutated by the storage event fan-out (fires from single-writer mutation paths) and `on_frame_begin/end` (called by `World::step`, single-threaded). | `mark_loaded`/`mark_failed` are the exception — see below. |
| `World` itself | the app's main thread | `is_alive`, `get_component` (read), `query<>()` construction & iteration: OK while no mutation | **No.** All the above plus `step`/`step_fixed`/`flush_destroys`. | "Not thread-safe" by its own doc. The frame loop owns it. |

## How the v2 `freeze()` / `FrozenView` guard fits

The per-structure rule "no structural mutation while a parallel iteration is in
flight" is exactly what D-002 v2's debug-only `freeze()` / `FrozenView` guard on
`crd::containers::Array` / `Vector` makes safe-by-construction: the storages'
backing `Array`s get frozen for the duration of a `for_each_chunk` / `par_each`,
and any mutating op (`push_back`, `resize`, swap-remove, …) asserts at the point
of misuse instead of corrupting a concurrent reader. When v2 lands, wire
`for_each_chunk` to freeze the relevant arrays around the visitor dispatch.

## What the D-002 v6 stress matrix will exercise

Only the legal patterns, hard, from many fibers:

- `for_each_chunk` read-only — N jobs summing/hashing disjoint chunks; oracle
  checks the world is unchanged afterwards.
- `for_each_chunk` disjoint in-place writes — each job writes an idempotent
  per-entity value into its chunk's component array; oracle verifies every
  entity's value between rounds (a job that wrote outside its chunk corrupts a
  neighbour → caught, seed printed).
- Single-producer `Commands` recording while a (serial) system runs, drained at
  the phase boundary; oracle verifies the structural delta is exactly the
  recorded set.
- Per-worker isolated allocators backing temporary per-job storage (covered by
  the v0 allocator/container stress tests; reused here).

It will **not** test concurrent `insert`/`remove`, concurrent
`register_component`, concurrent `Commands` append, or `destroy_immediate`
mid-iteration — those are illegal and a TSan red on them would be a false
positive, not a bug.

## Open contract questions (flagged for v1h+1 / a follow-up)

1. **`AsyncAwareIndex::mark_loaded` / `mark_failed` thread context.** The
   doc-comment says the caller flips state "when their async work completes
   (mesh GPU upload, audio decode, scripted-asset bind)". If that completion
   runs on a worker fiber and a query reads the index concurrently, that's a
   race on the index's `HashMap`. Proposed contract: async completions must
   *enqueue* a load-state change (like a `Command`) that the main thread applies
   at a phase boundary — `mark_loaded`/`mark_failed` are then main-thread-only.
   Needs a code check: is the async-upload path already wired this way, or does
   it call `mark_loaded` directly off-thread? If the latter, either move it
   behind a queue or give `AsyncAwareIndex` an internal SPSC/MPSC ingress.
2. **Parallel `get_mut` version-counter semantics.** `get_mut` bumps the
   chunk's per-component version (caller-declared write). If a future `par_each`
   has many jobs `get_mut`-ing the same chunk/component, the version bump is a
   contended write. The v1h+1 `par_each` design needs to define this — e.g. each
   chunk-job bumps once at the end, or version is per-chunk-job and merged.
   Until then: one job per chunk, and the job bumps version at most once.
3. **Per-fiber `Commands` stripes.** v1h+1 will let `par_each` jobs queue
   commands. The stripe design (one `Commands` buffer per worker fiber, merged
   in registration→fiber→sequence order at flush) is reserved; the public API
   (`world.commands()`, return type `EntityId` from `spawn`) doesn't change.
