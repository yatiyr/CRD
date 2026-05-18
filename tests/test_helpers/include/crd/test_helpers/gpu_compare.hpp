#pragma once

// ---------------------------------------------------------------------------
// crd::test::ulp_compare / bit_compare — buffer-vs-reference compare
// helpers for v9 GPU slices. Phase 3.1.7 v9-prereq-test-harness
// (2026-05-18).
//
// USE CASE: v9a GPU LBVH builder needs ULP-conformance against CPU
// `bvh_build` reference. v9e shader-helpers need per-pixel ULP-compare
// across a 64³ sample grid. Manual loops at every call site are noisy;
// these helpers package "given a CPU reference span + a mapped GPU
// output ptr, find the first mismatch + report ULP delta".
//
// Pattern (caller stages the GPU buffer into a host-visible read-back
// span first — staging is consumer's responsibility, since the right
// staging strategy depends on whether the GPU buffer is GpuToCpu
// already or needs a copy):
//
//   auto* gpu_ptr = static_cast<f32*>(host_visible_buf->map());
//   auto result = crd::test::ulp_compare<f32>(
//       cpu_ref, ConstSpan<f32>{gpu_ptr, N}, /*max_ulp*/ 1);
//   host_visible_buf->unmap();
//   if (!result.ok)
//   {
//       INFO("first mismatch at " << result.first_mismatch_index
//            << ": cpu=" << result.cpu_value
//            << " gpu=" << result.gpu_value
//            << " ulp_diff=" << result.ulp_diff);
//   }
//   REQUIRE(result.ok);
// ---------------------------------------------------------------------------

#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>

#include <bit>
#include <cmath>
#include <cstring>
#include <type_traits>

namespace crd::test
{

template <typename T>
struct CompareResult
{
    bool       ok                   = false;
    crd::usize first_mismatch_index = 0;
    T          cpu_value{};
    T          gpu_value{};
    crd::u32   ulp_diff             = 0; // 0 for int types (bit-exact compare)
    crd::usize compared_count       = 0;
};

namespace detail
{

// f32 ULP distance via biased-bit-representation trick (Bruce Dawson).
// Handles +0/-0 correctly. NaN compares always non-equal.
[[nodiscard]] inline crd::u32 ulp_distance_f32(float a, float b) noexcept
{
    if (std::isnan(a) || std::isnan(b))
    {
        return UINT32_MAX;
    }
    crd::u32 ai = 0;
    crd::u32 bi = 0;
    std::memcpy(&ai, &a, sizeof(ai));
    std::memcpy(&bi, &b, sizeof(bi));
    // Map signed-magnitude → two's-complement ordering so subtraction
    // gives a valid lane-step distance across zero.
    constexpr crd::u32 kSignBit = 0x80000000U;
    if ((ai & kSignBit) != 0U) { ai = kSignBit - ai; }
    if ((bi & kSignBit) != 0U) { bi = kSignBit - bi; }
    return ai > bi ? (ai - bi) : (bi - ai);
}

} // namespace detail

// f32 ULP-compare: `max_ulp` lane-steps tolerance per element.
[[nodiscard]] inline CompareResult<float>
ulp_compare(crd::containers::ConstSpan<float> cpu_reference,
            crd::containers::ConstSpan<float> gpu_output,
            crd::u32                          max_ulp = 1) noexcept
{
    CompareResult<float> r{};
    r.compared_count = (cpu_reference.size() < gpu_output.size())
                           ? cpu_reference.size() : gpu_output.size();
    for (crd::usize i = 0; i < r.compared_count; ++i)
    {
        const float cpu_v = cpu_reference[i];
        const float gpu_v = gpu_output[i];
        const crd::u32 d  = detail::ulp_distance_f32(cpu_v, gpu_v);
        if (d > max_ulp)
        {
            r.ok                   = false;
            r.first_mismatch_index = i;
            r.cpu_value            = cpu_v;
            r.gpu_value            = gpu_v;
            r.ulp_diff             = d;
            return r;
        }
    }
    r.ok = (cpu_reference.size() == gpu_output.size());
    return r;
}

// Integer / byte bit-exact compare. `max_ulp` ignored (always 0).
template <typename T>
    requires std::is_integral_v<T>
[[nodiscard]] inline CompareResult<T>
bit_compare(crd::containers::ConstSpan<T> cpu_reference,
            crd::containers::ConstSpan<T> gpu_output) noexcept
{
    CompareResult<T> r{};
    r.compared_count = (cpu_reference.size() < gpu_output.size())
                           ? cpu_reference.size() : gpu_output.size();
    for (crd::usize i = 0; i < r.compared_count; ++i)
    {
        if (cpu_reference[i] != gpu_output[i])
        {
            r.ok                   = false;
            r.first_mismatch_index = i;
            r.cpu_value            = cpu_reference[i];
            r.gpu_value            = gpu_output[i];
            r.ulp_diff             = 0;
            return r;
        }
    }
    r.ok = (cpu_reference.size() == gpu_output.size());
    return r;
}

} // namespace crd::test
