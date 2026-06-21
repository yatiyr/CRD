// crd-hesap-dsp v11-r — waveform generators. Gated vs scipy.signal (chirp/sawtooth/square/gausspulse/sweep_poly/
// unit_impulse) to ~1e-10, on the SAME time vectors. Generators (one-time) ⇒ correctness only, no perf bench.

#include <crd/hesap/dsp/waveforms.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include "waveforms_refs.inc"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

namespace dsp = crd::hesap::dsp;
namespace cont = crd::containers;
using crd::f64;
using crd::usize;
using Catch::Matchers::WithinAbs;

namespace
{
template <usize N> void check(const double (&ref)[N], const cont::Array<f64>& got, double tol)
{
    REQUIRE(got.size() == N);
    for (usize i = 0; i < N; ++i)
    {
        INFO("i=" << i);
        CHECK_THAT(got[i], WithinAbs(ref[i], tol));
    }
}

cont::Array<f64> lin(crd::memory::IAllocator* a, usize n, f64 scale, f64 off = 0.0)
{
    cont::Array<f64> t(a);
    t.resize(n);
    for (usize i = 0; i < n; ++i)
    {
        t[i] = (static_cast<f64>(i) + off) * scale;
    }
    return t;
}
} // namespace

TEST_CASE("dsp waveforms: chirp (4 methods) matches scipy", "[v11-r][dsp][waveforms]")
{
    crd::memory::TlsfAllocator alloc(1U << 20);
    const auto tc = lin(&alloc, 256, 1.0 / 255.0);
    const cont::ConstSpan<f64> ts(tc.data(), 256);
    check(ref_chirp_linear, dsp::chirp<f64>(&alloc, ts, 6.0, 1.0, 60.0, dsp::ChirpMethod::Linear), 1e-10);
    check(ref_chirp_quadratic, dsp::chirp<f64>(&alloc, ts, 6.0, 1.0, 60.0, dsp::ChirpMethod::Quadratic), 1e-10);
    check(ref_chirp_logarithmic, dsp::chirp<f64>(&alloc, ts, 6.0, 1.0, 60.0, dsp::ChirpMethod::Logarithmic), 1e-10);
    check(ref_chirp_hyperbolic, dsp::chirp<f64>(&alloc, ts, 6.0, 1.0, 60.0, dsp::ChirpMethod::Hyperbolic), 1e-10);
}

TEST_CASE("dsp waveforms: sawtooth + square match scipy", "[v11-r][dsp][waveforms]")
{
    crd::memory::TlsfAllocator alloc(1U << 20);
    const auto t = lin(&alloc, 256, 0.1);
    const cont::ConstSpan<f64> ts(t.data(), 256);
    check(ref_sawtooth_1, dsp::sawtooth<f64>(&alloc, ts, 1.0), 1e-12);
    check(ref_sawtooth_05, dsp::sawtooth<f64>(&alloc, ts, 0.5), 1e-12);
    check(ref_square_05, dsp::square<f64>(&alloc, ts, 0.5), 1e-12);
    check(ref_square_03, dsp::square<f64>(&alloc, ts, 0.3), 1e-12);
}

TEST_CASE("dsp waveforms: gausspulse + sweep_poly + unit_impulse match scipy", "[v11-r][dsp][waveforms]")
{
    crd::memory::TlsfAllocator alloc(1U << 20);
    const auto tg = lin(&alloc, 256, 1.0 / 256.0, -128.0);
    check(ref_gausspulse, dsp::gausspulse<f64>(&alloc, cont::ConstSpan<f64>(tg.data(), 256), 5.0, 0.5), 1e-10);
    const auto tc = lin(&alloc, 256, 1.0 / 255.0);
    const f64 poly[] = {0.05, -0.75, 2.5, 8.0};
    check(ref_sweep_poly, dsp::sweep_poly<f64>(&alloc, cont::ConstSpan<f64>(tc.data(), 256), cont::ConstSpan<f64>(poly, 4)), 1e-10);
    check(ref_unit_impulse, dsp::unit_impulse<f64>(&alloc, 16, 5), 0.0);
}
