#pragma once

// ---------------------------------------------------------------------------
// crd-hesap-dsp v11-d — least-squares FIR design (firls).
//
// firls minimises the weighted integral of |D(w) - A(w)|^2 over the specified
// bands (D = desired, A = the Type-I FIR response). This is a LINEAR least-
// squares problem with a CLOSED FORM: a symmetric (Toeplitz + Hankel) normal-
// equation matrix Q and a vector b, both integrals evaluable in closed form,
// solved once — so firls (unlike the iterative Remez) gets the FULL coefficient
// match to scipy, not just spec-compliance. Faithful scipy.signal.firls.
// numtaps must be ODD (Type-I linear phase). Lower-layer raw scalars.
// ---------------------------------------------------------------------------

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/dense/lu.hpp>
#include <crd/hesap/dense/matrix.hpp>
#include <crd/hesap/dsp/fir.hpp> // np_sinc
#include <crd/memory/allocator.hpp>

#include <crd/math/cmath.hpp>
#include <numbers>

namespace crd::hesap::dsp
{

// firls: `bands` = flattened band-edge pairs normalized to Nyquist (1.0 == fs/2), monotonic, non-overlapping;
// `desired` = the gain at each band edge (same length as bands); `weight` = per-band relative weight (half the
// length of bands), or empty for all-ones. Returns numtaps symmetric (linear-phase) taps. numtaps must be odd.
template <typename T>
[[nodiscard]] crd::containers::Array<T> firls(crd::memory::IAllocator* alloc, crd::usize numtaps,
                                              crd::containers::ConstSpan<T> bands, crd::containers::ConstSpan<T> desired,
                                              crd::containers::ConstSpan<T> weight = {})
{
    CRD_ASSERT(numtaps % 2 == 1 && bands.size() % 2 == 0 && desired.size() == bands.size());
    const crd::usize nband = bands.size() / 2;
    const crd::usize M = (numtaps - 1) / 2;
    const T pi = static_cast<T>(std::numbers::pi_v<double>);

    auto bw = [&](crd::usize b, int e) { return bands[2 * b + static_cast<crd::usize>(e)]; };
    auto dw = [&](crd::usize b, int e) { return desired[2 * b + static_cast<crd::usize>(e)]; };
    auto ww = [&](crd::usize b) { return weight.empty() ? T(1) : weight[b]; };

    // q[i] = Σ_band w * (sinc(right*i)*right - sinc(left*i)*left), i = 0..2M.
    crd::containers::Array<T> q(alloc);
    q.resize(numtaps);
    for (crd::usize i = 0; i < numtaps; ++i)
    {
        T s = T(0);
        for (crd::usize b = 0; b < nband; ++b)
        {
            const T l = bw(b, 0);
            const T r = bw(b, 1);
            s += ww(b) * (np_sinc<T>(r * static_cast<T>(i)) * r - np_sinc<T>(l * static_cast<T>(i)) * l);
        }
        q[i] = s;
    }

    // Q = Toeplitz(q[0..M]) + Hankel ⇒ Q[r,c] = q[|r-c|] + q[r+c]. (M+1)x(M+1), symmetric positive definite.
    const crd::usize dim = M + 1;
    dense::Matrix<T> Q(alloc, dim, dim);
    for (crd::usize r = 0; r < dim; ++r)
    {
        for (crd::usize c = 0; c < dim; ++c)
        {
            const crd::usize d = (r > c) ? (r - c) : (c - r);
            Q(r, c) = q[d] + q[r + c];
        }
    }

    // b[i] = Σ_band w * diff over edges of  [ bands*(m*bands+c)*sinc(bands*i)  (+ n=0/n>=1 corrections) ].
    crd::containers::Array<T> b(alloc);
    b.resize(dim);
    for (crd::usize i = 0; i < dim; ++i)
    {
        T s = T(0);
        for (crd::usize bnd = 0; bnd < nband; ++bnd)
        {
            const T l = bw(bnd, 0);
            const T r = bw(bnd, 1);
            const T m = (dw(bnd, 1) - dw(bnd, 0)) / (r - l); // slope
            const T c0 = dw(bnd, 0) - l * m;                 // intercept
            auto term = [&](T edge) -> T
            {
                T v = edge * (m * edge + c0) * np_sinc<T>(edge * static_cast<T>(i));
                if (i == 0)
                {
                    v -= m * edge * edge / T(2);
                }
                else
                {
                    v += m * crd::math::cos(static_cast<T>(i) * pi * edge) / (pi * static_cast<T>(i)) / (pi * static_cast<T>(i));
                }
                return v;
            };
            s += ww(bnd) * (term(r) - term(l));
        }
        b[i] = s;
    }

    // solve Q a = b (LU; Q is SPD but LU is robust + already available).
    dense::LU<T> lu(alloc, dim);
    dense::factor_lu<T, dense::Layout::RowMajor>(lu, Q);
    dense::solve_lu<T, dense::Layout::RowMajor>(lu, crd::containers::Span<T>(b.data(), dim)); // b ← a

    // coeffs = [a[M],...,a[1], 2 a[0], a[1],...,a[M]] — symmetric, length 2M+1.
    crd::containers::Array<T> h(alloc);
    h.resize(numtaps);
    h[M] = T(2) * b[0];
    for (crd::usize k = 1; k <= M; ++k)
    {
        h[M + k] = b[k];
        h[M - k] = b[k];
    }
    return h;
}

} // namespace crd::hesap::dsp
