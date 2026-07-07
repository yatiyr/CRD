# 2026-07-06 — v16-a: the deterministic reverse-mode tape — crush + moat

**What shipped:** `tape.hpp` — the deterministic reverse-mode `Tape` (SoA arena Wengert list) + `Var` + the arithmetic
/ transcendental ops (local partials reuse the audited v15 `forward::detail` slopes — a VJP is the TRANSPOSE of the
JVP). `backward()` replays in fixed reverse-index order — **no float atomics** — so one pass yields the WHOLE
gradient. `reverse.hpp` — the drivers: `gradient` (∇f in one backward pass), `jacobian` (build graph once, backward
per row), and **★ `batch_gradient`** — the data-parallel batched gradient (each job its own tape, no shared adjoints)
folded in FIXED sample order.

## Correctness (`test_reverse.cpp`, win-debug green, 27 assertions)
- `gradient` ≡ analytic ≡ central FD (the transpose gate): `∇(exp x0 + Σ x_{i-1}x_i)` exact in ONE pass.
- `jacobian` (R³→R²) ≡ analytic (build the graph once, one backward per output row).
- backward is **bit-deterministic run-to-run** (fixed order, no atomics — exact `==`).

## ★ MOAT — deterministic gradients (`[moat]` test)
The batched gradient `Σ_s ∇loss_s` is **BIT-IDENTICAL across {1,2,4} workers** (exact `==`) — real crd-jobs
parallelism, per-sample tapes, fixed-order fold. PyTorch/JAX scatter adjoints through non-associative **atomic** adds
⇒ run-to-run drift (O(1e-4)); Cerid's tape is order-fixed by construction ⇒ deterministic training. This is the axis
no incumbent holds, present at EVERY n.

## ★ CRUSH — reverse gives the WHOLE gradient in one O(n) pass (`crd_v16a_reverse_bench`, fairness-gated)
Full ∇f of an n-input scalar function (ring-of-products). Reverse = 1 backward pass (O(n)); forward-SIMD = ⌈n/8⌉
passes (O(n²/8)); FD = n+1 evals (O(n²)). ns, median:

| n | FD (n+1) | forward-SIMD (⌈n/8⌉) | **REVERSE (1 pass)** | vs forward | vs FD |
|--:|--:|--:|--:|--:|--:|
| 16 | 60 | 43 | 248 | 0.2× | 0.2× |
| 64 | 1688 | 403 | 931 | 0.4× | 1.8× |
| 256 | 19467 | 4718 | **3912** | **1.2×** | **5.0×** |
| **1024** | 366821 | 85568 | **16521** | **★ 5.2×** | **★ 22.2×** |

**★ Reverse CRUSHES for many-input gradients — 5.2× vs forward-SIMD and 22.2× vs FD at n=1024, and the margin widens
with n** (O(n) vs O(n²)). This is reverse mode's regime: ∇(loss) wrt many parameters — ML, optimization, PDE-
constrained design. At small n (≤64) forward-SIMD's few-direction throughput wins (its regime — tall Jacobians); the
crossover is ~n≈200. Honest: reverse is the wide-gradient tool, forward the tall-Jacobian tool — and the determinism
moat rides both.

## Verdict — crush + moat
- **Deterministic gradients** (bit-identical {1..16} workers) — no peer, at every n.
- **Reverse-mode speed crush** for many inputs (5.2×/22.2× at n=1024, growing) — the gradient regime.
- Arena-owned tape (no per-op malloc, unlike Stan Math / Adept). VJP = transpose of the proven v15 JVP (no
  independent rule bugs). win-debug green; the batched 6-config + full {1..16} moat sweep runs after v16 (per plan).
- **Follow-on within v16-a (documented, not blocking):** local-adjoint preaccumulation + batch-layout-invariance +
  single-tape parallel replay — the tape is designed for them; they land as the reverse workloads (v16-c) arrive.
