#pragma once

// crd-hesap-interp v13-d (part 2) — rational (Padé) approximation from a Taylor series.
//
//   RationalPade — the [numer/denom] Padé approximant of a power series: solves the linear system [A | B]·[p;q] = c
//     (REPLICATING scipy.interpolate.pade) over the shipped dense least-squares (SANITY 8). Captures poles/asymptotes
//     a polynomial can't. ⚠ a Padé can have a SPURIOUS pole inside the interval — `min_denominator_abs` is the guard a
//     safety case must check (per ADR-0095's no-bare-number contract).
//
// Eval is deterministic (Horner, pure FMUL/FADD). Gated ≤1e-10 vs scipy.interpolate.pade.

#include <crd/hesap/interp/piecewise.hpp>

#include <crd/hesap/dense/lstsq.hpp>
#include <crd/hesap/dense/matrix.hpp>
#include <crd/hesap/dense/vector.hpp>

namespace crd::hesap::interp
{

template <Real T>
class RationalPade
{
public:
    explicit RationalPade(crd::memory::IAllocator* alloc) noexcept : m_alloc(alloc), m_p(alloc), m_q(alloc) {}

    // Taylor coefficients c[0..]; denom = order of q, numer = order of p (need c.size() ≥ denom+numer+1).
    [[nodiscard]] InterpStatus build(crd::containers::ConstSpan<T> c, crd::usize denom, crd::usize numer)
    {
        const crd::usize big_n = denom + numer;
        if (c.size() < big_n + 1)
        {
            return InterpStatus::BadInput;
        }
        for (crd::usize i = 0; i <= big_n; ++i)
        {
            if (!detail::is_finite(c[i]))
            {
                return InterpStatus::BadInput;
            }
        }
        const crd::usize dim = big_n + 1;
        crd::hesap::dense::Matrix<T> mat(m_alloc, dim, dim); // zero-initialized
        crd::hesap::dense::Vector<T> rhs(m_alloc, crd::containers::ConstSpan<T>{c.data(), dim});
        for (crd::usize k = 0; k <= numer; ++k) // A = eye(dim, numer+1): the p columns
        {
            mat.at(k, k) = static_cast<T>(1);
        }
        for (crd::usize row = 1; row <= big_n; ++row) // B: the q-convolution columns (numer+1 .. dim-1)
        {
            const crd::usize jmax = (row < denom) ? row : denom; // min(row, denom)
            for (crd::usize j = 0; j < jmax; ++j)
            {
                mat.at(row, numer + 1 + j) = -c[row - 1 - j];
            }
        }
        auto ls = crd::hesap::dense::lstsq(m_alloc, mat, rhs);
        m_p.resize(numer + 1);
        m_q.resize(denom + 1);
        for (crd::usize i = 0; i <= numer; ++i)
        {
            m_p[i] = ls.x.at(i, 0);
        }
        m_q[0] = static_cast<T>(1);
        for (crd::usize i = 1; i <= denom; ++i)
        {
            m_q[i] = ls.x.at(numer + i, 0);
        }
        return InterpStatus::Ok;
    }

    [[nodiscard]] T denominator(T x) const noexcept
    {
        const crd::usize m = m_q.size() - 1;
        T qv = m_q[m];
        for (crd::usize i = m; i-- > 0;)
        {
            qv = qv * x + m_q[i];
        }
        return qv;
    }

    [[nodiscard]] T eval(T x) const noexcept
    {
        const crd::usize nn = m_p.size() - 1;
        T pv = m_p[nn];
        for (crd::usize i = nn; i-- > 0;)
        {
            pv = pv * x + m_p[i];
        }
        return pv / denominator(x);
    }

    // Spurious-pole guard: the smallest |q(x)| over a uniform sampling of [lo,hi]. Near zero ⇒ a pole in the interval.
    [[nodiscard]] T min_denominator_abs(T lo, T hi, crd::usize samples) const noexcept
    {
        T mn = detail::abs_(denominator(lo));
        for (crd::usize k = 1; k <= samples; ++k)
        {
            const T x = lo + (hi - lo) * static_cast<T>(k) / static_cast<T>(samples);
            const T a = detail::abs_(denominator(x));
            if (a < mn)
            {
                mn = a;
            }
        }
        return mn;
    }

private:
    crd::memory::IAllocator* m_alloc;
    crd::containers::Array<T> m_p; // numerator coeffs, low→high
    crd::containers::Array<T> m_q; // denominator coeffs, low→high (q[0]==1)
};

} // namespace crd::hesap::interp
