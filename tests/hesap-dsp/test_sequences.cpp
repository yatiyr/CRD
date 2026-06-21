// crd-hesap-dsp v11-r (sequences) — MLS / Gold / Kasami. Self-contained: the m-sequence two-valued periodic
// autocorrelation (2ⁿ−1 at lag 0, −1 elsewhere) IS the maximal-length property; the Gold preferred-pair
// cross-correlation is bounded by 2^((n+1)/2)+1.

#include <crd/hesap/dsp/sequences.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>
#include <cmath>

namespace dsp = crd::hesap::dsp;
namespace cont = crd::containers;
using crd::i8;
using crd::usize;

namespace
{
long circ_corr(cont::ConstSpan<i8> a, cont::ConstSpan<i8> b, usize tau)
{
    const usize len = a.size();
    long s = 0;
    for (usize i = 0; i < len; ++i)
    {
        s += static_cast<long>(a[i]) * static_cast<long>(b[(i + tau) % len]);
    }
    return s;
}
} // namespace

TEST_CASE("dsp sequences: MLS has the two-valued autocorrelation (maximal length)", "[v11-r][dsp][sequences]")
{
    crd::memory::TlsfAllocator alloc(1U << 18);
    const usize n = 7;
    const auto s = dsp::mls<i8>(&alloc, n);
    const usize len = (1u << n) - 1; // 127
    REQUIRE(s.size() == len);
    const cont::ConstSpan<i8> ss(s.data(), len);
    CHECK(circ_corr(ss, ss, 0) == static_cast<long>(len)); // peak
    for (usize tau = 1; tau < len; ++tau)
    {
        INFO("tau=" << tau);
        CHECK(circ_corr(ss, ss, tau) == -1); // two-valued: -1 off-peak
    }
    long sum = 0;
    for (usize i = 0; i < len; ++i)
    {
        sum += s[i];
    }
    CHECK(sum == -1); // balance: one more -1 than +1
}

TEST_CASE("dsp sequences: Gold preferred pair has bounded cross-correlation", "[v11-r][dsp][sequences]")
{
    crd::memory::TlsfAllocator alloc(1U << 18);
    const usize n = 5;
    const usize len = (1u << n) - 1; // 31
    const auto a = dsp::mls<i8>(&alloc, n);
    const auto b = dsp::decimate_seq<i8>(&alloc, cont::ConstSpan<i8>(a.data(), len), 9); // preferred-pair decimation
    const auto g = dsp::gold<i8>(&alloc, cont::ConstSpan<i8>(a.data(), len), cont::ConstSpan<i8>(b.data(), len));
    REQUIRE(g.size() == len);
    for (usize i = 0; i < len; ++i)
    {
        CHECK((g[i] == 1 || g[i] == -1)); // ±1 valued
    }
    // the preferred-pair cross-correlation is three-valued, bounded by 2^((n+1)/2)+1 = 9.
    long maxabs = 0;
    for (usize tau = 0; tau < len; ++tau)
    {
        maxabs = std::max(maxabs, std::labs(circ_corr(cont::ConstSpan<i8>(a.data(), len),
                                                       cont::ConstSpan<i8>(b.data(), len), tau)));
    }
    CHECK(maxabs <= 9);
}

TEST_CASE("dsp sequences: Kasami sequence is ±1, full length", "[v11-r][dsp][sequences]")
{
    crd::memory::TlsfAllocator alloc(1U << 18);
    const usize n = 6;
    const auto k = dsp::kasami<i8>(&alloc, n, 3);
    const usize len = (1u << n) - 1; // 63
    REQUIRE(k.size() == len);
    for (usize i = 0; i < len; ++i)
    {
        CHECK((k[i] == 1 || k[i] == -1));
    }
}
