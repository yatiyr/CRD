// crd-hesap-dsp v11-i — filter APPLICATION gates. sosfilt is pure multiply-add ⇒ the honest gate is BIT-EXACT
// vs scipy.signal.sosfilt + the {1..16}-style STREAMING determinism moat: feeding the kernel in ARBITRARY block
// sizes produces output IDENTICAL (bit-for-bit) to one batch call. References = the SAME SOS scipy used.

#include <crd/hesap/dsp/filter.hpp>
#include <crd/hesap/dsp/filtering.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include "filtering_refs.inc"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <cstring>

namespace dsp = crd::hesap::dsp;
namespace cont = crd::containers;
using crd::f64;
using crd::usize;
using Catch::Matchers::WithinAbs;

namespace
{
// rebuild scipy's exact SOS (row-major [b0,b1,b2,a0,a1,a2]) into a Cerid cascade.
dsp::SecondOrderSections<f64> load_sos(crd::memory::IAllocator* a)
{
    dsp::SecondOrderSections<f64> sos(a);
    for (int s = 0; s < ref_filt_nsec; ++s)
    {
        dsp::Biquad<f64> bq;
        bq.b0 = ref_filt_sos[6 * s + 0];
        bq.b1 = ref_filt_sos[6 * s + 1];
        bq.b2 = ref_filt_sos[6 * s + 2];
        bq.a1 = ref_filt_sos[6 * s + 4]; // a0 (index 3) == 1
        bq.a2 = ref_filt_sos[6 * s + 5];
        sos.sections.push_back(bq);
    }
    return sos;
}
} // namespace

TEST_CASE("dsp filtering: sosfilt is BIT-EXACT vs scipy.signal.sosfilt", "[v11-i][dsp][filtering]")
{
    crd::memory::TlsfAllocator alloc(1U << 20);
    const auto sos = load_sos(&alloc);
    const usize n = sizeof(ref_filt_x) / sizeof(double);
    const auto y = dsp::sosfilt<f64>(&alloc, sos, cont::ConstSpan<f64>(ref_filt_x, n));
    REQUIRE(y.size() == n);
    for (usize i = 0; i < n; ++i)
    {
        INFO("y[" << i << "]");
        CHECK(y[i] == ref_filt_y[i]); // BIT-EXACT (pure multiply-add, same DF2T recurrence, -ffp-contract=off)
    }
}

TEST_CASE("dsp filtering: STREAMING moat — arbitrary block sizes == one batch call (bit-identical)",
          "[v11-i][dsp][filtering][moat]")
{
    crd::memory::TlsfAllocator alloc(1U << 20);
    const auto sos = load_sos(&alloc);
    const usize n = sizeof(ref_filt_x) / sizeof(double);

    const auto batch = dsp::sosfilt<f64>(&alloc, sos, cont::ConstSpan<f64>(ref_filt_x, n));

    // stream the SAME input through the stateful kernel in irregular blocks (7, 13, 1, 20, ...).
    cont::Array<f64> streamed(&alloc);
    streamed.resize(n);
    cont::Array<dsp::BiquadState<f64>> state(&alloc);
    state.resize(sos.sections.size());
    for (usize s = 0; s < state.size(); ++s)
    {
        state[s] = dsp::BiquadState<f64>{};
    }
    const usize blocks[] = {7, 13, 1, 20, 5, 18};
    usize off = 0;
    usize bi = 0;
    while (off < n)
    {
        usize blk = blocks[bi % 6];
        if (off + blk > n)
        {
            blk = n - off;
        }
        dsp::sosfilt_stream<f64>(sos, cont::ConstSpan<f64>(ref_filt_x + off, blk),
                                 cont::Span<f64>(streamed.data() + off, blk),
                                 cont::Span<dsp::BiquadState<f64>>(state.data(), state.size()));
        off += blk;
        ++bi;
    }
    // bit-identical to the batch call (the streaming determinism moat).
    CHECK(std::memcmp(streamed.data(), batch.data(), n * sizeof(f64)) == 0);
}

TEST_CASE("dsp filtering: sosfiltfilt is zero-phase (symmetric impulse response)", "[v11-i][dsp][filtering]")
{
    crd::memory::TlsfAllocator alloc(1U << 20);
    const auto sos = load_sos(&alloc);
    // impulse centred in a LONG buffer (the sharp elliptic response must fully settle before the edges, since
    // the no-pad filtfilt has edge transients) ⇒ the zero-phase output is symmetric about the impulse.
    const usize n = 2001;
    const usize ctr = 1000;
    cont::Array<f64> imp(&alloc);
    imp.resize(n);
    for (usize i = 0; i < n; ++i)
    {
        imp[i] = (i == ctr) ? 1.0 : 0.0;
    }
    const auto y = dsp::sosfiltfilt_nopad<f64>(&alloc, sos, cont::ConstSpan<f64>(imp.data(), n));
    for (usize k = 1; k < 300; ++k)
    {
        INFO("symmetry offset " << k);
        CHECK_THAT(y[ctr + k], WithinAbs(y[ctr - k], 1e-11)); // zero phase ⇒ symmetric (interior, settled)
    }
}

TEST_CASE("dsp filtering: lfilter BIT-EXACT vs scipy + filtfilt matches scipy (zero-phase)", "[v11-i][dsp][filtering]")
{
    crd::memory::TlsfAllocator alloc(1U << 21);
    const usize nx = sizeof(ref_lf_x)/sizeof(double), nb = sizeof(ref_lf_b)/sizeof(double), na = sizeof(ref_lf_a)/sizeof(double);
    const auto yl = dsp::lfilter<f64>(&alloc, cont::ConstSpan<f64>(ref_lf_b,nb), cont::ConstSpan<f64>(ref_lf_a,na), cont::ConstSpan<f64>(ref_lf_x,nx));
    REQUIRE(yl.size() == nx);
    for (usize i = 0; i < nx; ++i) { INFO("lf[" << i << "]"); CHECK(yl[i] == ref_lfilter[i]); } // BIT-EXACT (DF2T mul-add)
    const auto yff = dsp::filtfilt<f64>(&alloc, cont::ConstSpan<f64>(ref_lf_b,nb), cont::ConstSpan<f64>(ref_lf_a,na), cont::ConstSpan<f64>(ref_lf_x,nx));
    REQUIRE(yff.size() == nx);
    for (usize i = 0; i < nx; ++i) { INFO("ff[" << i << "]"); CHECK_THAT(yff[i], WithinAbs(ref_filtfilt[i], 1e-10)); } // vs scipy
}
