# Decision brief: the flight recorder's global ring against per-thread rings (DIAG.11b follow-up (q))

<!-- doc-role: historical -->
> Dated evidence and a proposal awaiting the user's decision; nothing here is accepted or implemented. Live owner:
> [DIAG.11b](../ROADMAP.md#slice-diag.11b), follow-up (q). Contract: [ADR-0133](../decisions/0133-runtime-diagnostics-and-instrumentation.md),
> [flight recorder](../design/runtime-diagnostics.md) (DIAG.9b, settled 2026-10-10). Boards it builds on:
> [Tracy peer](../bench/2026-10-26-diag-11b-tracy-peer.md), [tick clock](../bench/2026-10-26-diag-11b-tick-clock-inlining.md).
> Rules: [AGENTS](../../AGENTS.md).

## The question

At four jobs threads on Windows Cerid's kept event per job costs more than Tracy's message per job, and the
four-thread jobs frame in full mode is over Tracy connected. Earlier experiments put the cost on the recorder's one
claim word (every event's compare-exchange) and its ring slots, which every thread writes. Removing them changes what
the recorder promises. **Which guarantee is the user willing to trade for the frame time?**

What the recorder promises today (`flight_recorder.hpp`, the DIAG.9b contract):

1. **One total order.** Every kept event gets the next sequence of one word by a compare-exchange; a capture lists
   its events in that order. Because every claim is a read-modify-write of the same word, the order agrees with
   happens-before: an event that happens before another on any thread has the lower sequence.
2. **An exact trigger window.** The trigger moves the same word to Triggered in one exchange, so every event claimed
   before it is in the pre window and every event claimed after it in the post window ("an event racing the trigger
   lands on one side, never both"); the claim that reaches `post_events` closes the window in its own exchange, so
   the post window holds exactly `post_events` events (or fewer, marked, on shutdown, fatal or time-out).
3. **Bounded, allocation-free recording.** One ring of `capacity` fixed 152-byte events (1024 by default, 152 KiB;
   at most 65,536, about 9.5 MiB); `pre_events + post_events <= capacity`; the record path takes no lock and never
   allocates.

## Measurements

Prototypes lived only in an uncommitted working tree (`build/q1/patch_proto.py`, removed before any commit): the
record path of each option behind an environment variable, so every comparison is two arms of **one binary**. Only
the Armed record path was prototyped — trigger, capture, merge, reload and the fatal path were not, and their costs
fall on rare paths. All boards on the i9-14900K, win-shipping-profile (MSVC 19.51, LTCG), after the tick-clock fix
(`1fcd532d`), the jobs workload (2,048 jobs a frame, four threads, affinity `5555`), full mode, 200 timed frames,
interleaved processes, bootstrap 95% intervals, `scripts/build-bench.py host` idle before each run.

| Option | What a record does | Site cost a frame, harness (16 processes) | Site cost, peer binary | Full frame vs Tracy connected (peer binary, 10 processes) | CPU vs Tracy | One thread, per site |
|---|---|---:|---:|---:|---:|---:|
| A, global ring (today) | compare-exchange of the word; the event into the sequence's slot | 87.7 µs | 83.4 µs | **+7.63% (+0.50 .. +9.76)** | +5.65% (+2.42 .. +9.10) | 70.2 ns |
| B, per-thread rings | the word only read (frozen check); the event into the thread's own ring | 49.7 µs | 51.2 µs | **−2.81% (−3.76 .. −1.47)** | −6.14% (−7.86 .. −4.76) | +0.3 ns |
| C, global sequence and locator ring | `fetch_add` of the word; the event into the thread's ring; a 16-byte locator into a global ring | 77.3 µs | 79.0 µs | +5.21% (+3.04 .. +7.82) | +4.59% (+0.06 .. +7.32) | +1.5 ns |
| C2, global sequence, per-thread rings | `fetch_add` of the word; the event and its sequence into the thread's ring only | 65.8 µs | 66.1 µs | **+1.71% (−2.79 .. +3.51)** | −1.46% (−4.19 .. +2.08) | +0.4 ns |
| D, claims in blocks of 16 | one `fetch_add` per 16 events; the word read otherwise; the event and its sequence into the thread's ring | 54.5 µs | 55.8 µs | **−0.14% (−1.86 .. +0.55)** | −3.54% (−5.74 .. −1.64) | +0.9 ns |
| Tracy connected (`TracyMessage`) | streamed to the client by Tracy's worker | | ≈51 µs | reference | | 71.4 ns (peer board) |

- "Site cost a frame" is the full frame minus the same binary's full frame with the jobs workload's site left out
  (`CRD_EXP_NOSITE`): the harness binary (`2743a050`) on one 16-process board, the peer binary (`7f4bba91`) from a
  board per option and one with the sites left out (Cerid 239.9 µs, Tracy connected 250.2 µs), so its column mixes
  boards. The frame comparison against Tracy is within each board.
- One thread: the `events` workload (4,096 sites a frame, 8 processes): every option within 1.5 ns of A. The cost is
  contention, not work.
- Reading the table: C2 against B is the price of the shared counter alone (about 16 µs a frame); A against C2 is the
  shared slots (about 22 µs); C shows that moving the slot writes to a shared locator ring buys almost nothing — any
  per-event write to a line other threads write costs about as much as the event's slot.
- Since the tick-clock fix A is +7.6% over Tracy on this frame (it was +15.9% on the peer board before it). Every
  Cerid number here is inflated on Windows by follow-up (r) (crd-perf's `std::atomic` operations left out of line by
  MSVC LTCG); the options' relative order is what this brief compares.

## The options

### (A) Keep the global ring

- Semantics: unchanged (1, 2, 3 above).
- Risk: none.
- Cost: the four-thread jobs frame +7.6% over Tracy connected, a kept event's share 83 to 88 µs a frame against
  Tracy's about 51; at one thread the same as Tracy. The loss stays open against clause (8)'s peer goal.

### (B) Per-thread rings, merged at capture by timestamp

- How: a pool of rings allocated at `init` (no allocation on the record path; a thread takes the next free ring on its
  first event, one past the pool uses a shared overflow ring with today's claim). While Armed an event goes into its
  thread's ring with its timestamp and the thread's own sequence; the record path reads the state word but writes no
  shared line. While Triggered (rare: one capture at a time) events take today's global claim, so the post window is
  counted exactly. The capture merges every ring's pre-trigger events by timestamp, ties broken by thread.
- What survives: the trigger boundary stays exact, with the event's state read as its linearisation point (an event
  that read Armed is in the pre window, one that read Triggered claims a post-window sequence), and the post window
  exactly as today. Program order within a thread survives.
- What is lost: one total order of the pre window that agrees with happens-before. Timestamps (TickClock, the
  invariant counter) order events in time, but `rdtsc` is not ordered with memory operations, so two events
  synchronised across threads within tens of nanoseconds can appear inverted; on a host without an invariant counter
  MonotonicClock's 100 ns resolution makes ties common. A sequence number per event exists only per thread.
- Memory: every ring must hold `pre_events + post_events` for the pre window to be complete whichever thread was
  busiest: a 64-ring pool at the default 1024 events is 9.5 MiB (today 152 KiB), at the policy's maximum 608 MiB, so
  the policy needs a pool or per-ring bound and loss metadata for a ring that wrapped inside the window.
- Changes: ADR-0133's recorder contract and the design note's wording (order 1), the CFR1 payload (a thread per
  event; a version bump), the capture's builder (a k-way merge), the decoder's checks, the `[diag11b]` and DIAG.9b
  tests of exact order.
- Cost: the frame 2.8% under Tracy connected and 6.1% less CPU; the site's share at Tracy's (about 51 µs).

### (C) Hybrids that keep a global sequence

**(C2) One global sequence, per-thread rings.** A `fetch_add` of the word gives each event its sequence; the event and
its sequence go into the thread's own ring; the capture gathers the window's sequences from every ring.
- What survives: all of (1) and (2): one total order agreeing with happens-before, and the trigger's exchange on the
  same word keeps the boundary exact in claim order. Only the slots move.
- What is lost: nothing in the order; a sequence whose thread's ring wrapped before collection is missing, so the
  capture must report it (a new loss count). Memory as in (B).
- Changes: the ring layout and the capture's gathering; the contract's wording only for the storage bound.
- Cost: +1.7% (−2.8 .. +3.5) against Tracy connected, unresolved; the site's share 66 µs, halfway between A and B.

**(D) Claims in blocks.** While Armed, a thread claims 16 sequences with one `fetch_add` and spends them on its next
events, reading the state word for each; on reading Triggered it abandons the rest of its block (holes) and claims per
event as today.
- What survives: unique global sequences; the trigger boundary exact at each event's state read; the post window
  exact; program order within a thread.
- What is lost: sequence order across threads is block order, not claim order, so it no longer agrees with
  happens-before (a thread's later event can carry a lower sequence than another thread's earlier one); the pre window
  has up to (threads × 15) holes, so `pre_events` must count events, not sequences.
- Cost: −0.1% (−1.9 .. +0.6) against Tracy connected, a tie, with 3.5% less CPU; the site's share 55 µs. More moving
  parts than B (blocks, holes, two claim paths) for weaker order than C2 and less speed than B.

## Recommendation

- **If ADR-0133's order is to stay as written: C2.** It keeps every guarantee, removes most of the frame's gap to
  Tracy (A +7.6% → C2 +1.7%, unresolved) and about half of the site's excess (83 → 66 µs against Tracy's 51). The rest is the shared counter itself; follow-up (r)'s fix
  would lower every atomic on Windows, but no option that keeps one shared counter per event reaches B.
- **If the pre window may be ordered by time rather than by claims: B.** It is the simplest of the fast options and
  beats Tracy in frame time and CPU; it keeps the exact boundary and post window but gives up the happens-before
  total order and costs memory per thread.
- Not recommended: C (no gain worth its locator ring) and D (B's semantics loss without B's speed or simplicity).

My recommendation is **C2 now**, keeping the contract, and B only with an explicit amendment of ADR-0133's order
guarantee. Either choice is a recorder contract change that needs the user's decision before implementation; until
then the global ring (A) stays and follow-up (q) stays open, **awaiting the user's decision — not deferred work**.
