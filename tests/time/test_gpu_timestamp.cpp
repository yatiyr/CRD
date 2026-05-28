// crd-time -- GPU timestamp API surface tests.
//
// These tests verify the API shape only; actual GPU timestamp capture
// lives in crd-rhi-vulkan + D-003 profiler. crd-time provides the
// types + conversion helpers.

#include <crd/time/gpu_timestamp.hpp>

#include <catch2/catch_test_macros.hpp>

namespace
{
using namespace crd::time;
using crd::f64;
using crd::u64;
} // namespace

TEST_CASE("GpuTimestampHandle: default-constructed is invalid",
          "[d-006][gpu-timestamp]")
{
    GpuTimestampHandle h;
    CHECK(!h.is_valid());
}

TEST_CASE("GpuTimestampHandle: explicit value is valid",
          "[d-006][gpu-timestamp]")
{
    GpuTimestampHandle h{42};
    CHECK(h.is_valid());
    CHECK(h.value == 42);
}

TEST_CASE("gpu_ticks_to_duration: conversion is correct",
          "[d-006][gpu-timestamp]")
{
    // Vulkan timestampPeriod is in ns/tick. NVIDIA RTX 4090 reports ~1.0 ns/tick.
    constexpr f64 ns_per_tick = 1.0;
    constexpr u64 k_delta = 5'000'000;  // 5 million ticks
    Duration d = gpu_ticks_to_duration(k_delta, ns_per_tick);
    CHECK(d.value == 5'000'000.0 * 1.0 * 1e-9);  // 5 ms
    CHECK(d.value == 0.005);
}

TEST_CASE("gpu_ticks_to_duration: different timestamp periods",
          "[d-006][gpu-timestamp]")
{
    // Intel iGPU might have e.g. 83.3 ns/tick (12 MHz clock).
    constexpr f64 ns_per_tick = 83.333;
    constexpr u64 k_delta = 1'000'000;
    Duration d = gpu_ticks_to_duration(k_delta, ns_per_tick);
    CHECK(d.value > 0.08);
    CHECK(d.value < 0.09);
}

TEST_CASE("gpu_timestamp_elapsed: from a GpuTimestampValues pair",
          "[d-006][gpu-timestamp]")
{
    GpuTimestampValues values{1000, 11000};  // 10000 tick delta
    constexpr f64 ns_per_tick = 1.0;
    Duration d = gpu_timestamp_elapsed(values, ns_per_tick);
    CHECK(d.value == 10'000.0 * 1e-9);  // 10 microseconds
    CHECK(d.value == 1e-5);
}
