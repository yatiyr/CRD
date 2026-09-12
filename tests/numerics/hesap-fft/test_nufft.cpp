// v10-g gates: 1D NUFFT (type-1 + type-2). THE correctness gate is the DIRECT nonuniform DFT (exact, kernel-
// free), NOT type1∘type2 round-trip — which can cancel spreading errors the same way IFFT(FFT) hides a
// twiddle-sign bug (the odeint-d4 trap). We check the gridded transform against the direct sum at the
// tolerance the chosen kernel width promises, for both isign and both types, f32 + f64; plus run-twice
// determinism (the spreader is the moat-critical scatter) and the tol->accuracy monotonicity.

#include <crd/containers/array.hpp>
#include <crd/hesap/complex.hpp>
#include <crd/hesap/fft/nufft.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstring>

using crd::f32;
using crd::f64;
using crd::usize;
namespace fft = crd::hesap::fft;
namespace cont = crd::containers;
using crd::hesap::Complex;

namespace
{
constexpr double kTwoPi = 6.283185307179586476925286766559;

struct Lcg
{
    crd::u64 s;
    explicit Lcg(crd::u64 seed) : s(seed) {}
    double next()
    {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        return (static_cast<double>(s >> 11) / static_cast<double>(1ULL << 53)) * 2.0 - 1.0; // [-1,1)
    }
};

template <typename T> void fill_points(cont::Array<T>& x, usize m, crd::u64 seed)
{
    x.resize(m);
    Lcg g(seed);
    for (usize j = 0; j < m; ++j)
    {
        x[j] = static_cast<T>((g.next() * 0.5 + 0.5) * kTwoPi); // [0, 2*pi)
    }
}

template <typename T> void fill_strengths(cont::Array<Complex<T>>& c, usize m, crd::u64 seed)
{
    c.resize(m);
    Lcg g(seed);
    for (usize j = 0; j < m; ++j)
    {
        c[j] = Complex<T>{static_cast<T>(g.next()), static_cast<T>(g.next())};
    }
}

template <typename T> double rel_err(cont::ConstSpan<Complex<T>> a, cont::ConstSpan<Complex<T>> ref)
{
    double maxd = 0.0;
    double maxr = 0.0;
    for (usize i = 0; i < ref.size(); ++i)
    {
        maxr = std::max(maxr, std::hypot(static_cast<double>(ref[i].re), static_cast<double>(ref[i].im)));
        maxd = std::max(maxd, std::hypot(static_cast<double>(a[i].re) - static_cast<double>(ref[i].re),
                                         static_cast<double>(a[i].im) - static_cast<double>(ref[i].im)));
    }
    return maxd / (1.0 + maxr);
}

// type-1 vs the direct nonuniform DFT, at a given tolerance and sign.
template <typename T> double type1_vs_direct(usize n_modes, usize m, double tol, int isign, crd::memory::IAllocator* a)
{
    fft::NufftPlan<T> plan(a, n_modes, m, fft::NufftOpts{tol, 2.0});
    cont::Array<T> x(a);
    fill_points<T>(x, m, 0xABCD1234ULL ^ (n_modes * 131 + m));
    plan.set_points(cont::ConstSpan<T>(x.data(), m));
    cont::Array<Complex<T>> c(a);
    fill_strengths<T>(c, m, 0x55AA7777ULL ^ m);
    cont::Array<Complex<T>> f(a);
    cont::Array<Complex<T>> fref(a);
    f.resize(n_modes);
    fref.resize(n_modes);
    plan.type1(cont::ConstSpan<Complex<T>>(c.data(), m), cont::Span<Complex<T>>(f.data(), n_modes), isign);
    plan.direct_type1(cont::ConstSpan<Complex<T>>(c.data(), m), cont::Span<Complex<T>>(fref.data(), n_modes), isign);
    return rel_err<T>(cont::ConstSpan<Complex<T>>(f.data(), n_modes), cont::ConstSpan<Complex<T>>(fref.data(), n_modes));
}

template <typename T> double type2_vs_direct(usize n_modes, usize m, double tol, int isign, crd::memory::IAllocator* a)
{
    fft::NufftPlan<T> plan(a, n_modes, m, fft::NufftOpts{tol, 2.0});
    cont::Array<T> x(a);
    fill_points<T>(x, m, 0x1357ECCAULL ^ (n_modes * 17 + m));
    plan.set_points(cont::ConstSpan<T>(x.data(), m));
    cont::Array<Complex<T>> f(a);
    fill_strengths<T>(f, n_modes, 0x99CC3311ULL ^ n_modes);
    cont::Array<Complex<T>> c(a);
    cont::Array<Complex<T>> cref(a);
    c.resize(m);
    cref.resize(m);
    plan.type2(cont::ConstSpan<Complex<T>>(f.data(), n_modes), cont::Span<Complex<T>>(c.data(), m), isign);
    plan.direct_type2(cont::ConstSpan<Complex<T>>(f.data(), n_modes), cont::Span<Complex<T>>(cref.data(), m), isign);
    return rel_err<T>(cont::ConstSpan<Complex<T>>(c.data(), m), cont::ConstSpan<Complex<T>>(cref.data(), m));
}
} // namespace

TEST_CASE("nufft type-1 matches direct nonuniform DFT (f64)", "[nufft]")
{
    crd::memory::TlsfAllocator alloc(64ULL << 20);
    // tol 1e-9: expect agreement near or below the requested tolerance for both signs and a few sizes.
    CHECK(type1_vs_direct<f64>(64, 200, 1e-9, +1, &alloc) < 1e-7);
    CHECK(type1_vs_direct<f64>(64, 200, 1e-9, -1, &alloc) < 1e-7);
    CHECK(type1_vs_direct<f64>(256, 1000, 1e-9, +1, &alloc) < 1e-7);
    CHECK(type1_vs_direct<f64>(128, 50, 1e-9, +1, &alloc) < 1e-7); // M < N (sparse points)
}

TEST_CASE("nufft type-1 tolerance tightens accuracy (f64)", "[nufft]")
{
    crd::memory::TlsfAllocator alloc(64ULL << 20);
    const double e6 = type1_vs_direct<f64>(128, 400, 1e-6, +1, &alloc);
    const double e12 = type1_vs_direct<f64>(128, 400, 1e-12, +1, &alloc);
    CHECK(e6 < 1e-4);
    CHECK(e12 < 1e-10);
    CHECK(e12 < e6); // tighter tol => smaller error (the kernel-width lever works)
}

TEST_CASE("nufft type-2 matches direct nonuniform DFT (f64)", "[nufft]")
{
    crd::memory::TlsfAllocator alloc(64ULL << 20);
    CHECK(type2_vs_direct<f64>(64, 200, 1e-9, +1, &alloc) < 1e-7);
    CHECK(type2_vs_direct<f64>(64, 200, 1e-9, -1, &alloc) < 1e-7);
    CHECK(type2_vs_direct<f64>(256, 1000, 1e-9, +1, &alloc) < 1e-7);
}

TEST_CASE("nufft f32 path matches direct DFT at a loose tolerance", "[nufft]")
{
    crd::memory::TlsfAllocator alloc(64ULL << 20);
    CHECK(type1_vs_direct<f32>(64, 200, 1e-4, +1, &alloc) < 1e-2);
    CHECK(type2_vs_direct<f32>(64, 200, 1e-4, +1, &alloc) < 1e-2);
}

TEST_CASE("nufft type-1 spreader is run-twice bit-identical (the moat)", "[nufft]")
{
    crd::memory::TlsfAllocator alloc(64ULL << 20);
    const usize n_modes = 128;
    const usize m = 500;
    fft::NufftPlan<f64> plan(&alloc, n_modes, m, fft::NufftOpts{1e-9, 2.0});
    cont::Array<f64> x(&alloc);
    fill_points<f64>(x, m, 0xDEADBEEFULL);
    plan.set_points(cont::ConstSpan<f64>(x.data(), m));
    cont::Array<Complex<f64>> c(&alloc);
    fill_strengths<f64>(c, m, 0xFEEDFACEULL);
    cont::Array<Complex<f64>> f1(&alloc);
    cont::Array<Complex<f64>> f2(&alloc);
    f1.resize(n_modes);
    f2.resize(n_modes);
    plan.type1(cont::ConstSpan<Complex<f64>>(c.data(), m), cont::Span<Complex<f64>>(f1.data(), n_modes), +1);
    plan.type1(cont::ConstSpan<Complex<f64>>(c.data(), m), cont::Span<Complex<f64>>(f2.data(), n_modes), +1);
    CHECK(std::memcmp(f1.data(), f2.data(), n_modes * sizeof(Complex<f64>)) == 0); // bit-identical
}
