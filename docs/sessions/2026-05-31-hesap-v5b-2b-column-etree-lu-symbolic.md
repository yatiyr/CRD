# 2026-05-31 — hesap v5b-2b: column-etree + supernodal-LU SYMBOLIC (the static-pivot structure)

Continues the **v5b sparse-direct LU cluster** (v5b-1 GP-LU oracle ✅, v5b-2a static-pivot front-end ✅).
v5b-2b materialises the EXACT static-pivot L/U structure UP FRONT — the symbolic phase the v5b-2c/d
supernodal numeric will fill. The next-session crush (vs Eigen SparseLU + UMFPACK) is v5b-2e.

## The load-bearing decision (advisor-corrected before any code)

I initially planned the SuperLU split: 2b = skeleton (etree + counts + supernodes), with the per-column
L/U structure discovered **on the fly during the numeric**. The advisor caught the conflation: that is
**sequential SuperLU** (dynamic pivot ⇒ structure must be found during numeric ⇒ NOT bit-identical across
thread counts). Our model is **SuperLU_DIST** (MC64 + static pivot), whose defining property is that the
full L/U structure is computable **up front** in a separate symbolic phase, leaving the numeric a
fixed-structure dataflow — the exact pattern that earned the v5a Cholesky cross-thread determinism moat.

So 2b **materialises the full static-pivot structure** (kept the slice boundary: symbolic in 2b, numeric in
2c/d). Static pivot ⇒ no interchanges ⇒ `pinv` is the identity ⇒ the structure is a deterministic pure
function of B's pattern, computable pattern-only. And we already owned the machine: v5b-1's Gilbert-Peierls
reachability (`cs_reach`/`cs_dfs`) run **pattern-only with the diagonal as the fixed pivot** IS the
static-pivot symbolic. Also pinned: chol(BᵀB) counts are the **loose all-permutations** bound (Gilbert-Ng) —
used only as oracle + reserve, NOT as the structure we factor into (here fill = flops, so a loose bound
would directly lose the crush — the v5a "fill ≠ flops" lesson does NOT transfer).

## What shipped

**`crd-hesap-ordering` (reusable column-symbolic primitives — v5c QR consumes them too).** Generalised the
faithful CSparse port to the `ata = 1` path, operating on the matrix's CSC pattern DIRECTLY (NOT
build_adjacency, which symmetrises):
- `column_elimination_tree(csc_pattern)` — `cs_etree(ata=1)`: a single column pass with a `prev[row]` array;
  AᵀA never formed. The column etree of B.
- `column_counts_ata(csc_pattern, etree)` — `cs_counts(ata=1)` + `init_ata`: transposes B once and uses the
  head/next row-merge (rows bucketed by least-postorder column); `first` is repurposed as invpost exactly
  as the reference. The chol(BᵀB) column-count = Gilbert-Ng fill upper bound.

**`crd-hesap-direct` — `lu_symbolic.hpp/.cpp` (`LuSymbolic` + `lu_symbolic(b_csc)`).** Pattern-only
reachability (the value-free twin of `sparse_lu.cpp`'s `lu_dfs`) with `pinv` = identity → per-column EXACT
L (`lp`/`li`, unit diagonal first) + U (`up`/`ui`, diagonal last), each **sorted ascending** (canonical ⇒
deterministic ⇒ panel-ready) via `crd::containers::sort`. Relaxed supernode partition over the column etree
(Liu-Ng-Peyton fundamental-supernode rule on the L colcounts). `fill_bound` = Σ chol(BᵀB) counts, used to
pre-reserve L/U (nnz(L), nnz(U) ≤ bound by Gilbert-Ng ⇒ zero growth). Exposed L/U pattern accessors on
`SparseLU` so the oracle test can read the GP-LU fill.

## Verification

**Oracle-first (advisor: tightest constraint first).** `tests/hesap-ordering/test_lu_symbolic.cpp` —
column etree + counts (ata) vs an INDEPENDENT **explicit struct(BᵀB)** oracle (forms BᵀB on small matrices,
runs the proven SYMMETRIC `elimination_tree`/`column_counts` on it). Bit-identical across 4 sizes + an
irregular wide-row stress (11 asserts / 4 cases). `tests/hesap-direct/test_supernodal_lu.cpp` (+4 cases /
29 asserts):
- **superset oracle** — every entry of the GP-LU(B, tol→0 = static/diagonal pivot) factor lies in the
  symbolic structure of its column, AND **exact nnz equality** (no cancellation on these DD matrices ⇒
  symbolic = numeric fill); the **Gilbert-Ng sandwich** (symbolic nnz ≤ chol(BᵀB) bound); complex; the
  **supernode panel-density invariant** (leading column's below-supernode pattern ⊇ each member's ⇒ dense
  trapezoid); **canonical order** (L diagonal-first ascending, U diagonal-last ascending); **run-to-run
  determinism** (pure function of B — bit-identical re-run).

**Configs:** win-debug (ordering 7771 asserts / 44 cases, direct 591487 / 41 — no regression) + **ctest**
(`-N` confirmed all 8 v5b-2b cases register distinctly — no bracket-comma fusion — and pass via ctest, not
just the binary) + ctest guards (no-std-sort / no-non-ascii / no-std-math / no-untagged) + clang-cl + gcc
`-Werror` (cross-platform parity: the integer-only symbolic is bit-identical MSVC/clang-cl/gcc — the
determinism property the moat needs) + **win-tidy** + clang-format clean. **Fixed 2 latent clang-cl
`-Wunused-lambda-capture`** in v5b-1 `test_sparse_lu.cpp` + v5a-2 `test_cli.cpp` (a `const` local captured
but used only as a constant expression ⇒ capture not odr-required) AND **latent `readability-uppercase-
literal-suffix`** (lowercase `u` int suffixes) across the v5b-1/v5b-2a WIP test files — owed-to-CI from those
slices; solved here, not deferred. **Owed before v5b phase commit:** win-asan + win-shipping (CI; near-certain
passes for a pure-integer symbolic with no UB-prone paths).

## Next — v5b-2c (supernodal LU numeric, serial)

Left-looking supernodal numeric over the `LuSymbolic` structure: BLAS-3 dense panels (reuse the Cholesky
`cmod` machinery + the L/U split), MC64-scaled values from v5b-2a, static diagonal pivot with a deterministic
√ε·‖A‖ perturbation for tiny pivots + iterative refinement (Demmel GESP) to recover the static-pivot
stability. Then v5b-2d tree-parallel + the {1,2,4,8} moat (fixed structure ⇒ deterministic by construction),
v5b-2e complex + CLI `hesap.direct.lu.*` + the crush bench vs Eigen SparseLU + UMFPACK at scale.
