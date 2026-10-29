#pragma once

// The pocketfft peer of the FFT and DCT boards (ADR-0134 §8). pocketfft's C++ header is compiled in a shared library
// of its own at the measuring host's best ISA (crd_add_bench_peer), apart from the bench units, which build at the
// baseline like all Cerid code. A plan is a handle the library owns, built outside the timed region as every other
// side's plan is. A transform is one exported call doing what pocketfft's c2c and dct do for one contiguous array (the
// input copied to the output, the plan executed there in place), without the plan and buffer those build on every
// call at the header's default POCKETFFT_CACHE_SIZE of 0. Complex values are re/im pairs of doubles.

#include "peer_api.hpp"

#include <cstdint>

namespace crd_peer::pocketfft
{
using crd_peer::Owned;
using Index = std::int64_t;

struct C2c;  // pocketfft_c<double>: the complex transform of one length
struct Dct2; // T_dcst23<double>: the DCT-II of one length

// The build: the header's SHA-256 prefix, the f64 vector length it compiled and the library's ISA flags.
CRD_BENCH_PEER_API const char* isa() noexcept;
CRD_BENCH_PEER_API C2c* c2c_plan(Index n);
CRD_BENCH_PEER_API Dct2* dct2_plan(Index n);
CRD_BENCH_PEER_API void destroy(C2c* plan) noexcept;
CRD_BENCH_PEER_API void destroy(Dct2* plan) noexcept;

// out = the unscaled forward DFT of in, n complex values each; in and out do not overlap.
CRD_BENCH_PEER_API void forward(const C2c* plan, const double* in, double* out);
// out[k] = 2 sum_j in[j] cos(pi k (2j + 1) / 2n), FFTW's REDFT10 (pocketfft's type 2, unnormalized).
CRD_BENCH_PEER_API void dct2(const Dct2* plan, const double* in, double* out);
} // namespace crd_peer::pocketfft
