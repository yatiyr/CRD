#pragma once

// ---------------------------------------------------------------------------
// crd-hesap-dsp v11-g — IIR digital design: the unified design path.
//
// v11-e/f gave the analog lowpass prototypes (Butterworth / Chebyshev I+II /
// Bessel / Elliptic) and the lowpass digital chain. v11-g completes the IIR
// design surface, faithful to scipy.signal:
//
//   lp2hp_zpk / lp2bp_zpk / lp2bs_zpk   analog frequency transforms (s-plane)
//   iirfilter                           the unified dispatcher: prototype (by
//                                       kind) -> prewarp -> band transform ->
//                                       bilinear -> digital zpk (NEVER tf — the
//                                       v11-a data-flow rule; caller -> zpk_to_sos)
//   buttord / cheb1ord / cheb2ord /     order estimation: minimum N + natural
//   ellipord                            frequency Wn for a (wp,ws,gpass,gstop) spec
//   iirdesign                           *ord -> iirfilter (one call, design from spec)
//   iirnotch / iirpeak / iircomb        2nd-order notch/peak + harmonic comb (tf —
//                                       inherently low-/sparse-order, well-conditioned)
//
// DESIGN slice ⇒ the honest gate is SPEC-COMPLIANCE (passband ripple <= gpass,
// stopband >= gstop, equiripple/monotone as appropriate) + coeffs/order vs
// scipy: order N is exact (a ceil of a ratio), Wn/coeffs to N digits. NOT a
// bit-match (transcendental prototypes drift cross-libm) and NO perf bench
// (one-time setup, not a streaming hot loop). Lower-layer raw scalars.
// ---------------------------------------------------------------------------

#include <crd/containers/array.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/complex.hpp>
#include <crd/hesap/dsp/ellip.hpp>       // ellipap
#include <crd/hesap/dsp/elliptic_fn.hpp> // ellipk
#include <crd/hesap/dsp/filter.hpp>      // Zpk, TransferFunction
#include <crd/hesap/dsp/iir.hpp>         // buttap/cheb1ap/cheb2ap/besselap, lp2lp_zpk, bilinear_zpk, detail::cmul/cdiv
#include <crd/memory/allocator.hpp>

#include <algorithm>
#include <crd/math/cmath.hpp>
#include <numbers>
#include <utility>

namespace crd::hesap::dsp
{

enum class BandType
{
    Lowpass,
    Highpass,
    Bandpass,
    Bandstop
};

enum class IirKind
{
    Butter,
    Cheby1,
    Cheby2,
    Ellip,
    Bessel
};

// Order-estimation result: minimum order n + the natural frequency Wn (1 element for
// lowpass/highpass, 2 for bandpass/bandstop), Nyquist-fraction units (scipy fs=2 default).
template <typename T> struct IirOrder
{
    crd::usize n = 0;
    crd::containers::Array<T> wn;
    explicit IirOrder(crd::memory::IAllocator* alloc) : wn(alloc) {}
};

// =========================================================================
// Analog frequency transforms (s -> s), zpk in / zpk out. Faithful scipy.
// =========================================================================

namespace detail
{
// Principal-branch complex square root (numpy/scipy convention: Re >= 0).
template <typename T> [[nodiscard]] Complex<T> csqrt_pb(Complex<T> w) noexcept
{
    const T r = crd::math::hypot(w.re, w.im);
    T re = crd::math::sqrt((r + w.re) / T(2));
    T im = crd::math::sqrt((r - w.re) / T(2));
    if (w.im < T(0))
    {
        im = -im;
    }
    return Complex<T>{re, im};
}
} // namespace detail

// lp2hp_zpk: analog lowpass prototype -> highpass with cutoff wo (s -> wo/s).
template <typename T> [[nodiscard]] Zpk<T> lp2hp_zpk(crd::memory::IAllocator* alloc, const Zpk<T>& in, T wo)
{
    Zpk<T> out(alloc);
    const crd::usize degree = in.p.size() - in.z.size();
    Complex<T> negz{T(1), T(0)}, negp{T(1), T(0)}; // prod(-z), prod(-p)
    for (crd::usize i = 0; i < in.z.size(); ++i)
    {
        out.z.push_back(detail::cdiv<T>(Complex<T>{wo, T(0)}, in.z[i]));
        negz = detail::cmul<T>(negz, Complex<T>{-in.z[i].re, -in.z[i].im});
    }
    for (crd::usize i = 0; i < in.p.size(); ++i)
    {
        out.p.push_back(detail::cdiv<T>(Complex<T>{wo, T(0)}, in.p[i]));
        negp = detail::cmul<T>(negp, Complex<T>{-in.p[i].re, -in.p[i].im});
    }
    for (crd::usize i = 0; i < degree; ++i)
    {
        out.z.push_back(Complex<T>{T(0), T(0)}); // poles that moved to origin
    }
    out.k = in.k * detail::cdiv<T>(negz, negp).re;
    return out;
}

// lp2bp_zpk: analog lowpass prototype -> bandpass, centre wo, bandwidth bw.
template <typename T> [[nodiscard]] Zpk<T> lp2bp_zpk(crd::memory::IAllocator* alloc, const Zpk<T>& in, T wo, T bw)
{
    Zpk<T> out(alloc);
    const crd::usize degree = in.p.size() - in.z.size();
    const T wo2 = wo * wo;
    auto xform = [&](const crd::containers::Array<Complex<T>>& src, crd::containers::Array<Complex<T>>& dst)
    {
        for (crd::usize i = 0; i < src.size(); ++i)
        {
            const Complex<T> lp{src[i].re * bw / T(2), src[i].im * bw / T(2)};
            const Complex<T> disc = detail::csqrt_pb<T>(Complex<T>{detail::cmul<T>(lp, lp).re - wo2, detail::cmul<T>(lp, lp).im});
            dst.push_back(Complex<T>{lp.re + disc.re, lp.im + disc.im});
            dst.push_back(Complex<T>{lp.re - disc.re, lp.im - disc.im});
        }
    };
    xform(in.z, out.z);
    xform(in.p, out.p);
    for (crd::usize i = 0; i < degree; ++i)
    {
        out.z.push_back(Complex<T>{T(0), T(0)});
    }
    out.k = in.k * crd::math::pow(bw, static_cast<T>(degree));
    return out;
}

// lp2bs_zpk: analog lowpass prototype -> bandstop, centre wo, bandwidth bw.
template <typename T> [[nodiscard]] Zpk<T> lp2bs_zpk(crd::memory::IAllocator* alloc, const Zpk<T>& in, T wo, T bw)
{
    Zpk<T> out(alloc);
    const crd::usize degree = in.p.size() - in.z.size();
    const T wo2 = wo * wo;
    Complex<T> negz{T(1), T(0)}, negp{T(1), T(0)};
    auto xform = [&](const crd::containers::Array<Complex<T>>& src, crd::containers::Array<Complex<T>>& dst)
    {
        for (crd::usize i = 0; i < src.size(); ++i)
        {
            const Complex<T> hp = detail::cdiv<T>(Complex<T>{bw / T(2), T(0)}, src[i]);
            const Complex<T> disc = detail::csqrt_pb<T>(Complex<T>{detail::cmul<T>(hp, hp).re - wo2, detail::cmul<T>(hp, hp).im});
            dst.push_back(Complex<T>{hp.re + disc.re, hp.im + disc.im});
            dst.push_back(Complex<T>{hp.re - disc.re, hp.im - disc.im});
        }
    };
    xform(in.z, out.z);
    xform(in.p, out.p);
    for (crd::usize i = 0; i < in.z.size(); ++i)
    {
        negz = detail::cmul<T>(negz, Complex<T>{-in.z[i].re, -in.z[i].im});
    }
    for (crd::usize i = 0; i < in.p.size(); ++i)
    {
        negp = detail::cmul<T>(negp, Complex<T>{-in.p[i].re, -in.p[i].im});
    }
    for (crd::usize i = 0; i < degree; ++i)
    {
        out.z.push_back(Complex<T>{T(0), wo});
        out.z.push_back(Complex<T>{T(0), -wo});
    }
    out.k = in.k * detail::cdiv<T>(negz, negp).re;
    return out;
}

// =========================================================================
// iirfilter — the unified design dispatcher (digital). Returns digital zpk.
// =========================================================================

namespace detail
{
template <typename T>
[[nodiscard]] Zpk<T> iir_prototype(crd::memory::IAllocator* alloc, IirKind kind, crd::usize n, T rp, T rs)
{
    switch (kind)
    {
    case IirKind::Cheby1:
        return cheb1ap<T>(alloc, n, rp);
    case IirKind::Cheby2:
        return cheb2ap<T>(alloc, n, rs);
    case IirKind::Ellip:
        return ellipap<T>(alloc, n, rp, rs);
    case IirKind::Bessel:
        return besselap<T>(alloc, n);
    case IirKind::Butter:
    default:
        return buttap<T>(alloc, n);
    }
}
} // namespace detail

// iirfilter: design an order-n digital IIR filter. wn = band edges in Nyquist
// fractions [0,1] (1 element for lp/hp, 2 for bp/bs). rp/rs = ripple/atten dB
// (used only by the kinds that need them). Returns the digital zpk.
template <typename T>
[[nodiscard]] Zpk<T> iirfilter(crd::memory::IAllocator* alloc, crd::usize n, crd::containers::ConstSpan<T> wn,
                               BandType btype, IirKind kind, T rp = T(0), T rs = T(0))
{
    const Zpk<T> proto = detail::iir_prototype<T>(alloc, kind, n, rp, rs);
    const T fs = T(2);
    const T pi = static_cast<T>(std::numbers::pi_v<double>);
    // prewarp the band edges (bilinear): warped = 2*fs*tan(pi*wn/fs).
    T w0 = T(2) * fs * crd::math::tan(pi * wn[0] / fs);
    Zpk<T> sxf(alloc);
    if (btype == BandType::Lowpass)
    {
        sxf = lp2lp_zpk<T>(alloc, proto, w0);
    }
    else if (btype == BandType::Highpass)
    {
        sxf = lp2hp_zpk<T>(alloc, proto, w0);
    }
    else
    {
        const T w1 = T(2) * fs * crd::math::tan(pi * wn[1] / fs);
        const T bw = w1 - w0;
        const T wo = crd::math::sqrt(w0 * w1);
        sxf = (btype == BandType::Bandpass) ? lp2bp_zpk<T>(alloc, proto, wo, bw) : lp2bs_zpk<T>(alloc, proto, wo, bw);
    }
    return bilinear_zpk<T>(alloc, sxf, fs);
}

// =========================================================================
// Order estimation: faithful scipy buttord / cheb1ord / cheb2ord / ellipord.
// =========================================================================

namespace detail
{
// band_stop_obj: scipy's bandpass band-edge objective (n as a function of moved edge).
template <typename T>
[[nodiscard]] T band_stop_obj(T wp, int ind, const T passb[2], const T stopb[2], T gpass, T gstop, IirKind kind) noexcept
{
    T pc[2] = {passb[0], passb[1]};
    pc[ind] = wp;
    const T nat0 = stopb[0] * (pc[0] - pc[1]) / (stopb[0] * stopb[0] - pc[0] * pc[1]);
    const T nat1 = stopb[1] * (pc[0] - pc[1]) / (stopb[1] * stopb[1] - pc[0] * pc[1]);
    const T nat = std::min(std::abs(nat0), std::abs(nat1));
    if (kind == IirKind::Butter)
    {
        const T gs = crd::math::pow(T(10), T(0.1) * std::abs(gstop));
        const T gp = crd::math::pow(T(10), T(0.1) * std::abs(gpass));
        return crd::math::log10((gs - T(1)) / (gp - T(1))) / (T(2) * crd::math::log10(nat));
    }
    if (kind == IirKind::Cheby1 || kind == IirKind::Cheby2)
    {
        const T gs = crd::math::pow(T(10), T(0.1) * std::abs(gstop));
        const T gp = crd::math::pow(T(10), T(0.1) * std::abs(gpass));
        return crd::math::acosh(crd::math::sqrt((gs - T(1)) / (gp - T(1)))) / crd::math::acosh(nat);
    }
    // Ellip (note: scipy uses gstop/gpass WITHOUT abs here).
    const T gs = crd::math::pow(T(10), T(0.1) * gstop);
    const T gp = crd::math::pow(T(10), T(0.1) * gpass);
    const T arg1 = crd::math::sqrt((gp - T(1)) / (gs - T(1)));
    const T arg0 = T(1) / nat;
    const T d00 = ellipk<T>(arg0 * arg0), d01 = ellipk<T>(T(1) - arg0 * arg0);
    const T d10 = ellipk<T>(arg1 * arg1), d11 = ellipk<T>(T(1) - arg1 * arg1);
    return d00 * d11 / (d01 * d10);
}

// fminbound — faithful port of scipy.optimize._minimize_scalar_bounded (Brent bounded,
// xatol=1e-5, maxfun=500). Needed only for bandpass (type 3) order estimation.
template <typename T, typename F> [[nodiscard]] T fminbound(F func, T x1, T x2) noexcept
{
    const T xatol = static_cast<T>(1e-5);
    const int maxfun = 500;
    const T sqrt_eps = crd::math::sqrt(static_cast<T>(2.2e-16));
    const T golden_mean = T(0.5) * (T(3) - crd::math::sqrt(T(5)));
    T a = x1, b = x2;
    T fulc = a + golden_mean * (b - a);
    T nfc = fulc, xf = fulc;
    T rat = T(0), e = T(0);
    T x = xf;
    T fx = func(x);
    int num = 1;
    T ffulc = fx, fnfc = fx;
    T xm = T(0.5) * (a + b);
    T tol1 = sqrt_eps * std::abs(xf) + xatol / T(3);
    T tol2 = T(2) * tol1;
    while (std::abs(xf - xm) > (tol2 - T(0.5) * (b - a)))
    {
        bool golden = true;
        if (std::abs(e) > tol1)
        {
            golden = false;
            T r = (xf - nfc) * (fx - ffulc);
            T q = (xf - fulc) * (fx - fnfc);
            T p = (xf - fulc) * q - (xf - nfc) * r;
            q = T(2) * (q - r);
            if (q > T(0))
            {
                p = -p;
            }
            q = std::abs(q);
            r = e;
            e = rat;
            if ((std::abs(p) < std::abs(T(0.5) * q * r)) && (p > q * (a - xf)) && (p < q * (b - xf)))
            {
                rat = p / q;
                x = xf + rat;
                if (((x - a) < tol2) || ((b - x) < tol2))
                {
                    const T si = (xm - xf >= T(0)) ? T(1) : T(-1);
                    rat = tol1 * si;
                }
            }
            else
            {
                golden = true;
            }
        }
        if (golden)
        {
            e = (xf >= xm) ? (a - xf) : (b - xf);
            rat = golden_mean * e;
        }
        const T si = (rat > T(0)) ? T(1) : ((rat < T(0)) ? T(-1) : T(1));
        x = xf + si * std::max(std::abs(rat), tol1);
        const T fu = func(x);
        ++num;
        if (fu <= fx)
        {
            if (x >= xf)
            {
                a = xf;
            }
            else
            {
                b = xf;
            }
            fulc = nfc;
            ffulc = fnfc;
            nfc = xf;
            fnfc = fx;
            xf = x;
            fx = fu;
        }
        else
        {
            if (x < xf)
            {
                a = x;
            }
            else
            {
                b = x;
            }
            if ((fu <= fnfc) || (nfc == xf))
            {
                fulc = nfc;
                ffulc = fnfc;
                nfc = x;
                fnfc = fu;
            }
            else if ((fu <= ffulc) || (fulc == xf) || (fulc == nfc))
            {
                fulc = x;
                ffulc = fu;
            }
        }
        xm = T(0.5) * (a + b);
        tol1 = sqrt_eps * std::abs(xf) + xatol / T(3);
        tol2 = T(2) * tol1;
        if (num >= maxfun)
        {
            break;
        }
    }
    return xf;
}

template <typename T> struct OrderSetup
{
    int filter_type = 1; // 1=lp, 2=hp, 3=bp, 4=bs
    T passb[2] = {T(0), T(0)};
    T stopb[2] = {T(0), T(0)};
    crd::usize npassb = 1;
    T nat = T(0);
};

// Shared scipy order-estimation preamble: validate_wp_ws + pre_warp + find_nat_freq.
template <typename T>
[[nodiscard]] OrderSetup<T> order_setup(crd::containers::ConstSpan<T> wp, crd::containers::ConstSpan<T> ws, T gpass,
                                        T gstop, IirKind objkind)
{
    OrderSetup<T> s;
    const crd::usize nwp = wp.size();
    s.npassb = nwp;
    int ft = 2 * (static_cast<int>(nwp) - 1) + 1;
    if (wp[0] >= ws[0])
    {
        ft += 1;
    }
    s.filter_type = ft;
    const T pi = static_cast<T>(std::numbers::pi_v<double>);
    for (crd::usize i = 0; i < nwp; ++i)
    {
        s.passb[i] = crd::math::tan(pi * wp[i] / T(2));
        s.stopb[i] = crd::math::tan(pi * ws[i] / T(2));
    }
    if (ft == 1)
    {
        s.nat = std::abs(s.stopb[0] / s.passb[0]);
    }
    else if (ft == 2)
    {
        s.nat = std::abs(s.passb[0] / s.stopb[0]);
    }
    else if (ft == 3)
    {
        const T wp0 = fminbound<T>([&](T w) { return band_stop_obj<T>(w, 0, s.passb, s.stopb, gpass, gstop, objkind); },
                                   s.passb[0], s.stopb[0] - static_cast<T>(1e-12));
        const T wp1 = fminbound<T>([&](T w) { return band_stop_obj<T>(w, 1, s.passb, s.stopb, gpass, gstop, objkind); },
                                   s.stopb[1] + static_cast<T>(1e-12), s.passb[1]);
        s.passb[0] = wp0;
        s.passb[1] = wp1;
        const T n0 = s.stopb[0] * (s.passb[0] - s.passb[1]) / (s.stopb[0] * s.stopb[0] - s.passb[0] * s.passb[1]);
        const T n1 = s.stopb[1] * (s.passb[0] - s.passb[1]) / (s.stopb[1] * s.stopb[1] - s.passb[0] * s.passb[1]);
        s.nat = std::min(std::abs(n0), std::abs(n1));
    }
    else // ft == 4
    {
        const T n0 = (s.stopb[0] * s.stopb[0] - s.passb[0] * s.passb[1]) / (s.stopb[0] * (s.passb[0] - s.passb[1]));
        const T n1 = (s.stopb[1] * s.stopb[1] - s.passb[0] * s.passb[1]) / (s.stopb[1] * (s.passb[0] - s.passb[1]));
        s.nat = std::min(std::abs(n0), std::abs(n1));
    }
    return s;
}

template <typename T> void postprocess_wn(T* wn, crd::usize n) noexcept // WN -> digital Wn = atan(WN)*2/pi
{
    const T pi = static_cast<T>(std::numbers::pi_v<double>);
    for (crd::usize i = 0; i < n; ++i)
    {
        wn[i] = crd::math::atan(wn[i]) * T(2) / pi;
    }
}
} // namespace detail

template <typename T>
[[nodiscard]] IirOrder<T> buttord(crd::memory::IAllocator* alloc, crd::containers::ConstSpan<T> wp,
                                  crd::containers::ConstSpan<T> ws, T gpass, T gstop)
{
    const auto s = detail::order_setup<T>(wp, ws, gpass, gstop, IirKind::Butter);
    const T gs = crd::math::pow(T(10), T(0.1) * std::abs(gstop));
    const T gp = crd::math::pow(T(10), T(0.1) * std::abs(gpass));
    const int ord = static_cast<int>(crd::math::ceil(crd::math::log10((gs - T(1)) / (gp - T(1))) / (T(2) * crd::math::log10(s.nat))));
    const T w0 = crd::math::pow(gp - T(1), T(-1) / (T(2) * static_cast<T>(ord)));
    IirOrder<T> r(alloc);
    r.n = static_cast<crd::usize>(ord);
    if (s.filter_type == 1)
    {
        r.wn.push_back(w0 * s.passb[0]);
    }
    else if (s.filter_type == 2)
    {
        r.wn.push_back(s.passb[0] / w0);
    }
    else if (s.filter_type == 3)
    {
        const T diff = s.passb[1] - s.passb[0];
        const T discr = crd::math::sqrt(diff * diff + T(4) * w0 * w0 * s.passb[0] * s.passb[1]);
        T a0 = std::abs((diff + discr) / (T(2) * w0));
        T a1 = std::abs((diff - discr) / (T(2) * w0));
        if (a0 > a1)
        {
            std::swap(a0, a1);
        }
        r.wn.push_back(a0);
        r.wn.push_back(a1);
    }
    else // 4
    {
        const T diff = s.passb[1] - s.passb[0];
        const T root = crd::math::sqrt(w0 * w0 / T(4) * diff * diff + s.passb[0] * s.passb[1]);
        T a0 = std::abs(w0 * diff / T(2) + root);  // W0[0] = -w0
        T a1 = std::abs(-w0 * diff / T(2) + root); // W0[1] = +w0
        if (a0 > a1)
        {
            std::swap(a0, a1);
        }
        r.wn.push_back(a0);
        r.wn.push_back(a1);
    }
    detail::postprocess_wn<T>(r.wn.data(), r.wn.size());
    return r;
}

template <typename T>
[[nodiscard]] IirOrder<T> cheb1ord(crd::memory::IAllocator* alloc, crd::containers::ConstSpan<T> wp,
                                   crd::containers::ConstSpan<T> ws, T gpass, T gstop)
{
    const auto s = detail::order_setup<T>(wp, ws, gpass, gstop, IirKind::Cheby1);
    const T gs = crd::math::pow(T(10), T(0.1) * std::abs(gstop));
    const T gp = crd::math::pow(T(10), T(0.1) * std::abs(gpass));
    const T v = crd::math::acosh(crd::math::sqrt((gs - T(1)) / (gp - T(1))));
    const int ord = static_cast<int>(crd::math::ceil(v / crd::math::acosh(s.nat)));
    IirOrder<T> r(alloc);
    r.n = static_cast<crd::usize>(ord);
    for (crd::usize i = 0; i < s.npassb; ++i)
    {
        r.wn.push_back(s.passb[i]);
    }
    detail::postprocess_wn<T>(r.wn.data(), r.wn.size());
    return r;
}

template <typename T>
[[nodiscard]] IirOrder<T> cheb2ord(crd::memory::IAllocator* alloc, crd::containers::ConstSpan<T> wp,
                                   crd::containers::ConstSpan<T> ws, T gpass, T gstop)
{
    const auto s = detail::order_setup<T>(wp, ws, gpass, gstop, IirKind::Cheby2);
    const T gs = crd::math::pow(T(10), T(0.1) * std::abs(gstop));
    const T gp = crd::math::pow(T(10), T(0.1) * std::abs(gpass));
    const T v = crd::math::acosh(crd::math::sqrt((gs - T(1)) / (gp - T(1))));
    const int ord = static_cast<int>(crd::math::ceil(v / crd::math::acosh(s.nat)));
    const T new_freq = T(1) / crd::math::cosh((T(1) / static_cast<T>(ord)) * v);
    IirOrder<T> r(alloc);
    r.n = static_cast<crd::usize>(ord);
    if (s.filter_type == 1)
    {
        r.wn.push_back(s.passb[0] / new_freq);
    }
    else if (s.filter_type == 2)
    {
        r.wn.push_back(s.passb[0] * new_freq);
    }
    else if (s.filter_type == 3)
    {
        const T diff = s.passb[1] - s.passb[0];
        const T nat0 = new_freq / T(2) * (s.passb[0] - s.passb[1]) +
                       crd::math::sqrt(new_freq * new_freq * diff * diff / T(4) + s.passb[1] * s.passb[0]);
        const T nat1 = s.passb[1] * s.passb[0] / nat0;
        r.wn.push_back(nat0);
        r.wn.push_back(nat1);
    }
    else // 4
    {
        const T diff = s.passb[1] - s.passb[0];
        const T nat0 = T(1) / (T(2) * new_freq) * (s.passb[0] - s.passb[1]) +
                       crd::math::sqrt(diff * diff / (T(4) * new_freq * new_freq) + s.passb[1] * s.passb[0]);
        const T nat1 = s.passb[0] * s.passb[1] / nat0;
        r.wn.push_back(nat0);
        r.wn.push_back(nat1);
    }
    detail::postprocess_wn<T>(r.wn.data(), r.wn.size());
    return r;
}

template <typename T>
[[nodiscard]] IirOrder<T> ellipord(crd::memory::IAllocator* alloc, crd::containers::ConstSpan<T> wp,
                                   crd::containers::ConstSpan<T> ws, T gpass, T gstop)
{
    const auto s = detail::order_setup<T>(wp, ws, gpass, gstop, IirKind::Ellip);
    const T arg1_sq = (crd::math::pow(T(10), T(0.1) * gpass) - T(1)) / (crd::math::pow(T(10), T(0.1) * gstop) - T(1));
    const T arg0 = T(1) / s.nat;
    const T d00 = ellipk<T>(arg0 * arg0), d01 = ellipk<T>(T(1) - arg0 * arg0);
    const T d10 = ellipk<T>(arg1_sq), d11 = ellipk<T>(T(1) - arg1_sq);
    const int ord = static_cast<int>(crd::math::ceil(d00 * d11 / (d01 * d10)));
    IirOrder<T> r(alloc);
    r.n = static_cast<crd::usize>(ord);
    for (crd::usize i = 0; i < s.npassb; ++i)
    {
        r.wn.push_back(s.passb[i]);
    }
    detail::postprocess_wn<T>(r.wn.data(), r.wn.size());
    return r;
}

// band type implied by (wp, ws): scipy's filter_type logic.
template <typename T>
[[nodiscard]] BandType band_type_of(crd::containers::ConstSpan<T> wp, crd::containers::ConstSpan<T> ws) noexcept
{
    if (wp.size() == 1)
    {
        return (wp[0] < ws[0]) ? BandType::Lowpass : BandType::Highpass;
    }
    // 2-element: a BANDPASS spec has the passband INSIDE the stopband (wp[0] >= ws[0]); bandstop is the reverse.
    return (wp[0] >= ws[0]) ? BandType::Bandpass : BandType::Bandstop;
}

// iirdesign: design from a spec (wp, ws, gpass, gstop) — order estimation then iirfilter.
// kind ∈ {Butter, Cheby1, Cheby2, Ellip} (Bessel has no order estimation in scipy iirdesign).
template <typename T>
[[nodiscard]] Zpk<T> iirdesign(crd::memory::IAllocator* alloc, crd::containers::ConstSpan<T> wp,
                               crd::containers::ConstSpan<T> ws, T gpass, T gstop, IirKind kind)
{
    const BandType bt = band_type_of<T>(wp, ws);
    auto design = [&](const IirOrder<T>& ord)
    { return iirfilter<T>(alloc, ord.n, crd::containers::ConstSpan<T>(ord.wn.data(), ord.wn.size()), bt, kind, gpass, gstop); };
    if (kind == IirKind::Cheby1)
    {
        return design(cheb1ord<T>(alloc, wp, ws, gpass, gstop));
    }
    if (kind == IirKind::Cheby2)
    {
        return design(cheb2ord<T>(alloc, wp, ws, gpass, gstop));
    }
    if (kind == IirKind::Ellip)
    {
        return design(ellipord<T>(alloc, wp, ws, gpass, gstop));
    }
    return design(buttord<T>(alloc, wp, ws, gpass, gstop));
}

// =========================================================================
// Notch / peak / comb (tf — inherently 2nd-order / sparse, well-conditioned).
// =========================================================================

namespace detail
{
// scipy _design_notch_peak_filter (fs = 2 ⇒ w0 in [0,1] Nyquist fraction).
template <typename T>
[[nodiscard]] TransferFunction<T> design_notch_peak(crd::memory::IAllocator* alloc, T w0, T q, bool peak)
{
    const T pi = static_cast<T>(std::numbers::pi_v<double>);
    const T fs = T(2);
    T w0n = T(2) * w0 / fs;
    T bw = w0n / q;
    bw *= pi;
    w0n *= pi;
    const T beta = crd::math::tan(bw / T(2));
    const T gain = T(1) / (T(1) + beta);
    TransferFunction<T> tf(alloc);
    if (!peak)
    {
        tf.b.push_back(gain * T(1));
        tf.b.push_back(gain * (T(-2) * crd::math::cos(w0n)));
        tf.b.push_back(gain * T(1));
    }
    else
    {
        tf.b.push_back(T(1) - gain);
        tf.b.push_back(T(0));
        tf.b.push_back(-(T(1) - gain));
    }
    tf.a.push_back(T(1));
    tf.a.push_back(T(-2) * gain * crd::math::cos(w0n));
    tf.a.push_back(T(2) * gain - T(1));
    return tf;
}
} // namespace detail

template <typename T> [[nodiscard]] TransferFunction<T> iirnotch(crd::memory::IAllocator* alloc, T w0, T q)
{
    return detail::design_notch_peak<T>(alloc, w0, q, false);
}
template <typename T> [[nodiscard]] TransferFunction<T> iirpeak(crd::memory::IAllocator* alloc, T w0, T q)
{
    return detail::design_notch_peak<T>(alloc, w0, q, true);
}

// iircomb: harmonic comb (notches/peaks at integer multiples of w0). fs = 2 ⇒ w0 in (0,1);
// fs must be divisible by w0 (N = round(fs/w0) integer). pass_zero flips the DC behaviour.
template <typename T>
[[nodiscard]] TransferFunction<T> iircomb(crd::memory::IAllocator* alloc, T w0, T q, bool peak, bool pass_zero = false)
{
    const T pi = static_cast<T>(std::numbers::pi_v<double>);
    const T fs = T(2);
    const auto nn = static_cast<crd::usize>(crd::math::lround(static_cast<double>(fs / w0)));
    const T w0r = T(2) * pi * w0 / fs;
    const T w_delta = w0r / q;
    const T g0 = peak ? T(0) : T(1);
    const T g = peak ? T(1) : T(0);
    const T beta = crd::math::tan(static_cast<T>(nn) * w_delta / T(4));
    const T ax = (T(1) - beta) / (T(1) + beta);
    const T bx = (g0 + g * beta) / (T(1) + beta);
    const T cx = (g0 - g * beta) / (T(1) + beta);
    const bool negative_coef = (peak && pass_zero) || (!peak && !pass_zero);
    const T sgn = negative_coef ? T(-1) : T(1);
    TransferFunction<T> tf(alloc);
    tf.b.resize(nn + 1);
    tf.a.resize(nn + 1);
    for (crd::usize i = 0; i <= nn; ++i)
    {
        tf.b[i] = T(0);
        tf.a[i] = T(0);
    }
    tf.b[0] = bx;
    tf.b[nn] = sgn * cx;
    tf.a[0] = T(1);
    tf.a[nn] = sgn * ax;
    return tf;
}

} // namespace crd::hesap::dsp
