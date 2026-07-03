# 2026-07-03 — v14-e einsum path optimizer vs opt_einsum 3.4.0

- **Oracle protocol (python-verified BEFORE the port):** 33-case corpus (`scripts/v14e_einsum_corpus.py`
  → `tests/hesap-tensor/ref_einsum_paths.inc`): matrix chains (2–7 mats), the tensor-network classics,
  20 seeded random networks (3–7 operands), each with opt_einsum's greedy and optimal path FLOPs.
- **Finding (verified in `build/crd_einsum_diag2.py`):** opt_einsum's `optimal` SEARCH minimizes an
  internally different objective (`inner = shared-removed`, paths.py `calc_k12_flops`) than its
  REPORTED `opt_cost` (`inner = any-removed`). Cerid's branch-and-bound minimizes the reported metric
  directly — so it produces paths that genuinely beat opt_einsum's "optimal" under opt_einsum's own
  accounting (e.g. 24,384 vs 25,728 FLOPs on a corpus network).

## The gates (all green; 263 asserts, debug+asan+tidy+gcc)

| Gate | Result |
|---|---|
| Cerid optimal vs opt_einsum optimal (reported metric) | **≤ on every case; strictly better where their search inconsistency bites** |
| Cerid auto/greedy vs opt_einsum greedy | **≤ on every case** (auto routes n≤7 to exact search — opt_einsum's own preset shape; multi-heuristic greedy beyond) |
| Parser | NumPy semantics: implicit-output occurrence rule ("ii"→trace), ellipsis (incl. implicit "...(batch)" output), diagonals flagged, status adversaries |

## Planning cost (the build-once story; same machine, 1T pinned, corpus mean)

| Planner | µs/plan | vs Cerid |
|---|---|---|
| **Cerid `einsum_plan_build`** (parse + exact ≤7 / multi-greedy) | **8.15** | — |
| opt_einsum greedy `contract_path` | 72.96 | **9.0×** |
| opt_einsum optimal `contract_path` | 336.81 | **41×** |

And the `EinsumPlan` is built once and reused across executions (allocation-free, 34-bit index masks,
WCET-bounded), where NumPy re-plans every `einsum` call. Execution (TTGT over the own GEMM) = v14-f.
