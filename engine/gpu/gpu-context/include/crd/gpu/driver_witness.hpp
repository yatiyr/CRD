#pragma once

// crd-gpu-context -- the numerical-mode witness around driver and shader-compiler calls (DIAG.10c).
//
// A graphics driver, its runtime and the shader compilers are code the engine does not own, and they run on the
// engine's threads. One that leaves flush-to-zero, denormals-are-zero, another rounding mode or other exception masks
// set changes every later float result on that thread without a crash (D3D9 did this by default). Both backends wrap
// the calls that enter a driver or compiler, by kind:
//
// - CreateInstance: vkCreateInstance (DX12 has none: the factory is created on first use by DXGI).
// - CreateDevice:   D3D12CreateDevice, vkCreateDevice.
// - CompileShader:  dxc's Compile (DX12 and the Vulkan HLSL path), shaderc's compile (GLSL).
// - Submit:         ExecuteCommandLists, vkQueueSubmit (the completion seams).
// - Present:        IDXGISwapChain::Present, vkQueuePresentKHR.
//
// `witness_driver_call` reads the thread's mode before the call and after it; when they differ it puts the mode from
// before back and counts a divergence for that kind, keeping the first one's fields and operation label. The caller's
// mode is never judged here (crd/core/fp_environment.hpp's declared mode is the jobs boundary's business): the witness
// only guarantees a driver cannot change it behind the engine's back, and reports when one tried. Counts are
// process-wide relaxed atomics; reading the mode costs two register reads.

#include <crd/core/fp_environment.hpp>
#include <crd/core/types.hpp>

#include <type_traits>
#include <utility>

namespace crd::gpu
{

// NOLINTNEXTLINE(performance-enum-size)
enum class DriverCall : crd::u8
{
    CreateInstance,
    CreateDevice,
    CompileShader,
    Submit,
    Present,
};
inline constexpr crd::usize kDriverCallCount = 5U;
[[nodiscard]] const char* driver_call_name(DriverCall c) noexcept;

struct DriverWitnessReport
{
    crd::u64    calls[kDriverCallCount]       = {}; // witnessed calls per kind
    crd::u64    divergences[kDriverCallCount] = {}; // of those, calls that left the mode changed
    crd::u32    first_fields    = 0U;      // FpDifference bits of the process's first divergence
    const char* first_operation = nullptr; // its operation label (static storage); nullptr before any
    DriverCall  first_call      = DriverCall::CreateInstance; // its kind, meaningful when first_operation is set

    [[nodiscard]] crd::u64 total_divergences() const noexcept
    {
        crd::u64 n = 0U;
        for (const crd::u64 d : divergences)
        {
            n += d;
        }
        return n;
    }
};

// The witness's counts now.
[[nodiscard]] DriverWitnessReport driver_witness() noexcept;

// Record one witnessed call of `kind`; `changed` is the FpDifference bits it left (0 when it left the mode alone).
// `operation` must have static storage. Called by witness_driver_call; public so a backend with a call shape the
// template does not fit can record its own.
void note_driver_call(DriverCall kind, const char* operation, crd::u32 changed) noexcept;

namespace detail
{
inline void settle_driver_call(DriverCall kind, const char* operation, const FpEnvironment& before) noexcept
{
    const crd::u32 changed = fp_differences(read_fp_environment(), before);
    if (changed != 0U)
    {
        write_fp_environment(before);
    }
    note_driver_call(kind, operation, changed);
}
} // namespace detail

// Run `call` (a driver or compiler entry) and keep a mode it sets from reaching the caller. Returns what `call`
// returns.
template <typename Call> decltype(auto) witness_driver_call(DriverCall kind, const char* operation, Call&& call)
{
    const FpEnvironment before = read_fp_environment();
    if constexpr (std::is_void_v<std::invoke_result_t<Call>>)
    {
        std::forward<Call>(call)();
        detail::settle_driver_call(kind, operation, before);
    }
    else
    {
        auto result = std::forward<Call>(call)();
        detail::settle_driver_call(kind, operation, before);
        return result;
    }
}

} // namespace crd::gpu
