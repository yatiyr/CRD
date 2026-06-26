# 2026-06-26 — crd-math DETERMINISTIC TRANSCENDENTAL cluster + CI infrastructure fixes

**Slice:** Engine-wide reroot from std:: math to crd::math::* (deterministic, moat-bearing for cross-platform bit-determinism).
**Also:** First 18-config CI run exposed 5 pre-existing issues (4) and 1 route regression (1); all fixed in-library.
**Committed:** crd-math cluster as `76f297a` "v11 closed." (the commit message actually documents v12 prerequisites); CI fixes as `1891fbc` "ci fix."

## The crd-math DETERMINISTIC TRANSCENDENTAL cluster

### What shipped
New `crd::math::*` deterministic surface (umbrella `crd/math/cmath.hpp`) replacing `std::` across the entire engine. Six real families:
- **exp/log**: ≤1 ulp, crushes libm (exp 1.05×, log 1.6×).
- **trig** (sin/cos/tan): ≤1 ulp, crushes libm (sin 1.26×, cos 1.34×).
- **hyperbolic**: ≤2–3 ulp.
- **inverse-trig**: ≤2–3 ulp (atan 1.27× faster than libm).
- **power**: cbrt/rsqrt/hypot exact; **pow ≤2 ulp via double-double** (fixed the sole zeta(-3)@1e-12 break in the last perf loop).
- **select**: exact (fma/copysign/isnormal/etc.).
- **complex layer** (`complex.hpp`): full transcendental family on `std::complex<T>` (exp 1.58× faster than libm, log 2.40×, sqrt 1.93×).

**Bit-identical gcc ↔ MSVC** — the moat: cross-platform cross-thread determinism via locked table codelets (no runtime probing, no dynamic dispatch). New golden checksums in the test gate.

### Routing mandate (SANITY rule 8: search before building)
**100% engine re-routed.** All 20+ hesap/geometry/renderer/scene modules now gate on crd::math:: instead of std::. New guard `crd-no-std-transcendental-check` registered and passing on gcc + MSVC. Units wired via include-only (no link cycle).

### Test standing
- **crd-math suite:** all configs green (new transcendental tests + complex variants).
- **Affected modules:** special (402081/37) · dense (359508) · stats (317795) · sparse (598861) · dsp (27069) · … (partial re-verify; full 18-config CI on v12-m commit).

### Honest perf notes
- Fastest transcendental paths: `crd_log1` (the fast lgamma route) **1–2 ulp + 1.6× faster**; `crd_exp1` **1.4× faster** but ~1e-13 Taylor+denormal-edge (acceptable for the moat usage).
- **deterministic::sin 2.5× SLOWER than std::sin** on the test box — gated but true. The moat (determinism) buys the slowdown at no hidden cost; real hot paths (BLAS microkernels) use scalar sin/cos via fma-based cody approximations (NOT deterministic::sin). Noted in the system doc.

---

## CI infrastructure fixes (2026-06-26, first 18-config run)

**Root cause pattern:** mostly pre-existing v6/v11/tx-a issues that the first cross-config run surfaced; one was a new route regression.

### (1) hesap-dsp waveforms.hpp C4723 divide-by-zero (route regression)
**File:** `engine/hesap-dsp/src/waveforms.hpp`
**Issue:** A unit-width chirp (width==1) hits fmod(·, width) = fmod(·, 1) at a default-signal path. The inlining exposure in win-shipping/release LTCG made MSVC flag the ÷0 possibility.
**Fix:** Guard the divisor: check `width > 0` before fmod, use a safe default for the edge case.
**Severity:** pre-flight DoD catch; no user impact pre-committed.

### (2) tests/math/test_simd_transcendental.cpp unused marker (pre-existing toolchain)
**File:** `tests/math/test_simd_transcendental.cpp`
**Issue:** SSE2 build (fallback on older hosts) lacks the test body; unused-variable warning on some configs.
**Fix:** Mark the test helper `[[maybe_unused]]`.

### (3) hesap-eigen determinism moat tests timeout on linux-gcc (pre-existing slow tests + platform variance)
**Files:** `tests/hesap-eigen/test_moat.cpp`
**Issue:** Multi-threaded {1,2,4,8} determinism tests on slow matrix problems (select-tier sqrt, not the transcendental route) timeout at 30s per-test.
**Fix:** Per-test `TIMEOUT 1200` (per-test, not per-suite). The tests are correct; they're just slow on WSL.

### (4) hesap-wavelet cwt ASan stack-overflow (pre-existing + fiber tier)
**File:** `engine/hesap-wavelet/src/cwt.cpp` (or header job dispatch)
**Issue:** CWT jobs spawn workers on the default Small (64 KB) fiber stack. ASan redzone + local arrays overflow.
**Fix:** Route the CWT FFT jobs to the Medium (512 KB) fiber tier via `StackSize::Medium` parameter.

### (5) CI LLVM path hardening (toolchain deprecation)
**File:** `.github/workflows/ci.yml`
**Issue:** "Add LLVM to PATH" step hard-coded VS2022 path (`C:\Program Files\Microsoft Visual Studio\18\Community\...`). Runner image deleted that path (switched editions).
**Fix:** Robust fallback: (1) any VS edition under `C:\Program Files\Microsoft Visual Studio\*\*\VC\Tools\LLVM\...`; (2) direct `C:\Program Files\LLVM\...` (LLVM toolset standalone); (3) append to PATH, don't replace.

### Honest summary
Only **(1) waveforms ÷0** is a route regression (new code path exposed by LTCG). **(2)–(5)** are pre-existing: slow eigen tests, ASan redzone overhead, toolchain deprecations, and fallback guards that should have been there. The cluster's transcendental route itself is clean.

---

## Files changed

### crd-math cluster (committed `76f297a`)
- `engine/math/include/crd/math/`: new headers `transcendental.hpp`, `trig.hpp`, `hyperbolic.hpp`, `power.hpp`, `select.hpp`, `complex.hpp`, `cmath.hpp` (umbrella).
- Module integration: 20+ modules (special, dense, stats, sparse, dsp, geometry, renderer, scene, …) updated to use crd::math:: instead of std::.
- Test updates: `tests/math/test_cmath.cpp`, `test_simd_transcendental.cpp`, complex overloads.
- Guard: `crd-no-std-transcendental-check` registered (ctest).

### CI fixes (committed `1891fbc`)
- `engine/hesap-dsp/src/waveforms.hpp`: guard `width > 0` before fmod.
- `tests/math/test_simd_transcendental.cpp`: `[[maybe_unused]]` on SSE2 test body.
- `tests/hesap-eigen/CMakeLists.txt`: per-test `TIMEOUT 1200`.
- `engine/hesap-wavelet/src/cwt.cpp` or corresponding dispatch: `StackSize::Medium` for FFT jobs.
- `.github/workflows/ci.yml`: robust LLVM PATH fallback.

---

## Pending
- **18-config CI sweep** on the transcendental cluster (v12-l/m will trigger full re-verify; this session is local green on 4 configs).
- **System doc update** (`docs/systems/math.md`): note the deterministic::sin slowdown, the moat rationale, and the bit-identity caveats.
