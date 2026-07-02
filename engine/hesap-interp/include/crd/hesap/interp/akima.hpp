#pragma once

// crd-hesap-interp v13-c (part 1) — Akima + modified-Akima (makima) interpolation.
//
// Local C¹ piecewise-cubic from slope-weighted divided differences: no global ringing, reduced overshoot.
//   akima  — Akima 1970 (the classic; has a 0/0 qualitative discontinuity on flats/equal slopes).
//   makima — Moler/Ionita modified weights (MATLAB R2019b+ `makima`): no spurious oscillation on flats/constant runs;
//            the best general LOCAL interpolant when C² is not required. ★ best-in-class for sensor/robotics LUTs.
// The slope formula REPLICATES scipy.interpolate.Akima1DInterpolator EXACTLY (extended-secant padding + the
// f1/f2 weighting + the break_mult=1e-9 degeneracy fill). Slopes computed once; eval via the cubic Hermite basis.
// Gated ≤1e-12 vs scipy (slopes + eval, both methods).

#include <crd/hesap/interp/piecewise.hpp>

namespace crd::hesap::interp
{

// Akima/makima per-knot slopes into `d` (length n). `mext` = caller-provided scratch (length n+3, the padded secants).
// `makima=true` selects the modified weights. Allocation-free, deterministic. scipy Akima1DInterpolator._init.
template <Real T>
[[nodiscard]] InterpStatus akima_slopes(crd::containers::ConstSpan<T> x, crd::containers::ConstSpan<T> y, bool makima,
                                        crd::containers::Span<T> d, crd::containers::Span<T> mext,
                                        crd::containers::Span<T> f12cache) noexcept
{
    const crd::usize n = x.size();
    if (n < 2 || y.size() != n || d.size() != n || mext.size() != n + 3 || f12cache.size() != n)
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
    const auto sec = [&](crd::usize k)
    {
        return (y[k + 1] - y[k]) / (x[k + 1] - x[k]);
    };
    if (n == 2)
    {
        const T s = sec(0);
        d[0] = s;
        d[1] = s;
        return InterpStatus::Ok;
    }
    const T* CRD_RESTRICT xp = x.data();
    const T* CRD_RESTRICT yp = y.data();
    T* CRD_RESTRICT me = mext.data();
    for (crd::usize k = 0; k + 1 < n; ++k) // the real secants land at mext[2..n] (restrict ⇒ the loop vectorizes)
    {
        me[2 + k] = (yp[k + 1] - yp[k]) / (xp[k + 1] - xp[k]);
    }
    me[1] = static_cast<T>(2) * me[2] - me[3]; // 2-point left extension
    me[0] = static_cast<T>(2) * me[1] - me[2];
    me[n + 1] = static_cast<T>(2) * me[n] - me[n - 1]; // 2-point right extension
    me[n + 2] = static_cast<T>(2) * me[n + 1] - me[n];

    // Single slope pass with f12 cached + the makima branch HOISTED out of the loop + CRD_RESTRICT pointers ⇒ the body
    // auto-vectorizes (vdivpd select + vmaxpd reduction; Boost wins the scalar build only by vectorizing the same way).
    // The `(f12>0)` form computes the slope for all lanes and selects; degenerate lanes (f12≈0) produce a discarded
    // inf, then the cheap override pass fixes the relative-degenerate knots. Bit-identical to the scalar two-pass form.
    // BRANCHLESS so the divide vectorizes (vdivpd): the slope is computed UNCONDITIONALLY (a degenerate knot, f12==0,
    // yields a 0/0 NaN that the branchless override blends away below). gcc reported "control flow in loop" on the
    // former (f12>0?slope:fill) ternary; this form has none ⇒ vdivpd + vmaxpd reduction. Bit-identical: for f12>thresh
    // the slope formula is unchanged; for f12<=thresh the averaged fill replaces it exactly as before.
    // Pure element-wise slope loop (NO reduction, NO branch) ⇒ the divide vectorizes to vdivpd. The max-reduction and
    // the degenerate override are split into their own loops so nothing entangles the divide. Bit-identical to scalar:
    // for f12>thresh the slope formula is unchanged; for f12<=thresh the averaged fill replaces it (degenerate f12==0
    // produces a transient NaN that the override overwrites).
    T* CRD_RESTRICT dp = d.data();
    T* CRD_RESTRICT fc = f12cache.data();
    if (makima)
    {
        for (crd::usize i = 0; i < n; ++i)
        {
            const T f2 = detail::abs_val(me[i + 1] - me[i]) + static_cast<T>(0.5) * detail::abs_val(me[i + 1] + me[i]);
            const T f1 =
                detail::abs_val(me[i + 3] - me[i + 2]) + static_cast<T>(0.5) * detail::abs_val(me[i + 3] + me[i + 2]);
            const T f12 = f1 + f2;
            fc[i] = f12;
            dp[i] = me[i + 1] + (f2 / f12) * (me[i + 2] - me[i + 1]);
        }
    }
    else
    {
        for (crd::usize i = 0; i < n; ++i)
        {
            const T f2 = detail::abs_val(me[i + 1] - me[i]);
            const T f1 = detail::abs_val(me[i + 3] - me[i + 2]);
            const T f12 = f1 + f2;
            fc[i] = f12;
            dp[i] = me[i + 1] + (f2 / f12) * (me[i + 2] - me[i + 1]);
        }
    }
    T maxf12 = static_cast<T>(0);
    for (crd::usize i = 0; i < n; ++i) // pure max-reduction over the cached weights ⇒ vmaxpd
    {
        maxf12 = fc[i] > maxf12 ? fc[i] : maxf12;
    }
    const T thresh = static_cast<T>(1e-9) * maxf12;
    for (crd::usize i = 0; i < n; ++i) // branchless override: relative-degenerate knots take the averaged fill
    {
        const T fill = static_cast<T>(0.5) * (me[i] + me[i + 3]);
        dp[i] = fc[i] <= thresh ? fill : dp[i];
    }
    return InterpStatus::Ok;
}

// Build-once / evaluate-many Akima (or makima) interpolant.
template <Real T> class AkimaInterpolant
{
public:
    explicit AkimaInterpolant(crd::memory::IAllocator* alloc) noexcept : m_d(alloc), m_scratch(alloc), m_f12(alloc) {}

    [[nodiscard]] InterpStatus build(crd::containers::ConstSpan<T> x, crd::containers::ConstSpan<T> y,
                                     bool makima = false)
    {
        const crd::usize n = x.size();
        m_x = x;
        m_y = y;
        m_d.resize(n);
        m_scratch.resize(n + 3);
        m_f12.resize(n);
        m_cache = 0;
        return akima_slopes(x, y, makima, crd::containers::Span<T>{m_d.data(), m_d.size()},
                            crd::containers::Span<T>{m_scratch.data(), m_scratch.size()},
                            crd::containers::Span<T>{m_f12.data(), m_f12.size()});
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
    crd::containers::ConstSpan<T> m_x{};
    crd::containers::ConstSpan<T> m_y{};
    crd::containers::Array<T> m_d;
    crd::containers::Array<T> m_scratch;
    crd::containers::Array<T> m_f12;
    mutable crd::usize m_cache = 0;
};

} // namespace crd::hesap::interp
