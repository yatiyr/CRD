#pragma once

// ---------------------------------------------------------------------------
// crd-hesap-wavelet v11w-c — stationary (undecimated) wavelet transform.
//
//   swt    multilevel SWT (algorithme à trous): no downsampling; the filters
//          are dilated by 2^(j-1) at level j. Every level keeps length N ⇒
//          shift-invariant (the redundant transform used for denoising).
//   iswt   inverse SWT (pywt's phase-averaging reconstruction).
//
// Convention pinned vs PyWavelets (probed): level-j approximation
//   cA_j[i] = Σ_k dec_lo[k] · cA_{j-1}[(i + 2^(j-1)·(F/2) - k·2^(j-1)) mod N]
// periodic, no decimation. Output ordered COARSE-FIRST [(cA_n,cD_n)…(cA_1,cD_1)]
// (drop-in with pywt.swt / pywt.iswt). Requires N divisible by 2^level.
//
// Gate (ADR-0093): per-level coefficients vs pywt + iswt(swt(x))==x (perfect
// reconstruction, self-contained) + vs pywt.iswt + run-twice bit-identical.
// ---------------------------------------------------------------------------

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/wavelet/dwt.hpp>
#include <crd/hesap/wavelet/families.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::hesap::wavelet
{

// One SWT level: approximation + detail, both length N (undecimated).
template <typename T> struct SwtLevel
{
    crd::containers::Array<T> cA;
    crd::containers::Array<T> cD;
    explicit SwtLevel(crd::memory::IAllocator* a) : cA(a), cD(a) {}
};

namespace detail
{

// à trous periodic convolution: out[i] = Σ_k filt[k]·cur[(i + offset - k·dil) mod n]. `rfilt` is reversed
// (rfilt[j]=filt[F-1-j]) so the support reads cur[base + j·dil] ascending; interior runs mod-free.
template <typename T>
void atrous_convolution(const T* cur, crd::usize n, crd::containers::ConstSpan<T> rfilt, crd::isize dil,
                        crd::isize offset, T* out) noexcept
{
    const crd::isize ni = static_cast<crd::isize>(n);
    const crd::isize f = static_cast<crd::isize>(rfilt.size());
    const T* rf = rfilt.data();
    const crd::isize span = (f - 1) * dil;
    for (crd::isize i = 0; i < ni; ++i)
    {
        const crd::isize base = i + offset - span; // index of rfilt[0]
        T acc = T(0);
        if (base >= 0 && base + span < ni) // whole dilated window inside [0,n): mod-free
        {
            for (crd::isize j = 0; j < f; ++j)
            {
                acc += rf[j] * cur[base + j * dil];
            }
        }
        else
        {
            for (crd::isize j = 0; j < f; ++j)
            {
                crd::isize idx = (base + j * dil) % ni;
                if (idx < 0)
                {
                    idx += ni;
                }
                acc += rf[j] * cur[idx];
            }
        }
        out[static_cast<crd::usize>(i)] = acc;
    }
}

} // namespace detail

// Multilevel SWT. Returns COARSE-FIRST levels [(cA_n,cD_n), ..., (cA_1,cD_1)] (pywt order). n must be a multiple
// of 2^level.
template <typename T>
[[nodiscard]] crd::containers::Array<SwtLevel<T>> swt(crd::memory::IAllocator* alloc,
                                                      crd::containers::ConstSpan<T> x, const Wavelet& w,
                                                      crd::usize level)
{
    const crd::usize n = x.size();
    const crd::usize f = w.len();
    crd::containers::Array<T> rlo(alloc), rhi(alloc);
    rlo.resize(f);
    rhi.resize(f);
    for (crd::usize k = 0; k < f; ++k)
    {
        rlo[k] = static_cast<T>(w.dec_lo[f - 1 - k]);
        rhi[k] = static_cast<T>(w.dec_hi[f - 1 - k]);
    }
    const crd::containers::ConstSpan<T> rlos(rlo.data(), f), rhis(rhi.data(), f);

    crd::containers::Array<SwtLevel<T>> fine_first(alloc); // build [L1..Llevel] then reverse to coarse-first
    fine_first.reserve(level);
    crd::containers::Array<T> cur(alloc);
    cur.resize(n);
    for (crd::usize i = 0; i < n; ++i)
    {
        cur[i] = x[i];
    }
    for (crd::usize j = 1; j <= level; ++j)
    {
        const crd::isize dil = static_cast<crd::isize>(crd::usize{1} << (j - 1));
        const crd::isize offset = dil * static_cast<crd::isize>(f / 2);
        SwtLevel<T> lvl(alloc);
        lvl.cA.resize(n);
        lvl.cD.resize(n);
        detail::atrous_convolution<T>(cur.data(), n, rlos, dil, offset, lvl.cA.data());
        detail::atrous_convolution<T>(cur.data(), n, rhis, dil, offset, lvl.cD.data());
        for (crd::usize i = 0; i < n; ++i)
        {
            cur[i] = lvl.cA[i];
        }
        fine_first.push_back(std::move(lvl));
    }
    crd::containers::Array<SwtLevel<T>> result(alloc); // coarse-first
    result.reserve(level);
    for (crd::usize j = 0; j < level; ++j)
    {
        result.push_back(std::move(fine_first[level - 1 - j]));
    }
    return result;
}

// Inverse SWT — pywt's phase-averaging reconstruction. coeffs is coarse-first (the swt() output).
template <typename T>
[[nodiscard]] crd::containers::Array<T> iswt(crd::memory::IAllocator* alloc,
                                             const crd::containers::Array<SwtLevel<T>>& coeffs, const Wavelet& w)
{
    const crd::usize num_levels = coeffs.size();
    const crd::usize n = coeffs[0].cA.size();
    crd::containers::Array<T> output(alloc);
    output.resize(n);
    for (crd::usize i = 0; i < n; ++i)
    {
        output[i] = coeffs[0].cA[i]; // coarsest approximation
    }
    crd::containers::Array<T> ca_sub(alloc), cd_sub(alloc), x1(alloc), x2(alloc);
    for (crd::usize jj = num_levels; jj >= 1; --jj)
    {
        const crd::usize step = crd::usize{1} << (jj - 1);
        const crd::containers::Array<T>& cD = coeffs[num_levels - jj].cD; // detail at this level
        for (crd::usize first = 0; first < step; ++first)
        {
            // indices = first, first+step, ... ; even = indices[0::2], odd = indices[1::2].
            crd::usize cnt = 0;
            for (crd::usize idx = first; idx < n; idx += step)
            {
                ++cnt;
            }
            const crd::usize ke = (cnt + 1) / 2; // even count
            const crd::usize ko = cnt / 2;       // odd count
            ca_sub.resize(ke > ko ? ke : ko);
            cd_sub.resize(ke > ko ? ke : ko);
            // even half
            {
                crd::usize m = 0;
                for (crd::usize p = 0; p < cnt; p += 2)
                {
                    const crd::usize idx = first + p * step;
                    ca_sub[m] = output[idx];
                    cd_sub[m] = cD[idx];
                    ++m;
                }
                idwt<T>(alloc, crd::containers::ConstSpan<T>(ca_sub.data(), ke),
                        crd::containers::ConstSpan<T>(cd_sub.data(), ke), w, SignalExtensionMode::Periodization, x1);
            }
            // odd half
            {
                crd::usize m = 0;
                for (crd::usize p = 1; p < cnt; p += 2)
                {
                    const crd::usize idx = first + p * step;
                    ca_sub[m] = output[idx];
                    cd_sub[m] = cD[idx];
                    ++m;
                }
                idwt<T>(alloc, crd::containers::ConstSpan<T>(ca_sub.data(), ko),
                        crd::containers::ConstSpan<T>(cd_sub.data(), ko), w, SignalExtensionMode::Periodization, x2);
            }
            // circular shift x2 right by 1, average, scatter back to indices.
            const crd::usize lx = x1.size(); // == cnt
            for (crd::usize m = 0; m < lx; ++m)
            {
                const T x2r = x2[(m + lx - 1) % lx];
                const crd::usize idx = first + m * step;
                output[idx] = (x1[m] + x2r) / T(2);
            }
        }
        if (jj == 1)
        {
            break; // avoid unsigned underflow
        }
    }
    return output;
}

} // namespace crd::hesap::wavelet
