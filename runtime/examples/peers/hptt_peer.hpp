#pragma once

// The HPTT peer of the permute board (ADR-0134 §8; cmake/pins.json hptt). HPTT's sources build at the measuring
// host's best ISA with OpenMP (crd-hptt, runtime/CMakeLists.txt) inside a shared library of its own
// (crd_add_bench_peer), apart from the bench units, which build at the baseline like all Cerid code. A plan is a
// handle the library owns, built outside the timed region as every Cerid arm's tables are; it binds its two arrays.

#include "peer_api.hpp"

#include <cstdint>

namespace crd_peer::hptt
{
using crd_peer::Owned;

enum class Element : std::uint8_t
{
    F32,
    F64,
    C32, // complex values are (re, im) pairs of floats
    C64, // and of doubles
};

struct Plan; // hptt::Transpose of the element type

// The build: the sources' SHA-256 prefix, the kernel (AVX or portable) and the library's ISA flags.
CRD_BENCH_PEER_API const char* isa() noexcept;
// b = a permuted, row-major: b's dimension d is a's dimension perm[d] (a's sizes `size`), with alpha 1 and beta 0
// (b is never read), HPTT's ESTIMATE plan selection, on `threads` OpenMP threads.
CRD_BENCH_PEER_API Plan* plan(Element element, int rank, const int* size, const int* perm, const void* a, void* b,
                              int threads);
CRD_BENCH_PEER_API void destroy(Plan* plan) noexcept;
CRD_BENCH_PEER_API void execute(Plan* plan) noexcept;
} // namespace crd_peer::hptt
