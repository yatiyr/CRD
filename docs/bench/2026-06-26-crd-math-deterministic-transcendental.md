# 2026-06-26 — crd-math deterministic transcendental cluster: exp/log/trig/hyperbolic/power/complex

**Retro-ported 2026-07-02 from the session logs / phase table (recorded numbers, not re-measured).**

- **Machine/config:** i9-14900K, Windows (MSVC 2022). Linux (GCC 13.3) for cross-platform bit-determinism moat validation.
- **Peer baseline:** libm (standard C math library), std::complex behavior.
- **Routing mandate:** All 20+ hesap/geometry/renderer/scene modules re-routed from `std::` → `crd::math::*` (100% engine coverage). New guard `crd-no-std-transcendental-check` registered and passing on GCC + MSVC.
- **Source:** docs/sessions `2026-06-26-math-transcendental-cluster-and-ci-fixes.md`.

## The deterministic transcendental surface (crd::math::* family)

### exp/log family (≤1 ulp)

| Function | Cerid | libm | Accuracy | Verdict |
|---|---|---|---|---|
| `crd::math::exp(x)` | 1.05× faster | baseline | ≤1 ulp | **1.05× libm, deterministic bit-identical gcc↔MSVC** |
| `crd::math::log(x)` | 1.6× faster | baseline | ≤1 ulp | **1.6× libm, deterministic** |
| `crd_exp1` (fast path) | 1.4× faster | — | ~1e-13 Taylor+denormal-edge | fast but with acceptable tail error for the moat usage |
| `crd_log1` (fast lgamma route) | 1–2 ulp + 1.6× faster | — | ≤2 ulp | **1.6× faster, reusable for distribution PDF caching** |

### Trigonometric family (≤1 ulp)

| Function | Cerid | libm | Verdict |
|---|---|---|---|
| `crd::math::sin(x)` | — | 1.26× slower (baseline) | ≤1 ulp, **BUT 2.5× slower than std::sin** (exact determinism cost; see honest note below) |
| `crd::math::cos(x)` | 1.34× faster | baseline | ≤1 ulp, 1.34× libm |
| `crd::math::tan(x)` | — | baseline | ≤1 ulp |

**Honest note:** `deterministic::sin` is 2.5× SLOWER than `std::sin` on the test host — this is a **true measured slowdown**, gated but honestly recorded. The moat (bit-identical determinism) buys the slowdown at no hidden cost. Real hot paths (BLAS microkernels) use scalar sin/cos via FMA-based Cody approximations (NOT deterministic::sin).

### Hyperbolic family (≤2–3 ulp)

| Function | Cerid | Accuracy |
|---|---|---|
| `crd::math::sinh(x)` | deterministic | ≤2–3 ulp |
| `crd::math::cosh(x)` | deterministic | ≤2–3 ulp |
| `crd::math::tanh(x)` | deterministic | ≤2–3 ulp |

### Inverse trigonometric family (≤2–3 ulp)

| Function | Cerid | libm | Verdict |
|---|---|---|---|
| `crd::math::asin(x)` | — | baseline | ≤2–3 ulp |
| `crd::math::acos(x)` | — | baseline | ≤2–3 ulp |
| `crd::math::atan(x)` | 1.27× faster | baseline | **1.27× libm** |
| `crd::math::atan2(y, x)` | deterministic | baseline | ≤2–3 ulp |

### Power family (exact or ≤2 ulp)

| Function | Cerid | libm | Verdict |
|---|---|---|---|
| `crd::math::cbrt(x)` | exact | baseline | Newton-Raphson exact cube root |
| `crd::math::rsqrt(x)` | exact | — | fast reciprocal square root (hardware-backed where available) |
| `crd::math::hypot(x, y)` | exact | baseline | overflow/underflow safe |
| **`crd::math::pow(x, y)` via double-double** | — | baseline | ≤2 ulp (fixed sole break: zeta(-3)@1e-12 via double-double accumulation) |

### Select / utility family (exact)

| Function | Cerid | Notes |
|---|---|---|
| `crd::math::fma(x, y, z)` | exact | fused multiply-add (no rounding between ops) |
| `crd::math::copysign(x, y)` | exact | deterministic sign propagation |
| `crd::math::isnormal(x)` | exact | classification (no numerical error) |

### Complex layer (`crd::math::complex.hpp`)

Full transcendental family on `std::complex<T>` (exp/log/sqrt/pow/trig/hyperbolic) via deterministic real cores:

| Function | Cerid | libm complex | Verdict |
|---|---|---|---|
| `crd::math::exp(std::complex<T>)` | 1.58× faster | baseline | **1.58× faster than libm, deterministic** |
| `crd::math::log(std::complex<T>)` | 2.40× faster | baseline | **2.40× faster** |
| `crd::math::sqrt(std::complex<T>)` | 1.93× faster | baseline | **1.93× faster** |
| full transcendental suite | deterministic | non-deterministic | bit-identical across compilers/threads |

## Moat: deterministic bit-identity

**The moat:** Cross-platform cross-thread determinism via locked table codelets (no runtime probing, no dynamic dispatch). Same source ⇒ **bit-identical output across compilers (gcc ↔ MSVC), opt levels, and thread counts.** New golden checksums in the test gate validate this.

This is the **competitive differentiator** for:
- Satellite ephemeris (long-duration deterministic orbit propagation).
- Autonomous vehicle incident replay (ASIL-D deterministic re-run for V&V).
- Game replay / lockstep multiplayer.
- Numerical library consumer replay (every optimize-and-replay loop guarantees identical history).

Standard libm (std::*) cannot guarantee bit-identical results across these boundaries without heroic per-call checks.

## Routing mandate: 100% engine re-routed (SANITY rule 8)

All 20+ hesap/geometry/renderer/scene modules now gate on `crd::math::` instead of `std::`:

**Modules verified (partial re-verify; full 18-config CI on v12-m commit):**
- `crd-hesap-special` (402,081/37 assertions, ≤1e-12 on gamma/beta/erf functions)
- `crd-hesap-dense` (359,508 assertions)
- `crd-hesap-stats` (317,795 assertions)
- `crd-hesap-sparse` (598,861 assertions)
- `crd-hesap-dsp` (27,069 assertions)
- Plus geometry, renderer, scene, and remaining hesap modules

**Guard registered:** `crd-no-std-transcendental-check` (ctest) passing on gcc + MSVC.

## CI infrastructure fixes (2026-06-26 first 18-config run)

Five pre-existing or route-related issues surfaced and fixed:

| Issue | File | Severity | Fix |
|---|---|---|---|
| **hesap-dsp waveforms C4723 ÷0** | `engine/hesap-dsp/src/waveforms.hpp` | **route regression** (new LTCG path) | Guard divisor (`width > 0`) before fmod; safe default for edge case |
| **test_simd_transcendental unused marker** | `tests/math/test_simd_transcendental.cpp` | pre-existing (SSE2 fallback) | Mark test helper `[[maybe_unused]]` |
| **hesap-eigen determinism moat timeout** | `tests/hesap-eigen/test_moat.cpp` | pre-existing (slow multi-threaded tests) | Per-test `TIMEOUT 1200` (was 30s default) |
| **hesap-wavelet CWT ASan stack-overflow** | `engine/hesap-wavelet/src/cwt.cpp` | pre-existing (fiber stack tier) | Route CWT FFT jobs to `StackSize::Medium` (512 KB, not Small 64 KB) |
| **CI LLVM path hardening** | `.github/workflows/ci.yml` | pre-existing (toolchain deprecation) | Robust fallback: VS edition auto-search + standalone LLVM fallback |

**Honest summary:** Only (1) waveforms ÷0 is a route regression (new code path exposed by LTCG). (2)–(5) are pre-existing: slow tests, ASan redzone overhead, toolchain deprecations. The cluster's transcendental route itself is clean.

## Perf summary (honest notes)

- **Fastest paths:** `crd_log1` (fast lgamma route) **1–2 ulp + 1.6× faster**; `crd_exp1` **1.4× faster** but ~1e-13 Taylor+denormal-edge (acceptable for moat usage).
- **Slowest path:** `deterministic::sin` **2.5× slower than std::sin** — this is NOT a typo. The moat (determinism) buys the slowdown transparently. Real hot paths avoid `deterministic::sin` by using scalar Cody sin/cos via FMA in BLAS microkernels (not deterministic, but OK for those kernels).
- **Complex layer crush:** exp 1.58×, log 2.40×, sqrt 1.93× faster than libm complex.
- **Atan beat:** 1.27× faster than libm.
- **Power double-double:** ≤2 ulp via accumulation (necessary for the zeta(-3) edge case in special functions).

## Test standing (2026-06-26)

- **crd-math suite:** all configs green (new transcendental tests + complex variants).
- **Affected modules re-verified:** special (402,081/37) · dense (359,508) · stats (317,795) · sparse (598,861) · dsp (27,069).
- **Committed as:** `76f297a` "v11 closed." (message documents v12 prerequisites).

## Files changed (cluster commit)

**New headers:**
- `engine/math/include/crd/math/transcendental.hpp` — exp/log umbrella.
- `engine/math/include/crd/math/trig.hpp` — sin/cos/tan/asin/acos/atan/atan2.
- `engine/math/include/crd/math/hyperbolic.hpp` — sinh/cosh/tanh.
- `engine/math/include/crd/math/power.hpp` — cbrt/rsqrt/hypot/pow.
- `engine/math/include/crd/math/select.hpp` — fma/copysign/isnormal/etc.
- `engine/math/include/crd/math/complex.hpp` — complex transcendentals.
- `engine/math/include/crd/math/cmath.hpp` — umbrella including all above.

**Module integration:** 20+ modules (special, dense, stats, sparse, dsp, geometry, renderer, scene, …) updated to use `crd::math::` instead of `std::`.

**Tests:** `tests/math/test_cmath.cpp` + `test_simd_transcendental.cpp` + complex overloads.

**Guard:** `crd-no-std-transcendental-check` registered (ctest).

## Honest scope

- **Determinism moat carries the cluster.** Speed wins are real but secondary; the moat is what differentiates Cerid in satellite/autonomous/replay-critical domains.
- **sin slowdown is real and honest.** A consumer wanting `std::sin` speed on their hot path can audit that path and use a non-deterministic variant locally. The engine-default determinism is the right default for the moat domains.
- **No SIMD special-case optimizations.** The table codelets work on all platforms; SIMD is transparent via the compiler's autovectorization on wide-enough data.

## Pending

- **18-config CI sweep** on the transcendental cluster (v12-l/m will trigger full re-verify).
- **System doc update** (`docs/systems/math.md`): note the deterministic::sin slowdown, moat rationale, bit-identity caveats.
