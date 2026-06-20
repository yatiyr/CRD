#pragma once

// ---------------------------------------------------------------------------
// crd-hesap-dsp v11-i — filter APPLICATION (the streaming hot loop).
//
// This is the two-layer (ADR-0078) LOWER layer: allocation-free, stateful,
// block-processing kernels — the DAW/SDR real-time path. Filter application is
// pure multiply-add (no transcendentals), so the honest gate is BIT-EXACT vs
// scipy.signal.sosfilt + the {1..16} streaming determinism moat: with a fixed
// per-element FP order (-ffp-contract=off, ADR-0078), feeding the kernel in any
// block sizes produces output IDENTICAL to one batch call (and to scipy).
//
// SOS cascade applied as Direct-Form-II-Transposed biquads (scipy's form), 2
// state words per section. The PERF battleground vs Intel IPP / liquid-dsp.
// ---------------------------------------------------------------------------

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/dense/lu.hpp>
#include <crd/hesap/dense/matrix.hpp>
#include <crd/hesap/dsp/filter.hpp> // SecondOrderSections

namespace crd::hesap::dsp
{

// lfilter (scipy.signal.lfilter): general transfer-function IIR, Direct-Form-II-Transposed. b/a (a normalized by
// a[0]). Optional initial state `zi` (length order). Pure multiply-add ⇒ BIT-EXACT vs scipy (-ffp-contract=off).
template <typename T>
[[nodiscard]] crd::containers::Array<T> lfilter(crd::memory::IAllocator* alloc, crd::containers::ConstSpan<T> b,
                                                crd::containers::ConstSpan<T> a, crd::containers::ConstSpan<T> x,
                                                crd::containers::ConstSpan<T> zi = {})
{
    const crd::usize order = ((b.size() > a.size() ? b.size() : a.size())) - 1;
    crd::containers::Array<T> bn(alloc), an(alloc), z(alloc), y(alloc);
    bn.resize(order + 1);
    an.resize(order + 1);
    z.resize(order == 0 ? 1 : order);
    const T a0 = a[0];
    for (crd::usize i = 0; i <= order; ++i)
    {
        bn[i] = (i < b.size() ? b[i] : T(0)) / a0;
        an[i] = (i < a.size() ? a[i] : T(0)) / a0;
    }
    for (crd::usize i = 0; i < order; ++i)
    {
        z[i] = (i < zi.size()) ? zi[i] : T(0);
    }
    y.resize(x.size());
    for (crd::usize nidx = 0; nidx < x.size(); ++nidx)
    {
        const T xn = x[nidx];
        const T yn = bn[0] * xn + (order > 0 ? z[0] : T(0));
        for (crd::usize i = 0; i + 1 < order; ++i)
        {
            z[i] = bn[i + 1] * xn + z[i + 1] - an[i + 1] * yn; // scipy's exact association ⇒ bit-exact
        }
        if (order > 0)
        {
            z[order - 1] = bn[order] * xn - an[order] * yn;
        }
        y[nidx] = yn;
    }
    return y;
}

// lfilter_zi (scipy.signal.lfilter_zi): the steady-state initial conditions of the step response, zi =
// (I - companion(a)ᵀ)⁻¹ (b[1:] - a[1:]·b[0]) (a normalized). Used by filtfilt to suppress edge transients.
template <typename T>
[[nodiscard]] crd::containers::Array<T> lfilter_zi(crd::memory::IAllocator* alloc, crd::containers::ConstSpan<T> b,
                                                   crd::containers::ConstSpan<T> a)
{
    const crd::usize n = (b.size() > a.size() ? b.size() : a.size());
    crd::containers::Array<T> bn(alloc), an(alloc);
    bn.resize(n);
    an.resize(n);
    const T a0 = a[0];
    for (crd::usize i = 0; i < n; ++i)
    {
        bn[i] = (i < b.size() ? b[i] : T(0)) / a0;
        an[i] = (i < a.size() ? a[i] : T(0)) / a0;
    }
    const crd::usize d = n - 1;
    crd::containers::Array<T> zi(alloc);
    zi.resize(d);
    if (d == 0)
    {
        return zi;
    }
    // (I - companion(a)ᵀ) zi = B, B[i] = b[i+1] - a[i+1]·b[0]. companion(a) row 0 = -a[1..]; sub-diag 1s ⇒ ᵀ has
    // first column = -a[1..], super-diagonal 1s.
    dense::Matrix<T> M(alloc, d, d);
    for (crd::usize i = 0; i < d; ++i)
    {
        for (crd::usize j = 0; j < d; ++j)
        {
            M(i, j) = (i == j) ? T(1) : T(0);
        }
    }
    for (crd::usize i = 0; i < d; ++i)
    {
        M(i, 0) -= -an[i + 1]; // I - companionᵀ[i][0]
    }
    for (crd::usize i = 0; i + 1 < d; ++i)
    {
        M(i, i + 1) -= T(1); // companionᵀ super-diagonal
    }
    crd::containers::Array<T> rhs(alloc);
    rhs.resize(d);
    for (crd::usize i = 0; i < d; ++i)
    {
        rhs[i] = bn[i + 1] - an[i + 1] * bn[0];
    }
    dense::LU<T> lu(alloc, d);
    dense::factor_lu<T, dense::Layout::RowMajor>(lu, M);
    dense::solve_lu<T, dense::Layout::RowMajor>(lu, crd::containers::Span<T>(rhs.data(), d));
    for (crd::usize i = 0; i < d; ++i)
    {
        zi[i] = rhs[i];
    }
    return zi;
}

// filtfilt (scipy.signal.filtfilt, padtype='odd', default padlen): forward-backward zero-phase IIR with odd-
// reflection edge padding + lfilter_zi transient suppression ⇒ matches scipy + zero phase.
template <typename T>
[[nodiscard]] crd::containers::Array<T> filtfilt(crd::memory::IAllocator* alloc, crd::containers::ConstSpan<T> b,
                                                 crd::containers::ConstSpan<T> a, crd::containers::ConstSpan<T> x)
{
    const crd::usize ntaps = (b.size() > a.size() ? b.size() : a.size());
    const crd::usize edge = 3 * ntaps;
    const crd::usize n = x.size();
    crd::containers::Array<T> out(alloc);
    if (n <= edge)
    {
        out.resize(n);
        for (crd::usize i = 0; i < n; ++i)
        {
            out[i] = x[i];
        }
        return out;
    }
    crd::containers::Array<T> ext(alloc);
    ext.resize(n + 2 * edge);
    for (crd::usize i = 0; i < edge; ++i) // odd extension: 2·x[0] − x[edge−i]
    {
        ext[i] = T(2) * x[0] - x[edge - i];
    }
    for (crd::usize i = 0; i < n; ++i)
    {
        ext[edge + i] = x[i];
    }
    for (crd::usize i = 0; i < edge; ++i)
    {
        ext[edge + n + i] = T(2) * x[n - 1] - x[n - 2 - i];
    }
    const auto zi = lfilter_zi<T>(alloc, b, a);
    crd::containers::Array<T> zi0(alloc), zi1(alloc);
    zi0.resize(zi.size());
    for (crd::usize i = 0; i < zi.size(); ++i)
    {
        zi0[i] = zi[i] * ext[0];
    }
    auto yf = lfilter<T>(alloc, b, a, crd::containers::ConstSpan<T>(ext.data(), ext.size()),
                         crd::containers::ConstSpan<T>(zi0.data(), zi0.size()));
    crd::containers::Array<T> yr(alloc);
    yr.resize(yf.size());
    for (crd::usize i = 0; i < yf.size(); ++i)
    {
        yr[i] = yf[yf.size() - 1 - i];
    }
    zi1.resize(zi.size());
    for (crd::usize i = 0; i < zi.size(); ++i)
    {
        zi1[i] = zi[i] * yr[0];
    }
    auto yb = lfilter<T>(alloc, b, a, crd::containers::ConstSpan<T>(yr.data(), yr.size()),
                         crd::containers::ConstSpan<T>(zi1.data(), zi1.size()));
    out.resize(n); // reverse back + trim the padding
    for (crd::usize i = 0; i < n; ++i)
    {
        out[i] = yb[yb.size() - 1 - (edge + i)];
    }
    return out;
}

// Per-section DF2T state (z1, z2). One per biquad; carried across streaming blocks.
template <typename T> struct BiquadState
{
    T z1 = T(0), z2 = T(0);
};

// The CORE streaming kernel: apply the SOS cascade to `x` (length n) writing `out` (length n), advancing the
// per-section `state` (length sos.sections.size()). Direct-Form-II-Transposed, exactly scipy's recurrence ⇒
// bit-exact + block-size-invariant. Allocation-free (caller owns out + state). out may alias x.
template <typename T>
void sosfilt_stream(const SecondOrderSections<T>& sos, crd::containers::ConstSpan<T> x, crd::containers::Span<T> out,
                    crd::containers::Span<BiquadState<T>> state) noexcept
{
    const crd::usize ns = sos.sections.size();
    for (crd::usize i = 0; i < x.size(); ++i)
    {
        T xn = x[i];
        for (crd::usize s = 0; s < ns; ++s)
        {
            const Biquad<T>& bq = sos.sections[s];
            BiquadState<T>& st = state[s];
            const T xnew = bq.b0 * xn + st.z1;
            st.z1 = bq.b1 * xn - bq.a1 * xnew + st.z2;
            st.z2 = bq.b2 * xn - bq.a2 * xnew;
            xn = xnew;
        }
        out[i] = xn;
    }
}

// Batch convenience (zero initial state): allocate the output + state, run the kernel. Bit-exact to scipy sosfilt.
template <typename T>
[[nodiscard]] crd::containers::Array<T> sosfilt(crd::memory::IAllocator* alloc, const SecondOrderSections<T>& sos,
                                                crd::containers::ConstSpan<T> x)
{
    crd::containers::Array<T> out(alloc);
    out.resize(x.size());
    crd::containers::Array<BiquadState<T>> state(alloc);
    state.resize(sos.sections.size());
    for (crd::usize s = 0; s < state.size(); ++s)
    {
        state[s] = BiquadState<T>{};
    }
    sosfilt_stream<T>(sos, x, crd::containers::Span<T>(out.data(), out.size()),
                      crd::containers::Span<BiquadState<T>>(state.data(), state.size()));
    return out;
}

// Zero-phase forward-backward filtering (scipy sosfiltfilt, default padding off here): filter forward, reverse,
// filter again, reverse — doubles the order, zero phase. Bit-exact to a matched scipy call (no edge padding).
template <typename T>
[[nodiscard]] crd::containers::Array<T> sosfiltfilt_nopad(crd::memory::IAllocator* alloc,
                                                          const SecondOrderSections<T>& sos,
                                                          crd::containers::ConstSpan<T> x)
{
    crd::containers::Array<T> fwd = sosfilt<T>(alloc, sos, x);
    const crd::usize n = fwd.size();
    crd::containers::Array<T> rev(alloc);
    rev.resize(n);
    for (crd::usize i = 0; i < n; ++i)
    {
        rev[i] = fwd[n - 1 - i];
    }
    crd::containers::Array<T> bwd = sosfilt<T>(alloc, sos, crd::containers::ConstSpan<T>(rev.data(), n));
    crd::containers::Array<T> out(alloc);
    out.resize(n);
    for (crd::usize i = 0; i < n; ++i)
    {
        out[i] = bwd[n - 1 - i];
    }
    return out;
}

} // namespace crd::hesap::dsp
