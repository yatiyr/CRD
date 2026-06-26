#pragma once

// crd-math — THE deterministic <cmath> replacement, and the unified scalar surface. ONE include for all of
// crd::math::* (exp/log families · trig + inverse trig · hyperbolic · pow/cbrt/hypot/rsqrt · the exact select/round
// tier). Engine + tool code includes THIS instead of <cmath> and calls crd::math::<fn> instead of std::<fn>
// (the Cerid Math Mandate — docs/PRINCIPLES.md; the crd-no-std-transcendental guard enforces it).
//
// Why: every crd::math::<fn> is deterministic (-ffp-contract=off + explicit fma ⇒ bit-identical on every
// platform/compiler — the certification moat), ≤ a few ulp vs mpmath (gated in tests/math/test_transcendental.cpp),
// and faster-than-libm where measured (exp 1.05× · log 1.6× · sin 1.26× · cos 1.34× · atan 1.27× · cbrt 1.71×).

#include <crd/math/complex.hpp>        // complex exp log sqrt pow sin cos tan sinh cosh tanh abs arg polar norm conj
#include <crd/math/hyperbolic.hpp>     // sinh cosh tanh asinh acosh atanh
#include <crd/math/power.hpp>          // pow cbrt hypot rsqrt
#include <crd/math/select.hpp>         // min max clamp abs floor ceil round trunc nearbyint lround fmod sqrt copysign sign lerp fma
#include <crd/math/transcendental.hpp> // exp exp2 exp10 expm1 log log2 log10 log1p (+ trig.hpp: sin cos sincos tan atan asin acos atan2)
