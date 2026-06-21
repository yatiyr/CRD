#pragma once

// ---------------------------------------------------------------------------
// crd-hesap-dsp v11-d — Parks-McClellan optimal (equiripple) FIR design (remez).
//
// The Remez exchange algorithm (McClellan-Parks-Rabiner 1973): find the FIR that
// MINIMISES the maximum weighted error |W(w)(D(w)-A(w))| (the Chebyshev/minimax
// criterion) — giving EQUIRIPPLE pass/stop bands. It iterates: pick L+1 trial
// extremal frequencies, solve so the weighted error alternates ±delta there
// (barycentric interpolation + the closed-form delta), find the new error
// extrema on a dense grid, exchange, repeat until the extrema stabilise.
//
// This is ITERATIVE + transcendental ⇒ the honest gate is SPEC-COMPLIANCE
// (equiripple: all extrema reach |delta|; meets the band spec) + ~N-digit coeff
// agreement with scipy (the minimax solution is unique) — NOT bit-match.
// Type-I (odd numtaps, symmetric). bands/desired/weight in [0, 0.5] (fs=1).
// ---------------------------------------------------------------------------

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/dense/lu.hpp>
#include <crd/hesap/dense/matrix.hpp>
#include <crd/memory/allocator.hpp>

#include <cmath>
#include <numbers>

namespace crd::hesap::dsp
{

// remez (Type-I): numtaps ODD. `bands` = flattened edge pairs in [0,0.5] (fs=1). `desired` = one gain per BAND
// (length = nbands). `weight` = one per band (or empty ⇒ all ones). `grid_density` = grid points per coefficient.
template <typename T>
[[nodiscard]] crd::containers::Array<T> remez(crd::memory::IAllocator* alloc, crd::usize numtaps,
                                              crd::containers::ConstSpan<T> bands, crd::containers::ConstSpan<T> desired,
                                              crd::containers::ConstSpan<T> weight = {}, crd::usize grid_density = 16,
                                              crd::usize max_iter = 40)
{
    CRD_ASSERT(numtaps % 2 == 1 && bands.size() % 2 == 0);
    const crd::usize nbands = bands.size() / 2;
    CRD_ASSERT(desired.size() == nbands);
    const crd::usize nfcns = (numtaps + 1) / 2; // M+1 cosine basis functions
    const T two_pi = static_cast<T>(2.0 * std::numbers::pi_v<double>);
    const T pi = static_cast<T>(std::numbers::pi_v<double>);

    // --- dense frequency grid over the bands (with per-grid desired + weight) ---
    crd::containers::Array<T> grid(alloc), gdes(alloc), gwt(alloc);
    const T delf = T(0.5) / static_cast<T>(grid_density * nfcns);
    for (crd::usize b = 0; b < nbands; ++b)
    {
        const T f0 = bands[2 * b];
        const T f1 = bands[2 * b + 1];
        T f = f0;
        while (f <= f1 + delf * T(0.5))
        {
            grid.push_back(f > f1 ? f1 : f);
            gdes.push_back(desired[b]);
            gwt.push_back(weight.empty() ? T(1) : weight[b]);
            if (f >= f1)
            {
                break;
            }
            f += delf;
        }
    }
    const crd::usize ngrid = grid.size();

    // --- initial extremal set: nfcns+1 indices evenly spaced over the grid ---
    const crd::usize next = nfcns + 1;
    crd::containers::Array<crd::usize> ext(alloc);
    ext.resize(next);
    for (crd::usize i = 0; i < next; ++i)
    {
        ext[i] = i * (ngrid - 1) / (next - 1);
    }

    crd::containers::Array<T> x(alloc), y(alloc), ad(alloc);
    x.resize(next);
    y.resize(next);
    ad.resize(next);
    crd::containers::Array<T> err(alloc);
    err.resize(ngrid);

    auto cosg = [&](crd::usize gi) { return std::cos(two_pi * grid[gi]); };

    for (crd::usize iter = 0; iter < max_iter; ++iter)
    {
        // x[i] = cos(2*pi*f) at the extrema; Lagrange weights ad[i] = 1 / Π_{j!=i} (x[i]-x[j]).
        for (crd::usize i = 0; i < next; ++i)
        {
            x[i] = cosg(ext[i]);
        }
        for (crd::usize i = 0; i < next; ++i)
        {
            T p = T(1);
            for (crd::usize j = 0; j < next; ++j)
            {
                if (j != i)
                {
                    p *= (x[i] - x[j]);
                }
            }
            ad[i] = T(1) / p;
        }
        // delta = Σ ad*D / Σ (-1)^i ad / W.
        T numer = T(0), denom = T(0), sign = T(1);
        for (crd::usize i = 0; i < next; ++i)
        {
            numer += ad[i] * gdes[ext[i]];
            denom += sign * ad[i] / gwt[ext[i]];
            sign = -sign;
        }
        const T delta = numer / denom;
        // y[i] = the value A must interpolate at the extrema.
        sign = T(1);
        for (crd::usize i = 0; i < next; ++i)
        {
            y[i] = gdes[ext[i]] - sign * delta / gwt[ext[i]];
            sign = -sign;
        }

        // error over the whole grid: E = W*(D - A), A by barycentric interpolation through (x, y, ad).
        for (crd::usize gi = 0; gi < ngrid; ++gi)
        {
            const T xf = cosg(gi);
            T num = T(0), den = T(0);
            bool hit = false;
            T aval = T(0);
            for (crd::usize j = 0; j < next; ++j)
            {
                const T d = xf - x[j];
                if (std::abs(d) < static_cast<T>(1e-12))
                {
                    aval = y[j];
                    hit = true;
                    break;
                }
                const T c = ad[j] / d;
                den += c;
                num += c * y[j];
            }
            const T A = hit ? aval : (num / den);
            err[gi] = gwt[gi] * (gdes[gi] - A);
        }

        // --- search: the new extremal set = local extrema of |E| (+ band edges), alternating, top next ---
        crd::containers::Array<crd::usize> cand(alloc);
        for (crd::usize gi = 0; gi < ngrid; ++gi)
        {
            const bool left_edge = (gi == 0) || (grid[gi] < grid[gi - 1]); // band start
            const bool right_edge = (gi == ngrid - 1) || (grid[gi + 1] < grid[gi]);
            bool is_ext = false;
            if (left_edge || right_edge)
            {
                is_ext = true;
            }
            else if ((err[gi] > err[gi - 1] && err[gi] >= err[gi + 1]) ||
                     (err[gi] < err[gi - 1] && err[gi] <= err[gi + 1]))
            {
                is_ext = true;
            }
            if (is_ext)
            {
                cand.push_back(gi);
            }
        }
        // keep an alternating subsequence by |E| (greedy: within equal-sign runs keep the largest |E|).
        crd::containers::Array<crd::usize> alt(alloc);
        crd::usize ci = 0;
        while (ci < cand.size())
        {
            crd::usize best = cand[ci];
            const T s0 = (err[cand[ci]] >= T(0)) ? T(1) : T(-1);
            crd::usize cj = ci;
            while (cj < cand.size() && ((err[cand[cj]] >= T(0)) ? T(1) : T(-1)) == s0)
            {
                if (std::abs(err[cand[cj]]) > std::abs(err[best]))
                {
                    best = cand[cj];
                }
                ++cj;
            }
            alt.push_back(best);
            ci = cj;
        }
        // reduce/grow to exactly `next` extrema: drop the smallest-|E| ends.
        while (alt.size() > next)
        {
            if (std::abs(err[alt[0]]) <= std::abs(err[alt[alt.size() - 1]]))
            {
                alt.erase(0);
            }
            else
            {
                alt.erase(alt.size() - 1);
            }
        }

        // convergence: extrema unchanged.
        bool same = (alt.size() == next);
        if (same)
        {
            for (crd::usize i = 0; i < next; ++i)
            {
                if (alt[i] != ext[i])
                {
                    same = false;
                    break;
                }
            }
        }
        if (alt.size() == next)
        {
            for (crd::usize i = 0; i < next; ++i)
            {
                ext[i] = alt[i];
            }
        }
        if (same)
        {
            break;
        }
    }

    // --- final coefficients: A(w_k) at M+1 points w_k = pi k / M, then solve C alpha = A (C[k][n]=cos(n w_k)) ---
    const crd::usize M = nfcns - 1;
    // recompute x,y,ad at the converged extrema.
    for (crd::usize i = 0; i < next; ++i)
    {
        x[i] = cosg(ext[i]);
    }
    for (crd::usize i = 0; i < next; ++i)
    {
        T p = T(1);
        for (crd::usize j = 0; j < next; ++j)
        {
            if (j != i)
            {
                p *= (x[i] - x[j]);
            }
        }
        ad[i] = T(1) / p;
    }
    {
        T numer = T(0), denom = T(0), sign = T(1);
        for (crd::usize i = 0; i < next; ++i)
        {
            numer += ad[i] * gdes[ext[i]];
            denom += sign * ad[i] / gwt[ext[i]];
            sign = -sign;
        }
        const T delta = numer / denom;
        sign = T(1);
        for (crd::usize i = 0; i < next; ++i)
        {
            y[i] = gdes[ext[i]] - sign * delta / gwt[ext[i]];
            sign = -sign;
        }
    }
    auto Aeval = [&](T w) -> T
    {
        const T xf = std::cos(w);
        T num = T(0), den = T(0);
        for (crd::usize j = 0; j < next; ++j)
        {
            const T d = xf - x[j];
            if (std::abs(d) < static_cast<T>(1e-12))
            {
                return y[j];
            }
            const T c = ad[j] / d;
            den += c;
            num += c * y[j];
        }
        return num / den;
    };

    const crd::usize dim = M + 1;
    dense::Matrix<T> C(alloc, dim, dim);
    crd::containers::Array<T> rhs(alloc);
    rhs.resize(dim);
    for (crd::usize k = 0; k < dim; ++k)
    {
        const T wk = pi * static_cast<T>(k) / static_cast<T>(M == 0 ? 1 : M);
        for (crd::usize nn = 0; nn < dim; ++nn)
        {
            C(k, nn) = std::cos(static_cast<T>(nn) * wk);
        }
        rhs[k] = Aeval(wk);
    }
    dense::LU<T> lu(alloc, dim);
    dense::factor_lu<T, dense::Layout::RowMajor>(lu, C);
    dense::solve_lu<T, dense::Layout::RowMajor>(lu, crd::containers::Span<T>(rhs.data(), dim)); // rhs ← alpha

    crd::containers::Array<T> h(alloc);
    h.resize(numtaps);
    h[M] = rhs[0];
    for (crd::usize n = 1; n <= M; ++n)
    {
        h[M + n] = rhs[n] / T(2);
        h[M - n] = rhs[n] / T(2);
    }
    return h;
}

namespace detail
{
// Type-III (odd numtaps, ANTISYMMETRIC) Parks-McClellan over a precomputed per-grid (desired/sin, weight·sin).
// A(w) = sin(w)·P(w), P a cosine sum of degree M-1 (M = (numtaps-1)/2). Returns the antisymmetric impulse response.
template <typename T>
[[nodiscard]] crd::containers::Array<T> remez_type3(crd::memory::IAllocator* alloc, crd::usize numtaps,
                                                    crd::containers::ConstSpan<T> grid, crd::containers::ConstSpan<T> gdes,
                                                    crd::containers::ConstSpan<T> gwt, crd::usize max_iter = 60)
{
    const crd::usize m = (numtaps - 1) / 2; // P has M cosine terms cos(0..M-1)
    const crd::usize nfcns = m;
    const crd::usize next = nfcns + 1;
    const T two_pi = static_cast<T>(2.0 * std::numbers::pi_v<double>);
    const T pi = static_cast<T>(std::numbers::pi_v<double>);
    const crd::usize ngrid = grid.size();
    auto cosg = [&](crd::usize gi) { return std::cos(two_pi * grid[gi]); };

    crd::containers::Array<crd::usize> ext(alloc);
    ext.resize(next);
    for (crd::usize i = 0; i < next; ++i)
    {
        ext[i] = i * (ngrid - 1) / (next - 1);
    }
    crd::containers::Array<T> x(alloc), y(alloc), ad(alloc), err(alloc);
    x.resize(next);
    y.resize(next);
    ad.resize(next);
    err.resize(ngrid);
    for (crd::usize iter = 0; iter < max_iter; ++iter)
    {
        for (crd::usize i = 0; i < next; ++i)
        {
            x[i] = cosg(ext[i]);
        }
        for (crd::usize i = 0; i < next; ++i)
        {
            T p = T(1);
            for (crd::usize j = 0; j < next; ++j)
            {
                if (j != i)
                {
                    p *= (x[i] - x[j]);
                }
            }
            ad[i] = T(1) / p;
        }
        T numer = T(0), denom = T(0), sign = T(1);
        for (crd::usize i = 0; i < next; ++i)
        {
            numer += ad[i] * gdes[ext[i]];
            denom += sign * ad[i] / gwt[ext[i]];
            sign = -sign;
        }
        const T delta = numer / denom;
        sign = T(1);
        for (crd::usize i = 0; i < next; ++i)
        {
            y[i] = gdes[ext[i]] - sign * delta / gwt[ext[i]];
            sign = -sign;
        }
        for (crd::usize gi = 0; gi < ngrid; ++gi)
        {
            const T xf = cosg(gi);
            T num = T(0), den = T(0), aval = T(0);
            bool hit = false;
            for (crd::usize j = 0; j < next; ++j)
            {
                const T d = xf - x[j];
                if (std::abs(d) < static_cast<T>(1e-12))
                {
                    aval = y[j];
                    hit = true;
                    break;
                }
                const T cc = ad[j] / d;
                den += cc;
                num += cc * y[j];
            }
            err[gi] = gwt[gi] * (gdes[gi] - (hit ? aval : num / den));
        }
        crd::containers::Array<crd::usize> cand(alloc);
        for (crd::usize gi = 0; gi < ngrid; ++gi)
        {
            const bool le = (gi == 0) || (grid[gi] < grid[gi - 1]);
            const bool re = (gi == ngrid - 1) || (grid[gi + 1] < grid[gi]);
            if (le || re || (err[gi] > err[gi - 1] && err[gi] >= err[gi + 1]) ||
                (err[gi] < err[gi - 1] && err[gi] <= err[gi + 1]))
            {
                cand.push_back(gi);
            }
        }
        crd::containers::Array<crd::usize> alt(alloc);
        crd::usize ci = 0;
        while (ci < cand.size())
        {
            crd::usize best = cand[ci];
            const T s0 = (err[cand[ci]] >= T(0)) ? T(1) : T(-1);
            crd::usize cj = ci;
            while (cj < cand.size() && ((err[cand[cj]] >= T(0)) ? T(1) : T(-1)) == s0)
            {
                if (std::abs(err[cand[cj]]) > std::abs(err[best]))
                {
                    best = cand[cj];
                }
                ++cj;
            }
            alt.push_back(best);
            ci = cj;
        }
        while (alt.size() > next)
        {
            if (std::abs(err[alt[0]]) <= std::abs(err[alt[alt.size() - 1]]))
            {
                alt.erase(0);
            }
            else
            {
                alt.erase(alt.size() - 1);
            }
        }
        bool same = (alt.size() == next);
        for (crd::usize i = 0; same && i < next; ++i)
        {
            same = (alt[i] == ext[i]);
        }
        if (alt.size() == next)
        {
            for (crd::usize i = 0; i < next; ++i)
            {
                ext[i] = alt[i];
            }
        }
        if (same)
        {
            break;
        }
    }
    // recover P's cosine coefficients b[0..M-1]: solve P(w_k) = A(w_k)/sin(w_k) at M points, A by barycentric.
    for (crd::usize i = 0; i < next; ++i)
    {
        x[i] = cosg(ext[i]);
    }
    for (crd::usize i = 0; i < next; ++i)
    {
        T p = T(1);
        for (crd::usize j = 0; j < next; ++j)
        {
            if (j != i)
            {
                p *= (x[i] - x[j]);
            }
        }
        ad[i] = T(1) / p;
    }
    {
        T numer = T(0), denom = T(0), sign = T(1);
        for (crd::usize i = 0; i < next; ++i)
        {
            numer += ad[i] * gdes[ext[i]];
            denom += sign * ad[i] / gwt[ext[i]];
            sign = -sign;
        }
        const T delta = numer / denom;
        sign = T(1);
        for (crd::usize i = 0; i < next; ++i)
        {
            y[i] = gdes[ext[i]] - sign * delta / gwt[ext[i]];
            sign = -sign;
        }
    }
    auto Peval = [&](T w) -> T // P(w) = the barycentric interpolant (already = A/sin, i.e. gdes scale)
    {
        const T xf = std::cos(w);
        T num = T(0), den = T(0);
        for (crd::usize j = 0; j < next; ++j)
        {
            const T d = xf - x[j];
            if (std::abs(d) < static_cast<T>(1e-12))
            {
                return y[j];
            }
            const T cc = ad[j] / d;
            den += cc;
            num += cc * y[j];
        }
        return num / den;
    };
    const crd::usize dim = m; // M cosine coefficients
    dense::Matrix<T> cmat(alloc, dim, dim);
    crd::containers::Array<T> rhs(alloc);
    rhs.resize(dim);
    for (crd::usize k = 0; k < dim; ++k)
    {
        const T wk = pi * static_cast<T>(k) / static_cast<T>(dim == 1 ? 1 : dim - 1) * static_cast<T>(0.5) +
                     static_cast<T>(0.25) * pi; // interior points (avoid w=0,π where sin=0)
        for (crd::usize nn = 0; nn < dim; ++nn)
        {
            cmat(k, nn) = std::cos(static_cast<T>(nn) * wk);
        }
        rhs[k] = Peval(wk);
    }
    dense::LU<T> lu(alloc, dim);
    dense::factor_lu<T, dense::Layout::RowMajor>(lu, cmat);
    dense::solve_lu<T, dense::Layout::RowMajor>(lu, crd::containers::Span<T>(rhs.data(), dim)); // rhs ← b[0..M-1]
    // convert P → A = sin(w)·P → sine coefficients c[n], then the antisymmetric impulse response.
    crd::containers::Array<T> c(alloc);
    c.resize(m + 1);
    for (crd::usize n = 0; n <= m; ++n)
    {
        c[n] = T(0);
    }
    auto bget = [&](crd::usize idx) -> T { return (idx < m) ? rhs[idx] : T(0); };
    for (crd::usize n = 1; n <= m; ++n)
    {
        c[n] = T(0.5) * bget(n - 1) - T(0.5) * bget(n + 1);
    }
    c[1] = bget(0) - T(0.5) * bget(2); // m=0 contributes b[0] (not 0.5·b[0]) to c[1]
    crd::containers::Array<T> h(alloc);
    h.resize(numtaps);
    h[m] = T(0); // centre of an antisymmetric type-III filter is 0
    for (crd::usize n = 1; n <= m; ++n)
    {
        h[m - n] = c[n] / T(2);
        h[m + n] = -c[n] / T(2);
    }
    return h;
}
} // namespace detail

// remez Hilbert transformer (type III, odd numtaps): |H(w)| ≈ 1 over [f1, f2], 90° phase. Antisymmetric, h[centre]=0.
template <typename T>
[[nodiscard]] crd::containers::Array<T> remez_hilbert(crd::memory::IAllocator* alloc, crd::usize numtaps, T f1, T f2,
                                                      crd::usize grid_density = 16)
{
    const crd::usize m = (numtaps - 1) / 2;
    const T two_pi = static_cast<T>(2.0 * std::numbers::pi_v<double>);
    crd::containers::Array<T> grid(alloc), gdes(alloc), gwt(alloc);
    const T delf = T(0.5) / static_cast<T>(grid_density * m);
    for (T f = f1; f <= f2 + delf * T(0.5); f += delf)
    {
        const T ff = (f > f2) ? f2 : f;
        const T sw = std::sin(two_pi * ff);
        grid.push_back(ff);
        gdes.push_back(T(1) / sw); // D = 1 ⇒ D/sin
        gwt.push_back(sw);         // W = 1 ⇒ W·sin
        if (f >= f2)
        {
            break;
        }
    }
    return detail::remez_type3<T>(alloc, numtaps, crd::containers::ConstSpan<T>(grid.data(), grid.size()),
                                  crd::containers::ConstSpan<T>(gdes.data(), gdes.size()),
                                  crd::containers::ConstSpan<T>(gwt.data(), gwt.size()));
}

// remez differentiator (type III, odd numtaps): |H(w)| ≈ 2π·f over [0, fmax], a 90° phase derivative.
template <typename T>
[[nodiscard]] crd::containers::Array<T> remez_differentiator(crd::memory::IAllocator* alloc, crd::usize numtaps, T fmax,
                                                             crd::usize grid_density = 16)
{
    const crd::usize m = (numtaps - 1) / 2;
    const T two_pi = static_cast<T>(2.0 * std::numbers::pi_v<double>);
    crd::containers::Array<T> grid(alloc), gdes(alloc), gwt(alloc);
    const T delf = T(0.5) / static_cast<T>(grid_density * m);
    const T f0 = delf; // exclude exactly 0 (sin = 0)
    for (T f = f0; f <= fmax + delf * T(0.5); f += delf)
    {
        const T ff = (f > fmax) ? fmax : f;
        const T sw = std::sin(two_pi * ff);
        grid.push_back(ff);
        gdes.push_back(two_pi * ff / sw);          // D(f) = 2π·f ⇒ D/sin
        gwt.push_back(sw / (two_pi * ff));         // relative-error weight W = 1/D ⇒ W·sin
        if (f >= fmax)
        {
            break;
        }
    }
    return detail::remez_type3<T>(alloc, numtaps, crd::containers::ConstSpan<T>(grid.data(), grid.size()),
                                  crd::containers::ConstSpan<T>(gdes.data(), gdes.size()),
                                  crd::containers::ConstSpan<T>(gwt.data(), gwt.size()));
}

} // namespace crd::hesap::dsp
