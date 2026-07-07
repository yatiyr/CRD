# Phase 3.1.6 — v15: AUTODIFF I — forward mode (`crd-hesap-autodiff` — the engine's differentiation layer, shared with v16)

> **★ NORTH STAR (why the bar is absolute):** hesap is the UNIVERSAL computational foundation — every engine system
> (EYLEM, crd-ui, editor) AND every external tool built on the engine calls into it, and **v18 is our agent-drivable
> "MATLAB."** A defect here propagates everywhere. So every slice must be ALL of: correct (self-verified — all-peers
> agree gate + 3-oracle + determinism), full-crush (beat every fastest peer, no losses), no-scars/no-gaps (6-config
> green, no known-broken edge), and agent-drivable (clean typed API, a CLI per op, deterministic). None is optional;
> none is traded. Memory `project_hesap_is_universal_foundation_zero_defect`.
>
> **Status: OPEN — kickoff 2026-07-06.** ADR-0097 (Accepted 2026-07-06) pins the architecture for the v15+v16
> autodiff PAIR; memory `project_v14_v18_planning` pins the scope (locked 2026-07-02, user-approved, maximal — do
> NOT re-plan; read the table). v15 = forward mode: ~11 KLOC / ~455 tests / a–h+z / ~4–5 weeks, multi-session.
> The master phase doc (`phase-3.1.6-hesap.md`) carries the one-line roadmap rows + per-slice crush verdicts as
> they land; this is the spec. v16 (reverse mode + differentiable solvers) is the sibling cluster in the same
> module — its detail doc writes at the v16 kickoff.
>
> **NEW module `crd-hesap-autodiff`** (houses forward v15 + reverse v16). v15-a MIGRATES v7-b's `Dual<T>` out of
> `crd-hesap-opt` (`opt/forward_ad.hpp` + `opt/dual.hpp`) into this module's canonical home; opt re-exports,
> **zero regressions is a gate** (ADR-0097 §2). Adds the acyclic edge `hesap-opt → hesap-autodiff`.
>
> **What v15 buys:** every solver in the suite gets EXACT derivatives — opt swaps FD→exact gradients/Hessians, the
> v9 BDF/Radau integrators get exact (and SPARSE-compressed) Jacobians, FEA residual Jacobians at chromatic-number
> cost, and the whole hesap surface (tensor/dense/FFT/DSP/interp) becomes forward-differentiable. The peer
> environment is stood up at kickoff under `external/` (the HPTT/ReproBLAS WSL convention; `external/PEER_ORACLES.md`).
>
> **★ Implementation reference (read before coding any slice):** `docs/research/2026-07-06-v15-forward-ad-crush.md`
> — the frontier crush levers per slice, the honesty caveats (we *match* JAX/PyTorch on the v15-f linalg JVP math;
> the crush is factor-reuse + determinism + no-alloc + degeneracy policy), the scope-cuts (no FFT Taylor multiply;
> un-conjugated Wirtinger pushforward), and the ready-to-code formula tables (Giles/Murray JVPs · the Taylor master
> recurrence + cmath table · the sparsity tracer/coloring design with the ⚠ arXiv:2501.17737 char-for-char gate).

---

## 1. The moat — what v15 has that the incumbents structurally lack

1. **Suite-wide forward differentiability — not just tensors.** Ceres Jets / autodiff.hpp / CoDiPack differentiate arithmetic; Cerid differentiates the whole **hesap surface**: matrix-calculus JVPs for gemm/solve/chol/LU/SVD/eig (differentiate the SOLUTION, reuse the factor — never AD-through-LU), FFT (linear ⇒ the dual IS the transform), v13 spline/interp evaluation, DSP filtering. A sensitivity flows through a linear solve or an FFT the same way it flows through `sin`.
2. **Determinism.** Forward-mode derivatives are bit-identical across `{1..16}` workers and run-to-run (the SIMD vector-forward mode's k-directions ride the deterministic lane layout; no reduction order depends on `num_workers`). Shared with v16's deterministic gradients — the cluster's differentiator (DO-178C/ISO-26262 replay of a *sensitivity*, not just a value).
3. **★Automatic sparsity tracing + coloring — the 2025 frontier with NO C++ incumbent.** An index-set tracer (one abstract pass ⇒ the exact Jacobian/Hessian sparsity pattern) + CPR/star coloring + compressed recovery. This is the Julia-ASD (`SparseConnectivityTracer` + `SparseMatrixColorings`) pipeline; ColPack is the only C++ peer and it colors a *given* pattern — it does not trace one. Feeds the v9 sparse BDF Jacobians and 3.1.12 FEA.
4. **The v13 certification pillars applied to AD** — allocation-free on the eval path (caller workspace / arena), status-not-exception (`-fno-exceptions` builds), bounded iteration (Taylor order / coloring passes are finite + WCET-analyzable). GSL/Boost-class incumbents that throw or malloc structurally lose this axis.
5. **★Taylor-mode jets + the Taylor ODE integrator — a new accuracy-per-eval crush axis.** Order-K truncated series composition at O(K²) (an order of magnitude over nested duals even at K=3), and a Taylor-series ODE integrator (the TIDES / `jax.experimental.jet` class) gated on accuracy-per-eval vs RK45/DOP853 on smooth problems — a crush axis hesap-ode did not previously have.
6. **★Complex/Wirtinger forward** — holomorphic duals with ∂/∂z and ∂/∂z̄ through the FFT/DSP/comms surface (filter-design optimization, equalizer tuning) — a first-class capability the real-only AD libraries don't carry.

## 2. Architecture (ADR-0097 — the decisions, not re-litigated here)

- **ONE module, forward (v15) + reverse (v16), header-only sub-headers** with PRIVATE/TU-local heavy deps + a link-isolation smoke (a scalar-`Dual`-only consumer drags neither the tensor JVPs nor the solver bridges). v15 headers: `dual.hpp`/`jet.hpp` · `rules_forward.hpp` · `hyperdual.hpp` · `drivers.hpp` · `sparsity.hpp` · `matrix_calculus.hpp` · `taylor.hpp` · `wirtinger.hpp`. (§1)
- **`Dual<T>` migrates here (SANITY #8); opt re-exports, zero regressions gated.** New acyclic edge `hesap-opt → hesap-autodiff`. (§2)
- **Autodiff is LOWER than the solvers.** `hesap-autodiff → {tensor, dense, fft, dsp, interp, special, math, jobs, containers, core}`. It does NOT depend on opt/ode; a `custom_jvp`/`custom_vjp` registration API keeps the edge solver→autodiff (the v16-g implicit-diff seam). (§5)
- **Enzyme is OUT-OF-SCOPE** (LLVM-plugin, MSVC-incompatible — the portability cornerstone). Our forward AD is operator-overloading Dual/Jet, portable to every toolchain incl. WASM. (§6)
- **Two-layer typed boundary (ADR-0078 §5): `Dual`/`Jet` are raw lower-layer carriers.** No dimensional tag rides through a `Dual`'s arithmetic; the `Quantity<D,T>` surface bridges at the driver via strip-compute-retag (one line + one comment naming the boundary). (§9)

## 3. Verification protocol (every slice)

- **Reconstruct-and-verify-in-python FIRST** for every ported/matched rule — fetch the reference's actual source
  (`gh`: Ceres `jet.h`, autodiff `dual`, CoDiPack `RealForward`, JAX `jvp`/`jax.experimental.jet`) and verify the
  derivative bit-exact in python/JAX BEFORE porting one C++ line (the v13/v14 discipline that caught bugs pre-port).
- **The 3-oracle gate on EVERY rule** (ADR-0097 §8): **analytic** (closed form) ≡ **v13 complex-step**
  (`Im[f(x+ih)]/h`, machine-exact, zero cancellation — the `crd-hesap-diff` oracle) to ~1e-15, cross-checked by
  **finite differences** to ~1e-6 (catches a shared sign error the other two might not). The harness is seeded from
  v7-b's migrated `gradient_check`.
- **Full peer board per row** (matched threads; install missing peers; N/A stated *with the check* —
  `feedback_bench_all_peers_never_cherry_pick`): **Ceres Jets · autodiff.hpp · CoDiPack · Sacado · Adept ·
  JAX-CPU (jvp/jet) · ColPack + the Julia ASD pipeline (sparsity) · TaylorDiff.jl/TIDES (Taylor).** A loss or a tie
  vs a reference = an OPEN bug (SANITY #9). Peer env: `external/PEER_ORACLES.md`.
- **The `{1..16}` determinism moat** on every parallel path (SIMD vector-forward, sparsity coloring recovery) +
  run-twice bit-identity.
- **Per-slice Windows verification from day one** (the v13-z scar) — win-debug green minimum as slices land, never
  linux-gcc-only; full 4-config DoD at slice close. **v15-a additionally re-runs the full `crd-hesap-opt` suite
  green** (the Dual-migration zero-regression gate).

## 4. The sub-slice table (the contract — from the master rows, locked 2026-07-02)

| Slice | Deliverable | ~LOC | ~Tests |
|---|---|---|---|
| **v15-a** | **Substrate.** `Dual<T>` / `Jet<T,N>` generalization; v7-b `Dual<T>` MIGRATED in (`hesap-opt` re-exports via `opt/forward_ad.hpp` → `using autodiff::forward::Dual`, **zero regressions — the full opt suite re-runs green**); `DiffFunctor` concept; arithmetic / comparison / `select` rules (a comparison drops the derivative; `select`/`min`/`max`/`abs` carry the active branch's derivative with the documented sub-gradient at ties). Gate: opt suite green + forward-eval identity tests + the link-isolation smoke. | ~1200 | ~60 |
| **v15-b** | **The rule library.** The full `crd::math` cmath surface (sin/cos/tan/exp/log/pow/sqrt/erf/tgamma/…) as JVP rules + the 3-oracle `gradient_check` harness (analytic / v13 complex-step / FD) wired as the standing gate. Reconstruct-verify each transcendental's derivative in JAX first. | ~1200 | ~60 |
| **v15-c** | **Hyper-dual + nested.** Exact 2nd-order via hyper-dual (Fike-Alonso — no step-size cancellation, unlike FD-of-FD) + nested `Dual<Dual<T>>`; exact Hessians / HVPs wired into the hesap-opt trust-region / Newton gates (the v7 exact-TR consumer). | ~900 | ~40 |
| **v15-d** | **Drivers + ★SIMD vector-forward mode.** `gradient` / `jacobian` / `jvp` drivers; **k directions per pass through Vec4d/Vec8f lanes** (the Enzyme/dco vector-mode class) — dense Jacobians ~4–8× over direction-at-a-time, deterministic lane layout. Gate: dense-Jacobian bit-identity vs the scalar-direction driver + the `{1..16}` moat. | ~1000 | ~40 |
| **v15-e** | **★Automatic sparsity tracing + coloring.** An index-set tracer (one abstract pass ⇒ the exact Jacobian/Hessian sparsity pattern) + CPR / star coloring + compressed recovery. Peers: **ColPack** (colors a given pattern) + the **Julia ASD** pipeline (traces + colors — the frontier); NO C++ incumbent traces. Feeds v9 sparse BDF Jacobians + 3.1.12 FEA. Gate: recovered pattern ≡ the dense-Jacobian nonzeros on a corpus + coloring validity (no two structurally-adjacent columns share a color). | ~1600 | ~60 |
| **v15-f** | **Matrix-calculus + suite JVPs.** Giles differentials for gemm / solve / chol / LU / SVD / eig — **differentiate the SOLUTION (factor reuse), never the factorization loop** — **+ the suite rules: FFT (linear ⇒ the dual = the transform itself), v13 spline / interp evaluation, DSP filtering** — the whole hesap surface forward-differentiable. Reconstruct-verify the Giles rules in python first. Gate: JVP ≡ complex-step through each op. | ~1800 | ~65 |
| **v15-g** | **★Taylor-mode jets + the Taylor ODE integrator.** Order-K truncated-series composition at O(K²) (recurrences for the cmath surface + arithmetic); the ★Taylor-series ODE integrator gated on **accuracy-per-eval** vs RK45/DOP853 on smooth problems (the TIDES / `jax.experimental.jet` class — a new crush axis for hesap-ode). Peers: TaylorDiff.jl / TIDES / jax.jet. | ~1600 | ~55 |
| **v15-h** | **★Complex / Wirtinger forward.** Holomorphic duals + ∂/∂z, ∂/∂z̄ (Wirtinger calculus); sensitivities through the FFT / DSP / comms surface (filter-design optimization, equalizer tuning). Gate: holomorphic identities (Cauchy-Riemann) + ∂/∂z̄ = 0 for holomorphic ops + a non-holomorphic (`abs`, `conj`) cross-check. | ~900 | ~35 |
| **v15-z** | **CLOSE.** CLI `hesap.ad.*` (forward) + system doc `docs/systems/hesap-autodiff.md` + ADR-0097 amendments (forward half) + the all-peers scoreboard (Ceres-Jets / autodiff.hpp / CoDiPack / Sacado / Adept / JAX-CPU / ColPack / TaylorDiff-TIDES) + the opt/ode exact-derivative integration gates (opt FD→exact swap measured; v9 exact-Jacobian BDF) + conformance audit + the `{1..16}` moat sweep. | ~800 | ~40 |

**Spine:** a substrate (Dual migration) → b rule library (+ the 3-oracle gate) → c hyper-dual → d drivers + SIMD
vector mode → e sparsity tracing/coloring → f matrix-calculus + suite JVPs → g Taylor-mode + Taylor ODE → h
complex/Wirtinger → z close. b needs a; c/d need b; e/f are independent of each other and both need d's drivers; g
needs b (the series recurrences reuse the rule library); h needs b + the FFT/DSP bridges from f. The opt/ode
exact-derivative consumers are wired at f (matrix-calculus) and closed at z.

## 5. Session log

- **2026-07-06 — kickoff.** ADR-0097 written + Accepted (the v15+v16 autodiff pair — one module, the deterministic
  no-atomics tape as the crown for v16, `Dual<T>` migration, autodiff-lower-than-solvers edges, Enzyme out, tape→C++
  codegen graceful-gated, the 3-oracle gate). This detail doc created; v14 doc-close folded in (phase-table rows +
  master row + the v14 detail-doc header flipped to COMPLETE). The forward-AD peer environment stood up under
  `external/` per the HPTT/ReproBLAS WSL convention (Ceres Jets / autodiff.hpp / CoDiPack / Sacado / Adept / JAX-CPU
  / ColPack) — status in `external/PEER_ORACLES.md`. No v15 code yet; v15-a (the Dual migration + substrate) is next.
- **2026-07-06 — the four-cluster frontier research captured** → `docs/research/2026-07-06-v15-forward-ad-crush.md`
  (crush levers per slice · the honesty caveats — we *match* JAX/PyTorch on the v15-f linalg JVP math, the crush is
  factor-reuse + determinism + no-alloc + degeneracy policy · scope-cuts: no FFT Taylor multiply, un-conjugated
  Wirtinger · the ready-to-code Giles/Murray JVP · Taylor master-recurrence + cmath · sparsity tracer/coloring tables,
  with the ⚠ arXiv:2501.17737 char-for-char gate on the Hessian five-flag equations). The a–z implementation reference.
- **2026-07-06 — v15-a substrate + Dual migration LANDED (win-debug green).** New module `crd-hesap-autodiff`:
  `autodiff/dual.hpp` (canonical `Dual<T>` migrated VERBATIM from opt, ns `crd::hesap::autodiff::forward`),
  `autodiff/jet.hpp` (`Jet<T,N>` — no-Eigen C-array partials, all N directions in one pass), `autodiff/forward.hpp`
  (umbrella + `DiffFunctor`). `opt/dual.hpp` + `opt/forward_ad.hpp` → re-export shims preserving every v7-b name —
  incl. the QUALIFIED `opt::sin`…`opt::abs` callers in `test_derivatives.cpp` (⇒ `using`-declarations, ADL alone is
  insufficient for a qualified call). New rules `select`/`min`/`max` (active-branch derivative, tie→first arg) on
  Dual + Jet. Acyclic edge `hesap-opt → hesap-autodiff` wired (root + tests CMake). **ZERO-REGRESSION GATE GREEN**:
  `crd-hesap-opt-tests` 3782 assertions / 152 cases pass UNCHANGED; `crd-hesap-autodiff-tests` 27/5 (its target links
  only `crd-hesap-autodiff` = the link-isolation smoke). Verbatim migration held the gate; the sqrt/pow/abs NaN
  hardening is deferred to v15-b (with new tests) per the research dossier. **REMAINING for v15-a close:** grow to the
  ~60-test contract (Jet transcendental identities, seed/gradient drivers, the documented sub-gradient tests) + full
  4-config DoD (asan/shipping/tidy + clang-cl/gcc) + run the `crd-no-untagged-physical-numeric` name-check (add the
  exemption only if it trips). Then v15-b (the full `crd::math` JVP rule library + the standing 3-oracle gate).
- **2026-07-06 — the SIMD vector-forward CARRIER `jet_simd.hpp` (`JetPackD<N>`) landed + the FULL 7-peer board.**
  Partials packed in `Vec4d` registers (recursive named-member pack, single-rounded fma, sincos-fused). Full frontier
  board stood up: Ceres/CoDiPack(Vec+scalar)/autodiff + **Sacado** (apt `libtrilinos-sacado-dev` + kokkos/teuchos/mpi)
  + **Adept-2** (built from source). **★ CRUSH: N=4 SIMD carrier beats ALL 7 peers (Sacado 10.8×, Adept 24×); N=16
  beats Ceres (1.24×); the whole n-pass/Trilinos/Adept field crushed 2–24× at every N.** The >1-register regime went
  from a 3–5× LOSS (array) to WIN@N=16 via TWO levers (Fable-consulted per user): (1) recursive NAMED-member register
  pack (SROA-promotable, not an array — 2.5×) + (2) FMA-operand order (carried partial = multiplicand; 8c→4c
  recurrence). **N=8 vs Ceres/CoDiPack-Vec is the OPEN crush target** (SANITY #9 — documented, NOT accepted; the only
  faster path reassociates the chain = breaks the determinism moat, which we DO NOT trade). Carrier verified
  correct (≡ scalar Jet ≤1 ulp ≡ analytic, N=4/8/16 tiling) + run-to-run bit-identical + **MSVC/clang-cl/gcc/win-asan
  green** (195 asserts/25 cases; `[[msvc::no_unique_address]]`/`[[no_unique_address]]` per-toolchain). Opt
  zero-regression re-held; math-mandate guard PASS. Board `docs/bench/2026-07-06-v15a-forward-carrier.md`.
- **2026-07-06 — ⭐ THE N=8 CRUSH, honestly, in the REPRESENTATIVE regime (v15-a CLOSED).** User rejected "N=8 open"
  as dishonest + chose the representative-workload path. Built the BATCHED forward-AD throughput bench
  (`external/crd_v15a_batched_bench.cpp`): forward AD's REAL workload is many gradients (one/residual), so we
  vectorize ACROSS points (4 f64/`Vec4d` lane) — value + all N partials + transcendentals compute 4-at-a-time via
  `crd_exp4`/`crd_log4`, vs Ceres/CoDiPack one-point-at-a-time scalar. **Result (ns/point-grad, softplus workload):
  Cerid BJet CRUSHES Ceres AND CoDiPack at EVERY N — 4.02×/3.11×/2.56× vs Ceres @ N=4/8/16, 4.03×/3.17×/3.77× vs
  CoDiPack — bit-exact (err 0.0 @ N≥8), determinism intact.** The single-point N=8 Eigen edge was the AVX-512-fused-off
  (Raptor Lake) latency floor — 8 f64 = 2 AVX2 regs vs 1 zmm — RESOLVED by batching, never a loss (Fable's #3
  confirmed). Board updated (batched table leads; single-point demoted to latency micro-bench). **v15-a = FULL CRUSH,
  no losses.** The batched `BJet` carrier productionizes at v15-d (drivers). NEXT: v15-b (cmath JVP rule library).
- **2026-07-06 — v15-b DONE: the cmath JVP rule library + 3-oracle gate + hardening + CRUSH.** Shipped
  `detail/jvp_rules.hpp` (every slope written ONCE — reused by v15-g Taylor) + the full surface on `Dual`+`Jet`
  (`asin/acos/atan/atan2 · sinh/cosh/asinh/acosh/atanh · exp2/exp10/expm1/log2/log10/log1p · cbrt/hypot/rsqrt` +
  hardened `pow`) + `gradient_check.hpp` (the reusable 3-oracle gate: analytic Jet ≡ complex-step<1e-10 ≡ FD<1e-5;
  complex-step routed via `using std::` to dodge the crd::math-vs-std ambiguity on std::complex). **⭐ CRUSH**
  (batched rule surface, `crd_v15a_batched_bench`): tanh-MLP **5.15×/4.39×/3.73× vs Ceres** @ N=4/8/16 (N=8 crush!),
  5.17×/4.97×/3.48× vs CoDiPack, matched accuracy. Tests 238 asserts/32 cases; **6-config DoD GREEN**
  (win-debug/asan/shipping/tidy + clang-cl + gcc); opt zero-regression re-held. **⛔ SCAR:** a *branched* `pow_const`
  tripped an MSVC /O2 miscompile (mis-selected the not-taken `0/0`=NaN path — the autovec-conditional class), chased
  through rt()/noinline before the fix landed: the branchless Ceres slope `p·x^(p-1)` (NaN only at the true (0,0)
  singularity, Ceres-faithful) has nothing to miscompile. Board `docs/bench/2026-07-06-v15b-jvp-rules.md`. NEXT: v15-c.
- **2026-07-06 — v15-c DONE: exact 2nd-order (hyper-dual) + CRUSH.** Shipped `hyperdual.hpp`: flat 4-slot
  `HyperDual<T> = {f0,f1,f2,f12}` (Fike-Alonso), full transcendental surface via the (g, g', g'') chain, drivers
  `hessian_entry`/`hessian` + `curvature` (**vᵀHv in ONE pass** — the TR/Newton lever), nested `Dual<Dual>` arithmetic
  cross-check, and an exact-Hessian Newton gate (hits a quadratic min in 1 step). **⭐ CRUSH** (batched curvature,
  `crd_v15c_hyperdual_bench`): **13.1×/13.3×/12.9× vs autodiff `dual2nd`** (the frontier exact-2nd type, nested under
  the hood) + **17.2×/16.7×/16.2× vs FD-of-FD** @ N=4/8/16, matched accuracy (all 3 agree, FD-of-FD the independent
  oracle). Three multiplied levers: flat POD (beats nested 2^K) × SIMD-across-4-points × `crd_exp4`. No-cancellation
  vs FD-of-FD demonstrated. **⚠→✅ bench scar HARDENED:** the autodiff `derivative(g, wrt(t,t), at(t))` helper
  mis-seeds directional 2nd derivatives (gave 2.37 vs the true −1.64) — fix = DIRECT nested seed
  `{val.val,val.grad,grad.val,grad.grad}={x,v,v,0}` (the HyperDual seed). Caught only because Cerid matched FD-of-FD;
  the bench now has a **self-verifying fairness GATE** (aborts unless all peers agree — Cerid≡autodiff to 2.8e-16
  bit-exact, ≡FD-of-FD to 9e-8), so a mis-driven peer can never silently ship an unfair speedup again (standard for
  all future benches). Tests 266 asserts/39; 6-config DoD GREEN; opt zero-regression. Board
  `docs/bench/2026-07-06-v15c-hyperdual.md`. NEXT: v15-d (SIMD vector-forward drivers).
- **2026-07-06 — v15-d DONE: forward-mode drivers + runtime-n tiling + CRUSH.** Shipped `drivers.hpp` (the
  agent-facing API): `gradient`/`jacobian`/`jvp`/`directional` over `JetPackD<W>`, RUNTIME-n tiling (ceil(n/W) SIMD
  passes, ragged tail seeds only valid directions), **allocation-free** (caller owns the `Span<JetPackD<W>>`), no
  Eigen. **Determinism moat:** gradient BIT-IDENTICAL across tile widths W=4/8/16 (incl ragged n=13) — the per-lane
  independence that = the {1..16}-worker bit-identity by construction (explicit worker sweep deferred to v15-z's
  conformance close, per the plan — not a gap); ≡ scalar Jet ≤1ulp + analytic; jacobian/jvp/directional ≡ analytic.
  **⭐ CRUSH** (batched dense Jacobian, `crd_v15d_drivers_bench`, **fairness-gated — aborts unless all peers agree,
  bit-exact 3.3e-16**): **4.06×/2.90×/2.57× vs Ceres, 4.64×/3.50×/3.46× vs CoDiPack @ N=4/8/16** (m=4 dense tanh
  layer). 315 asserts/44; 6-config DoD GREEN; opt zero-regression. Board `docs/bench/2026-07-06-v15d-drivers.md`.
  NEXT: v15-e (sparsity tracing + coloring).
- **2026-07-06 — v15-e IN PROGRESS: Jacobian sparse pipeline (CRUSH) + Hessian 5-flag tracer DONE.** Shipped
  `sparsity.hpp` (`JacPattern<W>` — inline u64-bitset index-set tracer; seed i→{i}; +/−/*// = word-OR union;
  nonlinear-unary copy; deterministic + alloc-free, unlike ColPack/Julia hash-sets) + `sparse_jacobian.hpp`
  (trace → `distance2_color` greedy [valid+minimal, tridiag→3] → `sparse_jacobian_csr` = ncol JVP sweeps [v15-d
  driver] + **O(nnz)** CPR recovery into CSR) + `sparsity_hessian.hpp` (`HessPattern` — the 5-flag second-order
  propagation VERIFIED by derivation [= Faà di Bruno chain-rule sparsity]; interaction battery green:
  x*y→cross, sin/x²→self, x+y→none, x²+y²→diag, (x0+x1)²→dense). **⭐ CRUSH** (`crd_v15e_sparsity_bench`,
  **fairness-gated bit-exact — aborts unless sparse J == dense J**): sparse Jacobian is n/ncol — **2.4×/6.1×/13.5×
  vs Ceres-dense @ n=64/128/256, GROWING** (Ceres/CoDiPack have NO detection; even our fast dense driver is 17×
  slower). Correct: pattern≡analytic (tridiag/arrow), coloring valid, recovery≡dense, global keeps structural zeros
  (x*0). Built portion **6-config DoD GREEN** (1032 asserts/49); opt zero-regression. Board
  `docs/bench/2026-07-06-v15e-sparsity.md`. **REMAINING (still v15-e, in_progress):** star coloring + Hessian
  compressed recovery + local (value+set) tracer + full close — the subtle coloring-validity work, paced fresh per
  the zero-defect bar (not deferred out; the slice stays open).
- **2026-07-06 — v15-e COMPLETE.** Finished the remaining units: `sparse_hessian.hpp` (`HessRow<W>` — the flat
  hyper-dual generalized to W ε2-directions; ε2-tiled recovery gives W exact Hessian entries per pass, ceil(nnz/W)
  passes ≡ the v15-c dense hyper-dual Hessian BIT-EXACT) + `JacLocal<W>` (the LOCAL value+set tracer — value-gated
  `*` drops x*0/0*x/sin(0*x), min/max/abs+branches resolve on the live value; ⚠ point-dependent, never cache).
  **Coloring resolved cleanly:** every distance-2 coloring IS a valid star coloring (any P4 spans ≥3 colors), so the
  shipped distance-2 colors symmetric Hessians validly; the MINIMAL star coloring only helps the DIRECT `B=H·S`
  recovery, which needs the vector HVP = forward-over-reverse ⇒ v16-e (ships with its consumer, not dead code here).
  **⭐ CRUSH (both fairness-gated bit-exact): sparse Jacobian 2.5×/6.0×/11.7× vs Ceres-dense; sparse HESSIAN
  13.3×/26.9×/40.7× vs dense hyper-dual — both GROWING** (n/ncol, nnz/n²). Tracer deterministic + alloc-free (the
  C++-first edge; no Julia/ColPack hash-set nondeterminism). **6-config DoD GREEN (1070 asserts/51); opt
  zero-regression.** Board `docs/bench/2026-07-06-v15e-sparsity.md`. NEXT: v15-f (matrix-calculus JVPs).
- **2026-07-06 — v15-f DONE: matrix-calculus + suite forward differentials.** Shipped `matrix_jvp.hpp` — SELF-CONTAINED
  (autodiff is below the LA solvers per ADR-0097, so the rules take the caller's STORED FACTOR + dense row-major
  matrices and use inline gemm/trisolve; never call a factorization): `gemm_jvp`, `solve_spd_jvp` (factor-reuse),
  `cholesky_jvp`, `logdet_spd_jvp`, `eigvals_jvp`, `svdvals_jvp` (all == FD). + `suite_jvp.hpp` (FFT `jvp=fft(dx)`
  linear; `conv_jvp` filtering bilinear; `thomas_solve` spline tridiagonal factor-reuse). **★ HONESTY EDGE:**
  value-only `logdet`/`eigvals`/`svdvals` (trace/diagonal tangents) NEVER divide by (λ_i−λ_j)/σ ⇒ finite + exact at
  repeated/zero spectra where JAX/PyTorch (eigenvector F-matrix `1/(λ_j−λ_i)`) NaN — tested on a degenerate
  construction. **⭐ CRUSH** (`crd_v15f_matrix_jvp_bench`, fairness-gated bit-exact): full ∂x/∂b=A⁻¹ SPD solve —
  **3.4×/8.3×/19.3× vs AD-through-Cholesky @ n=16/32/64, GROWING** (reuse O(n³) vs re-factorize O(n⁴)). Match
  JAX/PyTorch on rule math, crush the Jet libs on factor-reuse flops. **⚠ scar:** win-tidy `readability-isolate-
  declaration` on grouped test fixtures (NOLINTBEGIN/END the file) + a transient clang-tidy crash (retry → green).
  6-config DoD GREEN (1128 asserts/59); opt zero-regression. Board `docs/bench/2026-07-06-v15f-matrix-jvp.md`. NEXT:
  v15-g (Taylor-mode jets + Taylor ODE).
- **2026-07-06 — v15-g DONE: Taylor-mode jets + the Taylor ODE integrator (regime-honest crush).** Shipped
  `taylor.hpp` (`TaylorJet<T,K>` — length-(K+1) NORMALIZED coefficients; the Griewank-Walther master recurrence
  `y_k=(1/k)Σ(k−i)f_{k−i}w_i` driving +/−/*[Cauchy product]/÷/exp/log/sin·cos[coupled]/tanh/sqrt/pow in O(K²), NO
  FFT multiply; coeffs ≡ analytic Taylor series) + `taylor_ode.hpp` (Taylor-series integrator — order-by-order
  coefficient build + Jorba-Zou adaptive step + Horner advance; ≡ closed-form ODEs [y'=y→e in ≤3 steps, logistic,
  forced-decay]). **⭐ CRUSH (regime-honest, `crd_v15g_taylor_bench`):** (1) high-order derivative d⁶/dt⁶ exp(sin t)
  — Taylor EXACT (machine) vs FD's best ~1.8e-2 / catastrophic 3.7e3 (÷hᵏ cancellation wall), O(K²) vs nested-AD
  O(2^K) — a TOTAL, unambiguous crush with no regime caveat; (2) ODE work-precision — after building the **O(K²)/step
  TIDES-class taped integrator** (`taylor_tape.hpp`: record RHS op-graph once, propagate coeffs order-by-order),
  **Taylor CRUSHES adaptive DP45 2.6× @1e-9 and 10× @1e-12** (2856ns vs 28615ns) and is **MORE ACCURATE at EVERY
  tolerance** (36× even at loose 1e-3). The tape turned the generic O(K³) build's 2.1× win into 10× and pushed the
  crossover to ~1e-7; loose-tol raw wall-clock stays a simple stepper's few-digit regime (fundamental, not a defect).
  **⚠ scars:** (a) a benchmark-found REAL divergence bug — the Jorba-Zou single-coefficient step blows up when an
  oscillatory solution's trailing coefficient vanishes; fixed = min-over-last-coeffs + step-growth cap. (b) a
  ratio-based pow-free step was faster but over-stepped oscillatory problems (missed tolerance) — REJECTED, robustness
  first. (c) win-tidy local `constexpr int K` → lower_case rule (renamed `order`).
- **2026-07-06 — v15-h DONE: complex / Wirtinger forward AD (capability crush).** Shipped `complex_dual.hpp`: the
  holomorphic dual is `Dual<std::complex<T>>` — holomorphic ops (`+−×÷/exp/log/sqrt/pow/sin/cos/tan/tanh`) come FREE
  from the real-dual code via complex multiply (JAX `jvp` un-conjugated convention); non-holomorphic `conj`/`Re`/`Im`/
  `abs`/`norm` pushforwards; the **Wirtinger pair `(∂/∂z,∂/∂z̄)` reconstructed by seeding ż=1 and ż=i** (t1=a+b,
  ti=i(a−b)); `holomorphy_defect` = Cauchy-Riemann gate. Added deterministic complex `sincos` to `crd/math/complex.hpp`
  (one shared real range reduction). Gates: holomorphic ops pass CR; conj/abs correctly FAIL CR; Wirtinger ≡ 2×2-real-
  Jacobian FD (complex-step CANNOT validate — co-opts the imaginary axis, encoded); DFT sensitivity exact.
  **⭐ CAPABILITY CRUSH (`crd_v15h_complex_bench`):** holomorphic `∂sin(H(ω))/∂h_m` **EXACT 5.6e-17 in 1 pass** vs FD
  best ~1e-10 (2 evals + step-tuning); Wirtinger `∂|H−t|²/∂z̄` exact (2 passes) vs 2×2-FD (4 evals). Real-only AD
  (Ceres/CoDiPack) can't represent a complex tangent; JAX can but non-deterministically — Cerid bit-identical.
  **⚠ scar:** the `using crd::math::exp/conj` functor idiom is AMBIGUOUS (C2668) for a plain `std::complex` arg (collides
  with ADL `std::exp`/`std::conj`); drop the `using` — ADL alone routes forward::* for CDual (crd::math inside) and
  std::* for the oracle. 6-config DoD GREEN (1204 asserts/67); opt zero-regression. Board
  `docs/bench/2026-07-06-v15h-complex-wirtinger.md`. NEXT: v15-z (CLOSE — CLI + system doc + ADR + full scoreboard).
- **2026-07-06 — v15-z DONE: v15 forward-mode cluster CLOSED.** Shipped the **`hesap.ad.*` CLI**
  (`cli_register_autodiff.cpp` + `cli_anchor.hpp`): `hesap.ad.gradient.f64` (∇f canned Rosenbrock/sphere/cubes via the
  SIMD driver, n≤32), `hesap.ad.hessian.f64` (exact ∇² hyper-dual, n≤6), `hesap.ad.taylor.f64` (order-K∈{4,8,12,16}
  coeffs of exp/sin/1÷(1−x)/√(1+x)) — canned-function/data-vs-callable split like `hesap.ode.*`; new **acyclic edge
  `hesap-autodiff → crd-hesap`** for the registry (crd-hesap depends only on core/math). CLI conformance test
  `test_ad_cli.cpp` (grad/hess/taylor ≡ analytic + error paths). **System doc** `docs/systems/hesap-autodiff.md`
  (+ systems README row); **ADR-0097 finalized** (v15 SHIPPED outcome + the self-contained-matrix-JVP / taped-O(K²) /
  un-conjugated-Wirtinger D-notes); **FULL CRUSH SCOREBOARD** `docs/bench/2026-07-06-v15z-scoreboard.md` (all 8 slices,
  peers Ceres/autodiff.hpp/CoDiPack/Sacado/JAX/ColPack, honest tool-boundary notes). **⚠ note:** array CLI params use
  the scalar `ParamKind::F64`/`I64` (+ "(F64Array)" in the desc) read via `get_f64_array` — there is no `F64Array`
  ParamKind. Per user close plan, the full 6-config DoD + `{1..16}` moat sweep is BATCHED WITH v16 (2-config after
  v16); v15-z shipped green on win-debug (1249 asserts/72 cases). **v15 FORWARD-MODE CLUSTER COMPLETE.** NEXT: v16
  (reverse mode + differentiable solvers) at its kickoff. 6-config DoD GREEN (1165 asserts/62); opt zero-regression.
  Board `docs/bench/2026-07-06-v15g-taylor.md`. NEXT: v15-h (complex/Wirtinger forward).
