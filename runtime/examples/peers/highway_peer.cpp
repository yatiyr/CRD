// Highway's f16 to f32 conversion, compiled once per target by foreach_target.h (which includes this unit again for
// each target: HWY_TARGET_INCLUDE, its name on the include path, is the target's compile definition) and dispatched at
// run time (highway_peer.hpp).

#include "highway_peer.hpp"

#include <array>
#include <cstdio>
#include <hwy/foreach_target.h>
#include <hwy/highway.h>

HWY_BEFORE_NAMESPACE();
namespace crd_peer::highway::HWY_NAMESPACE
{
namespace hn = hwy::HWY_NAMESPACE;

// Whole vectors, then the remainder through Highway's partial load and store.
void convert(const std::uint16_t* HWY_RESTRICT src, float* HWY_RESTRICT dst, std::size_t n)
{
    const hn::ScalableTag<float> df;
    const hn::Rebind<hwy::float16_t, decltype(df)> dh;
    const std::size_t lanes = hn::Lanes(df);
    const auto* const in = reinterpret_cast<const hwy::float16_t*>(src);
    std::size_t i = 0;
    for (; i + lanes <= n; i += lanes)
    {
        hn::StoreU(hn::PromoteTo(df, hn::LoadU(dh, in + i)), df, dst + i);
    }
    if (i < n)
    {
        hn::StoreN(hn::PromoteTo(df, hn::LoadN(dh, in + i, n - i)), df, dst + i, n - i);
    }
}

std::int64_t target()
{
    return HWY_TARGET;
}
} // namespace crd_peer::highway::HWY_NAMESPACE
HWY_AFTER_NAMESPACE();

#if HWY_ONCE
namespace crd_peer::highway
{
HWY_EXPORT(convert);
HWY_EXPORT(target);

const char* isa() noexcept
{
    static const std::array<char, 512> kText = []()
    {
        std::array<char, 512> text{};
        const auto room = [&](int used)
        {
            return used >= 0 && static_cast<std::size_t>(used) < text.size();
        };
        int used =
            std::snprintf(text.data(), text.size(), "Highway %d.%d.%d %s, dispatched %s, compiled", HWY_MAJOR,
                          HWY_MINOR, HWY_PATCH, CRD_HIGHWAY_COMMIT, hwy::TargetName(HWY_DYNAMIC_DISPATCH(target)()));
        for (std::int64_t targets = HWY_TARGETS; targets != 0 && room(used); targets &= targets - 1)
        {
            const auto at = static_cast<std::size_t>(used);
            used += std::snprintf(text.data() + at, text.size() - at, " %s", hwy::TargetName(targets & -targets));
        }
        if (room(used))
        {
            const auto at = static_cast<std::size_t>(used);
            (void)std::snprintf(text.data() + at, text.size() - at, " (%s)", CRD_BENCH_PEER_ISA);
        }
        return text;
    }();
    return kText.data();
}

void f16_to_f32(const std::uint16_t* src, float* dst, std::size_t n) noexcept
{
    HWY_DYNAMIC_DISPATCH(convert)(src, dst, n);
}

void f16_to_f32_dispatched(const std::uint16_t* src, float* dst, std::size_t n, std::size_t calls) noexcept
{
    for (std::size_t c = 0; c < calls; ++c)
    {
        HWY_DYNAMIC_DISPATCH(convert)(src, dst, n);
    }
}

void f16_to_f32_pointer(const std::uint16_t* src, float* dst, std::size_t n, std::size_t calls) noexcept
{
    // An empty conversion chooses the target; until then the table's first entry is the chooser.
    HWY_DYNAMIC_DISPATCH(convert)(src, dst, 0);
    const auto chosen = HWY_DYNAMIC_POINTER(convert);
    for (std::size_t c = 0; c < calls; ++c)
    {
        chosen(src, dst, n);
    }
}
} // namespace crd_peer::highway
#endif
