#pragma once

// ---------------------------------------------------------------------------
// crd-hesap-dsp v11-a — frequency response: freqz / sosfreqz / group_delay.
// THE most-used analysis functions in the toolbox; every design/analysis slice
// needs them. Gate = vs scipy.signal.freqz / sosfreqz / group_delay to N digits.
//
//   H(e^{jw}) = B(e^{jw}) / A(e^{jw}),  B/A the filter-convention polynomials.
//   group delay  tau(w) = -d/dw arg H(e^{jw}).
// ---------------------------------------------------------------------------

#include <crd/containers/array.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/complex.hpp>
#include <crd/hesap/dsp/filter.hpp>
#include <crd/hesap/dsp/polynomial.hpp>
#include <crd/memory/allocator.hpp>

#include <cmath>
#include <numbers>

namespace crd::hesap::dsp
{

// Evaluate H over `worN` frequencies linearly spaced on [0, pi) (scipy default,
// whole=false). Writes the angular grid `w` (rad/sample) and the complex `h`.
template <typename T>
void freqz(const TransferFunction<T>& tf, crd::usize worN, crd::containers::Array<T>& w,
           crd::containers::Array<Complex<T>>& h, bool whole = false)
{
    w.resize(worN);
    h.resize(worN);
    const T span = whole ? static_cast<T>(2.0 * std::numbers::pi_v<double>)
                         : static_cast<T>(std::numbers::pi_v<double>);
    for (crd::usize i = 0; i < worN; ++i)
    {
        const T wi = span * static_cast<T>(i) / static_cast<T>(worN);
        w[i] = wi;
        const Complex<T> zinv{std::cos(wi), -std::sin(wi)}; // e^{-jw}
        const Complex<T> num = poly_eval_negpow<T>(crd::containers::ConstSpan<T>(tf.b.data(), tf.b.size()), zinv);
        const Complex<T> den = tf.a.empty()
                                   ? Complex<T>{T(1), T(0)}
                                   : poly_eval_negpow<T>(crd::containers::ConstSpan<T>(tf.a.data(), tf.a.size()), zinv);
        // num / den
        const T dd = den.re * den.re + den.im * den.im;
        h[i] = Complex<T>{(num.re * den.re + num.im * den.im) / dd, (num.im * den.re - num.re * den.im) / dd};
    }
}

// freqz directly from the FACTORED (zpk) form: H = k * prod(1 - z_i z^{-1}) / prod(1 - p_i z^{-1}).
// Well-conditioned at ANY order (no polynomial expansion) — this is the reference for high-order filters,
// where tf coefficients (and roots-of-tf) are Wilkinson-ill-conditioned. Design path never forms tf.
template <typename T>
void zpk_freqz(const Zpk<T>& zpk, crd::usize worN, crd::containers::Array<T>& w,
               crd::containers::Array<Complex<T>>& h, bool whole = false)
{
    w.resize(worN);
    h.resize(worN);
    const T span = whole ? static_cast<T>(2.0 * std::numbers::pi_v<double>)
                         : static_cast<T>(std::numbers::pi_v<double>);
    for (crd::usize i = 0; i < worN; ++i)
    {
        const T wi = span * static_cast<T>(i) / static_cast<T>(worN);
        w[i] = wi;
        const Complex<T> zinv{std::cos(wi), -std::sin(wi)}; // e^{-jw}
        Complex<T> num{zpk.k, T(0)};
        for (crd::usize j = 0; j < zpk.z.size(); ++j)
        {
            // (1 - z_j * zinv)
            const T fr = T(1) - (zpk.z[j].re * zinv.re - zpk.z[j].im * zinv.im);
            const T fi = -(zpk.z[j].re * zinv.im + zpk.z[j].im * zinv.re);
            num = Complex<T>{num.re * fr - num.im * fi, num.re * fi + num.im * fr};
        }
        Complex<T> den{T(1), T(0)};
        for (crd::usize j = 0; j < zpk.p.size(); ++j)
        {
            const T fr = T(1) - (zpk.p[j].re * zinv.re - zpk.p[j].im * zinv.im);
            const T fi = -(zpk.p[j].re * zinv.im + zpk.p[j].im * zinv.re);
            den = Complex<T>{den.re * fr - den.im * fi, den.re * fi + den.im * fr};
        }
        const T dd = den.re * den.re + den.im * den.im;
        h[i] = Complex<T>{(num.re * den.re + num.im * den.im) / dd, (num.im * den.re - num.re * den.im) / dd};
    }
}

// freqz over a SOS cascade: product of the per-section responses.
template <typename T>
void sosfreqz(const SecondOrderSections<T>& sos, crd::usize worN, crd::containers::Array<T>& w,
              crd::containers::Array<Complex<T>>& h, bool whole = false)
{
    w.resize(worN);
    h.resize(worN);
    const T span = whole ? static_cast<T>(2.0 * std::numbers::pi_v<double>)
                         : static_cast<T>(std::numbers::pi_v<double>);
    for (crd::usize i = 0; i < worN; ++i)
    {
        const T wi = span * static_cast<T>(i) / static_cast<T>(worN);
        w[i] = wi;
        const Complex<T> zi{std::cos(wi), -std::sin(wi)};
        const Complex<T> zi2{std::cos(T(2) * wi), -std::sin(T(2) * wi)};
        Complex<T> acc{T(1), T(0)};
        for (crd::usize s = 0; s < sos.sections.size(); ++s)
        {
            const Biquad<T>& bq = sos.sections[s];
            const Complex<T> num{bq.b0 + bq.b1 * zi.re + bq.b2 * zi2.re, bq.b1 * zi.im + bq.b2 * zi2.im};
            const Complex<T> den{T(1) + bq.a1 * zi.re + bq.a2 * zi2.re, bq.a1 * zi.im + bq.a2 * zi2.im};
            const T dd = den.re * den.re + den.im * den.im;
            const Complex<T> hs{(num.re * den.re + num.im * den.im) / dd, (num.im * den.re - num.re * den.im) / dd};
            acc = Complex<T>{acc.re * hs.re - acc.im * hs.im, acc.re * hs.im + acc.im * hs.re};
        }
        h[i] = acc;
    }
}

// Group delay tau(w) = -d/dw arg H = Re{ (B'/B) - (A'/A) } in the zi=e^{-jw}
// variable, chain-ruled by dzi/dw = -j*zi. Using d arg H/dw = Im{ H'/H }, and
// with z = e^{jw}: tau(w) = Re{ z * (B'(z)/B(z) - A'(z)/A(z)) } evaluated via the
// derivative w.r.t. z. We compute it stably through the zi-Horner derivative.
template <typename T>
void group_delay(const TransferFunction<T>& tf, crd::usize worN, crd::containers::Array<T>& w,
                 crd::containers::Array<T>& gd)
{
    w.resize(worN);
    gd.resize(worN);
    // tau(w) = -d/dw arg(H). With P(zi)=Σ c_k zi^k, dP/dw = (dP/dzi)(dzi/dw),
    // dzi/dw = -j zi. d arg P/dw = Im{ (dP/dw)/P } = Im{ -j zi P'(zi)/P(zi) }.
    // tau = -(d arg B/dw - d arg A/dw).
    auto darg = [&](crd::containers::ConstSpan<T> c, Complex<T> zi) -> T
    {
        if (c.size() <= 1)
        {
            return T(0);
        }
        Complex<T> p, dp;
        poly_eval_with_deriv<T>(c, zi, p, dp);
        // ratio = zi * dp / p
        const Complex<T> num{zi.re * dp.re - zi.im * dp.im, zi.re * dp.im + zi.im * dp.re};
        const T dd = p.re * p.re + p.im * p.im;
        const Complex<T> ratio{(num.re * p.re + num.im * p.im) / dd, (num.im * p.re - num.re * p.im) / dd};
        // Im{ -j * ratio } = -Re{ratio}
        return -ratio.re;
    };
    const T pi = static_cast<T>(std::numbers::pi_v<double>);
    for (crd::usize i = 0; i < worN; ++i)
    {
        const T wi = pi * static_cast<T>(i) / static_cast<T>(worN);
        w[i] = wi;
        const Complex<T> zi{std::cos(wi), -std::sin(wi)};
        const T db = darg(crd::containers::ConstSpan<T>(tf.b.data(), tf.b.size()), zi);
        const T da = tf.a.empty() ? T(0) : darg(crd::containers::ConstSpan<T>(tf.a.data(), tf.a.size()), zi);
        gd[i] = -(db - da);
    }
}

} // namespace crd::hesap::dsp
