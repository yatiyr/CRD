# crd-math Transcendental Library — Cluster Plan (DRAFT)

> **Deterministic, faster-than-libm transcendental + math-primitive kernel that the WHOLE engine routes through.**
> One owner (`crd-math`), one API (`crd::math::*`), one bit-reproducible result on every platform.
> Status: PROPOSED (2026-06-25). ADR pending. Replaces scattered `std::exp/sin/cos/pow/…` across the engine.

## 0. Why (the case, ranked)

1. **It closes the last hole in the determinism MOAT.** `std::exp`/`sin`/`pow` are **not bit-identical across platforms** (glibc ≠ MSVC ≠ Apple ≠ WASM, and they drift between versions). Every consumer that calls libm is platform-dependent — so the "bit-identical, certifiable everywhere" story (DO-178C / ISO 26262 / FDA) is only *actually* true once Cerid owns its transcendentals. This is the headline reason; speed is the bonus.
2. **Speed.** ~1 ulp minimax (vs libm's correctly-rounded 0.5 ulp + full edge handling) → **solid 1.3–2× scalar, decisive crush batched/SIMD** (libm has no vectorized loop).
3. **WASM / browser goal.** No libm dependency, no asm — a portable C++ minimax kernel is exactly what the browser target needs (relaxed-SIMD pinned strict for determinism).
4. **Single guarded surface.** One place to audit, vectorize, and certify; a guard forbids `std::` transcendentals re-opening the hole.

## 1. The contract (every function obeys this)

- **Accuracy: ≤ 1 ulp** default (≤ 2 ulp permitted for the genuinely hard ones — general `pow`, large-arg trig), measured vs **MPFR/mpmath ground truth**. A few precision-critical consumers (special-fn internals) may opt into a **high-accuracy tier** (≤ 0.51 ulp) — so the design carries *two tiers*, not one.
- **Determinism (the moat):** bit-identical across {gcc, clang, MSVC} × {x64, ARM, WASM} × {scalar ↔ each SIMD width} × {1..N threads}. Enforced by **`-ffp-contract=off`** on the kernel TUs (already the discipline for the existing log/exp) + no `Math.random`-class state. This is a *gated* property, not a hope.
- **Edge cases are correct, not ignored:** NaN / ±Inf / subnormal / sign-of-zero / huge args handled exactly — via the **fast common path + fallback** pattern `crd_log1` already uses (cheap reduction on the hot path; extended/`std::` fallback on the rare edge). A "fast exp" that's wrong on Inf is not shippable.
- **Raw lower layer (ADR-0078):** these are `f32`/`f64` (and `Vec*`) primitives — no `Quantity<>` tags. Consumers bridge at their surface.

## 2. What already exists (build on, don't restart)

- `crd/math/simd/transcendental.hpp` — **deterministic scalar+SIMD `log` & `exp`** (Cephes range reduction, ln2 hi/lo split, FMA, edge fallback). The pattern + home are set.
- `crd/math/deterministic.hpp` — f32 log/exp + erf/erfc/lgamma (reuse — do NOT rebuild erf/gamma; SANITY rule 8).
- `crd/math/simd/vec*.hpp` — `Vec4f/Vec8f/Vec4d` (the SIMD substrate).
- v12 minimax pipeline (Chebyshev fit + Lawson reweight → monomial `.inc`, gated) — **the exact tool to generate these**, just proven 7/7 on Boost.

## 3. The catalog (target: complete; f32 + f64 scalar + SIMD per function)

**Tier S — select/round (exact; value = unified API + SIMD + defined NaN/sign semantics, NOT speed):**
`min · max · clamp · abs · copysign · sign · floor · ceil · round · trunc · nearbyint · fmod · remainder · ldexp · frexp · scalbn · fma · sqrt(hw) · saturate · lerp`

**Tier 1 — exp/log:** `exp · exp2 · exp10 · expm1 · log · log2 · log10 · log1p` *(exp/log mostly done — complete the rest)*

**Tier 2 — power/root:** `pow · powi(integer) · cbrt · rsqrt(refined) · hypot` *(pow: fast exact paths for integer & half-integer exponents first; general real pow last & hardest)*

**Tier 3 — trig:** `sin · cos · sincos · tan · cot` *(Cody–Waite reduction on the hot small-arg path; Payne–Hanek/fallback for huge args)*

**Tier 4 — inverse trig:** `asin · acos · atan · atan2`

**Tier 5 — hyperbolic:** `sinh · cosh · tanh · asinh · acosh · atanh`

**Reuse (note, don't rebuild):** `erf/erfc/erfinv` (math::deterministic), `lgamma/tgamma/digamma` + the special functions (hesap-special).

## 4. Sub-slices (each: generate → gate vs MPFR + libm + SLEEF → bench all peers → moat-proof)

| slice | scope | gold gate / peers |
|---|---|---|
| **tx-a** | **Substrate + policy.** Unified `crd::math::` scalar+SIMD API skeleton; the two-tier accuracy contract; the gen+gate harness (MPFR/mpmath ulp oracle); the **`crd-no-std-transcendental` guard (authored, OFF)**; the determinism harness; complete exp2/exp10/expm1/log2/log10/log1p. | MPFR ulp · glibc/MSVC libm |
| **tx-b** | **Trig** sin/cos/sincos/tan (f32+f64+SIMD) + range reduction. | MPFR · libm · SLEEF |
| **tx-c** | **Inverse trig** asin/acos/atan/atan2. | MPFR · libm · SLEEF |
| **tx-d** | **Power/root** pow (powi + half-integer fast paths + general), cbrt, rsqrt, hypot. | MPFR · libm (honest: general pow may be parity) |
| **tx-e** | **Hyperbolic** sinh/cosh/tanh/asinh/acosh/atanh. | MPFR · libm · SLEEF |
| **tx-f** | **Select/round tier** min/max/clamp/abs/floor/… (exact, SIMD, defined semantics). | semantics + SIMD bit-exact |
| **tx-g** | **ROUTING + ENFORCE.** Migrate every engine consumer `std::*`→`crd::math::*` module-by-module; **re-run each consumer's gold-standard gates** (the ~1-ulp shift must not break tight gates); flip the guard **ON**; write the enforcement (§6). | every consumer suite stays green |
| **tx-z** | **Close.** Full board (all fns × libm/SLEEF/MATLAB, ulp + ns) · **cross-platform determinism proof** (the 18-config CI is essential here) · system doc `docs/systems/math-transcendental.md` · ADR. | the scoreboard + the moat proof |

## 5. Honest caveats (set expectations — SANITY rule 6/9)

- **"Faster every time" is true batched, solid scalar, NOT a blowout.** Modern libm scalar `exp` is ~10–20 cy; we win 1.3–2× by dropping correct-rounding + edge cost. The 4–8× wins are SIMD-batched.
- **General `pow` may land at parity, not crush** — accurate `pow` needs double-double internally and libm's is genuinely good. We *crush* integer/half-integer powers (the common case in stats/geometry) and aim parity-or-better on general real `pow`. Said up front, not buried.
- **Large-argument `sin/cos` reduction** (Payne–Hanek) is the subtle correctness trap; the hot path is small-arg, the rare huge-arg path falls back. Don't pay PH on every call.
- **The big risk is tx-g, not the kernels:** swapping the transcendental under hesap-stats/dsp/geometry shifts results ~1 ulp, which can disturb a *tight* gate (1e-15 / bit-exact moats). Every consumer's gate must be re-verified. The cross-thread moats survive (the new fns are deterministic too); the vs-reference gates need a re-gate pass. This is the real cost.
- It's a **multi-slice mini-SLEEF**, not a patch. Do it right.

## 6. Enforcement (the policy you asked for — applied in tx-a/tx-g)

- **`docs/PRINCIPLES.md`** — new pinned cornerstone: *"All elementary math + transcendentals in engine/tool code route through `crd::math::*`. `std::exp/log/sin/cos/tan/pow/…` are forbidden outside `crd-math`'s own implementation + its edge fallbacks. Reason: cross-platform bit-determinism (the moat) + speed + WASM."*
- **`docs/SANITY.md`** — a rule + ledger entry; the guard is the check.
- **`CLAUDE.md`** — a Hard-rules bullet ("use `crd::math::*`, never `std::` transcendentals in engine code").
- **`AGENTS.md`** + a **memory `reference` entry** — so every agent inherits it.
- **ctest guard `crd-no-std-transcendental-check`** (sibling of `no-std-sort`/`no-std-math`): greps engine/tool sources for `std::(exp|log|sin|cos|tan|pow|…)`, **exempts** `crd-math`'s kernel + fallbacks, fails the build otherwise. **Turned ON only at tx-g** (after routing — else it breaks the build day one).

## 7. Sequencing & recommendation

- The guard goes **ON last** (tx-g) — author it OFF in tx-a, enforce after migration.
- Two viable orders relative to v12 stats: **(A)** do tx now (closes the moat hole before more code piles on `std::exp`); **(B)** finish v12 stats l→z first, then tx. Leaning (A) — every slice added on `std::` is one more re-route later — but (B) keeps current momentum. Either is defensible.
- This subsumes the 4 parity distribution rows (exponential/weibull/gumbel) — they crush as a free side-effect of tx-a/tx-d.

## 8. Open decisions (need your call)

1. **Now (A) or after v12 l→z (B)?**
2. **One accuracy tier (≤1 ulp) or two (add a ≤0.51-ulp high-accuracy path for special-fn internals)?** Two is safer, more work.
3. **Reference SLEEF/Cephes for range-reduction recipes (study, not transcribe) — confirm that's acceptable** (it's the standard reference for this exact problem; coefficients we generate ourselves).
4. **Guard strictness:** hard-fail at tx-g, or warn-then-fail over a grace slice?

## 9. Execution — one-session orchestration (LOCKED 2026-06-25)

User decisions locked: **detour from hesap NOW, finish the cluster this session.** **Normal CI is sufficient** — cross-platform determinism is proven by committed **golden-value tests** every CI platform must reproduce (no special 18-config sweep). Decision answers: **(A)** now, kernels first · **one** accuracy tier (~1 ulp), high-accuracy added lazily only where a re-gate demands · SLEEF/Cephes as **reference** (study, not transcribe; coefficients generated by our v12 pipeline) · guard **hard-fails at tx-g**.

Orchestration (parallel forks where independent):
1. **tx-a (foundation — sequential, first):** the `crd::math::` scalar+SIMD API surface + the two-tier contract + the gen→gate(mpmath ulp)→bench(libm/SLEEF) harness + the **golden-value determinism test** (committed expected bits, platform-agnostic = the moat proof under normal CI) + complete `exp2/exp10/expm1/log2/log10/log1p` + **author the guard OFF** + **write the enforcement policy + THE AGENT RULE (§10)**. Everything downstream replicates this pattern, so it must be exemplary + documented.
2. **tx-b … tx-f (parallel forks, after tx-a lands the pattern):** trig · inverse-trig · power · hyperbolic · select-tier. Each: scalar f32/f64 + SIMD, gated vs mpmath ≤1 ulp, benched vs libm (+SLEEF where present), golden determinism test.
3. **COMMIT GATE:** the uncommitted v12 batch is committed BEFORE tx-g (tx-g edits v12 files that call `std::exp`). Kernels tx-a..f are new crd-math files — safe on uncommitted v12.
4. **tx-g (routing + enforce — sequential, careful):** migrate every consumer `std::*`→`crd::math::*` module-by-module; **re-run each consumer's gold gates** (the ~1 ulp shift must not break a tight gate — the real cost of the cluster); flip the guard ON.
5. **tx-z (close):** full board (all fns × libm/SLEEF/MATLAB; ulp + ns) · ADR · `docs/systems/math-transcendental.md`.

## 9b. tx-a OUTCOME + REFRAME (2026-06-25) — UPGRADE, not build-from-scratch

tx-a searched+measured first (rule 8/5) and found the premise partly wrong: **Cerid already ships deterministic transcendentals in 3 partial homes** — `crd::math::deterministic::` (sin/cos/tan/asin/acos/atan/exp/exp2/log/log2/pow, f32+f64+SIMD) + the newer `crd_exp1`/`crd_log1` f64 cores + the new `transcendental.hpp` facade. So the cluster is **UPGRADE + UNIFY + complete-gaps + route**, not build.

**Audit (gcc -O3 -march=native -ffp-contract=off; ulp vs mpmath, ns/call):** `crd_log1` = **1–2 ulp, 1.6× faster than libm** ✅ (the proof that deterministic+faster+accurate is reachable via explicit `fma`). `crd_exp1` = fast but **~1e-13 (Taylor, hundreds of ulp) + denormal-broken** (x≲−745 → garbage) ❌. legacy `deterministic::sin` = **2.5× SLOWER than libm** ❌ (bit-exact but unoptimized). ⟹ the engine ALREADY pays a trig tax for the moat; this cluster *removes* it.

**Revised slice list (replaces tx-b..f framing):**
- **tx-exp** — upgrade `crd_exp1` → minimax ≤1 ulp + **fix the denormal exponent-injection bug**; whole exp family (exp2/exp10/expm1) rides it. *(highest-value — running now)*
- **tx-trig** — the headline: rebuild sin/cos/sincos/tan to `crd_log1`-class (explicit `fma`, Cody–Waite reduction) = **faster-than-libm AND ≤1 ulp AND deterministic**. (legacy trig is slower — this is real work, not "add sin".)
- **tx-gaps** — atan2/sincos + sinh/cosh/tanh/asinh/acosh/atanh + cbrt/hypot/rsqrt + select tier.
- **tx-unify** — collapse the 3 homes into ONE canonical `crd::math::*` surface (pick the namespace; `deterministic::` + `crd_*` + facade → one).
- **tx-route+guard** — route consumers, broaden the existing `check_no_std_math` guard to the hesap-* siblings, **flip hard-enforce ONLY after faster-than-libm is proven** (tx-a correctly did NOT enforce a slower path).

tx-a delivered (build-safe, unwired): the `transcendental.hpp` exp/log facade (log family ≤2 ulp; exp family pending the minimax upgrade), the reusable **mpmath ulp-gate harness** (`tests/math/gen_transcendental_refs.py`), and the Mandate as *direction* (memory + SANITY ledger) — guard/PRINCIPLES/CLAUDE cornerstones deferred to tx-route (don't enforce a slower path).

## 10. THE AGENT RULE — "Cerid Math Mandate" (written to PRINCIPLES/SANITY/CLAUDE/AGENTS/memory at tx-route, once faster-than-libm holds)

> All engine + tool code uses `crd::math::*` for EVERY elementary math + transcendental (exp/log/sin/cos/tan/pow/sqrt/floor/min/max/…). `std::` math is **FORBIDDEN** outside `crd-math`'s own kernel + its edge fallbacks. **If you need a function `crd::math` does not yet have, you STOP and IMPLEMENT it there first — deterministic, ≤1 ulp (gated vs mpmath), benchmarked vs libm — BEFORE using it or going further. You do NOT fall back to `std::`, and you do NOT proceed without it.** Enforced by the `crd-no-std-transcendental` ctest guard. Reason: cross-platform bit-determinism (the certification moat) + speed + WASM.
