# 2026-06-07 — v7-e-2 gate (symbolic-once) + the NLS-vs-CHOLMOD bench + root-cause dig

## Summary

Two pieces of v7-e-2 (sparse-Jacobian LM, the "crush vehicle"):

1. **The gate — DONE & verified.** `SupernodalCholesky` symbolic-once / numeric-per-trial split, wired into the
   sparse-LM driver. Bit-identical, moat-preserving. (Details below.)
2. **The honest CHOLMOD head-to-head — a real, measured NEGATIVE on the synthetic NLS matrix**, and a **root-cause
   dig that corrected my interpretation twice.** The per-factor win is **structure-dependent**.

## The gate (symbolic-once / numeric-per-trial)

`SupernodalCholesky::factorize(..., bool reuse_symbolic=false)` wraps ONLY the expensive symbolic phase
(`symbolic_factorize` = AMD + etree + supernode amalgamation — the v5a CHOLMOD-gap cost) in `if(!reuse)`; `m_sym`
persists. `refactorize(pattern, values, num_workers)` reuses it (numeric only), with a `CRD_ASSERT` guarding the
fixed-sparsity contract. The sparse-LM driver analyzes once, refactorizes every λ-trial — symbolic paid once across
the whole solve (Ceres caches its symbolic the same way). The cheap O(nnz) rebuilds (panel layout, update-lists,
etree levels) rerun each call — negligible vs the numeric factor.

**Verified bit-identical:** `reuse=false` byte-unchanged (v5a moat 547,959 asserts, `[v5a-3]`/`[v5a-4]` green);
new `[v7-e-2]` gate test (`refactorize == fresh factorize`, moat survives reuse); sparse-LM `[v7]` 279/31 green.
Advisor-blessed. Files: `supernodal_cholesky.{hpp,cpp}`, `levenberg_marquardt_sparse.hpp`, `test_supernodal.cpp`.

## The bench

`dump_nls_lattice_jtj` (new runtime example) generates a **3D elastic-lattice NLS** (s³ nodes, 3 DOF, nonlinear
edge-length residuals + soft anchors ⇒ SPD; genuinely NLS, deformation/FEA-class), forms JᵀJ via the same
`transpose`+`spgemm` the driver uses, writes symmetric Matrix-Market. Fed to the existing
`bench_hesap_cholesky_vs_cholmod` (CHOLMOD = exactly what Ceres-sparse uses). Both factor the same AMD-permuted
matrix; CHOLMOD forced supernodal + natural order. Sizes nls_lat{12..32} (n=5K..98K).

### Scoreboard (8 threads, FACTOR ratio = CHOLMOD/Cerid; >1 = Cerid wins)

| matrix | n | FACTOR | note |
|---|---|---|---|
| hood (FEA ref) | 220542 | **1.53× WIN** | reproduces v5a |
| ldoor (FEA ref) | 952203 | **1.57× WIN** | reproduces v5a |
| bcsstk25 (FEA ref) | 15439 | **1.66× WIN** | |
| bmwcra_1 (FEA ref) | 148770 | 1.02× | marginal |
| nls_lat12 | 5184 | 0.74× lose | |
| nls_lat16 | 12288 | 0.76× lose | |
| nls_lat20 | 24000 | 0.82× lose | |
| nls_lat24 | 41472 | 0.82× lose | |
| nls_lat28 | 65856 | 0.74× lose | |
| nls_lat32 | 98304 | 0.67× lose | |

**Cerid LOSES on every NLS-lattice size (0.57–0.83× across 1T/8T), and its fill is ~22% LOWER than CHOLMOD's** (it
finds a better ordering) yet the factor is slower. Real result, fair fight (shared order, residuals match 1e-14).

## Root cause — corrected THREE times (the deep dig)

The "fewer flops yet slower" tell drove the investigation. Hypotheses, in order, each **measured then refuted**:

1. ❌ **"Tiny-supernode (nc 2-4) kernel wall"** (my first claim, advisor-endorsed) — **BACKWARDS.** The CHOLPROF
   bins show **94% of cdiv flops are in nc≥512 (large) supernodes**, not tiny ones.
2. ❌ **Conservative amalgamation** — refuted: Cerid vs CHOLMOD supernode histograms are **identical** (nls_lat32:
   nsuper 20796 vs 20788; bins match). Same supernode structure.
3. ❌ **Skinny-K cmod / left-looking per-pair overhead → update-aggregation** (the advisor's alternative) — refuted:
   instrumenting `cmod_slab` by K=knc shows the cmod is **96% fat-K (knc≥128) at 63.7 GF/s**, and the
   binary-search/scatter overhead is only **5–8%**. The cmod GEMM is *efficient*.

✅ **Actual cause (deep dig, thread-confound removed): the within-supernode PANEL FACTORIZATION (cdiv — the
below-diagonal TRSM), NOT the GEMM and NOT asm.** The user twice pushed back on "kernel wall"; both times correct.

Clean **single-thread** numbers (verified genuinely 1-thread by `taskset -c 0`: pinning the whole process to one
core leaves CHOLMOD's lattice time UNCHANGED at ~3188ms, so it was single-threaded all along. cc.fl=210 GFLOP /
3.19s = 66 GF/s — a valid single-core rate. An earlier "132 GF/s / multithreaded" worry was a flop-convention
misread: cc.fl counts flops, not MAD-pairs, so my Σ2(m-i)² structural count was exactly 2× cc.fl):

| matrix | Cerid GF/s | CHOLMOD GF/s | ratio | panels |
|---|---|---|---|---|
| nls_lat32 | 38.6 | 65 (cc.fl=210 GFLOP / 3.23s) | **0.59×** | large nc≥512 |
| hood | 19.8 | 20.4 | **0.97× (parity)** | small nc 5-8 |

The gap is **panel-size-dependent**, which localizes it. Decomposition of the lattice factor: cmod (between-supernode
GEMM, 58% of time) + cdiv (within-supernode panel factorization, 42%). And **83% of cdiv is the below-diagonal TRSM**
(the "outer_trail," 1924ms = **35% of the whole factor**). The code's own v5a-4 comment states the gap directly:
*"OpenBLAS dtrsm = 65 GF/s on this shape, Cerid was 26"* (now ~40). **So the dominant single gap is the panel TRSM
(~0.6× OpenBLAS dtrsm), not the GEMM.** The cmod GEMM is the *competitive* path (≈0.7× OpenBLAS — the known
ADR-0082 intrinsic-vs-framework gap). **asm targeted the GEMM — the wrong kernel** — which is why ADR-0088's
hand-tuned asm (built, benchmarked, reverted at ≈0.98× the inlined intrinsic) made no difference: the GEMM was never
the bottleneck.

**Why panel-size-dependent:** the lattice is a high-fill 3D volume → huge supernodes (maxnc 5385) → the panel
factorization (cdiv TRSM/POTRF) is 42% of the work and runs at ~0.6×. hood/ldoor are thin-walled FEA (nc 5-8) →
panel factorization is negligible → the factor is gemm-dominated and Cerid is at parity/wins (the cmod GEMM ≈
OpenBLAS on small panels, and Cerid out-scales CHOLMOD at 8T on the many small fronts).

**The lever (corrected — there IS one, C-level, moat-safe):** the **cdiv panel factorization** — a better-blocked /
recursive POTRF + the below-diagonal TRSM (LAPACK/OpenBLAS dpotrf/dtrsm class). It's already been improved once
(v5a-4: 26→40 GF/s); OpenBLAS is at 65. This is C-level blocking work, NOT asm, and it **preserves the determinism
moat**. Secondary: the cmod GEMM's ~0.7× (Goto-class C packing framework — bigger, also not asm). Estimated reach:
fixing the panel TRSM toward 65 takes the lattice from 0.59× toward ~0.75–0.8×; closing both gaps → ~parity, with the
`{1..16}` moat as the differentiator. ⚠ convention caveat: CHOLPROF flop counts (×2) vs cc.fl (×1) are not directly
comparable — the GFLOP/s *ratios* and the panel-TRSM localization are solid; exact per-kernel %-of-OpenBLAS carries
residual uncertainty. (Linking OpenBLAS would win speed but forfeit the moat — not an option.)

## Honest v7-e-2 verdict

- **Per-factor speed is structure-dependent:** Cerid beats CHOLMOD on FEA/deformation-thin-walled JᵀJ (hood/ldoor
  1.5×, bcsstk25 1.66×) but loses on high-fill 3D-volume JᵀJ (the lattice, 0.6–0.8×) — the ADR-0088 dense-kernel
  ceiling on large panels.
- **The universal differentiator is the `{1..16}` determinism moat** CHOLMOD/Ceres lack — holds on *every* matrix.
- **Do NOT headline "crush Ceres-sparse."** Honest claim: matched accuracy + bit-identical-across-threads, with a
  per-factor win on thin-walled FEA structures and a kernel-bound loss on dense 3D volumes.
- scipy `least_squares` is not a same-class peer (dense QR / LSMR-SVD) → correctness cross-check only.

Raw logs: `docs/sessions/data/2026-06-07-nls-cholmod-bench-8t.log` (+ 1T per-size in this session's transcript).

## FINAL UPDATE — gate-measured root cause + first fix SHIPPED (supersedes the kernel-ceiling/TRSM framing above)

The "no crush / kernel ceiling / panel-TRSM" conclusions above were **superseded by a convention-pinned flop
measurement** (the earlier framings compared cross-tool flop counts without pinning the convention — the churn signal
the advisor flagged). Pinned result (both numbers in Cerid's own fma=2 units; min validated == CHOLMOD cc.fl;
`taskset -c 0` proved CHOLMOD genuinely single-thread so cc.fl is the true necessary work):

- **CERID Cholesky-min-flops = Σ_cols(m-i)² = 210.5 GFLOP = CHOLMOD cc.fl** (the minimum).
- **CERID EXECUTED = cmod 207 + cdiv 85 = 292 GFLOP ⇒ 1.39× EXCESS.**
- Cause: **symmetric Schur/trailing updates computed as full GEMM where SYRK (half) is needed** — real redundant
  arithmetic, NOT a kernel ceiling, NOT asm, NOT Amdahl, NOT the TRSM. (My earlier "structural=421" had an erroneous
  ×2; min is 210.5.)

**FIX STEP 1 SHIPPED + VALIDATED — serial cdiv (C) trailing symmetry split:** the within-panel trailing's diagonal
block [koend:nc]² is computed LOWER-TRIANGLE-ONLY (kTriPanel=128 column-panels) + strictly-below block one full gemm.
**lattice nls_lat32 1T 0.57→0.65× (5552→4886ms); nls_lat20 0.65→0.74×; moat green (549K asserts {1,2,4,8});
ZERO FEA regression (hood 8T 1.60× WIN, ldoor 1.73×); residuals 9.1e-15.** Bit-identical on the used lower triangle
(each entry keeps its single K=obw reduction; only the unused upper triangle is skipped). The crush mechanism is
PROVEN on one of three redundancy pools.

**STEP 2 tried+reverted:** node-parallel (C) per-panel split hit the v5a-7 per-call-fork/join wall (~40 forks/front,
zero 8T gain) → reverted to the proven path. The 8T version needs a ROW-SLAB parallelization (one fork).

**REMAINING (careful moat-critical, focused-session work):** (a) **cmod diagonal-block SYRK** — the biggest pool
(~52 GFLOP; sub≈m1 so ~half each call), best via a syrk-ACCUMULATE restructure (per-(s,k) triangular risks the
per-call-overhead trap on 82K calls); (b) node-parallel (C) row-slab split (8T). Kill these → 1T→parity → 8T win via
Cerid's measured 3.4×-vs-2.93× scaling. The crush is reachable; the path is proven; the remaining steps are the same
transformation on the harder pools, each verified the same way (clawback + moat + FEA no-regression).

## STEP 3+4 SHIPPED — cmod diagonal SYRK + balanced-triangular node-parallel (C) two-pass

All three redundancy pools now eliminated; **min-flops == CHOLMOD cc.fl** (lat32: CERID 210.54 GFLOP == cc.fl
210.38) — the 1.39× symmetry excess is **gone**, and Cerid still has **~22% LESS fill** (67.9M vs 87.1M).

- **STEP 3 (cmod diagonal SYRK):** the cmod's diagonal block [r0:nc] computed lower-triangle-only via
  `syrk_lower_minus` + a `col_limit` param (skips cross-band upper); below-band via gemm. Serial + node-parallel
  (two-pass over `parallel_for_triangular`).
- **STEP 4 (node-parallel (C) two-pass):** the v5a-7 full-Tᵀ `gemm_parallel`+`sub_col` path (which carried the
  symmetry redundancy at 8T) replaced by the **balanced-triangular** primitive
  (`engine/hesap-dense/.../detail/parallel_triangular.hpp`, `triangular_bound(n,k,w)=round(n·√(k/w))`): DIAGONAL
  block split into sqrt-balanced row-bands (band [r0,r1) computes cols [0,r1), lower-subtract, per-lane `ubuf`
  scratch) + BELOW block via regular `parallel_for`. The earlier row-slab attempt regressed (last lane ~8× the
  first); the sqrt partition balances triangular area (tested: max/min < 1.30).

**Determinism:** the partition decides only WHO computes a row, never the VALUE (each entry keeps its single K=obw
reduction) ⇒ bit-identical across {1,2,4,8,16}. `[v5a-3]`/`[v5a-4]` (grid3d huge-front node-parallel) + `[v7-e-2]`
green; full hesap-direct suite 598,861 asserts green.

**8T scoreboard (FACTOR ratio CHOLMOD/Cerid; >1 = Cerid wins):**

| matrix | n | FACTOR 8T | vs prior |
|---|---|---|---|
| hood (FEA) | 220542 | **1.57× WIN** | held |
| ldoor (FEA) | 952203 | **1.74× WIN** | held |
| bmwcra_1 (FEA) | 148770 | **1.15× WIN** | was the ADR-0082 wall → now a win |
| bcsstk25 (FEA) | 15439 | **1.73× WIN** | held |
| nls_lat20 | 24000 | **0.95× (parity)** | 0.82→0.95 |
| nls_lat24 | 41472 | 0.94× | 0.82→0.94 |
| nls_lat28 | 65856 | 0.82× | 0.74→0.82 |
| nls_lat32 | 98304 | **0.77×** | 0.67→0.77 |

**Verdict: parity reached on mid lattices (lat16–24 at 0.87–0.95×); the two largest (lat28/32 at 0.77–0.83×) remain
short.** The 1.39× flop excess that drove the loss is fully removed AND Cerid runs fewer flops with less fill — so
the residual gap is no longer arithmetic.

## STEP 5 — the DECISIVE 8T phase decomposition (refutes the "kernel ceiling" verdict)

The advisor flagged that "within-front parallel-BLAS ceiling" was a guess, and that the verdict was BLOCKED on a
real measurement: of the giant root front's 8T time, how much is (a) the sequential panel-sweep critical path
(POTF2 + within-panel solve — un-parallelizable ⇒ Amdahl ceiling) vs (b) the parallel phases not scaling. Added a
`CRD_HESAP_CHOL_SCALE_PROFILE` within-node-front phase split (node-parallel fronts dispatch SEQUENTIALLY ⇒ the
dispatcher is a single writer ⇒ race-free non-atomic timers). Clean lat32 8T (median of the steady runs):

| phase | ms | what |
|---|---|---|
| SETUP (alloc+lvl) | **194** | the ~2 GB `ubuf` per-worker scratch alloc + first-touch + level build |
| SETUP (sym) | 19 | symbolic (reused under LM) |
| node cmod | **641** | between-supernode assembly (Schur updates) — the biggest single phase |
| node ctrailC | **364** | (C) outer trailing Schur (two-pass) |
| node bsolveB | 91 | (B) below-outer trsm |
| **node cdivA (serial chain)** | **16 (1.5%)** | (A) POTF2 + within-obw solve — the *only* un-parallelizable part |
| tree + starved | 73 + 80 | small-front levels |
| **total** | **≈1483** | vs CHOLMOD 1267 (gap 216) |

**(a) is REFUTED: the serial chain (cdivA) is 16 ms = 1.5% of the node front.** lat32 is NOT Amdahl-serial-bound —
my pre-measurement "sequential critical path" worry (and the older "kernel ceiling") were both wrong. The cost lives
in the **parallel** GEMM phases (cmod 57% + ctrailC 32%) and the **SETUP allocation** (194 ms = 13% of the whole
factor). The fork size-gate (STEP 4.5, shipped — gate the late-panel `parallel_for_triangular`/below forks below
`kGemmParallelMinFlop`, bit-identical) nudged lat32 0.77→0.83×, confirming fork overhead was a minor (c) contributor
but not the bulk.

**The honest reframe:** the residual gap is (i) the **SETUP scratch alloc** (194 ms — pure overhead, NOT GEMM, and
re-paid every `refactorize` under LM ⇒ doubly worth fixing) and (ii) the parallel **cmod/ctrailC GEMM throughput**
(the ADR-0082 deterministic-intrinsic-vs-OpenBLAS-MT gap — the user has confirmed hand-tuned asm does not move it).
Lever (i) is concrete and moat-safe (allocation sizing/reuse — values untouched); lever (ii) is the known dense-BLAS
wall. Killing (i) alone projects lat32 → ~0.95×. **No FEA regression; the {1..16} moat holds on every matrix.**

## STEP 6 — uninit-scratch lever + a real GEMM beta=0 BUG it exposed (SHIPPED, parity on lat24)

Lever (i): `ubuf.resize()` (value-init = a ~2.2 GB zero-fill memset, the 194 ms) → `ubuf.resize_uninitialized()`.
`ubuf` is per-worker GEMM/copy scratch — every element is written before read — so the zero-fill is dead work.

**The advisor's warning was load-bearing: the {1..16} moat CANNOT catch a resulting uninitialized read** (reused
resident pages read coincidentally-identical bytes ⇒ bit-identical yet UMR). Validated instead with a **NaN-poison**
(`0xFF`-fill the uninit `ubuf`; any read-before-write ⇒ NaN into the factor). The win-debug moat PASSED with poison
(small grids use the thin scalar cdiv path — no `ubuf`), but the **lat32 root front (nc=5385) factored NOT-SPD
(info=529)** — the poison caught a genuine read-before-write that the moat could not.

**Root cause = a latent BLAS bug, not my audit miss.** `blas3.cpp` `gemm`/`small_gemm`/`gemm_parallel`/`gemm_mixed`
scaled C by beta as `c = beta * c` with **no `beta == 0` branch** (the comment claimed one; the code lacked it). So
gemm with beta=0 *reads* C and computes `0 * c` — which is `NaN` for non-finite c, violating the BLAS contract
(*beta=0 ⇒ C not referenced on input*). Harmless with zeroed scratch (`0*0=0`) but corrupts on uninitialized (or,
latently, on any `refactorize` whose pages hold a prior factor's Inf/NaN). **Fix:** `if (beta == 0) c = 0; else c =
beta*c;` in all four sites. Bit-identical for finite C (`0*finite = ±0.0`, and `±0.0 + alpha·AB = alpha·AB`) ⇒
moat-safe; correct for uninitialized ⇒ enables the lever; and a genuine spec-compliance fix on its own.

**Validated:** poison re-run with the fix ⇒ lat32 resid **9.1e-15** (correct, not NaN) with NaN garbage in `ubuf` ⇒
every consumer is provably write-before-read. Full suites green: dense gemm **359 508** asserts / 349 cases, direct
moat **598 861** asserts / 190 cases (`{1..16}` bit-identical). ASan + 6-config pending.

**8T scoreboard after the lever (clean):**

| matrix | n | FACTOR 8T | arc this session |
|---|---|---|---|
| hood (FEA) | 220542 | **1.53× WIN** | held |
| ldoor (FEA) | 952203 | **1.69× WIN** | held |
| bmwcra_1 (FEA) | 148770 | **1.09× WIN** | held (was ADR-0082 wall) |
| **nls_lat24** | 41472 | **0.99× = PARITY** | 0.82 → 0.99 |
| nls_lat20 | 24000 | **0.95×** | 0.82 → 0.95 |
| nls_lat28 | 65856 | 0.87× | 0.74 → 0.87 |
| nls_lat32 | 98304 | **0.83×** (1240 ms, was 1483) | 0.66 → 0.83 |

**Verdict: parity reached up through lat24; lat28/32 at 0.83–0.87×.** The session moved every lattice up (lat32
0.66→0.83×) via three real, measured, moat-safe levers — SYRK (the 1.39× flop excess), the fork size-gate, and the
uninit-scratch lever (which surfaced+fixed the gemm beta=0 bug). **Zero FEA regression; the {1..16} moat holds on
every matrix — the differentiator.**

## STEP 7 — the conclusive scaling sweep (it is the SERIAL kernel, NOT parallel scaling)

The advisor required measuring cmod/ctrailC scaling before any "kernel wall" claim. lat32 FACTOR by worker count:

| W | Cerid ms | CHOLMOD ms | Cerid scaling | CHOLMOD scaling | ratio |
|---|---|---|---|---|---|
| 1 | 4300 | 3147 | 1.00× | 1.00× | **0.73×** |
| 2 | 3237 | 1975 | 1.33× | 1.59× | 0.61× |
| 4 | 2033 | 1218 | 2.12× | 2.58× | 0.60× |
| 8 | 1359 | 1139 | **3.16×** | **2.76×** | **0.84×** |

**Cerid SCALES BETTER than CHOLMOD (3.16× vs 2.76× at 8T).** The lat32 loss is therefore NOT a parallel-scaling
failure and NOT an Amdahl serial-chain ceiling (cdivA=1.5%, STEP 5). It is **inherited from the serial (W=1) gap of
0.73×** — the per-thread GEMM kernel rate (~0.73× OpenBLAS serial = the ADR-0082 intrinsic-vs-OpenBLAS wall, which
the user has confirmed hand-tuned asm does not move). Cerid's superior scaling *narrows* the gap from 0.73× (1T) to
0.84× (8T), and the trend (3.16× vs 2.76×) crosses to a Cerid win at higher core counts. **This is the honest,
measured ceiling for the largest 3D-volume lattices: it is the single-thread dense-BLAS kernel, full stop — not
allocation, not symmetry flops, not fork overhead, not Amdahl, not scaling.** Closing it = re-opening ADR-0082
(deferred indefinitely); the determinism moat is the standing differentiator CHOLMOD lacks.

### Net session result (v7-e-2 perf arc)
lat32 0.57× (1T, pre-SYRK) / 0.66× (8T) → **0.83× (8T)**; lat24 → **0.99× = parity**; lat20 0.95×; FEA hood/ldoor/
bmwcra all WIN; a real BLAS beta=0 spec bug found+fixed; the {1..16} moat intact throughout. Every gain came from a
profile-/poison-measured real cause, never a guess — and the residual is now pinned to one measured wall.
