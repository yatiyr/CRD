# 2026-06-10 — hesap v7-f CLOSE: first-order momentum methods (+ the win-shipping stale-obj root cause)

**Phase:** 3.1.6 `crd-hesap` · v7 optimisation (`crd-hesap-opt`, ADR-0090)
**Slice:** v7-f (first-order) — CLOSED. Steepest descent (v7-a) · nonlinear CG (landed in `b261478`) · **momentum/Nesterov (this session)**.

---

## What was built

### `momentum.hpp` — Polyak heavy-ball + Nesterov accelerated gradient (FISTA form)

`engine/hesap-opt/include/crd/hesap/opt/momentum.hpp` — `minimize_momentum<T>(obj, x0, opts, alloc, mopts, variant)`:

- **HeavyBall:** `v ← μ·v − α·g(x); x⁺ = x + v` — gradient at the iterate. `μ = 0` degenerates to plain
  fixed-step GD (used as the baseline in the acceleration test).
- **Nesterov:** `y = x + μ·v; x⁺ = y − α·∇f(y)` — gradient at the lookahead. Momentum is either fixed
  `μ ∈ [0,1)` or (default) the parameter-free **FISTA t-sequence** `tₖ₊₁ = (1+√(1+4tₖ²))/2, μₖ = (tₖ−1)/tₖ₊₁`
  (Beck-Teboulle 2009), with the **O'Donoghue-Candès (2015) adaptive gradient restart**
  (`∇f(y)·(x⁺−x) > 0` ⇒ reset t + velocity) — restores the linear rate on strongly-convex problems without
  knowing κ.
- `MomentumOptions{step (required α), momentum (fixed or <0 = auto), adaptive_restart}`.
- **Convergence semantics (documented in the header):** Nesterov's only gradient is at the lookahead, so the
  optimality test uses it; when `‖∇f(y)‖∞ ≤ grad_tol` the returned iterate IS y (gradient and iterate agree
  exactly). The bottom stall/flat tests use the same proxy norm — a Success there is within `(1+αL)·grad_tol`
  of the iterate gradient. Heavy-ball has no proxy.
- **Determinism moat:** serial scalar recurrences; only the objective eval is parallel-but-bit-exact ⇒
  trajectory bit-identical across worker counts.
- Added to the `opt.hpp` umbrella. These are the FULL-gradient deterministic forms; stochastic SGD+momentum
  is v7-i.

### Tests (`tests/hesap-opt/test_momentum.cpp`) — v7-f totals: **91 asserts / 7 cases** (CG 3 + momentum 4)

1. **The acceleration THEOREM as the sharp gate:** on the 1-D-Laplacian quadratic n=64 (exact spectrum known,
   κ ≈ 1712), optimally-tuned heavy-ball (`α = 4/(√λmax+√λmin)², μ = ((√κ−1)/(√κ+1))²`) and parameter-free
   NAG each reach `grad_tol=1e-8` in **> 4× fewer iterations** than plain fixed-step GD at the same `α = 1/L`
   (true ratio ~√κ ≈ 41; 4× is the safe floor). A wrong μ schedule / restart / velocity update fails this
   where a "does it converge" check passes.
2. **Smooth convex non-quadratic:** log-cosh (L=1, f*=0 exactly) — default path + fixed-μ + restart-off branches.
3. **Determinism moat `{1,2,4,8,16}`:** NAG over `ParallelSparseLinearOp` quadratic — trajectory + iteration
   count bit-identical across worker counts (non-vacuous: κ~1700 ⇒ many iterations).
4. **Boundary (SANITY #3):** n=0 (immediate Success, zero evals) + n=1 (both variants).

## Verification (module-local per the standing directive; CI owns the full sweep)

| Config | Result |
|---|---|
| win-debug | hesap-opt suite **370 asserts / 38 cases** pass; hesap-direct suite **598 861 / 190** pass; 5 source guards pass via ctest |
| win-asan | hesap-opt suite pass (DLL PATH fix) |
| win-shipping | hesap-opt suite pass — after a **clean rebuild** (see below) |
| win-tidy | builds clean after the `kTriPanel` fix (below) |

## En-route findings (not this slice's code)

### 1. win-shipping silent-stale-obj landmine — ROOT-CAUSED (`msvc_deps_prefix` locale mismatch)

The first win-shipping link failed: `test_lm_sparse.cpp.obj` referenced the OLD 4-param
`SupernodalCholesky::factorize` (pre-`reuse_symbolic`, June-7 obj) and ninja had NOT recompiled it after the
June-8 header change. `ninja -t deps` showed **`#deps 0 (VALID)`** — the dir records ZERO header dependencies
for every TU. Mechanism: `rules.ninja` in win-shipping says `msvc_deps_prefix = Note: including file:`
(English — it was configured by the VS-bundled CMake), but this host's cl.exe emits the **Turkish**
`Not: eklenen dosya:`, so ninja never matches a `/showIncludes` line and stores empty deps ⇒ header changes
NEVER trigger recompiles in that dir. Audit of all build dirs: **win-shipping + win-tidy-local are broken**
(English prefix + MSVC); all other MSVC dirs carry the Turkish prefix (healthy — configured by the standalone
CMake under the Turkish locale); the two clang-cl dirs are CORRECT with the English prefix (clang-cl is not
localized). **FIXED IN-SESSION (no debt — the user's hard "no debts" rule):** wiped both dirs, reconfigured
with the standalone CMake (detected prefix now `Not: eklenen dosya:` in both — verified in `rules.ninja`),
fresh-built the win-shipping test closure and **verified deps are now recorded** (`ninja -t deps` on
`test_lm_sparse.cpp.obj`: `#deps 0` → `#deps 95`) + suite green. A reconfigure alone fixes only future
compiles — the wipe was required to purge the empty-deps objs. Recorded in CLAUDE.md Troubleshooting + the
Sanity Ledger (scar #2 family: a stale obj almost shipped a phantom "shipping breaks" verdict — and conversely
can ship phantom greens).

### 2. Pre-existing tidy error in committed v7-e-2 code

`supernodal_cholesky.cpp:1332` `constexpr crd::u32 kTriPanel` violates the local-constant lower_case rule
(`readability-identifier-naming`). Renamed → `tri_panel` (identifier-only; semantics byte-identical). It was
committed in `b261478` because win-tidy hadn't seen that TU since the edit. After the fix: hesap-direct suite
598 861 asserts green in win-debug; hesap-opt suite re-verified on debug + asan + shipping.

## Decisions

- Eval-parity bench vs scipy `CG` stays deferred to v7-z (per the conjugate_gradient.hpp header note) — plain
  nonlinear CG/NAG are the same algorithms as the references, so eval-parity is the honest ceiling and the
  `{1..16}` moat is the differentiator; no new bench this slice.
- Nesterov optimality-test-at-lookahead chosen over an extra per-iteration gradient at the iterate
  (the standard practice; semantics documented in the header + bounded by `(1+αL)·grad_tol`).

## Next

The next v7 subslice: Newton family (v7-g) / trust-region (v7-h), or jump to the constrained spine
v7-j → v7-k QP (OSQP-class) → v7-n NLP (SQP/IPOPT-class) — the consumer-pull ⭐ path.
