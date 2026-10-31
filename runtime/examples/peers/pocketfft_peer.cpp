// The pocketfft peer's plans and transforms (pocketfft_peer.hpp).

#include "pocketfft_peer.hpp"

#include <array>
#include <cstddef>
#include <cstdio>
#include <cstring>

// pocketfft's passes index scratch arrays its arr<> leaves null at length 0, a length its plans never give; GCC 13
// reports that path (-Wnull-dereference) inside the system header's instantiations all the same. The pragma covers the
// header's text alone, so the peer's own code keeps the tree's warning.
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wnull-dereference"
#endif
#include <pocketfft_hdronly.h>
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

namespace pf = ::pocketfft::detail;

namespace crd_peer::pocketfft
{
struct C2c
{
    pf::pocketfft_c<double> plan;
};

struct Dct2
{
    pf::T_dcst23<double> plan;
};

const char* isa() noexcept
{
    static const std::array<char, 160> kText = []()
    {
        std::array<char, 160> text{};
        (void)std::snprintf(text.data(), text.size(), "pocketfft %s, f64 vector length %zu, one thread (%s)",
                            CRD_POCKETFFT_COMMIT, pf::VLEN<double>::val, CRD_BENCH_PEER_ISA);
        return text;
    }();
    return kText.data();
}

C2c* c2c_plan(Index n)
{
    return new C2c{pf::pocketfft_c<double>(static_cast<std::size_t>(n))};
}

Dct2* dct2_plan(Index n)
{
    return new Dct2{pf::T_dcst23<double>(static_cast<std::size_t>(n))};
}

void destroy(C2c* plan) noexcept
{
    delete plan;
}

void destroy(Dct2* plan) noexcept
{
    delete plan;
}

void forward(const C2c* plan, const double* in, double* out)
{
    std::memcpy(out, in, plan->plan.length() * 2U * sizeof(double));
    plan->plan.exec(reinterpret_cast<pf::cmplx<double>*>(out), 1.0, true);
}

void dct2(const Dct2* plan, const double* in, double* out)
{
    std::memcpy(out, in, plan->plan.length() * sizeof(double));
    plan->plan.exec(out, 1.0, false, 2, true);
}
} // namespace crd_peer::pocketfft
