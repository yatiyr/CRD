#pragma once

// The Boost.Math peer of the special-function boards (ADR-0134 §8). The pinned standalone Boost.Math (cmake/pins.json
// boost_math) is compiled in a shared library of its own at the measuring host's best ISA (crd_add_bench_peer), apart
// from the bench units, which build at the baseline like all Cerid code. Boost runs under its default policy, as its
// own build would: a double is promoted to long double, which is x87's 64-bit significand with GCC and Clang on x64,
// software binary128 on AArch64 Linux and double itself on MSVC, so the toolchains' numbers differ by design (isa()
// names the significand). One exported call evaluates a whole array; no call per element is timed.

#include "peer_api.hpp"

#include <cstdint>

namespace crd_peer::boost_math
{
using Index = std::int64_t;

// The timed functions under their Boost names, each with its arguments in Boost's order.
enum class Special : std::uint8_t
{
    Erf,        // erf(x)
    Erfc,       // erfc(x)
    ErfInv,     // erf_inv(x)
    Lgamma,     // lgamma(x)
    Tgamma,     // tgamma(x)
    Digamma,    // digamma(x)
    GammaP,     // gamma_p(a, x)
    CylBesselJ, // cyl_bessel_j(nu, x)
    CylNeumann, // cyl_neumann(nu, x)
    CylBesselI, // cyl_bessel_i(nu, x)
    CylBesselK, // cyl_bessel_k(nu, x)
    AiryAi,     // airy_ai(x)
    AiryBi,     // airy_bi(x)
    Ei,         // expint(x)
    E1,         // expint(1, x)
    Zeta,       // zeta(s)
    LambertW0,  // lambert_w0(x)
    Ellint1,    // ellint_1(k): the complete integral of the first kind of modulus k
    Ellint2,    // ellint_2(k): the complete integral of the second kind of modulus k
    EllintRf,   // ellint_rf(x, y, z)
};

// The arguments of n evaluations: the i-th call takes first[i], then second[i] and third[i] when the function has
// them (unused ones may be null).
struct Arguments
{
    const double* first;
    const double* second;
    const double* third;
};

// The build: the pinned Boost.Math version, the evaluation type of its double policy and the library's ISA flags.
CRD_BENCH_PEER_API const char* isa() noexcept;

// out[i] = f(arguments at i) for i < n; out does not overlap the arguments.
CRD_BENCH_PEER_API void eval(Special f, const Arguments& arguments, double* out, Index n);
} // namespace crd_peer::boost_math
