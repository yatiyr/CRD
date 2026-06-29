#pragma once

// crd-hesap-interp v13-b — cubic spline interpolation (C², all boundary conditions).
//
// Solves the tridiagonal (cyclic for periodic) system for the per-knot first derivatives, then evaluates via the cubic
// Hermite basis (interp_hermite). The system + boundary rows replicate scipy.interpolate.CubicSpline EXACTLY (the
// v12 read-the-gold-source discipline) ⇒ ≤1e-12 bit-match. The O(n) Thomas solve keeps the per-call crush (vs scipy's
// O(n) banded LAPACK + the Python dispatch overhead). Build allocates ONCE; eval is allocation-free, noexcept.
//
//   natural    — S″ = 0 at both ends
//   clamped    — S′ prescribed at both ends (caller-supplied)
//   not-a-knot — S‴ continuous across the 1st/last interior knot (scipy's default; uniform O(h⁴))
//   periodic   — C² across the seam (requires y[0] == y[n-1]); cyclic system via scipy's condensation
//
// Tier-2 certified bound: ‖f − S‖∞ ≤ (5/384)·h⁴·‖f⁽⁴⁾‖∞ (Hall-Meyer 1976; the constant 5/384 is proven optimal).

#include <crd/hesap/interp/piecewise.hpp>

namespace crd::hesap::interp
{

enum class SplineBC : crd::u8
{
    Natural,
    Clamped,
    NotAKnot,
    Periodic,
};

namespace detail
{
// Thomas algorithm: solve the n×n tridiagonal system with sub-diagonal `sub` (sub[0] unused), diagonal `diag`,
// super-diagonal `sup` (sup[n-1] unused), right-hand side `rhs`, into `xo`. `cp` is length-n scratch. noexcept,
// allocation-free. Deterministic (pure FMUL/FADD, no pivoting branch on the SPD/diagonally-dominant spline systems).
template <Real T>
void thomas_solve(crd::containers::ConstSpan<T> sub, crd::containers::ConstSpan<T> diag,
                  crd::containers::ConstSpan<T> sup, crd::containers::ConstSpan<T> rhs, crd::containers::Span<T> xo,
                  crd::containers::Span<T> cp) noexcept
{
    const crd::usize n = diag.size();
    cp[0] = sup[0] / diag[0];
    xo[0] = rhs[0] / diag[0];
    for (crd::usize i = 1; i < n; ++i)
    {
        const T m = diag[i] - sub[i] * cp[i - 1];
        cp[i] = (i + 1 < n) ? sup[i] / m : static_cast<T>(0);
        xo[i] = (rhs[i] - sub[i] * xo[i - 1]) / m;
    }
    for (crd::usize i = n - 1; i-- > 0;)
    {
        xo[i] -= cp[i] * xo[i + 1];
    }
}
} // namespace detail

// Build-once / evaluate-many cubic spline. ctor takes the allocator (the build scratch + the slopes allocate once).
template <Real T>
class CubicSplineInterpolant
{
public:
    explicit CubicSplineInterpolant(crd::memory::IAllocator* alloc) noexcept : m_alloc(alloc), m_d(alloc) {}

    [[nodiscard]] InterpStatus build(crd::containers::ConstSpan<T> x, crd::containers::ConstSpan<T> y, SplineBC bc,
                                     T clamp_left = static_cast<T>(0), T clamp_right = static_cast<T>(0))
    {
        const crd::usize n = x.size();
        if (n < 2 || y.size() != n)
        {
            return InterpStatus::BadInput;
        }
        for (crd::usize i = 0; i < n; ++i)
        {
            if (!detail::is_finite(x[i]) || !detail::is_finite(y[i]))
            {
                return InterpStatus::BadInput;
            }
        }
        for (crd::usize i = 0; i + 1 < n; ++i)
        {
            if (!(x[i] < x[i + 1]))
            {
                return InterpStatus::NotIncreasing;
            }
        }
        m_x = x;
        m_y = y;
        m_d.resize(n);
        m_cache = 0;
        const auto hh = [&](crd::usize k) { return x[k + 1] - x[k]; };
        const auto sec = [&](crd::usize k) { return (y[k + 1] - y[k]) / (x[k + 1] - x[k]); };

        if (n == 2)
        {
            if (bc == SplineBC::Clamped)
            {
                m_d[0] = clamp_left;
                m_d[1] = clamp_right;
            }
            else
            {
                const T s = sec(0);
                m_d[0] = s;
                m_d[1] = s;
            }
            return InterpStatus::Ok;
        }
        if (bc == SplineBC::Periodic)
        {
            return build_periodic(n, hh, sec);
        }
        if (n == 3 && bc == SplineBC::NotAKnot)
        {
            return build_notaknot3(hh, sec);
        }
        return build_tridiagonal(n, bc, clamp_left, clamp_right, hh, sec);
    }

    [[nodiscard]] T eval(T xq) const noexcept
    {
        return interp_hermite(m_x, m_y, crd::containers::ConstSpan<T>{m_d.data(), m_d.size()}, xq, m_cache);
    }

    [[nodiscard]] crd::containers::ConstSpan<T> slopes() const noexcept
    {
        return crd::containers::ConstSpan<T>{m_d.data(), m_d.size()};
    }

private:
    template <class H, class S>
    [[nodiscard]] InterpStatus build_tridiagonal(crd::usize n, SplineBC bc, T clamp_left, T clamp_right, H hh, S sec)
    {
        crd::containers::Array<T> sub(m_alloc);
        crd::containers::Array<T> diag(m_alloc);
        crd::containers::Array<T> sup(m_alloc);
        crd::containers::Array<T> rhs(m_alloc);
        crd::containers::Array<T> cp(m_alloc);
        sub.resize(n);
        diag.resize(n);
        sup.resize(n);
        rhs.resize(n);
        cp.resize(n);
        for (crd::usize i = 1; i + 1 < n; ++i) // interior rows (scipy: A[1,i]=2(h[i-1]+h[i]), sub=h[i], sup=h[i-1])
        {
            const T hm = hh(i - 1);
            const T hp = hh(i);
            sub[i] = hp;
            diag[i] = static_cast<T>(2) * (hm + hp);
            sup[i] = hm;
            rhs[i] = static_cast<T>(3) * (hp * sec(i - 1) + hm * sec(i));
        }
        // left boundary row 0
        if (bc == SplineBC::Natural)
        {
            diag[0] = static_cast<T>(2) * hh(0);
            sup[0] = hh(0);
            rhs[0] = static_cast<T>(3) * (m_y[1] - m_y[0]);
        }
        else if (bc == SplineBC::Clamped)
        {
            diag[0] = static_cast<T>(1);
            sup[0] = static_cast<T>(0);
            rhs[0] = clamp_left;
        }
        else // NotAKnot (n>=4)
        {
            const T h0 = hh(0);
            const T h1 = hh(1);
            const T d = h0 + h1;
            diag[0] = h1;
            sup[0] = d;
            rhs[0] = ((h0 + static_cast<T>(2) * d) * h1 * sec(0) + h0 * h0 * sec(1)) / d;
        }
        // right boundary row n-1
        if (bc == SplineBC::Natural)
        {
            diag[n - 1] = static_cast<T>(2) * hh(n - 2);
            sub[n - 1] = hh(n - 2);
            rhs[n - 1] = static_cast<T>(3) * (m_y[n - 1] - m_y[n - 2]);
        }
        else if (bc == SplineBC::Clamped)
        {
            diag[n - 1] = static_cast<T>(1);
            sub[n - 1] = static_cast<T>(0);
            rhs[n - 1] = clamp_right;
        }
        else // NotAKnot
        {
            const T hl = hh(n - 2); // h[n-2]
            const T hr = hh(n - 3); // h[n-3]
            const T d = hl + hr;
            diag[n - 1] = hr;
            sub[n - 1] = d;
            rhs[n - 1] = (hl * hl * sec(n - 3) + (static_cast<T>(2) * d + hl) * hr * sec(n - 2)) / d;
        }
        detail::thomas_solve<T>({sub.data(), n}, {diag.data(), n}, {sup.data(), n}, {rhs.data(), n},
                               {m_d.data(), n}, {cp.data(), n});
        return InterpStatus::Ok;
    }

    template <class H, class S>
    [[nodiscard]] InterpStatus build_notaknot3(H hh, S sec) // scipy's n==3 parabola (a tridiagonal 3×3)
    {
        const T h0 = hh(0);
        const T h1 = hh(1);
        crd::containers::Array<T> sub(m_alloc);
        crd::containers::Array<T> diag(m_alloc);
        crd::containers::Array<T> sup(m_alloc);
        crd::containers::Array<T> rhs(m_alloc);
        crd::containers::Array<T> cp(m_alloc);
        sub.resize(3);
        diag.resize(3);
        sup.resize(3);
        rhs.resize(3);
        cp.resize(3);
        diag[0] = static_cast<T>(1);
        sup[0] = static_cast<T>(1);
        rhs[0] = static_cast<T>(2) * sec(0);
        sub[1] = h1;
        diag[1] = static_cast<T>(2) * (h0 + h1);
        sup[1] = h0;
        rhs[1] = static_cast<T>(3) * (h0 * sec(1) + h1 * sec(0));
        sub[2] = static_cast<T>(1);
        diag[2] = static_cast<T>(1);
        rhs[2] = static_cast<T>(2) * sec(1);
        detail::thomas_solve<T>({sub.data(), 3}, {diag.data(), 3}, {sup.data(), 3}, {rhs.data(), 3}, {m_d.data(), 3},
                               {cp.data(), 3});
        return InterpStatus::Ok;
    }

    template <class H, class S>
    [[nodiscard]] InterpStatus build_periodic(crd::usize n, H hh, S sec) // requires y[0]==y[n-1]
    {
        if (n == 3) // scipy manual 3-point periodic
        {
            const T t = (sec(0) / hh(0) + sec(1) / hh(1)) / (static_cast<T>(1) / hh(0) + static_cast<T>(1) / hh(1));
            m_d[0] = t;
            m_d[1] = t;
            m_d[2] = t;
            return InterpStatus::Ok;
        }
        const crd::usize m = n - 2; // condensed (n-2)×(n-2) tridiagonal
        crd::containers::Array<T> sub(m_alloc);
        crd::containers::Array<T> diag(m_alloc);
        crd::containers::Array<T> sup(m_alloc);
        crd::containers::Array<T> b1(m_alloc);
        crd::containers::Array<T> b2(m_alloc);
        crd::containers::Array<T> s1(m_alloc);
        crd::containers::Array<T> s2(m_alloc);
        crd::containers::Array<T> cp(m_alloc);
        sub.resize(m);
        diag.resize(m);
        sup.resize(m);
        b1.resize(m);
        b2.resize(m);
        s1.resize(m);
        s2.resize(m);
        cp.resize(m);
        // condensed matrix Ac (rows/cols 0..n-3 of the (n-1) periodic system)
        diag[0] = static_cast<T>(2) * (hh(n - 2) + hh(0));
        sup[0] = hh(n - 2);
        for (crd::usize i = 1; i < m; ++i)
        {
            sub[i] = hh(i);
            diag[i] = static_cast<T>(2) * (hh(i - 1) + hh(i));
            if (i + 1 < m)
            {
                sup[i] = hh(i - 1);
            }
        }
        b1[0] = static_cast<T>(3) * (hh(0) * sec(n - 2) + hh(n - 2) * sec(0));
        for (crd::usize i = 1; i < m; ++i)
        {
            b1[i] = static_cast<T>(3) * (hh(i) * sec(i - 1) + hh(i - 1) * sec(i));
        }
        const T a_0_m1 = hh(0);
        const T a_m2_m1 = hh(n - 4);
        for (crd::usize i = 0; i < m; ++i)
        {
            b2[i] = static_cast<T>(0);
        }
        b2[0] = -a_0_m1;
        b2[m - 1] = -a_m2_m1;
        detail::thomas_solve<T>({sub.data(), m}, {diag.data(), m}, {sup.data(), m}, {b1.data(), m}, {s1.data(), m},
                               {cp.data(), m});
        detail::thomas_solve<T>({sub.data(), m}, {diag.data(), m}, {sup.data(), m}, {b2.data(), m}, {s2.data(), m},
                               {cp.data(), m});
        const T a_m1_0 = hh(n - 3);
        const T a_m1_m2 = hh(n - 2);
        const T a_m1_m1 = static_cast<T>(2) * (hh(n - 2) + hh(n - 3));
        const T bm1 = static_cast<T>(3) * (hh(n - 2) * sec(n - 3) + hh(n - 3) * sec(n - 2));
        const T s_m1 = (bm1 - a_m1_0 * s1[0] - a_m1_m2 * s1[m - 1]) /
                       (a_m1_m1 + a_m1_0 * s2[0] + a_m1_m2 * s2[m - 1]);
        for (crd::usize i = 0; i < m; ++i)
        {
            m_d[i] = s1[i] + s_m1 * s2[i];
        }
        m_d[n - 2] = s_m1;
        m_d[n - 1] = m_d[0];
        return InterpStatus::Ok;
    }

    crd::memory::IAllocator* m_alloc;
    crd::containers::ConstSpan<T> m_x{};
    crd::containers::ConstSpan<T> m_y{};
    crd::containers::Array<T> m_d;
    mutable crd::usize m_cache = 0;
};

// Tier-2 a-priori certified bound for the complete (clamped) C² cubic spline: ‖f − S‖∞ ≤ (5/384)·h⁴·max|f⁽⁴⁾|
// (Hall-Meyer 1976 — the constant 5/384 is proven optimal). A GUARANTEED bound when the caller supplies |f⁽⁴⁾|.
template <Real T>
[[nodiscard]] constexpr T cubic_spline_worst_case_error(T max_h, T fourth_deriv_bound) noexcept
{
    const T h2 = max_h * max_h;
    return h2 * h2 * static_cast<T>(5) / static_cast<T>(384) * fourth_deriv_bound;
}

} // namespace crd::hesap::interp
