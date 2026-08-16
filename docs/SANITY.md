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

### 10. A CI-only failure is a config-specific miscompile — confirm the toolchain, name the blind spot, pinpoint, fix at the source.
- **Scar:** the `wpt` test SegFaulted *only* on win-release/shipping in CI (3/3 runs), never locally (27/27 + gcc green).
  Three guesses cost real time: "older CI MSVC" (it was *newer* — 19.51/14.51 vs local 14.50), the `level-- > 0` reverse
  idiom (rewriting it changed nothing), and a NOINLINE (didn't help). The green **win-asan** run was *misleading* — MSVC
  ASan builds carry **no LTCG**, so they are structurally blind to an LTCG miscompile (rule #4). The real bug: 19.51
  `/O2`+LTCG miscompiled the inline `crd::usize{1} << level` in three hot loops to a garbage stack-address value (markers
  printed `count = 140698301264476` for `level == 2`) ⇒ ~1e14-iteration inner loop ⇒ OOB ⇒ SegFault.
- **Rule:** a deterministic pass-here/fail-there is a codegen/toolchain bug, not luck. Don't theorize from clean source —
  get runtime facts from the *failing* config, and never trust a green from a build that doesn't exercise the failing
  codegen path.
- **Check:** (1) pull the CI compiler version first (`cl` banner) — the local-vs-CI delta is the lead, and CI can be
  *newer*; (2) spin a **minimal temporary CI job** (one failing config × one target × one test, `branches-ignore: [main]`
  so the full matrix stays silent — ~3 min vs ~30) for a tight loop; (3) pinpoint with **flushed-stderr markers**
  (`fputs`+`fflush`; catch2's crash handler gives only the TEST_CASE line), escalating to printing the **actual variable
  values** when the structure looks right but the result is wrong (that exposed the garbage `count`); (4) **fix
  engine-side** — eliminate the miscompiled construct for all consumers (here: drop the in-loop `1 << level`, iterate by
  heap id), never mask by lowering the *test's* optimization; (5) strip every marker + the temp job before close (grep).

### 11. Suspect your own instrument before you claim a defect. (Verify the harness, not the vibe.)
- **Scar (2026-08-15):** in ONE session I manufactured **four phantom "failures"** and reported them as real — a PowerShell
  `| Select-Object -First` that closed the pipe and killed the exe (fake exit **255**); a healthy but SLOW whole-repo `ctest`
  I misread as "hung" (I queried the WRONG process name and never computed the ~3 h expected duration), then KILLED it at
  4175/6384 and wrote a **fabricated "2-hour ASan deadlock"** into four docs and told the user; a win-asan binary run without
  the ASan DLL on PATH (fake `0xC0000135`); and a raw `clang-tidy` over an MSVC PCH that FALSE-CLEANED (`0 warnings` — it never
  parsed). Zero were real; `B14-c`, the "hung" test, passes in **3.5 s**.
- **Rule:** a surprising failure is YOUR TOOLING until proven otherwise. Never state a defect — failed / hung / broken /
  timeout — to the user or in a doc before ruling out the harness. An unverified defect written as fact is a lie, and in a
  repo whose first rule is never-disguise-failure it is worse than the original misread.
- **Check:** *slow ≠ hung* — before "hung", compute expected duration (tests × per-test), confirm CPU-climb / last-completion
  times on the RIGHT process, and never kill a run you haven't proven wedged (this is the 2026-07-02 ledger rule, re-broken).
  `$LASTEXITCODE` after a native pipe is valid ONLY if nothing downstream early-closes it (no `Select-Object -First`; use
  `-Last` / collect-then-filter / `Out-Null`). win-asan exe ⇒ MSVC ASan DLL dir on PATH (`0xC0000135` = missing DLL,
  BUILDING.md §win-asan). A tidy "0 warnings" is real ONLY if it PARSED — confirm "N warnings generated" (raw clang-tidy on an
  MSVC PCH errors and false-cleans; pass `/Y-`). And **whole-repo build/test is CI's job** — locally build+run only what you
  changed + its blast radius; a whole-repo sweep is a multi-hour job you must not launch on the host.

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

- 2026-08-15 — **Added rule #11 (suspect your own instrument)** after manufacturing FOUR phantom failures in one session:
  a `Select-Object -First` fake-255; a healthy ~3 h whole-repo win-asan `ctest` misread as "hung" then KILLED at 4175/6384,
  with a fabricated "2-hour ASan deadlock" written into four docs and told to the user (all retracted — `B14-c` passes in
  3.5 s); a win-asan run without the ASan DLL PATH (fake `0xC0000135`); a raw clang-tidy over an MSVC PCH that false-cleaned.
  Reinforced the user directive **whole-repo build/test = CI's job** (AGENTS.md conduct + BUILDING.md §Per-slice banner) and
  scrubbed the four false records.

- 2026-08-07 — **Repository-wide doc-hygiene pass (rules #2/#6 applied to the docs themselves):** `context.md`
  2,012 → 108 lines (history moved VERBATIM to `docs/sessions/2026-08-07-context-md-history-archive.md` — the
  2026-07-06…16 + 08-01/02 blocks were the ONLY narrative record of those days); `debt.md` 982 → ~360 (closed
  entries deleted when session-log-homed, orphans salvaged into the hygiene log's appendix); ROADMAP status table
  rebuilt honest (D-007 named as the live front; renderer-era rows annotated RETIRED); systems index refreshed + the four
  retired-module overviews (rhi/rhi-compute/renderer/shader) DELETED per user direction (git history keeps them;
  the index's Retired note points at successors); AGENTS/PRINCIPLES cornerstones annotated to
  gpu-context/RAF reality; ADR tag index extended 0076–0107; ADR-0032's recorded supersession (by 0106) struck in
  place per the SUPERSEDED rule; 36 research dossiers stamped with outcomes; BUILDING smoke list purged of the 9
  RET-deleted smokes; 5 broken doc links fixed; a stale scope comment in `deterministic.hpp` corrected (only
  non-doc touch). Canonical source-of-truth table added to `docs/README.md`. Full report:
  `docs/sessions/2026-08-07-doc-hygiene-pass.md`. Rule reinforced: **a living doc that restates status owned by
  another doc WILL go stale — state facts in one home, point everywhere else.**

- 2026-07-25 — **A "content hash" that memcpys a POD hashes STACK HISTORY, not content — and the tool that was blamed
  was innocent twice over (rules #1/#4/#5/#10).** The parallel-cook dedup failure (D3/D5/D6/D8/D10/D12 red on
  win-asan/win-shipping, green on win-debug) had been handed over as "address-dependent ordering in the KIR→GLSL emitter",
  with a glslang serialization mutex added on the theory that shaderc carried process-global state. Both wrong.
  `serialize_graph` blasted the POD pools raw, so every INDETERMINATE PADDING byte of `KNode`/`KStmt`/`KType`/`KEntry`
  entered the hash — the builders default-initialize (`KNode n;`), so the "content" hash was a function of whatever the
  stack held. A purpose-built gate (build the same graph twice with `dirty_stack(0xAA)` / `dirty_stack(0x55)` between)
  reproduced it in one run and named the byte: **offset 33 = `KNode+1`, the hole between `KOp op` and the 2-aligned
  `KType type`.** win-debug was structurally blind because MSVC `/RTC1` 0xCC-fills locals *deterministically* (rule #4:
  name what your diagnostic cannot see). Root fix = a canonical packed padding-free encoding (bonus: the artifact is now
  ABI-independent). A SECOND instance in the same slice survived that fix — `reflect()`'s `ShaderReflection` is written
  RAW into the `REFL` chunk, and diffing the two cooked cache files pinned the divergence at **file offsets 1881..1883**,
  the 3-byte hole after `KStage stage`; note `T r{}` is NOT sufficient under MSVC (it runs the implicit default ctor and
  leaves the holes), only an explicit `memset` is. The mutex was deleted once the root was found (rule #1). Rules: a POD
  that becomes a hash or an artifact must be serialized FIELD BY FIELD; and when a bug appears only under ASan/parallel,
  suspect *layout*, not *ordering*, before you blame a third-party compiler.

- 2026-07-25 — **The "non-deterministic upstream clang-tidy crash" was the HOST running out of commit — and the tidy gate
  had been analysing a configuration we do not ship (rules #2/#4/#6).** win-tidy died with a different check on a
  different SIMD file every run; two checks had been disabled in `.clang-tidy` to dodge it, and the third instance was
  written off as "not deterministically fixable". Reading the actual log ended it: the message is `LLVM ERROR: out of
  memory` with `Exception Code: 0xC000001D` (LLVM's own abort), **not** the reported `0xC0000005`. Measurements: a
  clang-tidy edge peaks at 200–300 MB; the crashed files run **0/5 standalone** and **0/6 through the exact
  `__run_co_compile` command**; the host sat at **83 GB of a 96 GB commit limit** with Visual Studio (8.7 GB), clangd
  (8.0 GB) and a DAW resident. `malloc_allocator.cpp` — a tiny file — crashing at `<eof>` is the clincher: ambient
  pressure, not TU complexity (a failing heap request gives the OOM; a stack growth that cannot commit gives the AV).
  Both disables reverted; both sweep scripts gained a commit-headroom preflight that measures free commit and clamps
  `-BuildJobs`, so this can never again read as a code failure. **The bigger find underneath it:** clang-tidy silently
  DROPS every `/`-spelled MSVC flag arriving via the compile command — proven with an `#error`-guarded probe through
  the exact CMake path — so `/EHsc` and `/arch:AVX2` never reached the TU: exceptions looked disabled (any `try` = hard
  error, the real `filesystem.cpp` failure, and `bugprone-exception-escape` could never fire) and `__AVX2__` was
  UNDEFINED, meaning **every AVX2-guarded path was preprocessed out and never analysed** — rule #2's exact shape, a
  green you cannot trust. Fixed via `--extra-arg=`, the ISA flag exported by the same CrdSimd branch that sets the real
  one. Correcting it unmasked **120 findings** that had accumulated while the build died at edge ~18/1070; all fixed.
  Rule: when a gate crashes at random, read its actual exit signature before theorising — and periodically prove the
  gate sees the flags you think it does.

- 2026-07-18 — **A green test binary is not a green slice — the close-out full ctest+shipping peeled a five-layer onion the
  binaries had hidden (rules #2/#10; B16 close).** Closing B16 = "just tidy + sweep" turned into: 12 non-ASCII TEST_CASE names
  (guard red + ctest-unselectable under CP1254), 3 `std::pow` in `ckir_ocean.hpp`, and a win-shipping-ONLY `C4789` — all
  pre-existing, all missed because prior B14/B15 sessions verified with `crd-*-tests.exe` not `ctest`+shipping. The C4789 was
  the `#deps 0` landmine again (win-shipping alone carried the English `msvc_deps_prefix` + a VS-bundled `CMAKE_COMMAND` it
  re-armed on a GLOB reconfigure ⇒ a stale pre-`m_stmts` 136-byte `KGraph` obj; every OTHER config had the Turkish prefix,
  which is why only shipping failed) — wiped + reconfigured with standalone CMake, clean rebuild green. The untagged-physical
  guard was resolved by a **full units typing** of the CKIR sim/GI configs — the gold-standard rule crystallized: *type where a
  real dimension exists (Length/Velocity/Acceleration/Angle + a custom InverseLength for 1/km extinction), keep genuinely
  dimensionless tuning knobs raw with an honest marker; `Dimensionless<f64>` on a knob is ceremony that catches nothing.* All
  km/(1/km) round-trips are bit-exact (×1000⇄÷1000 verified) so GPU==oracle held. Verified: win-debug full 5061 + a **focused**
  build+test of only the 7 touched targets on shipping/asan/release/tidy (CI owns the full matrix — don't re-grind untouched
  modules for a localized diff). Transient LTCG `C1001` cleared on retry (known). Rule: at a real slice close, run `ctest` +
  the LTCG configs, not the binary; and a config-specific build failure on ONE dir is a stale-obj/deps fingerprint, not code.

- 2026-07-15 — **A missing upload barrier is a grid-size time bomb (rules #4/#10; B16-a-3 multi-cascade):** the shared
  `dispatch_fft2d` harness copied inputs then dispatched pass 0 with **no `TransferDst→ShaderRead` barrier**, so a large-enough
  grid raced the still-in-flight upload and read STALE data. Latent for the whole GPU-FFT campaign because every prior 2-D test
  was small-batch (single image / batch ≤ 8) and the upload happened to finish first; B16-a-3's batched IFFT at batch = 4·C
  (C≥3 ⇒ batch ≥ 9, grid > device occupancy) first exposed it — **flaky, ~all-wrong past the threshold, bit-exact below**. The
  non-determinism (bad counts differed run-to-run) is what fingered it as a race, not an offset bug; the `[.ocean-ifft-bench]`
  self-verifying at batch 64 proved the KERNEL correct and localized it to the harness. Root-fixed (the exact `dispatch_1wg`
  upload-barrier scar, [[feedback_dispatch_1wg_missing_upload_barrier_race]]); batch 8–16 now bit-exact on Vulkan + DX12.
  Lesson: a multi-pass GPU harness needs the upload→first-read barrier as much as the between-pass barriers; small-grid tests
  never prove it.

- 2026-07-10 — **The ORACLE was more accurate than the kernels it certifies — a reference that cannot be matched cannot certify (rules #2/#4/#9; D-007 B0 fan-out):** `ckir_eval`'s A3 vec/mat corpus (Dot/VecLen/Normalize/Cross/MatVecMul/MatMatMul/Determinant/MatInverse/OuterProduct/geometric/quats/Slerp) accumulated in **f64 and rounded only on store**, while the elementwise ops and `Contract` rounded **every step**. For an F32 graph the oracle therefore computed an f64 dot rounded once, where any f32 GPU rounds each multiply-add ⇒ ~1 ULP delta with the reference wrong-by-being-better, leaving **ADR-0098's T1 certified-bit-exact core unreachable for vec/mat — i.e. for most of a shader**. It hid because **every Vulkan/DX12 vec test compares against ANALYTIC references with tolerances, never against the oracle** — nobody *could* gate bit-exactness against it, so nobody noticed it couldn't be matched (a structural blind spot: the reference itself was un-referenced). Surfaced only when the CUDA fan-out — scalarized, hence emitting explicit elementary ops with `--fmad=false` — first pointed the oracle at a vector graph and asserted `==` (320 mismatches). Fixed at the root (`eval_detail::rnd`; `mat_det`/`mat_minor_det` take the dtype): every elementary IEEE op rounds to the node dtype. Safe to retrofit — `round_dtype` is the identity for F64, so the 200-assert CPU suite passed unchanged first try. **Payoff:** CUDA vec3/mat3/bvec/struct now gate **bit-exact (`==`)** vs the oracle, the strongest gate in the repo. **Named, bounded gap:** GLSL/HLSL/WGSL call `dot()`/`normalize()`/`inverse()` **builtins with implementation-defined internal order**, so those stay ULP-tolerant until ADR-0098 §5's `float_controls` audit — stated, not assumed. Rules: bit-exactness needs BOTH (a) an oracle that rounds per elementary op and (b) a backend emitting the same ops in the same order with no FMA contraction; and *a reference whose tests only ever compare against analytic values has never been proven matchable.* Same session, same shape: the tidy gate that printed PASS without parsing, and `KirBackendCpu` ignoring `comps()` (vec graphs heap-overflowed) — three instruments, none of which had been pointed at themselves.

- 2026-07-05 — **MSVC /O1+/O2 auto-vectorizes a per-lane conditional TWO-ARRAY update WRONGLY — write lane logic as manual vector select chains (rules #4/#5/#10 compound; v14-h LU):** the batched-LU lane tier returned provably-false pivot comparisons on raw data, win-shipping only; /Od + gcc green, **ASan structurally blind (wrong-code, no bad access)**, fprintf-in-loop suppressed it (heisen). THREE plausible theories measured and KILLED first (stack UMR via poison-fill; alias-reorder via atomic_signal_fence; LTCG inline mis-scheduling via noinline seams — each zero effect); a 60-line standalone repro + flag bisection then pinned the true construct: MSVC's auto-vectorization of `if (v > best[q]) { best[q] = v; pr[q] = i; }` (masked blends over two arrays) — NOT LTCG (reproduces without /GL), NOT /O2-specific (/O1 too). Root fix at the construct for all consumers: pivot scan = pure manual-vector argmax (cmp/select, indices in f64 lanes); the same conditional-two-array shape hardened in the chol/LU singular checks. The fix measured FASTER than the miscompiled loop (LU 1.27–2.87× → 1.76–3.81× vs MKL). Rules: per-lane scalar `if (...) { two updates }` loops next to vector code are FORBIDDEN in lane kernels — express them as select chains; and a theory is not a fix — each of the three plausible mechanisms would have shipped as a lie without the measured refutation.

- 2026-07-05 — **A borrowed-lifetime member in a returned object is a cross-config time bomb (rules #1/#2/#4 compound; v14-g):** `HyperTree` stored `const HyperNet*` for index metadata; the driver's finalist path built trees from a lambda-LOCAL net ⇒ every later `stats()` read freed memory. **gcc -O3 was fully green (heap-reuse luck), MSVC-debug SEGV'd at a distant destructor, win-asan converted it to a precise OOB assert** — localized with rule-#10 flushed markers then ASan (match the tool to the bug class). Root fix: the tree OWNS copies of sizes/appearances — never a borrowed lifetime in anything that outlives a call (the allocator-outlives-borrowers scar, reference-member edition). ⚠ Rule-#2 tail: the pre-fix bench board contained garbage-derived values that looked BETTER; re-measured on the fixed artifact + corrected. Same session: an exact-value gate caught a pool-reallocation UAF in `merge_legs` (reserve-before-spans) — two UAFs, both found by discipline (exact values + cross-config), neither by the green gcc suite.

- 2026-07-05 — **The /Od stack-bomb scar has an LTCG sibling: `__forceinline` generated kernels are a link-time compiler-heap bomb (rules #1/#5):** the win-shipping FFT link died after ~40 min with C1002 (out of heap, pass 2) at `fft.hpp(895)` — MSVC honors `__forceinline` under LTCG, so 56 generated codelets (bodies to ~9K lines, 143K-line header) + both `execute_ip4aos` instantiations inlined into the ONE `execute()`; the mega-function exhausted pass-2 codegen heap (17 GB WS observed; system RAM never the limit). Root fix at the emitter, MSVC-scoped: `CRD_FFT_GEN_INLINE` (MSVC = plain `inline` — its /O2 cost model declines giant bodies; gcc/clang keep always_inline ⇒ measured boards untouched) in `batched_codelets_gen.hpp` + `gen_fft_batched.py`, plus a `__declspec(noinline)` seam on `execute_ip4aos`. Relink green, shipping ctest 29/29. Rule: giant generated straight-line kernels must never carry an unconditional force-inline — every unbounded inliner (debug stack, LTCG heap) eventually detonates; put the attribute policy IN the generator.

- 2026-07-03 — **A giant generated straight-line kernel is a debug-build stack bomb — measure the frame, fix at the emitter (rules #5/#10):** the first win-debug run of the FFT standalone-hier/deep-split paths SEGFAULTed (0xC00000FD); the mechanism was MEASURED before fixing (dumpbin `sub rsp` probe): at `/Od` MSVC gives every expression temporary its own un-reused slot ⇒ the 256-point codelet frames are **1.17–1.4 MB EACH** (~236 B/SSA value) — one call overflows the 1 MB Windows default stack; linux never sees it (8 MB stacks) and `#pragma optimize("gt",on)` in a /Od compiland does nothing (tested — frames stayed ~1 MB). Root fix at the GENERATOR, all consumers: dual-body emission — SIMD tiles under `NDEBUG||__OPTIMIZE__`, a lane-scalar edition (same DAG, same order per column ⇒ bit-identical; `0−x` not unary minus for signed-zero identity) otherwise, frames 23–57 KB measured. ⚠ the fix's own scar: the first gate (`NDEBUG` only) silently sent ad-hoc `g++ -O3` bench builds (no `-DNDEBUG`!) to the scalar bodies — caught by re-running the board after the regen (rule #2: verify the shipped artifact), closed with `__OPTIMIZE__`.

- 2026-07-02 — **A std concurrency primitive lost the wake — own the primitive (rules #1/#4/#5 compound):** the red 18-config CI (a DIFFERENT jobs-parallel moat test timing out >1500 s each run, Linux-only) was root-caused by *reproducing* it (4-CPU-pinned taskset loop in WSL, hangs at iters 6-76) then *dumping* it: CPU-ticks-over-5s == 0 killed every spin/livelock theory in one number; gdb stacks showed main in `shutdown()→join()` + one worker in `counting_semaphore::acquire→futex`; `/proc/tid/syscall` + `x/dw` on the futex word proved the worker asleep with **expected==1 while the counter word read 1** — a token present, no wake ever coming. Mechanism: GCC 13.3 libstdc++ preloads the futex expected BEFORE the predicate spin (`_S_do_spin`) and skips the wake when the counter was already >0 (`_M_release`, which carries its own FIXME) — the PR104928 class; our protocol above it was sound. Fix at the root: `crd::jobs::detail::Semaphore` (futex/WaitOnAddress, sleep only at observed-0, release always wakes) — the LAST std concurrency primitive in the jobs hot path is now Cerid-owned like the fibers/deques/MPMC. Repro loop: 300 clean post-fix. Lesson: a "can't-be-our-code" hang deserves the same forensics as our code — and a paper-correct protocol proof does NOT extend to the primitive it stands on (rule #4: name what your proof is blind to).
- 2026-07-02 — **A timeout is not a hang proof (rule #5's face of the binomial scar, inverted):** test 2661 (the fat-front NODE-PARALLEL moat) "hung" win-asan — a 150 s standalone timeout + a killed ~40-min sweep "confirmed" it, and the first diagnosis ("genuinely hangs, fiber/ASan bug") was WRONG. The real evidence: the win-debug baseline was **456 s** (a huge /Od test), the "hung" process's CPU climbed linearly, and thread stacks (a purpose-built DbgHelp `stackdump.exe` — no cdb on this host) showed the main thread hot in `gemm_microkernel_avx2_f64` with `_asan_loadN` on every SIMD load (ASan-on-debug ≈ 5-6×) and all workers parked in `WaitOnAddress` — **slow, not stuck; no deadlock, no fiber annotation gap**. Root fix, not a timeout bump: replaced grid3d(28) with `bordered_spd(24,48,560)` (dense border = a 560-col root supernode receiving cmod from 24 block supernodes — the SAME divergent paths, guard-enforced by `maxnc≥512` + a new `nsuper≥min` REQUIRE) ⇒ **456 s → 9.3 s debug, ~40 min → <1 min asan**, 53,214 asserts green both configs. Check before ever calling a hang: (1) the same test's baseline in a lighter config, (2) CPU-climb over minutes, (3) thread stacks.
- 2026-06-30 — **v13 1-D SCALAR quadrature ENGINE (v13-g/h/i) closed with ZERO open comparisons** (⚠ the quadrature MODULE is NOT done — the oscillatory v13-j {QAWO/QAWF/QAWS/QAWC/Levin} + multi-D cubature v13-k {Genz-Malik/Smolyak/Lebedev} rows are still pending; honest scope, not "module complete") — every IMPLEMENTED method (v13-g composite/Gauss·Lobatto/Radau/Newton-Cotes + v13-h adaptive QUADPACK QNG/QAG/QAGS/QAGP/QAGI + v13-i DE tanh/exp/sinh-sinh/Clenshaw-Curtis/Fejér/Romberg) beats EVERY available frontier peer: scipy + MATLAB + Boost + **GSL 2.7.1** (installed mid-session the moment the user demanded the full board). **The recurring crush lever (a fresh face of #5/#9): integrand-INDEPENDENT work must be precomputed ONCE, never per call** — it was the inefficiency behind FOUR losses that all flipped to wins: (1) the GK error estimate's `crd::math::pow(·,1.5)` (heavy double-double) → `x·√x` (one hardware sqrt) = QAGS 0.88×→**1.29×** / QAGI 0.89×→**1.36×** GSL; (2) Gauss symmetric-pair = parity→**edge** vs Boost `gauss<10>`; (3) the DE convergence estimate `d²/dₘ₋₁` (the double-exponential rate, halving the levels) = exp_sinh 0.48×→**1.17×** Boost; (4) the Clenshaw-Curtis O(N²) clencurt weights recomputed per call → precomputed `CcAdaptiveRule` = 0.59×→**2.05×** GSL-cquad. **Meta-scar: the user REFUSED "near-parity with the reference C is the ceiling"** ("I don't accept near-parity, CRUSH") — and was right every time: parity-with-the-same-algorithm is NEVER the wall; a per-operation cost (a heavy pow, a per-call recompute) always is. **Method that found every win: reconstruct-and-verify-in-python FIRST** — fetched scipy's `_interpnd.pyx` / `__quadpack.c` (the dqagse/dqelg/dqng C) / `_rules/_gauss_kronrod.py` + QUADPACK constants via `gh`, verified the algorithm bit-exact in python before porting a single C++ line (caught a Clough-Tocher gradient sign-flip + the GK roundoff-floor pre-port; faithful goto-preserving transliteration = the v7 NLopt-port discipline). Full peer board (scipy+MATLAB+Boost+GSL) on EVERY row; N/A stated with the check, never dropped (`feedback_bench_all_peers_never_cherry_pick`).

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
- 2026-06-28 — Added **rule #10** + root-caused/fixed the `wpt` CI SegFault: MSVC 19.51/14.51 `/O2`+LTCG miscompiled the inline `crd::usize{1} << level` in `WaveletPacket`'s ctor, `best_basis`, and `reconstruct` to a garbage stack-address value (`count=140698301264476` at `level=2`) ⇒ ~1e14-iteration inner loop ⇒ OOB ⇒ SegFault. **win-release/shipping only**; local MSVC 14.50 + gcc green; **win-asan blind (ASan disables LTCG)**. Found via a minimal `branches-ignore:[main]` CI job + flushed-stderr markers (escalated to numeric values when structure looked right). Fix: iterate by heap id (ctor + best_basis) / halving counter (reconstruct) — no in-loop variable shift; **verified green on the CI's exact 14.51**. Also fixed a clang-cl `-Wunused-lambda-capture` (constexpr `cap`). **3 wrong guesses first** (older-MSVC, reverse-idiom, NOINLINE) — the numeric marker was decisive.

- 2026-07-10 — **Two Accepted ADRs contradicted each other, and the plan followed the wrong one.** ADR-0101 says backend languages are outputs only, never authored or stored; **ADR-0099 §6 says `crd-shader` IS the shared GLSL/HLSL→SPIR-V/DXIL compiler** that CKIR routes through. Both Accepted. The D-007 B3 plan cited §6 and proposed gating the raster emitters on `crd::shader::compile_glsl(Stage::Vertex)` — making CKIR *depend on* a GLSL compiler, the exact inversion ADR-0101 exists to delete. Caught by the user, not by review. **Scar → rule:** an ADR that supersedes part of another must **strike the superseded clause in place, in the old document** (done: 0099 §6 is struck through and points at ADR-0103), because the next reader lands on the old text and follows it. A "Superseded" line in an index nobody reads is not a check. Corollary of rule #2 (verify the *shipped* artifact): the shipped artifact of a decision is the sentence someone will read six months later.
- 2026-07-10 — **A green test binary is not a green ctest.** `crd-kir-tests.exe` printed "All tests passed" while **6 of its cases could not be selected by ctest at all**: their `TEST_CASE` names contained an em-dash, and Windows decodes ctest's argv through the Active Code Page (CP1254 here), so Catch2's filter matched nothing and ctest reported `Failed`. The repo already has a ctest-registered guard for exactly this (`crd-no-non-ascii-test-names`) and it was **red**, plus `feedback_ascii_only_test_names` in memory — the rule, the check, and the memory all existed; only *running ctest* was skipped. 9 names fixed (kir + kir-vulkan); guard green. Reinforces `feedback_per_slice_run_ctest`: **guards are ctest-registered, so a binary-direct run cannot see them.** Also: a bare `ctest` outside vcvars fails `crd-simd-emission-check` (`dumpbin` not on PATH) — use `scripts/run-ctest.bat`, which sources vcvars, or you will chase a phantom.

- 2026-07-27 — **A shader pair can disagree and there is no layer that can tell you.** The scene’s SKINNED vertex program emitted 2 of the 4 varyings every cooked fragment program reads, so a skinned draw shaded from UNDEFINED interpolants at locations 2 and 3 — it linked, it bound, it rendered, and neither Vulkan validation nor the DX12 debug layer can see it. Found only when the varying set became DECLARED and a cook-time contract check (38-D4) compared the two by name, location, width AND interpolation. **Rule:** when two artifacts must agree and no runtime can check it, the agreement has to be a DECLARED contract verified at cook time — a convention is not a check. A name-only check is not one either: it passes a vec2-at-location-3 against a vec3-at-0.
- 2026-07-27 — **A helper’s documented "caller error" arm builds a graph that compiles the wrong thing.** `lighting::pcf_shadow` takes a **vec2** uv (a 2-D shadow map); every atlas in this engine is LAYERED. Passing a vec3 hits `nodes::detail::bin`’s *"two mismatched vectors — a caller error; leave to the shape checker"* branch, which emits a shape-invalid node: the COOK returns a valid node id and the SHADER fails to compile, with nothing in the message pointing at the uv width. **Rule:** a library arm that says "caller error" and returns a value anyway is a silent failure at the call site — read the operand WIDTHS a helper assumes, not just its parameter names. Rule #2’s shape: the artifact to trust is the emitted shader, not the graph that built cleanly.
- 2026-07-27 — **A convention documented one function away is a convention you will still walk into.** `scene_renderer.cpp` carries an explicit comment that the frame header stores the direction TOWARD the light while `lighting::directional_light` wants the direction light TRAVELS (it negates internally) — and I copied the header value straight into the new light record anyway. N·L ≤ 0 everywhere: a uniformly dark frame that still DRAWS. **Rule:** cross a convention boundary in exactly ONE place and name it there; the second copy of a negation is the one that is missing. Same class as the CSM shadow-camera inversion — two libraries, two conventions, no type to catch it.
- 2026-07-27 — **`parse_*_toml` never reset its output descriptor**, so parsing a second asset into a reused one APPENDED: overlapping names surfaced as `DuplicateName` (an error naming the wrong thing) and distinct names as a silently MERGED layout. Present in two independently written cookers. **Rule:** a parse-into-out-param owns the WHOLE of that object — reset first. Any tool with a load button hits this on the normal path, not as an edge case, which is why "the tests construct a fresh desc each time" hid it.
- 2026-07-27 — **Two argument kinds spelled identically in C++, and only a coverage gate could see it.** In `ckir_nodes.hpp` a WIRE (node id) and a COMPILE-TIME ATTRIBUTE (`extract`’s channel index, `convert_f_vec`’s width, `place2d`’s order, a geometric reader’s varying `location`) are both `int`. A generated op registry passed node ids into all of them: type-checks, builds, swizzles component 47. The same gate found `kMaxNodeInputs = 5` while the widest node takes SEVEN (making `gooch_shade`/`range` unauthorable, rejected for a reason that named the wrong thing). **Rule:** when a registry is generated from signatures, the generator must know what each argument MEANS — and the gate that proves it must CONSTRUCT every entry from the registry’s own account of its slots, not merely list them.
- 2026-07-27 — **Four gates of my own that could not fail.** Probe constants that also occur in the baseline (0.25 is a tint component; 0.8 is `surface_defaults`’ base colour); an IES baseline whose record already declared `ies_index` (comparing the feature with itself); a displacement check that only asserted the literal was PRESENT (passes with the node built and never wired — fixed by running both graphs through B7 `lower_entry` so DCE removes what the entry cannot reach); and a clustering check that would have passed with the cluster list declared and ignored (fixed by asserting the unrolled program gets SMALLER). **Rule:** before trusting a gate, ask what value would make it fail — if the answer needs the baseline to change too, the check is measuring nothing.
- 2026-07-27 (audit pass) — **The checker a comment defers to must EXIST, and its rules must come from the oracle.** Built `ckir_shape.hpp` (the checker `detail::bin` had always named); on its FIRST corpus run it found two latent defects in shipped light-cook paths: the PCSS blocker search reading depth through the COMPARISON sampler (an overload that does not exist in GLSL — the cook succeeded, the shader never could compile) and contact shadows feeding SCALARS to the 4-tap `contact_shadow` helper (lanes 1..3 of a 1-wide value; the close gate had measured node counts). **Rule:** derive validator rules from the oracle's actual read loops, wire the check at the cook boundary with a POINTING error, then run it over everything that already exists — the first catches are usually real.
- 2026-07-27 (audit pass) — **Byte-identity round-trips are blind to fields BOTH sides drop.** Frame blob v3 silently lost every post-REN-36 pass field (the RT pipeline's three program names, VRS, queue, sampler, filter) and the vertex emitter lost the per-stage parameter sections — while both round-trip gates stayed green, because a field dropped by the writer AND the reader round-trips "byte-identically". And the shipped `.crdl` was outright CORRUPT because it was an inert copy nothing ever parsed. **Rules:** serializer gates assert FIELD SURVIVAL (parse → cook → read → field-by-field against the parsed original), and two copies of one declaration get a CANONICAL-FORM drift gate the day the second copy is born.
- 2026-07-27 (audit pass) — **A backend half proven by compile only hides its whole dispatch path.** A16's DX12 RT pipeline had never been RUN by a device gate (HLSL-lowering compile only) — so the HLSL emitter's missing any-hit entry arm, the lazy `supports_rt_pipeline()` (the exact scar A16 fixed on Vulkan, alive in its DX12 twin), and the opaque-geometry any-hit skip all sat unfound until the first real DispatchRays gate ran. **Rule:** "both backends" requires one EXECUTING gate per backend with a distinguishable assertion; capability queries answer from the feature check, never from lazily created state.

### Open sanity backlog (small, claimable)
- Harden the remaining in-loop `crd::usize{1} << <loop-var>` shifts (swt.hpp `dil`/`step`, modwt.hpp `dil`) against the rule-#10 MSVC-LTCG miscompile — they pass on 14.51 today (CI-proven), but are the same fragile pattern; prefer a non-shift form (doubling counter / heap-id iteration) when next touched.
- Trim `MEMORY.md` back under its session-load limit — do it incrementally, don't risk losing info. **2026-07-10: 20,756 → 18,699 bytes by tightening hooks; all 175 links verified present before/after (`comm` on the extracted link sets). Still ~1.2 KB above the 17.1 KB advisory target — closing that gap needs entries MERGED or DROPPED, not hooks trimmed (593 non-ASCII chars cost ~1.2 KB alone, and the ⛔/⭐ scan markers earn their bytes). Hard read limit is 24.4 KB, so the index loads fully today.**
- Adversarial boundary-test pass on `crd-containers` (String/Array/HashMap capacity-edge cases).
- Confirm (don't assume) no other heavy-churn `GrowableTlsfAllocator` consumer was relying on the old behaviour now that `init_pool` is fixed.
- **Doc bloat (living/scannable class only — never truncate session logs/ADRs/dossiers):**
  - ~~Collapse `docs/ROADMAP.md` status-table rows to one line per phase~~ — ✅ DONE 2026-08-07 (doc-hygiene pass; the discipline stays: phase history belongs in the phase doc, never in the hub).
  - ~~Prune **closed** entries from `docs/debt.md`~~ — ✅ DONE 2026-08-07 (982 → ~360 lines; salvage in the hygiene session log). The rule is RECURRING: prune at every close, don't re-accumulate.
