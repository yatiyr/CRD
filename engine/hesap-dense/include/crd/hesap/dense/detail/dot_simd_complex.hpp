#pragma once

#include <crd/core/types.hpp>
#include <crd/hesap/dense/detail/dot_simd.hpp>
#include <crd/math/simd/vec4d.hpp>
#include <crd/math/simd/vec8f.hpp>

#include <type_traits>

// -----------------------------------------------------------------------
// Phase 3.1.6 v3d-2c-1 — fused complex SIMD primitives over the two-real-array
// (re, im) representation. A complex operation expressed as 4 separate real
// `simd_dot`/`simd_axpy` passes reads each operand row TWICE; these fuse the
// passes so every operand row is read ONCE, halving memory traffic in the
// O(n³) complex reductions (Hessenberg `zgehd2`, later the `zlahqr` bulge).
//
// Bit-stability: the fused forms preserve the exact per-element accumulation
// order of the separate-pass versions —
//   simd_cdot_nc keeps 4 independent accumulators (ar·vr, ai·vi, ar·vi, ai·vr)
//   then combines, identical to `simd_dot(ar,vr) − simd_dot(ai,vi)` etc.;
//   the axpy kernels chain the same two FMAs the two `simd_axpy` calls did.
// So results are bit-identical to the unfused path, just faster. FMA is
// single-rounded (the hesap convention), deterministic across widths.
// -----------------------------------------------------------------------

namespace crd::hesap::dense::detail
{
// simd_cdot_nc — complex dot WITHOUT conjugation: (ur, ui) = Σ (ar+i·ai)·(vr+i·vi)
//   ur = Σ ar·vr − ai·vi ;  ui = Σ ar·vi + ai·vr.   One pass over all four arrays.
template <typename R>
inline void simd_cdot_nc(const R* ar, const R* ai, const R* vr, const R* vi, crd::usize n, R& ur_out,
                         R& ui_out) noexcept
{
    namespace simd = crd::math::simd;
    crd::usize p = 0;
    R acc_arvr{0};
    R acc_aivi{0};
    R acc_arvi{0};
    R acc_aivr{0};
    if constexpr (std::is_same_v<R, crd::f64>)
    {
        // 8-wide (2× Vec4d per product) → 8 independent FMA accumulators in
        // flight, saturating the AVX2 FMA ports (latency-bound otherwise).
        simd::Vec4d s_arvr0 = simd::Vec4d::zero();
        simd::Vec4d s_arvr1 = simd::Vec4d::zero();
        simd::Vec4d s_aivi0 = simd::Vec4d::zero();
        simd::Vec4d s_aivi1 = simd::Vec4d::zero();
        simd::Vec4d s_arvi0 = simd::Vec4d::zero();
        simd::Vec4d s_arvi1 = simd::Vec4d::zero();
        simd::Vec4d s_aivr0 = simd::Vec4d::zero();
        simd::Vec4d s_aivr1 = simd::Vec4d::zero();
        for (; p + 8 <= n; p += 8)
        {
            const simd::Vec4d a0 = simd::Vec4d::load(ar + p);
            const simd::Vec4d a1 = simd::Vec4d::load(ar + p + 4);
            const simd::Vec4d b0 = simd::Vec4d::load(ai + p);
            const simd::Vec4d b1 = simd::Vec4d::load(ai + p + 4);
            const simd::Vec4d c0 = simd::Vec4d::load(vr + p);
            const simd::Vec4d c1 = simd::Vec4d::load(vr + p + 4);
            const simd::Vec4d d0 = simd::Vec4d::load(vi + p);
            const simd::Vec4d d1 = simd::Vec4d::load(vi + p + 4);
            s_arvr0 = simd::fma(a0, c0, s_arvr0);
            s_arvr1 = simd::fma(a1, c1, s_arvr1);
            s_aivi0 = simd::fma(b0, d0, s_aivi0);
            s_aivi1 = simd::fma(b1, d1, s_aivi1);
            s_arvi0 = simd::fma(a0, d0, s_arvi0);
            s_arvi1 = simd::fma(a1, d1, s_arvi1);
            s_aivr0 = simd::fma(b0, c0, s_aivr0);
            s_aivr1 = simd::fma(b1, c1, s_aivr1);
        }
        acc_arvr = simd::horizontal_sum(s_arvr0 + s_arvr1);
        acc_aivi = simd::horizontal_sum(s_aivi0 + s_aivi1);
        acc_arvi = simd::horizontal_sum(s_arvi0 + s_arvi1);
        acc_aivr = simd::horizontal_sum(s_aivr0 + s_aivr1);
    }
    else if constexpr (std::is_same_v<R, crd::f32>)
    {
        simd::Vec8f s_arvr = simd::Vec8f::zero();
        simd::Vec8f s_aivi = simd::Vec8f::zero();
        simd::Vec8f s_arvi = simd::Vec8f::zero();
        simd::Vec8f s_aivr = simd::Vec8f::zero();
        for (; p + 8 <= n; p += 8)
        {
            const simd::Vec8f a = simd::Vec8f::load(ar + p);
            const simd::Vec8f b = simd::Vec8f::load(ai + p);
            const simd::Vec8f c = simd::Vec8f::load(vr + p);
            const simd::Vec8f d = simd::Vec8f::load(vi + p);
            s_arvr = simd::fma(a, c, s_arvr);
            s_aivi = simd::fma(b, d, s_aivi);
            s_arvi = simd::fma(a, d, s_arvi);
            s_aivr = simd::fma(b, c, s_aivr);
        }
        acc_arvr = simd::horizontal_sum(s_arvr);
        acc_aivi = simd::horizontal_sum(s_aivi);
        acc_arvi = simd::horizontal_sum(s_arvi);
        acc_aivr = simd::horizontal_sum(s_aivr);
    }
    for (; p < n; ++p)
    {
        acc_arvr += ar[p] * vr[p];
        acc_aivi += ai[p] * vi[p];
        acc_arvi += ar[p] * vi[p];
        acc_aivr += ai[p] * vr[p];
    }
    ur_out = acc_arvr - acc_aivi;
    ui_out = acc_arvi + acc_aivr;
}

// simd_caxpy — complex axpy: y += s·x, one pass.
//   yr += sr·xr − si·xi ;  yi += sr·xi + si·xr.
template <typename R>
inline void simd_caxpy(R* yr, R* yi, R sr, R si, const R* xr, const R* xi, crd::usize n) noexcept
{
    namespace simd = crd::math::simd;
    crd::usize p = 0;
    if constexpr (std::is_same_v<R, crd::f64>)
    {
        const simd::Vec4d vsr(sr);
        const simd::Vec4d vsi(si);
        const simd::Vec4d vnsi = -vsi;
        for (; p + 8 <= n; p += 8)  // 8-wide (2× Vec4d) for FMA-port ILP
        {
            simd::Vec4d yr0 = simd::Vec4d::load(yr + p);
            simd::Vec4d yr1 = simd::Vec4d::load(yr + p + 4);
            simd::Vec4d yi0 = simd::Vec4d::load(yi + p);
            simd::Vec4d yi1 = simd::Vec4d::load(yi + p + 4);
            const simd::Vec4d xr0 = simd::Vec4d::load(xr + p);
            const simd::Vec4d xr1 = simd::Vec4d::load(xr + p + 4);
            const simd::Vec4d xi0 = simd::Vec4d::load(xi + p);
            const simd::Vec4d xi1 = simd::Vec4d::load(xi + p + 4);
            yr0 = simd::fma(vsr, xr0, yr0);
            yr0 = simd::fma(vnsi, xi0, yr0);
            yr1 = simd::fma(vsr, xr1, yr1);
            yr1 = simd::fma(vnsi, xi1, yr1);
            yi0 = simd::fma(vsr, xi0, yi0);
            yi0 = simd::fma(vsi, xr0, yi0);
            yi1 = simd::fma(vsr, xi1, yi1);
            yi1 = simd::fma(vsi, xr1, yi1);
            yr0.store(yr + p);
            yr1.store(yr + p + 4);
            yi0.store(yi + p);
            yi1.store(yi + p + 4);
        }
    }
    else if constexpr (std::is_same_v<R, crd::f32>)
    {
        const simd::Vec8f vsr(sr);
        const simd::Vec8f vsi(si);
        const simd::Vec8f vnsi = -vsi;
        for (; p + 8 <= n; p += 8)
        {
            const simd::Vec8f xrp = simd::Vec8f::load(xr + p);
            const simd::Vec8f xip = simd::Vec8f::load(xi + p);
            simd::Vec8f yrp = simd::Vec8f::load(yr + p);
            simd::Vec8f yip = simd::Vec8f::load(yi + p);
            yrp = simd::fma(vsr, xrp, yrp);
            yrp = simd::fma(vnsi, xip, yrp);
            yip = simd::fma(vsr, xip, yip);
            yip = simd::fma(vsi, xrp, yip);
            yrp.store(yr + p);
            yip.store(yi + p);
        }
    }
    for (; p < n; ++p)
    {
        const R xrp = xr[p];
        const R xip = xi[p];
        yr[p] += sr * xrp - si * xip;
        yi[p] += sr * xip + si * xrp;
    }
}

// simd_caxpy_conjx — complex axpy with a CONJUGATED vector: y += s·conj(x).
//   yr += sr·xr + si·xi ;  yi += −sr·xi + si·xr.
template <typename R>
inline void simd_caxpy_conjx(R* yr, R* yi, R sr, R si, const R* xr, const R* xi, crd::usize n) noexcept
{
    namespace simd = crd::math::simd;
    crd::usize p = 0;
    if constexpr (std::is_same_v<R, crd::f64>)
    {
        const simd::Vec4d vsr(sr);
        const simd::Vec4d vsi(si);
        const simd::Vec4d vnsr = -vsr;
        for (; p + 8 <= n; p += 8)  // 8-wide (2× Vec4d) for FMA-port ILP
        {
            simd::Vec4d yr0 = simd::Vec4d::load(yr + p);
            simd::Vec4d yr1 = simd::Vec4d::load(yr + p + 4);
            simd::Vec4d yi0 = simd::Vec4d::load(yi + p);
            simd::Vec4d yi1 = simd::Vec4d::load(yi + p + 4);
            const simd::Vec4d xr0 = simd::Vec4d::load(xr + p);
            const simd::Vec4d xr1 = simd::Vec4d::load(xr + p + 4);
            const simd::Vec4d xi0 = simd::Vec4d::load(xi + p);
            const simd::Vec4d xi1 = simd::Vec4d::load(xi + p + 4);
            yr0 = simd::fma(vsr, xr0, yr0);
            yr0 = simd::fma(vsi, xi0, yr0);
            yr1 = simd::fma(vsr, xr1, yr1);
            yr1 = simd::fma(vsi, xi1, yr1);
            yi0 = simd::fma(vnsr, xi0, yi0);
            yi0 = simd::fma(vsi, xr0, yi0);
            yi1 = simd::fma(vnsr, xi1, yi1);
            yi1 = simd::fma(vsi, xr1, yi1);
            yr0.store(yr + p);
            yr1.store(yr + p + 4);
            yi0.store(yi + p);
            yi1.store(yi + p + 4);
        }
    }
    else if constexpr (std::is_same_v<R, crd::f32>)
    {
        const simd::Vec8f vsr(sr);
        const simd::Vec8f vsi(si);
        const simd::Vec8f vnsr = -vsr;
        for (; p + 8 <= n; p += 8)
        {
            const simd::Vec8f xrp = simd::Vec8f::load(xr + p);
            const simd::Vec8f xip = simd::Vec8f::load(xi + p);
            simd::Vec8f yrp = simd::Vec8f::load(yr + p);
            simd::Vec8f yip = simd::Vec8f::load(yi + p);
            yrp = simd::fma(vsr, xrp, yrp);
            yrp = simd::fma(vsi, xip, yrp);
            yip = simd::fma(vnsr, xip, yip);
            yip = simd::fma(vsi, xrp, yip);
            yrp.store(yr + p);
            yip.store(yi + p);
        }
    }
    for (; p < n; ++p)
    {
        const R xrp = xr[p];
        const R xip = xi[p];
        yr[p] += sr * xrp + si * xip;
        yi[p] += -sr * xip + si * xrp;
    }
}

} // namespace crd::hesap::dense::detail
