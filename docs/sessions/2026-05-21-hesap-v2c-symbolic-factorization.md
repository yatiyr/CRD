# 2026-05-21 — Phase 3.1.6 `crd-hesap` v2c: full symbolic factorisation

> Module: `crd-hesap-ordering`. The v5 sparse-direct hand-off. Built on the v2a
> CSparse port (`cs_etree`/`cs_post`/`cs_counts`); adds the full L row pattern and
> the fundamental supernode partition. **CLOSED — 4-config DoD PASS.**

## What shipped

- **`postorder(etree, alloc)`** (public) — CSparse `cs_post` exposed standalone
  (was buried in `column_counts`). Children precede parents; sibling subtrees in
  ascending root index (deterministic).
- **`SymbolicFactor`** — the result struct: `parent` (etree) + `post` + `colcount`
  + **`lp`/`li`** (full L pattern, compressed CSC lower-triangular incl. diagonal,
  row indices ascending + diagonal-first per column) + **`super`/`nsuper`**
  (fundamental supernode partition). `nnz()` == `lp[n]`.
- **`symbolic_factorize(pattern, alloc)`** — the driver. One `build_adjacency`,
  shared scratch: etree → postorder → column counts (cs_counts, O(nnz(A))·α — used
  to size `lp`) → full L pattern via a faithful **`cs_ereach` row-subtree** port
  (one pass, cursor-fill into CSC, asserts each column lands exactly on `lp[j+1]`)
  → fundamental supernodes (Liu-Ng-Peyton; CHOLMOD `super_symbolic` test: column
  `j` starts a new supernode iff `parent[j-1] != j` **or** `colcount[j-1] !=
  colcount[j]+1` **or** `nchild[j] > 1`).
- **CLI** (+4 → 13 ordering commands): `postorder`, `symbolic_nnz_l`,
  `supernode_count`, `supernodes`.

## Gate — MET

**Pattern gate (the contract per the phase doc), bit-exact:** the bench does a
column-by-column L row-index diff of our `sf.li` against
`Eigen::SimplicialLLT<Lower, NaturalOrdering>` factor (NaturalOrdering → directly
comparable). **MATCH on bcsstk13 / bcsstk24 / bcsstk25.** This is strictly stronger
than v2a's `nnz` check — the actual structure, not just the count. An independent
O(n³) dense-boolean symbolic Cholesky oracle (a different algorithm) cross-checks
the small cases (tridiagonal/grids) per-column.

**Perf — symbolic analysis beats Eigen at scale:**

| matrix | n | L-pattern | nsuper | analyze ours vs Eigen | full(+Li+snode) |
|---|---|---|---|---|---|
| bcsstk13 | 2003 | MATCH | 501 | 1.65 vs 1.32 ms (0.80×) | 3.51 ms |
| bcsstk24 | 3562 | MATCH | 445 | 2.51 vs 4.44 ms (**1.77×**) | 11.97 ms |
| bcsstk25 | 15439 | MATCH | 6139 | 3.90 vs 5.82 ms (**1.49×**) | 14.51 ms |

The "analyze" column = our `nnz_l` (etree+post+counts) vs Eigen `analyzePattern`
(etree+counts+Lp) — the only apples-to-apples comparator, because **Eigen defers
the L row pattern `Li` to `factorize`** (`SimplicialCholesky_impl.h:143`), so the
full `symbolic_factorize` has no Eigen twin. Our symbolic **scales better** (2.4×
over the 7.7× n-range vs Eigen's 4.4×) → wins at every n ≥ 3562.

## Decisions / notes

- **Small-N analyze 0.80× is a constant-factor, not algorithmic** (advisor +
  scaling analysis). It crosses over by n=3562; almost certainly `build_adjacency`
  alloc/sort overhead amortised at scale. Filed `v2c-small-n-analyze-constant-factor`
  (debt.md). cs_counts O(nnz(A)) is the right choice (cheaper than a counting
  ereach pass for high-fill matrices).
- **No new D(ord) pin** — the supernode test is fully integer with no tie-breaks.
- **`column_counts` (v2a-public) still has no CLI** — deferred to the v2e
  CLI-completeness audit (noted here so the audit picks it up).

## Traps hit

- **Catch2 bracket-comma gotcha** ([[feedback_catch_discover_tests_bracket_comma]]):
  a test named `"...covers [0,n) and..."` has a `[…,…)` substring →
  `catch_discover_tests` fused 5 cases into one giant CTest entry. Fingerprint:
  win-tidy PASS + runtime configs CTEST-FAIL exit=8 (and the over-long fused name
  blew the win-asan ctest command line → false "input line too long"). Renamed to
  drop the brackets; all green.
- **Don't pre-source vcvars before `per-slice-check.ps1`** — the script
  self-sources vcvars per config inheriting `%PATH%`; the win-asan branch prepends
  the ASan DLL dir on top, and a pre-bloated `PATH` overflows cmd's 8191-char
  limit ("input line is too long", reported as BUILD-FAIL). Run the script in a
  clean session. New memory: [[feedback_per_slice_check_no_pre_vcvars]].
- **Tridiagonal nsuper = n-1, not n** (my test expectation was wrong, code was
  right): a path coalesces its last two columns into a fundamental supernode
  (column n-2 is the only child of n-1 and colcount[n-2] == colcount[n-1]+1).

## Verification

- `crd-hesap-ordering-tests`: 17 cases / 2412 assertions PASS (+10 v2c cases).
- 4-config per-slice DoD (win-debug + win-asan + win-shipping + win-tidy): **PASS.**
- Bench `bench_hesap_ordering_vs_reference` (CRD_BUILD_HESAP_VS_REFERENCE=ON,
  win-release): pattern MATCH + perf table above. Flag reset OFF after.
- Pre-existing intermittent `crd-perf` #297 ("nested CRD_PERF_SCOPE inside a job")
  flaked once under the first parallel run, passed 8/8 in isolation + did not
  recur — load-dependent, orthogonal to v2c (ordering touches neither perf nor
  jobs).

## Next

**v2d** — multilevel-METIS ND scaffold (heavy-edge-matching coarsening + initial
partition + uncoarsening framework, no refinement yet) → v2e (FM refinement + ND
driver + v2-close, locks ADR-0065 §16) → v3 (SVD/eig).
