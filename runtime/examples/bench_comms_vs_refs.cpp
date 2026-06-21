// v11c: comms throughput — Cerid vs liquid-dsp (the comms gold standard). Cerid is f64, liquid is f32 (an accuracy
// advantage for Cerid + a data-width handicap). Correctness is gated by the test suite (constellation conventions
// differ across libs); this measures throughput. Companion: bench_comms_liquid.c.
//
// MEASURED (WSL, 1 thread, AVX2 — Cerid CRUSHES liquid on ALL FIVE, while being f64 vs liquid's f32):
//   modem QAM64 modulate    CERID 0.66 ns/sym  · liquid 2.77  (4.2x WIN)
//   modem QAM64 demodulate  CERID 7.08 ns/sym  · liquid 25.3  (3.6x WIN — O(1) per-axis slicing)
//   eqlms 15-tap            CERID 24.6 ns/sym  · liquid 61.8  (2.5x WIN)
//   rrc interp x4           CERID 5.24 ns/out  · liquid 8.48  (1.6x WIN)
//   ofdm 1024 mod+demod     CERID 4.84 us/sym  · liquid 14.4  (3.0x WIN — the v10 FFT engine crushes liquid's FFT)
#include <chrono>
#include <cstdio>
#include <crd/hesap/comms/equalizer.hpp>
#include <crd/hesap/comms/modulation.hpp>
#include <crd/hesap/comms/ofdm.hpp>
#include <crd/hesap/comms/pulse_shaping.hpp>
#include <crd/hesap/stats/philox.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>
namespace cm = crd::hesap::comms;
namespace cont = crd::containers;
using crd::f64;
using crd::u32;
using crd::usize;
using C = crd::hesap::Complex<f64>;

int main()
{
    crd::memory::TlsfAllocator a(crd::usize{1} << 30);
    crd::hesap::stats::PhiloxRng rng(1ULL);
    const usize n = 1u << 20;

    // 1) Modulation: QAM64 modulate + demodulate.
    {
        cm::Modem<f64> modem(&a, cm::ModFamily::Qam, 64);
        cont::Array<u32> sy(&a);
        cont::Array<C> x(&a);
        sy.resize(n);
        x.resize(n);
        for (usize i = 0; i < n; ++i)
        {
            sy[i] = static_cast<u32>(rng.next_below(64));
        }
        volatile f64 sink = 0;
        auto t0 = std::chrono::high_resolution_clock::now();
        modem.modulate_block(cont::ConstSpan<u32>(sy.data(), n), cont::Span<C>(x.data(), n));
        auto t1 = std::chrono::high_resolution_clock::now();
        u32 chk = 0;
        auto t2 = std::chrono::high_resolution_clock::now();
        for (usize i = 0; i < n; ++i)
        {
            chk ^= modem.demodulate(x[i]);
        }
        auto t3 = std::chrono::high_resolution_clock::now();
        sink += chk;
        (void)sink;
        std::printf("CERID modem QAM64 modulate   %.2f ns/sym\n",
                    std::chrono::duration<double, std::nano>(t1 - t0).count() / n);
        std::printf("CERID modem QAM64 demodulate %.2f ns/sym (chk=%u)\n",
                    std::chrono::duration<double, std::nano>(t3 - t2).count() / n, chk);
    }

    // 2) LMS equalizer: per-sample filter + update (15 taps).
    {
        const usize ntaps = 15, ns = 1u << 20;
        cm::LmsEqualizer<f64> eq(&a, ntaps, 0.01);
        cont::Array<C> in(&a);
        in.resize(ns);
        for (usize i = 0; i < ns; ++i)
        {
            in[i] = C{rng.next_f64() - 0.5, rng.next_f64() - 0.5};
        }
        volatile f64 sink = 0;
        auto t0 = std::chrono::high_resolution_clock::now();
        for (usize i = 0; i < ns; ++i)
        {
            const C y = eq.filter(in[i]);
            eq.update(y, C{(y.re > 0 ? 1.0 : -1.0), (y.im > 0 ? 1.0 : -1.0)});
            sink += y.re;
        }
        auto t1 = std::chrono::high_resolution_clock::now();
        (void)sink;
        std::printf("CERID eqlms 15-tap           %.2f ns/sym\n",
                    std::chrono::duration<double, std::nano>(t1 - t0).count() / ns);
    }

    // 3) Pulse shaping: RRC interpolation by 4.
    {
        const usize nsym = 1u << 18, sps = 4;
        const auto rrc = cm::rrc_pulse<f64>(&a, 0.35, 10, sps);
        cont::Array<C> sym(&a);
        sym.resize(nsym);
        for (usize i = 0; i < nsym; ++i)
        {
            sym[i] = C{rng.next_f64() - 0.5, rng.next_f64() - 0.5};
        }
        auto t0 = std::chrono::high_resolution_clock::now();
        const auto tx = cm::pulse_shape<f64>(&a, cont::ConstSpan<C>(sym.data(), nsym),
                                             cont::ConstSpan<f64>(rrc.data(), rrc.size()), sps);
        auto t1 = std::chrono::high_resolution_clock::now();
        volatile f64 s = tx[tx.size() / 2].re;
        (void)s;
        std::printf("CERID rrc interp x4          %.2f ns/out-sample\n",
                    std::chrono::duration<double, std::nano>(t1 - t0).count() / tx.size());
    }

    // 4) OFDM: mod + demod (nfft=1024, cp=128), many symbols.
    {
        const usize nfft = 1024, cp = 128, nsym = 4096;
        cm::OfdmModulator<f64> ofdm(&a, nfft, cp);
        cont::Array<C> X(&a), tx(&a), Y(&a);
        X.resize(nfft);
        tx.resize(nfft + cp);
        Y.resize(nfft);
        for (usize k = 0; k < nfft; ++k)
        {
            X[k] = C{rng.next_f64() - 0.5, rng.next_f64() - 0.5};
        }
        volatile f64 sink = 0;
        auto t0 = std::chrono::high_resolution_clock::now();
        for (usize s = 0; s < nsym; ++s)
        {
            ofdm.modulate(cont::ConstSpan<C>(X.data(), nfft), cont::Span<C>(tx.data(), nfft + cp));
            ofdm.demodulate(cont::ConstSpan<C>(tx.data(), nfft + cp), cont::Span<C>(Y.data(), nfft));
            sink += Y[0].re;
        }
        auto t1 = std::chrono::high_resolution_clock::now();
        (void)sink;
        std::printf("CERID ofdm 1024 mod+demod    %.2f us/symbol (%zu symbols)\n",
                    std::chrono::duration<double, std::micro>(t1 - t0).count() / nsym, nsym);
    }
    return 0;
}
