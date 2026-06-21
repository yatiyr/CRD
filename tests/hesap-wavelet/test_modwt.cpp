// crd-hesap-wavelet v11w-e — MODWT (Percival & Walden). Self-contained gates (pywt has no modwt):
// perfect reconstruction imodwt(modwt(x))==x + the energy partition Σ_j||W_j||²+||V_J||²=||x||² + run-twice.

#include <crd/hesap/wavelet/modwt.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <cstring>

namespace wv = crd::hesap::wavelet;
namespace cont = crd::containers;
using crd::f64;
using crd::u64;
using crd::usize;
using Catch::Matchers::WithinAbs;

namespace
{
cont::Array<f64> make_signal(crd::memory::IAllocator* a, usize n)
{
    cont::Array<f64> x(a);
    x.resize(n);
    u64 s = 999ULL;
    for (usize i = 0; i < n; ++i)
    {
        const f64 t = static_cast<f64>(i);
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        const f64 u = (static_cast<f64>(s >> 11) * (1.0 / 9007199254740992.0)) * 2.0 - 1.0;
        x[i] = std::sin(0.07 * t) + 0.3 * u; // non-power-of-two length too
    }
    return x;
}
} // namespace

TEST_CASE("modwt: perfect reconstruction + energy partition", "[v11w-e][wavelet][modwt]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    for (const char* name : {"haar", "db2", "db4", "sym4"})
    {
        for (usize n : {200U, 256U}) // MODWT works for ANY length (not just powers of two)
        {
            const auto x = make_signal(&alloc, n);
            const cont::ConstSpan<f64> xs(x.data(), n);
            const auto w = wv::wavelet_by_name(name);
            const usize level = 3;
            const auto m = wv::modwt<f64>(&alloc, xs, *w, level);
            REQUIRE(m.w.size() == level);
            // perfect reconstruction
            const auto rec = wv::imodwt<f64>(&alloc, m, *w);
            REQUIRE(rec.size() == n);
            INFO(name << " n=" << n);
            for (usize i = 0; i < n; ++i)
            {
                INFO("i=" << i);
                CHECK_THAT(rec[i], WithinAbs(x[i], 1e-10));
            }
            // energy partition: Σ_j ||W_j||² + ||V_J||² == ||x||²
            f64 energy = 0.0;
            for (usize i = 0; i < n; ++i)
            {
                energy += x[i] * x[i];
            }
            f64 sum = 0.0;
            for (usize j = 0; j < level; ++j)
            {
                for (usize i = 0; i < n; ++i)
                {
                    sum += m.w[j][i] * m.w[j][i];
                }
            }
            for (usize i = 0; i < n; ++i)
            {
                sum += m.v[i] * m.v[i];
            }
            CHECK_THAT(sum, WithinAbs(energy, 1e-9));
        }
    }
}

TEST_CASE("modwt: run-twice bit-identical (determinism moat)", "[v11w-e][wavelet][modwt][moat]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    const usize n = 256;
    const auto x = make_signal(&alloc, n);
    const cont::ConstSpan<f64> xs(x.data(), n);
    const auto w = wv::wavelet_by_name("db4");
    const auto a = wv::modwt<f64>(&alloc, xs, *w, 4);
    const auto b = wv::modwt<f64>(&alloc, xs, *w, 4);
    for (usize j = 0; j < a.w.size(); ++j)
    {
        CHECK(std::memcmp(a.w[j].data(), b.w[j].data(), n * sizeof(f64)) == 0);
    }
    CHECK(std::memcmp(a.v.data(), b.v.data(), n * sizeof(f64)) == 0);
}
