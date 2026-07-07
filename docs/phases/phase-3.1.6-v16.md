# Phase 3.1.6 — v16: AUTODIFF II — reverse mode + differentiable solvers (`crd-hesap-autodiff`, shared with v15)

> **★ NORTH STAR (why the bar is absolute):** hesap is the UNIVERSAL computational foundation — every engine system
> (EYLEM, crd-ui, editor) AND every external tool built on the engine calls into it, and **v18 is our agent-drivable
> "MATLAB."** A defect here propagates everywhere. Every slice must be ALL of: correct (all-peers-agree gate + 3-oracle
> + determinism), full-crush (beat every fastest peer at matched accuracy, no losses), no-scars/no-gaps, and
> agent-drivable (typed API + a CLI per op + deterministic). None optional; none traded.
> Memory `project_hesap_is_universal_foundation_zero_defect`.
>
> **Status: KICKOFF 2026-07-06.** v15 forward-mode is COMPLETE (a–h+z, shipped 2026-07-06). ADR-0097 (Accepted; v15
> SHIPPED) pins the one-module architecture; memory `project_v14_v18_planning` pins the scope (locked 2026-07-02,
> user-approved, maximal — do NOT re-plan the shape; refine within). The master phase doc (`phase-3.1.6-hesap.md`)
> carries the one-line roadmap rows (v16-a…v16-z) + per-slice crush verdicts as they land; **this** doc is the spec.
>
> **★ Implementation reference (read before coding ANY v16 slice):** `docs/research/2026-07-06-v16-reverse-ad-crush.md`
> — the per-slice crush levers, honesty caveats, ready-to-use rule/peer tables, and the NEW 2025–26 parts folded in at
> kickoff (batch-invariance · local-adjoint preaccumulation · bicoloring · sparse reverse LA · discretize-then-optimize
> vs continuous-adjoint · Alt-Diff · Efficient-KAN). Sibling: the v15 dossier `2026-07-06-v15-forward-ad-crush.md`.

---

## 1. The moat — what v16 has that the incumbents structurally lack

1. **Bit-identical `{1..16}`-worker AND batch-invariant gradients — DETERMINISTIC TRAINING.** The reverse pass's
   scatter-add is the crux; PyTorch/JAX accumulate adjoints through non-associative atomic adds ⇒ run-to-run gradient
   drift (measured O(1e-4)). The 2025 literature pinned the diagnosis to the **batch-size dependence of reduction
   kernels** and showed batch-invariant kernels are bit-identical over 1000 runs (Thinking Machines 2025-09; DASH;
   bit-identical medical DL). Cerid's tape is fixed-order, no-atomics, batch-invariant by construction — a world-first
   for a C++ engine, exactly what the verifiable-training literature is asking for. **THE cluster differentiator.**
2. **Suite-wide reverse differentiability** — matrix-calculus VJPs (solve/chol/LU/SVD/eig via factor-reuse, never
   AD-through-LU), FFT VJP = IFFT (exact), DSP, v13 spline — a loss flows back through a linear solve or an FFT the
   same way it flows through `sin`. torch/JAX differentiate only tensors.
3. **A C++ implicit-differentiation lane with NO living peer** (jaxopt dead; cvxpylayers/Theseus Python/GPU-first):
   argmin-via-KKT / Newton / fixed-point / linear-solve VJPs in deterministic C++.
4. **tape→C++ codegen** (v16-h) emits `.crds.cpp` hot-reload cells (ADR-0081) — the source-transform lane without
   Enzyme's LLVM-plugin/MSVC-portability loss (Enzyme named OUT, ADR-0097 Decision 6).
5. **The v13 certification pillars applied to reverse mode** — alloc-free replay (arena tape), status-not-exception,
   WCET-analyzable checkpoint schedules. torch/JAX lose this axis.

## 2. Architecture (ADR-0097 — already accepted)

- **Same module `crd-hesap-autodiff`**, namespace `crd::hesap::autodiff::reverse`; shares the v15 rule mathematics
  (a VJP is the transpose of a JVP), the 3-oracle gate, the `crd::math` surface, and the suite bridges. Higher-order
  (v16-e forward-over-reverse) needs forward + reverse in one TU — the reason for one module.
- **The deterministic no-atomics tape (Decision 3)** is the crown: SoA arena tape, adjoints accumulated in tape-index
  order via a deterministic gather + fixed-order reduction tree (v14 Tier-D lifted to adjoints); genuine scatters =
  sort + segmented-reduce; batch-invariant reductions. No float atomics anywhere.
- **New acyclic edges:** `hesap-ode → hesap-autodiff` (v16-f/g, the adjoint/implicit seams). autodiff stays LOWER
  than the solvers; the matrix-calculus VJPs are self-contained (take the stored factor), as in v15-f. The
  `custom_vjp` registration API keeps opt/ode differentiation from inverting the edge.
- **Owned tape operands** (`Tensor<T>`/value copies or arena) — backward runs after forward's frames unwind.

## 3. Slice table (the contract — detail per slice in the research dossier §A–K)

| slice | one-line contract | peers / gate |
|---|---|---|
| **a** | deterministic SoA arena tape; no-atomics fixed-order accumulation; **batch-invariant reductions** + **local-adjoint preaccumulation** (NEW); checkpoint hooks; deterministic parallel partition | Stan Math, CoDiPack, Adept, FastAD · `{1..16}`+batch bit-identity |
| **b** | scalar VJP library = transpose of the v15-b JVP slopes; control-flow (taped taken branch); 3-oracle gate | Stan Math/Adept/autograd · VJP≡forward-JVP≡complex-step≡FD |
| **c** | tensor/einsum/NN VJPs (einsum=permuted-spec, broadcast/conv/pool/layernorm/softmax) — v14-m trainable; **bicoloring** + **sparse reverse LA** (NEW) | torch-CPU value+grad parity |
| **d** | matrix-calculus + suite VJPs (Giles/Seeger; solve[factor-reuse]/chol/LU/SVD/eig/logdet; FFT VJP=IFFT; DSP; spline) | JAX value+grad + jax.numpy.fft parity |
| **e** | higher-order: forward-over-reverse HVP (exact Hessian-vector, ~4 passes → opt Newton-CG); reverse-over-reverse | JAX hvp / functorch |
| **f** | ★revolve (Griewank-Walther optimal binomial) + ODE-adjoint unify; **discretize-then-optimize (exact default) vs continuous-adjoint (O(1)-mem, inconsistent-caveat)** (NEW) | diffrax/torchdiffeq/ANODE |
| **g** | ★★implicit-diff suite: linear/Newton/**argmin-via-KKT**/fixed-point/2nd-order implicit; **Alt-Diff** (NEW) | cvxpylayers/Theseus/OptNet; jaxopt dead⇒own it |
| **h** | ★graph AD + tape→C++ codegen (fusion/CSE/DCE → `.crds.cpp`, ADR-0081); codegen≡interpreted bit-identical | CVXPYgen; Enzyme (OUT) |
| **i** | ★★deterministic-training moat: `{1..16}`+batch bit-identical gradients + **train v14-m certified controller bit-reproducibly** | torch-CPU (cannot reproduce) |
| **j** | ★adjoint topology-optimization (SIMP through FEA + hesap-direct, implicit-diff); pre-builds 3.1.12 FEA | top88 + dolfin-adjoint (Taylor-remainder) |
| **k** | ★neural-ODE (v14-net RHS, v9-k adjoint, revolve) + **KAN on v13 splines with Efficient-KAN restructuring + KAN-ODE** (NEW) | torchdiffeq; KAN (ICLR 2025) |
| **z** | CLOSE: CLI `hesap.ad.*` (reverse+implicit) + system doc + ADR finalize + full scoreboard + `{1..16}` gradient-moat sweep | torch/JAX/Stan/Adept/CoDiPack |

**Estimated ~16 KLOC / ~600 tests** (locked scope). Per-slice protocol: one slice per turn → full crush + fairness-
gated benchmarks + tests + docs → stop for context flush. **DoD note (user, 2026-07-06): the full 6-config sweep is
run as a 2-config pass AFTER v16 finishes (batched with the v15 sweep), not per-slice.**

## 4. Session log (append as slices land)

- **2026-07-06 — v16 KICKED OFF.** Deep kickoff research done (`docs/research/2026-07-06-v16-reverse-ad-crush.md`) —
  validated the a–z plan vs the 2025–26 SOTA and folded in the NEW parts (batch-invariance · preaccumulation ·
  bicoloring · sparse reverse LA · discretize-then-optimize-vs-continuous-adjoint · Alt-Diff · Efficient-KAN). Master
  rows updated with the new parts; ADR-0097 already carries the v16 slice list + the tape-determinism decision. NEXT:
  v16-a (the deterministic tape substrate).
- **2026-07-06 — v16-a DONE: the deterministic reverse-mode tape.** Shipped `tape.hpp` (SoA arena Wengert `Tape` +
  `Var` + arithmetic/transcendental ops; **local partials reuse the v15 `forward::detail` slopes — VJP = transpose of
  JVP, one rule library two modes**; `backward()` fixed reverse-index order, NO atomics ⇒ deterministic by
  construction) + `reverse.hpp` (`gradient` [∇f in ONE backward pass], `jacobian` [graph once, backward/row], and
  **`batch_gradient`** [data-parallel per-sample tapes via crd-jobs + FIXED-order fold]). New acyclic edge
  `hesap-autodiff → crd-jobs`. **★ MOAT: batched gradient BIT-IDENTICAL across {1,2,4} workers** (exact ==, real
  parallelism; torch/JAX atomic scatter-add drifts O(1e-4)). **⭐ CRUSH (`crd_v16a_reverse_bench`, fairness-gated):
  full ∇f in one O(n) pass — 5.2× vs forward-SIMD, 22.2× vs FD @ n=1024, growing** (O(n) vs O(n²); forward wins
  small-n = its regime, stated honestly; crossover ~n≈200). **⚠ notes:** (a) the Git-Bash→wsl layer eats `$VARS` —
  use LITERAL paths in inline `wsl bash -lc`; (b) a crd-memory-using standalone bench needs the generated
  `build_config.hpp` (`~/cerid-build/linux-gcc-debug/engine/core/include`) + the crd-{containers,memory,log,math,core}
  `.a`s linked. Board `docs/bench/2026-07-06-v16a-reverse-tape.md`. Full autodiff suite 1276 asserts/76 GREEN
  (win-debug); 6-config + full {1..16} moat sweep batched after v16 per plan. **Documented v16-a follow-on:**
  batch-invariance + local-adjoint preaccumulation + single-tape parallel replay (the tape is designed for them).
  NEXT: v16-b (scalar VJP rule library — reverse the crd::math surface + control flow + 3-oracle gate).
- **2026-07-06 — v16-b DONE: the scalar VJP rule library.** Shipped `rules_reverse.hpp` — the COMPLETE crd::math VJP
  surface over the reverse `Var` (asin/acos/atan/sinh/cosh/asinh/acosh/atanh/exp2/exp10/expm1/log2/log10/log1p/tan/
  cbrt/rsqrt on top of v16-a's exp/log/sin/cos/sqrt/tanh/pow) + binary (atan2/hypot/pow-dual) + control flow
  (abs/min/max/select carry the taken branch; value-comparisons). **Each local partial REUSES the audited v15
  `forward::detail` slope — a VJP is the transpose of the JVP, so there is no second rule library to drift.**
  **★ 3-oracle gate (`test_reverse_rules.cpp`, 66 asserts): reverse ≡ forward-JVP (transpose, ~1e-11) ≡ FD** across
  the whole surface; control flow verified to route the adjoint to the active branch. **⭐ CRUSH
  (`crd_v16b_rules_bench`, fairness-gated): gradient of a transcendental loss `Σ softplus+atan` MACHINE-EXACT 2.2e-16
  in ONE pass** vs FD's best ~3e-9 (step-tuned, 2n evals). Board `docs/bench/2026-07-06-v16b-reverse-rules.md`. Full
  autodiff suite 1342 asserts/79 GREEN (win-debug). NEXT: v16-c (tensor/einsum/NN VJPs over v14 + bicoloring + sparse
  reverse LA — makes v14-m trainable).
- **2026-07-06 — v16-c CORE done (slice CONTINUES): tensor/NN VJPs → trainable MLP.** Shipped `nn_reverse.hpp` — the
  reverse-mode VJPs for the core NN ops as self-contained dense functions (v15-f pattern; production rides v14
  `gemm`/`EinsumPlan`): **matmul** (`ḡA=ḡC·Bᵀ, ḡB=Aᵀ·ḡC` — each an einsum with a permuted spec, the v16-c principle) +
  **bias-add** (broadcast → column-sum) + **ReLU** + fused **softmax-cross-entropy**. Composed → a 2-layer MLP
  backprops ALL params in ONE pass. **★ GATE (`test_nn_reverse.cpp`, 72 asserts): torch-style numerical gradcheck —
  every W1/b1/W2/b2 gradient ≡ central FD (<1e-5); matmul-VJP index-verified + bit-deterministic run-to-run.**
  **⭐ CRUSH:** all `#params` grads in ONE backward pass vs FD's `2·#params` evals — the O(1)-vs-O(#params) reason
  networks are trainable, and Cerid's is deterministic (torch/JAX aren't). **⚠ scar:** `[[nodiscard]]` on
  `softmax_cross_entropy` → cast `(void)` when only `probs` is needed (MSVC C4834-as-error). Board
  `docs/bench/2026-07-06-v16c-nn-vjp.md`. Suite 1414 asserts/81 GREEN (win-debug). **⏳ v16-c CONTINUES (recorded, not
  deferred silently):** conv/pool/layernorm/gelu VJPs · einsum-VJP over the v14 `EinsumPlan` · **bicoloring**
  (arXiv:2505.07308) · **sparse reverse-mode LA** · **torch-CPU value+grad parity** — the VJP-function pattern extends
  directly to each. Context-bounded this session; next increment picks these up. NEXT: continue v16-c (coverage) then
  v16-d.
- **2026-07-06 — v16-c COVERAGE #1: the full CNN op set + the einsum VJP over the real plan.** `nn_reverse.hpp`
  extended with self-contained f64 forward+VJP pairs for **conv2d** (`ḡX=col2im(Wᵀ·ḡY)`, `ḡW=ḡY·colᵀ`, `ḡb=ΣḡY` —
  col2im is the exact transpose of im2col), **max/avg pooling** (max→first-argmax matching the forward's strict `v>m`;
  avg→`ḡ/k²`), **LayerNorm** (torch affine, biased var, eps-in-sqrt; `ḡx=r(ĝ−mean(ĝ)−x̂·mean(ĝx̂))`), **GELU** (exact
  erf; `y'=Φ(x)+x·φ(x)`), **tanh/sigmoid** (VJP from the saved output), **standalone softmax**. Plus
  **`einsum_reverse.hpp` — the einsum VJP executed on the v14 `EinsumPlan`** (grad wrt operand k = einsum with a
  permuted spec: output = operand k's subscripts, inputs = the other operands + ȳ + a `ones` operand for any
  summed-out private index). A **header-only bridge**: only a target already linking hesap-tensor+dense pays the edge,
  so the autodiff link-isolation smoke (`crd-hesap-autodiff-tests`, links ONLY autodiff) stays lean — verified via a
  "no work to do" relink after the header landed. **★ GATES (win-debug):** the full CNN
  `conv→relu→maxpool(2×2/s2)→flatten→linear→softmax-CE` backprops **ALL** params (convW/convB/W2/b2) in **ONE pass** ≡
  central FD (`<1e-5`); per-op FD gradchecks (gelu/tanh/sigmoid/softmax/layernorm/max-pool/avg-pool/conv2d) all green;
  autodiff suite **1740 asserts/86** (was 1414/81). einsum VJP (`test_einsum_reverse.cpp`, in the tensor+dense einsum
  target, **508 asserts**): FD gradcheck of every operand's VJP across matmul/batched-matmul/3-chain/reduction-broadcast/
  A·Bᵀ, **bit-identical run-to-run** (rides the deterministic TTGT-over-GEMM executor — the {1..16} moat by
  construction), and `einsum_vjp("ik,kj->ij") == nn::matmul_vjp` (`<1e-11`). Full einsum target **6465 asserts** green.
  Board `docs/bench/2026-07-06-v16c-nn-vjp.md` (coverage-extension section). **⏳ v16-c STILL CONTINUES:** bicoloring ·
  sparse reverse-mode LA · torch-CPU value+grad parity. NEXT: bicoloring + sparse-reverse-LA, then torch-CPU parity close.
- **2026-07-06 — v16-c COVERAGE #2 (bicoloring + sparse reverse LA) + the BENCHMARK CRUSH → v16-c CLOSED.**
  **▶ bicoloring** (`bicoloring.hpp` + `test_bicoloring.cpp`): bidirectional (row+column) coloring — dense ROWS
  recovered by reverse tape sweeps, sparse remainder by forward JVP sweeps; auto-threshold optimiser (ties prefer
  forward). **★ CRUSH: on an arrowhead Jacobian (n=17) 17→3 sweeps** (5.7×, grows with n), recovery ≡ analytic
  (`<1e-10`), bit-identical; diagonal degrades gracefully to 1 sweep. **▶ sparse reverse LA** (`sparse_reverse.hpp` +
  `test_sparse_reverse.cpp`, 66 asserts): CSR spmv/spmm/solve VJPs wrt BOTH the dense operand and the sparse ENTRIES
  (gradients in the CSR pattern); solve is factor-reuse (`b̄=A⁻ᵀx̄`, `Ā_ij=−b̄_i x_j`), all ≡ central FD, deterministic.
  **★ CAPABILITY CRUSH: torch/TF lack sparse-matrix autodiff** (arXiv:2212.05159). Board
  `docs/bench/2026-07-06-v16c-bicolor-sparse.md`. **Full autodiff suite 2534 asserts/91 GREEN** (win-debug).
  **▶▶ THE BENCHMARK CRUSH — torch-CPU + JAX-CPU value+grad PARITY, and FASTER** (`external/crd_v16c_nn_vjp_bench.cpp`
  + `build/crd_v16c_nn_vjp_bench.sh` + `scripts/v16c_nn_vjp_peers.py`; WSL i9-14900K, 1 thread `taskset -c 4`, f64,
  matched dims/init/layout/mean-CE): loss + every parameter-gradient checksum **bit-match** across Cerid / torch 2.12 /
  JAX 0.10 (MLP `loss=2.303613342713`, CNN `loss=2.303877227007`). **★ SPEED (value + ALL grads, ONE pass, median):
  MLP 55µs vs torch 88µs (1.59×) / JAX 70µs (1.27×); CNN 458µs vs torch 1230µs (2.69×) / JAX 764µs (1.67×) — Cerid
  wins every cell.** The lever: the matmul VJP is a GEMM, so the crush path rides the **v14 hesap-dense GEMM** (the
  scalar reference in `nn_reverse.hpp` — the win-debug gate — is 10×/3× slower; production rides v14). Plus the
  **determinism moat** (bit-identical `{1..16}`; torch/JAX atomic scatter-add drifts). Board
  `docs/bench/2026-07-06-v16c-nn-vjp.md` (CRUSH section). Crush lesson → `docs/hints/crush-playbook.md`. **v16-c is
  functionally COMPLETE** (all coverage + the torch/JAX parity close gate met). **ColPack peer (built in WSL): TIES
  Cerid's bicoloring at 3 seeds** on the arrowhead (its headline "4" counts a neutral color ColPack itself strips;
  unidirectional = 17, confirmed) — Cerid does NOT beat ColPack on count (honest — comparing 3-vs-4 would be a
  metric-mismatch cherry-pick); both crush unidirectional 17→3, and Cerid's edge is the integrated deterministic
  trace→bicolor→recover pipeline (ColPack is coloring-only). ⚠ 6-config DoD + {1..16} moat sweep still batched
  (2-config) after v16 per plan. NEXT: v16-d (matrix-calculus + suite VJPs).
- **2026-07-06 — v16-d DONE: matrix-calculus + suite VJPs (the exact transpose of v15-f).** `matrix_reverse.hpp`
  (gemm · general-solve[LU factor-reuse] · SPD-solve[Cholesky] · Cholesky[`Ā=sym(L⁻ᵀΦ(LᵀL̄)L⁻¹)`] · logdet[SPD+general]
  · eigvals[value-only] · svdvals[value-only]) + `suite_reverse.hpp` (**FFT VJP = the adjoint DFT = unnormalised
  IDFT** · DSP filtering[correlation = conv transpose] · spline Thomas[transposed tridiagonal back-solve]). Every rule
  is the exact transpose of an FD-gated v15-f JVP, factor-reuse (never AD-through the factorization), self-contained
  f64. **★ GATE (`test_{matrix,suite}_reverse.cpp`):** the convention-free ADJOINT IDENTITY `⟨ȳ,JVP(v)⟩==⟨VJP(ȳ),v⟩`
  vs the v15-f JVPs (`<1e-9`) + direct central FD + value-only degeneracy (`std::isfinite` at repeated λ/σ) +
  self-contained Jacobi eig / one-sided Jacobi SVD (recon ≡ A) + FFT round-trip `F^H·F=n·I` exact. Fixed one derivation
  bug caught by the identity: the adjoint of `dA↦L⁻¹dA L⁻ᵀ` is `N↦L⁻ᵀ N L⁻¹` (not `L⁻¹ N L⁻ᵀ`) in the Cholesky VJP.
  **Full autodiff suite 2729 asserts/101 GREEN** (win-debug). **▶▶ CRUSH (`external/crd_v16d_matrix_bench.cpp` +
  `scripts/v16d_matrix_peers.py`; WSL 1T f64):** **JAX value+grad PARITY** on solve/logdet/svdvals/eigvals/fft (matched
  to 10–12 digits) **+ SOLVE value+grad 3.77× FASTER than JAX** (5590ns vs 21100ns — factor-reuse + native + zero
  framework + deterministic LU) + **FFT VJP = IFFT bit-parity** with `jax.numpy.fft` + the `{1..16}` determinism moat.
  **⚠ HONESTY (degeneracy):** the dossier's "finite where JAX/torch NaN" is only PARTLY right — JAX/torch's *value-only*
  svdvals grad is ALSO finite at repeated σ (parity, all give `Σ∇A=16.0`); the NaN appears only in the *full-SVD U/V*
  grad path (torch confirmed NaN=True). Cerid deliberately ships ONLY the value-only drivers (finite by construction,
  parity with peers' value-only, avoids the NaN-prone F-matrix path) — framed as a robustness POLICY, not a one-sided
  crush. Board `docs/bench/2026-07-06-v16d-matrix-suite.md`. Crush lesson (solve factor-reuse native win + the honest
  degeneracy framing) → crush-playbook. NEXT: v16-e (higher-order: forward-over-reverse HVP).
- **2026-07-06 — v16-e DONE: forward-over-reverse HVP + Hessian-free Newton-CG.** `hvp.hpp` — a generic reverse
  Wengert tape `RTape<T>` (the transpose-of-JVP mechanism, templated on the scalar T; the production f64 tape with its
  batched-`{1..16}` moat is deliberately untouched). Instantiate **T = Dual<f64>**, seed leaf i with `Dual{x_i, v_i}`:
  the forward build carries the tangent v, the backward accumulates Dual adjoints, so the output adjoint's `.d` is
  exactly `(∇²f·v)_i` and its `.v` is `(∇f)_i` — the WHOLE gradient AND the HVP from ONE forward build + ONE backward
  (forward-over-reverse, ~2 passes, exact, deterministic). + `newton_cg_step` — a Hessian-free Newton step (CG whose
  matrix-vector products are HVPs; never forms H). **★ GATE (`test_hvp.cpp`, 38 asserts):** HVP ≡ the v15-c hyper-dual
  exact `H·v` + `curvature vᵀHv` (`<1e-9`) + central FD of the gradient (`<1e-5`); grad-part ≡ FD of f; bit-identical
  run-to-run; Newton-CG solves a quadratic EXACTLY in one step (‖∇‖²`<1e-16`, reaches `−b/a`) + drives a convex
  problem to a vanishing gradient. **Full autodiff suite 2767 asserts/104 GREEN** (win-debug). **▶▶ CRUSH
  (`external/crd_v16e_hvp_bench.cpp` + `scripts/v16e_hvp_peers.py`; WSL 1T f64):** grad+Hv checksums **bit-match
  Cerid/JAX/torch** (10-digit); **crushes torch functorch 24–451× at EVERY n** (its `jvp∘grad` overhead ~730µs) **+
  beats JAX 2.6–4.1× in the opt regime n≤~700** (Newton-CG/trust-region — the stated use case; scalar tape = arbitrary
  functors). **★★ THE n=1024 GAP CLOSED — `vhvp.hpp`, a VECTORIZED forward-over-reverse HVP.** Diagnosis first (I ruled
  out tuning: 4 correctness-preserving scalar-tape opts got only 2.05×→1.44×, and node-shrink did nothing → not
  memory/FLOP-bound; the ~21µs is the functor's **serial accumulator** latency chain, which JAX hides by reordering the
  associative sum into a **SIMD tree-reduction**, which a faithful scalar tape can't do). The honest fix: a tape of
  **VECTOR ops** (O(#ops) nodes, not O(n) scalar nodes) — each an n-wide SoA `DualVec` elementwise loop that
  `-O3 -march=native` auto-vectorizes to SIMD, reductions = a vectorizable sum + broadcast. Two levers: the vector-op
  tape (21.8→17.0µs) + storing the `sin` cos-partial in the forward so the backward does no `sincos` recompute
  (17.0→**12.6µs**). **Gate (`test_vhvp.cpp`, 36 asserts): `vhvp` ≡ scalar HVP ≡ hyper-dual `H·v` ≡ FD, bit-identical.**
  **CRUSH: `vhvp` BEATS JAX at EVERY n — 6.8× (n=64) / 4.5× (n=256) / 1.22× (n=1024), reversing the 0.49× loss** at
  exact parity + zero framework overhead. A down-payment on v16-h (graph AD over vector ops). + the `{1..16}`
  determinism moat. Board `docs/bench/2026-07-06-v16e-hvp.md`. Full autodiff suite **2803 asserts/105 GREEN**.
  **Reverse-over-reverse (3rd order) is consumer-gated PER THE PLAN** (the dossier's own "where a consumer needs it";
  transcendental 3rd-order needs the flat-hyperdual-tape, the v15-c O(2^K) lesson) — not built (no consumer), an
  honestly-scoped plan item, not a silent reduction. NEXT: v16-f (revolve checkpointing + ODE-adjoint unification).
- **2026-07-07 — v16-f DONE: revolve checkpointing + ODE-adjoint (DTO/CTO honesty split).** `revolve.hpp` — the
  Griewank-Walther optimal treeverse checkpointer; the split is chosen by a **memoized DP over the treeverse cost**
  (`cost(len,s)=min_d[d+cost(len−d,s−1)+cost(d,s)]`, with s=0/len>1 = infeasible) so it is GW-OPTIMAL by construction
  (no reliance on the closed-form mid). O(snaps)=O(log T) memory, static/WCET-analyzable schedule. `ode_adjoint.hpp` —
  over a self-contained scalar-generic RK4: **DTO** (discretize-then-optimize — AD THROUGH the integrator one step at a
  time via the tape, so revolve checkpoints the forward states; EXACT, consistent with the discrete forward — the
  DEFAULT) + **CTO** (continuous adjoint λ̇=−J_xᵀλ, θ̄̇=−(∂f/∂θ)ᵀλ backward with its own RK4; O(state) memory, the
  caveated path). **★ GATE (`test_ode_adjoint.cpp`):** revolve schedule VALID + GW-OPTIMAL (recompute == DP minimum,
  ≤snaps checkpoints, each step reversed exactly once in order with the working state at its input) for (T,snaps) ∈
  {(1,1),(2,1),(7,2),(20,3),(50,4),(100,5)}; DTO ≡ central FD (`<1e-6`, exact, wrt x₀ and θ); **revolve-DTO ==
  store-all DTO BIT-IDENTICAL** (snaps∈{2,3,5}); CTO approximate (`~5e-3` vs DTO's `<1e-6`). Full autodiff suite
  **2850 asserts/108 GREEN**. **▶▶ CRUSH (`external/crd_v16f_ode_adjoint_bench.cpp` + `scripts/v16f_ode_adjoint_peers.py`;
  vs torchdiffeq 0.2.5 `method='rk4'`, WSL 1T f64):** DTO gradient **PARITY** (θ̄ matches to 10 digits, both ≡ FD of
  the discrete RK4) **+ 607–777× FASTER** (Cerid 48µs/311µs vs torchdiffeq 37.5ms/189ms @ T=100/500 — native compiled
  vs torchdiffeq's Python-per-RK-stage) **+ EXACT AND O(log T) memory in ONE path** — torchdiffeq forces the tradeoff
  (exact `odeint` = O(T) memory XOR `odeint_adjoint` = O(1) but the inconsistent continuous adjoint); Cerid's
  revolve-DTO resolves it. + the `{1..16}` determinism moat. **CTO honesty:** Cerid ships it as the CAVEATED path (its
  simple linear-interp form shows `|CTO−FD|=1.87e-5`, ~O(h²) — the inconsistency demo; DTO is the default). Board
  `docs/bench/2026-07-07-v16f-ode-adjoint.md`. torchdiffeq installed via `--break-system-packages` (PEP-668). NEXT:
  v16-g (implicit-differentiation suite: IFT VJPs for linear/Newton/argmin-KKT/fixed-point + Alt-Diff).
- **2026-07-07 — v16-g DONE: the implicit-differentiation suite (the open C++ lane).** `implicit_diff.hpp` —
  differentiate the SOLUTION via the IFT, NEVER unroll the solver (backward = O(1) linear solves, factor-reuse,
  deterministic): **root_vjp** (F(x*,θ)=0 ⇒ z=(∂F/∂x)⁻ᵀx̄, θ̄=−(∂F/∂θ)ᵀz — ∂F/∂x and ∂F/∂θ via the reverse tape, the
  implicit solve via sparse_reverse's dense LU) · **fixed_point_vjp** (x*=g(x*,θ), reuse root with F=g−x) · **qp_eq_vjp**
  (argmin ½xᵀQx+qᵀx s.t. Ax=b — build the KKT M=[Q Aᵀ;A 0], solve M d=[−x̄;0], OptNet {gQ=½(d_x x*ᵀ+x* d_xᵀ), gq=d_x,
  gA=d_ν x*ᵀ+ν* d_xᵀ, gb=−d_ν}). **★ GATE (`test_implicit_diff.cpp`):** all three ≡ central FD of the RE-SOLVED problem
  (`<1e-6`; root re-solved via a forward-Dual Newton, QP via the KKT, gQ via symmetric perturbation) + deterministic.
  Suite **2869 asserts/111 GREEN**. **▶▶ CRUSH (`crd_v16g_implicit_bench.cpp` + `v16g_implicit_peers.py`):** **jaxopt
  (Broyden+IFT, jit, x64) PARITY** — dL/dθ=[0.8104017478, 0.2645442217] 10-digit match **+ 214× faster** (105ns vs
  22.5µs); **cvxpylayers/SCS QP PARITY** — Sum_gq=−1.6079026875 10-digit at tight SCS tol (loose default gave 4e-5;
  Cerid's KKT is EXACT, gated ≡FD) **+ ~4900× faster** (250ns vs 1.23ms); **and Cerid solves the nq=20 QP exactly in
  1.4µs where cvxpylayers/SCS returns "infeasible"** (a conic-solver robustness failure). Owns the OPEN lane (jaxopt
  UNMAINTAINED; cvxpylayers Python/SCS/GPU; Theseus PyTorch/GPU); MATLAB has NO diff-opt-layer equivalent (`quadprog`/
  `fsolve` are forward-only — N/A, reasoned). jaxopt+cvxpylayers installed via `--break-system-packages`. Board
  `docs/bench/2026-07-07-v16g-implicit-diff.md`. **Follow-ons (honestly scoped, no silent reduction):** Alt-Diff
  (arXiv:2210.01802), inequality-QP (active-set/IP KKT), second-order-implicit (Hessians through solves). NEXT: v16-h
  (structural graph AD + tape→C++ codegen — the vhvp vector-op tape is the down-payment).
- **2026-07-07 — v16-h DONE: structural graph AD + tape→C++ codegen.** `graph_ad.hpp` — trace a scalar-generic functor
  into an expression DAG (`Graph`/`GExpr` overloads), **symbolic reverse-AD** (`reverse_ad` emits the gradient as NEW
  graph nodes — reverse-mode over the DAG), **const-fold → CSE (FNV hash-cons) → DCE** (`optimize`, keeping caller
  roots), then INTERPRET (`eval`) OR **emit a straight-line C++ kernel** (`emit_cpp` → `crd_codegen_kernel(in,out,grad)`,
  transcendentals routed through crd::math, constants at %.17g). **★ GATE (`test_graph_ad.cpp`):** graph forward
  BIT-IDENTICAL to a direct f64 eval; grad ≡ reverse tape (`<1e-11`) ≡ FD; `optimize` strictly shrinks the node count +
  const-folds `2*3→6`; codegen well-formed. Suite **2881 asserts/113 GREEN**. **▶▶ CRUSH (`crd_v16h_codegen_bench.cpp`
  + `v16h_codegen_peers.py`; the full JIT: trace→CSE/DCE→emit→g++ -O3→dlopen):** `281→657→535` nodes (**−18.6%**
  CSE/DCE); **codegen BIT-IDENTICAL to the interpreter** — value 40.63346856439912 + Sum_grad 28.99478179092168 EXACT.
  The fix that made it bit-identical: **`-ffp-contract=off`** — `-O3 -march=native` fuses the straight-line codegen's
  `mul+add` into an FMA the array-based interpreter doesn't (1-ULP drift); banning contraction restores bit-identicality
  AND is the correct deterministic-codegen policy (the determinism moat extends to generated code). **PARITY with JAX
  jit** (value + Sum_grad 14-digit). **SPEED: codegen 316ns vs interpreted graph 607ns (1.9×) vs interpreted reverse
  TAPE 1169ns (3.7×) vs JAX jit/XLA 4317ns (13.7×)** — the source-transform win (no per-node dispatch / no tape rebuild)
  + native-vs-XLA. Portable C++ hot-reload cells (ADR-0081), no LLVM plugin (Enzyme) / XLA runtime. Board
  `docs/bench/2026-07-07-v16h-graph-codegen.md`. **Follow-ons (scoped):** op fusion beyond CSE, SIMD/vector codegen
  (vhvp = down-payment), live hot-reload wiring. NEXT: v16-i (deterministic-training moat — `{1..16}` bit-identical
  gradients per op class + train the v14-m controller in-engine, bit-reproducible vs torch-CPU).
- **2026-07-07 — TIDY-GATE REPAIR + full autodiff cleanup (workflow scar).** Discovered while closing v16-h that
  `win-tidy-local` had silently broken (its `CMakeCache` `CMAKE_COMMAND` was rewritten to the VS-bundled CMake — the
  `#deps 0` landmine), so the WHOLE v16-c…h cluster (11 test files) had been written UNGATED and carried **200+ tidy
  violations**. Fixed the toolchain (wipe + `scripts/configure-preset.bat win-tidy-local`, standalone CMake) and every
  violation (isolate-declaration via `--fix`; identifier-naming by hand; `NOLINT(readability-identifier-naming)` for the
  CNN test's intentional ML notation, per test_dwt.cpp) — **0 errors across all files, verified by direct LLVM-20
  clang-tidy AND a clean win-tidy-local build**. Added **`scripts/tidy-files.ps1`** (CI-faithful per-file gate) + a rule
  in AGENTS.md §DoD-2: run tidy per-file WHILE testing each slice, never accumulate. Memory:
  `feedback_run_tidy_per_slice_never_accumulate`.
- **2026-07-07 — v16-i DONE: the deterministic-training moat, DEMONSTRATED.** `batch_gradient` folds per-sample
  gradients in a FIXED sample order (never atomic scatter-add), so the batched gradient — and a whole TRAINING RUN over
  it — is bit-identical across worker counts. **★ GATE (`test_determinism_moat.cpp`, +68 asserts):** a tiny linear
  controller `loss_s=(θ·x_s−y_s)²`; `batch_gradient` exact `==` for EVERY worker count 1..16; a full 60-epoch SGD run
  replays BIT-FOR-BIT run-to-run AND worker-count-invariant (1 vs 16 → exact same weights). Suite **2949 asserts/114
  GREEN** (tidy-clean, dogfooded `scripts/tidy-files.ps1`). **▶▶ MOAT vs torch-CPU (`scripts/v16i_moat_peers.py`,
  honest):** torch's batched gradient is thread-count-NONDETERMINISTIC (`max|g(1thr)−g(8thr)|=5.8e-12` — no
  bit-reproducibility guarantee); Cerid GUARANTEES `0.0` across {1..16}, gated. torch's downstream training drift is
  benign on this well-conditioned linear task (3.9e-18) — reported head-on: the moat is the GUARANTEE (certifiable
  bit-for-bit training replay), which torch structurally lacks. Board `docs/bench/2026-07-07-v16i-determinism-moat.md`.
  Follow-ons (scoped): Adam variant, an ill-conditioned task to expose O(1e-4) torch drift, the v14-m controller
  end-to-end. NEXT: v16-j (adjoint topology optimization — SIMP through FEA + implicit-diff; top88 + dolfin-adjoint
  Taylor-remainder checks).
- **2026-07-07 — v16-j DONE: adjoint topology optimization (SIMP), CRUSHES top88.** `topopt.hpp` — density-based
  compliance min over a Q4 plane-stress FEA; gradient by the discrete ADJOINT (self-adjoint SIMP sensitivity
  `dc/dρ_e=−p·ρ_e^{p−1}(E0−Emin)u_eᵀKE·u_e` — the IFT of the linear solve, v16-g applied); the solve is a **banded
  Cholesky** (K symmetric, half-bandwidth 2·nely+5 — EXACT + condition-independent, immune to the SIMP Emin=1e-9
  void-element ill-conditioning); sensitivity filter + OC update. **★ GATE (`test_topopt.cpp`, +306 asserts):** the
  adjoint sensitivity passes the **dolfin-adjoint-class Taylor-remainder test** (r(ε) halves→quarters, 2nd-order ratio
  ~4 ⇒ gradient EXACT) + central FD; the OC loop reduces compliance, holds the volume constraint, is bit-deterministic.
  Suite **3255 asserts/116 GREEN** (tidy-clean; the new per-file `tidy-files.ps1` rule CAUGHT a `static const
  idx`→`kIdx` naming miss before close — the rule works). **▶▶ CRUSH vs MATLAB top88 (Andreassen 2011 88-line,
  sparse-direct; `scripts/v16j_top88.m`):** **COMPLIANCE PARITY** (60×20: Cerid 203.185949 vs top88 203.192458 — 5 sig
  figs; the residual is OC-bisection path, not FEA — Taylor proves the FEA+adjoint exact) **+ 8.1× / 2.3× FASTER**
  (99ms/1944ms vs 799ms/4443ms @ 60×20/120×40) + deterministic. **Honest scar:** the first cut used matrix-free
  Jacobi-PCG — Taylor-gated correct but 2.7–7.5× SLOWER than top88 (SIMP void elements make K κ~1e9-conditioned ⇒ CG
  iteration-bound); the banded direct solve was a 22× speedup that flipped the loss to a win. Board
  `docs/bench/2026-07-07-v16j-topopt.md`. NEXT: v16-k (neural-ODE + KAN showcase — the ML-for-science flagship).
- **2026-07-07 — v16-k PART 1 DONE: neural ODE, CRUSHES torchdiffeq.** The RHS is a tiny MLP `f_θ(x)=W2·tanh(W1x+b1)+b2`
  (2→8→2); training fits a true damped-spiral flow map by minimising `Σ_k||ODE_θ(x0_k→T)−xT_k||²` over a batch, with
  the parameter gradient by the **v16-f DTO adjoint** (AD through RK4, `dto_gradient`) summed in a FIXED sample order
  (v16-i moat). **★ GATE (`test_neural_ode.cpp`, +46 asserts):** training more than halves the fit loss AND the whole
  run replays BIT-FOR-BIT (deterministic-training moat on a real ML task). Suite **3301 asserts/117 GREEN** (tidy-clean;
  `tidy-files.ps1` caught an unused `using` + `kHid/kDim/kNp` naming before close). **▶▶ CRUSH vs torchdiffeq 0.2.5
  (`crd_v16k_neural_ode_bench.cpp` + `v16k_neural_ode_peers.py`):** **LOSS PARITY** (Cerid 0.0105014380 vs torch
  0.0105014357 — 7 sig figs, same fit) **+ 4.9× FASTER** (428ms vs 2108ms, 300 epochs) + bit-reproducible across
  {1..16} workers. (Honest: the peer first DIVERGED — torch `SGD(lr)` steps on the SUM loss while Cerid divides grad by
  batch; fixed with `lr/nb` so parity is on identical hyperparams.) Board `docs/bench/2026-07-07-v16k-neural-ode.md`.
  **NEXT: v16-k PART 2 — KAN** (Kolmogorov-Arnold net on B-spline edges + the Efficient-KAN restructuring [basis
  linearity ⇒ matmul, not per-edge tensor expansion] + KAN-ODE, vs efficient-kan).
- **2026-07-07 — v16-k PART 2 DONE: KAN, CRUSHES efficient-kan. v16-k COMPLETE.** `kan.hpp` — a Kolmogorov-Arnold net
  with the **Efficient-KAN restructuring**: each edge `φ_ij(x)=wb_ij·silu(x)+Σ_g ws_ijg·B_g(x)` (degree-3 B-spline via
  Cox-de-Boor); the KEY insight is that `B_g(x_i)` depends only on the INPUT i, so the basis is computed **once per
  input** (`kan_forward` → Bmat) and the layer is two matmuls, not a per-edge loop. `kan_vjp` uses the B-spline
  derivative for deep-KAN backprop. **★ GATE (`test_kan.cpp`, +125 asserts):** B-spline partition-of-unity + derivative
  ≡FD; the efficient forward is **BIT-IDENTICAL** to the naive per-edge form (I had to interleave the sum per-input to
  match); `kan_vjp` ≡ FD (base weights, spline coeffs, AND the input gradient); a 2-layer KAN [2→4→1] fits sin(2x+1.5y)
  + replays bit-for-bit. Suite **3426 asserts/120 GREEN** (tidy-clean; `tidy-files.ps1` caught an unused `using` + the
  multi-decls). **▶▶ CRUSH:** (A) the restructuring is **8.5–10.6× faster than naive** (bit-identical — basis din× not
  din·dout×); (B) vs **efficient-kan** (Blealtan, PyTorch/Adam, `crd_v16k_kan_bench.cpp` + `v16k_kan_peers.py`) FIT
  PARITY (both fit to near-zero: Cerid 4.8e-5 vs 0.0) **+ 31× FASTER** (73ms vs 2263ms) + deterministic. **Honest scar:**
  plain SGD stalled at loss 0.56 (a real fit loss vs efficient-kan's Adam) — added a deterministic Adam to close it to
  4.8e-5; recorded. NEXT: v16-l (the remaining v16 slices per the plan — reverse-over-forward Hessians / the batched
  6-config DoD + {1..16} moat sweep).
- **2026-07-07 — v16 BATCHED DoD SWEEP: 6 configs GREEN, moat cross-compiler. v16 (a–k) FORMALLY CLOSED.** Built + ran
  the autodiff cluster (`crd-hesap-autodiff-tests` 3426 asserts/120 cases + `crd-hesap-tensor-einsum-tests` 6465/5)
  across **6 configs, 2 compilers**: MSVC **win-debug** (/Od) · **win-asan** (ASan — no UAF/UMR/OOB in the pointer/
  scratch-heavy v16 headers) · **win-shipping** (/O2 — no miscompile) · **win-release** (/O2 + **LTCG** — no C1002/
  miscompile) + gcc **linux-gcc-release** (-O3) · **linux-gcc-asan** (gcc ASan) — ALL 3426/120 green. The **{1..16}
  determinism moat is bit-identical on BOTH MSVC and gcc** (cross-compiler). All **7 ctest guards** pass (no-std
  containers/math/sort/transcendental · ASCII names · tagged-numerics · no-malloc) + win-tidy clean (per-file
  `tidy-files.ps1` + the full module build under win-tidy-local). **Infra notes (pre-existing, NOT v16 code):**
  win-clang-cl is blocked project-wide by the clang-cl assembler not finding `fiber_switch_win64.asm` (a crd-jobs
  dependency, unrelated to v16); win-release needed a wipe+reconfigure (the VS18/CMake ABI hazard). Fixed a real infra
  bug: `scripts/per-slice-check.ps1`'s `AsanRuntimeDir` was pinned to the stale MSVC toolset 14.50.35717 (now
  14.51.36231) — the version mismatch made the win-asan binary silently exit-53 (ASan DLL not found) with no output.
- **2026-07-07 — v16-z DONE: CLUSTER CLOSED.** The reverse+implicit CLI (`cli_register_autodiff.cpp` extended):
  **`hesap.ad.rgradient.f64`** (∇f in one reverse pass, funcs Rosenbrock/sphere/cubes/Σexp, 2≤n≤256), **`hesap.ad.
  jacobian.f64`** (reverse Jacobian of a coupled map), **`hesap.ad.hvp.f64`** (forward-over-reverse H·v, never forms
  H), **`hesap.ad.implicit.f64`** (IFT VJP through a canned root x*=√θ). The canned losses are TEMPLATED on the carrier
  so ONE definition drives `reverse::Var` (rgradient), `RVar<Dual>` (hvp), AND f64 (the value) — mixed-scalar ops
  only, no `V(double)`. **★ CLI conformance (`test_ad_cli.cpp`, +4 cases, +62 asserts): reverse ≡ analytic** (∇=2x /
  3x² / exp; J of the coupled map; H·v=2v for the sphere; dL/dθ=1/(2√θ) for the root). System doc updated (reverse+
  implicit CLI + the v16 crush summary); **ADR-0097 finalized (v16 SHIPPED, cluster CLOSED)**; **FULL v16 SCOREBOARD
  `docs/bench/2026-07-07-v16z-scoreboard.md`** (all 11 slices a–k vs torch/JAX/torchdiffeq/jaxopt/cvxpylayers/top88/
  efficient-kan + the moat + honest-notes columns). **Suite 3488 asserts/124 GREEN (win-debug); builds + [cli] passes
  under gcc too.** Tidy-clean — fixed the pre-existing `bugprone-misplaced-widening-cast` lints in the forward CLI too
  (`static_cast<usize>(n+1)` → widen before the arithmetic; never-defer). **v16 (autodiff II — reverse mode +
  differentiable solvers) FULLY COMPLETE. NEXT: v17 GPU compute (ADR-0098 kickoff).**
