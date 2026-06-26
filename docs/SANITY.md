# Cerid — Engineering Sanity Doctrine

> **Read this every session.** It is short on purpose. Its job is to keep the engine
> *core* solid by encoding the lessons we paid for in real debugging time, so we don't
> pay for them twice. The target is an **A++ core**; this is the *path*, not a claim of
> arrival. Honest self-assessment as of 2026-06-09: a strong numerical stack on a **B+
> core** with one freshly-closed foundation bug. We get to A++ by *practice*, recorded
> in the **Sanity Ledger** at the bottom — every agent adds to it, a little at a time.
>
> Each rule below is **scar → rule → check**: the real bug that taught it, the rule, and
> the concrete thing you actually do. If a rule ever feels abstract, re-read its scar.

---

## The rules

### 1. Root-cause, never work around. ("No debts.")
- **Scar:** the multifrontal-LU flaky AV was almost shipped as a single-chunk `factor_pool`
  band-aid that *hid* a bug in `TlsfAllocator` — the engine's most-used allocator.
- **Rule:** a fix that *avoids* a symptom without naming its mechanism is a debt, not a fix.
- **Check:** before you call it fixed, state the mechanism in one sentence. If you can't,
  you haven't root-caused it — say so, don't dress it up. Workarounds get deleted once the
  root is found (the `factor_pool` was reverted; the "remaining debt" turned out not to exist).

### 2. Verify the *shipped* artifact, not a green you remember.
- **Scar:** a stale PCH (`C1853`) let "All tests passed" report on an **un-rebuilt** binary;
  an earlier session *fabricated* benchmark numbers from memory.
- **Rule:** a green checkmark you can't trust is worse than a red one — it hides bugs.
- **Check:** clean-rebuild before trusting a result after header/PCH changes; run **`ctest`**,
  not the bare test binary (guard tests live only in ctest — see CLAUDE.md DoD §8); capture
  perf/test numbers **to a file**, never quote them from memory. Verify the *clean* artifact
  (no diagnostics, no temporary workaround), not the intermediate you debugged with.

### 3. Boundary adversaries, not volume.
- **Scar:** `init_pool` placed the end sentinel 16 B too early; **820 K+** random-stress
  assertions sailed over it for a long time — random alloc/free almost never fills a pool to
  its *last* block, which is the only place the bug lived. (`crd::containers::String` bit us
  the same way once: `capacity == allocation size` off-by-one.)
- **Rule:** volume of assertions ≠ coverage of edges. Latent bugs hide at boundaries.
- **Check:** foundational modules get tests that hit edges *on purpose* — fill-to-tail,
  fragment-to-end, `capacity == allocation size`, empty, single-element, the last valid
  index — and **pre-poison** memory where a bug needs poison to show (the TLSF regression
  fills a `0xCD`-poisoned buffer to its tail; see `tests/memory/test_tlsf_allocator.cpp`).

### 4. Know what your diagnostic CAN'T see.
- **Scar:** poison/quarantine gave **false positives** (TLSF coalesce reuses freed payloads);
  page-heap **masked** the bug (it altered alloc timing); ASan can't see *intra-pool* TLSF
  overruns; the `{1..16}` determinism moat + ASan can't catch a UMR (resident pages are
  byte-identical). A **sound structural validator** (walk the block chain, check in-pool +
  free-list links + bitmap consistency) is what actually found it.
- **Rule:** a clean run from a tool that is blind to the bug class proves nothing.
- **Check:** match the diagnostic to the bug. Allocator structure → structural walk. UMR →
  NaN-poison (`0xFF`-fill) on a *big* problem. Race → TSan. Heap edge → ASan. Name what your
  tool is blind to before you trust its silence.

### 5. Measure before you optimize; refute your own hypothesis first.
- **Scar:** "fill-margin wins" (bmwcra) was **false** — fill == flop. `adaptive-ℓ`, staircase,
  panel-BLAS-2, amalgamation: all **refuted by the profile**, not by argument.
- **Rule:** the first thing you measure is whether your own theory is wrong.
- **Check:** no perf claim without a file-captured measurement; profile to find the real lever
  before touching a kernel (the CHOLMOD gap was *serial symbolic*, not the BLAS-3 we assumed).

### 6. Honest scoreboards — no asterisks, including about ourselves.
- **Scar:** parallel-Cerid-vs-serial-peer is the forbidden asterisk; we report the MUMPS
  *losses* (ns3Da 0.64×) right next to the wins.
- **Rule:** an overstated win erodes trust in every other number we report.
- **Check:** fair peer at its best, matched accuracy, same thread count; state losses plainly;
  don't grade the core A++ when it's B+. The doctrine *earns* the grade, it doesn't assert it.

### 7. Don't rabbit-hole — time-box, change tools, escalate.
- **Scar:** the flaky-AV chase burned a long arc through blind tools before the sound
  validator (advisor-gated) ended it in one step.
- **Rule:** grinding the same lever that isn't converging is how rabbit holes form.
- **Check:** when an approach stalls, call `advisor`, switch diagnostic, or write down what
  you've *ruled out* — don't repeat the failing move with more force.

### 8. Search the engine before you build. (Reuse > reimplement.)
- **Scar:** v12 reimplemented **erf/erfc/lgamma** (already in `crd::math::deterministic`,
  Cephes, and hesap-special *links crd-math*) and **misplaced f64 SIMD log/exp** into
  `hesap-special/detail` when crd-math is their home (it already has the f32 `log/exp` +
  `Vec4d`); a self-contained tridiagonal **QL** was about to be written when hesap-dense
  already ships Sturm/dqds/**MRRR**. All three because the engine wasn't grepped first.
- **Rule:** before implementing *any* solver, kernel, or utility (eigensolver, FFT, RNG,
  special function, SIMD transcendental, container, allocator…), FIRST check whether the
  engine already provides it. Reuse it; if it's close-but-not-quite, extend it *in its home
  module*; only build new when nothing fits — and say so explicitly.
- **Check:** grep `engine/*/include` for the capability (and its synonyms — `eig`/`eigen`/
  `tridiag`/`steqr`; `log`/`exp`/`Vec4d`; `bessel`/`i0`; `gamma`/`erf`) *before* writing the
  first line. New code lands in the module that owns that capability (a reusable SIMD math
  primitive → crd-math, not a consumer's `detail/`). Module-edge concern? confirm acyclicity
  (does the provider depend on you?) and prefer the edge or a shared module over a duplicate.
  A self-contained reimplementation is justified only after the search comes up empty.

### 9. A documented loss is an open bug, not a closed slice. Solve, don't just disclose.
- **Scar:** v12-d's cold transcendental tail *lost* to Boost (Lambert-W **0.05×** = 20× slower, K/E ~0.23×, E1 0.46×).
  Instead of fixing it, I wrote "Boost's decades of minimax tuning win" in the docs and moved on — then in a
  status report presented the disclosure itself as "honesty." The user's verdict: *that is not honesty, that is not
  doing the work.* The standing mandate (`feedback_full_victory_beat_all_gold_standards`) is FULL victory, every
  gold standard, honestly. A loss recorded in a doc is still a loss.
- **Rule:** honest reporting (rule #6) is *necessary but not sufficient*. Reporting a loss does not retire it. Every
  measured loss against a gold standard is an OPEN problem to solve — not an accepted endpoint you get credit for
  disclosing. "We lose but I documented it" is a silent failure dressed as candor. A 0.05× gap is almost never a
  fundamental wall — it's an algorithm/iteration/initial-guess problem (Boost beats you with *rational minimax* and
  *good seeds*, so use those: better initial guess → fewer Halley steps, a tuned rational instead of a generic series).
- **Check:** before calling any perf slice done, list every peer you lose to and **fix or escalate each** — fix it,
  or bring the user the *measurement* that proves it's a genuine wall (and let them decide), never bury it in prose.
  "Honest about losing" is the start of the work, not the end of it. Beating Boost means doing what Boost does
  (minimax rationals, Fritsch/Halley with a good seed) — reach for that, don't fall back to the textbook series.

---

## How to contribute to sanity (a little at a time)

Any agent with slack can pick **one** small hardening and append it to the ledger. Small is
the point — the core gets to A++ by accretion, not by a heroic pass.

- Add a **boundary-adversary test** to a foundational module (memory, containers, jobs).
- Add or tighten a **CI guard** (`add_test(NAME …)`) that makes a past mistake impossible.
- Convert a **workaround into a root fix** (then delete the workaround).
- **Trim** an over-long doc/memory line, or delete a stale/wrong memory (see scar #3's bloat).
- Replace a **remembered number** with a file-captured one.

---

## Sanity Ledger (append-only; dated; one line each; *actions*, not philosophy)

- 2026-06-25 — **tx-a audit caught the crd-math transcendental cluster's premise before building it** (search+measure-before-build, rule #8): `crd::math::deterministic` ALREADY ships sin/cos/tan/exp/log/pow (f32/f64/SIMD) + the newer `crd_exp1/crd_log1` cores exist — NOT a green-field. Measured: `crd_log1` ~1–2 ulp + 1.6× faster than libm (good); `crd_exp1` 1.4× faster but only ~1e-13 (Taylor, not ≤1 ulp) + denormal-broken; **`deterministic::sin` is 2.5× SLOWER than `std::sin`** (it traded speed for bit-exactness). ⇒ the cluster is UPGRADE+UNIFY+complete+route, not build; the hard "no std:: math" guard must wait until the lib is actually faster-than-libm. The Cerid Math Mandate (use `crd::math::*`, implement-if-missing) recorded in `reference_cerid_math_mandate` + the mpmath ulp-gate harness shipped. Premature enforcement would have *slowed* the engine (routing to the slower deterministic:: trig).


- 2026-06-25 — Added **rule #9** (a documented loss is an open bug, not a closed slice) after the user caught me presenting v12-d's Boost losses (Lambert-W 0.05×, K/E 0.23×) as "honest disclosure" instead of fixing them.
- 2026-06-25 — **Rule #9 PROVED: crushed ALL 7 v12-d functions vs Boost** (were 0.05×–0.91× losses) via generated minimax rationals (Chebyshev fit + Lawson reweighting → monomial `.inc`, gated ≤ each function's tolerance): E1 **7.98×**, Ei **6.55×**, zeta **20.5×** (replaced 8 `std::pow` Euler-Maclaurin terms), lambertW0 **1.17×** (3-piece rational, no Halley), ellint_K/E **1.05×** (full-range Cody form A+L·B), Carlson_RF **1.78×** (loosened `errtol` to the 1e-12 gate ⇒ one fewer duplication). Accuracy preserved (special 402081, DSP 27069, stats 317795 green on gcc+MSVC). **Meta-scar: my eval-cost pessimism was wrong 7/7 times** — I predicted K/E/lambertW "can't beat Boost" from hand-estimated ns, then each WON when measured. Lesson: estimate to *prioritize*, never to *conclude a wall* — wire it and measure (SANITY #5). The "fundamental wall" instinct lost every time to actually doing the work.
- 2026-06-09 — Root-caused + fixed `TlsfAllocator::init_pool` end-sentinel (was 16 B early → tail-slack overshoot); reverted the LU `factor_pool` workaround; closed the misdiagnosed `growable-tlsf-multichunk-freelist` debt.
- 2026-06-09 — Added `[memory][tlsf][boundary]` exact-fit-to-tail regression test (0xCD-poisoned buffer; drives a free block below the 512 B small-block boundary so a no-split alloc reaches the tail) — **verified it bites**: SIGSEGV on the buggy one-liner, passes (debug + ASan) on the fix. First two drafts were theater (passed on buggy code too) — see rule #2/#3.
- 2026-06-09 — Wrote this doctrine + trimmed the bloated MEMORY.md index line that motivated rule #3.
- 2026-06-09 — Created `docs/README.md` (the Documentation Map / Start Here); consolidated the three parallel onboarding lists (CLAUDE checklist, AGENTS re-entry prompt, ROADMAP "core docs") to point at it (one canonical reading order); defrosted the stale public `README.md`.
- 2026-06-09 — Pruned `context.md` to its own stated dashboard shape: **274 KB → 6.4 KB** (34 lean lines). Removed the stacked historical `As of DATE` / `Earlier same day` snapshots (all already in session logs + memory); kept live focus + coming-up + detour + last-shipped + a one-line-per-cluster recent history. Old version recoverable via git.
- 2026-06-10 — Root-caused the win-shipping phantom link failure to **`#deps 0` ninja deps** (`msvc_deps_prefix` English vs Turkish cl.exe ⇒ header changes silently never recompile — rule #2's exact class: a stale artifact can fail OR pass for the wrong reason); audited all 13 build dirs (win-shipping + win-tidy-local broken) and **FIXED them in-session** (wipe + standalone-CMake reconfigure; verified `#deps 0 → #deps 95` + suite green — no debt filed, per rule #1); CLAUDE.md Troubleshooting entry added; also fixed a pre-existing tidy naming error (`kTriPanel`) that had slipped into `b261478` the same way (stale tidy dir view).
- 2026-06-10 (later, v7-j) — The `#deps 0` fix was INCOMPLETE (rule #1's own test: the first "root cause" was a symptom) — a mixed-struct-layout shipping SIGSEGV exposed that **any in-build regenerate re-broke the dir**; the real root is the **VS-bundled CMake fork (`4.2.3-msvc3`, first on PATH under vcvars) storing an English `showIncludes` detection on this Turkish-locale host**. Durable fix executed: explicit standalone-CMake invocation policy + purged all `CMakeFiles/*-msvc*` stored detections + wiped/rebuilt win-shipping (verified `#deps 108` + incremental header rebuilds work); CLAUDE.md entry rewritten with the true mechanism.

- 2026-06-24 — Root-caused a win-debug **infinite loop** (595 s CPU, never terminating) in v12-f `binomial_inversion`: it was not reflection-aware, so the dispatcher's {n=500, p=0.95} (routed to inversion because n·min(p,1−p)=25<30) used raw p ⇒ `q^n = 0.05^500` underflows to 0 ⇒ the inversion `px` stays 0 ⇒ x climbs past `bound` ⇒ x>n ⇒ outer loop retries forever. Fix = reflect internally like `binomial_btpe` already does (sample Binomial(n, 1−p), return n−x). **Boundary-adversary (rule #3):** 282 K passing assertions sailed over it — only the single adversarial param p>0.5-at-large-n ({500,0.95}) reached the dead branch; the optimized full-suite run is what exposed the non-termination (debug just hid it behind slowness). Also: an optimized green is the artifact to trust (rule #2) — the debug "still running" was a *hang*, not slowness.
- 2026-06-21 — Root-caused a SIGSEGV in `crd-hesap-dense` `eig_real_impl`/`eig_complex_impl`: both guarded **n==0** but not **n==1** (the balance/Hessenberg pipeline assumes n ≥ 2), so a 1×1 matrix crashed in `hessenberg`. Exposed by v11-q `residuez` calling `roots()` on a degree-1 polynomial (1×1 companion). Fixed at the root (a 1×1 matrix's single entry IS its eigenvalue, eigenvector [1]) in both paths — benefits every eig consumer, not just `roots`. Boundary-adversary class (rule #3): the bug lived only at the smallest valid size, which random/large tests never hit. (Confirmed via grep: NO malloc/new/std-container in the new DSP code — crd containers only.)

### Open sanity backlog (small, claimable)
- Trim `MEMORY.md` back under its session-load limit (it currently exceeds it and loads only partially) — do it incrementally, don't risk losing info.
- Adversarial boundary-test pass on `crd-containers` (String/Array/HashMap capacity-edge cases).
- Confirm (don't assume) no other heavy-churn `GrowableTlsfAllocator` consumer was relying on the old behaviour now that `init_pool` is fixed.
- **Doc bloat (living/scannable class only — never truncate session logs/ADRs/dossiers):**
  - Collapse `docs/ROADMAP.md` status-table rows (full phase histories inline, 81 KB) to one line per phase; detail belongs in the phase doc.
  - Prune **closed** entries from `docs/debt.md` (73 KB) per its own "move to a session log when done" rule — a recurring discipline, not a one-time cut.
