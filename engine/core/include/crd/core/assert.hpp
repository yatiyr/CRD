#pragma once

#include <crd/core/platform.hpp>
#include <crd/core/build_config.hpp>

namespace crd::detail
{
	int report_assert_failure(const char* expression, const char* file,
		int line, const char* message = nullptr);
}

#if CRD_COMPILER_MSVC
    #define CRD_WHILE_FALSE __pragma(warning(push)) __pragma(warning(disable : 4127)) while (false) __pragma(warning(pop))
#else
    #define CRD_WHILE_FALSE while (false)
#endif

#if CRD_ENABLE_ASSERTS
    #define CRD_ASSERT(expr)                                                                                               \
        do                                                                                                                 \
        {                                                                                                                  \
            if (CRD_UNLIKELY(!(expr)))                                                                                     \
            {                                                                                                              \
                static bool crd_ignore = false;                                                                            \
                if (!crd_ignore)                                                                                           \
                {                                                                                                          \
                    int crd_result = ::crd::detail::report_assert_failure(#expr, __FILE__, __LINE__);                      \
                    if (crd_result == 2)                                                                                   \
                    {                                                                                                      \
                        CRD_DEBUGBREAK();                                                                                  \
                    }                                                                                                      \
                    else if (crd_result == 0)                                                                              \
                    {                                                                                                      \
                        crd_ignore = true;                                                                                 \
                    }                                                                                                      \
                }                                                                                                          \
            }                                                                                                              \
        } CRD_WHILE_FALSE

    #define CRD_ASSERT_MSG(expr, msg)                                                                                      \
        do                                                                                                                 \
        {                                                                                                                  \
            if (CRD_UNLIKELY(!(expr)))                                                                                     \
            {                                                                                                              \
                static bool crd_ignore = false;                                                                            \
                if (!crd_ignore)                                                                                           \
                {                                                                                                          \
                    int crd_result = ::crd::detail::report_assert_failure(#expr, __FILE__, __LINE__, msg);                 \
                    if (crd_result == 2)                                                                                   \
                    {                                                                                                      \
                        CRD_DEBUGBREAK();                                                                                  \
                    }                                                                                                      \
                    else if (crd_result == 0)                                                                              \
                    {                                                                                                      \
                        crd_ignore = true;                                                                                 \
                    }                                                                                                      \
                }                                                                                                          \
            }                                                                                                              \
        } CRD_WHILE_FALSE
#else
    #define CRD_ASSERT(expr) ((void)0)
    #define CRD_ASSERT_MSG(expr, msg) ((void)0)
#endif

#if CRD_ENABLE_ASSERTS
    #define CRD_VERIFY(expr) CRD_ASSERT(expr)
#else
    #define CRD_VERIFY(expr) ((void)(expr))
#endif

// CRD_FATAL will always be active
#define CRD_FATAL(msg)                                                                                                 \
    do                                                                                                                 \
    {                                                                                                                  \
        ::crd::detail::report_assert_failure("FATAL", __FILE__, __LINE__, msg);                                        \
        CRD_DEBUGBREAK();                                                                                              \
    } CRD_WHILE_FALSE
