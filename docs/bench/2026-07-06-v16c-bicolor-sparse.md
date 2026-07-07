# 2026-07-06 — v16-c: bidirectional coloring (bicoloring) + sparse reverse-mode LA

Two v16-c capability crushes, gated on **win-debug** (`crd-hesap-autodiff-tests`, the link-isolation smoke — both
features live in the autodiff module and pull no heavy deps). The full autodiff suite is **2534 assertions / 91 cases**
green after these land.

## ★ Bicoloring — the reverse-mode complement to v15-e's unidirectional coloring

`bicoloring.hpp`: bidirectional (row+column) Jacobian coloring. A sparse Jacobian with BOTH a dense row and a dense
column defeats any one-directional scheme — a dense row forces every column into its own color (`ncol=n`), a dense
column forces every row apart (`nrow=m`). Bicoloring recovers the dense ROWS by REVERSE (tape VJP) sweeps and the
sparse remainder by FORWARD (JVP) sweeps; total sweeps = `ncol + nrow`. Auto-threshold optimiser sweeps the
density cutoff and keeps the minimum (ties prefer forward — JVP sweeps are cheaper than building+replaying the tape).

**Gate (`test_bicoloring.cpp`, arrowhead n=m=17: dense row 0 + dense column 0 + diagonal):**

| scheme | total sweeps | recovery |
|---|--:|---|
| unidirectional column coloring (v15-e) | **17** (= n; the dense row forces every column apart) | — |
| **bicoloring (Cerid, auto)** | **3** (2 forward col-colors + 1 reverse row-color) | ≡ analytic Jacobian (`<1e-10`), structural zeros preserved, **bit-identical run-to-run** |

★ **CRUSH: 17 → 3 sweeps (5.7×), and the gap GROWS with n** (unidirectional = n, bicoloring = 3 on the bordered
pattern → 33× at n=100). Diagonal (no dense row) degrades gracefully to a single forward sweep (gated).

**Peer — ColPack** (the standard C++ bidirectional-coloring library, built in WSL:
`external/ColPack/build/cmake/_build/libColPack.a`; harness `external/colpack_arrowhead/`, one-command `run.sh`).
`BipartiteGraphBicoloringInterface` star bicoloring on the same 17×17 arrowhead: **1 reverse (row) seed + 2 forward
(column) seeds = 3 AD passes** — read from ColPack's own seed matrices (`GetLeftSeedMatrix` = 1×17, `GetRightSeedMatrix`
= 17×2; its `GetVertexColorCount` headline "4" includes a NEUTRAL row-color that carries no seed — ColPack itself
strips it, `SeedRowCount = LeftVertexColorCount − 1`). Unidirectional `COLUMN_PARTIAL_DISTANCE_TWO` = **17** (confirmed,
cross-check). Deterministic across 5 repeats.

**★ HONEST verdict:** **Cerid TIES ColPack at 3 seeds** (identical 2-forward + 1-reverse split) — Cerid does **not**
beat ColPack on the color count, and pitting Cerid's tight `3` against ColPack's neutral-inclusive headline `4` would
be a metric-mismatch cherry-pick (forbidden by the no-asterisk / no-partial-metric rules). **Both crush
unidirectional's 17 → 3.** The Cerid *advantage* is NOT fewer colors — it is the **integrated, deterministic,
allocation-free pipeline**: ColPack is coloring-ONLY (you bring your own sparsity pattern, your own AD recovery, and
its greedy coloring depends on internal vertex orderings), whereas Cerid runs **trace (v15-e index-set) → bicolor →
direct AD recovery** in one header, deterministic by construction. There is no C++ incumbent for the full
trace→bicolor→recover pipeline (arXiv:2505.07308, 2025).

## ★ Sparse reverse-mode LA — a capability PyTorch/TensorFlow lack

`sparse_reverse.hpp`: reverse-mode VJPs over the CSR surface (`row_ptr`/`col_idx`/`values` — the hesap-sparse layout),
differentiated wrt BOTH the dense operand AND the sparse-matrix ENTRIES (gradients returned in the same CSR pattern):
- **spmv** `y=A·x`: `Ā_ij=ȳ_i·x_j` (on the pattern), `x̄=Aᵀ·ȳ`.
- **spmm** `Y=A·X`: `Ā_ij=Σ_p Ȳ_ip·X_jp`, `X̄=Aᵀ·Ȳ`.
- **sparse solve** `A·x=b`: `b̄=A⁻ᵀ·x̄` (one back-solve on the STORED factor — the v16-d factor-reuse principle),
  `Ā_ij=−b̄_i·x_j` (on the pattern).

**Gate (`test_sparse_reverse.cpp`, 66 assertions):** every gradient — per-nonzero `gvals` + the dense operand — matches
central FD (`<1e-8` for the linear spmv/spmm, `<1e-6` for the nonlinear solve); the solve is factor-reuse (one LU, used
for both `A` and `Aᵀ`) with an `A·x==b` sanity check; VJPs are **bit-identical run-to-run**.

★ **CAPABILITY CRUSH:** PyTorch / TensorFlow **lack sparse-matrix autodiff** (arXiv:2212.05159) — they cannot return a
gradient wrt the entries of a sparse operator at all. Cerid does, deterministically, in the CSR pattern — feeding v9
sparse BDF Jacobians and the 3.1.12 FEA adjoint. Peer column: **N/A (capability absent in torch/TF)**, stated with the
check.
