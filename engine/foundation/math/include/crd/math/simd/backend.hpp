// SIMD backend selection (ADR-0063, ADR-0065, ADR-0134 §5).
//
// A kernel unit (CRD_KERNEL_LEVEL, compiled once per CPU level by crd_add_cpu_kernels in cmake/CrdSimd.cmake) gets
// the backend of its own level: portable is the scalar reference, sse2 and sse4.2 the 128-bit backends, avx2 the
// 256-bit one, and avx512 the 256-bit one plus 512-bit registers for Vec16f, Vec8d and Vec16i (vec16f.hpp, vec8d.hpp,
// vec16i.hpp; composed of two 256-bit halves below it). General code gets CRD_SIMD_TARGET, which
// crd-simd-flags defines with the matching ISA flags; it is never read from compiler ISA macros (`__AVX2__`), which
// MSVC defines only under /arch.
//
// Every type and function of these headers lives in the unit's level namespace (CRD_MATH_LEVEL_NS,
// crd/math/kernel_level.hpp), so a copy compiled for one level never provides another level's symbol. The headers
// therefore call no inline function of the standard library: lane math goes through crd/math/libm.hpp.

#pragma once

#include <crd/core/platform.hpp>
#include <crd/core/types.hpp>
#include <crd/math/kernel_level.hpp>

// CRD_SIMD_UNIT_LEVEL: the CRD_KERNEL_LEVEL_* value whose backend this unit's types use.
#if defined(CRD_KERNEL_LEVEL)
    #define CRD_SIMD_UNIT_LEVEL CRD_KERNEL_LEVEL
#elif defined(CRD_SIMD_TARGET)
    // Defined by cmake/CrdSimd.cmake (0 scalar, 1 sse2, 2 avx2, 3 neon). Authoritative source.
    #if CRD_SIMD_TARGET == 2
        #define CRD_SIMD_UNIT_LEVEL CRD_KERNEL_LEVEL_AVX2
    #elif CRD_SIMD_TARGET == 1
        #define CRD_SIMD_UNIT_LEVEL CRD_KERNEL_LEVEL_SSE2
    #elif CRD_SIMD_TARGET == 3
        #define CRD_SIMD_UNIT_LEVEL CRD_KERNEL_LEVEL_NEON
    #else
        #define CRD_SIMD_UNIT_LEVEL CRD_KERNEL_LEVEL_PORTABLE
    #endif
#elif CRD_ARCH_X64
    // Only when crd-simd-flags is not linked: the architecture's baseline.
    #define CRD_SIMD_UNIT_LEVEL CRD_KERNEL_LEVEL_SSE2
#elif CRD_ARCH_ARM64
    #define CRD_SIMD_UNIT_LEVEL CRD_KERNEL_LEVEL_NEON
#else
    #define CRD_SIMD_UNIT_LEVEL CRD_KERNEL_LEVEL_PORTABLE
#endif

// The backend rungs; each holds on the rungs above it (HAS_SSE2 on the SSE4.2, AVX2 and AVX-512 backends, HAS_SSE42 on
// AVX2 and AVX-512, HAS_AVX2 on AVX-512, whose 256-bit types are the AVX2 ones).
#define CRD_SIMD_HAS_AVX512 (CRD_SIMD_UNIT_LEVEL == CRD_KERNEL_LEVEL_AVX512)
#define CRD_SIMD_HAS_AVX2  (CRD_SIMD_UNIT_LEVEL == CRD_KERNEL_LEVEL_AVX2 || CRD_SIMD_HAS_AVX512)
#define CRD_SIMD_HAS_SSE42 (CRD_SIMD_UNIT_LEVEL == CRD_KERNEL_LEVEL_SSE42 || CRD_SIMD_HAS_AVX2)
#define CRD_SIMD_HAS_SSE2  (CRD_SIMD_UNIT_LEVEL == CRD_KERNEL_LEVEL_SSE2 || CRD_SIMD_HAS_SSE42)
#define CRD_SIMD_HAS_NEON  (CRD_SIMD_UNIT_LEVEL == CRD_KERNEL_LEVEL_NEON)
#define CRD_SIMD_IS_SCALAR (!CRD_SIMD_HAS_SSE2 && !CRD_SIMD_HAS_NEON)

#if CRD_SIMD_HAS_AVX2
    #include <immintrin.h>
#elif CRD_SIMD_HAS_SSE42
    #include <nmmintrin.h>
#elif CRD_SIMD_HAS_SSE2
    #include <emmintrin.h>
#elif CRD_SIMD_HAS_NEON
    #include <arm_neon.h>
#endif

namespace crd::math::simd::inline CRD_MATH_LEVEL_NS
{
using crd::f32;
using crd::f64;
using crd::usize;

// Lane widths for the SIMD types. Vec4f is 4 lanes on every backend (one
// 128-bit register on SSE2/SSE4.2/AVX2/NEON, four-element array on scalar).
// Vec8f is 8 lanes (one 256-bit register on AVX2; two 128-bit registers
// composed on SSE2/SSE4.2/NEON; two four-element arrays on scalar).
inline constexpr usize kVec4fLanes = 4;
inline constexpr usize kVec8fLanes = 8;

// The AoSoA lane width (Soa chunks, eylem body and collider columns). It is a data layout, so it is the same on every
// level and backend (ADR-0134 §5): one binary and the data it cooks agree whichever level a CPU runs. Eight is the
// widest level's f32 register; the 128-bit backends process a chunk column as two halves.
inline constexpr usize kNativeLaneWidth = 8;

// The backend this unit's types compiled for. Smokes and startup logs check it against the configure summary, and
// the cross-level tests against each kernel unit's level.
[[nodiscard]] constexpr const char* backend_name() noexcept
{
#if CRD_SIMD_HAS_AVX512
    return "AVX-512";
#elif CRD_SIMD_HAS_AVX2
    return "AVX2";
#elif CRD_SIMD_HAS_SSE42
    return "SSE4.2";
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
[[nodiscard]] constexpr bool deterministic_fp() noexcept
{
#if defined(CRD_DETERMINISTIC_FP) && CRD_DETERMINISTIC_FP
    return true;
#else
    return false;
#endif
}

// Determinism contract (ADR-0063, ADR-0134 §7): every operation of these types gives the same bits on every backend
// for every input, NaN payloads aside (IEEE 754 leaves the payload of a two-NaN operation open). Hardware FMA is
// used only by fma()/fnmadd()/fmaddsub(), which are single-rounded on every backend; mul_add is (a*b) + c with two
// roundings. Reductions (dot, horizontal_sum) use a fixed pairwise tree that matches the scalar reference order.
} // namespace crd::math::simd::inline CRD_MATH_LEVEL_NS
