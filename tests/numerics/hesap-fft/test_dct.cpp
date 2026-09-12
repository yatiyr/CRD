// v10-f gates: DCT-II/III + DST-II/III via Makhoul-over-FFT. THE gate is each transform vs its DIRECT O(N²)
// sum (verified == scipy.fft in scripts/dct_research.py), NOT a forward∘inverse round-trip — which can cancel
// a sign/shuffle error (the odeint-d4 trap). Round-trip dct3(dct2(x))==2N·x is a SECONDARY check. Plus f32 +
// run-twice determinism.

#include <crd/containers/array.hpp>
#include <crd/hesap/fft/dct.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstring>

using crd::f32;
using crd::f64;
using crd::usize;
namespace fft = crd::hesap::fft;
namespace cont = crd::containers;

namespace
{
template <typename T> void fill(cont::Array<T>& x, usize n, crd::u64 seed)
{
    x.resize(n);
    crd::u64 s = seed;
    for (usize i = 0; i < n; ++i)
    {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        x[i] = static_cast<T>((static_cast<double>(s >> 11) / static_cast<double>(1ULL << 53)) * 2.0 - 1.0);
    }
}

template <typename T> double rel(cont::ConstSpan<T> a, cont::ConstSpan<T> ref)
{
    double maxd = 0.0;
    double maxr = 0.0;
    for (usize i = 0; i < ref.size(); ++i)
    {
        maxr = std::max(maxr, std::abs(static_cast<double>(ref[i])));
        maxd = std::max(maxd, std::abs(static_cast<double>(a[i]) - static_cast<double>(ref[i])));
    }
    return maxd / (1.0 + maxr);
}

enum class Kind : crd::u8
{
    Dct2,
    Dct3,
    Dst2,
    Dst3
};

template <typename T> double vs_direct(Kind kind, usize n, crd::memory::IAllocator* a)
{
    fft::DctPlan<T> plan(a, n);
    cont::Array<T> x(a);
    fill<T>(x, n, 0xC7C7ULL ^ (n * 7 + static_cast<usize>(kind)));
    cont::Array<T> y(a);
    cont::Array<T> yref(a);
    y.resize(n);
    yref.resize(n);
    const cont::ConstSpan<T> xs(x.data(), n);
    const cont::Span<T> ys(y.data(), n);
    const cont::Span<T> rs(yref.data(), n);
    switch (kind)
    {
    case Kind::Dct2: plan.dct2(xs, ys); plan.direct_dct2(xs, rs); break;
    case Kind::Dct3: plan.dct3(xs, ys); plan.direct_dct3(xs, rs); break;
    case Kind::Dst2: plan.dst2(xs, ys); plan.direct_dst2(xs, rs); break;
    case Kind::Dst3: plan.dst3(xs, ys); plan.direct_dst3(xs, rs); break;
    }
    return rel<T>(cont::ConstSpan<T>(y.data(), n), cont::ConstSpan<T>(yref.data(), n));
}
} // namespace

TEST_CASE("dct: DCT-II/III + DST-II/III match the direct O(N^2) sum (f64)", "[dct]")
{
    crd::memory::TlsfAllocator alloc(16ULL << 20);
    for (usize n : {2U, 4U, 8U, 16U, 64U, 256U, 1024U})
    {
        CHECK(vs_direct<f64>(Kind::Dct2, n, &alloc) < 1e-12);
        CHECK(vs_direct<f64>(Kind::Dct3, n, &alloc) < 1e-12);
        CHECK(vs_direct<f64>(Kind::Dst2, n, &alloc) < 1e-12);
        CHECK(vs_direct<f64>(Kind::Dst3, n, &alloc) < 1e-12);
    }
}

TEST_CASE("dct: f32 path matches the direct sum at a looser tolerance", "[dct]")
{
    crd::memory::TlsfAllocator alloc(16ULL << 20);
    for (usize n : {8U, 64U, 512U})
    {
        CHECK(vs_direct<f32>(Kind::Dct2, n, &alloc) < 1e-4);
        CHECK(vs_direct<f32>(Kind::Dst2, n, &alloc) < 1e-4);
    }
}

TEST_CASE("dct: inverse round-trips to 2N*x (secondary)", "[dct]")
{
    crd::memory::TlsfAllocator alloc(16ULL << 20);
    for (usize n : {4U, 16U, 256U, 1024U})
    {
        fft::DctPlan<f64> plan(&alloc, n);
        cont::Array<f64> x(&alloc);
        fill<f64>(x, n, 0x1234ULL ^ n);
        cont::Array<f64> t(&alloc);
        cont::Array<f64> r(&alloc);
        t.resize(n);
        r.resize(n);
        // DCT: dct3(dct2(x)) == 2N x
        plan.dct2(cont::ConstSpan<f64>(x.data(), n), cont::Span<f64>(t.data(), n));
        plan.dct3(cont::ConstSpan<f64>(t.data(), n), cont::Span<f64>(r.data(), n));
        double e = 0.0;
        const double s = 2.0 * static_cast<double>(n);
        for (usize i = 0; i < n; ++i)
        {
            e = std::max(e, std::abs(r[i] - s * x[i]));
        }
        CHECK(e / (1.0 + s) < 1e-12);
        // DST: dst3(dst2(x)) == 2N x
        plan.dst2(cont::ConstSpan<f64>(x.data(), n), cont::Span<f64>(t.data(), n));
        plan.dst3(cont::ConstSpan<f64>(t.data(), n), cont::Span<f64>(r.data(), n));
        double e2 = 0.0;
        for (usize i = 0; i < n; ++i)
        {
            e2 = std::max(e2, std::abs(r[i] - s * x[i]));
        }
        CHECK(e2 / (1.0 + s) < 1e-12);
    }
}

TEST_CASE("dct: run-twice bit-identical (determinism)", "[dct]")
{
    crd::memory::TlsfAllocator alloc(16ULL << 20);
    const usize n = 512;
    fft::DctPlan<f64> plan(&alloc, n);
    cont::Array<f64> x(&alloc);
    fill<f64>(x, n, 0xBEEFULL);
    cont::Array<f64> y1(&alloc);
    cont::Array<f64> y2(&alloc);
    y1.resize(n);
    y2.resize(n);
    plan.dct2(cont::ConstSpan<f64>(x.data(), n), cont::Span<f64>(y1.data(), n));
    plan.dct2(cont::ConstSpan<f64>(x.data(), n), cont::Span<f64>(y2.data(), n));
    CHECK(std::memcmp(y1.data(), y2.data(), n * sizeof(f64)) == 0);
}
