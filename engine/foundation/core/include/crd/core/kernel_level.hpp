// The CPU level of the unit being compiled (SIMD-DISPATCH, ADR-0134 §5).
//
// A kernel unit is compiled once per CPU level by crd_add_cpu_kernels (cmake/CrdSimd.cmake), with CRD_KERNEL_LEVEL set
// to the level's crd::CpuLevel value and that level's ISA flags; every other unit is general code. CRD_MATH_LEVEL_NS
// names the inline namespace into which a header shared by general code and kernel units puts its inline functions,
// so that no copy a kernel unit compiles shares a symbol, and with it a linker-merged body, with another level's copy
// or with general code's. This header includes nothing.

#pragma once

// NOLINTBEGIN(cppcoreguidelines-macro-usage): the level reaches the preprocessor and names a namespace.
#define CRD_KERNEL_LEVEL_PORTABLE 0
#define CRD_KERNEL_LEVEL_SSE2 1
#define CRD_KERNEL_LEVEL_SSE42 2
#define CRD_KERNEL_LEVEL_AVX2 3
#define CRD_KERNEL_LEVEL_AVX512 4
#define CRD_KERNEL_LEVEL_NEON 5

// CRD_KERNEL_LEVEL_SUFFIX names the level in the identifiers a kernel unit defines. A feature variant
// (crd_add_cpu_kernels VARIANTS: CRD_KERNEL_FEATURE_<FEATURE> on its level's unit) has a namespace and suffix of its
// own, so its copies never merge with the level's plain unit; CRD_KERNEL_HAS_<FEATURE> tells its bodies apart.
#if defined(CRD_KERNEL_FEATURE_AVXVNNI)
#if !defined(CRD_KERNEL_LEVEL) || CRD_KERNEL_LEVEL != CRD_KERNEL_LEVEL_AVX2
#error "AVX-VNNI is a feature variant of the avx2 level"
#endif
#define CRD_KERNEL_HAS_AVXVNNI 1
#define CRD_MATH_LEVEL_NS avx2_avxvnni
#define CRD_KERNEL_LEVEL_SUFFIX Avx2AvxVnni
#elif defined(CRD_KERNEL_FEATURE_AVX512VNNI)
#if !defined(CRD_KERNEL_LEVEL) || CRD_KERNEL_LEVEL != CRD_KERNEL_LEVEL_AVX512
#error "AVX512-VNNI is a feature variant of the avx512 level"
#endif
#define CRD_KERNEL_HAS_AVX512VNNI 1
#define CRD_MATH_LEVEL_NS avx512_avx512vnni
#define CRD_KERNEL_LEVEL_SUFFIX Avx512Avx512Vnni
#elif defined(CRD_KERNEL_FEATURE_PCLMULQDQ)
#if !defined(CRD_KERNEL_LEVEL) || CRD_KERNEL_LEVEL != CRD_KERNEL_LEVEL_SSE42
#error "PCLMULQDQ is a feature variant of the sse4.2 level"
#endif
#define CRD_KERNEL_HAS_PCLMULQDQ 1
#define CRD_MATH_LEVEL_NS sse42_pclmulqdq
#define CRD_KERNEL_LEVEL_SUFFIX Sse42Pclmulqdq
#elif defined(CRD_KERNEL_FEATURE_VPCLMULQDQ)
#if !defined(CRD_KERNEL_LEVEL) ||                                                                                      \
    (CRD_KERNEL_LEVEL != CRD_KERNEL_LEVEL_AVX2 && CRD_KERNEL_LEVEL != CRD_KERNEL_LEVEL_AVX512)
#error "VPCLMULQDQ is a feature variant of the avx2 and avx512 levels"
#endif
#define CRD_KERNEL_HAS_PCLMULQDQ 1
#define CRD_KERNEL_HAS_VPCLMULQDQ 1
#if CRD_KERNEL_LEVEL == CRD_KERNEL_LEVEL_AVX2
#define CRD_MATH_LEVEL_NS avx2_vpclmulqdq
#define CRD_KERNEL_LEVEL_SUFFIX Avx2Vpclmulqdq
#else
#define CRD_MATH_LEVEL_NS avx512_vpclmulqdq
#define CRD_KERNEL_LEVEL_SUFFIX Avx512Vpclmulqdq
#endif
#elif defined(CRD_KERNEL_FEATURE_CRC32)
#if !defined(CRD_KERNEL_LEVEL) || CRD_KERNEL_LEVEL != CRD_KERNEL_LEVEL_NEON
#error "CRC32 is a feature variant of the neon level"
#endif
#define CRD_KERNEL_HAS_CRC32 1
#define CRD_MATH_LEVEL_NS neon_crc32
#define CRD_KERNEL_LEVEL_SUFFIX NeonCrc32
#elif defined(CRD_KERNEL_FEATURE_PMULL)
#if !defined(CRD_KERNEL_LEVEL) || CRD_KERNEL_LEVEL != CRD_KERNEL_LEVEL_NEON
#error "PMULL is a feature variant of the neon level"
#endif
#define CRD_KERNEL_HAS_CRC32 1 // the PMULL variant selects on CRC32 too
#define CRD_KERNEL_HAS_PMULL 1
#define CRD_MATH_LEVEL_NS neon_pmull
#define CRD_KERNEL_LEVEL_SUFFIX NeonPmull
#elif !defined(CRD_KERNEL_LEVEL)
#define CRD_MATH_LEVEL_NS general
#elif CRD_KERNEL_LEVEL == CRD_KERNEL_LEVEL_PORTABLE
#define CRD_MATH_LEVEL_NS portable
#define CRD_KERNEL_LEVEL_SUFFIX Portable
#elif CRD_KERNEL_LEVEL == CRD_KERNEL_LEVEL_SSE2
#define CRD_MATH_LEVEL_NS sse2
#define CRD_KERNEL_LEVEL_SUFFIX Sse2
#elif CRD_KERNEL_LEVEL == CRD_KERNEL_LEVEL_SSE42
#define CRD_MATH_LEVEL_NS sse42
#define CRD_KERNEL_LEVEL_SUFFIX Sse42
#elif CRD_KERNEL_LEVEL == CRD_KERNEL_LEVEL_AVX2
#define CRD_MATH_LEVEL_NS avx2
#define CRD_KERNEL_LEVEL_SUFFIX Avx2
#elif CRD_KERNEL_LEVEL == CRD_KERNEL_LEVEL_AVX512
#define CRD_MATH_LEVEL_NS avx512
#define CRD_KERNEL_LEVEL_SUFFIX Avx512
#elif CRD_KERNEL_LEVEL == CRD_KERNEL_LEVEL_NEON
#define CRD_MATH_LEVEL_NS neon
#define CRD_KERNEL_LEVEL_SUFFIX Neon
#else
#error "CRD_KERNEL_LEVEL names no CPU level"
#endif
#if !defined(CRD_KERNEL_HAS_AVXVNNI)
#define CRD_KERNEL_HAS_AVXVNNI 0
#endif
#if !defined(CRD_KERNEL_HAS_AVX512VNNI)
#define CRD_KERNEL_HAS_AVX512VNNI 0
#endif
#if !defined(CRD_KERNEL_HAS_PCLMULQDQ)
#define CRD_KERNEL_HAS_PCLMULQDQ 0
#endif
#if !defined(CRD_KERNEL_HAS_VPCLMULQDQ)
#define CRD_KERNEL_HAS_VPCLMULQDQ 0
#endif
#if !defined(CRD_KERNEL_HAS_CRC32)
#define CRD_KERNEL_HAS_CRC32 0
#endif
#if !defined(CRD_KERNEL_HAS_PMULL)
#define CRD_KERNEL_HAS_PMULL 0
#endif

// The name of the kernel table a unit of this level defines: CRD_KERNEL_TABLE(kFoo) is kFooAvx2 in an AVX2 unit.
#define CRD_KERNEL_TABLE_JOIN(base, level) base##level
#define CRD_KERNEL_TABLE_NAME(base, level) CRD_KERNEL_TABLE_JOIN(base, level)
#define CRD_KERNEL_TABLE(base) CRD_KERNEL_TABLE_NAME(base, CRD_KERNEL_LEVEL_SUFFIX)

// The instruction set the unit being compiled targets, for code whose choice of instruction is not a kernel table's:
// CRD_TARGET_X64 is 1 on x64 (ARM64EC, which compiles x64 code as AArch64, excluded), and CRD_TARGET_X64_FMA is 1 when
// such a unit may also issue the FMA instruction. That is GCC and Clang's -mfma (the avx2 and avx512 levels' x86-64-v3
// and -v4) and MSVC's /arch:AVX2 and up, which defines __AVX2__ but no __FMA__. Both are 0 on every other target.
#if (defined(__x86_64__) || defined(_M_X64)) && !defined(_M_ARM64EC)
#define CRD_TARGET_X64 1
#if (defined(_MSC_VER) && defined(__AVX2__)) || (!defined(_MSC_VER) && defined(__FMA__))
#define CRD_TARGET_X64_FMA 1
#else
#define CRD_TARGET_X64_FMA 0
#endif
#else
#define CRD_TARGET_X64 0
#define CRD_TARGET_X64_FMA 0
#endif

// 1 in general code of an x64 build without FMA: the scalar transcendental families reach their bodies through
// crd-math's scalar table (crd/math/scalar_kernels.hpp), so a CPU with FMA runs them with the instruction inline.
#if !defined(CRD_KERNEL_LEVEL) && CRD_TARGET_X64 && !CRD_TARGET_X64_FMA
#define CRD_MATH_SCALAR_DISPATCH 1
#else
#define CRD_MATH_SCALAR_DISPATCH 0
#endif

// Whether a family's mask of built levels (CRD_KERNELS_<FAMILY>, defined on the owning archive by
// crd_add_cpu_kernels; bit n is CpuLevel n) holds a level: CRD_KERNEL_BUILT(CRD_KERNELS_FLOAT_CONVERT, AVX2).
#define CRD_KERNEL_BUILT(mask, level) ((((mask) >> CRD_KERNEL_LEVEL_##level) & 1) != 0)
// NOLINTEND(cppcoreguidelines-macro-usage)
