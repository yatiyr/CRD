#pragma once

// The Highway peer of the dispatch-cost board (ADR-0134 §8; cmake/pins.json highway). Highway compiles its f16 to f32
// conversion once per target from its baseline up and selects one at run time (HWY_DYNAMIC_DISPATCH), the dispatch the
// board sets against a crd kernel table's call. The library builds at the baseline, as Highway's own build does
// (crd_add_bench_peer DISPATCHES), and the calls loop inside it, so the board's per-call cost holds no call into the
// shared library beyond one per timed sample.

#include "peer_api.hpp"

#include <cstddef>
#include <cstdint>

namespace crd_peer::highway
{
// Highway's version, the commit, the target its dispatch chose on this host and every target the library compiled.
CRD_BENCH_PEER_API const char* isa() noexcept;
// One conversion through HWY_DYNAMIC_DISPATCH: dst[i] = the binary16 value src[i] as a binary32 (exact).
CRD_BENCH_PEER_API void f16_to_f32(const std::uint16_t* src, float* dst, std::size_t n) noexcept;
// `calls` conversions of the same arrays, each through HWY_DYNAMIC_DISPATCH (the dispatch every call pays).
CRD_BENCH_PEER_API void f16_to_f32_dispatched(const std::uint16_t* src, float* dst, std::size_t n,
                                              std::size_t calls) noexcept;
// `calls` conversions through the chosen target's pointer taken once (HWY_DYNAMIC_POINTER after the first dispatch):
// an indirect call to the same body, the control that leaves the dispatch out.
CRD_BENCH_PEER_API void f16_to_f32_pointer(const std::uint16_t* src, float* dst, std::size_t n,
                                           std::size_t calls) noexcept;
} // namespace crd_peer::highway
