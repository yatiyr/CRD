# 2026-06-01 — hesap v5b-3d: node-level parallelism + adaptive MC64 (the MUMPS-gap attack)

> Continues the v5b-3 multifrontal LU. Goal (user): attack the MUMPS parallel gap on the CFD targets,
> honestly. Advisor-steered throughout; measure-first; every claim file-captured. **WIP — not committed.**
> Full session arc + every measurement: memory `project_lu_umfpack_gap_is_mc64_not_gemm`.

## The steer (advisor): node-level, NOT async-DAG
For a 3D problem (ns3Da) the root separator front holds a *constant fraction* of total factor flops and depends
transitively on everything ⇒ tree/async-DAG parallelism cannot start it earlier or split it. Async-DAG only
helps wide/shallow trees — the circuit/2D class we already win. Node-level (within-front) parallelism is what
MUMPS/PaStiX/SuperLU_DIST all carry for 3D; it's mandatory, not optional.

**Measured to confirm (ns3Da @8w):** the narrow (big-front) path is **74%** of the numeric and scales only
**1.87×** at 8 workers (slowest front 40→18 ms). Stage split of the 1407-front: panel ~2% (hidden by
lookahead), **TRSM ~21% serial**, trailing GEMM ~76% (rank-48 thin-K ⇒ bandwidth-bound). Barrier-idle small.
Async-DAG would have chased the wrong thing.

## Levers landed (all moat-safe; suite 591617/54 green, gcc -Werror clean)
1. **Adaptive MC64** — the big one. Factor the **natural diagonal first** (faster + better-conditioned), fall
   back to MC64 only when element growth blows past `kMfGrowthThreshold` (1e12·‖B‖). Growth is tracked
   near-free in the L/U store; a per-level **early-abort** + a **fraction-of-dense pre-bail** cap the cost of a
   doomed natural attempt (gemat11 1751→147→32 ms). The moat is *static* pivoting, not MC64 ⇒ preserved.
   - Drops the ~100 ms MC64 tax on strong-diagonal CFD.
   - **Fixes the saddle-point matrices**: garon2 `5.6e+01 INACCURATE → 1.7e-12 [ok]`, raefsky3 fixed. MC64's
     scaling was *causing* their inaccuracy — reverses the v5b-2c "static-pivot-unsafe-on-saddle-point" call.
2. **Parallel TRSM** — split the panel's U block-row across workers by column (independent ⇒ bit-identical).
3. **Wider panel nb=128** — the trailing rank-nb GEMM becomes compute-bound (better single-thread + scaling);
   coupled to (2) since wider nb grows TRSM work linearly.

## Why adaptive needs growth, not a diagonal pre-check
`min_diag_dominance` does NOT separate the cases: it is **0.000** for both gemat11 (blows up → needs MC64)
*and* ns3Da/garon2 (factor fine without it). The distinguisher is **element growth during factorization**
(gemat11 → 1e164; targets stay O(1)–O(1e2)). User chose the growth-monitored fallback (SuperLU_DIST-class).

## Honest scoreboard — vs MUMPS @8t (warm, OPENBLAS=1, 3 runs, matched accuracy)
| matrix | MF factor | vs MUMPS | note |
|---|---|---|---|
| af23560 | 241–242 ms | **1.04–1.06×** | consistent win |
| wang3 | 279–292 ms | 0.94–0.98× | near-parity |
| ns3Da | 417–422 ms | 0.93–0.95× | near-parity (was **0.64×** at session start) |

**Honest framing:** from a clear ns3Da loss (0.64×) to near-parity on ns3Da/wang3 + a real af23560 win, **plus
the determinism moat MUMPS lacks, plus the saddle-point matrices fixed.** NOT "beat MUMPS on all three" — the
1.01×/1.20× single runs were favorable samples; ties are not wins.

**Disclosures:** af23560 loses to UMFPACK-*serial* 0.86× (small-front regime); gemat11 4→32 ms non-target
regression; MUMPS *solve* not separately timed (solve win confirmed only vs UMFPACK — ns3Da 26.5 vs 61 ms);
warm/cold cuts in our favor (we re-allocate per call and still reach parity); MUMPS uses its own ICNTL(7)=auto
ordering on the same input matrix (fair, not handicapped onto AMD).

## Moat — verified on the NEW paths (advisor caught the gap)
The old `[v5b-3c]` largest front (260×260) never tripped parallel TRSM (post-128-panel trailing 132 < 256).
Extended it: a **512-wide dense front** (trips parallel TRSM) + an **n=96 ~zero-diagonal fallback matrix**
(confirmed the MC64 fallback fires: PREP mc64_off=1→0) factored at {1,2,4,8} → **L,U bit-identical**. The
growth/abort/fallback decision is worker-count-independent by construction (computed from a bit-identical
factor, reduced by an order-independent max) and now tested.

## v5b-3e — CRUSH ACHIEVED (win/win/tie vs MUMPS); the lever was a one-line tuning, not the malleable rewrite

**Step 0 (measure first):** bucketed the narrow time by level width. ns3Da = **80% SINGLETON** (k=1 root chain
→ node-parallel only); wang3 = **73% MULTI-front** (k=2..7). The two near-tie targets needed *different* levers.

**Step 1a (node-parallel singletons → ns3Da):** parallel `zero_fill` + parallel extend-add (the serial
assembly of the big singleton fronts; the scatter parallelizes over child columns — `cmap` injective ⇒
disjoint parent columns ⇒ bit-identical, children still fixed-order). ns3Da singleton 158→135 ms; **+3–5%**.

**Step 2 (wang3): ran the cheap discriminator before the risky rewrite.** Planned malleable-2D (teams), but
first tried an env threshold `CRD_MF_FRONTPAR_K` (`cnt >= K → front-parallel`). **Swept it: the old `cnt >= sw`
was simply too conservative; the sweet spot ≈ sw/2 (=4 @8w) flipped both targets — no team-plumbing needed.**
Shipped `front_par_thresh = max(2, sw/2)`.

**Final honest 3× vs MUMPS @8t (warm, OPENBLAS=1, matched accuracy):**
| matrix | session start | now | verdict |
|---|---|---|---|
| af23560 | 1.16× | **1.12–1.20×** | WIN |
| wang3 | 0.88× | **1.01–1.11×** | WIN (bounces) |
| ns3Da | **0.64×** | **0.99–1.01×** | TIE |

**Cerid now wins af23560 + wang3 and ties ns3Da vs MUMPS — with the determinism moat none of them have, and the
saddle-point matrices fixed.** Honest: wang3 win is variable; ns3Da is a tie, not a win; af23560 still loses to
UMFPACK-*serial* (0.86×); gemat11 carries 4→32 ms. ns3Da's last ~1% is the singleton GEMM (row-slab/bandwidth
wall, a repeated dead-end) — diminishing returns, not chased.

**Lesson:** before a multi-session "true malleable-2D / async-DAG / asm" rewrite, run the cheap discriminator —
here a tuning knob was already at-or-past the gold standard.

## Banking (DONE — clean WIP, not committed)
- **Cleanup ✅** — removed the throwaway `CRD_MF_PROFILE`/`[FRONT]`/`[PREP]`/`CRD_MF_NARROW_SERIAL` instrumentation.
  Kept documented dev overrides `CRD_MF_PANEL` / `CRD_MF_FRONTPAR_K` / `CRD_MF_FORCE_MC64` / `CRD_MF_NO_MC64`.
- **CLI + dispatcher ✅** — `hesap.direct.lu.{f32,f64,c32,c64}` registered: multifrontal-adaptive-MC64 PRIMARY →
  GP-LU dynamic-pivot FALLBACK (+3 CLI tests: registration + f64/c64 solve). The serial CLI path is jobs-free
  (short-circuited the `jobs::num_workers()` calls), so it needs no `jobs::init` (matches the chol CLI).
- **Per-slice DoD ✅** — win-debug (MSVC `/WX` + suite **591658/57** + all 6 ctest guards), win-tidy
  (clang-tidy 20.1.8 clean), gcc -Werror (WSL re-verified), win-asan (targeted: the new parallel
  `zero_fill`/`extend_add`/TRSM + CLI dispatch + adaptive-MC64 fallback all memory-clean). **The DoD caught +
  fixed real cross-config bugs the never-run v5b-3 DoD had missed: MSVC C4996 `std::getenv` (→ `mf_getenv`
  wrapper) and two local-constexpr naming violations (`kNone`/`kNoLoc` → `no_col`/`no_loc`).** win-shipping /
  win-release / clang-cl delegated to CI (the full 18-config sweep).
