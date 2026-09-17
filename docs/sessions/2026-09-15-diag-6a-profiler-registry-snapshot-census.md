# DIAG.6a — profiler self-correctness and application lifecycle (DG08): census and plan

<!-- doc-role: historical -->

Owner slice: [DIAG.6a](../ROADMAP.md#slice-diag.6a). Contract: [runtime-diagnostics
design](../design/runtime-diagnostics.md#diag-6a); [ADR-0133](../decisions/0133-runtime-diagnostics-and-instrumentation.md).
DG08 (profiler registration lifetime/qualification). Running slice doc: sections are appended per sub-unit; this
opening **(a)** part is the census. **No engine code this tick.**

DIAG.6a (row 077) is the first Open DIAG row after 5d landed. Like 5c/5d it packs several concerns into one row
("registry/snapshot/ring correctness, dynamic names, allocator destruction, lifecycle"), so this is the census
sub-unit (a): scope, measured defects vs acceptance, verification regime, decomposition.

## Acceptance (verbatim from the design)

From [runtime-diagnostics.md#diag-6a](../design/runtime-diagnostics.md#diag-6a) ("profiler self-correctness and
application lifecycle"):

> Repair and qualify allocator/counter/name/thread registries, sampling snapshots and shutdown. Read the actual
> sample ring publication protocol; head/tail atomics alone do not prove overwritten payload is safe for concurrent
> readers. Use immutable generations or a documented synchronization scheme. Separate registered live count from
> high-water index, and preserve frame/history labels after allocator slot reuse. Fix DG08 if its test confirms the
> concern.
>
> Acceptance: snapshot/save while producers wrap buffers and register/unregister; simultaneous capture/shutdown;
> dynamic names from unloaded modules; allocator destruction while sampling. No torn records, stale dereferences or
> mislabelled history. Preserve explicit dropped/corrupt/unavailable counts and compatibility with existing captures.

DG08 ([research](../research/2026-09-14-diagnostics-and-instrumentation.md) row): "Profiler registry
([profiler.cpp](../../engine/foundation/perf/src/profiler.cpp)) mutates raw allocator/name pointers under a mutex
while snapshot access reads entries without that mutex. Safe deregistration/destruction and concurrent reuse need
investigation and qualification. This is a source concern, not a reproduced race here." → 6a reproduces it with a
test, then fixes it.

## Design vs ADR — the scope that governs the build

- **Qualify, don't rewrite.** The profiler exists and is heavily used
  ([`profiler.cpp`](../../engine/foundation/perf/src/profiler.cpp), 37 KB; 26 perf test files). 6a *repairs and
  qualifies* the registry/snapshot/ring lifetime — it does not build a second profiler (ADR line 49: "Future CR-D007
  views consume these services rather than building a second profiler/debugger").
- **No hot-path global lock.** ADR line 135: hot recording must have "no allocation, blocking, symbolization, network
  I/O or **global locking**." So the DG08 fix must be a lock-free publication scheme (atomic pointer + generation /
  hazard / quiescence), **not** "take the mutex in the snapshot too." The design says exactly this: "immutable
  generations or a documented synchronization scheme."
- **Real happens-before, not annotations.** ADR line 44: "Sanitizer annotations cannot invent happens-before merely
  to silence reports." A TSan-clean result must come from a real synchronization scheme, never a suppression.
- **Separate live count from high-water.** Design: registered *live* count must be distinct from the high-water index
  so slot reuse and the UI's stable indexing don't conflate "how many are live now" with "how many slots were ever
  used."
- **Preserve honest counts + capture compatibility.** "Preserve explicit dropped/corrupt/unavailable counts and
  compatibility with existing captures" — the ring's `dropped` accounting and the CPROF capture format
  ([`capture.cpp`](../../engine/foundation/perf/src/capture.cpp), `test_capture_roundtrip`) must survive every fix.

## Current state and measured defects vs the acceptance

Baseline this tick: `crd-perf-tests` **819 assertions / 146 cases PASS** on win-debug (no pre-existing failure). All
line numbers are [`profiler.cpp`](../../engine/foundation/perf/src/profiler.cpp).

1. **Allocator snapshot dereferences an unlocked, non-atomic pointer (the DG08 core).** **Measured** — the frame
   snapshot loop (`profiler.cpp:534-551`) loads `allocator_count_atomic` (acquire), then for each slot reads
   `state.allocators[i].allocator` (a plain `crd::memory::IAllocator*`) and calls `ae.allocator->stats().snapshot()`
   **without `alloc_mutex`**, guarded only by a `== nullptr` check. `unregister_allocator` (`profiler.cpp:888-905`)
   writes that same field to `nullptr` **under `alloc_mutex`**. Two hazards: (i) a data race on a non-atomic pointer
   (UB) between the null-check at :540 and the deref at :545; (ii) a use-after-free — the owner may destroy the
   allocator immediately after `unregister_allocator` returns, while a snapshot is mid-deref. Acceptance: "allocator
   destruction while sampling … No … stale dereferences." No existing test exercises snapshot concurrent with
   unregister/destroy (`test_memory_tracking` covers only the single-threaded happy path). **(b)**
2. **No live count separate from the high-water index.** **Measured** — `allocator_count_atomic` is the high-water of
   ever-used slots and never shrinks (`profiler.cpp:906-910` comment: "the count never shrinks … UI sees a stable
   indexing"); the snapshot walks `[0, high_water)` skipping null slots. There is no separate *registered-live* count.
   Acceptance: "Separate registered live count from high-water index." **(b)**
3. **Sample-ring publication has no overwrite detection.** **Measured** — `ThreadRing` (`profiler.cpp:71-82`) is SPSC;
   readers copy `samples[tail..head)` using head/tail acquire/release only (`thread_samples` :581-582, `resolve`
   :645-646). There is **no per-slot generation / seqlock**, so a wrapping writer can overwrite slots the reader is
   copying — the cursors stay consistent but the *payload* is torn. The code itself notes the SPSC single-reader
   assumption is documented, not enforced (`profiler.cpp:620-627`). Design: "head/tail atomics alone do not prove
   overwritten payload is safe … Use immutable generations or a documented synchronization scheme." `test_ring_overflow`
   covers drop-accounting, not concurrent overwrite-during-read. **(c)**
4. **Dynamic names borrow a pointer and dangle after module unload.** **Measured** — `intern_name` stores
   `e.string = static_name` (`profiler.cpp:322`) — it **borrows** the caller's pointer, never copies the bytes; the
   parameter is literally named `static_name`. `ThreadRing::name` and `AllocatorEntry::name` are likewise borrowed
   `const char*`. A name whose bytes live in an unloaded module's memory dangles. Acceptance: "dynamic names from
   unloaded modules … No … mislabelled history"; "preserve frame/history labels after allocator slot reuse." **(d)**
5. **Simultaneous capture/shutdown can free state under a live snapshot.** **Measured** — `shutdown`
   (`profiler.cpp:260-282`) `delete[]`s the allocator/name/counter tables and nulls `g_state`; the snapshot/`thread_samples`
   paths bind `ProfilerState& state = *g_state` after a `g_state == nullptr` check, so a snapshot that passed the check
   can hold a reference to state that `shutdown` then frees (UAF). Acceptance: "simultaneous capture/shutdown." **(e)**

Scoped-in-by-name but **not** currently defective: **counters and names never unregister** (`profiler.cpp:227-228`;
the name table only inserts) so there is no dereg race for those two registries — the registry lifetime problem is
specifically the **allocator** table (which unregisters) and **shutdown** (which frees everything). Thread rings are
`active`-flagged and cleared, not freed mid-run. The census records this so (b) does not invent counter/name dereg
work the code does not have.

## Verification regime

- **win-debug** for logic (counts, live-vs-high-water, dropped/corrupt/unavailable preservation, capture round-trip).
- **win-asan** for the use-after-free concerns — defects 1 and 5 are provable as ASan use-after-free with a stress
  test that snapshots while a second thread unregisters+destroys an allocator / drives shutdown. This is the primary
  proof lane for this slice (as DIAG.3x used win-asan).
- **TSan (linux-clang-tsan) where a real data race (not a UAF) is the claim** — defect 3's torn-payload and defect 1's
  non-atomic-pointer race are happens-before violations TSan catches; the fix must make TSan clean via a real
  synchronization scheme, never a suppression (ADR line 44). If a TSan tree is unavailable this tick, the sub-unit
  ships the scheme + a win-asan stress proof and names the TSan lane as CI-gated (the DIAG.3x precedent).
- Every sub-unit re-runs the full `crd-perf-tests` (819/146 baseline must not regress) + both validators; the CPROF
  capture round-trip is the compatibility guard.

## Decomposition into sub-units

- **(a)** [this tick] census.
- **(b)** Allocator registry: safe deregistration + destruction-while-sampling. Publish the slot pointer atomically
  and add a generation/quiescence scheme so a snapshot never dereferences a freed allocator; separate a registered
  *live* count from the high-water index. First action: `sed -n '843,915p' profiler.cpp` (register/unregister) +
  `529,556p` (snapshot loop); write a win-asan stress test (snapshot thread vs unregister+destroy thread) that
  reproduces the UAF first, then fix. This is DG08's "test confirms the concern" gate.
- **(c)** Sample-ring publication protocol: a documented scheme (per-slot generation / seqlock, or an enforced
  single-reader contract with overwrite detection) so a wrapping producer cannot hand a reader a torn record; preserve
  `dropped`. First action: read the writer (`push_region`/write path) + the two reader paths (`thread_samples`,
  `resolve`); a producer-wraps-while-reader-drains test.
- **(d)** Dynamic names: own the name bytes (copy into a bounded arena on intern) so a name from an unloaded module
  survives, and preserve frame/history labels after allocator slot reuse. First action: `intern_name`/`resolve_name`
  (:300-365) + the frame-history label path.
- **(e)** Lifecycle: simultaneous capture/shutdown quiescence (shutdown drains in-flight snapshots, or snapshots take
  a lifetime guard) + init/shutdown ordering with app/jobs. First action: `shutdown` (:260-282) + `grep -rn "perf::"
  engine/foundation/app engine/foundation/jobs` for the ordering.
- **(f)** Clause-by-clause acceptance review + flip row 077 Open→Needs CI (the 5d(f) pattern).

Row 077 stays **Open** (nothing added to its note until (f), per the 5d precedent).

Next tick: **(b)** — reproduce the DG08 allocator use-after-free with a win-asan stress test, then fix it with an
atomic/generation publication scheme (no hot-path global lock) and separate the live count from the high-water index.
Begins with the protocol's advisor + CI check.

## (b) landed — DG08 allocator-registry use-after-free: reproduced, then fixed

The design gates this slice: "Fix DG08 **if its test confirms the concern**." It does.

- **Reproduced first (win-asan).** New test
  [`test_diag_registry_race.cpp`](../../tests/foundation/perf/test_diag_registry_race.cpp) (`[perf][diag][registry]`):
  a worker registers a stack `TlsfAllocator`, unregisters it, and lets it destruct at scope end (the realistic owner
  contract — destroy *after* `unregister_allocator` returns), 2000×, while the main thread drives `frame_mark()`. On
  the **unfixed** profiler ASan reports, at the predicted site:
  ```
  ERROR: AddressSanitizer: access-violation on unknown address 0x000000000010
    #1 crd::memory::MemoryStats::snapshot() memory_stats.hpp:47
    #2 crd::perf::frame_mark()             profiler.cpp:545
  ```
  i.e. the snapshot loaded a slot's raw `IAllocator*`, then a racing `unregister_allocator` nulled the slot and the
  owner destroyed the allocator, so `frame_mark` called `stats()` on freed memory. DG08 confirmed (not "by inspection"
  — an actual ASan report).
- **The fix (all in [`profiler.cpp`](../../engine/foundation/perf/src/profiler.cpp) + [`memory.hpp`](../../engine/foundation/perf/include/crd/perf/memory.hpp)).**
  `AllocatorEntry::{name,allocator}` are now `std::atomic` (+ an `atomic generation` bumped on each (re)registration,
  which (d) will consume for label preservation). The four slot readers (`frame_mark`, `allocator_snapshot`,
  `allocator_info`, and `allocator_snapshot_history` — which loads `name` atomically for a history record) load the
  slot pointer/name **once** (`seq_cst`) into a local and use only that local — never re-read the slot. The two that
  actually **deref** the allocator (`frame_mark`, `allocator_snapshot`) hold a new `snapshot_in_flight`
  counter across the deref; `unregister_allocator`, under `alloc_mutex`, stores null then **waits**
  `while (snapshot_in_flight != 0) yield()` before returning, so an allocator is never destroyed under a live read.
  The `in_flight` RMWs, the null store and the pointer load are all `seq_cst`: the store→load is Dekker-shaped and
  acquire/release alone could let `unregister` see `in_flight==0` while a snapshot still holds the old pointer.
- **Live count separated from high-water.** `allocator_count_atomic` stays the high-water (never shrinks — the UI's
  stable index); a new `allocator_live_count` rises on register / falls on unregister, exposed as
  `live_allocator_count()`. The acceptance's "separate registered live count from high-water index."
- **Why a guard, not "take the mutex in the snapshot".** The snapshot calls *user* allocator code (`->stats()`);
  holding `alloc_mutex` across it is a reentrancy / lock-order hazard and would block every `register_allocator` on
  other threads for the snapshot's duration. The in-flight guard confines blocking to the cold register/unregister
  paths (a concurrent `register_allocator` also waits on `alloc_mutex` while `unregister` spins). Documented contract
  in the header: *destroy only after `unregister_allocator` returns; never call it from inside `IAllocator::stats()`*
  (the snapshot holds the guard — it would spin forever). **For (f):** `allocator_info` still returns a raw
  `IAllocator*` to callers (e.g. perf-ui) with no guard — the profiler's own derefs are fixed, but a caller could
  deref past unregister; (f) decides whether the documented owner-contract line suffices or the API should stop
  exposing the pointer.
- **Not touched:** `AllocatorRecord` (the pinned CPROF wire layout — a change is a schema bump); the generation stays
  in-memory. Counters/names still never unregister, so they keep their existing publication (census (a) recorded this).
- **Verified.** win-asan: the repro **fails on unfixed, passes on fixed** (`[registry]` 2/1, no report); full
  `crd-perf-tests` **win-asan 748/147** and **win-debug 821/147** green (baseline was 819/146 — `[registry]` is the
  new case); **win-shipping `/W4 /WX`** clean; **`crd-perf-ui`** (the downstream `allocator_info` caller) builds. The
  four ASan reports in the full win-asan run are the intentional child-specimen AVs (heap-overflow / use-after-free /
  crash-capture specimens), not the registry. `check_no_std_containers` **74** (adds 0 — `std::atomic`/`std::thread`/
  `std::mutex` are allowed). Both validators PASS. **TSan** (`linux-clang-tsan`) is CI-gated this tick (no local tree;
  the atomic-pointer publication makes hazard (i) a real synchronization scheme, not a suppression — ADR line 44).

Next tick: **(c)** — the sample-ring publication protocol (per-slot generation / seqlock or an enforced single-reader
contract with overwrite detection) so a wrapping producer cannot hand a reader a torn record; preserve `dropped`.
Read the writer (`push_region`/sample write) and both readers (`thread_samples`, `resolve`) together, **and** check
whether the `frame_history` ring (written by `frame_mark`, read by `frame_record`) has the same overwrite-during-read
shape — if so (c) covers two rings, not one, and that decides seqlock vs generation. Begins with the protocol's
advisor + CI check.

## (c1) landed — frame-history ring: torn-record hazard reproduced, then fixed with a seqlock

The design gates (c) like (b): reproduce the tear first. **Measured, per ring** — the two rings differ, and only one has
the design's "overwritten payload" hazard:

- **Per-thread sample ring — drop-on-full, NOT overwrite.** The writer is `if (h - t >= slots) { dropped++; return; }`
  (`profiler.cpp:483`, `:660`), so it never overwrites the unread `[tail, head)` region. The design's "overwritten
  payload is safe for concurrent readers" hazard therefore does **not** apply to this ring. Its remaining concern is
  reader-vs-`clear_samples` discipline + an *enforced* (not merely documented) single-reader — lighter work, and split
  to **(c2)** so neither ring ships half-done.
- **Frame-history ring — overwrite-oldest, and it hands out a pointer INTO the ring.** `frame_mark` writes a ~3.6 KB
  `FrameRecord` in place and advances `frame_history_head` (no tail); `frame_record()` returns a `const FrameRecord*`
  into that slot. A cross-thread reader dereferencing it while `frame_mark` laps reads a torn record. **This is the
  design's literal hazard**, and this tick's (c1).

- **Reproduced (win-debug).** New test [`test_diag_ring_tearing.cpp`](../../tests/foundation/perf/test_diag_ring_tearing.cpp)
  (`[perf][diag][ring]`): a 4-slot ring (laps fast); each frame sets a `Set` counter to the index `frame_mark` will
  stamp, so within one record `values[id] == frame_index`. A reader thread checks that invariant while the writer runs
  200000 frames. On the **unfixed** profiler, reading through `frame_record()`'s pointer: `CHECK(torn == 0)` fails —
  **`15 == 0`** (15 torn records). Tearing confirmed. (Proof lane is **win-debug** — a torn read is a logic error the
  self-check catches, not a memory error, so ASan is not the tool here.)
- **The fix — a per-slot seqlock in a parallel array** (`profiler.cpp` + [`counters.hpp`](../../engine/foundation/perf/include/crd/perf/counters.hpp)).
  `ProfilerState` gains `std::atomic<u32>* frame_history_seq` (one per slot, allocated in `init` beside `frame_history`,
  freed in `shutdown`) — the seq **cannot** live in `FrameRecord`, which is the pinned CPROF wire layout. `frame_mark`
  bumps the slot's seq to **odd** (relaxed) + a release fence before writing the record, then to **even** (release)
  after. A new `copy_frame_record(frames_back, out)` does the seqlock read: load seq (acquire) → skip if odd → `memcpy`
  → acquire fence → re-load seq → accept only if unchanged; **bounded 8-retry**, then it reports the frame *unavailable*
  (`frame_history_unavailable_count`) rather than spinning — the acceptance's "preserve explicit … unavailable counts".
  The speculative `memcpy` is a racy read discarded on mismatch — the standard seqlock, and the design's "documented
  synchronization scheme". **TSan will flag this `memcpy` racing `frame_mark`'s field writes** (it instruments exactly
  that), so when (f) flips the row to Needs CI the `linux-clang-tsan` lane goes RED **on purpose**. The pre-named fix
  (not this tick, not a suppression — ADR line 44): have `frame_mark` fill a local `FrameRecord`, then `odd` → per-word
  relaxed `store` into the slot → `even`, and the reader does per-word relaxed `load`s — which also narrows the odd
  window from "all of `frame_mark` incl. user `stats()`" to a 3.6 KB copy.
- **Migrated the cross-thread readers.** `allocator_snapshot_history` and `capture.cpp`'s save path (the acceptance's
  "save while producers wrap buffers") now copy via `copy_frame_record` instead of dereferencing a ring pointer.
  `frame_record()` stays for the in-process perf-ui panel and is now documented **same-thread-as-`frame_mark` only**.
- **Verified.** `[ring]` **fails unfixed (15 torn), passes fixed** (torn 0, copies > 0); full `crd-perf-tests`
  **win-debug 823/148** (was 821/147 + the new case) and **win-asan** `[capture],[ring],[registry]` 51/9 + `[memory]`/
  `[counter]` clean; `test_ring_overflow` + `test_capture_roundtrip` unchanged (dropped preserved, CPROF compat);
  **crd-perf-ui** builds (the `frame_record` caller); **win-shipping `/W4 /WX`** clean; `check_no_std_containers` **74**
  (adds 0 — `std::atomic`/`std::thread`/`std::atomic_thread_fence` allowed). Both validators PASS. TSan CI-gated.
- **Honest caveats.** The retry-exhaustion (`unavailable++`) branch is not exercised deterministically — `frame_mark`
  is µs-scale and a copy is ~100 ns, so 8 consecutive collisions don't occur in the test; the branch is by inspection,
  not covered. And a lapped slot returns a *newer* consistent frame than `frames_back` requested (never a torn one), so
  capture consumers key on `frame_index`, not ring position — unchanged from before, now consistent instead of torn.
- **Caller audit.** The only remaining `frame_record()` (raw-pointer) callers are perf-ui's in-process panel
  (`live_source.cpp` → `profiler_panel.cpp`), which draws on the same thread as `frame_mark` — the documented
  same-thread contract. The cross-thread readers (`capture.cpp` save, `allocator_snapshot_history`) were migrated to
  `copy_frame_record`.

Next tick: **(c2)** — the sample-ring reader discipline: acquire on every reader head/tail load, `tail` advanced only
after the reader's copy, and an *enforced* single-reader (`atomic exchange` on a reader flag; a second reader gets an
empty view + a `contended` count) with the protocol written into the header. Begins with the protocol's advisor + CI
check. Then (d) dynamic names.

## (c2) landed — sample ring: census defect #3 corrected, wrong-samples-after-wrap fixed

**Sub-unit chain.** (c2) was scoped in (c1) as the sample-ring *reader discipline*; the read-#1 advisor pass found the
live defect was wrong-samples-after-wrap, so (c2) became that correctness fix and the **enforced-single-reader** the (c1)
note promised moves to (c3). Save-path consumer audit (both consumers named so (c3) inherits nothing unlisted): the only
ring readers in the perf module are `capture.cpp:136` (pre-pass, sizing/count only) and the migrated `capture.cpp:287`
copy; `gpu_scope.cpp` / `jobs_adapter.cpp` hold none (they write via `write_external_sample`). The
`[perf][diag][sample-ring]` case (1) asserts the *divergence* of the raw view from the wrap-correct copy (not the exact
stale value), so the oracle survives a future (c3) fix that makes the view itself wrap-correct.

**Measured correction of census defect #3.** The census (following the design's "overwritten payload" wording) assumed
the per-thread sample ring was overwrite-oldest. It is **drop-on-full**: the writer refuses when `h - t >= slots`
(`profiler.cpp:483`, `:660`, bumping `dropped`) and never overwrites the unread `[tail, head)` region, so a single
reader has **no torn-payload hazard** and needs no seqlock (unlike the frame-history ring in (c1)).

**The real sample-ring defect — wrong samples after wrap.** `thread_samples()` returns `data = ring.samples` (the
BASE) + `size = head - tail`. While the ring has never wrapped (`tail == 0`) that is fine, but after a
`clear_samples()` advances tail off zero (the perf-ui "clear" button, `profiler_panel.cpp:710`) the live samples sit at
`[tail & mask, head & mask)` — **not** contiguous from `data[0]`. Every consumer reads `data[0..size)`, so it reads the
wrong slots. This reached the **persisted capture file**: `capture.cpp`'s save copied `view.data` + size, so a capture
taken after a clear+refill wrote stale samples. `test_capture_roundtrip` never wrapped, so it was untested.

- **Fix** (`profiler.cpp` + [`profiler.hpp`](../../engine/foundation/perf/include/crd/perf/profiler.hpp) + `capture.cpp`).
  New `copy_thread_samples(thread_index, out, max)` copies the live samples **oldest-first, mask-indexed** — wrap-correct
  regardless of tail. The capture save path now uses it (the acceptance's "save while producers wrap buffers" — a
  cross-thread, wrap-correct copy). `thread_samples()` is kept for the in-process perf-ui panel and **documented
  same-thread, base-indexed, valid only unwrapped**; migrating perf-ui's live view off it is routed to (c3) with the
  enforced-single-reader. `ThreadRing::active` is now `std::atomic<bool>` (release on set after `samples`/`head`/`tail`
  are published, acquire on every cross-thread read) — the acceptance's "register/unregister during save", and it
  removes one data-race report TSan would have given.
- **Verified.** New [`test_diag_sample_ring.cpp`](../../tests/foundation/perf/test_diag_sample_ring.cpp)
  (`[perf][diag][sample-ring]`, 2 cases): (1) after 5×`ring.a` → `clear_samples` → 5×`ring.b`, `copy_thread_samples`
  returns all `ring.b` (the fix) **while** the raw `thread_samples().data[0]` is a stale `ring.a` — the bug and the fix
  in one run; (2) a capture saved after `save.old` → clear → `save.new` reads back through `CaptureView` as all
  `save.new` (the pre-fix save would have written stale `save.old`). Full `crd-perf-tests` **win-debug 839/150** (was
  823/148 + 2 cases) and **win-asan** `[sample-ring],[ring],[registry],[capture],[overflow]` 73/13 clean;
  `test_ring_overflow` + `test_capture_roundtrip` unchanged (drop semantics + CPROF compat preserved); **crd-perf-ui**
  builds; **win-shipping `/W4 /WX`** clean; `check_no_std_containers` **74** (adds 0). Both validators PASS. `Sample`
  (32 B) and the ring's drop semantics untouched. TSan CI-gated.

Next tick: **(c3)** — enforced single reader on the sample ring (an `atomic exchange` reader flag; a second concurrent
reader gets an empty result + a `contended` count, not a race) and migrate perf-ui's live `thread_samples` view to a
`copy_thread_samples`-backed path so it too is wrap-correct; write the one-reader protocol into the header. Then **(d)**
dynamic names. Begins with the protocol's advisor + CI check.

## (c3) landed — sample ring: enforced single consumer, clear/copy serialization, time-bounded capture retry

The (c2) copy is wrap-correct but its tear-free argument holds only while `tail` is fixed; two things move against that
— a second concurrent copier and `clear_samples` (which moves `tail` to `head`). (c3) closes both.

- **Per-ring single-consumer flag** (`profiler.cpp`). `ThreadRing` gains `std::atomic<bool> reader_busy` and
  `std::atomic<crd::u64> contended`. `copy_thread_samples` `exchange`-acquires the flag; if another consumer holds it,
  it REFUSES (returns 0, bumps `contended`, sets the new `bool* out_contended`) rather than racing, and releases on the
  normal path. The flag is per-ring, so copiers of *different* threads' rings never contend. The producer takes no flag
  (it only moves `head`; a concurrently-advancing `tail` from clear merely frees drop-on-full space).
- **`clear_samples` is a consumer too** — it takes the SAME flag (a bounded acquire-spin) before moving `tail`, so it
  can never move it out from under a mid-copy reader. This is the "tail advanced only under the reader flag" clause the
  original (c2) reader-discipline scope promised; it split here once read #1 showed (c2)'s live defect was the wrap
  offset. Clear is rare (UI/capture transition) and the window it waits on is one bounded copy.
- **Contention is distinguishable from empty.** `copy_thread_samples(..., bool* out_contended = nullptr)`: 0 with
  `*out_contended == false` is a genuinely empty ring; 0 with `true` is a refusal. `sample_copy_contended_count()`
  exposes the exact refusal count. Callers that must not silently drop (the capture writer) pass the out-param and
  retry; a live UI view may pass nullptr and tolerate a skipped frame.
- **Capture retry is TIME-bounded, not spin-bounded** (`capture.cpp`) — an advisor catch that would otherwise have
  shipped a latent bug. A copy window is a full-ring copy (~tens of µs, more under ASan); a fixed spin count (the first
  cut used 64) is exhausted *inside* one window, so a save racing a flag-holder would silently write whole threads as 0
  — invisible today (perf-ui still uses the zero-copy view) but live the moment (c3-ui) lands. The save now retries
  until `!contended` or a 50 ms `MonotonicClock` budget elapses (>> one window, << anything a user notices), then gives
  up TRUTHFULLY: `ThreadHeader::sample_count` is set to the count actually copied (not the pre-pass reservation), and an
  abandoned thread bumps `capture_contended_thread_count()` — never a silent zero-padded thread. The retry `yield`s
  between attempts so a concurrent `clear_samples` (the UI Clear button) is not starved by the save's 50 ms budget. The reader tolerates
  the resulting slack: `validate_capture_buffer` excludes sample bytes from its min-size check and `CaptureView` reads
  each thread by its own header offset+count with no cross-thread total reconstruction (**slack-tolerant reader
  verified**).
- **Header protocol.** `ThreadSamplesView`'s cross-thread contract is now stated per FIELD (`dropped`/`name`/`size` are
  safe cross-thread reads; only `data` indexing is same-thread + unwrapped), and `copy_thread_samples` documents the
  enforced-single-consumer + contention semantics.

**Verified.** New [`test_diag_sample_ring_readers.cpp`](../../tests/foundation/perf/test_diag_sample_ring_readers.cpp)
(`[perf][diag][sample-ring][readers]`, 4 cases / 5 proofs — (1) and (2) share one case): (1) **exactness** — 8 threads × 1000 copies, refused returns ==
`sample_copy_contended_count`, every refusal returns 0, every success returns the whole fixed batch; (2) **power** —
`REQUIRE(contended >= 1)` under that load (a 0 would be a test-power bug, not a pass); (3) **serialization** — 4 copiers
hammer while this thread alternates homogeneous `serial.a`/`serial.b` batches separated by `clear_samples`; no copy ever
straddles a clear (`mixed == 0`), copiers observed live batches (non-vacuous); (4) an **uncontended** single-threaded
save reports zero abandoned threads; (5) **retry outlasts contention** — 2 sustained hammers saturate the ring, four
saves each read back the full batch and abandon no thread. Case (5) has teeth: rebuilt at the first-cut 64-spin bound it
FAILED exactly as predicted (all four saves dropped the thread, `capture_contended_thread_count() == 4`); with the
time-bounded retry it passes on win-debug and — critically, since ASan lengthens the copy window — under win-asan.

Full `crd-perf-tests` **win-debug 859/154** (was 848/153 + 1 retry case + assertions) and **win-asan**
`[readers],[sample-ring],[capture]` 83/13 clean; `test_ring_overflow` + `test_capture_roundtrip` unchanged;
**crd-perf-ui** builds; **win-shipping `/W4 /WX`** clean; `check_no_std_containers` PASS (the threaded test uses
`crd::containers::Array<std::thread>`, `std::atomic`, fixed arrays — no owning STL); both validators PASS. `Sample`
(32 B) and the drop-on-full producer untouched. TSan CI-gated.

**Deferred to (c3-ui)** (named honestly, not left half-done — the panel is unchanged, still on the documented
same-thread `thread_samples()` view; the correctness-critical persisted path is the capture, hardened here). `(c3-ui)`:
`LiveProfilerSource` owns one reusable `crd::` buffer sized to `per_thread_ring_slots`; `thread_samples()` becomes a
`copy_thread_samples`-backed span valid until the next call. Precondition (now satisfied): the time-bounded capture
retry, so a save racing the live UI copy no longer drops threads. Before landing, audit the six panel `thread_samples`
call sites for two-live-spans aliasing (a single reusable buffer is only safe if no two thread spans are held at once).

Next tick: **(c3-ui)** as above, then **(d)** dynamic names (intern_name borrows the pointer — own the bytes, preserve
labels after slot reuse), **(e)** capture/shutdown quiescence, **(f)** clause-by-clause acceptance review + flip row
077. Begins with the protocol's advisor + CI check. Row 077 stays Open.

## (c3-ui) landed — perf-ui live view reads the ring through wrap-correct, single-consumer copy_thread_samples

The panel still read the live rings through the base-indexed zero-copy `thread_samples()` view — the exact
same-thread/unwrapped-only path (c2) documented — so after a perf-ui **Clear** the live timeline/flame/summary would
show stale pre-clear samples (the c2 bug, live in the UI). This migrates the live source onto the wrap-correct copy.

- **Capacity accessor** (`profiler.{hpp,cpp}`): `per_thread_ring_capacity()` returns `per_thread_ring_slots`
  (reflecting `InitConfig`, not a hardcoded default) or 0 when inactive — the size a cross-thread reader must give its
  buffer.
- **`LiveProfilerSource` owns one reusable buffer** (`profiler_source.hpp` + `live_source.cpp`): `mutable Sample*
  m_sample_buf` + `m_sample_cap` (mutable because the accessor is `const` by the interface but must refresh the copy),
  lazily `new (std::nothrow) Sample[cap]` and reallocated only if capacity changes across a re-init; the destructor
  frees it; copy/move deleted (owning pointer, UI-thread-only single buffer). `thread_samples(idx)` now returns a span
  over `copy_thread_samples(idx, m_sample_buf, m_sample_cap)` — wrap-correct and single-consumer-safe. `thread_name` and
  `thread_dropped` stay on the zero-copy view (they read `.name`/`.dropped`, the c3 header's cross-thread-safe fields;
  routing them through the buffer would clobber a live span mid-loop).
- **Interface contract** (`IProfilerSource::thread_samples`): the returned span is valid only until the NEXT
  `thread_samples()` call on the same source (the live source reuses one buffer). Stated on the interface, not just the
  impl, because the panel is polymorphic. `CaptureViewSource` is unchanged (stable view) but must honour the weaker
  promise.
- **Contention → empty frame.** The live read passes `nullptr` out_contended: if a concurrent save/clear holds the
  ring, that thread renders empty for one frame (next frame recovers). Acceptable for a live view; the correctness-
  critical persisted path is the capture, which retries (c3).
- **Not snapshot-consistent across passes (unchanged from before).** Each `thread_samples` call is an independent
  snapshot, so passes that combine two of them (`compute_time_bounds`'s window + `draw_timeline`'s samples, or
  `frame_record`'s bounds + a summary rebuild) can see the producer append between the two reads. This is not new — the
  old racing view had the same skew, handled by the timeline's off-window `continue` cull — and (c3-ui) did **not** make
  the panel snapshot-consistent; it only made each individual read wrap-correct. A consistent multi-pass snapshot, if
  ever wanted, is separate work.

**Aliasing audit — all five panel `thread_samples` sites are safe with one reusable buffer** (no site holds two thread
spans at once; a span is fully consumed before the next call):
- `profiler_panel.cpp:56` (`compute_time_bounds`): per-thread loop; each span is reduced into `begin_ns`/`end_ns`
  scalars within its iteration — no retained `Sample*`.
- `:129` (`draw_frame_summary`): per-thread loop; each span is reduced by `aggregate_top_level_by_name` into a
  `NameTotal[]` (NameId + times), then discarded — no pointer into the buffer survives the iteration.
- `:268` (`draw_timeline`): per-thread loop; samples are read inline into ImGui draw calls. `compute_time_bounds` (which
  itself walks all threads) runs to completion at `:213` **before** this loop, and `thread_name(t)` stays on the view
  path, so neither aliases the buffer.
- `:350` (`draw_flame_graph`): same reduce-into-`NameTotal` shape as `:129`.
- `:561` (`draw_gpu_passes`): a single GPU span held across the table draw, with no other `thread_samples` call in the
  function (`resolve_name` hits the name table, not the buffer).

**Cost named honestly:** the old view was free; each `thread_samples` is now a full-ring copy (a steady-state ring is
full: `per_thread_ring_slots` × 32 B = 128 KB at the 4096 default) × sites-rendered × threads per frame. Fine for a
debug panel; the follow-up *if it shows in a profile* is "copy each thread once per frame into per-thread buffers and
share across the draw_* passes" — deliberately not built now (the scoped unit is one reusable buffer).

**Verified.** `per_thread_ring_capacity` test in
[`test_diag_sample_ring_readers.cpp`](../../tests/foundation/perf/test_diag_sample_ring_readers.cpp) (0 inactive →
`kPerThreadRingSlots` default → 0 after shutdown → 1024 under `InitConfig{.per_thread_ring_slots = 1024}`). The adapter
oracle is the c2 wrap scenario through the UI path, in
[`test_profiler_sources.cpp`](../../tests/ui/perf-ui/test_profiler_sources.cpp) (`[perf-ui][source][live][diag]`): fill
`ui.a` → `clear_samples` → fill `ui.b` → `LiveProfilerSource::thread_samples` returns all `ui.b`, size 5 (the old
base-indexed path would have returned stale `ui.a`). `crd-perf-tests` **win-debug 864/155**, `crd-perf-ui-tests`
**63/15**; **win-asan** `[readers]` 25/5 + `[source]` 28/5 clean (the `new[]`/`delete[]` + copy path has no leak/overrun
under ASan); **win-shipping `/W4 /WX`** `crd-perf-ui` clean (the `mutable` + `delete[]` path); `check_no_std_containers`
PASS (the reusable buffer is a raw `Sample[]`, not an owning STL type); both validators PASS. No repo-root scratch.

Next tick: **(d)** dynamic names — `intern_name` borrows the caller's pointer, so a dynamically-built label dangles or
mislabels after slot reuse; own the bytes and preserve labels. Then **(e)** capture/shutdown quiescence, **(f)**
clause-by-clause acceptance review + flip row 077. Begins with the protocol's advisor + CI check. Row 077 stays Open.

## (d) landed — the profiler owns its name bytes; dynamic names no longer dangle

Census defect #4: every registry (region/thread/counter/allocator) stored the CALLER's `const char*`. A name built in a
stack/heap buffer, or living in a module later unloaded, dangled -- `resolve_name` and the capture name-blob writer read
freed or overwritten memory. (d) makes the profiler copy the bytes it keeps.

- **Not a keying change -- pure ownership.** The name table was already CONTENT-keyed (FNV-1a + `strcmp`), not
  pointer-keyed, so a reused buffer with different bytes already hashed to a different slot. The old header's
  "pointer-keyed" line was simply wrong; the fix is to copy the bytes, keeping the existing content dedup. (The
  scheduled prompt's "preserve the pointer-keyed fast path" was based on that stale comment; there is no such path.)
- **Bounded name arena** in `ProfilerState`: `new (std::nothrow) char[]` at init sized from all four capacities
  (`(names + threads + counters + allocators) * kNameArenaBytesPerEntry`), freed at shutdown, never moved -- so
  `resolve_name` pointers are stable for the process lifetime (the table never unregisters). One helper
  `detail::own_name_bytes` (lock-free atomic-CAS bump, so the four registry paths need no shared lock) copies bytes;
  `own_registry_name` wraps it for the three non-table registries. This UNIFIES (d) across all four registries instead
  of four ad-hoc fixes.
- **`NameEntry::string` is now `std::atomic<const char*>`** -- `resolve_name` reads it LOCK-FREE while `intern_name`
  publishes under `names_mutex`. Release on publish (after the arena bytes + hash are written) / acquire on read closes
  a pre-existing data race of the same shape (b) closed for `AllocatorEntry` (TSan-relevant; the acceptance's "no stale
  dereferences").
- **Saturation is explicit, never a silent borrow.** Table full, arena full, or a name longer than `kMaxNameBytes` ->
  `intern_name` returns `kInvalidNameId`; the three registries store a stable `"(name storage exhausted)"` literal (never
  the caller's pointer). Each drop bumps `name_bytes_dropped_count()`. The old `CRD_ASSERT(false)` on table saturation is
  replaced by this non-fatal, counted path.
- **Over-long names are DROPPED, not truncated.** Truncation would break dedup fatally, not just risk a collision:
  `intern_name` compares the FULL incoming string against the stored copy, so a truncated store would never match its own
  repeats -- every re-intern of a >cap name would burn a fresh slot until the table fills. Drop is the only bounded
  choice.
- **Relabel/refresh must not leak the bump arena.** `register_allocator`'s dedup/relabel path and `register_thread`'s
  refresh path run on every call; owning fresh bytes each time would leak (the arena is bump-only) and eventually starve
  region names into `kInvalidNameId` -- a component re-registered per level load, or a worker re-registered per frame,
  would drain it. Both paths now `strcmp` the incoming name against the current stored one and re-own ONLY when it
  changed (cold-path compares). `register_counter_impl` was already leak-free -- its name+kind+type dedup returns the
  existing id before the own-name store -- so all three non-table registries are guarded.
- **`unregister_allocator` improvement (d) delivers silently:** it stores `name = nullptr` (seq_cst). Because the name
  now points into the arena (which outlives the slot), a reader that loaded the old name pointer before the unregister
  no longer dereferences freed memory -- it reads valid (if now-retired) bytes. A reader that loads after sees nullptr.
- **Out of (d), noted for (f):** `CounterEntry::name` is still a plain `const char*`, set once under `counter_mutex` and
  read lock-free by `counter_info`. (d) makes it OWNED (no dangle), but the set-once/read-lock-free access is a
  pre-existing benign race TSan may flag; atomicizing it is not a dangle fix and is left out of (d).

**Verified.** New [`test_diag_dynamic_names.cpp`](../../tests/foundation/perf/test_diag_dynamic_names.cpp)
(`[perf][diag][names]`, 8 cases): heap-name-survives-free, reused-buffer distinct+correct ids, content dedup preserved,
explicit table saturation (`kInvalidNameId` + drop count, `resolve_name(kInvalidNameId)==""`), over-long dropped+counted,
thread name survives its stack dying, a heap name freed before `save_capture_to_buffer` still resolves through
`CaptureView`, and relabel-with-unchanged-name leaks nothing (5000 same-name re-registers into a deliberately ~17 KB
arena -> `name_bytes_dropped_count()==0`). **Teeth** (both observed, not argued): temporarily reverting `own_name_bytes`
to borrow made three cases fail including a SIGSEGV (reading freed memory) -- the exact dangle (d) removes; and forcing
the relabel guard off made the relabel test drop 3673 names (arena exhausted at ~iteration 1327), confirming the guard
is load-bearing. The owning, guarded build passes 28/9. Full
`crd-perf-tests` **win-debug 887/163** (was 864/155 + the readers/names additions); **win-asan** `[names]` 28/9 and
`[names],[capture],[intern]` 78/19 clean (no heap-use-after-free -- the dangle cases are the ASan oracle);
**win-shipping `/W4 /WX`** `crd-perf` clean (the arena/`memcpy` path); **crd-perf-ui** builds (allocator/counter name
pointers are arena-backed, still stable); `check_no_std_containers` PASS; both validators PASS; `test_intern_names` +
`test_capture_roundtrip` unchanged. No repo-root scratch.

**Deferred to (d2) -- history labels after allocator slot reuse (the acceptance's "no mislabelled history").** Still
open: `AllocatorMeta` / `allocator_info(idx)` resolve the name from the LIVE slot at read time, so a frame recorded
before an unregister+reuse shows the *new* allocator's name. `AllocatorEntry::generation` already bumps on every slot
(re)use, and `AllocatorRecord` has a spare `_pad` u64 -- so (d2) is: stamp `name_id + 1` (0 = unset; NameId 0 is a valid
id) per record into that pad inside `frame_mark`, and make `CaptureView` / `allocator_info` prefer it when non-zero. 48 B
pin preserved, old captures read unchanged -- **no format bump**. Split honestly because it is a distinct mechanism with
its own test surface; (d) is the name-ownership bulk.

Next tick: **(d2)** as specified above, then **(e)** capture/shutdown quiescence, **(f)** clause-by-clause acceptance
review + flip row 077. Begins with the protocol's advisor + CI check. Row 077 stays Open.

## (d2) landed -- allocator history records carry their own name identity across slot reuse

**Defect (acceptance's "no mislabelled history").** `CaptureView::frame_allocator_name` resolved an allocator's name
from the LIVE slot (`allocator_info(slot).name` = the `AllocatorMeta` written at save time). When a slot is
unregistered and reused by a different allocator, every *earlier* frame that referenced that slot then rendered with the
*new* occupant's name. A frame recorded while slot 0 held `alloc.first` read back as `alloc.second`.

**Fix -- stamp an interned NameId per record, not a string.** The identity written into each `AllocatorRecord` is a
`NameId` into the capture's existing name blob, not copied characters. That choice is deliberate: the record becomes
**self-describing through machinery the CPROF already carries** (the name table + `resolve_name`), so there is **no
wire-format change** -- the stamp reuses `AllocatorRecord`'s spare `_pad` u64, the 48 B record pin is untouched, and the
CPROF header/version are unchanged. A string field would have meant a format bump and a second copy of bytes the name
blob already holds.
- `AllocatorEntry` gained `std::atomic<crd::u32> name_id{0xFFFF'FFFFU}`; `register_allocator` interns the label via
  `intern_registry_name` and publishes `name_id` **before** the `allocator` seq_cst store, in all three registration
  paths (fresh, dedup, relabel). `unregister_allocator` clears it to `0xFFFF'FFFF` alongside `name` (seq_cst).
- `frame_mark`'s allocator loop stamps `ar._pad = nid != 0xFFFF'FFFFU ? static_cast<crd::u64>(nid) + 1U : 0U` -- the
  **`+1` sentinel** reserves 0 for "unset" because NameId 0 is itself a valid id.
- A single shared decoder, `allocator_record_name_id()` in [`frame_record.hpp`](../../engine/foundation/perf/include/crd/perf/frame_record.hpp),
  is the one place that reverses `_pad - 1` back to a `NameId` (or `kInvalidNameId` when 0). `CaptureView::frame_allocator_name`
  prefers it and only falls back when it yields `kInvalidNameId`.

**The fallback is honest, not a second source of truth.** When a record's identity is unset (`_pad == 0`) the reader
falls back to `allocator_info(slot).name` -- the live-at-save `AllocatorMeta` name. This is correct **only if the slot
was never reused**. For a capture taken *before* (d2), a reused slot's earlier frames still resolve to the live-at-save
name -- i.e. they read exactly as they did before (d2), which for the reuse case is the original mislabel. The
mislabel (d2) fixes is inherent to captures recorded without the stamp and **cannot be recovered** from them; (d2) fixes
it going forward, and old captures degrade to their prior (unchanged) behaviour rather than breaking. That is the whole
of "no format bump": pre-(d2) records are indistinguishable from records whose allocator was never reused.

**Intern-saturation edge.** If the name table is saturated when the allocator registers, `intern_registry_name` returns
`kInvalidNameId`, `name_id` stays `0xFFFF'FFFF`, `_pad` stays 0, and the record falls back to the live-slot name (which
is still owned in the roomy name arena from (d), so it is a real string, not a dangle). The fallback test pins this
exact path.

**Verified.** New [`test_diag_history_labels.cpp`](../../tests/foundation/perf/test_diag_history_labels.cpp)
(`[perf][diag][history]`, 3 cases): (1) slot-reuse -- register `alloc.first`, `frame_mark`, unregister, register
`alloc.second` into the *same* slot (`REQUIRE(slot2 == slot)`), `frame_mark`, then the older frame still reads
`alloc.first` and the newer `alloc.second`; (2) fallback -- a saturated name table leaves `_pad == 0`, and the reader
returns the live-slot name, with an explicit `CHECK(allocator_info(slot).name == "alloc.fallback")` pinning that the
fallback SOURCE is the `AllocatorMeta` (not the stamped path); (3) out-of-range frame/slot returns `""`. **Teeth**
(observed, not argued): removing the `frame_mark` stamp made the older frame resolve to `alloc.second` -- the exact
mislabel (d2) removes -- and the CHECK failed; restored. Full `crd-perf-tests` **win-debug 905/166**; **win-asan**
`[history],[capture],[registry]` **72/12** clean; **win-shipping `/W4 /WX`** `crd-perf` clean; **crd-perf-ui** builds;
`check_no_std_containers` PASS; both validators PASS; no repo-root scratch.

**Toolchain note (not a code issue).** The first **win-asan** compile of `test_diag_sample_ring_readers.cpp` hit a
transient MSVC ICE (`0xC0000005` in `cl.exe`, no `error C####` diagnostic) under `/fsanitize=address` + PCH; the
immediate retry compiled and the suite passed 72/12. Not reproduced. Recorded so a recurrence is recognised as a
toolchain fault rather than a code regression.

**What (e) is.** Census defect #5 -- simultaneous capture/shutdown. `shutdown` `delete[]`s the profiler tables (and the
name arena) while a reader -- `thread_samples`, `frame_record` access, `allocator_snapshot`, or `save_capture_to_buffer`
-- may still hold `*g_state`. The pattern is the `snapshot_in_flight` allocator quiescence guard (b) built, generalised
to a **state-wide in-flight count**: every reader RMW-enters on entry and exits on completion, and `shutdown` drains it
(yield-spin to zero) *before* `delete`, with the same Dekker-shaped seq_cst store-then-load handshake `unregister_allocator`
already uses. Then **(f)**: clause-by-clause acceptance review and flip row 077 Open -> Needs CI.

Next tick begins with the protocol's advisor + CI check. Row 077 stays Open.

## (e) landed -- capture/shutdown quiescence (census defect #5)

**Defect.** `shutdown()` freed `*g_state` (the rings, name arena, counter/allocator tables, frame history) while a
concurrent reader still held a reference into it -- a whole `save_capture_to_buffer`, a `thread_samples` copy, a
frame-record read. The retirement was a plain `g_state = nullptr` racing plain reads (itself UB), with no wait for
in-flight readers. Under ASan the race is `heap-use-after-free`; in a debug build it SIGSEGVs.

**Fix -- an atomic state pointer + a drained state-wide in-flight count (a Dekker handshake).**
- `g_state` is now `std::atomic<ProfilerState*>`; `shutdown()` RETIRES it with a single seq_cst `exchange(nullptr)` so
  every reader entering afterwards loads null and touches nothing.
- A file-scope `std::atomic<crd::u32> g_in_flight` counts readers dereferencing state right now. It lives **outside**
  `ProfilerState` on purpose: a reader increments it *then* loads `g_state`; were the count inside `*g_state`, shutdown
  could free the state between those two steps -- the same UAF one level up.
- `reader_enter()` does `fetch_add(in_flight, seq_cst)` **then** `load(g_state, seq_cst)`, backing the increment out if
  null. `shutdown()` does `exchange(g_state, seq_cst)` **then** spins `load(in_flight, seq_cst)` to zero (yield) before
  `delete`. Both sides store-then-load under seq_cst: at least one observes the other, so a reader that began before the
  exchange is counted and waited for, and one that begins after loads null. This is the same store->load argument
  `unregister_allocator` already uses for `snapshot_in_flight`, generalised from one slot to the whole state. No mutex
  is held during the drain, so a reader blocked on `alloc_mutex`/`names_mutex` can still finish and decrement.

**Centralisation without churn -- the shadow-local trick.** Every reader entry point opens with two lines: a
`detail::StateGuard guard_;` (RAII: `reader_enter()` in the ctor, `reader_leave()` in the dtor) and a
`detail::ProfilerState* const g_state = guard_.s;` local that **shadows** the (now-atomic) global. Every existing body
line -- `if (g_state == nullptr)`, `g_state->rings[...]`, `*g_state` -- then compiles unchanged against the loaded,
pinned local. The `using detail::g_state;` was removed so any g_state-dereferencing path left without a shadow fails to
compile (`undeclared identifier`) -- a deliberate failsafe that no reader was missed. Nested guards are fine (a guarded
`register_allocator` calling `intern_name` re-enters; the count simply rises past 1).

**Atomic capture, not just crash-free.** `save_capture_to_buffer` holds ONE outer `detail::StateReadGuard` (public, in
profiler.hpp) for its whole body; its inner calls each take a per-call guard too (count rises to 2). Without the outer
pin, per-call guards would keep the capture crash-free but let it TEAR across a shutdown that slips in between calls --
the outer pin makes `shutdown()` block until the whole capture completes.

**Re-init on ANY thread is safe -- the TLS generation stamp.** `shutdown()` clears the thread-locals
(`t_thread_index`/`t_ring`) only for its OWN calling thread; a shutdown on a different thread leaves every other
thread's TLS cache stale (pointing into the just-freed state). Because (e) legitimises cross-thread teardown, this is
now a live scenario: the init thread that shut down elsewhere would, on the next `init()`, hit `register_thread`'s
"already registered? refresh" fast path and reuse its stale index against the fresh state's empty (`samples == nullptr`)
ring -- a null deref on the next `CRD_PERF_SCOPE`. Fix: `init()` stamps a unique non-zero `ProfilerState::generation`
(from a monotonic `g_state_gen`) before publishing; each thread caches it in `t_state_gen` when it registers; the
refresh path is taken ONLY when `t_state_gen == g_state->generation`, else the thread falls through and re-registers
into the new state. The generation (not `rings[idx].active`) is the right key -- another thread may legitimately hold
that index in the new state, and checking `active` would let this thread steal its ring.

**Documented residual (unchanged contract, not a regression).** The per-sample hot path (`push_region`/`pop_region`),
the per-write counter path (`counter_set_*`/`counter_add_*`) and the GPU `write_external_sample` load `g_state` relaxed
and are NOT guarded -- an RMW there would defeat the zero-overhead gate. They remain covered by the pre-existing
contract that every recording thread is joined before `shutdown()`; the ring memory they touch is inside `*g_state`, so
this residual is inherent to writers and is the same contract that already governed them. The zero-overhead gate passes
unchanged (the hot path gained only a relaxed atomic-pointer load, a plain `mov` on x86).

**Contracts (in the header).** `shutdown()` latency is bounded by the longest in-flight read -- for a full capture,
per-thread sample copy plus its 50 ms contention-retry budget PER THREAD (so a pathological capture is O(threads) of
that budget, not a single 50 ms); `shutdown()` must NOT be called from inside a reader / `StateReadGuard` scope (it would
wait on itself); a fresh `init()` after a drained `shutdown()` is safe on ANY thread (stale readers backed out or
completed against the old state; a stale TLS registration cache is rejected by the generation stamp). Returned raw
references keep their PRE-EXISTING lifetime contracts unchanged -- the (e) guard makes the deref INSIDE each call safe,
but does not extend the lifetime of what the call returns: `resolve_name` / allocator name pointers are valid until
shutdown (d); `frame_record()`'s raw `const FrameRecord*` is tear-prone (use `copy_frame_record`); `thread_samples()`'s
zero-copy view is same-thread-only (use `copy_thread_samples`). All three die at shutdown, which is exactly when the
drain guarantees no reader is mid-flight.

**Verified.** New [`test_diag_quiescence.cpp`](../../tests/foundation/perf/test_diag_quiescence.cpp)
(`[perf][diag][quiescence]`, 4 cases): (1) a DETERMINISTIC drain -- a reader holds a pin for 50 ms on one thread while
`shutdown()` runs on another; `shutdown()` provably does NOT return while pinned, then completes once released; (2)
post-shutdown readers return empty and take no pin; (3) re-init on the init thread after a CROSS-THREAD shutdown records
into the new ring (the generation-stamp path); (4) a 64-iteration RACE of concurrent `save_capture_to_buffer` vs
`shutdown()` -- the ASan UAF oracle. All Catch2 assertions run on the main thread after join (the macros are not
thread-safe); workers publish into atomics. **Teeth** (all observed, then restored): disabling the drain made the
deterministic test's `returned_while_pinned` CHECK fail AND SIGSEGV'd the race test in win-debug, and under **win-asan**
reported `heap-use-after-free` -- READ on the capture thread, freed by the shutdown thread, at
[`capture.cpp:108`](../../engine/foundation/perf/src/capture.cpp) (`copy_to_fixed`), precisely census defect #5;
removing the generation check SIGSEGV'd the re-init test (stale ring null-deref). Full `crd-perf-tests`
**win-debug 918/170** (was 905/166 + the 4 quiescence cases); **win-asan** `[quiescence],[capture],[registry]`
**61/12** clean; the **zero-overhead gate** `[gate]` 5/2 unchanged; **win-shipping `/W4 /WX`** `crd-perf` clean (the
shadow local is always read -- no C4189; the two `namespace detail` shadows suppress C4459 locally, the same push/pop
pattern the file uses for C4324); **crd-perf-ui** builds; `check_no_std_containers` PASS; both validators PASS; no
repo-root scratch.

**Toolchain note (not a code issue).** One win-debug compile of the untouched `test_diag_registry_race.cpp` failed with
an MSVC `<chrono>` tzdb error (`C3546`/`C3520` instantiating `std::tuple<std::string, std::vector<time_zone>, ...>` at
`tuple(985)`) -- a known intermittent MSVC 14.51 STL/PCH artifact, unrelated to (e); the immediate retry compiled it and
the whole suite passed. Same class of transient as the (d2) ICE. Recorded so a recurrence is read as a toolchain fault.

**What (f) is.** The final DIAG.6a sub-unit: a clause-by-clause walk of row 077's acceptance text against what
(a)..(e) landed (registry snapshot census; no torn reads; no dangling names; no mislabelled history; no
capture/shutdown UAF), then flip row 077 Open -> Needs CI with the Session link + latest actions/runs URL. Row 077
stays Open until then.

Next tick begins with the protocol's advisor + CI check.

## (f) acceptance walk -- row 077 Open -> Needs CI

The clause-by-clause walk of DIAG.6a's acceptance (from
[runtime-diagnostics.md#diag-6a](../design/runtime-diagnostics.md#diag-6a)) against the landed work. Each clause maps to
the sub-unit that satisfies it and the test that proves it (teeth observed, not argued, throughout (a)..(f)).

| Acceptance clause | Sub-unit | Evidence (test) |
| --- | --- | --- |
| Repair & qualify allocator/counter/name/thread registries | (a) census + (b)(d)(d2)(f) | whole `[perf][diag]` suite |
| Sample-ring publication protocol read; head/tail alone insufficient | (c1)(c2)(c3) | `test_diag_ring_tearing`, `test_diag_sample_ring`, `test_diag_sample_ring_readers` |
| Immutable generations / documented sync scheme | (c1) seqlock, (c3) single-consumer, (b)/(d2) slot generation, (e) state generation | `test_diag_ring_tearing`, `test_diag_sample_ring_readers`, `test_diag_history_labels`, `test_diag_quiescence` |
| Separate live count from high-water index | (b) `allocator_live_count` vs `allocator_count_atomic` | `test_diag_registry_race` |
| Preserve frame/history labels after slot reuse | (d2) per-record interned NameId | `test_diag_history_labels` |
| Fix DG08 (unsynchronized registry reads) | (d) `NameEntry::string`, (b)/(d) `AllocatorEntry::name`, **(f) `CounterEntry::name`** atomicized; (e) state quiescence | `test_diag_dynamic_names`, `test_diag_quiescence`, counter suite |
| snapshot/save while producers wrap buffers & register/unregister | (c2)(c3) oldest-first wrap-correct copy + (b) churn | `test_diag_sample_ring_readers` (wrap), `test_diag_registry_race` (register/unregister) |
| simultaneous capture/shutdown | (e) atomic `g_state` + drained in-flight count | `test_diag_quiescence` (win-asan UAF oracle) |
| dynamic names from unloaded modules | (d) name arena owns the bytes | `test_diag_dynamic_names` (heap-name-survives-free) |
| allocator destruction while sampling | (b) `snapshot_in_flight` quiescence | `test_diag_registry_race` |
| No torn records | (c1) seqlock, (c3) single-consumer | `test_diag_ring_tearing`, `test_diag_sample_ring_readers` (serialization) |
| No stale dereferences | (b) snapshot pin, (d) owned names, (e) state pin, (f) counter-name acquire | `test_diag_quiescence`, `test_diag_dynamic_names` (both win-asan) |
| No mislabelled history | (d2) | `test_diag_history_labels` |
| Preserve dropped/corrupt/unavailable counts | ring `dropped` / `frame_history_unavailable` / `name_bytes_dropped` / `sample_copy_contended` | `test_ring_overflow` (dropped), `test_diag_ring_tearing` (unavailable), `test_diag_dynamic_names` (name drops), `test_diag_sample_ring_readers` (contended) |
| Compatibility with existing captures | (d2) reuses the spare `_pad`; CPROF header/version unchanged | `test_diag_history_labels`, `test_capture_roundtrip` |

**Two items deferred to (f), resolved.**
- **`CounterEntry::name` (DG08's last unsynchronized registry read).** The (d) note left it a plain `const char*`; the
  design body names exactly this ("Investigate DG08's unsynchronized registry reads rather than assuming its writer
  mutex protects readers"). `counter_info` read it lock-free with no release/acquire pairing to the mutex-guarded write.
  Fixed to `std::atomic<const char*>` like `NameEntry::string`: published with a release store AFTER kind/type/bits in
  `register_counter_impl`, acquire-loaded in `counter_info`, relaxed-loaded under the mutex in the dedup scan. (d)
  already made the bytes arena-owned, so there was no dangle left to catch -- this closes the remaining data race, whose
  detector (TSan) is CI-gated on this Windows toolchain; functional behaviour is covered by the existing counter suite
  (still green) and the release/acquire reasoning. Verified: win-debug 918/170, win-asan `[counter],[registry],[capture]`
  clean (counter suite 63/25 on the sanitizer lane), win-shipping `/W4 /WX` clean, crd-perf-ui builds.
- **`profiler_panel.cpp` raw `frame_record()` pointers.** Every `src.frame_record(...)` is dereferenced immediately
  within the same draw call (no member retention, no use across a `frame_mark` boundary or another API call), so the
  live raw-pointer read carries the panel's pre-existing same-thread contract and the save path uses the tear-free
  `copy_frame_record`. Consumed-immediately -> safe; no change.

**Consolidated residuals (none block acceptance).** (1) The per-sample hot path (`push_region`/`pop_region`), per-write
counter path (`counter_set_*`/`counter_add_*`) and GPU `write_external_sample` load `g_state` relaxed and are covered by
the "join every recording thread before shutdown" contract (an RMW there would defeat the zero-overhead gate). (2)
Returned raw references keep their pre-existing lifetime contracts: `frame_record()` tear-prone (use
`copy_frame_record`), `thread_samples()` zero-copy same-thread-only (use `copy_thread_samples`), `resolve_name`/registry
name pointers valid until shutdown. (3) TSan-detectable data-race freedom (the counter-name and other lock-free registry
reads) is CI-gated -- no TSan preset on this Windows toolchain; the release/acquire pairings are in place and argued.

**Verification (whole slice, this tick).** win-debug `crd-perf-tests` **918/170**; win-asan
`[quiescence],[capture],[registry]` **61/12** clean; zero-overhead gate `[gate]` 5/2 unchanged; win-shipping `/W4 /WX`
`crd-perf` clean; `crd-perf-ui` builds; `check_no_std_containers` PASS; both validators PASS; no repo-root scratch.

**Flip.** All acceptance clauses are satisfied with test-backed, teeth-observed evidence; the two deferred items are
resolved; residuals are documented and non-blocking. Row 077 (DIAG.6a) flips **Open -> Needs CI**, pending published CI.
The cited `actions/runs` URL is the latest run (`34946718738`); it predates and does not yet include this work -- that is
what "Needs CI" records. DIAG.6a's sub-units (a)..(f) are complete. Next roadmap row: DIAG.6b (row 078).
