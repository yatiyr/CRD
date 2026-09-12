#pragma once

// crd-hesap-stats v12-f — the Ziggurat method (Marsaglia & Tsang 2000) for standard normal + exponential: the
// fastest known samplers (a single table lookup + comparison on the common path, ~97% of draws). Generic over any
// BitGenerator. Tables are built once at static init from the published layer constants (deterministic per build;
// the cross-THREAD moat holds since the table is shared read-only). Gated statistically (KS + moments vs erf/exp).

#include <crd/hesap/stats/bitgen.hpp>
#include <crd/core/types.hpp>

#include <crd/math/cmath.hpp>

namespace crd::hesap::stats
{
namespace detail
{
// 128-layer normal ziggurat tables (Marsaglia-Tsang constants r = 3.442619855899, area v = 9.91256303526217e-3).
struct ZigNormal
{
    crd::u32 kn[128];
    double wn[128];
    double fn[128];

    ZigNormal() noexcept
    {
        constexpr double m1 = 2147483648.0; // 2^31 (signed draw)
        double dn = 3.442619855899;
        double tn = dn;
        constexpr double vn = 9.91256303526217e-3;
        const double q = vn / crd::math::exp(-0.5 * dn * dn);
        kn[0] = static_cast<crd::u32>((dn / q) * m1);
        kn[1] = 0;
        wn[0] = q / m1;
        wn[127] = dn / m1;
        fn[0] = 1.0;
        fn[127] = crd::math::exp(-0.5 * dn * dn);
        for (int i = 126; i >= 1; --i)
        {
            dn = crd::math::sqrt(-2.0 * crd::math::log(vn / dn + crd::math::exp(-0.5 * dn * dn)));
            kn[i + 1] = static_cast<crd::u32>((dn / tn) * m1);
            tn = dn;
            fn[i] = crd::math::exp(-0.5 * dn * dn);
            wn[i] = dn / m1;
        }
    }
};

// 256-layer exponential ziggurat (r = 7.697117470131487, area v = 3.949659822581572e-3).
struct ZigExp
{
    crd::u32 ke[256];
    double we[256];
    double fe[256];

    ZigExp() noexcept
    {
        constexpr double m2 = 4294967296.0; // 2^32 (unsigned draw)
        double de = 7.697117470131487;
        double te = de;
        constexpr double ve = 3.949659822581572e-3;
        const double q = ve / crd::math::exp(-de);
        ke[0] = static_cast<crd::u32>((de / q) * m2);
        ke[1] = 0;
        we[0] = q / m2;
        we[255] = de / m2;
        fe[0] = 1.0;
        fe[255] = crd::math::exp(-de);
        for (int i = 254; i >= 1; --i)
        {
            de = -crd::math::log(ve / de + crd::math::exp(-de));
            ke[i + 1] = static_cast<crd::u32>((de / te) * m2);
            te = de;
            fe[i] = crd::math::exp(-de);
            we[i] = de / m2;
        }
    }
};

[[nodiscard]] inline const ZigNormal& zig_normal() noexcept
{
    static const ZigNormal t;
    return t;
}
[[nodiscard]] inline const ZigExp& zig_exp() noexcept
{
    static const ZigExp t;
    return t;
}
} // namespace detail

// Standard normal N(0,1) via the ziggurat. ~2× faster than Box-Muller; identical distribution.
template <BitGenerator G>
[[nodiscard]] double standard_normal(G& g) noexcept
{
    const detail::ZigNormal& z = detail::zig_normal();
    constexpr double r = 3.442619855899;
    for (;;)
    {
        const auto u = static_cast<crd::u32>(g.next_u64() >> 32);
        const auto hz = static_cast<crd::i32>(u);
        const crd::u32 iz = u & 127U;
        const crd::u32 az = hz < 0 ? (0U - static_cast<crd::u32>(hz)) : static_cast<crd::u32>(hz);
        if (az < z.kn[iz])
        {
            return static_cast<double>(hz) * z.wn[iz]; // ~97% fast path
        }
        if (iz == 0U) // the tail (exponential rejection)
        {
            double x;
            double y;
            do
            {
                x = -crd::math::log(1.0 - next_double(g)) / r;
                y = -crd::math::log(1.0 - next_double(g));
            } while (y + y < x * x);
            return hz > 0 ? r + x : -(r + x);
        }
        const double x = static_cast<double>(hz) * z.wn[iz]; // the wedge
        if (z.fn[iz] + next_double(g) * (z.fn[iz - 1] - z.fn[iz]) < crd::math::exp(-0.5 * x * x))
        {
            return x;
        }
    }
}

// Standard exponential Exp(1) via the ziggurat.
template <BitGenerator G>
[[nodiscard]] double standard_exponential(G& g) noexcept
{
    const detail::ZigExp& z = detail::zig_exp();
    constexpr double r = 7.697117470131487;
    for (;;)
    {
        const auto jz = static_cast<crd::u32>(g.next_u64() >> 32);
        const crd::u32 iz = jz & 255U;
        if (jz < z.ke[iz])
        {
            return static_cast<double>(jz) * z.we[iz]; // fast path
        }
        if (iz == 0U)
        {
            return r - crd::math::log(1.0 - next_double(g)); // tail
        }
        const double x = static_cast<double>(jz) * z.we[iz];
        if (z.fe[iz] + next_double(g) * (z.fe[iz - 1] - z.fe[iz]) < crd::math::exp(-x))
        {
            return x;
        }
    }
}

// Affine helpers.
template <BitGenerator G>
[[nodiscard]] double normal(G& g, double mean, double stddev) noexcept
{
    return mean + stddev * standard_normal(g);
}
template <BitGenerator G>
[[nodiscard]] double exponential(G& g, double scale) noexcept
{
    return scale * standard_exponential(g);
}

} // namespace crd::hesap::stats
