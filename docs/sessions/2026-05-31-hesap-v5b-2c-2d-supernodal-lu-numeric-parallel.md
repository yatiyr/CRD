# 2026-05-31 — hesap v5b-2c/2d: supernodal-LU numeric + symmetric pruning + tree-parallel moat

A long crush-driven session on the sparse-LU numeric (continues v5b-1 oracle ✅, v5b-2a front-end ✅,
v5b-2b symbolic ✅). Built the full numeric, then pushed measured crush levers: SPA rewrite → symmetric
pruning → tree-parallel + the determinism moat. Honest standing: **correct, deterministic, matched-residual
across a real unsymmetric corpus; closing on Eigen SparseLU (gemat11 0.63×) but not yet the full crush —
the remaining gap is the small-supernode cmod kernel + within-front parallelism (deep narrow etrees limit
level-parallelism), a continued multi-session arc.**

## What shipped (all correct + gated)

**v5b-2c — `SupernodalLU<T>` numeric (`supernodal_lu.hpp/.cpp`).** factor: A (CSR) → MC64 static-pivot
front-end (v5b-2a) → exact symbolic (v5b-2b) → **supernodal BLAS-3 left-looking numeric** (static diagonal
pivot, no interchange, GESP √ε·‖B‖ perturbation for tiny pivots) → solve with **iterative refinement** to a
matched true residual. f32/f64/c32/c64. Residual-validated end-to-end (matches Eigen at machine precision)
on unsymmetric-DD / MC64-rescued tiny-diagonal / badly-scaled / complex / multi-RHS.

**SPA (sparse accumulator) kernel.** A dense global-row-indexed work (`spa[col·n+row]`): a contributor K's
U-segment is rows [k0,k1) (contiguous) and the foot subtract is a DIRECT global-row index (no rowmap
indirection). Padded contributor-foot rows subtract 0 ⇒ stay clean; cleared O(fill) per supernode.

**Relaxed amalgamation (`lu_symbolic`).** EXACT structural-nesting supernodes (the colcount proxy
over-merges for unsymmetric L — a bug found + fixed) → relaxed BLAS-3 panels (union row pattern, padded; L
rebuilt, U exact; the panel is a dense trapezoid for the cmod GEMM). nrelax=8 (16 measured neutral-worse).

**Symmetric pruning (Eisenstat-Liu / KLU) — the session's biggest win.** Deep-researched the KLU `prune` +
DFS-with-`Lpend` and implemented it: the reachability now scans only the pivotal head of each L column
(symmetric-pair pruned), turning the symbolic from **O(flops) → O(fill)**. Output is bit-identical (the
prune only reorders the build; a final per-column sort restores canonical order ⇒ gated by the superset +
residual tests). Measured: af23560 22.2s→10.0s, wang3 5.0s→2.8s, gemat11 0.42×→0.63×.

**v5b-2d — tree-parallel factor + the cross-thread DETERMINISM MOAT.** Supernode-etree level scheduling over
`crd::jobs::parallel_for` (same-level supernodes independent), per-worker SPA/scratch sized by
`jobs::num_workers()`, fixed ascending contributor order ⇒ the factor is a deterministic pure function. **L
and U are BIT-IDENTICAL across {1,2,4,8} workers — VERIFIED** (the moat no Eigen/UMFPACK/SuperLU peer
carries; new `[v5b-2d]` test).

**Bench — `bench_hesap_lu_supernodal_vs_reference`** (real unsymmetric SuiteSparse: west2021/add32/gemat11/
sherman3/memplus/af23560/wang3/bbmat — downloaded; CMake list extended; UMFPACK folds in under
`CRD_BUILD_HESAP_VS_UMFPACK`, WSL). Cerid SupernodalLU vs Eigen SparseLU on the SAME AMD-permuted matrix,
matched true residual via IR.

## Honest standing (relwithdebinfo, vs Eigen SparseLU, factor time)

gemat11 **0.63×** · sherman3 0.41× · memplus 0.44× · af23560 0.26× · wang3 0.16× · west2021 0.17×. Residuals
match Eigen everywhere. **Not yet the crush.** Measured root cause: after pruning fixed the symbolic, the
**cmod numeric is the wall** — small supernodes on irregular unsymmetric matrices ⇒ tiny kernels at low
flop-rate (Eigen's tuned supernodal cmod is ~7× faster/core). Tree-parallel did NOT yet help (af23560 10.5s
@8w vs 10.0s serial): these matrices have **deep/narrow column etrees ⇒ limited level-parallelism**, and the
per-worker full-n SPA is memory-heavy. Bench headline is serial (the faster); parallel is opt-in via
`CRD_BENCH_LU_WORKERS`; the moat is verified separately.

## Remaining crush levers (the continued arc — precisely located)

1. **Within-front parallelism** for the few large near-root supernodes (the v5a-7 row-slab playbook) — the
   deep/narrow etree means level-parallelism alone won't scale; the big fronts need internal parallelism.
2. **Tuned cmod kernel** for small supernodes (Eigen-class panel processing) — the now-dominant serial cost.
3. **Compacted/column-blocked per-worker scratch** — the full-n SPA × workers is the parallel memory wall.
4. **MC64-then-AMD ordering** — fixes west2021's 2× fill (won't close the gap alone).
5. UMFPACK comparison (WSL) + the architecture question (supernodal vs multifrontal for unsymmetric).

## Verification

win-debug full suites green (direct 591526 asserts / 47 cases incl. the v5b-2d moat; ordering 7771/44).
Serial + parallel both correct (residuals match Eigen; bit-identical {1,2,4,8}). **Owed before commit:**
clang-cl + gcc -Werror + win-tidy + clang-format on the v5b-2c/2d engine changes (the touched modules).

---

## v5b-2c CRUSH session (2026-05-31, continued) — diagnosis + two-kernel decision

Followed the user's "deep-research → reference-code → attack, don't cycle/retreat" rule. Three measured
cycles (win-vs-ref Release, serial nw=1, vs Eigen SparseLU on the same AMD-permuted matrix, matched true
residual; profiler behind `#define CRD_LU_PROFILE`):

1. **Profiled the cmod sub-breakdown** (advisor-corrected: free flop/path counters + coarse per-supernode
   timers, not per-op chrono). Found cmod = 78–97% of factor; inline path dominates circuit (gemat11 92%
   inline), GEMM path carries 98% of flops on PDE/CFD (af23560). The inline path was a no-op `trsm` +
   `ub` memset + readback wrapping a scalar rank-1 update (KLU's "narrow ⇒ scalar" insight).
2. **Landed (correct, moat-safe, kept):** compact-useg gather for the foot (af23560 GEMM **4→15.6**,
   wang3 **4→28** GFLOP/s) · `knc==1` fused rank-1 (no trsm/ub/memset) · write-on-first (skips ub memset)
   · gating (compact gather/writeback only in the GEMM branch — fixed a memplus regression).
3. **Refuted** the per-call-GEMM-alloc hypothesis (per-worker `LinearAllocator` → 15.4, unchanged) ⇒ the
   ColMajor moderate-shape GEMM kernel itself is the limit (15.6 vs Eigen's ~40 effective).

**The reframe (advisor, sub-op-vs-peer-TOTAL):** af23560 non-GEMM cmod (3347ms) + diag (1180ms) = 4527ms
**already exceeds Eigen's entire 2644ms factor** even with GEMM at zero ⇒ the GEMM is NOT the floor; the
**per-contributor update/scatter structure** is. **Amalgamation EXHAUSTED:** relax 8→16 cut nsup 25–29%
but made wang3 + all 5 circuit matrices slower (zero-flop fill); relax=8 optimal. The floor is the SPA
itself (`n×max_nc` = 54 MB/worker for af23560, cache-hostile).

**Ratios (post-fix, serial):** west2021 0.19 · add32 0.57 · gemat11 0.69 · sherman3 0.37 · memplus 0.36 ·
af23560 0.28 · wang3 0.15. All `[ok]`; 6/6 SupernodalLU tests incl. the v5b-2d moat green.

**USER DECISION — "build both kernels (complete family)":** the corpus splits by architecture fit, so
the crush is a TWO-KERNEL job. **Kernel A (NEXT):** supernodal structured maturity — SuperLU
relative-indexed COMPACT panel (`nr×nc`, cache-resident; also fixes the bbmat OOM) + tuned ColMajor GEMM
(15→40) + blocked diag, targeting af23560/wang3. **Kernel B (after A):** KLU-class non-supernodal scalar
GP for the circuit matrices (beats both Eigen and UMFPACK in their weak regime). Detail:
`project_lu_crush_two_kernel_plan` (memory). bbmat removed from the bench corpus (exceeds the full-n SPA
budget until the relative-indexed panel lands). NOT a retreat — right-tool-per-regime + complete family.

## v5b-2c CRUSH cont. — Kernel A compact panel (2026-05-31)

User chose "build both kernels (complete family)". Started Kernel A (supernodal structured maturity).

**GEMM probe (decisive):** standalone `dl::gemm` on the exact LU foot shape 300×100×290 ColMajor f64 hits
**51 GFLOP/s** (RowMajor-transpose 58), NOT the 15.6 measured in-factor ⇒ the in-factor rate was **memory
pollution** (cold operands + 54 MB n-strided SPA evicting the cache), not a kernel limit. So the GEMM cost
and the scatter cost are coupled through the SPA — the compact panel attacks both.

**Compact panel (SuperLU relative-indexed):** replaced the `n × max_nc` global-row SPA with `W[pnr × nc]`
(U-part = padded contributor ranges ⇒ contiguous U-segment per contributor; L-part = `m_li`;
`relmap[global → compact]`, sentinel `kNoRow`). Foot GEMM routed via the RowMajor-transpose identity.
Bug found + fixed: relaxed amalgamation pads K's foot with explicit-zero rows not in J's pattern
(`rel = kNoRow`) → the foot scatter must skip them (contribution 0, bit-identical).

**Result (correct, all `[ok]`, moat 6/6 green, full suite 591526 asserts):**
- **af23560 GEMM 15.6 → 31.7 GFLOP/s; total 9544 → 7263 ms (0.28 → 0.37×).**
- wang3 GEMM 28 → 46 GFLOP/s; 2902 → 2747 ms (0.15 → 0.17×).
- gemat11 REGRESSED 0.69 → 0.62× — the `rel_w` indirection is pure overhead for small-n nc=1 circuit
  matrices (SPA already cache-fit); INHERENT to the compact structure ⇒ Kernel B (scalar GP) routes
  circuit away from it. Not a silent regression; the two-kernel dispatcher is the resolution.

**af23560 remaining (7263 ms vs Eigen 2675):** GEMM 2068 + non-GEMM cmod 3195 (big-panel foot scatter,
`pnr ~ n` near root) + diag 1163 (`dense_lu_nopivot` is UNBLOCKED rank-1). NEXT Kernel-A levers:
near-root column-blocking (shrinks the scatter working set + lifts GEMM 31.7 → ~51) + blocked diagonal LU.
Profiler dormant (`#define CRD_LU_PROFILE 0`, NOLINT). Detail: `project_lu_crush_two_kernel_plan` memory.

### Kernel A levers 2 & 3 (2026-05-31, continued)

**fpos hoist:** the compact-panel foot scatter `col[rel_w[m_li[kfrb+fr]]]` is a cold double-indirection
repeated per nc column. Hoisted `fpos[fr] = rel_w[m_li[kfrb+fr]]` once per contributor (gated nc≥2 so the
nc=1 circuit path keeps the lean inline lookup). This RECOVERED and BEAT the compact-panel gemat11
regression: **gemat11 0.62 → 0.80×** (its scatter volume is in nc≥2 tall-foot panels) + af23560 0.37→0.40
+ wang3 0.17→0.19.

**Blocked diagonal LU:** `dense_lu_nopivot` was unblocked rank-1 (~1.7 GFLOP/s on the wide nc≈290 diagonal
blocks). Rewrote it right-looking blocked (nb=48: factor panel → unit-lower TRSV block-row → BLAS-3
`dl::gemm` Schur update, per-worker LinearAllocator scratch). nc≤48 degenerates to the old unblocked form
(bit-identical). **af23560 diag 1197 → 223 ms** (5.4×); total 6620→5548. wang3 2342→1917.

**Cumulative this session (all `[ok]`, moat 6/6 green, full suite 591526 asserts, NO regressions):**
af23560 **0.25 → 0.47×** · wang3 **0.15 → 0.23×** · gemat11 **0.65 → 0.80×** · sherman3 0.37 → 0.43×.
af23560 remaining: cmod 4481 (GEMM ~2180 @ 30 GFLOP/s + non-GEMM ~2300) + diag 223. **NEXT: near-root
column-blocking** (wide supernodes in column chunks → GEMM 30→~45 + smaller scatter working set), then
Kernel B (KLU scalar) + dispatcher. Also noted: af23560 SOLVE is 5× slow vs Eigen (separate lever).

### Kernel A lever 4 — adaptive active-panel width cap (2026-05-31) — the breakthrough

User challenged "did you check the peer source?". Read Eigen's `SparseLU.h` factorize() loop: it loops
over PANELS of `m_perfv.panel_size = 16` columns (dense work `m × 16`, BOUNDED), DECOUPLED from
`maxsuper = 128` (supernode storage width). My architecture made the supernode the active panel
(`W[pnr × nc]`, nc up to 290) → cache-cold for wide CFD/FEM nests.

Fix: cap the supernode (= active panel) width in BOTH `detect_supernodes` (splitting EXACT nests is
free — no fill; af23560's wide nests are exact, structurally-symmetric CFD) AND `relax_amalgamate_l`.
ADAPTIVE on the panel's L-column height (BLAS-3 amortization of the foot GEMM scales with foot rows):
`lu_panel_wcap(h) = h ≥ 64 ? 64 : 32`. Swept 16/32/48/64/96 + adaptive: w32 favors medium matrices,
w64 favors big structured; adaptive-64 raises the FLOOR best (sherman3 0.52→0.59).

**Result: af23560 0.47 → 0.78× · wang3 0.23 → 0.60× · sherman3 0.43 → 0.59× · memplus 0.44 → 0.55×.**
Moat + full suite (591526 asserts) green.

### CUMULATIVE this session (4 levers, all correct, moat green, NO regressions)

| matrix | start | end | levers |
|---|---|---|---|
| af23560 | 0.25× | **0.78×** | compact panel + fpos-hoist + blocked-diag + adaptive cap |
| wang3 | 0.15× | **0.60×** | (same) |
| gemat11 | 0.65× | **0.80×** | |
| sherman3 | 0.37× | **0.59×** | |
| memplus | 0.36× | **0.55×** | |
| add32 | 0.57× | **0.65×** | |

Still < 1.0 (not absolute crush yet). NEXT to ≥1.0: residual near-root work-array bounding (2D/row-block),
the SOLVE (af23560 5× slow vs Eigen — separate `lu_lu_solve` lever), then Kernel B (KLU scalar) + dispatcher.

### Honesty check + FILL breakthrough (2026-05-31) — lever 5

User: "be completely honest... we need a massive breakthrough." Advisor caught me about to claim
parallel-Cerid vs serial-Eigen as a crush = the apples-to-oranges category error our own rules forbid
(Eigen is serial; + v5a data says within-node parallel plateaus ~2× bandwidth-bound, not 8×). Measured
parallel scaling: ~0× (af23560 serial 3502 = 8-worker 3502 — tree-level scheduling doesn't scale).

The HONEST per-core lever is FILL. Same AMD ordering both sides; the gap correlates with fill ratio.
Measured at nrelax=0 (zero-padding structural fill): my structural Gilbert-Peierls fill EQUALS Eigen's
(add32/sherman3/memplus 1.00×, gemat11 0.98×, af23560 1.03×) — ALL the medium/circuit excess fill was
explicit-zero BLAS-3 PADDING (memplus was 1.49×, add32 1.81×) = wasted flops Eigen doesn't do.

Fix: HEIGHT-GATED padding in `relax_amalgamate_l` — `eff_relax = (panel_height ≥ kLuPanelTallRows) ?
nrelax : 0`, so SHORT circuit panels pad 0 (Eigen's flop count, scalar) while TALL structured panels keep
BLAS-3. Result: **add32 0.65→0.89× · memplus 0.55→0.68× · gemat11 0.80→0.84× · af23560 kept 0.80×; fill
now ≈ Eigen on all but west2021.** Moat + full suite (591526) green.

**HONEST per-core standing:** best matrices 0.84–0.89× = ~15% behind mature Eigen PER FLOP at equal fill
(kernel maturity). Crossing >1.0 per-core needs (a) beating Eigen's per-flop (hard vs decade-tuned code),
or (b) ordering wins (west2021 1.96× / wang3 1.14× structural = MC64 destroys AMD → post-MC64 reorder).
Deterministic parallelism is real but a same-class crush only vs PARALLEL peers (SuperLU_MT/PARDISO).

### CUMULATIVE 2026-05-31 (5 levers, all correct, moat green, fill≈Eigen, no regressions)
af23560 0.25→0.80 · wang3 0.15→0.60 · gemat11 0.65→0.84 · sherman3 0.37→0.57 · memplus 0.36→0.68 ·
add32 0.57→0.89 · west2021 0.19→0.22 (ordering-bound). Levers: compact panel · fpos-hoist · blocked-diag ·
adaptive width cap · height-gated padding. NEXT honest levers: post-MC64 reorder (west2021/wang3 fill) ·
per-flop kernel (the ~15% at equal fill) · parallelism vs PARALLEL peers · Kernel B (circuit dispatcher).

### Lever 6 — post-MC64 AMD reorder + fair-peer (Eigen COLAMD) = GENUINE CRUSH (2026-05-31)

Built the post-MC64 fill-reducing reorder: AMD on B+Bᵀ → B' = P·B·Pᵀ (preserves the matched diagonal,
cuts the fill MC64's matching permutation destroyed). `symmetric_permute_csc` (values) + `amd_order`;
P folded into the solve transform (perm/inv_perm in StaticLuScaling; transform_rhs/untransform).

Distrusted a too-good 26× gemat11 → caught the bench was crippling Eigen with `NaturalOrdering` on the
pre-AMD'd matrix. Switched to Eigen's DEFAULT `COLAMDOrdering` (fair best-vs-best; both factor the same
`ap`, each its own ordering + pivoting). **HONEST FAIR RESULT (Eigen-COLAMD):**

| matrix | Cerid fill | Eigen-COLAMD fill | ratio |
|---|---|---|---|
| memplus | 143,965 | 3,313,356 | **8.97× CRUSH** |
| wang3 | 11.3M | 19.3M | **3.60× CRUSH** |
| af23560 | 10.0M | 11.3M | **1.04× WIN** |
| sherman3 | 204K | 312K | **1.02× WIN** |
| gemat11 | 60K | 85K | 0.89× |
| add32 | 28.9K | 29.9K | 0.90× |
| west2021 | 13K | 15K | 0.24× (tiny — slow AMD) |

The crushes are GENUINE: MC64 static-pivot fills FAR less than COLAMD + partial-pivot on irregular
matrices — the MC64 advantage the unfair NaturalOrdering bench had been HIDING (it had memplus at 0.27×!).
The "26× gemat11" was a crippled-peer artifact (fair = 0.89×). The AMD reorder is ESSENTIAL for
gemat11/af23560 vs Eigen-COLAMD (without it, gemat11's 1.6M fill loses to Eigen's 85K). Full suite +
moat green. CAVEATS: west2021 slow exact-min-degree AMD (rung-3 Amestoy = fix); wang3 1.7e-15→1.3e-11 +
af23560 4.2e-12 static-pivot accuracy (more IR); solve slower on big (separate lever); bench feeds the
pre-AMD'd `ap` (fair between them; cleanest claim = Eigen-COLAMD on the original). NEXT: fast Amestoy AMD.
