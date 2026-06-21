#pragma once

// ---------------------------------------------------------------------------
// crd-hesap-comms v11c-c — symbol timing recovery.
//
//   gardner_ted               non-data-aided timing error detector (2 sps).
//   mueller_muller_ted        decision-directed TED (1 sps).
//   SymbolSync<T>             Gardner-based symbol synchronizer: a cubic
//                             interpolator + a 2nd-order PI loop tracks the
//                             fractional sampling phase and outputs symbol-rate
//                             samples from an sps=2 oversampled input.
//
// Gate (ADR-0093): the Gardner TED S-curve (error tracks the timing offset with
// the right sign through zero) + the closed loop LOCKS and recovers the symbols
// from a fractionally-delayed signal (zero symbol errors in steady state) + the
// run-twice determinism moat. Lower-layer raw Complex<T>, alloc-free streaming.
// ---------------------------------------------------------------------------

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/comms/loop.hpp>
#include <crd/hesap/complex.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::hesap::comms
{

// Gardner TED (2 sps): e = Re{(cur_sym − prev_sym)·conj(mid)}. Non-data-aided; zero at the correct timing.
template <typename T>
[[nodiscard]] T gardner_ted(Complex<T> prev_sym, Complex<T> mid, Complex<T> cur_sym) noexcept
{
    return (cur_sym.re - prev_sym.re) * mid.re + (cur_sym.im - prev_sym.im) * mid.im;
}

// Mueller & Müller TED (1 sps, decision-directed): e = Re{conj(prev_dec)·cur − conj(cur_dec)·prev}.
template <typename T>
[[nodiscard]] T mueller_muller_ted(Complex<T> prev, Complex<T> cur, Complex<T> prev_dec, Complex<T> cur_dec) noexcept
{
    const T t1 = prev_dec.re * cur.re + prev_dec.im * cur.im; // Re{conj(prev_dec)·cur}
    const T t2 = cur_dec.re * prev.re + cur_dec.im * prev.im; // Re{conj(cur_dec)·prev}
    return t1 - t2;
}

// Gardner symbol synchronizer (sps = 2). A moving symbol-strobe index t walks the oversampled input; per symbol it
// cubic-interpolates the on-time sample at t and the half-symbol sample at t−1, runs the Gardner TED, and a 2nd-order
// PI loop corrects the per-symbol step (nominally sps). Outputs one symbol per ~sps input samples.
template <typename T> class SymbolSync
{
public:
    SymbolSync(T loop_bw, T zeta, crd::usize sps = 2) noexcept : m_lf(loop_bw, zeta), m_sps(sps) {}

    void reset() noexcept { m_lf.reset(); }

    // Process a block of oversampled samples; append recovered symbols to `out`.
    void process(crd::containers::ConstSpan<Complex<T>> in, crd::containers::Array<Complex<T>>& out)
    {
        const crd::usize n = in.size();
        const T sps = static_cast<T>(m_sps);
        T t = sps + T(2); // start with enough history for the cubic window of the mid sample (t-1)
        Complex<T> prev_on{T(0), T(0)};
        bool have_prev = false;
        while (true)
        {
            const T tmid = t - sps / T(2);
            const crd::isize i_on = static_cast<crd::isize>(t);
            const crd::isize i_mid = static_cast<crd::isize>(tmid);
            if (i_on + 2 >= static_cast<crd::isize>(n))
            {
                break;
            }
            const Complex<T> y_on = interp(in.data(), n, t);
            const Complex<T> y_mid = interp(in.data(), n, tmid);
            (void)i_on;
            (void)i_mid;
            if (have_prev)
            {
                const T e = gardner_ted<T>(prev_on, y_mid, y_on);
                const T v = m_lf.advance(e);
                t += sps - v; // PI-corrected step (Gardner e>0 when sampling late ⇒ shorten the step toward earlier)
            }
            else
            {
                t += sps;
            }
            out.push_back(y_on);
            prev_on = y_on;
            have_prev = true;
        }
    }

private:
    // cubic interpolate the input at fractional position `pos` (needs in[floor-1 .. floor+2]).
    [[nodiscard]] static Complex<T> interp(const Complex<T>* in, crd::usize n, T pos) noexcept
    {
        crd::isize i0 = static_cast<crd::isize>(pos);
        T mu = pos - static_cast<T>(i0);
        Complex<T> win[4];
        for (int j = 0; j < 4; ++j)
        {
            const crd::isize idx = i0 - 1 + j;
            win[j] = (idx >= 0 && idx < static_cast<crd::isize>(n)) ? in[static_cast<crd::usize>(idx)]
                                                                    : Complex<T>{T(0), T(0)};
        }
        return cubic_interp<T>(win, mu);
    }

    LoopFilter2<T> m_lf;
    crd::usize m_sps;
};

} // namespace crd::hesap::comms
