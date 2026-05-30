# 2026-05-30 — hesap v5a-6: per-level solve work-gate (REFUTED) + bmwcra scaling diagnosis

## Context / how this session started
Resumed mid-CHOLMOD-crush. The PRIOR session (v5a-5) emitted **fabricated bench numbers**
(tool-output corruption; documented in its own log). So this session's first job was to separate
trust layers: **the code is real** (diff verified vs git), but **every perf ratio was a suspect
claim** until re-measured with file-captured output.

## Anti-fabrication protocol (used throughout)
- Every benchmark writes stdout to a FILE, then the file is Read (fabrication entered via inline
  tool output; file capture is the defense).
- WSL runs go through a **script file** (`tr -d '\r'` then `bash`), never `wsl bash -c "...$VAR..."`
  — shell vars get eaten through the wsl layer (caused an empty-`$T` sweep early on).
- Threads capped at 8 (14900K Raptor Lake crash hazard).

## Correctness re-established (LOCAL, trustworthy)
`ctest --preset win-debug -I 2465,2478` → **14/14 passed, exit 0**, incl. #2473 (v5a-5 single-RHS
solve bit-identical across {1,2,3,pool} workers) + all determinism moats + residual checks. The
v5a-5 code is sound. `ninja: no work to do` confirmed the working-tree source was already compiled
clean.

## Re-established CHOLMOD baseline — REAL, 8T, file-captured (vs prior FABRICATED claims)
CHOLMOD on openblas-pthread (verified `update-alternatives`); same fast BLAS as Cerid → fair.
Ratio = chol/cerid (≥1.0 = Cerid WIN). Fill: Cerid is 11–16% UNDER CHOLMOD everywhere.

| Matrix | n | FACTOR | SOLVE x1 | SOLVE x16 |
|---|---|---|---|---|
| bcsstk13 | 2003 | 0.57× lose | 1.53× WIN | 0.16× lose |
| bcsstk24 | 3562 | 0.66× lose | 0.97× | 0.19× lose |
| bcsstk25 | 15439 | **1.69× WIN** | **1.38× WIN** | 0.52× lose |
| bmwcra_1 | 148770 | **0.66× lose** | **0.62× lose** | **0.86× lose** |
| hood | 220542 | **1.45× WIN** | ~0.85–0.98× (noise floor) | **1.66× WIN** |
| ldoor | 952203 | **1.26× WIN** | **1.10× WIN** | **2.09× WIN** |

Corrections vs the fabricated v5a-5 claims: hood x1 is ~0.85× not 0.91× (and is noise-floor, see
below); SOLVE x16 LOSES on all small matrices (0.16–0.52×) — the prior "x16 crushes every matrix"
claim was false (small-matrix x16 dispatch overhead; bounded-by-importance, sub-ms). FACTOR wins
confirmed real and slightly better than claimed (the symbolic-breakthrough wins hold).

## v5a-6 per-level work gate — IMPLEMENTED, SWEPT, **REFUTED**, REVERTED
Hypothesis: hood x1 (0.85×) loses to per-level `parallel_for` dispatch overhead on its deep etree;
gate = parallelize a level only when `nrhs · Σ(panel entries) ≥ threshold`, else serial. Built it
(precompute `m_lvl_work`, nrhs-scaled gate on fwd/back), with an env-override tuning hook, then
**swept threshold-to-ceiling on hood x1 first** (premise check before tuning, per advisor):

| Threshold | hood x1 (ms) | x1 ratio | x16 ratio |
|---|---|---|---|
| 0 (=current always-parallel) | 24.83 | **0.98× (best)** | 1.81× WIN |
| 1e3–1e5 | 25.6 | 0.89–0.94× | ~1.7× |
| 1e6 | 27.2 | 0.83× | 1.73× |
| ≥1e7 | ~47 | 0.47× | collapses |

**Premise refuted (advisor predicted it):** T=0 (current) is the BEST point — serializing thin
levels only HURTS, collapsing once parallelism breaks. hood's levels all benefit from parallelism;
there is no dispatch-overhead waste to reclaim. When hood x1 loses, the gap is **memory bandwidth
on the gather/scatter** (CHOLMOD's threaded-BLAS solve matches it), NOT dispatch overhead a gate
could touch. Also: the hood x1 ratio swings 0.85→0.98× purely by run context (hood-after-bmwcra vs
hood-alone) = ~±10% bench variance that swamps the signal → hood x1 sits at the **noise floor**, not
a reliable loss, and is not improvable by this lever.

**Reverted in full** (gate was a literal no-op at default=0 anyway; carried a temp env hook;
clean-structure value). KEPT the harmless `CRD_BENCH_ONLY=<name>` bench filter (tooling for fast
low-load sweeps, like `CRD_BENCH_NO_REF`). Tree is back on the verified v5a-5 (14/14).

## Honest v5a-solve standing
ldoor = clean sweep (factor+x1+x16 WIN). hood = factor+x16 WIN, x1 at noise floor. bcsstk25 =
factor+x1 WIN. **The lone genuine crush gap is bmwcra** (factor 0.66 / x1 0.62 / x16 0.86) — the
two-walls matrix (per-thread intrinsics ceiling + scaling plateau). Small-matrix x16 losses are
bounded-by-importance (sub-ms; would need a SEPARATE x16-only gate — one threshold can't serve both
kernels, the sweep proved a value helping small-x16 regresses hood x1). User chose: tackle bmwcra.

## bmwcra scaling diagnosis (measure-first, in progress)
WSL2 **homogenizes the P/E topology** (reports 16 cores × 2 threads, no P/E distinction) → in-WSL
`taskset` P-core pinning is unreliable. BUT the Cerid-vs-CHOLMOD **ratio is fair** (both threads in
the same opaque-scheduled environment); the E-core confound only muddies the *absolute* self-scaling
number, not the comparison. So: read the **bucket SPLIT** (tree/node/starved), not the absolute
scaling, to choose the lever.

Decision tree (pre-committed before reading the numbers, per advisor):
- **starved-dominated** → cross-level task DAG (`crd-hesap-sched::DependencyGraph`): drop level
  barriers, fill idle workers with ready supernodes from other levels.
- **node-dominated** → finer 2D within-front decomposition (huge fronts not scaling via gemm_parallel).
- **tree-dominated + scales cleanly** → NO scaling lever; it's the ADR-0082 intrinsic-vs-asm kernel
  ceiling → characterize bmwcra factor at the kernel wall, pivot to v5b. Report honestly, don't force a DAG.

Moat invariant for any dynamic scheduler: the **fixed per-supernode k-ascending reduction order**
(the DAG changes who/when, not the accumulation order → bit-identity survives, as it did for the
level-parallel solve).

### bmwcra diagnosis RESULT (CRD_HESAP_CHOL_SCALE_PROFILE, WSL 8T capped, file-captured)
Factor scaling (vs CHOLMOD 8T = 1397ms): 1T 3418 → 2T 2520 (1.36×) → 4T **1798 (1.90×, BEST)**
→ 8T **1994 (1.71×, REGRESSES vs 4T)**. So bmwcra factor PEAKS at ~4 threads and gets WORSE at 8.

8T NUMERIC split (stable repeats): tree **263ms** | **node 1354ms (75%)** | starved 183ms (10%) |
SETUP ~190ms (sym 67 + alloc+lvl 126; serial, ~10%). Bucket scaling:
- **tree** 1255@2T → 487@4T → 263@8T (~4.8×, scales beautifully).
- **node** 958@2T → 985@4T → **1354@8T** (ANTI-scales — the wall).
- starved 133→158→183 (minor, grows slowly as `cnt<num_workers` triggers more at higher W).

**DIAGNOSIS → node-dominated, and node anti-scales past 4T.** The few huge fronts (nc=2406, one
level = 156ms, cnt=1) run node-parallel (supernodes sequential on the dispatcher, each driving
`gemm_parallel`); that `gemm_parallel` on the front's modest NT-ColMajor cmod/cdiv gemms does NOT
scale and is counterproductive at 8 workers (overhead + memory contention + the `cnt<num_workers`
routing sends MORE levels to node-parallel at higher W). CHOLMOD factors the same fronts via
multithreaded OpenBLAS dpotrf/dsyrk/dgemm INSIDE the front, which DOES scale.

**This CORRECTS the standing assumption:** the cross-level task DAG (`DependencyGraph`) is NOT the
bmwcra lever — it would help `starved` (10%), not `node` (75%). The lever is **within-front parallel
efficiency / 2D within-front decomposition** (advisor decision-tree: node-dominated branch). Part of
the gap is the ADR-0082 intrinsic-vs-asm gemm ceiling (Cerid gemm ~0.71× OpenBLAS), part is
parallel-gemm over-subscription on modest fronts.

### Proposed lever ladder for bmwcra (next session — measure each)
1. **CHEAP first:** cap within-front parallelism so 8T doesn't over-subscribe modest fronts
   (per-front worker count ∝ front flop; the existing `kGemmParallelMinFlop` gate may under-serve).
   Goal: recover the 4T→8T regression (≥ match 4T's 1798ms at 8T → ~0.78×). Bounded, contained.
2. **DEEP:** a within-front parallel dense Cholesky that SCALES to 8T (2D block decomposition of the
   huge fronts), against the ADR-0082 gemm ceiling — the real crush.

NOTE the success metric is the CHOLMOD ratio (both in WSL → fair); the absolute "1.71×@8T self-scaling"
is partly the WSL E-core homogenization artifact and is NOT the design target.

### bmwcra node-regime split (n_node_levels + cnt histogram, stable 8T runs)
| W | NODE total | cnt==1 huge fronts | cnt>=2 reclassified | tree |
|---|---|---|---|---|
| 2 | 959ms / 12lev | 959ms / 12lev | 0 | 1248ms |
| 4 | 977ms / 19lev | 636ms / 12lev | 341ms / 7lev | 491ms |
| 8 | 1375ms / 20lev | 778ms / 12lev | 597ms / 8lev | 267ms |

Confirms the advisor's reclassification hypothesis: the **12 cnt==1 huge fronts are STABLE** (they
scale 2->4: 959->636, then OVER-SUBSCRIBE 4->8: 636->778); the **cnt>=2 node levels GROW** (0->7->8)
as `cnt < num_workers` admits more — pure routing waste (597ms@8T) that should stay tree-parallel.
tree scales beautifully (1248->267, ~4.7x). So BOTH a routing-predicate fix AND a within-front
worker-cap are indicated.

### ▶▶ PIVOTAL: the flop ratio is ~1.00, NOT the 0.84 fill ratio (MEASURED)
`cerid_flop = 1.2859e11` (Sum nc^3/3 + nc^2*below + nc*below^2 over Cerid supernodes) vs CHOLMOD
`cc.fl = 1.2866e11` → **flop ratio ~= 1.00 (EQUAL to 0.05%).** Sanity: nonzero, ~1.3e11 (plausible),
not a 2x convention artifact.

**This REFUTES the load-bearing prior-session thesis** ("Cerid 11-16% under CHOLMOD fill => fewer
flops => matching efficiency WINS via the fill margin", repeated across context.md). **Fill != flops.**
The +19% CHOLMOD fill is in the small-front tail (negligible to flops); the flop-dominant huge fronts
are structurally near-identical (nsuper ~= equal per the deep-research probe) => EQUAL flops. There is
NO fill margin to exploit.

**Ceiling = (1/flop_ratio) x rate_ratio = 1.00 x rate_ratio ~= 0.71-0.82x** (Cerid gemm vs OpenBLAS:
cmod in-situ 51.7/63.4 = 0.82x; cdiv 0.45-0.71x; blended ~0.75x = the ADR-0082 intrinsic-vs-asm wall).
bmwcra's 4T factor (0.78x) is ALREADY at this ceiling; the 8T regression (0.66x) is pure waste.

### ▶ HONEST CONCLUSION (measured, not assumed) — bmwcra factor is KERNEL-CEILING-BOUND
1. **Cheap routing+cap fix** (route only cnt==1 && nc>=BIG to node; cap per-front gemm workers so 8T
   stops over-subscribing) recovers 8T 0.66x -> ~0.78x = the achievable ceiling. Bankable, contained,
   moat-safe (worker count doesn't change values — gemm_parallel splits by output rows).
2. **Tile-DAG within-front: NOT worth it.** Even a perfect barrier-free tile-DAG tops at the gemm
   rate ceiling (~0.82x); marginal over the cheap fix, multi-session, still <1.0x. Refuted by the
   equal-flop finding.
3. **>=1.0x requires the deferred ADR-0082 asm microkernel** (close the 0.71-0.82x gemm per-flop gap).
   That is the ADR-0082 three-condition-gate STRATEGIC decision (intrinsics->asm), not a lever — the
   user's call. "Full crush of bmwcra factor" provably needs it.

bmwcra x16/x1 SOLVE loss is the same gemm-ceiling story (solve is gemm-bound + serial-ish). SETUP
~190ms serial (alloc+lvl 126ms) is a growing Amdahl floor as numeric drops — note, don't chase yet.

## Working-tree state at this point
v5a-5 (verified, 14/14) + `CRD_BENCH_ONLY` bench filter (kept tooling). v5a-6 gate fully reverted;
temp profiler `#define` removed. Throwaway logs/scripts cleaned. Untracked: ADR 0086/0087 (unrelated
planning), the v5a-5 session log, this log. NOT yet committed.

