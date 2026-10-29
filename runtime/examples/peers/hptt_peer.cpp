// The HPTT peer's plans (hptt_peer.hpp).

#include "hptt_peer.hpp"

#include <array>
#include <cstdio>
#include <hptt.h>

namespace crd_peer::hptt
{
struct Plan
{
    Plan() = default;
    Plan(const Plan&) = delete;
    Plan& operator=(const Plan&) = delete;
    virtual ~Plan() = default;
    virtual void execute() noexcept = 0;
};

namespace
{
template <typename T> struct TypedPlan final : Plan
{
    TypedPlan(int rank, const int* size, const int* perm, const void* a, void* b, int threads)
        : transpose(size, perm, nullptr, nullptr, rank, static_cast<const T*>(a), T(1), static_cast<T*>(b), T(0),
                    ::hptt::ESTIMATE, threads, nullptr, true)
    {
    }
    void execute() noexcept override { transpose.execute(); }

    ::hptt::Transpose<T> transpose;
};

#if defined(HPTT_ARCH_AVX)
constexpr const char* kKernel = "AVX";
#else
constexpr const char* kKernel = "portable";
#endif
} // namespace

const char* isa() noexcept
{
    static const std::array<char, 160> kText = []()
    {
        std::array<char, 160> text{};
        (void)std::snprintf(text.data(), text.size(), "HPTT %s, %s kernels, OpenMP (%s)", CRD_HPTT_COMMIT, kKernel,
                            CRD_BENCH_PEER_ISA);
        return text;
    }();
    return kText.data();
}

Plan* plan(Element element, int rank, const int* size, const int* perm, const void* a, void* b, int threads)
{
    switch (element)
    {
        case Element::F32:
            return new TypedPlan<float>(rank, size, perm, a, b, threads);
        case Element::F64:
            return new TypedPlan<double>(rank, size, perm, a, b, threads);
        case Element::C32:
            return new TypedPlan<::hptt::FloatComplex>(rank, size, perm, a, b, threads);
        case Element::C64:
            return new TypedPlan<::hptt::DoubleComplex>(rank, size, perm, a, b, threads);
    }
    return nullptr;
}

void destroy(Plan* plan) noexcept
{
    delete plan;
}

void execute(Plan* plan) noexcept
{
    plan->execute();
}
} // namespace crd_peer::hptt
