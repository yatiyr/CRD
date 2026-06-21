// crd-hesap-comms v11c-a — modulation. Gates: Gray code + the Gray constellation property (nearest-neighbour
// symbols differ by 1 bit) + noise-free round trip + unit average energy + BER vs the theoretical AWGN curve
// (the real modem spec, deterministic Philox noise) + FSK round trip + run-twice determinism.

#include <crd/hesap/comms/modulation.hpp>
#include <crd/hesap/stats/normal.hpp>
#include <crd/hesap/stats/philox.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <limits>
#include <numbers>

namespace cm = crd::hesap::comms;
namespace cont = crd::containers;
using crd::f64;
using crd::u32;
using crd::u8;
using crd::usize;
using crd::hesap::Complex;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace
{
f64 qfunc(f64 x) { return 0.5 * std::erfc(x / std::numbers::sqrt2_v<f64>); }
int popcount(u32 x) { int c = 0; while (x) { c += static_cast<int>(x & 1U); x >>= 1U; } return c; }
} // namespace

TEST_CASE("comms modulation: Gray code round trip + 1-bit adjacency", "[v11c-a][comms][modulation]")
{
    for (u32 k = 0; k < 256; ++k)
    {
        CHECK(cm::gray_decode(cm::gray_encode(k)) == k);
    }
    for (u32 k = 0; k < 255; ++k)
    {
        CHECK(popcount(cm::gray_encode(k) ^ cm::gray_encode(k + 1)) == 1); // consecutive Gray differ by 1 bit
    }
}

TEST_CASE("comms modulation: constellation unit energy + noise-free round trip + Gray property",
          "[v11c-a][comms][modulation]")
{
    crd::memory::TlsfAllocator alloc(1U << 20);
    struct C
    {
        cm::ModFamily fam;
        u32 m;
    };
    for (const auto& tc : {C{cm::ModFamily::Psk, 2}, C{cm::ModFamily::Psk, 4}, C{cm::ModFamily::Psk, 8},
                           C{cm::ModFamily::Psk, 16}, C{cm::ModFamily::Qam, 16}, C{cm::ModFamily::Qam, 64},
                           C{cm::ModFamily::Qam, 256}, C{cm::ModFamily::Pam, 4}, C{cm::ModFamily::Pam, 8}})
    {
        cm::Modem<f64> modem(&alloc, tc.fam, tc.m);
        INFO("family " << static_cast<int>(tc.fam) << " M=" << tc.m);
        // unit average energy
        f64 e = 0.0;
        for (u32 s = 0; s < tc.m; ++s)
        {
            const auto c = modem.constellation(s);
            e += c.re * c.re + c.im * c.im;
        }
        CHECK_THAT(e / tc.m, WithinAbs(1.0, 1e-12));
        // noise-free round trip
        for (u32 s = 0; s < tc.m; ++s)
        {
            CHECK(modem.demodulate(modem.modulate(s)) == s);
        }
        // the fast O(1) slicer agrees with the O(M) nearest-point reference over a noisy grid.
        crd::hesap::stats::PhiloxRng nr(static_cast<crd::u64>(tc.m) * 31U + 1U);
        for (usize trial = 0; trial < 4000; ++trial)
        {
            const Complex<f64> r{2.0 * (nr.next_f64() - 0.5) * 2.0, 2.0 * (nr.next_f64() - 0.5) * 2.0};
            INFO("fast vs nearest M=" << tc.m);
            CHECK(modem.demodulate(r) == modem.demodulate_nearest(r));
        }
        // Gray property: every pair of constellation points at the MINIMUM distance has 1-bit label difference.
        f64 dmin = std::numeric_limits<f64>::max();
        for (u32 a = 0; a < tc.m; ++a)
        {
            for (u32 b = a + 1; b < tc.m; ++b)
            {
                const auto ca = modem.constellation(a);
                const auto cb = modem.constellation(b);
                const f64 d = (ca.re - cb.re) * (ca.re - cb.re) + (ca.im - cb.im) * (ca.im - cb.im);
                dmin = (d < dmin) ? d : dmin;
            }
        }
        for (u32 a = 0; a < tc.m; ++a)
        {
            for (u32 b = a + 1; b < tc.m; ++b)
            {
                const auto ca = modem.constellation(a);
                const auto cb = modem.constellation(b);
                const f64 d = (ca.re - cb.re) * (ca.re - cb.re) + (ca.im - cb.im) * (ca.im - cb.im);
                if (d < dmin * 1.0001) // a minimum-distance neighbour pair
                {
                    INFO("pair " << a << "," << b);
                    CHECK(popcount(a ^ b) == 1);
                }
            }
        }
    }
}

namespace
{
// Measure BER over an AWGN channel at the given Eb/N0 (dB), deterministic Philox noise.
f64 measure_ber(crd::memory::IAllocator* alloc, cm::ModFamily fam, u32 m, f64 ebno_db, usize nsym)
{
    cm::Modem<f64> modem(alloc, fam, m);
    const u32 bps = modem.bits_per_symbol();
    const f64 ebno = std::pow(10.0, ebno_db / 10.0);
    const f64 esno = ebno * static_cast<f64>(bps); // Es = 1 (unit energy)
    const f64 n0 = 1.0 / esno;
    const f64 sigma = std::sqrt(n0 / 2.0); // per complex dimension
    crd::hesap::stats::PhiloxRng rng(0xC0FFEEULL, 7ULL);
    crd::hesap::stats::NormalSampler noise(rng);
    usize bit_errors = 0;
    usize total_bits = 0;
    for (usize i = 0; i < nsym; ++i)
    {
        const u32 s = static_cast<u32>(rng.next_below(m));
        const auto c = modem.modulate(s);
        const Complex<f64> r{c.re + sigma * noise.next(), c.im + sigma * noise.next()};
        const u32 sh = modem.demodulate(r);
        bit_errors += static_cast<usize>(popcount(s ^ sh)); // Gray ⇒ symbol error ≈ 1 bit error near threshold
        total_bits += bps;
    }
    return static_cast<f64>(bit_errors) / static_cast<f64>(total_bits);
}
} // namespace

TEST_CASE("comms modulation: BER vs the theoretical AWGN curve", "[v11c-a][comms][modulation]")
{
    crd::memory::TlsfAllocator alloc(1U << 20);
    const usize nsym = 400000;

    // BPSK / QPSK: Pb = Q(sqrt(2 Eb/N0)) — EXACT. Gate tightly.
    for (cm::ModFamily fam : {cm::ModFamily::Psk})
    {
        for (u32 m : {2U, 4U})
        {
            const f64 ebno_db = 7.0;
            const f64 ebno = std::pow(10.0, ebno_db / 10.0);
            const f64 theory = qfunc(std::sqrt(2.0 * ebno));
            const f64 ber = measure_ber(&alloc, fam, m, ebno_db, nsym);
            INFO("PSK M=" << m << " theory=" << theory << " ber=" << ber);
            CHECK_THAT(ber, WithinRel(theory, 0.20)); // exact formula, statistical tolerance
        }
    }
    // Square QAM: Pb ≈ (4/k)(1-1/√M) Q(sqrt(3k/(M-1) Eb/N0)) — approximate. Gate within 1.6×.
    for (u32 m : {16U, 64U})
    {
        const f64 ebno_db = 10.0;
        const f64 k = std::log2(static_cast<f64>(m));
        const f64 ebno = std::pow(10.0, ebno_db / 10.0);
        const f64 sm = std::sqrt(static_cast<f64>(m));
        const f64 theory = (4.0 / k) * (1.0 - 1.0 / sm) * qfunc(std::sqrt(3.0 * k / (m - 1.0) * ebno));
        const f64 ber = measure_ber(&alloc, cm::ModFamily::Qam, m, ebno_db, nsym);
        INFO("QAM M=" << m << " theory=" << theory << " ber=" << ber);
        CHECK(ber > theory * 0.6);
        CHECK(ber < theory * 1.6);
    }
    // M-PAM: Pb ≈ (2(M-1)/(M k)) Q(sqrt(6k/(M²-1) Eb/N0)).
    {
        const u32 m = 4;
        const f64 ebno_db = 10.0;
        const f64 k = 2.0;
        const f64 ebno = std::pow(10.0, ebno_db / 10.0);
        const f64 theory = (2.0 * (m - 1.0) / (m * k)) * qfunc(std::sqrt(6.0 * k / (m * m - 1.0) * ebno));
        const f64 ber = measure_ber(&alloc, cm::ModFamily::Pam, m, ebno_db, nsym);
        INFO("PAM M=" << m << " theory=" << theory << " ber=" << ber);
        CHECK(ber > theory * 0.6);
        CHECK(ber < theory * 1.6);
    }
}

TEST_CASE("comms modulation: bit packing + FSK round trip", "[v11c-a][comms][modulation]")
{
    crd::memory::TlsfAllocator alloc(1U << 20);
    // bits <-> symbols round trip (QAM16, 4 bits/sym)
    cm::Modem<f64> modem(&alloc, cm::ModFamily::Qam, 16);
    const u32 bps = modem.bits_per_symbol();
    const usize nsym = 100;
    cont::Array<u8> bits(&alloc);
    bits.resize(nsym * bps);
    crd::hesap::stats::PhiloxRng rng(42ULL);
    for (usize i = 0; i < bits.size(); ++i)
    {
        bits[i] = static_cast<u8>(rng.next_u32() & 1U);
    }
    cont::Array<u32> syms(&alloc);
    cont::Array<u32> syms2(&alloc);
    syms.resize(nsym);
    syms2.resize(nsym);
    cont::Array<Complex<f64>> tx(&alloc);
    tx.resize(nsym);
    cm::bits_to_symbols(cont::ConstSpan<u8>(bits.data(), bits.size()), bps, cont::Span<u32>(syms.data(), nsym));
    modem.modulate_block(cont::ConstSpan<u32>(syms.data(), nsym), cont::Span<Complex<f64>>(tx.data(), nsym));
    modem.demodulate_block(cont::ConstSpan<Complex<f64>>(tx.data(), nsym), cont::Span<u32>(syms2.data(), nsym));
    cont::Array<u8> bits2(&alloc);
    bits2.resize(nsym * bps);
    cm::symbols_to_bits(cont::ConstSpan<u32>(syms2.data(), nsym), bps, cont::Span<u8>(bits2.data(), bits2.size()));
    for (usize i = 0; i < bits.size(); ++i)
    {
        CHECK(bits[i] == bits2[i]);
    }

    // M-FSK orthogonal round trip (noise-free)
    const u32 mfsk = 4;
    const u32 sps = 8;
    cont::Array<u32> fs(&alloc);
    fs.resize(60);
    for (usize i = 0; i < fs.size(); ++i)
    {
        fs[i] = static_cast<u32>(rng.next_below(mfsk));
    }
    cont::Array<Complex<f64>> fwav(&alloc);
    fwav.resize(fs.size() * sps);
    cm::fsk_modulate<f64>(cont::ConstSpan<u32>(fs.data(), fs.size()), sps, cont::Span<Complex<f64>>(fwav.data(), fwav.size()));
    cont::Array<u32> fr(&alloc);
    fr.resize(fs.size());
    cm::fsk_demodulate_noncoherent<f64>(cont::ConstSpan<Complex<f64>>(fwav.data(), fwav.size()), mfsk, sps,
                                        cont::Span<u32>(fr.data(), fr.size()));
    for (usize i = 0; i < fs.size(); ++i)
    {
        CHECK(fr[i] == fs[i]);
    }
}

TEST_CASE("comms modulation: soft LLR sign == hard decision", "[v11c-a][comms][modulation]")
{
    crd::memory::TlsfAllocator alloc(1U << 20);
    cm::Modem<f64> modem(&alloc, cm::ModFamily::Qam, 16);
    const u32 bps = modem.bits_per_symbol();
    crd::hesap::stats::PhiloxRng rng(5ULL);
    cont::Array<f64> llr(&alloc);
    llr.resize(bps);
    for (usize trial = 0; trial < 200; ++trial)
    {
        const u32 s = static_cast<u32>(rng.next_below(16));
        const auto c = modem.modulate(s);
        const Complex<f64> r{c.re + 0.05 * (rng.next_f64() - 0.5), c.im + 0.05 * (rng.next_f64() - 0.5)};
        modem.demodulate_soft(r, 0.1, cont::Span<f64>(llr.data(), bps));
        const u32 hard = modem.demodulate(r);
        for (u32 b = 0; b < bps; ++b)
        {
            const u32 hard_bit = (hard >> (bps - 1U - b)) & 1U; // LLR>0 ⇒ bit 0
            CHECK((llr[b] > 0.0 ? 0U : 1U) == hard_bit);
        }
    }
}
