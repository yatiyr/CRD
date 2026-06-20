// crd-hesap-dsp v11-m — Welch PSD gates: vs scipy.signal.welch (~1e-12) + the {1..16} MULTI-THREADED-FFT
// determinism moat (the PSD is BIT-IDENTICAL across thread counts — per-job plans + serial fixed-order average).
#include <crd/hesap/dsp/spectral.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>
#include "spectral_refs.inc"
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
namespace dsp = crd::hesap::dsp;
namespace cont = crd::containers;
using crd::f64; using crd::usize;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

TEST_CASE("dsp spectral: Welch PSD matches scipy + bit-identical across {1,2,4,8,16} threads (FFT moat)",
          "[v11-m][dsp][spectral][moat]")
{
    crd::memory::TlsfAllocator alloc(1U << 24);
    const usize N = sizeof(ref_welch_x) / sizeof(double);
    const usize nfreq = sizeof(ref_welch_pxx) / sizeof(double);

    cont::Array<f64> ref1(&alloc); // the 1-thread result, used as the moat reference
    bool have_ref = false;
    for (crd::u32 nw : {1U, 2U, 4U, 8U, 16U})
    {
        crd::jobs::Config cfg;
        cfg.num_threads = nw;
        crd::jobs::init(cfg);
        {
            const auto psd = dsp::welch_psd<f64>(&alloc, cont::ConstSpan<f64>(ref_welch_x, N), 100.0, 64);
            REQUIRE(psd.size() == nfreq);
            if (!have_ref)
            {
                // gate vs scipy (only once — same numbers every thread count).
                for (usize k = 0; k < nfreq; ++k)
                {
                    INFO("Pxx[" << k << "] nw=" << nw);
                    CHECK_THAT(psd[k], WithinRel(ref_welch_pxx[k], 1e-11));
                }
                ref1.resize(nfreq);
                for (usize k = 0; k < nfreq; ++k) { ref1[k] = psd[k]; }
                have_ref = true;
            }
            else
            {
                bool ident = true;
                for (usize k = 0; k < nfreq && ident; ++k) { ident = (psd[k] == ref1[k]); }
                INFO("thread count " << nw);
                CHECK(ident); // BIT-IDENTICAL across thread counts — the multi-threaded-FFT determinism moat
            }
        }
        crd::jobs::shutdown();
    }
}

TEST_CASE("dsp spectral: spectrogram matches scipy + bit-identical across threads", "[v11-m][dsp][spectral][moat]")
{
    crd::memory::TlsfAllocator alloc(1U << 24);
    const usize N = sizeof(ref_welch_x) / sizeof(double);
    cont::Array<f64> ref1(&alloc);
    bool have_ref = false;
    for (crd::u32 nw : {1U, 4U, 16U})
    {
        crd::jobs::Config cfg;
        cfg.num_threads = nw;
        crd::jobs::init(cfg);
        {
            usize nseg = 0, nfreq = 0;
            const auto S = dsp::spectrogram<f64>(&alloc, cont::ConstSpan<f64>(ref_welch_x, N), 100.0, 64, 32, nseg, nfreq);
            REQUIRE(static_cast<int>(nseg) == ref_spec_nseg);
            REQUIRE(static_cast<int>(nfreq) == ref_spec_nfreq);
            if (!have_ref)
            {
                for (usize i = 0; i < nseg * nfreq; ++i)
                {
                    INFO("S[" << i << "]");
                    // rel for the energetic bins; abs for the ~1e-12 noise-floor bins (rel is meaningless near 0).
                    CHECK_THAT(S[i], WithinRel(ref_spec[i], 1e-9) || WithinAbs(ref_spec[i], 1e-11));
                }
                ref1.resize(nseg * nfreq);
                for (usize i = 0; i < nseg * nfreq; ++i) { ref1[i] = S[i]; }
                have_ref = true;
            }
            else
            {
                bool ident = true;
                for (usize i = 0; i < nseg * nfreq && ident; ++i) { ident = (S[i] == ref1[i]); }
                INFO("threads " << nw);
                CHECK(ident); // spectrogram bit-identical across thread counts
            }
        }
        crd::jobs::shutdown();
    }
}

TEST_CASE("dsp spectral: STFT/ISTFT perfect reconstruction + {1..16} bit-identity moat", "[v11-m][dsp][stft][moat]")
{
    crd::memory::TlsfAllocator alloc(1U << 24);
    const usize N = sizeof(ref_welch_x) / sizeof(double);
    const usize nperseg = 64, noverlap = 32;

    cont::Array<crd::hesap::Complex<f64>> ref1(&alloc);
    bool have_ref = false;
    for (crd::u32 nw : {1U, 4U, 16U})
    {
        crd::jobs::Config cfg;
        cfg.num_threads = nw;
        crd::jobs::init(cfg);
        {
            usize nframes = 0, nfreq = 0;
            const auto Z = dsp::stft<f64>(&alloc, cont::ConstSpan<f64>(ref_welch_x, N), nperseg, noverlap, nframes, nfreq);
            REQUIRE(nframes > 0);
            if (!have_ref)
            {
                // perfect reconstruction: istft(stft(x)) == x on the interior (COLA hann + 50% overlap).
                const auto xr = dsp::istft<f64>(&alloc, cont::ConstSpan<crd::hesap::Complex<f64>>(Z.data(), Z.size()),
                                                nframes, nfreq, nperseg, noverlap);
                for (usize i = nperseg; i + nperseg < xr.size(); ++i)
                {
                    INFO("reconstruct[" << i << "]");
                    CHECK_THAT(xr[i], WithinAbs(ref_welch_x[i], 1e-11));
                }
                ref1.resize(Z.size());
                for (usize i = 0; i < Z.size(); ++i) { ref1[i] = Z[i]; }
                have_ref = true;
            }
            else
            {
                bool ident = true;
                for (usize i = 0; i < Z.size() && ident; ++i) { ident = (Z[i].re == ref1[i].re) && (Z[i].im == ref1[i].im); }
                INFO("threads " << nw);
                CHECK(ident); // STFT complex spectra bit-identical across thread counts (the FFT moat)
            }
        }
        crd::jobs::shutdown();
    }
}

TEST_CASE("dsp spectral: periodogram + csd + coherence match scipy", "[v11-m][dsp][spectral]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    const usize N = sizeof(ref_welch_x) / sizeof(double);
    // periodogram (single segment, boxcar).
    {
        const auto P = dsp::periodogram<f64>(&alloc, cont::ConstSpan<f64>(ref_welch_x, N), 100.0);
        REQUIRE(P.size() == sizeof(ref_periodogram) / sizeof(double));
        for (usize k = 0; k < P.size(); ++k)
        {
            INFO("per[" << k << "]");
            CHECK_THAT(P[k], WithinRel(ref_periodogram[k], 1e-9) || WithinAbs(ref_periodogram[k], 1e-12));
        }
    }
    // csd + coherence (x vs y).
    {
        const usize nf = sizeof(ref_csd_re) / sizeof(double);
        const auto Pxy = dsp::csd<f64>(&alloc, cont::ConstSpan<f64>(ref_welch_x, N), cont::ConstSpan<f64>(ref_y, N), 100.0, 64);
        REQUIRE(Pxy.size() == nf);
        for (usize k = 0; k < nf; ++k)
        {
            INFO("csd[" << k << "]");
            CHECK_THAT(Pxy[k].re, WithinRel(ref_csd_re[k], 1e-9) || WithinAbs(ref_csd_re[k], 1e-12));
            CHECK_THAT(Pxy[k].im, WithinRel(ref_csd_im[k], 1e-9) || WithinAbs(ref_csd_im[k], 1e-12));
        }
        const auto Cxy = dsp::coherence<f64>(&alloc, cont::ConstSpan<f64>(ref_welch_x, N), cont::ConstSpan<f64>(ref_y, N), 100.0, 64);
        for (usize k = 0; k < nf; ++k)
        {
            INFO("coh[" << k << "]");
            CHECK_THAT(Cxy[k], WithinAbs(ref_coherence[k], 1e-9));
            CHECK((Cxy[k] >= -1e-12 && Cxy[k] <= 1.0 + 1e-9)); // coherence in [0,1]
        }
    }
}
