# Research — 2026-07-06 — v15 forward-mode AD: the frontier crush levers + reconstruct-verify tables

> Deep-research dossier standing up v15 (`crd-hesap-autodiff`, forward mode). Captured
> BEFORE code so the crush levers, the honesty caveats, and the ready-to-code formula
> tables are durable (not lost in a session). Spec = `docs/phases/phase-3.1.6-v15.md`;
> architecture = ADR-0097. This is the implementation reference for slices a–h.
>
> Sourcing note: assembled from four primary-source research passes (peer source in
> `external/` + the 2025-26 literature). Formula tables were verified against the cited
> primary sources, but two items carry an explicit **VERIFY-BEFORE-CODING** gate (marked ⚠)
> — do not hardcode them without the char-for-char recheck.

## Question

For each v15 forward-AD slice, what do the gold-standard peers (Ceres Jets · autodiff.hpp ·
CoDiPack · Sacado · Adept · JAX/PyTorch · ColPack + Julia ASD · TaylorDiff/TIDES) actually
do, where is their ceiling, and what is Cerid's *concrete, honest* crush lever — so v15 is
crushingly performant **by design** and we never claim a win we don't have?

## TL;DR

- **The plan is frontier-correct and crushable.** Every slice has a concrete lever. Two are
  near-uncontested C++ slots: **v15-e** (the *only* C++ forward sparsity tracer — ColPack only
  colors, Julia traces but on a GC'd runtime) and **v15-g** (Taylor-mode + a Taylor ODE
  integrator hesap-ode never had).
- **The core moat is determinism + suite-integration + factor-reuse + no-alloc/WCET**, not
  raw rule math. On the *linear-algebra JVP rules themselves (v15-f) we MATCH JAX/PyTorch* —
  they already ship the canonical Giles/Murray/Townsend differentials. Say so; crush them on
  determinism/alloc/suite-integration/degeneracy, and crush the *Jet libraries* on
  factor-reuse flops (they AD-through-the-factorization-loop; we reuse the stored factor).
- **Two clean scope decisions fell out:** don't build the FFT O(K log K) series multiply
  (wrong regime + numerically worse for decaying coefficients — keep direct O(K²)); and pin
  the Wirtinger convention to the **un-conjugated pushforward** (JAX `jvp`), leaving the
  ℂ→ℝ grad-conjugation to v16.

## Recommendation for Cerid

### Per-slice crush levers (the honest scoreboard we're aiming at)

| Slice | Gold standard | Our concrete lever | Honest verdict |
|---|---|---|---|
| **a** substrate | Ceres `Jet<T,N>` (`T a` + `Eigen::Matrix<T,N,1> v`) | plain `Dual{v,d}` + `Jet<T,N>` with **no Eigen dep**, no forced alignment, alloc-free, deterministic | clean win on portability/compile/alloc; math parity |
| **b** rule library | autodiff.hpp / CoDiPack cmath | full `crd::math` surface + the **3-oracle gate**; recurrences generated once and reused by g | parity + a stronger correctness contract |
| **c** hyper-dual | Fike-Alonso; nested `Dual<Dual<T>>` | exact 2nd-deriv, no step cancellation; wired into opt Newton/TR | capability win (exact Hessian into the solver) |
| **d** SIMD vector-forward | Ceres N-partials via Eigen SIMD; dco vector-mode | `Vec8f/Vec4d` **deterministic lane layout** (`{1..16}` bit-identical) + **runtime-k tiling** for large gradients, no Eigen | the determinism moat + ~4–8× |
| **e** sparsity trace+color | ColPack (colors a *given* pattern); Julia SCT+SMC (traces+colors, GC'd) | **the only C++ forward tracer**: bitset index-sets, alloc-free, deterministic, WCET-bounded; Θ(k²n) vs Θ(kn²) | **empty C++ slot** — up to 3 orders of magnitude on real problems |
| **f** matrix-calculus JVPs | Giles/Murray/Townsend; JAX/PyTorch (same rules) | **factor-reuse** (never AD-through-loop) + value-only degeneracy-robust drivers | ⚠ *match* JAX/PyTorch on rule math; crush the Jet libs on flops + everyone on determinism/alloc/degeneracy |
| **g** Taylor-mode + Taylor ODE | TaylorDiff.jl / TIDES / jax.jet | compile-time-K unrolled jets; O(K²) ≫ nested O(2^K); Taylor ODE integrator | new crush axis on **smooth + tight-tol** only (bench the crossover) |
| **h** Wirtinger | JAX complex `jvp` | un-conjugated holomorphic dual `Dual<Complex<T>>`; CR + ∂/∂z̄=0 gate | capability win (holomorphic sensitivities through FFT/DSP) |

### The honesty caveats we MUST bake into the boards (per the full-honest-eval rule)

1. **v15-f: we match, not crush, JAX/PyTorch on the linalg JVP *math*.** Their `lax.linalg` /
   `torch.linalg` forward rules are the same Giles/Murray/Townsend differentials we'll ship.
   The crush axes to state explicitly: (a) **factor-reuse flops vs the Jet/OO libraries**
   (Ceres/autodiff.hpp/CoDiPack/Sacado/Adept push duals through *every* scalar op of the
   factorization → a full O(n³) re-factorization in dual arithmetic × k directions; ours is
   one extra fwd/back-substitution against the stored factor); (b) **determinism** (`{1..16}`
   bit-identical tangents; their batched-matmul reduction order is unpinned); (c)
   **alloc-free / status-not-exception** (they malloc + throw); (d) **suite integration** (the
   same `crd-hesap-direct` factor object feeds the JVP, zero marshalling); (e) **degeneracy
   policy** (below).
2. **v15-g Taylor ODE wins ONLY on smooth RHS + tight tolerance.** Order p ~ −½·ln(ε), so as
   ε→0 it needs fewer/bigger steps than DOP853; but it LOSES on non-smooth/`abs`/`min-max`,
   loose-tol (rtol 1e-3…1e-6 — EYLEM's real-time-physics regime), stiff, and black-box
   (point-sampled) RHS. The gate is the **work–precision crossover** vs DOP853/RK45, measured
   against a high-precision reference orbit — the honest crossover *is* the headline.
3. **Degeneracy is a first-class capability, not a NaN.** At repeated eigen/singular values,
   `F_ij = 1/(λ_j−λ_i)` and `1/(σ_j²−σ_i²)` diverge; JAX/PyTorch emit NaN in `dQ/dU/dV`
   (verified from source — no ε guard on repeated values; JAX guards only exactly-zero σ). We
   ship **value-only drivers** (`logdet`/`eigvals`/`svdvals` — trace/diagonal only, no `F`,
   always finite) + **invariant-subspace-projector sensitivity** as *supported* paths. Any
   `F`-damping (Tikhonov/Lorentzian) = a **biased** derivative → a documented `Dxxx`
   paper-divergence, never silent.

### Scope decisions (lock these to save effort / avoid traps)

- **Do NOT build the FFT-based O(K log K) Taylor series multiply.** ODE runs K≈20–30, jets
  K≤~10 — the schoolbook-O(K²) crossover is K≳100s; and FFT convolution is numerically poor
  for decaying coefficients (the `exp` / `1/k!` case). Keep the direct O(K²) Cauchy product.
- **Wirtinger = un-conjugated pushforward** (JAX `jvp` convention: tangent carried without
  conjugation). The conjugation in `grad` is a reverse-mode/cotangent convention for ℂ→ℝ —
  it belongs to **v16**, not here. `holomorphic=True` becomes a debug-mode Cauchy-Riemann
  check, not a different math path.
- **`Jet<T,N>` uses a C-array / `crd::Array` of partials, NOT Eigen.** Ceres' Eigen backing
  is its portability + compile-time anchor; dropping it is a lever, not a regression.

---

## Reference payload (the ready-to-code tables)

### A. v15-a/c/d — core representation, hyper-dual, SIMD vector-forward

- **Ceres `Jet<T,N>`** (`external/ceres-solver/include/ceres/jet.h`): `T a; Eigen::Matrix<T,N,1> v;`.
  Chain rule for a transcendental g: `g(a+u) = g(a) + g'(a)·u` ⇒ `Jet(g(a), g'(a) * x.v)` — the
  scalar derivative scales the whole partial vector (vectorized by Eigen). Comparisons act on
  the **value** `a` (SFINAE via `std::common_type`). Division uses `v·v=0`:
  `(a+u)/(b+v) = a/b + (u − (a/b)v)/b`. Requires `EIGEN_MAKE_ALIGNED_OPERATOR_NEW`.
  → **This IS vector-forward mode**, but tied to Eigen + compile-time N + no determinism.
- **autodiff.hpp** (`external/autodiff/.../forward/dual/dual.hpp`): expression templates (Op
  tag structs `AddOp/MulOp/SinOp/…`, lazy graph evaluated on assignment). Buys no-temporaries;
  costs compile time, aliasing hazards, opaque errors. The frontier does not clearly favor ETs
  over a plain single-pass `Dual` for our driver-based sweeps.
- **Our substrate**: `Dual<T>{v,d}` (migrated v7-b, `engine/hesap-opt/.../opt/dual.hpp`) →
  `crd::hesap::autodiff::forward`. `Jet<T,N>` = Ceres-class small-N carrier, C-array partials.
  Comparisons value-based; `select`/`min`/`max`/`abs` carry the active branch's derivative
  with the documented sub-gradient at ties (abs' at 0 takes +1 by convention — as today).
- **v15-d vector-forward design**: separate two things Ceres conflates. `Jet<T,N>` = small
  compile-time N (few params). The **driver** tiles an arbitrary gradient into
  `Vec8f`/`Vec4d` lane-width chunks (k = 8 f32 / 4 f64 directions per pass), using
  `Vec8f::load_partial/store_partial` for the tail. **Lane layout is a fixed function of the
  direction index only** (never `num_workers`) ⇒ bit-identical `{1..16}`. Pin the chain-rule
  FMA: `mul_add` (two-rounding, ADR-0063 deterministic default) vs `fma` (single-rounded) —
  choose one and hold it for the bit-identity contract (both exist in `crd/math/simd`).
- **v15-c hyper-dual**: exact 2nd derivatives with no step cancellation (Fike-Alonso 2011,
  AIAA-2011-886). Use a **flat 4-component POD** `struct HyperDual { T f0,f1,f2,f12; }`
  (`x = f0 + f1·ε1 + f2·ε2 + f12·ε1ε2`, with ε1²=ε2²=(ε1ε2)²=0 but ε1ε2≠0) — more
  cache/SIMD-friendly than nested `Dual<Dual<T>>` (which is *algebraically identical* but the
  O(2^order) tree). Master unary rule: `f0=g(x0); f1=g'·x1; f2=g'·x2; f12=g'·x12 + g''·x1·x2`
  (branch on the real part `f0` only). Seed x_i in ε1, x_j in ε2 ⇒ one eval yields
  `f, ∂f/∂x_i, ∂f/∂x_j, ∂²f/∂x_i∂x_j` exactly (slots extracted by projection, not differencing
  — h-independent to machine precision; beats complex-step for 2nd order, which reintroduces
  real-part cancellation in `f''`). Feeds opt TR/Newton: **`vᵀHv` curvature = ONE nested pass**
  (seed v in both ε1 and ε2, read f12). Full symmetric Hessian = n(n+1)/2 passes, O(n²)·cost(f);
  the cheap *vector* HVP is forward-over-reverse → defer to v16-e, ship hyper-dual entries/`vᵀHv` now.
- **v15-d carrier + FMA discipline** (concrete): `struct DualPack4 { f64 v; Vec4d d; }` /
  `DualPack8 { f32 v; Vec8f d; }` (value + one packed tangent register). Chain-rule kernels
  (single-rounded): unary `r.d = Vec4d(fprime) * x.d`; product `r.d = fma(Vec4d(a.v), b.d, a.d*Vec4d(b.v))`
  — this is Adept's `a += m·g` packet kernel (its `MULTIPASS_SIZE=4` = Vec4d), run **tape-free**
  on the live eval. Strip-mine `ceil(n/K)` passes for a dense Jacobian (`store_partial` for the
  ragged block). ⚠ **The scalar-reference driver and the SIMD driver MUST share one rounding
  discipline** (both `fma` or both `mul_add`, per the `vec4d.hpp` contract) or the bit-identity
  gate fails for a non-bug reason — define the kernels once over a scalar-or-Vec abstraction.
  Realistic speedup ~2–4× (locality saturates ~32 directions; v15-e coloring compresses N to the
  chromatic number). `std::simd` is NOT ready (C++26 `std::datapar`, transcendentals unimplemented)
  — use crd-math `Vec4d/Vec8f`. Peers as existence proofs: Adept packet, CoDiPack `Direction`
  (plain C-array + scalar loops, autovec-only — we beat it with explicit `alignas(32)` Vec + fma).
- **⚠ Migration safety (v15-a, zero-regression):** migrate `Dual<T>` + its 9 transcendentals
  **VERBATIM** (same math, same conventions). Our incumbent `sqrt` (unguarded `1/(2√0)=inf`),
  `pow` (naive — no zero/negative handling vs Ceres' 9-case `fpclassify`-branched NaN factory),
  and `abs` (+1 subgradient at 0 vs Ceres' `copysign`) are known hardening targets — but
  **hardening happens in v15-b (the rule library), NOT during the migration**: shifting a
  convention now would change `test_derivatives.cpp` behavior and break the gate. The transcendental
  callers are QUALIFIED (`opt::sin(D{...})`, …, `opt::pow`, `opt::abs` in `test_derivatives.cpp`),
  so the re-export shim must `using`-declare every named function into `namespace opt` (ADL does
  NOT satisfy a qualified `opt::sin`); operators propagate via ADL and need no re-export.
- **min/max/select tie convention** (v15-a): the phase-doc contract is "active branch's derivative
  + documented sub-gradient at ties." Ceres instead **averages** the two Jets on equality
  (order-independent, matches JAX/TF `reduce_max`). Pick one and document it; averaging is the more
  defensible sub-gradient but "active branch" is the literal contract — decide + record at implementation.

### B. v15-e — sparsity tracer + coloring + recovery

- **Tracer** = operator-overload pass carrying an **index set** (not a value). Seed input i
  with singleton `{i}`; `+`,`*` return `union(a,b)`; zero-derivative ops (`sign`,`floor`,
  comparisons) return `∅`; unary nonlinear copies. One abstract eval ⇒ each output's set = its
  Jacobian nonzero columns.
- **Set representation** (the core C++ choice): **fixed-width bitset** `span<uint64_t>` over
  ⌈N/64⌉ words from the caller arena — union = word-wise OR, branch-free, O(N/64), zero alloc,
  WCET-bounded, **deterministic** (unlike ColPack/Julia hash-set iteration). Fallback:
  **sorted index run** (`span<uint32_t>`, linear-merge union) for very large, very sparse N.
  Make it a policy template `SparsityTracer<SetPolicy>`.
- **Jacobian propagation** for `z = φ(x,y)` with derivative flags d₁,d₂:
  `grad(z) = (d₁ ? grad(x) : ∅) ∪ (d₂ ? grad(y) : ∅)`. Encode flags as a per-op compile-time
  trait; each overload body = one conditional-union.
- ⚠ **Hessian propagation (VERIFY-BEFORE-CODING against arXiv:2501.17737 §2nd-order — the
  transcription below is from the HTML render).** Carry a second set of interaction pairs
  `{(i,j) | ∂²z/∂xᵢ∂xⱼ ≠ 0}`; propagation through `z=φ(x,y)` depends on five scalar flags
  `[∂₁φ],[∂₂φ],[∂²₁φ],[∂²₂φ],[∂²₁₂φ]`:
  ```
  grad(z) = [∂₁φ]·grad(x) ∨ [∂₂φ]·grad(y)
  hess(z) = [∂₁φ]·hess(x) ∨ [∂₂φ]·hess(y)                 (inherited)
          ∨ [∂²₁φ]·(grad(x)⊗grad(x))                        (self, x)
          ∨ [∂²₂φ]·(grad(y)⊗grad(y))                        (self, y)
          ∨ [∂²₁₂φ]·(grad(x)⊗grad(y) ∨ grad(y)⊗grad(x))     (cross)
  ```
  `x*y` → cross only; `sin(x)`/`x²`/`exp(x)` → self; `x+y` → none (inherited only). Store
  pairs canonicalized `i≤j` (upper triangle) to avoid double-count. Watch the dense-row
  blow-up (`sum`/`norm`/`softmax` → O(N²) pairs — detect "fully-connected" and special-case).
- **Global vs local**: global (bare tracer) = conservative superset valid ∀x, **cacheable**;
  local (tracer + a live primal double so branches/`min`/`max`/`abs` resolve on the value) =
  sparser but **point-dependent, never cache across inputs** (a previously-dead entry going
  live is a silent wrong Jacobian).
- **Coloring**: Jacobian → **partial distance-2** coloring of the bipartite graph (≡ structural
  orthogonality of columns, Gebremedhin-Manne-Pothen 2005); symmetric Hessian direct →
  **star** coloring; Hessian substitution → **acyclic** (fewer colors, triangular solves).
  Greedy + orderings LF / SL(smallest-last) / ID(incidence-degree). **Validity is a hard gate**
  (no two structurally-adjacent columns share a color).
- **Compressed recovery**: seed `S` (n×c, one column per color, `S[i,color(i)]=1`); compressed
  `B = J·S` via c forward-AD tangent sweeps (drive with v15-d vector-forward!); **direct (CPR)
  recovery** `J[i,j] = B[i, color(j)]` (O(nnz) scatter, valid because a color class is
  structurally orthogonal). Hessian symmetry lets a lost `H_ij` be recovered from `H_ji`.
- **Correctness gate**: traced global pattern ⊇ union of numerical nonzeros over K random
  inputs; == the known analytic pattern for tridiag/banded/arrowhead/Brusselator/Bratu; local
  == dense numerical nonzeros at that x. Discriminating corpus test = **structural-vs-numerical
  zeros** (`x*0`, `x-x`, `sin(0*x)`, `x^0` — global keeps, local drops) + branches + `min/max/abs`
  + Hessian interaction battery (`x*y` cross, `x²`/`sin` self, `x+y` none, `norm(x)` dense).

### C. v15-f — matrix-calculus forward differentials (JVPs)

Notation: `dA` = input tangent; `dY` = output tangent; every rule reuses **stored factors**,
never re-derives them. Verified vs Giles NA-08/01, Murray arXiv:1602.07527, Townsend QR, and
JAX `jax/_src/lax/linalg.py` (source of truth for the ready-to-code form).

| Op | Forward differential (reuses stored factor) |
|---|---|
| `C=A+B` | `dC = dA+dB` |
| `C=A·B` | `dC = dA·B + A·dB` (bilinear ⇒ product rule) |
| `C=A⁻¹` | `dC = −A⁻¹·dA·A⁻¹` (two factor-solves; never differentiate the inversion) |
| `det/logdet` | `d(logdet) = Tr(A⁻¹·dA)` — solve `A·X=dA` once, take `Tr(X)`; **O(n²), degeneracy-free** |
| **`X=A⁻¹B` (solve)** | **`dX = A⁻¹·(dB − dA·X)`** — the v15-f principle in one line: RHS `r=dB−dA·X` (one gemm), **one** back-solve with the same factor |
| `A=LLᵀ` (chol) | `dL = L·Φ(L⁻¹·dA·L⁻ᵀ)`, `Φ(X)= tril(X)` with **halved diagonal** (Murray eq 6). Code: `tmp=trisolve(L,dA,ᵀ)`, `M=trisolve(L,tmp)`, `Φ(M)`, `dL=L·Φ(M)` (2 trisolves) |
| `PA=LU` | `M=L⁻¹(P·dA)U⁻¹`; `dL=L·tril(M,−1)`; `dU=triu(M)·U`. Pivots piecewise-constant ⇒ tangent 0 |
| `A=QR` | `M=Qᴴ·dA·R⁻¹`; `do=tril(M,−1)−tril(M,−1)ᴴ (+ complex diag corr)`; `dR=(M−do)R`; `dQ=Q(do−M)+dA·R⁻¹` |
| `A=QΛQᵀ` (eigh) | `P=Qᵀ·dA·Q`; `dλ=diag(P)`; `dQ=Q·(F∘P)`, `F_ij=1/(λ_j−λ_i)` (i≠j), 0 diag |
| `A=USVᵀ` (svd) | `dP=Uᴴ·dA·V`; `ds=Re diag(dP)`; `dU/dV` via `F∘(σ-scaled dP ± ᴴ)` + diag/phase corr, `F_ij=1/(σ_j²−σ_i²)`; **+ rectangular correction** for thin factors (`m>n`: `dU += (dA·V − U(UᴴdA·V))·diag(1/σ)`) |

- **Degeneracy**: `F` blows up at repeated λ/σ; `1/σ` at zero σ. `dλ`/`ds` (value tangents)
  never divide → always finite. Ship value-only + subspace-projector paths (Recommendation §3).
- **FFT JVP = the transform itself** (DFT linear ⇒ `jvp(fft)(x,dx)=fft(dx)`; normalization
  passes through unchanged; rfft with real tangent is trivial — the Hermitian packing only
  bites the v16 VJP). **Filtering** `y=h⊛x` bilinear: `dy=dh⊛x + h⊛dx` (the v15-h
  filter-design case).
- **Spline JVP**: wrt eval point → the spline **derivative** at t (Hermite derivative basis,
  free from `cubic_spline.hpp`); wrt control values → reuse the **build's Thomas tridiagonal
  factor** (`dd = T⁻¹(R·dy)`) — same factor-reuse principle as a dense solve.

### D. v15-g — Taylor-mode jets + Taylor ODE integrator

Jet = length-(K+1) array of **normalized** coefficients `a[k] = f^(k)(t₀)/k!` (store normalized
to avoid k! overflow; recover derivative as `k!·a[k]` on demand). Seed a variable = `{x,1,0,…}`
(generalizes `Dual{v,d}`). Prefer **compile-time K** (`Jet<T,K>`, C-array, loops unroll) for the
small-K path; arena dynamic-K for the ODE integrator (K≈20–30).

**The master composition rule** (for `y=g(f)`, `y'=w(f)·f'` with `w=g'∘f` a series you have):
```
y_k = (1/k) · Σ_{i=0}^{k−1} (k−i)·f_{k−i}·w_i           [Griewank-Walther Ch.13, Table 13.2]
```
Fold the `(k−i)/k` weight *inside* the accumulation (the overflow-guard "interleave"). Every
function is the master rule + a seed + its `w`; the `w` = its first-derivative rule (reuse the
v15-b JVP library — do not re-author).

| Op | Recurrence (normalized coefficients) |
|---|---|
| `f±g` | `(f±g)_k = f_k ± g_k` |
| `f·g` | `Σ_{i=0}^{k} f_i·g_{k−i}` (Cauchy product — the O(K²) core) |
| `f/g` | `(1/g_0)[ f_k − Σ_{i=0}^{k−1} (f/g)_i·g_{k−i} ]` |
| `exp` | `e_0=exp(f_0)`; `e_k=(1/k)Σ(k−i)f_{k−i}·e_i` (w = e itself) |
| `log` | `l_k=(1/f_0)[ f_k − (1/k)Σ_{i=1}^{k−1} i·f_{k−i}·l_i ]` |
| `sin/cos` | coupled: `s_k=(1/k)Σ(k−i)f_{k−i}·c_i`, `c_k=−(1/k)Σ(k−i)f_{k−i}·s_i` |
| `tan` | `w=1+t²` (keep aux `q=t²` via Cauchy square): `t_k=(1/k)Σ(k−i)f_{k−i}·w_i`, then `q_k=Σ t_i t_{k−i}` |
| `tanh` | as tan with `w=1−t²` |
| `sqrt` | `s_k=(1/(2s_0))[ f_k − Σ_{i=1}^{k−1} s_i·s_{k−i} ]` |
| `pow(f,α)` | `p_k=(1/(k·f_0))Σ_{i=0}^{k−1}(α(k−i)−i)·f_{k−i}·p_i` |
| `pow(f,g)` | `exp(g·log f)` (compose) |

Each is the K-general form of the existing `dual.hpp` closed forms (its K=1 slice). One
templated driver takes a "supply w_i" callback (jax's `def_deriv`).

**Taylor ODE integrator** (`x'=f(x,t)`; Jorba-Zou 2005 / TaylorIntegration.jl):
1. Coefficient bootstrap: sweep k=0…p−1; with `x_0..x_k` known, push through the RHS §D
   recurrences to get `f_k`, then `x_{k+1}=f_k/(k+1)`. Cost O(E·p²). (RHS = a scalar-generic
   functor on `Jet<T,p>` — reuses the `dual.hpp` ADL idiom.)
2. Order: `p = ⌈−½·ln(ε_m)+1⌉` (ε=1e-16→p≈20).
3. Step from the tail: `h = min_k (ε_m/‖x_k‖)^{1/k}` over k=p−1,p (× safety `e⁻²`); second
   control when a last coefficient ≈ 0.
4. Step = Horner eval of the local polynomial at t_n+h; **dense output is free** (event
   location, plotting) — a genuine advantage over RK.

Peers: TaylorDiff.jl / jax.jet / ADOL-C `hos_forward` (jets); TIDES / TaylorIntegration.jl +
DOP853/RK45 cross-method (integrator). Metric = accuracy-per-RHS-sweep AND per-wall-clock; a
work–precision diagram swept 1e-4…1e-14 vs a high-precision reference.

### E. v15-h — Wirtinger / complex forward

- `∂/∂z = ½(∂/∂x − i∂/∂y)`, `∂/∂z̄ = ½(∂/∂x + i∂/∂y)`; `df=(∂f/∂z)dz+(∂f/∂z̄)dz̄`;
  holomorphic ⟺ `∂f/∂z̄=0` ⟺ Cauchy-Riemann.
- **Carrier**: holomorphic dual = `Dual<Complex<T>>` (value + tangent both complex); a
  holomorphic op propagates `ẇ = f'(z)·ż` with **complex** multiply — the *identical real-dual
  code*, no new rules, for `+−×÷/exp/log/sin/sqrt/pow` and the linear FFT.
- **Non-holomorphic ops** need the full pair: `conj`→`ẇ=conj(ż)`; `Re`→`Re(ż)`; `Im`→`Im(ż)`;
  `|z|²`→`2Re(z̄ż)`; `|z|`→`Re(conj(z/|z|)·ż)`. Default single-complex-tangent carrier; switch a
  subgraph to a Wirtinger-pair carrier (trait dispatch) when a non-holomorphic primitive appears.
- **Gate**: seed `ż=1` and `ż=i`, reconstruct the 2×2 real Jacobian, assert scale-and-rotate
  (`u_x=v_y`, `u_y=−v_x`) = CR = ∂/∂z̄≈0; holomorphic complex-multiply path ≡ 2×2-real path
  bit-exact; `conj`/`abs` produce the right value **and** correctly fail the ∂/∂z̄=0 test.

### Reconstruct-verify discipline (ADR-0097 §8) — and its two limits

Every rule ships only when **analytic ≡ v13 complex-step (~1e-15) ≡ FD (~1e-6)**; verify in
python/JAX BEFORE the C++. One scalar-generic functor drives all three oracles (real `T` /
`Dual<T>` / `std::complex<T>` via `hesap-diff/complex_step.hpp`). **Two limits to encode:**
(1) complex-step **cannot** validate the Wirtinger rules (it co-opts the imaginary axis) → use
2×2-real-Jacobian FD + hyper-dual; (2) SVD/eig of a *complex* matrix isn't analytic → route
complex-step through the `σ² = eig(AᵀA)` surrogate (Giles §6).

---

## What we read

Peer source (local `external/`): Ceres `include/ceres/jet.h` · autodiff `forward/dual/dual.hpp` ·
CoDiPack · Adept-2 · ColPack `README.md` + `src/BipartiteGraphPartialColoring` + `src/Recovery`.
Cerid: `hesap-opt/{dual,forward_ad,gradient_check}.hpp` · `hesap-diff/complex_step.hpp` ·
`hesap-interp/cubic_spline.hpp` · `hesap-fft/fft.hpp` · `crd/math/simd/{vec4d,vec8f}.hpp`.

Literature:
- Giles, *An extended collection of matrix derivative results…* Oxford NA-08/01 (2008) — the JVP table + the complex-step validation harness.
- Murray, *Differentiation of the Cholesky decomposition*, arXiv:1602.07527 — the Φ operator + forward rule.
- Townsend, *Differentiating the QR decomposition* — the thin-QR JVP.
- JAX `jax/_src/lax/linalg.py` — the ready-to-code JVP forms + degeneracy handling (source of truth); JAX Autodiff Cookbook (complex convention).
- PyTorch `torch.linalg.{svd,eigh}` docs — the verbatim degeneracy contract.
- Hill & Dalle, *Sparser, Better, Faster, Stronger*, arXiv:2501.17737 + the ICLR-2025 ASD illustrated guide — the tracer + Θ(k²n) result. SparseConnectivityTracer.jl / SparseMatrixColorings.jl docs. Gebremedhin-Manne-Pothen, *What Color Is Your Jacobian?*, SIAM Review 2005.
- Griewank & Walther, *Evaluating Derivatives* (2008) Ch.13 Table 13.2 — the Taylor recurrences. TaylorSeries.jl / jax.experimental.jet / TaylorDiff.jl. Jorba & Zou (2005) + TaylorIntegration.jl — the Taylor ODE integrator. Bettencourt-Johnson-Duvenaud (JAX Taylor-mode) + Kelly et al. arXiv:2007.04504 (O(K²)≫O(2^K)).
- Fike & Alonso (2011) — hyper-dual numbers.

## Alternatives considered

- **Enzyme** — strongest AD-through-opaque-C++, but LLVM-plugin, MSVC-incompatible: OUT (ADR-0097 §6, portability cornerstone). We don't claim to beat it on its axis.
- **Expression templates (autodiff.hpp style)** for the substrate — rejected as the default: compile-time cost + aliasing hazards outweigh the no-temporaries win for our single-pass drivers.
- **FFT O(K log K) series multiply** — rejected: wrong K regime + numerically worse for decaying coefficients.
- **ColPack as the coloring backend** — it can't trace; and it's STL-heap + hash-nondeterministic. We reimplement the same greedy algorithms deterministically on the arena (matching its color counts).

## Pitfalls / gotchas

- **Determinism**: pin the chain-rule FMA (`mul_add` vs `fma`) and the vector-forward lane layout; use ordered bitsets/sorted runs (not hash sets) so colorings are bit-reproducible.
- **Sparsity**: structural-vs-numerical zero trap (never reuse a *local* pattern across inputs); Hessian dense-row pair-set blow-up; the ⚠ five-flag equations need the char-for-char recheck.
- **Matrix-calculus**: eigenvector-sign / singular-vector-phase gauge freedom (fix the *same* gauge in reference and hesap rule, else the oracle "fails" on a legal sign flip); `f_0≠0` (and `>0` for log/pow) guards return a status, no hidden NaN.
- **Taylor**: convergence radius governs the step (non-analytic RHS collapses it); normalized coefficients + interleave to dodge k! overflow; sin/cos & tan/tanh advance in lockstep.
- **Migration (v15-a)**: the re-export shims must preserve *every* `crd::hesap::opt::` name (Dual, free transcendentals, mixed operators, `forward_ad_gradient`, `FunctorObjective`) — the opt suite is the zero-regression gate. Blast radius is hesap-opt only (`log_density_grad.hpp`'s `Dual<` is a comment).

## Open questions

- Exact v16-g bridge placement (thin `hesap-opt-diff` seam vs in-module `custom_vjp` registration) — pinned per-slice at v16, not needed for v15.
- Sacado build (Trilinos) — likely N/A-with-check on the boards; confirm at peer-env pre-flight.
- Whether the v15-e coloring should also ship star-bicoloring now (Montoison-Dalle augmented-symmetric reformulation) or defer — decide at v15-e.

## Used by

- v15 kickoff session 2026-07-06 (this dossier). Consumed by v15-a…v15-z as the implementation reference; boards land in `docs/bench/` per slice.
