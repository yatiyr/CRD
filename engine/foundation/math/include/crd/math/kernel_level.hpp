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

// CRD_KERNEL_LEVEL_SUFFIX names the level in the identifiers a kernel unit defines.
#if !defined(CRD_KERNEL_LEVEL)
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

// The name of the kernel table a unit of this level defines: CRD_KERNEL_TABLE(kFoo) is kFooAvx2 in an AVX2 unit.
#define CRD_KERNEL_TABLE_JOIN(base, level) base##level
#define CRD_KERNEL_TABLE_NAME(base, level) CRD_KERNEL_TABLE_JOIN(base, level)
#define CRD_KERNEL_TABLE(base) CRD_KERNEL_TABLE_NAME(base, CRD_KERNEL_LEVEL_SUFFIX)

// Whether a family's mask of built levels (CRD_KERNELS_<FAMILY>, defined on the owning archive by
// crd_add_cpu_kernels; bit n is CpuLevel n) holds a level: CRD_KERNEL_BUILT(CRD_KERNELS_FLOAT_CONVERT, AVX2).
#define CRD_KERNEL_BUILT(mask, level) ((((mask) >> CRD_KERNEL_LEVEL_##level) & 1) != 0)
// NOLINTEND(cppcoreguidelines-macro-usage)
