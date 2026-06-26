// crd-hesap-stats v12-g — QMC sequences + ChaCha20. Sobol gated bit-vs-scipy (same Joe-Kuo direction numbers);
// Halton via independent radical-inverse anchors; ChaCha20 via the RFC all-zero-key KAT; QMC integration convergence;
// LHS stratification; determinism.

#include <crd/hesap/stats/stats.hpp>

#include "qmc_refs.inc"

#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>

namespace st = crd::hesap::stats;
using crd::u32;
using crd::u64;

TEST_CASE("qmc: Sobol matches scipy.stats.qmc (Joe-Kuo, scramble=false)", "[v12-g][stats][qmc]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(1) << 20);
    st::SobolSequence sob(4, &alloc);
    double pt[4];
    for (u64 i = 0; i < 16; ++i)
    {
        sob.point(i, crd::containers::Span<double>(pt, 4));
        for (int j = 0; j < 4; ++j)
        {
            CHECK(std::fabs(pt[j] - ref_sobol_4d_16[i * 4 + static_cast<u64>(j)]) < 1e-12);
        }
    }
}

TEST_CASE("qmc: Halton radical-inverse anchors", "[v12-g][stats][qmc]")
{
    st::HaltonSequence h(4); // bases 2,3,5,7
    double pt[4];
    h.point(1, crd::containers::Span<double>(pt, 4));
    CHECK(pt[0] == Catch::Approx(0.5));            // 1/2
    CHECK(pt[1] == Catch::Approx(1.0 / 3.0));      // 1/3
    CHECK(pt[2] == Catch::Approx(0.2));            // 1/5
    CHECK(pt[3] == Catch::Approx(1.0 / 7.0));      // 1/7
    h.point(2, crd::containers::Span<double>(pt, 4));
    CHECK(pt[0] == Catch::Approx(0.25));           // base 2: 2 = "10" → .01 = 0.25
}

TEST_CASE("qmc: low-discrepancy integration beats the 1/sqrt(N) rate", "[v12-g][stats][qmc]")
{
    // ∫_[0,1]^3 (x0 + x1^2 + x2^3) dx = 0.5 + 1/3 + 1/4 = 1.0833333...
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(1) << 20);
    st::SobolSequence sob(3, &alloc);
    const u64 n = 4096;
    double acc = 0.0;
    double pt[3];
    for (u64 i = 0; i < n; ++i)
    {
        sob.point(i, crd::containers::Span<double>(pt, 3));
        acc += pt[0] + pt[1] * pt[1] + pt[2] * pt[2] * pt[2];
    }
    const double est = acc / static_cast<double>(n);
    CHECK(std::fabs(est - (0.5 + 1.0 / 3.0 + 0.25)) < 2e-3); // far better than MC's ~1/sqrt(4096)≈0.016
}

TEST_CASE("qmc: ChaCha20 RFC all-zero-key keystream KAT", "[v12-g][stats][qmc][chacha]")
{
    const u32 key[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    const u32 nonce[2] = {0, 0};
    auto g = st::ChaCha20Rng::from_key(key, 0, nonce);
    // keystream bytes 76 b8 e0 ad a0 f1 3d 90 → u32 LE 0xade0b876, 0x903df1a0 → first u64 = 0x903df1a0ade0b876.
    CHECK(g.next_u64() == 0x903df1a0ade0b876ULL);
    // next bytes 40 5d 6a e5 53 86 bd 28 → 0xe56a5d40, 0x28bd8653.
    CHECK(g.next_u64() == 0x28bd8653e56a5d40ULL);
}

TEST_CASE("qmc: ChaCha20 determinism", "[v12-g][stats][qmc][moat]")
{
    st::ChaCha20Rng a(2024, 7);
    st::ChaCha20Rng b(2024, 7);
    for (int i = 0; i < 2000; ++i)
    {
        REQUIRE(a.next_u64() == b.next_u64());
    }
}

TEST_CASE("qmc: Latin Hypercube stratification", "[v12-g][stats][qmc]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(1) << 20);
    const u32 n = 50;
    const u32 d = 3;
    double out[150]; // n*d
    st::Pcg64Dxsm g(11);
    st::latin_hypercube(g, n, d, crd::containers::Span<double>(out, n * d), &alloc);
    // each dim: exactly one sample in each of the n equal strata.
    for (u32 j = 0; j < d; ++j)
    {
        int hit[50] = {0};
        for (u32 i = 0; i < n; ++i)
        {
            const double v = out[static_cast<crd::usize>(i) * d + j];
            REQUIRE(v >= 0.0);
            REQUIRE(v < 1.0);
            ++hit[static_cast<int>(v * n)];
        }
        for (u32 k = 0; k < n; ++k)
        {
            CHECK(hit[k] == 1); // perfect Latin stratification
        }
    }
}

TEST_CASE("qmc: rank-1 lattice points are in the half-open unit interval", "[v12-g][stats][qmc]")
{
    const u64 n = 1024;
    const u64 z[2] = {1, 433}; // a simple generating vector
    double pt[2];
    for (u64 i = 0; i < 64; ++i)
    {
        st::lattice_point(i, n, crd::containers::Span<const u64>(z, 2), crd::containers::Span<double>(pt, 2));
        CHECK(pt[0] >= 0.0);
        CHECK(pt[0] < 1.0);
        CHECK(pt[1] >= 0.0);
        CHECK(pt[1] < 1.0);
    }
}
