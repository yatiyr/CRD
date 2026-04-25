#pragma once

#include <crd/core/assert.hpp> // for CRD_WHILE_FALSE
#include <crd/core/build_config.hpp>
#include <crd/core/platform.hpp>
#include <crd/log/log_channel.hpp>
#include <crd/log/log_level.hpp>
#include <crd/log/logger.hpp>

#include <format>
#include <source_location>
#include <string_view>

// CRD_LOG_IMPL: the single shared expansion every CRD_LOG_xxx macro funnels through.
// It is wrapped in a CRD_WHILE_FALSE so that
//     if (cond) CRD_LOG_INFO(g_log_x, "..."); else foo();
// is well-formed (no dangling-else footgun).
//
// Ordering matters:
//  1) compile-time level check  -> selects whether the whole block is `((void)0)`
//  2) runtime should_log()      -> one byte compare; arguments are NOT yet evaluated
//  3) std::format(...)          -> only happens on the rare "we are logging" path
//  4) detail::dispatch(...)     -> push to queue (async) or fan-out (sync)
#define CRD_LOG_IMPL(LEVEL_ENUM, CHANNEL_REF, FMT, ...)                                                                \
    do                                                                                                                 \
    {                                                                                                                  \
        if (::crd::log::detail::should_log((CHANNEL_REF), (LEVEL_ENUM)))                                               \
        {                                                                                                              \
            ::crd::log::detail::dispatch((LEVEL_ENUM), (CHANNEL_REF), ::std::source_location::current(),               \
                                         ::std::string_view(::std::format(FMT __VA_OPT__(, ) __VA_ARGS__)));           \
        }                                                                                                              \
    }                                                                                                                  \
    CRD_WHILE_FALSE

// ---- Per-level macros -----------------------------------------------------
// Each pair selects either real expansion or `((void)0)` based on
// CRD_LOG_MIN_LEVEL_NUM (set at configure time).

#if CRD_LOG_LEVEL_TRACE >= CRD_LOG_MIN_LEVEL_NUM
#define CRD_LOG_TRACE(CH, FMT, ...) CRD_LOG_IMPL(::crd::log::LogLevel::Trace, (CH), FMT __VA_OPT__(, ) __VA_ARGS__)
#else
#define CRD_LOG_TRACE(CH, FMT, ...) ((void)0)
#endif

#if CRD_LOG_LEVEL_DEBUG >= CRD_LOG_MIN_LEVEL_NUM
#define CRD_LOG_DEBUG(CH, FMT, ...) CRD_LOG_IMPL(::crd::log::LogLevel::Debug, (CH), FMT __VA_OPT__(, ) __VA_ARGS__)
#else
#define CRD_LOG_DEBUG(CH, FMT, ...) ((void)0)
#endif

#if CRD_LOG_LEVEL_INFO >= CRD_LOG_MIN_LEVEL_NUM
#define CRD_LOG_INFO(CH, FMT, ...) CRD_LOG_IMPL(::crd::log::LogLevel::Info, (CH), FMT __VA_OPT__(, ) __VA_ARGS__)
#else
#define CRD_LOG_INFO(CH, FMT, ...) ((void)0)
#endif

#if CRD_LOG_LEVEL_WARN >= CRD_LOG_MIN_LEVEL_NUM
#define CRD_LOG_WARN(CH, FMT, ...) CRD_LOG_IMPL(::crd::log::LogLevel::Warn, (CH), FMT __VA_OPT__(, ) __VA_ARGS__)
#else
#define CRD_LOG_WARN(CH, FMT, ...) ((void)0)
#endif

#if CRD_LOG_LEVEL_ERROR >= CRD_LOG_MIN_LEVEL_NUM
#define CRD_LOG_ERROR(CH, FMT, ...) CRD_LOG_IMPL(::crd::log::LogLevel::Error, (CH), FMT __VA_OPT__(, ) __VA_ARGS__)
#else
#define CRD_LOG_ERROR(CH, FMT, ...) ((void)0)
#endif

// CRITICAL is never compiled out -- it's the engine's last-words channel.
#define CRD_LOG_CRITICAL(CH, FMT, ...)                                                                                 \
    CRD_LOG_IMPL(::crd::log::LogLevel::Critical, (CH), FMT __VA_OPT__(, ) __VA_ARGS__)

// ---- Conditional / once helpers ------------------------------------------

// Log only if cond is true. Cond is evaluated only when the level is enabled.
#define CRD_LOG_IF(COND, LEVEL_MACRO, CH, FMT, ...)                                                                    \
    do                                                                                                                 \
    {                                                                                                                  \
        if (COND)                                                                                                      \
        {                                                                                                              \
            LEVEL_MACRO((CH), FMT __VA_OPT__(, ) __VA_ARGS__);                                                         \
        }                                                                                                              \
    }                                                                                                                  \
    CRD_WHILE_FALSE

// Fire at most once for the lifetime of the program (per call site).
#define CRD_LOG_ONCE(LEVEL_MACRO, CH, FMT, ...)                                                                        \
    do                                                                                                                 \
    {                                                                                                                  \
        static bool crd_log_once_fired = false;                                                                        \
        if (!crd_log_once_fired)                                                                                       \
        {                                                                                                              \
            crd_log_once_fired = true;                                                                                 \
            LEVEL_MACRO((CH), FMT __VA_OPT__(, ) __VA_ARGS__);                                                         \
        }                                                                                                              \
    }                                                                                                                  \
    CRD_WHILE_FALSE
