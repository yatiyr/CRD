// SIMD backend selection — Phase 3.1 v0a (ADR-0063 + ADR-0065 substrate).
//
// Backend is decided by `CRD_SIMD_TARGET`, which the CMake module
// `cmake/CrdSimd.cmake` defines on the `crd-simd-flags` interface target.
// Anything that links `crd-math` (transitively almost everything) inherits
// the macro along with the matching `/arch:AVX2` / `-mavx2` / NEON flag.
//
// We do NOT auto-detect from compiler-defined ISA macros (`__AVX2__` etc.)
// because MSVC does not define those unless the user passes `/arch:AVX2`,
// which made earlier versions of this header silently fall back to SSE2 on
// every non-shipping config. Going through the CMake module forces an
// explicit, reportable choice.

#pragma once

#include <crd/core/platform.hpp>
#include <crd/core/types.hpp>

#define CRD_SIMD_BACKEND_SCALAR 0
#define CRD_SIMD_BACKEND_SSE2   1
#define CRD_SIMD_BACKEND_AVX2   2
#define CRD_SIMD_BACKEND_NEON   3

#if defined(CRD_SIMD_FORCE_SCALAR) && CRD_SIMD_FORCE_SCALAR
    #define CRD_SIMD_BACKEND CRD_SIMD_BACKEND_SCALAR
#elif defined(CRD_SIMD_TARGET)
    // Defined by cmake/CrdSimd.cmake. Authoritative source.
    #if   CRD_SIMD_TARGET == CRD_SIMD_BACKEND_AVX2
        #define CRD_SIMD_BACKEND CRD_SIMD_BACKEND_AVX2
    #elif CRD_SIMD_TARGET == CRD_SIMD_BACKEND_SSE2
        #define CRD_SIMD_BACKEND CRD_SIMD_BACKEND_SSE2
    #elif CRD_SIMD_TARGET == CRD_SIMD_BACKEND_NEON
        #define CRD_SIMD_BACKEND CRD_SIMD_BACKEND_NEON
    #else
        #define CRD_SIMD_BACKEND CRD_SIMD_BACKEND_SCALAR
    #endif
#else
    // Fallback only fires when `crd-simd-flags` is somehow not linked. We
    // pick the safest baseline per arch and emit a one-time compiler note
    // so the configure summary is the canonical source of truth.
    #if CRD_ARCH_X64
        #define CRD_SIMD_BACKEND CRD_SIMD_BACKEND_SSE2
    #elif CRD_ARCH_ARM64
        #define CRD_SIMD_BACKEND CRD_SIMD_BACKEND_NEON
    #else
        #define CRD_SIMD_BACKEND CRD_SIMD_BACKEND_SCALAR
    #endif
#endif

#define CRD_SIMD_HAS_SSE2  (CRD_SIMD_BACKEND == CRD_SIMD_BACKEND_SSE2 || CRD_SIMD_BACKEND == CRD_SIMD_BACKEND_AVX2)
#define CRD_SIMD_HAS_AVX2  (CRD_SIMD_BACKEND == CRD_SIMD_BACKEND_AVX2)
#define CRD_SIMD_HAS_NEON  (CRD_SIMD_BACKEND == CRD_SIMD_BACKEND_NEON)
#define CRD_SIMD_IS_SCALAR (CRD_SIMD_BACKEND == CRD_SIMD_BACKEND_SCALAR)

#if CRD_SIMD_HAS_AVX2
    #include <immintrin.h>
#elif CRD_SIMD_HAS_SSE2
    #include <emmintrin.h>
#elif CRD_SIMD_HAS_NEON
    #include <arm_neon.h>
#endif

namespace crd::math::simd
{
using crd::f32;
using crd::f64;
using crd::usize;

// Lane widths for the SIMD types. Vec4f is 4 lanes on every backend (one
// 128-bit register on SSE2/AVX2/NEON, four-element array on scalar).
// Vec8f is 8 lanes (one 256-bit register on AVX2; two 128-bit registers
// composed on SSE2/NEON; eight-element array on scalar).
inline constexpr usize k_vec4f_lanes = 4;
inline constexpr usize k_vec8f_lanes = 8;

// Native lane width for the host: AVX2 → 8, others → 4. Drives AoSoA
// layout choices in v0b.
#if CRD_SIMD_HAS_AVX2
inline constexpr usize k_native_lane_width = 8;
#else
inline constexpr usize k_native_lane_width = 4;
#endif

// Compile-time-known backend name. Used by smokes / startup logs to verify
// the binary was actually compiled with the SIMD level the configure
// summary advertised.
[[nodiscard]] inline constexpr const char* backend_name() noexcept
{
#if CRD_SIMD_HAS_AVX2
    return "AVX2";
#elif CRD_SIMD_HAS_SSE2
    return "SSE2";
#elif CRD_SIMD_HAS_NEON
    return "NEON";
#else
    return "SCALAR";
#endif
}

// Whether the binary was compiled under the ADR-0063 deterministic FP
// contract. CrdSimd.cmake defines CRD_DETERMINISTIC_FP=1 in that mode.
[[nodiscard]] inline constexpr bool deterministic_fp() noexcept
{
#if defined(CRD_DETERMINISTIC_FP) && CRD_DETERMINISTIC_FP
    return true;
#else
    return false;
#endif
}

// Determinism contract (ADR-0063): SIMD ops must produce bit-exact same
// results vs scalar reference for ADD / SUB / MUL / NEG / SQRT / MIN / MAX.
// Hardware FMA (single rounding) is deliberately NOT used in mul_add — we
// emit (a*b) + c with two roundings on every backend so SIMD/scalar parity
// holds. Reductions (dot, horizontal_sum) use a fixed pairwise tree that
// matches the scalar reference order.
}
