# DIAG.1a — scheduler free-list `next_free` races (DG05)

<!-- doc-role: historical -->

Owner slice: [DIAG.1a](../ROADMAP.md#slice-diag.1a). Contract: [runtime-diagnostics
design](../design/runtime-diagnostics.md#diag-1a); [ADR-0133](../decisions/0133-runtime-diagnostics-and-instrumentation.md).
DG05 transferred here from CORE-USE.2 (this is the primary repair owner; CORE-USE.2 later
verifies renderer integration, not a duplicate repair queue).

## The defect

Both scheduler pools are lock-free Treiber stacks whose head (`FiberPool::Tier::free_head`,
`CounterPool::m_free_head`) is a `std::atomic<u64>` packing `(index, generation)`; the
generation tag is bumped on every pop and defeats ABA. The **link** field, however, was a
plain non-atomic `crd::u32 next_free`:

- `engine/foundation/jobs/src/fiber.hpp` — `Fiber::next_free`
- `engine/foundation/jobs/src/counter.hpp` — `Counter::next_free`

`acquire()` reads `fibers[idx].next_free` between the head load and the CAS, while a
concurrent `release()` of that same node writes `next_free`. Even though the algorithm is
ABA-correct (a losing CAS discards the stale read), the plain load and plain store on the
same `next_free` object with no intervening synchronisation **are a C++ data race** — UB by
the standard, and exactly what TSan reports (DG05). "It works on x86" is not a fix.

## The repair

`next_free` is now `std::atomic<crd::u32>` in both structs, accessed with
`memory_order_relaxed` at all seven sites (`fiber_pool.cpp`: init/pop/push;
`counter.cpp`: init/pop/acquire-clear/push).

Why relaxed is sufficient — the linearization/ABA argument (documented in the source):

- **Ordering** is carried by the head, not the link. A pusher does a relaxed
  `next_free.store()` then a **release** CAS on the head that publishes both the new index
  and that store; a popper does an **acquire** load/CAS on the head that synchronises-with
  it, so the popper's subsequent relaxed `next_free.load()` observes the pushed successor.
- **Data race** is removed because atomic-vs-atomic accesses are never a data race
  regardless of order; relaxed is the minimum that keeps the fast path cheap.
- **ABA / uniqueness** are unchanged: the read successor is only installed if the CAS sees
  an unchanged `(index, generation)` head; every pop bumps the generation, so an interleaved
  pop+push restoring the same index cannot restore the same tag. A node is on the free list
  or acquired, never both. Linearization point is the successful CAS.

Atomic links were the design's named candidate — and here they are sufficient because the
reclamation model (fixed `unique_ptr<T[]>` backing store, never freed until shutdown, plus
the generation tag) already solves reuse/ownership. No node is ever deallocated while a
racer could touch it, so no hazard-pointer / epoch scheme is required.

`Fiber` gains an atomic member and so is no longer trivially assignable (correct: a live
pool descriptor must never be copied). Two test fixtures that reset via `= Fiber{}` now
reconstruct in place (`~Fiber(); new(&f) Fiber{};`).

## Proof (local, win-debug)

- `crd-jobs` + `crd-jobs-tests` build clean under `/W4 /WX /permissive-`.
- New adversarial stress tests (`[jobs][...][stress][diag]`), both green:
  - `fiber_pool: DG05 exhaustion/reclamation stress preserves uniqueness and completion`
  - `counter_pool: DG05 exhaustion/reclamation stress preserves uniqueness and completion`
  Each oversubscribes the pool (6 threads × batch 6 > 24 slots) so the free list is drained
  to empty and refilled for 6 000 iterations, with reverse/forward release order to churn
  link ordering. Invariants asserted: no `pool_index` held by two threads at once
  (uniqueness), acquired values survive round-trips, the pool is fully restored and the list
  is still exactly drainable afterward (completion).
- Full `jobs`/`counter`/`fiber` ctest set passes with no regressions.

The instrumented-path proof (TSan reports on the original code disappearing with **no
suppression**) is qualified on the hosted TSan lane by the next slice, [DIAG.1b](../ROADMAP.md#slice-diag.1b),
which owns the TSan preset/CI and the happens-before model with ordered and deliberately
racy negative controls.

## DIAG.1b — TSan preset + fiber happens-before positive controls

Landed alongside 1a: the `linux-clang-tsan` configure/build/test presets (the `CRD_ENABLE_TSAN`
`-fsanitize=thread` machinery and the `__tsan_*_fiber` model in `sanitizer_fibers.hpp` already
existed) and, in CI, `linux-clang-tsan` registered as a **diagnostic-tier** preset in
`.github/ci-tiers.json` (fixing the preflight "no tier owner" regression the preset first
introduced). Positive controls (`tests/foundation/jobs/test_tsan_fiber_model.cpp`,
`[jobs][tsan][diag]`) pass on win-debug: ordered write→wait→read across fibers, raw-thread/fiber
equivalence, and a migrating hand-off chain — all happens-before clean by construction, so under
the TSan lane they certify the model stays silent on correct orderings.

The negative control has since landed as well: `tests/support/diag/specimens/data_race_specimen.cpp`
(two threads racing a plain, unsynchronised int) built through the DIAG.0 specimen harness and driven by
`tests/foundation/jobs/test_tsan_race_specimen.cpp` (`[jobs][tsan][diag][harness]`). It asserts the
harness verdict is **SanitizerCaught** when built with `-fsanitize=thread` and **InstrumentAbsent**
otherwise — never a silent pass, mirroring the DIAG.0 ASan heap-overflow specimen. `specimen_common.hpp`
gained ThreadSanitizer detection (`CRD_DIAG_HAS_TSAN`, distinct from the ASan path). On win-debug the
verdict is InstrumentAbsent (green, 4 `[tsan]` cases). The only remaining piece is the hosted
linux-clang-tsan lane actually building the specimen with TSan and observing the catch.
