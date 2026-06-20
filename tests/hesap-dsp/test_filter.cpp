// crd-hesap-dsp v11-a — substrate gates: polynomial roots, filter representations + conversions,
// freqz / sosfreqz / group_delay. Gated on ANALYTIC references (exact, closed-form) + cross-representation
// consistency (tf == zpk == sos give the SAME H(z)) + the order-12 design-path-lock conditioning gate. f64.
// (Design slices v11-c.. add the scipy/MATLAB spec-compliance harness; the substrate needs no external oracle.)

#include <crd/hesap/dsp/dsp.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <numbers>

namespace dsp = crd::hesap::dsp;
namespace cont = crd::containers;
using crd::f64;
using crd::usize;
using crd::hesap::Complex;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace
{
dsp::TransferFunction<f64> make_tf(crd::memory::IAllocator* a, std::initializer_list<f64> b,
                                   std::initializer_list<f64> aa)
{
    dsp::TransferFunction<f64> tf(a);
    for (f64 x : b)
    {
        tf.b.push_back(x);
    }
    for (f64 x : aa)
    {
        tf.a.push_back(x);
    }
    return tf;
}
} // namespace

TEST_CASE("dsp: polynomial roots of 1 - z^-2 are +-1", "[v11-a][dsp][poly]")
{
    crd::memory::TlsfAllocator alloc(1U << 20);
    const f64 c[] = {1.0, 0.0, -1.0}; // x^2 - 1 (descending) ⇒ roots ±1
    const auto r = dsp::roots<f64>(&alloc, cont::ConstSpan<f64>(c, 3));
    REQUIRE(r.size() == 2);
    // roots come back in some order; check the SET {+1, -1}.
    f64 sum = r[0].re + r[1].re;
    f64 prod = r[0].re * r[1].re - r[0].im * r[1].im;
    CHECK_THAT(sum, WithinAbs(0.0, 1e-12));   // 1 + (-1)
    CHECK_THAT(prod, WithinAbs(-1.0, 1e-12)); // 1 * (-1)
    CHECK_THAT(r[0].im, WithinAbs(0.0, 1e-12));
    CHECK_THAT(r[1].im, WithinAbs(0.0, 1e-12));
}

TEST_CASE("dsp: freqz of FIR [0.5,0.5] matches |cos(w/2)|", "[v11-a][dsp][freqz]")
{
    crd::memory::TlsfAllocator alloc(1U << 20);
    const auto tf = make_tf(&alloc, {0.5, 0.5}, {1.0});
    cont::Array<f64> w(&alloc);
    cont::Array<Complex<f64>> h(&alloc);
    dsp::freqz<f64>(tf, 64, w, h);
    REQUIRE(h.size() == 64);
    CHECK_THAT(h[0].re, WithinAbs(1.0, 1e-12)); // H(0) = 1
    CHECK_THAT(h[0].im, WithinAbs(0.0, 1e-12));
    for (usize i = 0; i < 64; ++i)
    {
        const f64 mag = std::hypot(h[i].re, h[i].im);
        CHECK_THAT(mag, WithinAbs(std::abs(std::cos(w[i] / 2.0)), 1e-12));
    }
}

TEST_CASE("dsp: freqz of one-pole IIR 1/(1-0.5 z^-1) at DC = 2", "[v11-a][dsp][freqz]")
{
    crd::memory::TlsfAllocator alloc(1U << 20);
    const auto tf = make_tf(&alloc, {1.0}, {1.0, -0.5});
    cont::Array<f64> w(&alloc);
    cont::Array<Complex<f64>> h(&alloc);
    dsp::freqz<f64>(tf, 32, w, h);
    CHECK_THAT(h[0].re, WithinRel(2.0, 1e-12)); // 1/(1-0.5)
    // analytic: H(w) = 1/(1 - 0.5 e^{-jw}); check a mid bin.
    for (usize i = 0; i < 32; ++i)
    {
        // den = (1 - 0.5cos w) + j(0.5 sin w); H = 1/den = conj(den)/|den|^2.
        const f64 dr = 1.0 - 0.5 * std::cos(w[i]);
        const f64 di = 0.5 * std::sin(w[i]);
        const f64 dd = dr * dr + di * di;
        CHECK_THAT(h[i].re, WithinRel(dr / dd, 1e-12));
        CHECK_THAT(h[i].im, WithinAbs(-di / dd, 1e-12)); // H.im = -den.im/|den|^2
    }
}

TEST_CASE("dsp: group_delay of a pure 2-sample delay is constant 2", "[v11-a][dsp][groupdelay]")
{
    crd::memory::TlsfAllocator alloc(1U << 20);
    const auto tf = make_tf(&alloc, {0.0, 0.0, 1.0}, {1.0}); // z^-2
    cont::Array<f64> w(&alloc);
    cont::Array<f64> gd(&alloc);
    dsp::group_delay<f64>(tf, 40, w, gd);
    for (usize i = 0; i < 40; ++i)
    {
        CHECK_THAT(gd[i], WithinAbs(2.0, 1e-9));
    }
}

TEST_CASE("dsp: zpk <-> tf round-trip reconstructs H(z)", "[v11-a][dsp][convert]")
{
    crd::memory::TlsfAllocator alloc(1U << 21);
    // a 2nd-order IIR: poles 0.6 e^{±j 0.7}, zeros at -1 (double), gain 0.3.
    dsp::Zpk<f64> zpk(&alloc);
    zpk.z.push_back(Complex<f64>{-1.0, 0.0});
    zpk.z.push_back(Complex<f64>{-1.0, 0.0});
    zpk.p.push_back(Complex<f64>{0.6 * std::cos(0.7), 0.6 * std::sin(0.7)});
    zpk.p.push_back(Complex<f64>{0.6 * std::cos(0.7), -0.6 * std::sin(0.7)});
    zpk.k = 0.3;

    const auto tf = dsp::zpk_to_tf<f64>(&alloc, zpk);
    // a-coeffs of a conj pole pair: [1, -2*0.6cos0.7, 0.36]
    REQUIRE(tf.a.size() == 3);
    CHECK_THAT(tf.a[0], WithinRel(1.0, 1e-12));
    CHECK_THAT(tf.a[1], WithinRel(-2.0 * 0.6 * std::cos(0.7), 1e-12));
    CHECK_THAT(tf.a[2], WithinRel(0.36, 1e-12));
    // b-coeffs: 0.3 * (1 + z^-1)^2 = 0.3*[1,2,1]
    CHECK_THAT(tf.b[0], WithinRel(0.3, 1e-12));
    CHECK_THAT(tf.b[1], WithinRel(0.6, 1e-12));
    CHECK_THAT(tf.b[2], WithinRel(0.3, 1e-12));

    // tf -> zpk -> tf reconstructs.
    const auto zpk2 = dsp::tf_to_zpk<f64>(&alloc, tf);
    const auto tf2 = dsp::zpk_to_tf<f64>(&alloc, zpk2);
    for (usize i = 0; i < tf.b.size(); ++i)
    {
        CHECK_THAT(tf2.b[i], WithinAbs(tf.b[i], 1e-10));
    }
    for (usize i = 0; i < tf.a.size(); ++i)
    {
        CHECK_THAT(tf2.a[i], WithinAbs(tf.a[i], 1e-10));
    }
}

TEST_CASE("dsp: zpk -> sos -> tf reconstructs H(z) (the SOS pairing gate)", "[v11-a][dsp][sos]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    // a 4th-order filter: two conjugate pole pairs + two conjugate zero pairs.
    dsp::Zpk<f64> zpk(&alloc);
    zpk.z.push_back(Complex<f64>{std::cos(2.0), std::sin(2.0)});
    zpk.z.push_back(Complex<f64>{std::cos(2.0), -std::sin(2.0)});
    zpk.z.push_back(Complex<f64>{std::cos(2.6), std::sin(2.6)});
    zpk.z.push_back(Complex<f64>{std::cos(2.6), -std::sin(2.6)});
    zpk.p.push_back(Complex<f64>{0.7 * std::cos(0.5), 0.7 * std::sin(0.5)});
    zpk.p.push_back(Complex<f64>{0.7 * std::cos(0.5), -0.7 * std::sin(0.5)});
    zpk.p.push_back(Complex<f64>{0.85 * std::cos(1.1), 0.85 * std::sin(1.1)});
    zpk.p.push_back(Complex<f64>{0.85 * std::cos(1.1), -0.85 * std::sin(1.1)});
    zpk.k = 0.5;

    const auto sos = dsp::zpk_to_sos<f64>(&alloc, zpk);
    REQUIRE(sos.sections.size() == 2); // 4 poles / 4 zeros ⇒ 2 biquads

    // The honest gate: the SOS cascade response == the zpk response at every w.
    const auto tf_ref = dsp::zpk_to_tf<f64>(&alloc, zpk);
    cont::Array<f64> w1(&alloc), w2(&alloc);
    cont::Array<Complex<f64>> h_tf(&alloc), h_sos(&alloc);
    dsp::freqz<f64>(tf_ref, 128, w1, h_tf);
    dsp::sosfreqz<f64>(sos, 128, w2, h_sos);
    for (usize i = 0; i < 128; ++i)
    {
        CHECK_THAT(h_sos[i].re, WithinAbs(h_tf[i].re, 1e-9));
        CHECK_THAT(h_sos[i].im, WithinAbs(h_tf[i].im, 1e-9));
    }

    // and sos -> tf multiplies back to the same polynomials.
    const auto tf_from_sos = dsp::sos_to_tf<f64>(&alloc, sos);
    for (usize i = 0; i < tf_ref.b.size(); ++i)
    {
        CHECK_THAT(tf_from_sos.b[i], WithinAbs(tf_ref.b[i], 1e-9));
        CHECK_THAT(tf_from_sos.a[i], WithinAbs(tf_ref.a[i], 1e-9));
    }
}

// v11-a DATA-FLOW LOCK gate: at order 12 the tf path is Wilkinson-ill-conditioned (roots-of-tf garbage), so
// filters are designed in zpk and converted zpk->sos DIRECTLY. This proves (a) zpk->sos->sosfreqz reconstructs
// the well-conditioned zpk_freqz reference at order 12, and (b) the tf path visibly DIVERGES — the lock's reason.
TEST_CASE("dsp: order-12 design path lock (zpk->sos accurate; tf path diverges)", "[v11-a][dsp][sos][conditioning]")
{
    crd::memory::TlsfAllocator alloc(1U << 24);
    // 12 poles inside the unit circle (6 conjugate pairs, radii 0.80..0.97 near the circle) + 12 zeros on it.
    dsp::Zpk<f64> zpk(&alloc);
    for (int m = 0; m < 6; ++m)
    {
        const f64 r = 0.80 + 0.028 * static_cast<f64>(m);   // 0.80 .. 0.94 (high-Q, near unstable)
        const f64 th = 0.25 + 0.45 * static_cast<f64>(m);
        zpk.p.push_back(Complex<f64>{r * std::cos(th), r * std::sin(th)});
        zpk.p.push_back(Complex<f64>{r * std::cos(th), -r * std::sin(th)});
        const f64 tz = 0.20 + 0.46 * static_cast<f64>(m);
        zpk.z.push_back(Complex<f64>{std::cos(tz), std::sin(tz)});       // on the unit circle
        zpk.z.push_back(Complex<f64>{std::cos(tz), -std::sin(tz)});
    }
    zpk.k = 0.123;

    const auto sos = dsp::zpk_to_sos<f64>(&alloc, zpk);
    REQUIRE(sos.sections.size() == 6);

    cont::Array<f64> w0(&alloc), w1(&alloc);
    cont::Array<Complex<f64>> h_ref(&alloc), h_sos(&alloc);
    dsp::zpk_freqz<f64>(zpk, 256, w0, h_ref);     // well-conditioned factored reference
    dsp::sosfreqz<f64>(sos, 256, w1, h_sos);
    // SOS cascade reconstructs the reference to ~1e-9 even at order 12.
    f64 sos_err = 0.0;
    for (usize i = 0; i < 256; ++i)
    {
        sos_err = std::max(sos_err, std::hypot(h_sos[i].re - h_ref[i].re, h_sos[i].im - h_ref[i].im));
    }
    INFO("zpk->sos max |H| error at order 12 = " << sos_err);
    CHECK(sos_err < 1e-9);

    // The tf path: form tf, then evaluate freqz. The roots-of-tf in tf_to_zpk would be garbage, but even the
    // FORWARD zpk->tf->freqz accumulates error from the order-12 polynomial coefficients. Document the gap.
    const auto tf = dsp::zpk_to_tf<f64>(&alloc, zpk);
    cont::Array<f64> w2(&alloc);
    cont::Array<Complex<f64>> h_tf(&alloc);
    dsp::freqz<f64>(tf, 256, w2, h_tf);
    f64 tf_err = 0.0;
    for (usize i = 0; i < 256; ++i)
    {
        tf_err = std::max(tf_err, std::hypot(h_tf[i].re - h_ref[i].re, h_tf[i].im - h_ref[i].im));
    }
    INFO("zpk->tf->freqz max |H| error at order 12 = " << tf_err);
    // the SOS path is dramatically better-conditioned than the tf path.
    CHECK(sos_err < tf_err);
}

// group_delay on a NON-constant case (the pure-delay test is the trivial linear-phase case). One-pole
// 1/(1 - a z^-1) has the closed-form group delay tau(w) = (a^2 - a cos w) / (1 - 2a cos w + a^2).
TEST_CASE("dsp: group_delay of a one-pole matches the closed form (varying tau)", "[v11-a][dsp][groupdelay]")
{
    crd::memory::TlsfAllocator alloc(1U << 20);
    const f64 a = 0.7;
    dsp::TransferFunction<f64> tf(&alloc);
    tf.b.push_back(1.0);
    tf.a.push_back(1.0);
    tf.a.push_back(-a);
    cont::Array<f64> w(&alloc);
    cont::Array<f64> gd(&alloc);
    dsp::group_delay<f64>(tf, 50, w, gd);
    bool varies = false;
    for (usize i = 0; i < 50; ++i)
    {
        // tau(w) = (a cos w - a^2) / (1 - 2a cos w + a^2); at w=0 => a/(1-a) = +2.33 (causal, positive).
        const f64 expect = (a * std::cos(w[i]) - a * a) / (1.0 - 2.0 * a * std::cos(w[i]) + a * a);
        CHECK_THAT(gd[i], WithinAbs(expect, 1e-9));
        if (std::abs(gd[i] - gd[0]) > 1e-3)
        {
            varies = true;
        }
    }
    CHECK(varies); // confirms we tested a NON-constant group delay
}

// v11-a: state-space round-trip (tf -> ss controllable-canonical -> ss2tf Faddeev-LeVerrier reconstructs).
TEST_CASE("dsp: tf <-> ss round-trip (controllable canonical + Faddeev-LeVerrier)", "[v11-a][dsp][ss]")
{
    crd::memory::TlsfAllocator alloc(1U << 21);
    // a 3rd-order IIR with a non-trivial numerator.
    const auto tf = make_tf(&alloc, {0.2, 0.1, -0.05, 0.3}, {1.0, -0.5, 0.3, -0.1});
    const auto ss = dsp::tf_to_ss<f64>(&alloc, tf);
    REQUIRE(ss.n == 3);
    const auto tf2 = dsp::ss_to_tf<f64>(&alloc, ss);
    REQUIRE(tf2.a.size() == tf.a.size());
    REQUIRE(tf2.b.size() == tf.b.size());
    for (usize i = 0; i < tf.a.size(); ++i)
    {
        CHECK_THAT(tf2.a[i], WithinAbs(tf.a[i], 1e-12));
        CHECK_THAT(tf2.b[i], WithinAbs(tf.b[i], 1e-12));
    }
    // and the state-space response equals the tf response.
    cont::Array<f64> w1(&alloc), w2(&alloc);
    cont::Array<Complex<f64>> h1(&alloc), h2(&alloc);
    dsp::freqz<f64>(tf, 64, w1, h1);
    dsp::freqz<f64>(tf2, 64, w2, h2);
    for (usize i = 0; i < 64; ++i)
    {
        CHECK_THAT(h2[i].re, WithinAbs(h1[i].re, 1e-12));
        CHECK_THAT(h2[i].im, WithinAbs(h1[i].im, 1e-12));
    }
}

// v11-a: lattice round-trip + the stability property (all |k| < 1 <=> minimum phase).
TEST_CASE("dsp: denom <-> lattice round-trip + |k|<1 stability", "[v11-a][dsp][lattice]")
{
    crd::memory::TlsfAllocator alloc(1U << 21);
    // a STABLE all-pole denominator (poles inside the unit circle): from conj pairs r e^{±jth}, r<1.
    dsp::Zpk<f64> zpk(&alloc);
    zpk.p.push_back(Complex<f64>{0.7 * std::cos(0.6), 0.7 * std::sin(0.6)});
    zpk.p.push_back(Complex<f64>{0.7 * std::cos(0.6), -0.7 * std::sin(0.6)});
    zpk.p.push_back(Complex<f64>{0.5, 0.0});
    zpk.k = 1.0;
    const auto tf = dsp::zpk_to_tf<f64>(&alloc, zpk); // a = (1 - ...), b = {k}
    REQUIRE(tf.a.size() == 4);

    dsp::Lattice<f64> lat(&alloc);
    REQUIRE(dsp::denom_to_lattice<f64>(&alloc, cont::ConstSpan<f64>(tf.a.data(), tf.a.size()), lat));
    REQUIRE(lat.k.size() == 3);
    CHECK(dsp::lattice_is_stable<f64>(lat)); // all poles inside ⇒ all |k|<1

    const auto a2 = dsp::lattice_to_denom<f64>(&alloc, lat);
    REQUIRE(a2.size() == tf.a.size());
    for (usize i = 0; i < tf.a.size(); ++i)
    {
        CHECK_THAT(a2[i], WithinAbs(tf.a[i], 1e-12));
    }

    // an UNSTABLE denominator (a pole outside) ⇒ some |k| >= 1.
    dsp::Zpk<f64> uz(&alloc);
    uz.p.push_back(Complex<f64>{1.3, 0.0});
    uz.p.push_back(Complex<f64>{0.4, 0.0});
    uz.k = 1.0;
    const auto utf = dsp::zpk_to_tf<f64>(&alloc, uz);
    dsp::Lattice<f64> ulat(&alloc);
    const bool ok = dsp::denom_to_lattice<f64>(&alloc, cont::ConstSpan<f64>(utf.a.data(), utf.a.size()), ulat);
    REQUIRE(ok); // the recursion completes; instability shows as |k| >= 1
    CHECK_FALSE(dsp::lattice_is_stable<f64>(ulat));
}
