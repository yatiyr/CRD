// fft.cpp — Phase 3.1.6 v10-a: translation unit for crd-hesap-fft. The plan is a header template (fft.hpp); this TU
// instantiates it, builds the small-N codelet twiddles and, under CRD_FFT_PROFILE, defines and prints the kernels'
// counters. Including the header here also compile-checks it in isolation.

#include <crd/hesap/fft/fft.hpp>
#include <crd/math/cmath.hpp>

#ifdef CRD_FFT_PROFILE
#include <cstdio>
#endif

namespace crd::hesap::fft
{
// Force-instantiate the f32/f64 plans so the library carries their code (and a fresh TU catches header
// regressions even when no consumer instantiates them this build).
template class FftPlan<crd::f32>;
template class FftPlan<crd::f64>;

namespace
{

void fill_lanes(crd::f64 (&lanes)[4], crd::f64 v) noexcept
{
    for (crd::f64& lane : lanes)
    {
        lane = v;
    }
}

// Combine pair j packs [W_n^{2j} | W_n^{2j+1}] across the two 128-bit halves.
void fill_pairs(int n, int count, crd::f64 (*wr)[4], crd::f64 (*wi)[4]) noexcept
{
    constexpr double pi = 3.141592653589793238;
    for (int j = 0; j < count; ++j)
    {
        const double c0 = crd::math::cos(-2.0 * pi * (2 * j) / n);
        const double s0 = crd::math::sin(-2.0 * pi * (2 * j) / n);
        const double c1 = crd::math::cos(-2.0 * pi * (2 * j + 1) / n);
        const double s1 = crd::math::sin(-2.0 * pi * (2 * j + 1) / n);
        wr[j][0] = c0;
        wr[j][1] = c0;
        wr[j][2] = c1;
        wr[j][3] = c1;
        wi[j][0] = s0;
        wi[j][1] = s0;
        wi[j][2] = s1;
        wi[j][3] = s1;
    }
}

SmallNTwiddles make_small_n_twiddles() noexcept
{
    constexpr double pi = 3.141592653589793238;
    SmallNTwiddles t{};
    fill_lanes(t.w81r, crd::math::cos(-2.0 * pi / 8.0));
    fill_lanes(t.w81i, crd::math::sin(-2.0 * pi / 8.0));
    fill_lanes(t.w83r, crd::math::cos(-2.0 * pi * 3.0 / 8.0));
    fill_lanes(t.w83i, crd::math::sin(-2.0 * pi * 3.0 / 8.0));
    for (int k = 0; k < 4; ++k)
    {
        fill_lanes(t.w16r[k], crd::math::cos(-2.0 * pi * k / 16.0));
        fill_lanes(t.w16i[k], crd::math::sin(-2.0 * pi * k / 16.0));
        fill_lanes(t.w163r[k], crd::math::cos(-2.0 * pi * 3.0 * k / 16.0));
        fill_lanes(t.w163i[k], crd::math::sin(-2.0 * pi * 3.0 * k / 16.0));
    }
    fill_pairs(8, 2, t.wp8r, t.wp8i);
    fill_pairs(16, 4, t.wp16r, t.wp16i);
    fill_pairs(32, 8, t.wp32r, t.wp32i);
    return t;
}

} // namespace

const SmallNTwiddles& small_n_twiddles() noexcept
{
    static const SmallNTwiddles kTw = make_small_n_twiddles();
    return kTw;
}

#ifdef CRD_FFT_PROFILE
namespace prof
{
unsigned long long g_first = 0, g_combine = 0, g_last = 0;
long g_calls = 0;
// four-step phase counters (large-N path): P1 = column FFTs, P2 = row FFTs
unsigned long long g_p1_gather = 0, g_p1_sub = 0, g_p1_tw = 0;
unsigned long long g_p2_gather = 0, g_p2_sub = 0, g_p2_scatter = 0;
long g_fs_calls = 0;
// deep-split pass counters (S1 fused leaf / S2 notr leaf / S3 strided leaf)
unsigned long long g_ds1 = 0, g_ds2 = 0, g_ds3 = 0;
long g_ds_calls = 0;
// ip4-AoS phase counters (gather+len4 / the combine passes)
unsigned long long g_ip_gather = 0, g_ip_pass = 0;
long g_ip_calls = 0;

namespace
{
// A cycle count as a double (the conversion the profile tree's -Wconversion build asks to be spelled).
[[nodiscard]] double cycles(unsigned long long count) noexcept
{
    return static_cast<double>(count);
}

// One four-step phase: Mcyc a call and its share of the transform.
void print_phase(const char* name, unsigned long long count, double calls, double pct) noexcept
{
    std::fprintf(stderr, "  %-11s %8.1f  (%4.1f%%)\n", name, cycles(count) / calls / 1e6, cycles(count) * pct);
}
} // namespace

void dump_ip() noexcept
{
    const double c = static_cast<double>(g_ip_calls > 0 ? g_ip_calls : 1);
    std::fprintf(stderr, "[fft-prof ip4aos, %ld calls] gather %.1f Kcyc  passes %.1f Kcyc\n", g_ip_calls,
                 cycles(g_ip_gather) / c / 1e3, cycles(g_ip_pass) / c / 1e3);
    g_ip_gather = g_ip_pass = 0;
    g_ip_calls = 0;
}

void dump() noexcept
{
    const double c = static_cast<double>(g_calls > 0 ? g_calls : 1);
    std::fprintf(stderr, "[fft-prof] first=%.0f combine=%.0f last=%.0f cyc/call (%ld calls)\n",
                 static_cast<double>(g_first) / c, static_cast<double>(g_combine) / c, static_cast<double>(g_last) / c,
                 g_calls);
    g_first = g_combine = g_last = 0;
    g_calls = 0;
}

void dump_four_step() noexcept
{
    const double c = static_cast<double>(g_fs_calls > 0 ? g_fs_calls : 1);
    const double tot = cycles(g_p1_gather + g_p1_sub + g_p1_tw + g_p2_gather + g_p2_sub + g_p2_scatter);
    const double pct = tot > 0 ? 100.0 / tot : 0.0;
    std::fprintf(stderr, "[fft-prof four-step, %ld calls, Mcyc/call]\n", g_fs_calls);
    print_phase("P1 gather", g_p1_gather, c, pct);
    print_phase("P1 sub-FFT", g_p1_sub, c, pct);
    print_phase("P1 twid+NT", g_p1_tw, c, pct);
    print_phase("P2 gather", g_p2_gather, c, pct);
    print_phase("P2 sub-FFT", g_p2_sub, c, pct);
    print_phase("P2 scatter", g_p2_scatter, c, pct);
    g_p1_gather = g_p1_sub = g_p1_tw = g_p2_gather = g_p2_sub = g_p2_scatter = 0;
    g_fs_calls = 0;
    if (g_ds_calls > 0)
    {
        const double dc = static_cast<double>(g_ds_calls);
        std::fprintf(stderr, "[fft-prof deep-split, %ld calls, Mcyc/call]\n", g_ds_calls);
        std::fprintf(stderr, "  S1 fused    %8.2f\n", cycles(g_ds1) / dc / 1e6);
        std::fprintf(stderr, "  S2 notr     %8.2f\n", cycles(g_ds2) / dc / 1e6);
        std::fprintf(stderr, "  S3 strided  %8.2f\n", cycles(g_ds3) / dc / 1e6);
        g_ds1 = g_ds2 = g_ds3 = 0;
        g_ds_calls = 0;
    }
}
} // namespace prof
#endif

} // namespace crd::hesap::fft
