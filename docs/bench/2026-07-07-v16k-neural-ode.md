# 2026-07-07 -- v16-k (part 1): neural ODE vs torchdiffeq

**What shipped:** a NEURAL ODE trained end-to-end through the v16-f discretize-then-optimize adjoint. The RHS is a tiny
MLP `f_θ(x) = W2·tanh(W1 x + b1) + b2` (2→8→2, 42 params); training fits the flow map of a true damped-spiral ODE
`dx/dt = [[-0.1,-1],[1,-0.1]]x` by minimising `Σ_k ||ODE_θ(x0_k→T) − xT_k||²` over a batch, with the parameter gradient
by **AD through the RK4 integrator** (`dto_gradient`, v16-f) summed in a **FIXED sample order** (v16-i moat).

## Gate (`test_neural_ode.cpp`, win-debug, 46 asserts)
- Training **more than halves** the fit loss (`ll < 0.5·lf`), i.e. the neural ODE learns the dynamics.
- The whole run **replays BIT-FOR-BIT** — a second identical run yields the exact same weights (`==`) and loss (the
  deterministic-training moat on a real ML task).
- Full autodiff suite **3301 asserts / 117 cases** green (tidy-clean; the per-file `tidy-files.ps1` rule caught an
  unused `using` + `kHid`/`kDim`/`kNp` naming before close).

## ★★ CRUSH vs torchdiffeq
**Config:** Cerid g++ 13.3 `-O3 -march=native`, WSL 1T `taskset -c 4`; **torch 2.12.0+cpu + torchdiffeq 0.2.5**,
`method='rk4'`, single-thread f64. Same MLP RHS / true-spiral data / init (`0.2·sin(0.3+i)`) / batch=32 / 300 epochs /
matched effective LR. Harnesses `external/crd_v16k_neural_ode_bench.cpp` + `scripts/v16k_neural_ode_peers.py`.

★ **LOSS PARITY (the fairness gate):**

| | Cerid | torchdiffeq |
|--|--|--|
| final loss (300 epochs) | **0.0105014380** | 0.0105014357 |

Matched to ~7 significant figures — the neural ODE trains to the SAME fit as torchdiffeq (the residual is RK4
accumulation order, not a modelling difference).

★ **SPEED:**

| | train time (300 epochs) |
|--|--:|
| **Cerid (DTO adjoint)** | **428 ms** |
| torchdiffeq | 2 108 ms |

**4.9× faster** — native compiled AD-through-RK4 vs torchdiffeq's Python-per-RK-stage.

★ **DETERMINISM:** Cerid's training is bit-identical run-to-run (gated) AND, via the v16-i fixed-order fold,
bit-identical across `{1..16}` workers — a guarantee torch has none of under parallelism.

★ **Honest note:** the first peer run diverged (loss 1e11) because torch's `SGD(lr=0.05)` steps on the SUM loss while
Cerid divides the gradient by the batch size — a 32× effective-LR mismatch. Fixed by matching `lr=0.05/nb`; recorded
so the parity is on identical hyperparameters, not a lucky number.

## Verdict
- **Neural ODE via the DTO adjoint** — trains, halves the loss, bit-reproducible. ✓
- **LOSS PARITY with torchdiffeq** (7 sig figs) + **4.9× faster** + deterministic across {1..16} workers. ✓
- **v16-k part 2 (KAN)** — Kolmogorov-Arnold network on B-spline edges with the Efficient-KAN restructuring (basis
  linearity ⇒ matmul, avoiding the per-edge tensor expansion), vs efficient-kan — the remaining half of the flagship,
  next.
