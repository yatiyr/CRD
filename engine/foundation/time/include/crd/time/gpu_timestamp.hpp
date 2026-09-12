#pragma once

// ---------------------------------------------------------------------------
// crd-time -- GPU timestamp delegation API (Detour D-006).
//
// **Important: this is a thin API surface ONLY.** The actual GPU timestamp
// implementation lives in `crd-rhi-vulkan` (which uses `VkQueryPool` with
// `VK_QUERY_TYPE_TIMESTAMP` + `vkCmdWriteTimestamp` + `vkGetQueryPoolResults`).
// `crd-time` keeps the platform/backend separation clean: this header
// declares the type shape; implementation lives where the GPU sits.
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

} // namespace crd::time
