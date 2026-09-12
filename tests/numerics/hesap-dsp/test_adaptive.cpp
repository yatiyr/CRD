// crd-hesap-dsp v11-t — adaptive filters. Gated on KNOWN-PLANT RECOVERY (each adaptive filter converges to a
// planted FIR from a deterministic input) + Wiener-Hopf = the exact normal-equation solution + the run-twice
// determinism moat. Self-contained (no external reference needed for correctness — convergence IS the spec).

#include <crd/hesap/dsp/adaptive.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cstring>

namespace dsp = crd::hesap::dsp;
namespace cont = crd::containers;
using crd::f64;
using crd::u64;
using crd::usize;
using Catch::Matchers::WithinAbs;

namespace
{
constexpr usize kM = 4;
const f64 kPlant[kM] = {0.5, -0.3, 0.2, 0.1};

struct Lcg
{
    u64 s;
    f64 next()
    {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        return (static_cast<f64>(s >> 11) * (1.0 / 9007199254740992.0)) * 2.0 - 1.0; // [-1,1)
    }
};

// deterministic input x + plant output d = plant * x (causal FIR).
void make_signals(cont::Array<f64>& x, cont::Array<f64>& d, usize n)
{
    x.resize(n);
    d.resize(n);
    Lcg lcg{12345ULL};
    for (usize i = 0; i < n; ++i)
    {
        x[i] = lcg.next();
    }
    for (usize i = 0; i < n; ++i)
    {
        f64 s = 0.0;
        for (usize j = 0; j < kM; ++j)
        {
            if (i >= j)
            {
                s += kPlant[j] * x[i - j];
            }
        }
        d[i] = s;
    }
}

void check_plant(cont::ConstSpan<f64> w, f64 tol)
{
    REQUIRE(w.size() == kM);
    for (usize j = 0; j < kM; ++j)
    {
        INFO("tap " << j);
        CHECK_THAT(w[j], WithinAbs(kPlant[j], tol));
    }
}
} // namespace

TEST_CASE("dsp adaptive: LMS / NLMS / sign-LMS recover a known plant", "[v11-t][dsp][adaptive]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    cont::Array<f64> x(&alloc);
    cont::Array<f64> d(&alloc);
    make_signals(x, d, 30000);
    {
        dsp::NlmsFilter<f64> f(&alloc, kM, 0.5);
        for (usize n = 0; n < x.size(); ++n)
        {
            f.step(x[n], d[n]);
        }
        check_plant(f.weights(), 1e-3);
    }
    {
        dsp::LmsFilter<f64> f(&alloc, kM, 0.05);
        for (usize n = 0; n < x.size(); ++n)
        {
            f.step(x[n], d[n]);
        }
        check_plant(f.weights(), 1e-2);
    }
    {
        dsp::SignLmsFilter<f64> f(&alloc, kM, 0.002);
        for (usize n = 0; n < x.size(); ++n)
        {
            f.step(x[n], d[n]);
        }
        check_plant(f.weights(), 3e-2);
    }
}

TEST_CASE("dsp adaptive: affine projection recovers a known plant", "[v11-t][dsp][adaptive]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    cont::Array<f64> x(&alloc);
    cont::Array<f64> d(&alloc);
    make_signals(x, d, 20000);
    dsp::ApFilter<f64> f(&alloc, kM, 2, 0.5); // projection order 2
    for (usize n = 0; n < x.size(); ++n)
    {
        f.step(x[n], d[n]);
    }
    check_plant(f.weights(), 1e-2);
}

TEST_CASE("dsp adaptive: RLS recovers a known plant fast + exactly", "[v11-t][dsp][adaptive]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    cont::Array<f64> x(&alloc);
    cont::Array<f64> d(&alloc);
    make_signals(x, d, 4000);
    dsp::RlsFilter<f64> f(&alloc, kM, 1.0, 1e4);
    for (usize n = 0; n < x.size(); ++n)
    {
        f.step(x[n], d[n]);
    }
    check_plant(f.weights(), 1e-6);
}

TEST_CASE("dsp adaptive: Wiener-Hopf recovers the plant (autocorrelation method)", "[v11-t][dsp][adaptive]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    cont::Array<f64> x(&alloc);
    cont::Array<f64> d(&alloc);
    make_signals(x, d, 4000);
    const auto w = dsp::wiener_hopf<f64>(&alloc, cont::ConstSpan<f64>(x.data(), x.size()),
                                         cont::ConstSpan<f64>(d.data(), d.size()), kM);
    // the Toeplitz (autocorrelation-method) Wiener filter is a finite-sample estimate (window bias ~1/N),
    // asymptotically exact for a noiseless plant — not bit-exact like the covariance-LS RLS path above.
    check_plant(cont::ConstSpan<f64>(w.data(), w.size()), 1e-3);
}

TEST_CASE("dsp adaptive: NLMS is deterministic (run-twice bit-identical)", "[v11-t][dsp][adaptive]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    cont::Array<f64> x(&alloc);
    cont::Array<f64> d(&alloc);
    make_signals(x, d, 5000);
    auto run = [&](cont::Array<f64>& out)
    {
        dsp::NlmsFilter<f64> f(&alloc, kM, 0.5);
        for (usize n = 0; n < x.size(); ++n)
        {
            f.step(x[n], d[n]);
        }
        out.resize(kM);
        for (usize j = 0; j < kM; ++j)
        {
            out[j] = f.weights()[j];
        }
    };
    cont::Array<f64> a(&alloc);
    cont::Array<f64> b(&alloc);
    run(a);
    run(b);
    CHECK(std::memcmp(a.data(), b.data(), kM * sizeof(f64)) == 0); // the determinism moat
}
