#pragma once

#include <crd/core/platform.hpp>

// Keep unoptimized generated leaves as calls. Forced inlining combines their expression
// temporaries into multi-megabyte caller frames (clang-cl /Od, REPO.3c.8). Optimized builds
// retain the original inlining policy and arithmetic; no larger application stack is required.
#if defined(NDEBUG) || defined(__OPTIMIZE__)
#define CRD_FFT_CODELET_INLINE CRD_FORCEINLINE
#else
#define CRD_FFT_CODELET_INLINE inline
#endif
