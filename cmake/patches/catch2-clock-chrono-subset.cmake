# catch2-clock-chrono-subset — applied by crd_patched_copy() to Catch2/src/catch2/benchmark/catch_clock.hpp (3.7.1).
#
# Every assertion reaches this header: IResultCapture (interfaces/catch_interfaces_capture.hpp) declares
# benchmarkEnded(BenchmarkStats<> const&), whose default Duration is Benchmark::FDuration, defined here. It needs only
# std::chrono::duration, the ratio aliases and steady_clock, but it included the whole of <chrono>, which in C++20
# brings the calendar, the time-zone database and <format>: with catch2-chrono-stringmaker-opt-in applied, the
# 2026-10-18 profile measured this include at 0.47 s of a test unit's 0.89 s of Catch2 on MSVC
# (docs/bench/2026-10-18-build-speed-compile-time.md). Both standard libraries the build uses keep exactly that part in
# a header of their own, which their <chrono>, <thread> and <mutex> include: MSVC's <__msvc_chrono.hpp> and
# libstdc++'s <bits/chrono.h>. The copy includes that header when the library provides it and <chrono> otherwise (any
# other library, or a version without it); <ratio>, already part of both, comes first so the library's own macros
# decide. The declarations are the same either way, so every unit sees the same types; Catch2's sources and headers
# that use the rest of <chrono> (catch_config.hpp, catch_benchmark.hpp, catch_timer.cpp) include it themselves.
crd_patch_replace(
    [[#define CATCH_CLOCK_HPP_INCLUDED

#include <chrono>
]]
    [[#define CATCH_CLOCK_HPP_INCLUDED

// crd-patch catch2-clock-chrono-subset: the duration and clock part of <chrono> (cmake/patches/catch2-clock-chrono-subset.cmake)
#include <ratio>
#if defined(_MSVC_STL_VERSION) && __has_include(<__msvc_chrono.hpp>)
#include <__msvc_chrono.hpp>
#elif defined(__GLIBCXX__) && __has_include(<bits/chrono.h>)
#include <bits/chrono.h>
#else
#include <chrono>
#endif
]])
