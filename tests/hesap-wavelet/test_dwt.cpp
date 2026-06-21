// crd-hesap-wavelet v11w-b — DWT/IDWT/wavedec/waverec.
// Core gate: single-level DWT coefficients per-mode vs pywt (the convention pin — PR alone is not sufficient);
// IDWT vs pywt; multilevel wavedec coeffs vs pywt + perfect reconstruction; run-twice bit-identical (the moat).

#include <crd/hesap/wavelet/dwt.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <cstring>

namespace wv = crd::hesap::wavelet;
namespace cont = crd::containers;
using crd::f64;
using crd::usize;
using Catch::Matchers::WithinAbs;
using Mode = wv::SignalExtensionMode;

namespace
{
#include "wavelet_refs.inc"

template <usize N> void check_arr(const double (&ref)[N], const cont::Array<f64>& got, f64 tol)
{
    REQUIRE(got.size() == N);
    for (usize i = 0; i < N; ++i)
    {
        INFO("idx " << i);
        CHECK_THAT(got[i], WithinAbs(ref[i], tol));
    }
}

cont::ConstSpan<f64> span_of(const cont::Array<f64>& a)
{
    return cont::ConstSpan<f64>(a.data(), a.size());
}
} // namespace

// Per (wavelet, mode): dwt the shared input and gate cA/cD vs pywt.
#define CHECK_DWT(WAV, WAVSAN, MODE, MODEENUM)                                                                          \
    do                                                                                                                  \
    {                                                                                                                   \
        const auto w = wv::wavelet_by_name(WAV);                                                                        \
        REQUIRE(w.has_value());                                                                                         \
        cont::Array<f64> cA(&alloc), cD(&alloc);                                                                        \
        wv::dwt<f64>(&alloc, xs, *w, Mode::MODEENUM, cA, cD);                                                           \
        INFO(WAV << " / " << #MODE);                                                                                    \
        check_arr(ref_##WAVSAN##_##MODE##_cA, cA, 1e-11);                                                               \
        check_arr(ref_##WAVSAN##_##MODE##_cD, cD, 1e-11);                                                               \
    } while (0)

#define CHECK_ALL_MODES(WAV, WAVSAN)                                                                                    \
    CHECK_DWT(WAV, WAVSAN, zero, Zero);                                                                                 \
    CHECK_DWT(WAV, WAVSAN, symmetric, Symmetric);                                                                       \
    CHECK_DWT(WAV, WAVSAN, periodic, Periodic);                                                                         \
    CHECK_DWT(WAV, WAVSAN, periodization, Periodization);                                                               \
    CHECK_DWT(WAV, WAVSAN, reflect, Reflect);                                                                           \
    CHECK_DWT(WAV, WAVSAN, constant, Constant);                                                                         \
    CHECK_DWT(WAV, WAVSAN, smooth, Smooth);                                                                             \
    CHECK_DWT(WAV, WAVSAN, antisymmetric, Antisymmetric);                                                               \
    CHECK_DWT(WAV, WAVSAN, antireflect, Antireflect)

TEST_CASE("dwt: single-level coefficients per-mode vs pywt", "[v11w-b][wavelet][dwt]")
{
    crd::memory::TlsfAllocator alloc(1U << 20);
    const cont::ConstSpan<f64> xs(ref_input, sizeof(ref_input) / sizeof(ref_input[0]));
    CHECK_ALL_MODES("haar", haar);
    CHECK_ALL_MODES("db2", db2);
    CHECK_ALL_MODES("db4", db4);
    CHECK_ALL_MODES("sym4", sym4);
    CHECK_ALL_MODES("coif1", coif1);
    CHECK_ALL_MODES("bior2.2", bior2_2);
    CHECK_ALL_MODES("rbio2.2", rbio2_2);
}

TEST_CASE("idwt: single-level inverse vs pywt", "[v11w-b][wavelet][dwt]")
{
    crd::memory::TlsfAllocator alloc(1U << 20);
    const cont::ConstSpan<f64> xs(ref_input, sizeof(ref_input) / sizeof(ref_input[0]));

    struct Case
    {
        const char* name;
        Mode mode;
    };
    const auto run = [&](const char* name, Mode mode, auto&& ref) {
        const auto w = wv::wavelet_by_name(name);
        REQUIRE(w.has_value());
        cont::Array<f64> cA(&alloc), cD(&alloc), y(&alloc);
        wv::dwt<f64>(&alloc, xs, *w, mode, cA, cD);
        wv::idwt<f64>(&alloc, span_of(cA), span_of(cD), *w, mode, y);
        check_arr(ref, y, 1e-11);
    };
    run("db4", Mode::Symmetric, ref_idwt_db4_symmetric);
    run("db4", Mode::Periodization, ref_idwt_db4_periodization);
    run("bior2.2", Mode::Symmetric, ref_idwt_bior2_2_symmetric);
}

TEST_CASE("wavedec: multilevel coefficients vs pywt", "[v11w-b][wavelet][dwt]")
{
    crd::memory::TlsfAllocator alloc(1U << 20);
    const cont::ConstSpan<f64> xs(ref_input, sizeof(ref_input) / sizeof(ref_input[0]));
    const auto w = wv::wavelet_by_name("db4");
    REQUIRE(w.has_value());
    const auto coeffs = wv::wavedec<f64>(&alloc, xs, *w, Mode::Symmetric, 3);
    REQUIRE(static_cast<int>(coeffs.size()) == ref_wd_db4_l3_ncoef);
    check_arr(ref_wd_db4_l3_c0, coeffs[0], 1e-10);
    check_arr(ref_wd_db4_l3_c1, coeffs[1], 1e-10);
    check_arr(ref_wd_db4_l3_c2, coeffs[2], 1e-10);
    check_arr(ref_wd_db4_l3_c3, coeffs[3], 1e-10);
}

TEST_CASE("waverec: perfect reconstruction (round trip)", "[v11w-b][wavelet][dwt]")
{
    crd::memory::TlsfAllocator alloc(1U << 21);
    // Power-of-two periodization: exact non-redundant round trip.
    {
        const cont::ConstSpan<f64> xp(ref_input64, sizeof(ref_input64) / sizeof(ref_input64[0]));
        const auto w = wv::wavelet_by_name("db4");
        const auto coeffs = wv::wavedec<f64>(&alloc, xp, *w, Mode::Periodization, 4);
        REQUIRE(static_cast<int>(coeffs.size()) == ref_wd_db4_per_ncoef);
        check_arr(ref_wd_db4_per_c0, coeffs[0], 1e-10);
        const auto rec = wv::waverec<f64>(&alloc, coeffs, *w, Mode::Periodization);
        REQUIRE(rec.size() == xp.size());
        for (usize i = 0; i < xp.size(); ++i)
        {
            INFO("i=" << i);
            CHECK_THAT(rec[i], WithinAbs(xp[i], 1e-11));
        }
    }
    // Symmetric round trip for several wavelets (interior recovery).
    const cont::ConstSpan<f64> xs(ref_input, sizeof(ref_input) / sizeof(ref_input[0]));
    for (const char* name : {"haar", "db2", "db4", "sym4", "coif1", "bior2.2", "rbio2.2"})
    {
        const auto w = wv::wavelet_by_name(name);
        const auto coeffs = wv::wavedec<f64>(&alloc, xs, *w, Mode::Symmetric, 2);
        const auto rec = wv::waverec<f64>(&alloc, coeffs, *w, Mode::Symmetric);
        REQUIRE(rec.size() >= xs.size());
        INFO("wavelet " << name);
        for (usize i = 0; i < xs.size(); ++i)
        {
            INFO("i=" << i);
            CHECK_THAT(rec[i], WithinAbs(xs[i], 1e-9));
        }
    }
}

TEST_CASE("dwt: run-twice bit-identical (determinism moat)", "[v11w-b][wavelet][dwt][moat]")
{
    crd::memory::TlsfAllocator alloc(1U << 21);
    const cont::ConstSpan<f64> xs(ref_input, sizeof(ref_input) / sizeof(ref_input[0]));
    const auto w = wv::wavelet_by_name("db4");
    for (Mode m : {Mode::Symmetric, Mode::Periodization, Mode::Zero})
    {
        const auto c1 = wv::wavedec<f64>(&alloc, xs, *w, m, 3);
        const auto c2 = wv::wavedec<f64>(&alloc, xs, *w, m, 3);
        REQUIRE(c1.size() == c2.size());
        for (usize lvl = 0; lvl < c1.size(); ++lvl)
        {
            REQUIRE(c1[lvl].size() == c2[lvl].size());
            CHECK(std::memcmp(c1[lvl].data(), c2[lvl].data(), c1[lvl].size() * sizeof(f64)) == 0);
        }
    }
}
