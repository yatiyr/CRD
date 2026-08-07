# Session 2026-08-07 — repository-wide documentation hygiene pass

**Focus:** make the doc corpus trustworthy again — accurate current-state claims, explicit source-of-truth
ownership, history preserved and *labeled*, living dashboards actually small. User-directed quest (full spec in
the session prompt); no feature/architecture work. All changes UNCOMMITTED at session end (user commits).

## 1. The documentation architecture (final roles)

| Doc | Role |
| --- | --- |
| `README.md` (root) | project introduction + navigation; module areas point at `docs/systems/README.md` |
| `AGENTS.md` | stable working rules ONLY — the module inventory + current focus now live elsewhere by pointer |
| `context.md` | the current-work dashboard, back to ~110 lines (was 2,012 / 318 KB) |
| `docs/README.md` | the doc map + **the new canonical source-of-truth table** ("which document wins") |
| `docs/SANITY.md` | invariants + scar→rule→check ledger; the two doc-bloat backlog items struck as DONE + a dated ledger entry for this pass appended |
| `docs/PRINCIPLES.md` | durable principles; the Render-path + Culling cornerstones annotated superseded-by-events (ADR-0105/0106) — decision text preserved |
| `docs/ROADMAP.md` | phase hub with an HONEST status table (D-007 named as the live front; one line per phase) |
| `docs/systems/` | current-truth per-module overviews; retired modules: overview DELETED with the module (user direction, this pass) — the index's Retired note points at successors, git history keeps the text |
| `docs/decisions/` | ADRs, historical integrity kept; tag index now covers 0076–0107; supersessions annotated |
| `docs/detours/D-007-…` | the live campaign doc, now with a "Reading this document" 4-kinds-of-truth frame |
| `docs/research/` | dossiers, each stamped `Outcome: adopted / absorbed / executed / research-only` |
| `docs/sessions/` | chronological history (untouched except this log + the context archive below) |
| `docs/debt.md` | OPEN items only (982 → ~360 lines); dispositions + orphan salvage in this log's Appendix A |

## 2. The two big moves

- **`context.md` history → `docs/sessions/2026-08-07-context-md-history-archive.md`** (~1,960 lines moved
  VERBATIM, provenance header added). Rationale: session-log coverage of 2026-07-06…07-16 (the GPU FFT/sort/NRC
  crush campaigns, B14/B15/B16) and 2026-08-01/02 (REN-40 closes) is **absent** — those context blocks are the
  only narrative record of those days, so they were moved to the append-only area, not deleted.
- **`docs/debt.md` pruned** per its own rule: closed entries with a named session-log home deleted
  (git-recoverable); closed/superseded entries WITHOUT a home moved verbatim to **Appendix A** below with a
  one-line disposition each (incl. the whole Material-v1-gaps / GPU-instancing / renderer-backlog sections, all
  superseded by ADR-0104/0105/0106).

## 3. Important corrections (old claim → why stale → new state)

| Old claim | Why stale | Fixed where |
| --- | --- | --- |
| systems index: `crd-rhi` ✅ / `crd-shader` ✅ / `crd-renderer` 🚧 as current | modules DELETED at RET-8 (2026-07-23, ADR-0105) | `docs/systems/README.md` Retired note; the four retired overviews (rhi/rhi-compute/renderer/shader) first got ⛔ banners, then were **DELETED outright per user direction** (git history keeps them; historical mentions in old ADRs/phase docs left as written) |
| ROADMAP: "1.6 config 🚧 active", "2.4 Renderer v1 🚧 active", "3.1.7 geometry in-flight", no 3.1.6 row, no D-007 | frozen mid-May; renderer deleted; geometry closed 2026-05-19; the real front absent | ROADMAP status table rebuilt; D-007 row added as the live front; hesap row added |
| context.md: "Active detour: none", "Last shipped 2026-06-07 v6", "v10 FFT IN PROGRESS UNCOMMITTED" | June-era fossil tail | new dashboard (D-007 active; recently-landed = RAF/post-RAF) |
| AGENTS: "Phase 3.1.6 the current focus", "hesap-diff/motion next", rhi/renderer/shader listed as shipped, `crd::rhi::ValidationCapture` | motion/diff shipped; modules retired; namespace is `crd::gpu::` | AGENTS "What is Cerid" now points at systems index + context.md; cornerstones updated (ADR-0103/0105/0106); ValidationCapture namespace fixed |
| root README: Graphics = `rhi·shader·renderer`, "tensors (in progress)", "Planned next: autodiff", "MSVC 2022+/Vulkan 1.3+" | retired modules; v14–v16 shipped; SDK pin is 1.4.341 | module table rebuilt (gpu-context/CKIR/RAF); differentiator bullet added; requirements corrected |
| detours README: "NEXT = B3-c … then main roadmap resumes at v17" | a month of landed work later | landmark state as of 2026-08-07 + "status not restated here" rule |
| phase headers: 1.6/2/2.3/2.5 "active", eylem "v1b in flight", units "planned", v13 "z remains", v15 "OPEN", v16 "KICKOFF", hesap "ACTIVE: v14", v17 (no status) | all closed/paused/absorbed since | honest status headers, each labeled "(Header refreshed 2026-08-07)"; v17 marked ABSORBED INTO D-007 |
| ADR-0032 "Status: Accepted" plain | ADR-0106 records superseding its runtime-ownership half — the strike-in-place rule was violated | struck in place in 0032 (contracts preserved note) |
| ADR tag index ends at ~0080 | 0081–0107 (agent-native, hesap clusters, the GPU-era north stars, RAF, UI/2D) missing from the lazy-load surface | new tag sections added; era notes on `[rhi]`/`[shader]`/`[renderer]`; 0104/0105/0106/0107 chronological ordering fixed |
| research: `cerid-geometry` "planned after eylem v1d", v5b-3 "SKELETON LANDED", gold-standard mission unclosed | phases closed since | 36 dossiers stamped with Outcome/Status lines |
| `deterministic.hpp` header comment: v0c deferral list "NOT in v0c" + pointer to a debt entry | the deferrals were paid INTO that file same-day; debt entry now pruned | comment rewritten (the ONLY non-doc file touched) |
| PRINCIPLES cornerstones: "Renderer v1 ships Clustered Forward+ behind `IRenderPath`" / "per-light culling is part of clustered Forward+" | `IRenderPath`/`crd-renderer` deleted (ADR-0105); rendering is asset-driven (ADR-0106) | both cornerstones annotated superseded-by-events in place |
| BUILDING.md smoke protocol listed `smoke_shader/renderer/imgui_overlay/rhi_api/rhi_vulkan_bootstrap/material/resources_render/asset_import` | those nine smokes were DELETED at RET-7 | smoke lists rebuilt from the actual `runtime/examples/` tree; `crd-sandbox --smoke-test` named as the real GPU smoke |
| benchmarking policy: "Future: `crd-renderer`, `crd-rhi` vs reference renderers" | modules retired | retargeted to the gpu-context + RAF stack |
| design-spec index: REN-36 "⬜ next", REN-37 "📐 design", REN-3 "⬜ next", REN-41-S4 "⬜ active" | REN-36/37 shipped 2026-07-25…27; REN band superseded; S4 folded into VGE | index statuses corrected (specs themselves untouched — they are historical contracts) |

## 4. Verification run

- **Link check:** repo-local Python checker over 781 md files. 22 broken relative links found (none introduced by
  this pass): 5 wrong-filename ADR/session links **fixed** (0052/0064/0065 targets, texture_resource session);
  remainder = code-snippet false positives (`(usize)`, `(u32)`, `(URL)`…) and **anticipatory links to Reserved
  ADRs 0070–0074** (files not yet minted — left, they resolve when minted).
- **Stale-language sweep** over the living docs: remaining "active/currently" hits verified genuinely current.
- **Retired-names sweep** (`IRenderPath|crd-rhi|crd-renderer|crd-shader|ForwardRenderPath|smoke_*`) over the
  living docs — caught the PRINCIPLES cornerstones, the BUILDING smoke lists, the benchmarking-policy line and
  the D-008 stub (all fixed above); remaining mentions are era notes / historical records, verified hit-by-hit.
- **`scripts/tidy-files.ps1` run on the one touched header** — my change (comment-only) is clean; the gate
  surfaced **20 PRE-EXISTING naming findings** on `crd::math` namespace-scope constants (`pi`, `tau`, `e64`…),
  which exposes a real standards conflict recorded in §6 — NOT fixed here (engine-wide rename ≠ doc hygiene).
- Session logs / bench boards / recipes / lessons / ADR bodies: NOT rewritten (append-only history), except the
  ADR-0032 strike + 5 link fixes above.

**Retired-doc deletion (user direction, end of session):** `docs/systems/{rhi,rhi-compute,renderer,shader}.md`
deleted; `docs/systems/geometry-shader-helpers.md`'s live reference to rhi-compute retargeted to the
IComputeContext reality; index/source-of-truth-table/ledger wording updated to match.

## 5. Non-doc changes (complete list)

1. `engine/math/include/crd/math/deterministic.hpp` — header comment only: the stale v0c scope list pointed at a
   pruned debt entry and contradicted the file's own contents. No code changed.

## 6. Remaining ambiguities + deliberate deferrals (not hidden)

- **ADR-0016/0017 (render-path/culling strategy)** are factually overtaken by RAF/RPL (ADR-0106) but have **no
  formal superseding ADR** — flagged in the tag index; minting a superseding note is a user decision, not invented
  here.
- **No per-module overviews** for the live GPU modules (gpu-context, kir, render-graph, draw, cookers…) —
  `rendering-foundation.md` + D-007 are authoritative; the gap is declared in `docs/systems/README.md`.
- **`docs/decisions/README.md` chronological rows** remain paragraph-length (index budget miss) — deliberately
  deferred: they are the lazy-load navigation capsules; trimming them loses value.
- `docs/phases/crd-math-transcendental.md` "PROPOSED, ADR pending" — the tx-a audit + first cluster work landed
  (2026-06-25/26 sessions) but full execution status is unproven from docs; left as-is, flagged here.
- Reserved-ADR links (0070–0074) intentionally dangling until minted.
- `MEMORY.md` deeper cull (≈19.9 KB of a 24.4 KB limit) — out of this quest's scope, still owed.
- **Pre-existing standards conflict (found by the tidy gate, not fixed):** AGENTS.md's coding table pins
  "Constexpr var → lower_case" while the clang-tidy GlobalConstant rule demands `kCamelCase` at namespace scope
  — `crd::math::pi` et al. (20 findings in `deterministic.hpp` alone) violate the gate as configured. Either the
  `.clang-tidy` naming config or the coding table needs a decision; engine-wide rename was out of scope here.
- `docs/design/` specs audited via the index only (statuses corrected); the spec BODIES were not re-read — they
  are historical contracts per the directory's own convention.
- The research stamps for a few 2026-07-23 dossiers state band-level adoption (GEO/MED/OFF) from the D-007 master
  table rather than per-row verification — the master table is the source of truth for row-level status.

---

## Appendix A — debt.md salvage (closed/superseded entries without a session-log home)

> Each block below was removed from `docs/debt.md` on 2026-08-07 and is preserved verbatim; the disposition line
> states why it left the open-debt list. These are HISTORICAL records.

---

**Disposition (2026-08-07 hygiene pass):** FIXED 2026-07-15. The fix session (B15-b clouds) has no dedicated session log (the 2026-07-15 log is the FFT-ocean one); narrative otherwise only in the context-history archive.

### ✅ `ckir-offhost-emitter-cse` — FIXED 2026-07-15 (CUDA/MSL/WGSL compute emitters now CSE like GLSL/HLSL). No remaining debt.

> **Was:** the **GLSL/HLSL** compute-kernel emitters materialize every arithmetic node as a temp keyed by node id (`temped[]`), so a
> shared subtree emits ONCE. The three off-host emitters — `emit_compute_kernel_cuda` / `_msl` / `_wgsl` — instead **inline-expanded**
> each value recursively (`ev(self, nd.a)`) with **no temp cache**, re-emitting a node's whole subtree once per reference. Shallow
> kernels (the `build_reverse` gate, FFT/transpose butterflies = one-shallow-expression-per-statement) were fine — which is why it
> was never caught — but a **DEEP shared value DAG** (the B15-b Perlin-Worley cloud density: a 3-octave Perlin FBM + the
> Burtle-Jenkins hash in one final-store expression) expanded EXPONENTIALLY: emitting it to CUDA/MSL/WGSL exhausted a 128 MB TLSF
> arena (`TlsfAllocator: out of memory`).
>
> **Fix (this session):** gave all three off-host emitters the same node-id materialization the GLSL/HLSL path uses — an
> `is_inline_op` predicate + a `decl` pass that emits every non-inline arithmetic node ONCE as a `t<node>` temp (children first),
> keyed by the existing `matd[]` array, and wired `decl(...)` before each statement's value/index in `emit_body`. Leaves + cast/
> select/compare/bitops stay inline. Determinism is unaffected (CUDA compiles `--fmad=false`; the temps only dedupe). The five-backend
> emit gate `tests/kir/test_ckir_kernel_emit.cpp` "Perlin-Worley cloud density emits on ALL backends" now passes; all existing
> off-host structural `has(...)` checks (reverse/select/FFT) were UNCHANGED (materialization only adds temp-decl lines; the checked
> substrings are signatures/barriers/inline ops). Full kir 30554/153 green. Mission "portable across ALL backends" now holds for
> deep kernels at the emit level (real CUDA/Metal/WebGPU compile+run remains ADR-0098 Part C). → this session (2026-07-15 clouds).


---

**Disposition (2026-08-07 hygiene pass):** OBSOLETE: dct.hpp has since been reworked onto crd::math trig with explicit static_casts, and the full linux-gcc build is green (CI-1, 2026-07-24). The entry describes code that no longer exists.

### `dct-gcc-f32-werror-conversion` — pre-existing gcc-f32 `-Werror=conversion` in `dct.hpp` (found 2026-06-15)

`engine/hesap-fft/include/crd/hesap/fft/dct.hpp:244` (and the sibling DCT-III/DST-III direct loops) computes
`std::sin(pi * static_cast<double>(2*nn+1) * static_cast<double>(k+1) / (2.0*n))` where `nn,k,n` are `usize`;
the **f32** instantiation (`DctPlan<float>::direct_dst3`, triggered by `test_dct.cpp`) trips gcc
`-Werror=conversion` (`unsigned long → double` may change value). This blocks the WSL-gcc build of
`crd-hesap-fft-tests`. **It is latent because the v10-f DCT slice was DoD'd only on the 4 Windows configs**
(MSVC accepts it silently) — the documented `feedback_T_double_literal` / no-`T{double_literal}` hazard class.
**Fix:** wrap the integer subexpressions in explicit `static_cast<double>` (or compute the angle from an `int`
index), then re-confirm the DCT gates stay bit-identical on Windows. Not fixed in the FFT small-N slice (off
its scope; the FFT code itself is gcc-strict-clean). → `docs/sessions/2026-06-15-fft-small-n-engine-crush.md`.


---

**Disposition (2026-08-07 hygiene pass):** FIXED 2026-06-09 (TlsfAllocator init_pool end-sentinel). Covered by the SANITY ledger 2026-06-09 + memory scar; no dedicated session log, so full detail preserved here.

### ✅ `mf-lu-frontparallel-flaky-uaf` — FIXED 2026-06-09 (root cause: `TlsfAllocator::init_pool` end-sentinel mistiling). No remaining debt.

> **FIXED at the allocator root.** The pre-existing flaky crash (~20/30 runs) in the PARALLEL multifrontal-LU
> front-walk (`tests/hesap-direct/test_multifrontal_lu_pp.cpp` → "CONNECTED large grid", `grid3d_weak(22)`,
> nw∈{2,4,8}; always an AV in `TlsfAllocator` coalescing a free block whose links were overwritten) was a bug in
> **`TlsfAllocator::init_pool`**, the engine's most-used allocator.
>
> **Root cause (pinpointed):** `init_pool` reserves `free_block_size = capacity − 3·16` (start sentinel + free
> block's own header + end sentinel = 48 B), but placed the **end sentinel** at `base + 16 + free_block_size`
> (= `capacity − 32`) — accounting for only ONE preceding header, **16 bytes too early**. `block_next(free_block)`
> correctly lands at `base + 32 + free_block_size = capacity − 16`, so it **overshot the (mis-placed) sentinel into
> the chunk's uninitialised tail slack**, whose low bit reads as `kFreeBit`. The moment a block reached the chunk's
> end and was freed, coalescing merged that garbage → a free-list smash. **Benign until a chunk fills to its tail.**
> That is why it only ever surfaced under the **multi-chunk** GrowableTlsf path (the LU's heavy front alloc/free/
> **reuse** churn grows the pool into 256 MB chunks and fills them to the end); a single big chunk never fills to its
> tail, and the memory tests never allocated-to-tail-then-freed — so 820 K+ assertions passed over it for a long
> time. It is **placement, not a race** (hence ts-wrap serialized everything and still crashed; TSan clean).
>
> **The fix (`engine/memory/src/allocators/tlsf_allocator.cpp`):** place the end sentinel at
> `base + 2·kBlockHeaderOverhead + free_block_size` (= `block_next(free_block)` = `capacity − 16`), so the three
> 16-byte headers + payload tile the chunk exactly. One line; compiler-agnostic pointer arithmetic.
>
> **The LU single-chunk `factor_pool` workaround was REMOVED** — `factor_attempt` is back to a plain
> `ThreadSafeAllocator ts(m_alloc)` over the caller's (multi-chunk) allocator. The real v7-e-2 GEMM arena
> (`&gemm_arena` on `factor_front`) is unrelated and kept.
>
> **Verified:** clean shipped binary (no diagnostics, multi-chunk path forced) — grid3d moat **30/30 clean**;
> `crd-memory-tests` 820 014 assertions / 108 cases green with the fix relinked; win-debug `ctest -R "memory|hesap"`
> 48/48; the {1,2,4,8}-worker determinism moat intact. The previously-noted `growable-tlsf-multichunk-freelist`
> "remaining debt" **does not exist** — there was no separate multi-chunk free-list bug; the multi-chunk path was
> simply the only consumer that filled a chunk to its tail and tripped the single `init_pool` defect.
>
> **Lesson for the test suites:** the foundational allocators are validated by volume, not by boundary adversaries.
> A deliberate fill-to-tail / fragment-to-end TLSF stress test would have caught this years earlier — worth adding.


---

**Disposition (2026-08-07 hygiene pass):** RESOLVED 2026-06-05 (v5f-c central mark fix). No 2026-06-04/05 session log exists; lesson lives in memory (jobs parallel_for frame-arena exhaustion); detail preserved here.

### ✅ `gemm-parallel-frame-arena-leak` — RESOLVED 2026-06-05 (v5f-c, central mark fix)

> **RESOLVED.** Added the scoped frame marker to the jobs API (`frame_get_mark()`/`frame_set_mark()` +
> `FrameArena::set_cursor()`) and used it inside `gemm_parallel` + `small_gemm_parallel`: each call saves
> the dispatching thread's frame-arena mark and restores it after every `parallel_for` `wait()`,
> reclaiming only its own JobDecl arrays in place (nest-safe — preserves a caller's frame state below the
> mark, which `frame_reset()` cannot). Verified: a 4000-call leak-regression test (`[frame-arena]`,
> ColMajor main path) that previously exhausted the 1 MB arena at ~1000 calls now completes; jobs
> (29244/90) + hesap-dense (354905/346) green; clang-cl + win-asan + win-tidy clean. The driver-level
> `frame_reset()` workarounds (supernodal_cholesky/lu, multifrontal_lu/qr/ldlt, blr `reclaim_frame_arena`)
> are KEPT — they reclaim the drivers' own *direct* `parallel_for` JobDecls (e.g. `factor_cholesky`,
> cholesky.cpp:156), which the gemm-scoped fix does not cover (advisor-scoped: a `parallel_for`-level
> self-clean is a core-primitive contract change for a separate slice). Original entry below for history.

### `gemm-parallel-frame-arena-leak` — filed 2026-06-04 (v5e-3 Leg B)

> **`crd::hesap::dense::gemm_parallel` (`blas3.cpp`) `frame_alloc`s its per-call JobDecl arrays
> from the per-thread jobs FrameArena but NEVER reclaims them** — the in-code comment
> ("No frame_reset here — it would invalidate frame_alloc state the CALLER may hold") documents
> the deliberate non-reset. Each call leaks ~24 KB; a caller that fires many gemms without an
> external `frame_reset()` monotonically fills the 1 MB arena and **exhausts** it. Surfaced in the
> v5e-3 node-parallel multifrontal Cholesky: a single big front's factor fires hundreds of
> `gemm_parallel_auto` calls ⇒ exhaust mid-front (win-asan: `frame_arena.hpp:60` assert; linux-release:
> assert compiled out ⇒ pointer past the arena ⇒ glibc `corrupted size vs prev_size`).
>
> **Localized workaround SHIPPED in v5e-3 (blr.cpp `reclaim_frame_arena()`):** the driver calls
> `jobs::frame_reset()` after each parallel gemm in the factor hot loops (safe: serial dispatch from
> the main thread, each gemm `wait()`s before returning ⇒ valid frame boundary, no caller holds state).
>
> **Proper central fix (this debt):** add a per-thread **scoped frame marker** to the jobs API
> (`frame_get_mark()`/`frame_set_mark()` — save offset on entry, restore on exit) and use it inside
> `gemm_parallel` so each call reclaims ONLY its own allocations, correct even when nested under a
> caller that holds frame state (which the current all-threads `frame_reset()` cannot be). Removes the
> need for callers to know about the leak. Blast radius = every gemm caller ⇒ needs its own verified
> change (5-config + the parallel benches), not a mid-crush edit. Until then the localized reclamation
> is the safe path for new parallel direct-solver code.


---

**Disposition (2026-08-07 hygiene pass):** RESOLVED 2026-05-20 same day as filing.

### ✅ `v1a-3-assembly-smalln` — RESOLVED 2026-05-20 (same day as filing)

> Sparse COO→CSR/CSC assembly now **beats Eigen `setFromTriplets` at every size** (win-release, i9-class, best-of-3, f64): N=50k **1.03×**, N=200k **1.44×**, N=1M **1.76×** (was 0.74× / 1.16× / 1.19×). The candidate fix landed: `assemble<ByRow>` scatters **directly** into the final `inner_idx`/`values` (no `Entry` AoS), uses in-place parallel-array insertion sort for small inner vectors and a single reused merge-sort scratch for large ones (dense-row robustness preserved), and dedup-compacts in place. The last increment was a new `crd::containers::Array::resize_uninitialized` (trivially-constructible T only; value-inits otherwise) used for the two fully-scattered arrays — eliminating the zero-init pass that dominated the small-N residual. Verified by `bench_hesap_sparse_assembly_vs_reference`.


---

**Disposition (2026-08-07 hygiene pass):** RESOLVED 2026-05-28 (premise falsified by benchmark). No 2026-05-28 session log; detail preserved here.

### `v2e-weighted-compression` — ND fill loses Eigen-AMD on bcsstk25 (multi-DOF) — filed 2026-05-21 — ✅ RESOLVED 2026-05-28 (premise FALSIFIED by benchmark)

> **✅ RESOLVED 2026-05-28 — premise falsified by measurement; no code shipped (v5a-0).**
> Implemented supervariable graph compression (identical-closed-neighbourhood merge)
> with vertex-weight propagation through the ND bisection + CAMD `nv`, exactly as this
> entry prescribed, then benchmarked `bench_hesap_ordering_vs_reference` (Eigen-AMD peer).
> **Result: compression REGRESSED ND fill on all three matrices** (uniform ~5–12% worse):
> | matrix | compress engaged | un-compressed ND | compressed ND |
> |---|---|---|---|
> | bcsstk13 | 20.5% reduced, maxw=6 | **254079 (0.984× WIN)** | 273162 (1.058× lose) |
> | bcsstk24 | 75.0% reduced, maxw=6 | 285920 (1.001× tie) | 320113 (1.121× lose) |
> | bcsstk25 | 14.6% reduced, maxw=3 | 1670446 (1.157× lose) | 1714528 (1.187× lose) |
> The un-compressed numbers reproduce this entry's original baseline exactly.
> **Structural reason:** CAMD already detects supervariables DURING elimination (the
> `nv`-weighted approximate-degree update + the in-loop indistinguishable merge), at the
> granularity that helps. Pre-compression delivers that information earlier but COARSER —
> it strictly reduces the bisector/CAMD's choices, so it cannot beat un-compressed on a
> graph where un-compressed already wins (bcsstk13). The "DOFs split across separator
> classes" hypothesis is wrong: the un-compressed ND already handles them. **Reverted in
> full** (compression kernel + weighted plumbing); the kernel + this measurement are the
> durable artifact. **Status: closed.** ND legitimately loses to AMD at bcsstk13/24/25
> sizes (the win-small-lose-large inversion this entry flagged); **our AMD already beats
> Eigen-AMD** (1.039 / 0.960 / 1.007 — GATE-OK), so the v5 sparse-direct consumer picks
> AMD on these matrices. Large-3D-elliptic where ND wins asymptotically is HSS-front
> territory (v5e), not a fill-ordering question. Vindicates the benchmarks-at-slice-close
> mandate: unit tests (correctness) passed; the benchmark caught the net regression.
>
> --- original entry (premise, now falsified) ---
>
> **Tracked optimization follow-on, NOT a defect.** v2e nested dissection + CAMD
> **beats Eigen-AMD fill on bcsstk13 (0.983×) and bcsstk24 (0.999×)** but loses on
> **bcsstk25** (1.158× vs Eigen-AMD; n=15439, a tall 3D skyscraper stiffness matrix
> with multiple DOFs per node) — the *opposite* of the textbook ND-asymptotic
> pattern (win small, lose large). Fill is a **downstream-perf knob (factor memory +
> flops), never correctness** — `nd_order` always yields a valid permutation, so
> every solve is identical; and the v5 consumer can pick the better of AMD/ND
> per-matrix anyway (both are available).
>
> **Root cause:** bcsstk* are structural matrices with groups of identical-pattern
> rows (the multiple DOFs of one mesh node). AMD merges these via supervariables;
> our CAMD gates supervariable merge by `cmember`, so when a separator splits a
> node's DOFs across classes they can't merge → fill penalty, worst on the
> largest/densest case (bcsstk25). Verified via the path test: 1D is ND's worst
> case (AMD provably optimal) and CAMD-uniform reproduces AMD exactly, so the port
> is correct — the gap is purely separator/multi-DOF quality.
>
> **The fix (CHOLMOD/METIS technique):** supervariable **graph compression** —
> merge indistinguishable (identical-closed-neighbourhood) vertices into one
> super-vertex *before* ND, run bisection + cmember + CAMD on the compressed graph,
> expand after. A first cut was implemented; **unweighted** compression *regressed*
> all three matrices (it imbalances the bisection — supers counted as weight-1) and
> was reverted. The real fix needs **vertex-weight propagation**: thread member
> counts as `vwgt` through `assign_cmember` → `bipartition_refined` → `to_weighted`
> and into CAMD's initial `nv`, so the compressed bisection balances by original
> count and CAMD's degree accounting is correct. ~150 LOC, uncertain but likely
> flips bcsstk25. **Real trigger:** a v5 sparse-direct benchmark showing ND-fill
> (not the numeric kernels) is the bottleneck on a multi-DOF FEM workload.


---

**Disposition (2026-08-07 hygiene pass):** SHIPPED as Phase 3.0 v1o1+v1o2+v1o3 (2026-05-09, ADR-0061 realised per the phase doc) — then the crd-renderer home was RETIRED (ADR-0105). Async/batched upload today is a gpu-context concern (batch upload contract). Entry preserved for the ADR-0061 design narrative pointers.

### Async GPU upload (`GpuUploader`) — design closed by ADR-0061; impl lands in v1o1+v1o2

**Status (2026-05-09):** **Design half closed.** ADR-0061 locks the contract: three layers, owned by three modules.
- `crd-rhi`: adds `Fence` + non-waiting `Queue::submit(cmd, fence)`.
- `crd-renderer`: adds `UploadHandle` + `GpuUploader::upload_mesh_async` / `upload_texture_async` + `PendingMeshUpload` component + `RenderUploadSystem` (RenderExtract phase).
- `crd-scene`: unchanged — already exposes `AsyncAwareIndex` + `query<...>().skip_pending<Renderable>()`.

**Implementation half:** lands as Phase 3.0 v1o1 (RHI fence) + v1o2 (UploadHandle plumbing + RenderUploadSystem). v1o3 is the sandbox integration that uses the async path — the first real consumer.

**Why it matters:** `GpuUploader::upload_mesh` / `upload_texture` today end with `device.graphics_queue().submit_and_wait(*cmd)` — a `vkQueueWaitIdle` on the main thread. For BoomBox-class assets (~10 MB GLB → ~30 MB raw mesh) that's a visible hitch even though the CPU-side load is already async (Phase 2.8 v1g). The sync entry points stay (some smokes/tests need immediate readiness); the async siblings join them.

**Reserved follow-ups (NOT blocking v1o):**
- `Device::transfer_queue()` — opportunistic dedicated transfer queue (Vulkan: separate `VK_QUEUE_TRANSFER_BIT` family); falls back to graphics when absent. Reserved for Phase 3.5+ when streaming pressure makes it worthwhile.
- Timeline semaphores — replace binary fences when a consumer needs multi-step ordering or batched waits.
- Streaming budget — at most N concurrent uploads; queue the rest. Phase 3.5+ when terrain/LOD streaming arrives.
- Async texture upload consumer — `PendingTextureUpload` sibling component. Lands when a real texture-streaming workload surfaces (likely Phase 3.5 IBL or 3.8 GPU-driven rendering).

**Where it's referenced:**
- `docs/decisions/0061-async-gpu-upload-contract.md` — full design + module ownership + caller pattern.
- `docs/phases/phase-3.0-scene-ecs.md::v1o` — implementation slicing.
- `engine/renderer/src/gpu_uploader.cpp` — current synchronous implementation; v1o1+v1o2 add the async siblings.
- `sandbox/src/sandbox_layer.cpp::try_finalize_pending_load()` — current consumer; v1o3 migrates to async.

---


---

**Disposition (2026-08-07 hygiene pass):** SUPERSEDED: the entire MATR/MaterialTemplate/ForwardRenderPath system this section tracks was retired (ADR-0104 CKIR materials, ADR-0105 renderer retirement). Items 4–5 (descriptor layouts, more shader stages) are moot — their modern equivalents shipped as CKIR + bindless (REN-38) + the full stage model (ADR-0103).

## Material system v1 known gaps

`MaterialResource` as shipped in Phase 2.6 v1e is a loader proof-of-concept, not a production material
abstraction. Phase 2.7 v1c (ADR-0048) redesigns it as a full material system foundation: `MaterialTemplate`
+ `MaterialInstance` two-tier split, new MATR artifact format (INFO/PRMS/DFLT/PASS/PSOS/OPTS chunks),
`ParameterType` enum, `ShaderOption` system with inline functor, `SurfaceData` GLSL contract, `PassType` enum,
`MaterialDomain` (pulled forward from Phase 2.8), `RasterState` encoding in the artifact.

**Updated status (post-ADR-0048):**
- **Items 1–3 (artifact layer)** — Closed by Phase 2.7 v1c. The artifact format now carries: parameter
  schema (PRMS), defaults (DFLT), pass-keyed shaders (PASS), PSO state per pass (PSOS), shader options (OPTS).
- **Items 1–3 (GPU wiring)** — Phase 2.8 wires the artifact data to Vulkan pipeline compilation
  (per-material pipeline cache, multi-pass ForwardRenderPath, depth-only prepass).
- **Items 4–5** — Still deferred (item 4 → Phase 3.5 CSM; item 5 → Phase 3.7 post-FX or Phase 3.8 GPU-driven).
  No consumer exists yet.

### 1. Material parameters, texture slots, and full parameter system ✅ Closes Phase 2.7 v1c

**What was missing:** `MaterialResource` held two shader handles and nothing else. No parameter schema,
no texture slots, no shader variants, no material domain, no render-pass awareness.

**What v1c delivers:**
- `MaterialTemplate` (replaces `MaterialResource`): loaded from MATR artifact. Carries parameter schema
  (`Array<CookedParameter>` sorted by name_hash), default values blob, pass-keyed shader handles
  (`HashMap<PassType, ResourceHandle<ShaderResource>>`), PSO state per pass, shader option declarations.
- `MaterialInstance` (caller-owned, not in ResourceManager): mutable overrides atop a `MaterialTemplate`.
  `set_float` / `set_vec4` / `set_texture` write into a `values_blob`. `variant_for_pass(pass)` evaluates
  inline functor rules and returns the correct `ShaderResource` permutation.
- `ParameterType` enum: Float/Float2/Float3/Float4/Color/Bool/Int/Enum/Texture2D/TextureCube/Sampler.
- Cook-time SPIR-V reflection: spirv-reflect extracts UBO offsets; cooker emits `CookedParameter` entries
  sorted by name_hash for O(log N) binary search at bind time.
- Inline functor: `enables_option = "USE_NORMAL_MAP"` on a texture parameter — no C++ subclass needed.

### 2. PSO state in the material artifact — ✅ Artifact layer closes Phase 2.7 v1c; GPU wiring Phase 2.8 v1a

**Artifact layer (v1c):** `PSOS` chunk carries a `RasterState` per PassType (present_mask + RasterState
array). `RasterState`: AlphaMode, CullMode, FillMode, depth_test, depth_write, src/dst BlendMode.

**GPU wiring (Phase 2.8 v1a):** `ForwardRenderPath` reads `material->pso_states[pass_type]` and
incorporates it into the `GraphicsPipelineDesc` key. Per-material pipeline cache keyed by
`(VariantKey, RasterState)`. `ForwardRenderPath` skips non-`Surface` domain materials.

### 3. Shader variant awareness (VariantKey + pass-keyed variants) — ✅ Artifact layer closes Phase 2.7 v1c; GPU wiring Phase 2.8 v1b

**Artifact layer (v1c):** `PASS` chunk stores `HashMap<PassType, ResourceId>`. `OPTS` chunk stores shader
option declarations. `MaterialInstance::variant_for_pass(pass)` evaluates inline functor rules, constructs
a `VariantKey`, and returns the appropriate `ShaderResource` from `tmpl->pass_shaders[pass]`.

**GPU wiring (Phase 2.8 v1b):** `ForwardRenderPath` calls `mat_inst.variant_for_pass(DepthPrepass)` in
the depth prepass and `mat_inst.variant_for_pass(Forward)` in the color pass. Each pass uses the shader
selected by the instance, not a hardcoded vert+frag pair.

### 4. Descriptor layout — per-material bindings — Deferred Phase 3.5

**What's missing:** Nothing in `MaterialTemplate` drives descriptor set creation or layout for set 1+
(per-material bindings). The `VulkanDescriptorAllocator` and `MaterialBindGroup` (formerly `MaterialInstance`)
are wired to hardcoded layouts, not artifact-driven layouts.

**What to add (Phase 3.5):**
- `MaterialTemplate` carries enough reflected binding data to construct a `VkDescriptorSetLayout` at load
  time (or defer to the first bind).
- `MaterialResourceLoader` merges spirv-reflect results across pass shaders to build the per-material
  binding table.
- `MaterialBindGroup` is rebuilt from `MaterialTemplate` rather than from a manually-constructed layout.

**Why deferred:** No concrete consumer (texture arrays, multiple samplers) until CSM and area-light
materials land in Phase 3.5, and post-FX materials in Phase 3.7.

### 5. Additional shader stages — Deferred Phase 3.5+

**What's missing:** The PASS chunk stores vertex+fragment shader pairs (one `ShaderResource` per PassType).
There is no slot for compute, mesh, or task shaders. A compute-only material (post-FX, particle simulation)
cannot be expressed.

**What to add (Phase 3.5):**
- Extend `ShaderResource` to carry multiple stages (vertex/fragment/compute/mesh as a tagged union).
- Update `MaterialTemplate::pass_shaders` value type to `ResourceHandle<ShaderResource>` where each
  `ShaderResource` declares its own stage set (already possible via the existing shader mechanism).
- The PASS chunk format is already stage-agnostic (one ResourceId per PassType entry). Only the shader
  artifact format changes — the material artifact format is unaffected.

**Why deferred:** Compute and mesh shaders are Phase 5 concerns. The PASS chunk format already accommodates
them — the `ShaderResource` inside can carry any combination of stages.

---

**Updated execution plan:**
- Phase 2.7 v1c closes the artifact layer of items 1–3 (full material foundation: ADR-0048).
- Phase 2.8 wires items 2–3 to actual Vulkan pipeline compilation and multi-pass rendering.
- Items 4 and 5 remain open; deferred until consumers create real demand (item 4 → Phase 3.5 CSM /
  area lights; item 5 → Phase 3.7 post-FX compute / Phase 3.8 GPU-driven culling).

See `docs/phases/phase-2.7-asset-import.md`, `docs/phases/phase-2.8-material-completion.md`,
ADR-0044, ADR-0046, ADR-0048.


---

**Disposition (2026-08-07 hygiene pass):** SUPERSEDED: describes adding instanced draws to the retired rhi CommandBuffer / ForwardRenderPath. Instanced + multi-draw + indirect draws shipped on gpu-context (REN-38/39); geometry completeness is tracked as post-RAF GVA-0.

## GPU instancing (planned Phase 3.2)

v1h ships `draw_indexed(index_count, first_index, vertex_offset)` — non-instanced only
(`instance_count` hardwired to 1 in the Vulkan call). When instancing lands:

**RHI changes:**
- Add `draw_instanced(vertex_count, instance_count, first_vertex, first_instance)` to
  `CommandBuffer` (non-indexed instanced path).
- Add `draw_indexed_instanced(index_count, instance_count, first_index, vertex_offset, first_instance)`
  to `CommandBuffer` (indexed instanced path, mirrors `vkCmdDrawIndexed` fully).
- All four draw variants (`draw`, `draw_indexed`, `draw_instanced`, `draw_indexed_instanced`)
  coexist; `VulkanCommandBuffer` implements all four.

**Renderer changes:**
- `Renderable` and `DrawItem` gain `instance_count = 1` (default keeps backward compat).
- `ForwardRenderPath` dispatch logic: `instance_count == 1` → non-instanced path (no
  regression); `instance_count > 1` → instanced path.
- GPU instance data buffer (transforms, material indices) is a Phase 3 GPU scene buffer
  concern — `crd-resources` provides per-frame upload; `ForwardRenderPath` binds it as
  a storage buffer at set 0 binding 1 or via push constants for the base instance.

**When:** After Phase 3.1 (stable entity/transform storage in the scene system) ships and
a GPU instance data layout is frozen. Instancing without a stable instance buffer contract
produces nothing useful. Target: Phase 3.2.

**Do NOT prematurely add `instance_count` to `Renderable` / `DrawItem` before that point.**


---

**Disposition (2026-08-07 hygiene pass):** SUPERSEDED: every item targets the retired crd-renderer. Modern equivalents landed (transient aliasing REN-1 2026-07-24; HDR chains, depth prepass 40-G, bindless 38-G, GPU-driven cull 40-A, indexed-pull streams REN-39) or are tracked post-RAF (RAH/RPL/GVA/TPR).

## Renderer optimization backlog (post-v1g)

Intentionally deferred. These require the render path to be working end-to-end
before they pay off. Implement in order of demonstrated need, not in anticipation.

- ~~**Transient image aliasing in the frame graph** — `FrameGraph::execute` currently
  creates transient images fresh each frame and destroys them on `reset()`. A proper
  aliasing pass would reuse GPU heap pages across mutually-exclusive transients,
  reducing VRAM by the sum of the largest non-overlapping resource sets. Prerequisite:
  lifetime analysis pass in `FrameGraph::build()`.~~ **RESOLVED 2026-07-24 by REN-1**
  (D-007 row 98): the new frame graph (on gpu-context, replacing the retired rhi
  `FrameGraph`) runs the lifetime-analysis pass in `build()` and does GREEDY interval-
  coloring aliasing on BOTH backends — disjoint-lifetime transients share one backing
  allocation; `transient_memory_bytes() < transient_logical_bytes()` proves it, gated in
  `test_vulkan_frame_graph.cpp` (`VK_IMAGE_CREATE_ALIAS_BIT`) and `test_dx12_frame_graph.cpp`
  (one `ID3D12Heap` per slot + `CreatePlacedResource`). Fully closed 2026-07-24 (no deferral).

- **HDR render target** — `ForwardRenderPath` uses `B8G8R8A8Unorm` (LDR). Switch to
  `R16G16B16A16Sfloat` (scene linear HDR) and add a tone-map pass before the swapchain
  blit. Required before bloom, exposure, or any physically-based lighting integral.

- **Depth-only pipeline for the depth prepass** — `ForwardRenderPath` v1g reuses the
  full vertex+fragment pipeline in the depth prepass. A vertex-only pipeline (null
  fragment shader, `Format::Undefined` color, `Format::D32Sfloat` depth only) removes
  unnecessary fragment work during the prepass. Requires the per-variant pipeline cache
  to store `{depth_pipeline, color_pipeline}` pairs.

- **Async pipeline compilation** — `PipelineResolver::resolve_pipeline()` is currently
  synchronous. Slow variant compiles stall the main thread. Solution: compile on a job
  thread, return a "pending" sentinel, and render with a fallback pipeline until the
  real one is ready. Integrates with `crd-jobs` (Phase 2.5).

- **Bindless material system** — Current: one descriptor set per material instance per
  frame (set 1), allocated from the ring pool. Future: global bindless descriptor heap
  (one giant `DescriptorSet` with an array of all textures + material CBs), indexed
  via a per-draw material index in the push constants. Eliminates per-draw
  `vkCmdBindDescriptorSets` calls. Requires Vulkan device features: `descriptorIndexing`.

- **GPU-driven rendering** — CPU culling + indirect draw. Replace per-object draw calls
  with a compute dispatch that reads a scene buffer, outputs `VkDrawIndirectCommand`
  structs, and optionally writes a visible-object list. Requires: stable GPU scene buffer
  (Phase 3 scene system), `VkDrawIndirectCount` (Vulkan 1.2 core), and a GPU frustum
  cull shader. Significant throughput gain for dense scenes (> ~10k draws).

- **Split vertex streams** — Separate position-only VBO from full-attribute VBO. The
  depth prepass only needs positions; pulling the full vertex (UVs, normals, tangents)
  wastes memory bandwidth. Requires `DrawItem` to carry both VBOs and shader variants to
  declare which stream they consume.


---

**Disposition (2026-08-07 hygiene pass):** Pre-2026-05-04 cleared items; per debt.md's own rule they belong in a session log once cleared.

## Cleared debt

- **Disabled-trace benchmark** (2026-05-03) — Replaced compile-time-eliminated
  `CRD_LOG_TRACE` call with `CRD_LOG_INFO` gated by `runtime_level = Error`. The
  benchmark now measures the runtime short-circuit cost in all build configurations.
- **Doxygen per-symbol comments in crd-core** (2026-05-03) — Added `///` docs to all
  symbols in `types.hpp` (14 aliases), `platform.hpp` (18 macros + 3 functions), and
  `assert.hpp` (2 type aliases + 4 functions + 4 macros).
- **No SPSC RingBuffer** (2026-05-03) — Added `SpscQueue<T>` in
  `engine/containers/include/crd/containers/spsc_queue.hpp`. Lock-free, cache-line
  padded head/tail atomics, wait-free push and pop. Tested single-threaded and with
  concurrent 1M-item producer/consumer.
- **No file watcher in crd-platform** (2026-05-03) — Added polling-based `FileWatcher`
  in `engine/platform/`. Uses `fs::last_modified_unix_seconds()` on each polled path.
  Handles add/remove by handle, fires callbacks synchronously in `poll()`.
- **Multi-viewport ImGui deferred** (2026-05-03) — Moved to "Long-term deferred" above.
  Will not land until `crd-ui` ships; ImGui stays debug-only forever.
