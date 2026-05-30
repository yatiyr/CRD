# 2026-05-31 — hesap v5a-7: within-front parallelism, sub-slice 1 (cmod row-slab)

## Context

This session opened by **closing the hand-tuned-asm investigation (ADR-0088) as REVERTED** — the
dual-syntax MASM/GAS f64 6×8 kernel + runtime CPUID dispatch were built end-to-end and bit-identical
to the intrinsic, but a same-process A/B measured the asm **~1–2% SLOWER** (the intrinsic inlines into
`gemm_packed_inner` at ~98% peak; hand-asm is a hard call that can't inline across TUs). ADR-0082
(intrinsics-first) stands, vindicated. Tree reverted to committed v5a-5. Then the user chose to pursue
the **bmwcra scaling lever**, which this session investigated, validated, and began.

## The bmwcra scaling investigation (measure-first, anti-fabrication)

All numbers below are file-captured from WSL `bench_hesap_cholesky_vs_cholmod` (CRD_BUILD_HESAP_VS_CHOLMOD,
matched OpenBLAS, same AMD-permuted matrix), not inline-channel.

1. **Premise re-checked at HEAD (refuted the stale "2.01× plateau" framing).** bmwcra FACTOR gap
   **WIDENS** with threads: 1T 0.77× → 8T 0.65× (CHOLMOD self-scales 1.87× vs Cerid 1.59×). So scaling
   IS a real lever — the earlier "lever-3 already fixed it / gap narrows" guess was mixing cross-state
   numbers.

2. **Scale-profiler decomposition (8T).** The loss is **concentrated ~70% in ~23 huge near-root fronts**
   (nc up to 2406), not spread out:
   - SETUP (serial) 207ms / 9% · tree 297ms / 13% · **node (huge fronts) 1640ms / 70%** · starved 207ms / 9%.
   - Within the huge fronts: **cmod scales 1.86×, cdiv only 1.20×** (the serial diagonal chain), scatter ~0.

3. **ob_probe — DECISIVE, NOT a bandwidth wall.** OpenBLAS-threaded dgemm on Cerid's *exact* cmod/cdiv
   shapes scales **4.5–6.8×** (cmod M2048/N512/K200 = 4.58×; M1024 = 6.70×; dtrsm/dgemm cdiv 6.4–6.8×)
   where Cerid in-situ gets 1.86×/1.20×. ⇒ the shapes parallelize fine; Cerid's poor scaling is a
   **fork/join + scheduling deficiency** (the front fired many small per-descendant `gemm_parallel`
   calls, each forking the pool; OpenBLAS does one efficient threaded call). FIXABLE.

4. **Decision.** User chose **GO** on a multi-session within-front-parallelism rewrite. Honest ceiling:
   parity-to-~1.1× on bmwcra (the per-thread gemm is ~0.7× OpenBLAS = the ADR-0082 intrinsic ceiling,
   caps the upside), but it is a **general within-front scaling lever for ALL large factors** + the
   substrate strength eylem needs. bmwcra is the worst-case validator, not the point.

## Sub-slice 1 SHIPPED — cmod across-descendant ROW-SLAB parallelism (one fork per front)

`supernodal_cholesky.cpp` `factor_one`: the node-parallel huge-front cmod no longer fires
`gemm_parallel` per descendant. Instead a single `parallel_for` partitions the front's **rows** across
the pool (`cmod_slab(r0,r1,w)`); each lane, for every descendant, does the sub-gemm over the
**contiguous** pr-range mapping into its row-slab (binary-searched on the descendant's sorted `srow`
against the slab's global-row window) and scatters into its own disjoint rows.

- **Row partition (the M dimension), not column** — advisor-corrected. Row-split partitions the *bigger*
  operand `am` (no redundant traffic), shares the small `am1`, and is exactly the M-partition OpenBLAS
  used to hit 4.58×. Column-split would re-read `am` W× (a bandwidth multiplier) and run a shape
  OpenBLAS never measured. Key fact: `lrm[pr]=rr[srow[..]]` is **strictly increasing** (rr is
  position-in-sorted-front-rows, the descendant's rows are a sorted subset) → not a scatter →
  contiguous pr-range per descendant.
- **Bit-identical to serial by construction:** each front row owned by exactly one lane; its subtracts
  applied in ascending-descendant order = the serial order; each gemm output element's K-reduction is
  independent of the row-slab (the property `gemm_parallel` already relied on). The serial/tree path
  (`par_workers==1`) goes through the same `cmod_slab(0,nr,worker)` = one full-front slab.

### Results (file-captured WSL, 8T, matched OpenBLAS; two runs averaged for ratios)

| matrix | FACTOR before | FACTOR after | note |
|---|---|---|---|
| **bmwcra** (148k) | 0.65× (8T ~2256ms) | **~0.86× (8T ~1630ms)** | the one loss, big jump; not yet parity |
| **hood** (220k) | 1.33× WIN | **1.47–1.49× WIN** | winner improved (+0.15) |
| **ldoor** (952k) | 1.28× WIN | **1.46× WIN** | winner improved (+0.18) |
| **bcsstk25** (15k) | 1.41× WIN | **1.70× WIN** | winner improved (+0.29) |

- **Denominator-independent headline: bmwcra factor self-scaling 1.59× → 2.19×** (1T 3578.7ms → 8T
  ~1630ms; −28% at 8T). Direct proof the cmod parallelism engaged (cmod is 70% of the front).
- **No regression anywhere — every prior winner got faster** (they have huge fronts too, so the
  row-slab cmod lifts them). This over-delivery on hood/ldoor is the strongest evidence that
  within-front parallelism is the right multi-session investment, independent of where bmwcra lands.
- **Determinism moat GREEN:** 591348 assertions / 22 cases bit-identical at {1,2,4,8} workers
  (`[v5a-4][determinism]` fat-front test factors `dense_spd(600)` nc≥512 → exercises the new
  node-parallel `cmod_slab`); residuals unchanged. win-debug only this session (gcc/clang-cl/asan/tidy
  + the cluster close-gate owed before commit).

## Sub-slice 2 SHIPPED — cdiv targeted serialization (the chain inflation fix)

The cmod/cdiv split probe (re-applied + reverted manually to protect the uncommitted cmod_slab keeper)
showed: **cmod now scales 4.46×** (1540→345ms; confirms sub-slice 1 hit OpenBLAS's ~4.5×). But **cdiv
only 1.29×**, and the split exposed *why*: cdiv = trail (parallel, 436→140ms = 3.11×) + **chain (serial
A-work) that INFLATED 223→375ms at 8T** — the chain didn't just fail to scale, it got *slower* under
threads. Cause: the chain fired many small per-block `gemm_parallel`/`parallel_for` calls (A2/A3
within-obw gemms ≤192×64, and the B1 apply over jbw≤64 cols) — fork/join overhead that costs more than
it saves at 8T (the same disease the cmod had).

Fix (targeted, not the shared `kGemmParallelMinFlop` knob): force **serial** on exactly the
bounded-small ops — A2 below-solve gemm, A3 within-obw trailing gemm (both pass `par_workers=1`), and
the `sub_col_b1` apply (always serial). The large B1/B2 gemms and the C outer-trail (the 3.11× scaler)
stay parallel. Bit-identical by construction (serial≡parallel gemm; column-disjoint apply).

**Result:** chain 375→**225ms** at 8T (recovered the ~150ms fork/join inflation, exact attribution);
cdiv 510→~360ms; **bmwcra 8T 1631→1453ms**.

## Sub-slice 3 SHIPPED — cdiv B-block ROW-SLAB (refactor #2; the bmwcra WIN)

A1-floor probe (the refactor-#2 gate) split the cdiv chain: **a1 (genuinely-serial POTF2) = 2.6ms —
negligible**; the ~220ms "rest" is the B below-outer work over `below_o` rows, fully parallelizable but
not scaling (the per-jb-block forks + sub-slice-2's wrong-axis serial apply). With a1≈0 the green-light
was unambiguous. Fix = the cmod row-slab pattern applied to B: **one `parallel_for` per outer block over
the `below_o` rows** (replacing the per-jb-block forks); each lane owns a row-slab and walks the
jb-blocks **sequentially** (B1 K-growing update → B2 jbw×jbw diagonal solve, ascending = the serial
accumulation order ⇒ bit-identical); per-lane `ub_w`/`linv_w`, shared read-only L11, redundant 64×64
diagonal invert per lane (noise). **Result: `rest` 220→~100ms@8T (2.2×); cdiv ~360→~275ms; bmwcra 8T
→ ~1320–1437ms.**

## v5a-7 FINAL (sub-slices 1+2+3, clean WSL, 8T, matched OpenBLAS, 2 clean runs)

| matrix | baseline 8T | **v5a-7 final 8T** |
|---|---|---|
| **bmwcra** (148k) | **0.65× LOSE** | **1.04–1.05× WIN** (both clean runs: 1318/1379, 1437/1489) |
| **hood** (220k) | 1.33× WIN | **1.48× WIN** |
| **ldoor** (952k) | 1.28× WIN | **1.44–1.76× WIN** |
| **bcsstk25** (15k) | 1.41× WIN | **1.75× WIN** |

- **EVERY matrix now beats CHOLMOD** — bmwcra, the lone loss, crossed 0.65→**~1.045× (both clean runs
  ≥1.04×, the advisor's win bar)**, *with the cross-thread bit-determinism moat none of the gold-standard
  peers carry.* bmwcra factor self-scaling 1.59× → ~2.4×.
- Honest scale: bmwcra's win is **modest (~4–5%)**, run-to-run absolute time varies ~9% (turbo/load) but
  the *ratio* is stable 1.04–1.05× because the CHOLMOD denominator co-varies. It's a genuine win, not a
  blowout — the per-thread intrinsic-gemm ceiling (ADR-0082) keeps it modest; the parallel scaling is
  what flipped it.
- **Determinism moat GREEN across all three sub-slices** (591348 assertions / 22 cases bit-identical at
  {1,2,4,8}); residuals unchanged.

## Load-imbalance polish — ATTEMPTED, NULL on the real workload, REVERTED.

The `rest` scaled only **2.2× (not the ~4.5× of cmod)** — row-slab **load imbalance** (near-root fronts
concentrate row mass; the cdiv does ~10 sequential forks/front vs cmod's 1). Tried the cheap bit-identical
knob: `num_jobs = par_workers*4` (over-decompose so the scheduler load-balances), on both row-slabs.
- **Isolated bmwcra-only:** clearly helped (1.13/1.06 vs ×8's 1.05/1.04).
- **Representative all-matrix run:** NULL — Cerid's own time barely moved (1324–1354 vs ×8's 1318–1437);
  the apparent ratio shifts were CHOLMOD-denominator noise. The bmwcra-only gain was the cold-start
  favorable-context confound (the same kind this session killed for the asm clock + fast-CHOLMOD lines).
- **Verdict (advisor-concurred): REVERTED both ×4 edits.** A magic constant with no measured benefit on
  the real workload is debt; bmwcra already wins from the three real sub-slices. The genuine residual fix
  is **guided/work-stealing chunking** (or per-front adaptive `num_jobs`) — a **characterized future
  lever**, plus starved-level overlap (~9%) + setup trim (~9%). Not chased (bounded-importance, already
  winning).

## STOP — banked.

v5a-7 is complete: the one losing matrix (bmwcra) now wins, every other factor improved, moat held across
all three sub-slices. Shipped state = three sub-slices at `par_workers` (no over-decompose).

## Verification owed before the phase-boundary commit

win-debug (both modules) + gcc -Werror (WSL bench compiled crd-hesap-direct clean) GREEN; determinism
moat + residuals GREEN. **Still owed (CI / phase close):** clang-cl, win-asan, win-tidy, win-shipping +
the ctest guards. ⚠ dev landmine: `supernodal_cholesky.cpp` has a dormant `-Werror=conversion`
(`double / g_cholprof.cmod_calls`) that bites only when CHOL_PROFILE is enabled under gcc.

No commit (user commits at phase boundaries). Tree: `supernodal_cholesky.cpp` (cmod row-slab + cdiv
serialization, verified) + the doc set; `build/ob_probe*.cpp` are gitignored throwaways.
