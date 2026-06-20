#pragma once

// ---------------------------------------------------------------------------
// crd-hesap-dsp v11-a — filter representations + conversions.
//
//   TransferFunction<T>  b/a (real, filter convention: ascending z^{-k}).
//   Zpk<T>               zeros, poles (complex), gain k. Numerically stable form.
//   SecondOrderSections  cascade of biquads [b0 b1 b2 1 a1 a2] — the STABLE
//                        default for high-order IIR application.
//
// Conversions: zpk<->tf (poly expand / companion-root factor) and zpk->sos
// (conjugate-pair pairing) + sos->tf. The honest gate (the v10 scar): a
// conversion is correct when the cascade RECONSTRUCTS the same H(z) (freqz
// agreement) and the sections carry REAL coefficients (conjugate pairs kept
// together) — NOT a bit-match of scipy's section ORDERING (an implementation
// choice for numerical conditioning).
// ---------------------------------------------------------------------------

#include <crd/containers/array.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/complex.hpp>
#include <crd/hesap/dsp/polynomial.hpp>
#include <crd/memory/allocator.hpp>

#include <cmath>

namespace crd::hesap::dsp
{

// Numerator b + denominator a, ascending negative powers (b[0] + b[1]z^{-1}+...).
// FIR ⇒ a == {1}.
template <typename T> struct TransferFunction
{
    crd::containers::Array<T> b;
    crd::containers::Array<T> a;
    explicit TransferFunction(crd::memory::IAllocator* alloc) : b(alloc), a(alloc) {}
};

// Zeros / poles (complex) + scalar gain.  H(z) = k * prod(1 - z_i z^{-1}) / prod(1 - p_i z^{-1}).
template <typename T> struct Zpk
{
    crd::containers::Array<Complex<T>> z;
    crd::containers::Array<Complex<T>> p;
    T k = T(1);
    explicit Zpk(crd::memory::IAllocator* alloc) : z(alloc), p(alloc) {}
};

// One biquad: b0 + b1 z^{-1} + b2 z^{-2} over 1 + a1 z^{-1} + a2 z^{-2}.
template <typename T> struct Biquad
{
    T b0 = T(1), b1 = T(0), b2 = T(0);
    T a1 = T(0), a2 = T(0); // a0 is normalized to 1
};

// Cascade of biquads (scipy SOS = rows of [b0 b1 b2 1 a1 a2]).
template <typename T> struct SecondOrderSections
{
    crd::containers::Array<Biquad<T>> sections;
    explicit SecondOrderSections(crd::memory::IAllocator* alloc) : sections(alloc) {}
};

// ---- zpk -> tf : expand the factored form (real coefficients out) -----------
template <typename T> [[nodiscard]] TransferFunction<T> zpk_to_tf(crd::memory::IAllocator* alloc, const Zpk<T>& zpk)
{
    TransferFunction<T> tf(alloc);
    const auto bz = poly_from_roots<T>(alloc, crd::containers::ConstSpan<Complex<T>>(zpk.z.data(), zpk.z.size()), zpk.k);
    const auto az = poly_from_roots<T>(alloc, crd::containers::ConstSpan<Complex<T>>(zpk.p.data(), zpk.p.size()), T(1));
    tf.b = real_part_of_poly<T>(alloc, crd::containers::ConstSpan<Complex<T>>(bz.data(), bz.size()));
    tf.a = real_part_of_poly<T>(alloc, crd::containers::ConstSpan<Complex<T>>(az.data(), az.size()));
    return tf;
}

// ---- tf -> zpk : companion-matrix roots -------------------------------------
// CONVENIENCE for USER-supplied tf only — NOT a design-path step. roots-of-tf is
// Wilkinson-ill-conditioned above order ~8 (see the data-flow rule in dsp.hpp).
template <typename T> [[nodiscard]] Zpk<T> tf_to_zpk(crd::memory::IAllocator* alloc, const TransferFunction<T>& tf)
{
    Zpk<T> zpk(alloc);
    zpk.z = roots<T>(alloc, crd::containers::ConstSpan<T>(tf.b.data(), tf.b.size()));
    zpk.p = roots<T>(alloc, crd::containers::ConstSpan<T>(tf.a.data(), tf.a.size()));
    zpk.k = tf.b.empty() ? T(0) : (tf.a.empty() ? tf.b[0] : tf.b[0] / tf.a[0]);
    return zpk;
}

namespace detail
{
// Split a complex set into conjugate pairs (one representative each, +imag) and
// reals. Tolerance-matched conjugates are paired; lone reals returned separately.
template <typename T>
void cplx_real_split(crd::memory::IAllocator* alloc, crd::containers::ConstSpan<Complex<T>> v,
                     crd::containers::Array<Complex<T>>& cplx, crd::containers::Array<T>& reals)
{
    const T tol = static_cast<T>(1e-6);
    crd::containers::Array<bool> used(alloc);
    used.resize(v.size());
    for (crd::usize i = 0; i < v.size(); ++i)
    {
        used[i] = false;
    }
    for (crd::usize i = 0; i < v.size(); ++i)
    {
        if (used[i])
        {
            continue;
        }
        if (std::abs(v[i].im) <= tol * (T(1) + std::abs(v[i].re)))
        {
            reals.push_back(v[i].re);
            used[i] = true;
            continue;
        }
        // find the conjugate partner.
        crd::usize best = v.size();
        T bestd = tol;
        for (crd::usize j = i + 1; j < v.size(); ++j)
        {
            if (used[j])
            {
                continue;
            }
            const T d = std::abs(v[j].re - v[i].re) + std::abs(v[j].im + v[i].im);
            if (d <= bestd)
            {
                bestd = d;
                best = j;
            }
        }
        // keep the +imag representative.
        cplx.push_back(Complex<T>{v[i].re, std::abs(v[i].im)});
        used[i] = true;
        if (best < v.size())
        {
            used[best] = true;
        }
    }
}
} // namespace detail

namespace detail
{
// A 2nd-order factor 1 + c1 z^{-1} + c2 z^{-2} built from a conjugate pair or
// two reals, with a complex `repr` used for nearest-neighbour pairing.
template <typename T> struct Quad
{
    T c1 = T(0), c2 = T(0);
    Complex<T> repr{T(0), T(0)}; // representative root (for unit-circle proximity + pairing distance)
    T mag = T(0);                // |repr| — proximity to the unit circle
};

// Build the 2nd-order factor list from a split (conjugate pairs + reals).
template <typename T>
void build_quads(crd::memory::IAllocator* alloc, crd::containers::Array<Complex<T>>& cc,
                 crd::containers::Array<T>& rr, crd::containers::Array<Quad<T>>& out)
{
    for (crd::usize i = 0; i < cc.size(); ++i) // conjugate pair r ± i ⇒ [1, -2r, r²+i²]
    {
        Quad<T> q;
        q.c1 = T(-2) * cc[i].re;
        q.c2 = cc[i].re * cc[i].re + cc[i].im * cc[i].im;
        q.repr = cc[i];
        q.mag = std::sqrt(q.c2);
        out.push_back(q);
    }
    crd::usize i = 0;
    for (; i + 1 < rr.size(); i += 2) // two reals ⇒ [1, -(r0+r1), r0 r1]
    {
        Quad<T> q;
        q.c1 = -(rr[i] + rr[i + 1]);
        q.c2 = rr[i] * rr[i + 1];
        const T m0 = std::abs(rr[i]);
        const T m1 = std::abs(rr[i + 1]);
        q.repr = Complex<T>{(m0 >= m1) ? rr[i] : rr[i + 1], T(0)};
        q.mag = (m0 >= m1) ? m0 : m1;
        out.push_back(q);
    }
    if (i < rr.size()) // a leftover real ⇒ first-order [1, -r, 0]
    {
        Quad<T> q;
        q.c1 = -rr[i];
        q.c2 = T(0);
        q.repr = Complex<T>{rr[i], T(0)};
        q.mag = std::abs(rr[i]);
        out.push_back(q);
    }
    (void)alloc;
}
} // namespace detail

// ---- zpk -> sos : nearest-neighbour pairing into real biquads ---------------
// The conditioning-correct pairing (the reason SOS exists, scipy zpk2sos): order
// the pole 2nd-order factors by proximity to the UNIT CIRCLE (most-unstable
// first), and pair each with its NEAREST zero factor — this minimises the
// dynamic range of each cascade stage in finite precision. Every section carries
// REAL coefficients (conjugate pairs kept together); the cascade equals H(z).
// (The conditioning BENEFIT is gated by the high-order sosfilt-vs-tf application
// test at v11-i; here the gate is exact-reconstruction at order 12.)
template <typename T> [[nodiscard]] SecondOrderSections<T> zpk_to_sos(crd::memory::IAllocator* alloc, const Zpk<T>& zpk)
{
    SecondOrderSections<T> sos(alloc);

    crd::containers::Array<Complex<T>> zc(alloc), pc(alloc);
    crd::containers::Array<T> zr(alloc), pr(alloc);
    detail::cplx_real_split<T>(alloc, crd::containers::ConstSpan<Complex<T>>(zpk.z.data(), zpk.z.size()), zc, zr);
    detail::cplx_real_split<T>(alloc, crd::containers::ConstSpan<Complex<T>>(zpk.p.data(), zpk.p.size()), pc, pr);

    crd::containers::Array<detail::Quad<T>> num(alloc), den(alloc);
    detail::build_quads<T>(alloc, zc, zr, num);
    detail::build_quads<T>(alloc, pc, pr, den);

    const crd::usize nsec = (num.size() > den.size()) ? num.size() : den.size();

    // pole order: closest to the unit circle FIRST (descending |repr|).
    crd::containers::Array<crd::usize> porder(alloc);
    porder.resize(den.size());
    for (crd::usize i = 0; i < den.size(); ++i)
    {
        porder[i] = i;
    }
    for (crd::usize i = 0; i + 1 < porder.size(); ++i) // small n ⇒ selection sort (deterministic)
    {
        crd::usize best = i;
        for (crd::usize j = i + 1; j < porder.size(); ++j)
        {
            if (den[porder[j]].mag > den[porder[best]].mag)
            {
                best = j;
            }
        }
        const crd::usize t = porder[i];
        porder[i] = porder[best];
        porder[best] = t;
    }

    crd::containers::Array<bool> zused(alloc);
    zused.resize(num.size());
    for (crd::usize i = 0; i < num.size(); ++i)
    {
        zused[i] = false;
    }

    for (crd::usize s = 0; s < nsec; ++s)
    {
        Biquad<T> bq;
        // denominator: the next pole factor in unit-circle order (trivial if exhausted).
        if (s < den.size())
        {
            const detail::Quad<T>& pq = den[porder[s]];
            bq.a1 = pq.c1;
            bq.a2 = pq.c2;
            // numerator: the nearest unused zero factor to this pole.
            crd::usize bestz = num.size();
            T bestd = T(0);
            for (crd::usize j = 0; j < num.size(); ++j)
            {
                if (zused[j])
                {
                    continue;
                }
                const T dr = num[j].repr.re - pq.repr.re;
                const T di = num[j].repr.im - pq.repr.im;
                const T d = dr * dr + di * di;
                if (bestz == num.size() || d < bestd)
                {
                    bestd = d;
                    bestz = j;
                }
            }
            if (bestz < num.size())
            {
                bq.b0 = T(1);
                bq.b1 = num[bestz].c1;
                bq.b2 = num[bestz].c2;
                zused[bestz] = true;
            }
        }
        else // pole factors exhausted ⇒ remaining zero factors as FIR sections
        {
            for (crd::usize j = 0; j < num.size(); ++j)
            {
                if (!zused[j])
                {
                    bq.b0 = T(1);
                    bq.b1 = num[j].c1;
                    bq.b2 = num[j].c2;
                    zused[j] = true;
                    break;
                }
            }
        }
        sos.sections.push_back(bq);
    }
    if (sos.sections.empty()) // pure gain
    {
        sos.sections.push_back(Biquad<T>{});
    }
    // fold the gain into the first (most-unstable) section's numerator.
    sos.sections[0].b0 *= zpk.k;
    sos.sections[0].b1 *= zpk.k;
    sos.sections[0].b2 *= zpk.k;
    return sos;
}

// ---- sos -> tf : multiply out the cascade (real convolution) ----------------
template <typename T>
[[nodiscard]] TransferFunction<T> sos_to_tf(crd::memory::IAllocator* alloc, const SecondOrderSections<T>& sos)
{
    TransferFunction<T> tf(alloc);
    tf.b.push_back(T(1));
    tf.a.push_back(T(1));
    auto conv = [&](crd::containers::Array<T>& acc, T s0, T s1, T s2)
    {
        crd::containers::Array<T> out(alloc);
        out.resize(acc.size() + 2);
        for (crd::usize i = 0; i < out.size(); ++i)
        {
            out[i] = T(0);
        }
        const T s[3] = {s0, s1, s2};
        for (crd::usize i = 0; i < acc.size(); ++i)
        {
            for (crd::usize j = 0; j < 3; ++j)
            {
                out[i + j] += acc[i] * s[j];
            }
        }
        acc = std::move(out);
    };
    for (crd::usize k = 0; k < sos.sections.size(); ++k)
    {
        const Biquad<T>& bq = sos.sections[k];
        conv(tf.b, bq.b0, bq.b1, bq.b2);
        conv(tf.a, T(1), bq.a1, bq.a2);
    }
    return tf;
}

} // namespace crd::hesap::dsp
