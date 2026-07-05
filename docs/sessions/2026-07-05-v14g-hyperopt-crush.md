# 2026-07-05 — v14-g: the cotengra-class hyper-optimizer — oracle → C++ → FULL CRUSH

> Boards + protocol: **`docs/bench/2026-07-05-v14g-hyperopt-oracle.md`**. This log = the arc +
> verification state + the two root-caused bugs. (The same day also closed the v10 FFT shipping
> gate — C1002 root fix — recorded in the 2026-07-04 session log's addendum.)

## The arc

1. **Reconstruct-verify-first (the mandated protocol):** cotengra 0.8.2's source staged +
   extracted (three parallel readers: driver/scoring/tree-surgery · greedy internals ·
   partition/SA/slicer — exact formulas with line refs). Python reconstruction
   (`scripts/v14g_hyperopt_recon{,2}.py`): cost model **bit-matches cotengra 6/6**, T=0 greedy
   **identical quality 6/6**, random-greedy statistically at par (16 seeds), SliceFinder at
   **matched-tree parity** (identical slice sets), full-pipeline quality **5W/1T/0L** vs their
   hq default — the rand200 loss closed by wiring treesa SA finalists (SANITY #9: measured
   lever, not acceptance).
2. **The C++ port** (`engine/hesap-tensor/include/crd/hesap/tensor/hyperopt.hpp`, ~2900 lines):
   `HyperNet` (pooled-leg SSA hypergraph, compacting edge slabs, saturating u64 costs) ·
   greedy engine (Boltzmann-Gumbel, batch-index guard, 2^14 heap prune, incumbent abort) ·
   `HyperTree` + subtree-reconfigure (exact subset-DP re-solve) · treesa SA (4 rotations,
   Metropolis-in-log) · labels-divide partition trees (own label-propagation — NO kahypar
   dependency) · SliceFinder (integer-exact, **the memory bound honored EXACTLY,
   NotFound-never-best-effort**) · `hyper_optimize` (stratified Philox-keyed trials, per-trial
   reconf, SA finalists; trials = the crd-jobs parallel unit). All stochastic draws keyed
   (seed, stream) ⇒ **bit-identical at any worker count** — gated `{1,2,4,8,16}`.
3. **The corpus gate** (`tests/hesap-tensor/ref_hyperopt.inc`, frozen by
   `scripts/v14g_export_corpus.py`): hyper_optimize @64 trials must land log10(flops)
   at-or-under cotengra greedy+kahypar's frozen results — **6/6, ctest-enforced**.
4. **Boards (fixed artifact, pinned):** driver **2.2–6.1× faster than cotengra's full stack
   at matched budgets while producing better trees on every network** (rand200: 2.60 s vs
   15.83 s, log10 12.21 vs 12.97); core engine **1.07–2.2× faster than cotengrust (RUST)**;
   peers installed fresh (cotengrust via pip) — the fastest available implementations, not
   just python.

## Two bugs root-caused in-session (both caught by the discipline, not by luck)

- **Pool-reallocation UAF in `HyperNet::merge_legs`** — pushed into the pool its own input
  spans pointed into; a mid-merge growth read freed memory. Caught by an EXACT-VALUE test
  assertion (boundary-adversary in new code); fixed by reserve-before-spans in `contract()`.
- **`HyperTree` borrowed-lifetime UAF** — the tree stored `const HyperNet*` for index
  metadata; the driver's finalist path built trees from a lambda-local net ⇒ every later
  `stats()`/`node_flops()` read freed memory. **gcc -O3 was SILENT (heap reuse), MSVC-debug
  SEGV'd at a distant destructor, ASan converted it to a precise OOB assert** — localized
  with rule-#10 flushed markers, then ASan (rule #4: match the tool). Root fix: the tree
  OWNS copies of sizes/appearances — no borrowed lifetime in a returned object, ever.
  ⚠ Rule #2 aftermath: the pre-fix bench board contained garbage-derived values (some
  looked BETTER); re-measured on the fixed artifact and corrected in the bench doc.

## Verification at close (the fixed artifact, every config)

- linux-gcc-release: **802 asserts / 11 cases** (incl. corpus gate + `{1..16}` moat).
- win-debug: **ctest 11/11** (TEST_CASE names ASCII-ized — the em-dashes broke ctest
  registration, the exact scar the `crd-no-non-ascii-test-names` guard encodes).
- win-asan: **802/11, zero ASan errors**.
- win-shipping (LTCG): **ctest 11/11** — the header is force-inline-free by design (the
  C1002 lesson from the same morning applied at design time).
- win-tidy: **GREEN for BOTH the hyperopt AND fft targets** (one real finding fixed —
  a missing [[nodiscard]]; the rest were the KNOWN transient clang-tidy AVs, cleared on
  retry per the recorded scar — fft.cpp needed two). ⇒ the v10 FFT tidy gate, earlier
  delegated to CI, is CLOSED LOCALLY too: both slices carry the full 5-config ladder.

## Handoff

- v14-g core is SHIPPED + crushing (phase row updated). The >16-operand einsum front-end
  dispatch bridge rides the v14-z integration row with the CLI per the v14 plan structure.
- Commit proposal delivered in-chat (v10 FFT close + v14-g together; user commits).
