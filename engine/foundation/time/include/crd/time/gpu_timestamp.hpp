#pragma once

// ---------------------------------------------------------------------------
// crd-time -- GPU timestamp delegation API (Detour D-006).
//
// **Important: this is a thin API surface ONLY.** The actual GPU timestamp
// implementation belongs in a per-backend GPU-context module (`crd-gpu-context-vulkan`, using `VkQueryPool` with
// `VK_QUERY_TYPE_TIMESTAMP` + `vkCmdWriteTimestamp` + `vkGetQueryPoolResults`; the D3D12 query-heap analogue in
// `crd-gpu-context-dx12`) -- DIAG.6b(i). The old `crd-rhi-vulkan` RHI was retired (ADR-0105). `crd-time` keeps the
// platform/backend separation clean: this header declares the type shape; implementation lives where the GPU sits.
//
// The API surface is opaque-handle-based:
//
//   GpuTimestampHandle h = renderer.begin_gpu_timing(cmd_buffer, "MyPass");
//   // ... record GPU commands ...
//   renderer.end_gpu_timing(cmd_buffer, h);
//   // Some frames later (after the GPU has finished):
//   Duration gpu_elapsed = renderer.resolve_gpu_timing(h);
//
// **`crd-time` does NOT call the Vulkan APIs.** It just provides the types
// and conventions that the renderer/profiler agree on. D-003 profiler wires
// up the actual Vulkan-side capture + resolve.
// ---------------------------------------------------------------------------

#include <crd/core/types.hpp>
#include <crd/time/duration.hpp>

namespace crd::time
{

// Opaque handle for a pair of GPU timestamps (begin + end). The actual
// underlying storage is a `VkQueryPool` slot pair, owned by the renderer
// / profiler.
//
// Sentinel value (default-constructed) = invalid handle (cannot resolve).
struct GpuTimestampHandle
{
    crd::u32 value = 0xFFFF'FFFFU;

    [[nodiscard]] constexpr bool is_valid() const noexcept { return value != 0xFFFF'FFFFU; }
};

// The "resolved" pair of GPU timestamp values, in GPU-ticks. Convert to
// Duration via the GPU's timestamp period (Vulkan: `VkPhysicalDeviceLimits::timestampPeriod`,
// in nanoseconds per tick).
struct GpuTimestampValues
{
    crd::u64 begin_ticks = 0;
    crd::u64 end_ticks   = 0;
};

// Convenience: convert raw GPU-tick delta to a Duration using the device's
// timestamp period.
[[nodiscard]] inline constexpr Duration gpu_ticks_to_duration(
    crd::u64 delta_ticks, crd::f64 ns_per_tick) noexcept
{
    return Duration{static_cast<crd::f64>(delta_ticks) * ns_per_tick * 1.0e-9};
}

[[nodiscard]] inline constexpr Duration gpu_timestamp_elapsed(
    GpuTimestampValues values, crd::f64 ns_per_tick) noexcept
{
    const crd::u64 delta = values.end_ticks - values.begin_ticks;
    return gpu_ticks_to_duration(delta, ns_per_tick);
}

// ---------------------------------------------------------------------------
// DIAG.6b(d): CPU<->GPU clock calibration.
//
// A calibration is a single instant sampled on BOTH clocks (Vulkan
// `vkGetCalibratedTimestampsEXT`, which also reports `maxDeviation`; the D3D12
// `ID3D12CommandQueue::GetClockCalibration` pair, whose deviation the caller
// bounds by bracketing the CPU read). It carries its own tick period so a GPU
// tick can be placed on the CPU timeline WITHOUT subtracting an unrelated
// CPU-submit timestamp from a GPU-execute timestamp -- that forbidden
// subtraction (design: "do not subtract unrelated CPU/GPU timestamps") is
// exactly what a fabricated offset would be.
struct GpuClockCalibration
{
    crd::i64 cpu_ns          = 0;   // CPU monotonic ns at the calibration instant
    crd::u64 gpu_ticks       = 0;   // GPU tick count at the same instant
    crd::f64 ns_per_tick     = 0.0; // this device's timestamp period (Vulkan timestampPeriod)
    crd::u64 max_deviation_ns = 0;  // reported bound on how far apart the two reads could be
};

// constexpr round-half-away-from-zero. std::llround is NOT constexpr on MSVC, and an implicit f64->i64 narrowing trips
// /W4 /WX -- so round explicitly with explicit casts.
[[nodiscard]] inline constexpr crd::i64 gpu_round_ns(crd::f64 x) noexcept
{
    return x >= 0.0 ? static_cast<crd::i64>(x + 0.5) : static_cast<crd::i64>(x - 0.5);
}

static_assert(gpu_round_ns(0.0) == 0, "round(0) == 0");
static_assert(gpu_round_ns(0.5) == 1, "round-half-away (+)");
static_assert(gpu_round_ns(-0.5) == -1, "round-half-away (-)");
static_assert(gpu_round_ns(1.4) == 1, "round down");
static_assert(gpu_round_ns(1.6) == 2, "round up");
static_assert(gpu_round_ns(-1.6) == -2, "round up (-)");

// DIAG.6b(f): the SIGNED delta `ticks - ref` in a `valid_bits`-wide modular GPU counter. Hardware timestamp counters are
// often narrower than 64 bits (Vulkan `timestampValidBits` can be e.g. 32/36/40); a counter that wrapped within a frame
// yields `ticks < ref` numerically while the true delta is small and positive. Masking to the window and sign-extending
// recovers it. `valid_bits >= 64` is the full-width case: the plain u64 subtract + cast IS the signed delta (and
// `1ULL << 64` would be UB, so it is branched away).
[[nodiscard]] inline constexpr crd::i64 gpu_tick_delta(crd::u64 ticks, crd::u64 ref, crd::u32 valid_bits) noexcept
{
    if (valid_bits >= 64U)
    {
        return static_cast<crd::i64>(ticks - ref);
    }
    const crd::u64 mask = (static_cast<crd::u64>(1) << valid_bits) - static_cast<crd::u64>(1);
    const crd::u64 d    = (ticks - ref) & mask;
    const crd::u64 half = static_cast<crd::u64>(1) << (valid_bits - 1U);
    return d >= half ? static_cast<crd::i64>(d) - static_cast<crd::i64>(mask + static_cast<crd::u64>(1))
                     : static_cast<crd::i64>(d);
}

static_assert(gpu_tick_delta(5U, 0xFFFF'FFF0ULL, 32U) == 21, "32-bit forward across wrap");
static_assert(gpu_tick_delta(0xFFFF'FFF0ULL, 5U, 32U) == -21, "32-bit backward across wrap");
static_assert(gpu_tick_delta(100U, 40U, 32U) == 60, "32-bit no wrap");
static_assert(gpu_tick_delta(5U, 0xFFFF'FFF0ULL, 64U) == static_cast<crd::i64>(5ULL - 0xFFFF'FFF0ULL),
              "64-bit does not wrap at 32");

// Place a GPU tick count on the CPU timeline using one calibration. The tick delta is the modular signed delta (see
// gpu_tick_delta) so a narrow counter that wrapped is placed correctly; a span may also begin before the calibration
// instant, which is legal (it just extrapolates backward). valid_bits defaults to 64 (full-width, the pre-(f) behavior).
[[nodiscard]] inline constexpr crd::i64 gpu_ticks_to_cpu_ns(const GpuClockCalibration& calib, crd::u64 ticks,
                                                            crd::u32 valid_bits = 64U) noexcept
{
    const crd::i64 tick_delta = gpu_tick_delta(ticks, calib.gpu_ticks, valid_bits);
    return calib.cpu_ns + gpu_round_ns(static_cast<crd::f64>(tick_delta) * calib.ns_per_tick);
}

// The honest uncertainty bound for a span placed via `cur`. With one calibration the bound is `cur.max_deviation_ns`.
// With a previous calibration, the two samples reveal the real relative drift over the interval between them --
// `observed_drift = (cur.cpu - prev.cpu) - round((cur.ticks - prev.ticks) * period)` -- and the bound widens by
// |observed_drift| (a measured quantity, never a fabricated ppm constant). If the span predates `cur` it is bracketed
// by prev..cur, so prev's own deviation is added too.
[[nodiscard]] inline crd::u64 gpu_span_uncertainty_ns(const GpuClockCalibration* prev,
                                                      const GpuClockCalibration& cur,
                                                      crd::u64                   begin_ticks,
                                                      crd::u32                   valid_bits = 64U) noexcept
{
    crd::u64 bound = cur.max_deviation_ns;
    if (prev != nullptr)
    {
        const crd::i64 cpu_delta  = cur.cpu_ns - prev->cpu_ns;
        const crd::i64 tick_delta = gpu_tick_delta(cur.gpu_ticks, prev->gpu_ticks, valid_bits);
        const crd::i64 gpu_delta  = gpu_round_ns(static_cast<crd::f64>(tick_delta) * cur.ns_per_tick);
        const crd::i64 drift      = cpu_delta - gpu_delta;
        bound += drift >= 0 ? static_cast<crd::u64>(drift) : static_cast<crd::u64>(-drift);
        if (gpu_tick_delta(begin_ticks, cur.gpu_ticks, valid_bits) < 0) // begin predates cur (modular)
        {
            bound += prev->max_deviation_ns;
        }
    }
    return bound;
}

} // namespace crd::time
