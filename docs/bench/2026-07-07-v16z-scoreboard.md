# 2026-07-07 — v16 reverse-mode AD + differentiable-solvers cluster — FULL CRUSH SCOREBOARD (close)

The consolidated verdict for the v16 reverse-mode automatic-differentiation cluster (`crd-hesap-autodiff`, slices
a–k). Per-slice detail + raw numbers live in the linked boards; this is the one-screen scoreboard. Peers named in the
plan: **PyTorch autograd**, **JAX** (`grad`/`vjp`/`hvp`), **Stan Math**, **Adept**, **CoDiPack**, **torchdiffeq**,
**jaxopt** (implicit, unmaintained), **cvxpylayers**, **dolfin-adjoint / top88** (adjoint-PDE), **efficient-kan**.
**Enzyme named OUT-OF-SCOPE** (LLVM-plugin AD, MSVC-incompatible — portability cornerstone). Machine: i9-14900K,
pinned core, `-O3 -march=native` (Cerid) vs single-thread f64 peers, determinism flags on. Every measured comparison
is fairness-gated (all methods compute the identical result at matched accuracy; abort on disagreement) and run at
each method's best.

## The scoreboard

| slice | op / regime | peer(s) | metric | **verdict** | board |
|---|---|---|---|---|---|
| **a** | reverse tape — full ∇f in ONE backward pass | forward-SIMD; finite diff | ns, matched ∇f | **CRUSH: O(n) vs O(n²)** — 5.2× vs forward-SIMD, 22.2× vs FD @ n=1024 (growing); forward wins small-n = its regime | `…v16a-reverse-tape.md` |
| **b** | scalar VJP rule surface (the whole `crd::math`) | finite diff; the v15 forward JVPs | gradient exactness | **MACHINE-EXACT 2.2e-16 in one pass** vs FD's best ~3e-9; VJP = transpose of the v15 JVP (one rule library) | `…v16b-reverse-rules.md` |
| **c** | tensor / einsum / NN-op VJPs (+ bicoloring, sparse reverse) | PyTorch 2.12, JAX 0.10, ColPack | value+grad parity, µs | **MLP 1.59×/1.27×, CNN 2.69×/1.67× vs torch/JAX** (rides the v14 GEMM) + **sparse-autodiff capability crush** (torch/TF lack it) + bicoloring ties ColPack | `…v16c-nn-vjp.md`, `…v16c-bicolor-sparse.md` |
| **d** | matrix-calculus VJPs (Giles/Seeger, factor-reuse) | JAX | value+grad parity, ns | **JAX PARITY + solve value+grad 3.77× FASTER** (5.6µs vs 21.1µs); FFT-VJP = IFFT bit-parity; degeneracy-robust (finite where value-only peers are) | `…v16d-matrix-suite.md` |
| **e** | higher-order — forward-over-reverse HVP (+ vectorized) | JAX `hvp`, PyTorch functorch | value+H·v parity, ns | **crushes functorch 32–640× at ALL n; beats JAX 2.6–4.1× (opt regime)**; the vectorized `vhvp` beats JAX at EVERY n (1.2–6.8×) | `…v16e-hvp.md` |
| **f** | revolve checkpointing + ODE-adjoint (DTO/CTO) | torchdiffeq 0.2.5 (`rk4`) | value+grad parity, µs; memory | **torchdiffeq PARITY + 607–777× FASTER + EXACT *and* O(log T) memory in ONE path** (torchdiffeq forces exact-O(T) XOR inexact-O(1)) | `…v16f-ode-adjoint.md` |
| **g** | ★★ implicit-diff through solvers (IFT: root / fixed-point / QP-KKT) | jaxopt (dead), cvxpylayers | value+grad parity, ns; robustness | **jaxopt PARITY + 214× / cvxpylayers PARITY + ~4900×; solves the nq=20 QP exactly where cvxpylayers/SCS returns "infeasible"** — owns the C++ implicit lane | `…v16g-implicit-diff.md` |
| **h** | structural graph AD + tape→C++ codegen | JAX jit/XLA; the interpreted tape | value+grad parity, ns | **JAX PARITY (14-digit) + codegen 3.7× vs the tape + 13.7× vs JAX-jit**; codegen BIT-IDENTICAL to the interpreter (`-ffp-contract=off`); portable C++ (no LLVM plugin) | `…v16h-graph-codegen.md` |
| **i** | ★★ the deterministic-training moat | PyTorch-CPU | bit-reproducibility guarantee | **`batch_gradient` bit-identical for EVERY worker count {1..16}; a whole training run replays BIT-FOR-BIT** — torch is thread-count-nondeterministic (no guarantee); certifiable (DO-178C/ISO-26262) | `…v16i-determinism-moat.md` |
| **j** | ★ adjoint topology optimization (SIMP FEA) | MATLAB top88 (Andreassen 2011), dolfin-adjoint | compliance parity, ms | **top88 COMPLIANCE PARITY (5 sig figs) + 8.1× / 2.3× FASTER** (banded-Cholesky adjoint); passes the dolfin-adjoint Taylor-remainder test | `…v16j-topopt.md` |
| **k** | ★ neural-ODE + KAN showcase | torchdiffeq; efficient-kan (PyTorch) | fit parity, ms | **neural-ODE: torchdiffeq loss-parity + 4.9× faster; KAN: restructuring 8.5–10.6× over naive + efficient-kan fit-parity + 31× faster** | `…v16k-neural-ode.md`, `…v16k-kan.md` |

## The moat (present in every slice)

- **Bit-identical `{1..16}`-worker gradients** — `batch_gradient` folds per-sample tapes in FIXED ascending sample
  order (never atomic scatter-add) ⇒ the batched gradient, and any training run over it, is bit-reproducible across
  worker count, gated exact `==` for 1..16 (v16-i). PyTorch/JAX cannot: atomic scatter-add. **Verified cross-compiler**
  (MSVC + gcc) in the DoD sweep below.
- **Deterministic codegen** — the tape→C++ emitter bans FMA-fusion drift (`-ffp-contract=off`), so generated cells are
  bit-identical to the interpreter and to each other (v16-h).
- **Allocation-free drivers** (caller-owned scratch), `crd::math` throughout (no `std::` transcendental), factor-reuse
  everywhere (matrix + implicit VJPs never AD *through* a factorization).

## The close DoD (6-config sweep + guards, 2026-07-07)

Autodiff cluster (`crd-hesap-autodiff-tests` 3488 asserts/124 cases incl. the reverse CLI + `crd-hesap-tensor-einsum
-tests` 6465/5) green across **6 configs, 2 compilers**: MSVC {win-debug /Od, win-asan, win-shipping /O2, win-release
/O2+LTCG} + gcc {linux-gcc-release -O3, linux-gcc-asan}. The `{1..16}` determinism moat is **bit-identical on both
MSVC and gcc**. All 7 ctest guards (no-std containers/math/sort/transcendental · ASCII names · tagged-numerics ·
no-malloc) + win-tidy clean.

## The agent CLI (`hesap.ad.*`, reverse + implicit — v16-z)

Reverse-mode + implicit derivatives on canned callables (the data-vs-callable split, like `hesap.ode.*`), gated ≡
analytic in `test_ad_cli.cpp`:
- **`hesap.ad.rgradient.f64`** — ∇f in ONE reverse pass (Rosenbrock/sphere/cubes/Σexp, 2 ≤ n ≤ 256).
- **`hesap.ad.jacobian.f64`** — reverse Jacobian of a coupled map (graph once, backward per row).
- **`hesap.ad.hvp.f64`** — forward-over-reverse H·v (grad + H·v in one pass, never forms H).
- **`hesap.ad.implicit.f64`** — the IFT VJP through a canned root (`x*=√θ`, `dL/dθ` without unrolling a solver).

## Honest notes (full scoreboard, no partial-metric spin)

- **Small-n reverse loses to forward** (v16-a) — reverse mode is O(n)-per-scalar-output but has per-op tape overhead;
  below n≈50 the SIMD forward driver wins. Reported as the honest regime split, not hidden.
- **CTO ODE-adjoint is the caveated path** (v16-f) — the continuous adjoint is O(state) memory but inconsistent
  (`|CTO−FD|≈2e-5`); DTO (AD-through-integrator, exact, `<1e-6`) is the DEFAULT. Both shipped; the caveat is on the
  board.
- **Value-only SVD/eig VJPs** (v16-d) — Cerid ships the robust value-only svdvals/eigvals drivers (finite at repeated
  spectra, PARITY with JAX/torch value-only); the full-SVD U/V path NaNs at repeated σ (torch confirmed) and is not
  shipped — a policy choice, framed honestly.
- **The topopt first cut was a LOSS** (v16-j) — matrix-free Jacobi-PCG was Taylor-gated-correct but 2.7–7.5× slower on
  the ill-conditioned SIMP K; the banded direct solve (22× speedup) flipped it to a win. Recorded.
- **The KAN first cut was a LOSS** (v16-k) — plain SGD stalled at loss 0.56 vs efficient-kan's Adam; a deterministic
  Adam closed it to 4.8e-5 (fit parity). Recorded — the lesson (match the optimizer) is banked in the crush-playbook.

## Verdict

**v16 reverse-mode + differentiable-solvers cluster: FULL CRUSH.** Every peer beaten at matched accuracy on its own
regime — PyTorch/JAX on NN + matrix + HVP, torchdiffeq on ODE-adjoint (607–777×), jaxopt/cvxpylayers on implicit-diff
(214×/4900× + solves what SCS can't), JAX-jit on codegen (13.7×), top88 on adjoint topopt (8.1×), efficient-kan on the
KAN (31×) — **plus the determinism/certification column no AD framework carries** (bit-identical `{1..16}` gradients +
bit-reproducible training + deterministic codegen). Losses that appeared mid-slice (CG topopt, SGD KAN) were solved
into wins, never documented-and-accepted.
