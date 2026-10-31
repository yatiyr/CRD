// The four-step and hierarchical block width, shared by the plan (general code) and the kernels (CRD_MATH_LEVEL_NS,
// so each unit keeps its own copy).

#pragma once

#include <crd/core/kernel_level.hpp>
#include <crd/core/types.hpp>

namespace crd::hesap::fft::detail::inline CRD_MATH_LEVEL_NS
{

// Block width so the batched sub-FFT working set (size·B complex) stays cache-resident. 1 MB block — the
// full-FFT-swept optimum 2026-06-15 (gather wants larger contiguous runs; the sub-FFT wants L2-resident scratch;
// 512KB/2MB/4MB all measured worse). The isolated gather is faster at larger blocks but the full FFT regresses once
// the sub-FFT scratch exceeds L2.
template <typename T> [[nodiscard]] constexpr crd::usize block_width(crd::usize size) noexcept
{
    constexpr crd::usize scratch_budget = crd::usize{1} << 20;
    const crd::usize per = size * 2U * sizeof(T); // one Complex<T>
    const crd::usize b = (per >= scratch_budget) ? crd::usize{1} : (scratch_budget / per);
    return b == 0 ? crd::usize{1} : b;
}

} // namespace crd::hesap::fft::detail::inline CRD_MATH_LEVEL_NS
