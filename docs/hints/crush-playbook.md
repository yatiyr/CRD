# Crush & Optimization Playbook (living hints)

> **Purpose.** The recurring techniques and traps for beating gold-standard peers (BLAS/LAPACK, Eigen, MKL, scipy,
> MATLAB, JAX/PyTorch, Ceres, …) at **matched accuracy** while keeping the **bit-determinism moat**. This is a LIVING
> document — when a slice teaches a new lesson, add it here (one crisp bullet + a pointer to the bench/memory).
> Durable one-fact scars live in agent memory (`~/.claude/.../memory/`); this doc is the human-readable index of the
> *playbook* those scars belong to. Read it before starting a crush; update it when you finish one.
>
> Cross-refs: `AGENTS.md` (full-crush policy), `docs/PRINCIPLES.md`, `docs/SANITY.md` (scar→rule→check),
> `docs/bench/README.md` (bench convention). Memory index: `MEMORY.md`.

---

## A. Benchmark methodology — fairness is non-negotiable

- **Bench ALL relevant peers, never cherry-pick.** scipy + MATLAB + Eigen + LAPACK + the domain library (Ceres,
  CoDiPack, TIDES, …), matched threading. One missing peer can be the one that beats you.
- **Fairness GATE: abort unless all peers compute the identical result.** Drive each peer via its leanest correct
  path; keep an *independent* oracle (analytic / high-precision). If peers disagree, the bench is wrong — fix it
  before trusting any number. (A v15-c mis-seed was caught only by luck before this became standard.)
- **Run YOUR method at its best, too.** Handicapping yourself is as dishonest as handicapping the peer: benchmarking
  Taylor at a fixed over-high order at loose tolerance made it "lose" until adaptive-order fixed it. Order matched to
  tolerance, optimal block size, right carrier width — each side at its optimum.
- **Matched ACCURACY, not matched nominal tolerance/iteration.** Peers over/under-deliver at the same `tol`; compare
  time at equal *achieved* error (the error-vs-time frontier), or equal true residual.
- **Representative workload, chosen honestly.** Pick the regime the method is *for* (batched-across-points for forward
  AD; high precision for Taylor ODE) — not to dodge a loss. State the regime; don't imply universality.
- **Report EVERY metric.** "More accurate at every tolerance AND faster where it counts" is an honest crush. A
  partial-metric victory is not a victory (see §B).
- **Measurement hygiene.** Median of many reps, pinned core (`taskset -c`), warmup iterations, a `keep()`/`asm
  volatile` DCE barrier, `-O3 -march=native`, the determinism flags. WSL perf counters are unavailable → `objdump` /
  drive from PowerShell.

## B. When you "lose" — triage honestly, never document-and-accept

A documented loss is an **OPEN BUG** (SANITY #9). Triage which kind:
1. **A real slow solution** → FIX IT. The gap is almost always algorithmic, not fundamental. Examples that flipped a
   loss to a crush: `O(K³)→O(K²)` (tape the op-graph), `O(n²) dense recovery → O(nnz) CSR`, factor-reuse instead of
   AD-through-the-loop, removing a per-step `pow`.
2. **A fundamental regime boundary** → STATE IT PLAINLY as tool-scope, not a defect. A high-order method genuinely
   can't beat a cheap low-order stepper at loose tolerance (few-digit work); say so, and show you still win on
   accuracy + on the high-precision frontier. This is honest scoping, not spin — but you must have ruled out (1)
   first, and you must not *frame* the peer's home turf as your loss.
3. **An unfair self-handicap** → FIX THE BENCH (see §A "your method at its best").
- **Scalar-tape AD beats a framework's dispatch at small-moderate n; a VECTORIZED-array peer wins at large n on a
  vectorizable functor — state the crossover, don't imply universality; and DIAGNOSE the wall before conceding.**
  v16-e HVP: Cerid's forward-over-reverse scalar tape **crushed torch functorch 32–640×** (its `jvp∘grad` per-call
  overhead ~700µs, n-independent) and **beat JAX 2.6–4.1× at n≤~700** (the Newton-CG opt regime), but JAX won at
  n=1024. Four correctness-preserving tape opts (AoS = 1 push/op, fused `sincos`, adjoint→compact SoA, add/sub fast
  nodes = unit-partial backward with no Dual multiply) only narrowed it **2.05×→1.44×** (29.9→21.7µs) — and node-shrink
  did NOTHING, which is the tell: **not memory- or FLOP-bound (FLOPs ≈ 5µs, 4× under the runtime) — it's the functor's
  SERIAL ACCUMULATOR** (`acc=acc+term`, a length-n dependency chain the scalar tape executes faithfully in order),
  which JAX hides by reordering the associative sum into a **SIMD tree-reduction**. **The fix that WON: stop fighting
  SIMD with a scalar tape — a tape of VECTOR ops** (`vhvp.hpp`: O(#ops) nodes not O(n); each an n-wide SoA DualVec
  elementwise loop that `-O3 -march=native` auto-vectorizes; reductions = a vectorizable sum + broadcast). That +
  **storing the transcendental's partial in the forward so the backward does NO sincos recompute** (a 2×-sincos → 1×
  lever, worth ~1.35× here) flipped n=1024 from **0.49× losing to 1.22× winning** vs JAX, and 6.8×/4.5× at n=64/256 —
  exact parity, deterministic. Lesson: when the peer's win is SIMD-vectorization of a reduction, the honest crush is to
  vectorize the *op graph* (one node per vector op), not to micro-opt the scalar tape; and cache transcendental
  partials — the backward's hidden 2× sincos is a common tape tax. (v16-e, `docs/bench/2026-07-06-v16e-hvp.md`.)
- **"Finite where the peer NaNs" claims must be EMPIRICALLY verified, not assumed.** The dossier claimed value-only
  svdvals/eigvals grads are "finite where JAX/torch NaN" — but a live probe showed modern JAX **and** torch `svdvals`
  grad is ALSO finite at repeated σ (they use a value-only rule too): PARITY, `Σ∇A=16.0` all three. The NaN appears
  ONLY in the full-SVD U/V grad path (`torch.linalg.svd(...).backward()`, F-matrix `1/(σ_i²−σ_j²)`). So frame it as "we
  ship ONLY the robust value-only driver (finite by construction, at parity with the peers' value-only path, avoiding
  the NaN-prone F-matrix path)" — NOT "finite where they NaN on the same op" (a metric-mismatch). Run the probe; report
  what the peer actually does. (v16-d.)
- **Correctness before speed, always.** NEVER ship a faster path that is wrong or silently misses tolerance. A
  ratio-based pow-free Taylor step was ~2× faster but over-stepped oscillatory problems → REJECTED. A passing
  self-test is not proof; test the failure mode the shortcut would break.

## C. Algorithmic crush levers (the order-of-magnitude wins)

- **Factor-reuse — differentiate the SOLUTION, not the factorization loop.** Given a stored `L`/`LU`/`QR`, a
  derivative is one back-solve `O(n²)`; AD-through re-factorizes `O(n³)` per direction. (v15-f: 3.4×→19.3× and
  growing with n.) Giles/Murray/Townsend JVP table.
- **Tape / stage the op-graph.** Order-by-order propagation over a recorded graph is `O(K²)`; re-evaluating the whole
  functor per order is `O(K³)`; nesting first-order AD is `O(2^K)`. (v15-g Taylor tape: 2.1×→10× at 1e-12.)
- **Value-only degeneracy robustness.** Trace/diagonal tangents (`logdet`, `dλ`, `dσ`) never divide by `(λ_i−λ_j)`
  or `σ` → finite and exact at repeated/zero spectra where JAX/PyTorch (eigenvector F-matrix) return NaN. Ship the
  value-only driver; the vector-derivative F-matrix path is opt-in.
- **Batched-across-points SIMD is where forward AD crushes.** Pack N points across `Vec4d` lanes; single-point
  scalar AD loses to a peer's AVX2 2-register optimum, batched wins big. Choose the representative regime accordingly.
- **Sparsity: trace → color → compressed recovery.** Bitset index-set tracer, distance-2/star coloring, CSR recovery
  `O(nnz)` not `O(n²)`. The recovery layout is where a weak 2× becomes a 13×.
- **Same-algorithm crush.** Beat the peer running the SAME algorithm (matvec count, flop count) — an algorithmic edge
  (fewer matvecs, symbolic reuse) is durable; a micro-opt edge evaporates on the peer's next release.
- **A reverse NN pass is ALL GEMMs — ride the production kernel, not the reference.** The matmul VJP is an einsum with
  a permuted spec (`ḡA=ḡC·Bᵀ`, `ḡB=Aᵀ·ḡC`), so forward + backward of an MLP/CNN are GEMMs (+ cheap elementwise). The
  self-contained *scalar* reference GEMM (the win-debug gate) self-handicaps ~10× — route the crush path through the
  production register-tiled GEMM (`hesap-dense::gemm` with the transpose flags for the two VJP products; serial to match
  `torch.set_num_threads(1)`). v16-c: 568µs→55µs MLP flipped a 7× *loss* into a **1.6× win vs torch / 1.3× vs JAX**
  (2.7×/1.7× on the CNN), at EXACT f64 value+grad parity + the determinism moat. Bench the PRODUCTION path, state the
  reference/production split. (`docs/bench/2026-07-06-v16c-nn-vjp.md`.)
- **Factor-reuse VJP + native beats a framework AD even at MATCHED flops.** A `solve` VJP that reuses the stored LU
  (`b̄=A⁻ᵀx̄` one back-solve, `Ā=−b̄xᵀ`) does the same work as JAX's Giles rule — yet Cerid's value+grad was **3.77×
  faster than JAX** (5.6µs vs 21µs, n=32, 1T f64) purely from native compiled code + zero framework dispatch + the
  deterministic LU. When the rule math is matched, the win is native + no-framework + determinism; measure it, claim it.
  (v16-d, `docs/bench/2026-07-06-v16d-matrix-suite.md`.)
- **Match the peer's OPTIMIZER, and reuse structure the peer exploits (Efficient-KAN).** v16-k(2): a KAN's B-spline
  basis `B_g(x_i)` depends only on the INPUT i, not the output j — so compute it ONCE per input and make the layer two
  matmuls, not a per-edge loop: **8.5–10.6× over the naive per-edge form at BIT-IDENTICAL output** (interleave the sum
  per-input so it matches the naive order bit-for-bit). vs efficient-kan (PyTorch): native gave **31× on wall-clock**
  — but a real FIT LOSS first: plain SGD stalled at loss 0.56 while efficient-kan's **Adam** fit to ~0. Adding a
  deterministic Adam closed it to 4.8e-5 (fit parity). Lesson: when a peer fits far better, suspect the OPTIMIZER
  before the model — a correct gradient (Taylor/FD-gated) + a weak optimizer looks like a modelling loss; match Adam.
  And record the SGD gap — it was real. (`docs/bench/2026-07-07-v16k-kan.md`.)
- **For FEA/topopt the solver choice IS the crush — a banded direct solve beats matrix-free CG by 22×.** v16-j
  (SIMP topology opt vs MATLAB top88): the first cut used a matrix-free Jacobi-PCG solve — Taylor-remainder-gated
  CORRECT and compliance-matched, but **2.7–7.5× SLOWER than top88** because SIMP's `Emin=1e-9` void elements make the
  stiffness K κ~1e9-conditioned ⇒ CG is iteration-bound (warm-start + looser tol barely helped — condition, not the
  initial guess, is the wall). The fix: the FEA K is **symmetric-banded** (half-bandwidth `2·nely+5` under a
  column-major node numbering), so a **banded Cholesky** is EXACT, condition-INDEPENDENT, and cache-friendly
  (contiguous band) — a **22× speedup** that flipped the loss to an **8.1× win** over MATLAB's general sparse solver.
  Lessons: (a) never fight an ill-conditioned system with unpreconditioned CG when the structure admits a direct
  solve; exploit the bandwidth. (b) The **Taylor-remainder test** (r(ε)=|J(ρ+εδ)−J(ρ)−ε∇J·δ| halves→quarters,
  2nd-order) is the gold-standard adjoint-gradient gate — it proves the gradient EXACT independent of the physics, so
  a correct-but-slow first cut is still a valid checkpoint to optimize from. (c) Record the loss-before-the-fix — the
  CG detour was real. (`docs/bench/2026-07-07-v16j-topopt.md`.)
- **Codegen AD beats interpreted-tape AD — and `-ffp-contract=off` is what makes the codegen deterministic.**
  v16-h: trace a functor to an expression DAG, reverse-AD it SYMBOLICALLY (the gradient is new graph nodes), CSE/DCE,
  then emit a straight-line C++ kernel + compile it. That codegen'd kernel is **3.7× faster than the interpreted
  reverse tape** (no per-node dispatch, no per-call tape rebuild, no dynamic alloc) and **13.7× faster than JAX jit/XLA**
  (native vs a jit runtime) at value+grad parity — the Enzyme/Tapenade source-transform win as PORTABLE C++ (no LLVM
  plugin). **The trap:** the codegen was NOT bit-identical to the interpreter until `-ffp-contract=off` — at
  `-O3 -march=native` the straight-line `n5=a*b; n6=n5+c;` fuses into an FMA, but the array-based interpreter
  (`vals[i]=a*b` round-tripped through memory) does NOT ⇒ a 1-ULP drift. Any time you compare a compiled straight-line
  kernel to a memory-round-tripped interpreter (or claim deterministic generated code), ban FMA contraction — it is
  BOTH the bit-identicality fix AND the determinism-moat policy for generated code. And gate codegen==interpret
  bit-identical, not just codegen≈FD. (`docs/bench/2026-07-07-v16h-graph-codegen.md`.)
- **Implicit differentiation: differentiate the SOLUTION (IFT), never unroll the solver — and own the dead lanes.**
  v16-g: for a root F(x*,θ)=0 / fixed point / argmin, the gradient is `θ̄=−(∂F/∂θ)ᵀ(∂F/∂x)⁻ᵀx̄` (a single linear solve
  reusing the solver's factor) — O(1) backward, INDEPENDENT of the solver's iteration count, deterministic. Native C++
  crushed the frameworks by their per-call overhead: **jaxopt 214×** (105ns vs 22.5µs, 10-digit parity), **cvxpylayers
  ~4900×** (250ns vs 1.23ms). Two extra levers: (a) the peer's approximate solver hides its own error — cvxpylayers/SCS
  matched only to its LOOSE default tolerance until tightened to 1e-10, whereas Cerid's KKT solve is EXACT (gated ≡FD);
  always tighten the peer's tolerance before claiming a parity gap is real. (b) the approximate conic solver **FAILS**
  where the exact KKT solve doesn't — SCS returned "infeasible" on a feasible nq=20 QP Cerid solved in 1.4µs (a
  robustness+capability win, not just speed). And when the reference library is UNMAINTAINED (jaxopt), owning the lane
  in deterministic C++ IS the crush — verify with a self-authored IFT reference + FD, don't skip it for lack of a
  living peer. MATLAB N/A here (`quadprog`/`fsolve` are forward-only, no diff-opt layer) — state the reason.
  (`docs/bench/2026-07-07-v16g-implicit-diff.md`.)
- **Differentiate the DISCRETE solver (DTO), not the continuous ODE (CTO) — exact + consistent beats memory-cheap.**
  v16-f: discretize-then-optimize (AD *through* the RK4 integrator, revolve-checkpointed) gives the EXACT gradient of
  the discrete forward (≡ FD, matched torchdiffeq `odeint` to 10 digits) at **O(log T) memory** (revolve) —
  torchdiffeq forces a choice its exact path (`odeint`) is O(T) memory, its memory-cheap path (`odeint_adjoint` =
  continuous adjoint) can be INCONSISTENT (the adjoint ODE's own discretisation ≠ the transpose of the forward solver,
  arXiv:2306.02192). Ship DTO as the default; ship CTO with the caveat stated loud. And a framework that calls the RHS
  per RK stage in Python is **600–780× slower** than native compiled — a per-step-interpreter crush. Build the
  optimal checkpoint schedule from a **memoized DP over the treeverse cost** (`cost(len,s)=min_d[d+cost(len−d,s−1)+
  cost(d,s)]`, s=0/len>1 infeasible) — GW-optimal by construction, no closed-form-mid to recall. (`docs/bench/2026-07-07-v16f-ode-adjoint.md`.)
- **Bidirectional (row+column) coloring collapses arrowhead Jacobians.** A dense row defeats column coloring (ncol=n),
  a dense column defeats row coloring (nrow=m); recover dense ROWS by reverse (tape VJP) sweeps + the sparse remainder
  by forward (JVP) sweeps → total = ncol+nrow = O(1) on bordered systems (v16-c: 17→3, growing with n). Prefer forward
  on ties (JVP sweeps are cheaper than building+replaying the tape).

## D. Low-level performance (SIMD / memory / codegen)

- **Profile before guessing — counters first.** 9 rounds of guessing lost to 2 VTune measurements. DTLB / port
  utilization / cache-miss counters point at the real wall.
- **Memory wall = TWO signals** (no parallel scaling AND fma-swap shows 0%); otherwise keep chasing compute.
- **AoSoA beats SoA outside L1** — interleaved shuffles vs planar streams; `[L×re | L×im]` rows = zero shuffles + one
  stream per row. Convert at the boundary exactly twice.
- **SROA + FMA-operand order in the SIMD carrier.** Carried partial = the multiplicand; named-register packs +
  `[[no_unique_address]]`; keep partials in registers, not memory.
- **Complex split-array SIMD must be ≥8-wide unrolled** (FMA-port latency wall) — measure, don't assume 4 is enough.
- **Register-tiling needs packing** (tiling strided rows in place regresses); unblocked row-wise beats blocked for
  small-K reductions; block only N≫512.
- **Big lookup tables are TLB killers** — shrink the table before adding huge pages (huge pages trap power-of-2
  strides).
- **Single-rounded FMA can BEAT a peer** (fewer roundings) — the determinism moat and a speed win at once.

## E. The determinism moat — a crush axis, never traded

- Route ALL math through `crd::math::*` (guard: `check_no_std_transcendental.ps1`); real cores + `crd/math/complex.hpp`
  make complex math bit-identical too.
- `crd::math::simd::fma` (single-rounded), `-ffp-contract=off`, `/fp:precise`, `{1..16}`-worker bit-identity test.
- The moat is a *feature the peer lacks*: bit-identical on every platform where std/libm/MKL vary run-to-run. Sell it.

## F. Cross-config correctness traps (all real, all cost hours)

- **MSVC `/O2` miscompiles an FP conditional whose dead branch computes NaN** → go BRANCHLESS (the `pow_const`
  singularity). `no_unique`/`noinline`/`rt()` don't fix it; shipping-only.
- **MSVC autovec miscompiles per-lane `if(){a[q]=…;b[q]=…;}`** (wrong masked blends) → manual select chains; ASan is
  blind, `fprintf` suppresses it.
- **MSVC `/Od` straight-line generated kernel = 1.4 MB stack frame** → dual-body emission (SIMD under
  `NDEBUG||__OPTIMIZE__`, lane-scalar else).
- **MSVC LTCG + `__forceinline` generated codelets = C1002 link heap bomb** → demote to `inline` at the emitter;
  `noinline` seam on multi-instantiated drivers.
- **6-config DoD catches these**: win-{debug, asan, shipping, tidy} + clang-cl + gcc. A binary-direct MSVC-only run
  misses guards AND clang/gcc. Run ≥1 ctest + 1 clang-cl + 1 gcc before "done".
- **Test on RANDOM/oscillatory inputs, not smooth ones.** Eigensolvers on random matrices; step control on an
  oscillatory ODE (a near-zero trailing coefficient blows up a single-coefficient step estimate).
- **The Edit tool doesn't bump mtime for ninja** → `touch` the file before a Windows rebuild, or the change won't
  compile.
- **Generic-functor `using crd::math::exp; exp(z)` is AMBIGUOUS (C2668) for a plain `std::complex` arg** — it collides
  with ADL's `std::exp`/`std::conj` (both exact matches). Drop the `using`: ADL alone routes `forward::exp` for a dual
  carrier (deterministic `crd::math` inside) and `std::exp` for the plain-complex oracle. (v15-h complex AD.)

## G. Verification discipline (ship only when green on all oracles)

- **3-oracle rule:** `analytic ≡ complex-step (~1e-15) ≡ FD (~1e-6)`, one scalar-generic functor driving all three;
  verify in python/JAX BEFORE the C++.
- **Reconstruct-verify:** seed the tangents, reconstruct the Jacobian, assert the structural identity (CR for
  holomorphic, symmetry for Hessians).
- **Know each oracle's limit:** complex-step can't validate Wirtinger rules (it co-opts the imaginary axis) → use a
  2×2-real-FD + hyper-dual; SVD/eig of a complex matrix isn't analytic → route complex-step through the `σ²=eig(AᵀA)`
  surrogate.
- **Adversarial second check before the headline** (a second matrix / a refutation pass); a measurement lever needs a
  second-matrix confirmation before you publish the number.

---

### How to use / update this doc
- **Before a crush:** skim §A–§C for the levers, §F for the config traps you'll hit.
- **After a crush:** add the new lever/trap here (one bullet, a bench pointer), and bank the one-fact scar to memory.
  Keep bullets crisp — this is an index, not a narrative. Detailed numbers live in `docs/bench/`, detailed scars in
  memory.
