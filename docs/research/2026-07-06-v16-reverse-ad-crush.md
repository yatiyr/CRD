# v16 — reverse-mode AD + differentiable solvers: the crush reference (2026-07-06)

> **Implementation reference — read before coding any v16 slice.** The frontier crush levers per slice, the honesty
> caveats, the ready-to-use rule/peer tables, and the NEW 2025–26 parts folded in from the kickoff research. Sibling
> to the v15 dossier `2026-07-06-v15-forward-ad-crush.md`. Governed by ADR-0097 (the one autodiff module, forward +
> reverse). North star: `project_hesap_is_universal_foundation_zero_defect` — correct + full-crush + no-scars +
> deterministic + agent-drivable, none traded. Crush playbook: `docs/hints/crush-playbook.md`.

## 0. The moat — what v16 has that the incumbents structurally lack

1. **Bit-identical `{1..16}`-worker AND batch-invariant gradients — deterministic training.** The reverse pass's
   scatter-add is the crux: PyTorch/JAX accumulate adjoints through non-associative atomic adds ⇒ run-to-run gradient
   drift (measured O(1e-4)). The 2025 literature made the diagnosis precise: non-determinism is dominated by the
   **batch-size dependence of reduction kernels**, and **batch-invariant** kernels give bit-identical outputs over
   1000 runs ("Defeating Nondeterminism in LLM Inference", Thinking Machines 2025-09; DASH arXiv:2601.21824;
   bit-identical medical DL arXiv:2603.28040; "reproducibility is the new copyleft" arXiv:2606.03019). Cerid's tape
   is **fixed-order accumulation, no atomics, batch-invariant reductions** by construction ⇒ deterministic training,
   a world-first for a C++ engine and exactly what the verifiable-training literature is asking for. THE cluster
   differentiator; gated per op class (`[moat]` asserts convergence too — the vacuous-max guard).
2. **Suite-wide reverse differentiability.** torch/JAX differentiate tensors; Cerid reverses the whole hesap surface:
   matrix-calculus VJPs (solve/chol/LU/SVD/eig via factor-reuse, never AD-through-LU), FFT VJP = IFFT (exact), DSP,
   v13 spline eval — a loss flows back through a linear solve or an FFT the same way it flows through `sin`.
3. **A C++ implicit-differentiation lane with NO living peer.** jaxopt is unmaintained; cvxpylayers/Theseus are
   Python/GPU-first. Cerid owns argmin-via-KKT / Newton / fixed-point / linear-solve VJPs in deterministic C++.
4. **tape→C++ codegen (v16-h)** emits `.crds.cpp` hot-reload cells (ADR-0081) — JIT-class differentiated hot loops
   with zero new infrastructure; the source-transform lane (Enzyme-class) without the LLVM-plugin/MSVC portability
   loss (Enzyme named OUT, ADR-0097 Decision 6).
5. **The v13 certification pillars applied to reverse mode** — allocation-free replay path (arena tape), status-not-
   exception, bounded passes (WCET-analyzable checkpoint schedules). torch/JAX structurally lose this axis.

## A. v16-a — the deterministic tape substrate

- **SoA tape**: operator records (opcode, operand tape-indices, saved-for-backward payloads) in an arena via
  `IAllocator`; adjoints in a parallel array. Saved intermediates are **owned** (`Tensor<T>`/value copies or an arena),
  NOT borrowed `TensorView`s — backward runs after forward's frames unwind (ADR-0096 §Consequences).
- **Deterministic accumulation (the moat)**: the scatter (each value's adjoint gets contributions from every consumer)
  is inverted to a **gather** in tape-index order; parallel execution partitions the tape into deterministic index
  ranges (crd-jobs deterministic `parallel_for`, block order = f(tape length) only, never `num_workers`), cross-
  partition merges via a fixed-order reduction tree (v14 Tier-D lifted to adjoints). **NO float atomics anywhere.**
  Where an op's adjoint is a genuine scatter (embedding/segment), the deterministic form is **sort + segmented-reduce**.
- **★ NEW (2025): batch-invariant reductions.** Make every adjoint reduction bit-identical regardless of batch
  position/size (the Sep-2025 diagnosis) — a fixed reduction split independent of the batch dimension. Gate: the
  `[moat]` test perturbs batch layout and asserts bit-identity, not just worker-count.
- **★ NEW: local-adjoint preaccumulation** (CoDiPack, arXiv:2405.07819): preaccumulate the local Jacobian of a
  side-effect-free subexpression (statement-level) so the tape stores one dense block instead of many records —
  shrinks the tape + speeds replay. Ship as an opt-in on hot statements; deterministic (fixed local order).
- **checkpoint hooks** (the v16-f revolve seam) + thread-parallel replay with the deterministic partition.
- Peers: Stan Math (`arena_allocator`), CoDiPack (Jacobian/primal taping + preaccumulation), Adept (expression
  templates), FastAD (arXiv:2102.03681), Enzyme (OUT — LLVM plugin). **Crush = determinism they can't hold + arena
  no-malloc replay + preaccumulated tape size.**

## B. v16-b — scalar VJP rule library

- **A VJP is the transpose of the JVP** — reuse the v15-b `detail::jvp_rules` slopes verbatim; the reverse rule is
  `x̄ += slope · ȳ`. One rule library, two modes. Control flow: the tape records the *taken* branch (values, not
  predicates), so `if/min/max/select`/loops replay along the executed path (the standard reverse-AD contract).
- **3-oracle gate**: `reverse-VJP ≡ v15-forward-JVP (transpose identity, exact) ≡ complex-step (~1e-15) ≡ FD (~1e-6)`.
  Every rule ships only when all agree; verify in python/JAX first.
- Peers: Stan Math, Adept, CoDiPack, autograd. Crush = determinism + alloc-free + the transpose-of-a-proven-JVP
  correctness (no independent rule bugs).

## C. v16-c — tensor / einsum / NN VJPs over v14 (makes v14-m TRAINABLE)

- **einsum VJP = einsum with a permuted spec**, riding the v14 `EinsumPlan` (the contraction path is reused; the VJP
  wrt operand k is `einsum(spec with k's indices as output, the other operands + ȳ)`). broadcast↔reduce are transposes;
  conv/pool/layernorm/softmax/GELU grads. The v14-m op set becomes trainable.
- **★ NEW: bicoloring** (arXiv:2505.07308, 2025) — for a sparse Jacobian, color rows AND columns jointly (combine one
  reverse pass over row-groups with one forward pass over column-groups) → strictly fewer evaluations than the v15-e
  unidirectional column coloring. This is the reverse-mode complement to v15-e's tracer; **the 2025 frontier, no C++
  incumbent.** Reuse the v15-e index-set tracer for the pattern; add the bidirectional coloring + recovery.
- **★ NEW: sparse reverse-mode LA** (arXiv:2212.05159) — VJPs over the hesap-sparse CSR surface (spmv/spmm/sparse
  solve). torch/TF **lack sparse-matrix autodiff** — a clean capability crush feeding v9 sparse Jacobians + 3.1.12 FEA.
- Gate: **torch-CPU value+grad parity** on the v14-m corpus (MLP/conv/attention blocks), matched threads.

## D. v16-d — matrix-calculus + suite VJPs

- **Giles/Seeger VJP table** = the transpose of the v15-f JVPs (differentiate the SOLUTION, reuse the stored factor —
  never AD-through-LU): `solve` (`Ā = −A⁻ᵀ x̄ xᵀ`, `b̄ = A⁻ᵀ x̄`, one back-solve on the stored factor) · `chol` · `LU` ·
  `SVD`/`eig` (adjoint via the F-matrix; ship the value-only degeneracy-robust path where JAX/PyTorch NaN) · `logdet`
  (`Ā = A⁻ᵀ · ḡ`). **Suite: FFT VJP = IFFT (exact, linear), DSP filtering (correlation = convolution transpose),
  v13 spline eval (transpose of the Thomas build).**
- Honesty: on the *rule math* we MATCH JAX/PyTorch (same Giles/Seeger); the crush is **factor-reuse flops +
  determinism + no-alloc + the value-only degeneracy policy**. Gate: **JAX value+grad parity** on the dense surface +
  `jax.numpy.fft` grad parity.

## E. v16-e — higher-order

- **Forward-over-reverse HVP** — exact Hessian-vector product in ~4 passes (a `Dual`-of-tape or reverse over the
  v15 forward): feeds opt Newton-CG / trust-region (the v15-c hyper-dual is the small-N cross-check oracle). Reverse-
  over-reverse for third order where a consumer needs it. Peers: JAX `hvp`, PyTorch `functorch`. Crush = the exact
  HVP driving opt at matched accuracy + determinism.

## F. v16-f — revolve checkpointing + ODE-adjoint unification

- **Griewank-Walther optimal binomial checkpointing** (revolve): O(log T) live checkpoints for a T-step backward pass,
  provably optimal recompute/memory trade — the WCET-analyzable schedule the certification pillar wants.
- **★ NEW — the exact-vs-cheap split (honesty, arXiv:2306.02192 / arXiv:2410.11648):** ship BOTH:
  (1) **discretize-then-optimize** (AD *through* the integrator, revolve-checkpointed) — gradients EXACT + consistent
  with the discrete forward; the DEFAULT. (2) **continuous adjoint** (optimize-then-discretize) — O(1)-memory backward,
  but the adjoint ODE solved independently can be *inconsistent* with the forward discretisation ⇒ gradient error;
  offer it with the caveat stated loud, and route it through a reverse-accurate integrator (symplectic-adjoint
  arXiv:2102.09750 / MALI) when the RHS is reversible. Unify: the tape drives v9-k's adjoint ODE seam — one adjoint
  story across the suite. Peers: diffrax, torchdiffeq, ANODE, Diffrax reversible.

## G. v16-g — the implicit-differentiation suite (★★ the open C++ lane)

- **IFT custom VJP/JVP** (differentiate the fixed point / optimality condition, never unroll the solver): linear solve
  (dense/sparse/direct) · nonlinear/Newton (`F(x*,θ)=0 ⇒ ∂x*/∂θ = −(∂F/∂x)⁻¹ ∂F/∂θ`, reuse the last Newton factor) ·
  **argmin via KKT** (through hesap-opt QP/NLP — differentiable MPC / OptNet-class layers) · fixed-point ·
  **second-order implicit** (Hessians through solves — the 2026 FEM frontier).
- **★ NEW: Alt-Diff** (arXiv:2210.01802) — alternating differentiation decouples the KKT Jacobian into cheap
  block updates for large structured QPs (avoids forming/factoring the full KKT system); offer alongside the direct
  KKT-implicit path. Peers: jaxopt (DEAD ⇒ own the lane), cvxpylayers/CuClarabel/Moreau, Theseus, OptNet, DiffOpt,
  "black-box QP differentiation" (arXiv:2410.06324). jaxopt-class API in deterministic C++ = the crush.

## H. v16-h — structural graph AD + tape→C++ codegen

- Trace the tape to an expression graph; **fusion / CSE / DCE** passes; then either interpret OR **emit `.crds.cpp`
  hot-reload cells** (ADR-0081) — a JITed differentiated hot loop with zero new runtime. The source-transform lane
  (Enzyme/Tapenade-class) without Enzyme's LLVM-plugin portability loss. Peer for the codegen lane: CVXPYgen (2025, C
  codegen for QP/SOCP solvers). Gate: the codegen'd kernel is bit-identical to the interpreted tape.

## I. v16-i — the deterministic-training MOAT demo (★★ world-first)

- `{1..16}` bit-identical AND batch-invariant gradients gated per op class; the demo: **train the v14-m certified
  tiny controller in-engine** (v7-i SGD/Adam), bit-reproducible run-to-run, benched vs torch-CPU (which structurally
  cannot reproduce — O(1e-4) drift). The certifiable-learned-controller story end-to-end (DO-178C/ISO-26262 evidence
  = replay a *training run* bit-for-bit). The 2025–26 verifiable-training literature (above) is the demand signal.

## J. v16-j — adjoint topology optimization (pre-builds the 3.1.12 FEA seam)

- Density-based topopt (SIMP) through FEA assembly + the hesap-direct solve, gradients by implicit-diff (from g) /
  the discrete adjoint. Gate: the classic **88-line top88** compliance results + **dolfin-adjoint**-class adjoint
  checks (Taylor-remainder convergence test). Refs: "Differentiable Programming for DEs" (arXiv:2406.09699),
  differentiable structural analysis (arXiv:2409.09247), differentiable-simulators review (arXiv:2407.05560).

## K. v16-k — neural-ODE + KAN showcase (the ML-for-science flagship)

- **Neural ODE**: the RHS is a v14 network, trained end-to-end via the v9-k adjoint driven by the tape (revolve-
  checkpointed, discretize-then-optimize default). Gate: torchdiffeq-class problems (spiral, MNIST-ODE).
- **★ KAN** (Kolmogorov-Arnold, ICLR 2025) on the v13 spline machinery — learnable 1-D spline edge functions =
  the natural certified/interpretable architecture. **★ NEW: the Efficient-KAN restructuring** — exploit B-spline
  basis *linearity* to avoid the high-dimensional tensor expansion, making forward AND backward memory/compute-
  efficient (do NOT ship the naive per-edge-spline evaluation). KAN-ODE (arXiv/medrxiv 2024) = KAN as the neural-ODE
  RHS, an interpretable dynamics model — the joint showcase.

## Peer environment (`external/`, stood up at kickoff; `external/PEER_ORACLES.md`)

Reverse: **PyTorch autograd** · **JAX** (vjp/grad/hvp) · **Stan Math** · **Adept-2** · **CoDiPack** · **FastAD** ·
dolfin-adjoint/top88 (adjoint-PDE) · cvxpylayers/Theseus/jaxopt (implicit; jaxopt dead) · diffrax/torchdiffeq (ODE
adjoint) · torchdiffeq + KAN reference (v16-k). Enzyme named OUT (Decision 6). Full-board rule, matched threads,
reconstruct-verify-in-python-first.

## What we read (kickoff research, 2026-07-06)

- **Determinism/moat:** Thinking Machines, *Defeating Nondeterminism in LLM Inference* (2025-09, the batch-invariance
  diagnosis) · DASH arXiv:2601.21824 · bit-identical medical DL arXiv:2603.28040 · reproducible-builds arXiv:2606.03019.
- **Tape/C++ AD:** Stan Math arXiv:1509.07164 · CoDiPack (TOMS 2019) + local-adjoint preaccumulation arXiv:2405.07819 ·
  FastAD arXiv:2102.03681 · Adept (Hogan, TOMS 2014).
- **Sparse reverse:** Hill-Dalle arXiv:2501.17737 (tracer) · **bicoloring arXiv:2505.07308** · sparse reverse LA
  arXiv:2212.05159 · sparse Jac/Hess AD arXiv:2111.05207.
- **ODE adjoint:** *Correcting Auto-Differentiation in Neural-ODE Training* arXiv:2306.02192 · *Efficient, Accurate,
  Stable Gradients for Neural ODEs* arXiv:2410.11648 · symplectic adjoint arXiv:2102.09750 · ANODE arXiv:1902.10298.
- **Implicit diff:** implicit-layers-tutorial.org · cvxpylayers (LocusLab) · jaxopt implicit_diff docs · Alt-Diff
  arXiv:2210.01802 · black-box QP diff arXiv:2410.06324 · CVXPYgen (2025).
- **Diff-sim / topopt:** differentiable-simulators review arXiv:2407.05560 · diff-programming-for-DEs arXiv:2406.09699
  · differentiable structural analysis arXiv:2409.09247 · dolfin-adjoint · top88 (Bendsøe/Sigmund).
- **KAN:** KAN (ICLR 2025) · Efficient-KAN (B-spline linearity restructuring) · KAN-ODE (2024).
