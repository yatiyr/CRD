#pragma once

// ---------------------------------------------------------------------------
// crd-hesap-wavelet v11w-e — maximal-overlap DWT (MODWT, Percival & Walden).
//
//   modwt    multilevel MODWT: shift-invariant, undecimated, ENERGY-PRESERVING.
//            Filters h̃ = dec_hi/√2, g̃ = dec_lo/√2; level-j à trous (dilate by
//            2^(j-1)), circular, NO offset (the P&W convention; cf. SWT's F/2
//            offset). Returns W_1..W_J detail + V_J smooth, each length N.
//   imodwt   exact inverse (the adjoint +shift pyramid).
//
// Gate (self-contained — pywt has no modwt): perfect reconstruction
// imodwt(modwt(x))==x + the energy partition Σ_j ||W_j||² + ||V_J||² = ||x||²
// (the defining MODWT property) + run-twice bit-identical.
// ---------------------------------------------------------------------------

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/wavelet/families.hpp>
#include <crd/memory/allocator.hpp>

#include <cmath>
#include <numbers>

namespace crd::hesap::wavelet
{

template <typename T> struct ModwtResult
{
    crd::containers::Array<crd::containers::Array<T>> w; // detail W_1..W_J (each length N)
    crd::containers::Array<T> v;                         // smooth V_J (length N)
    crd::usize n = 0, levels = 0;
    explicit ModwtResult(crd::memory::IAllocator* a) : w(a), v(a) {}
};

namespace detail
{
// MODWT forward conv: out[t] = Σ_l f[l]·in[(t - dil·l) mod n]  (circular, no offset).
template <typename T>
void modwt_fwd(const T* in, crd::usize n, crd::containers::ConstSpan<T> f, crd::isize dil, T* out) noexcept
{
    const crd::isize ni = static_cast<crd::isize>(n);
    const crd::isize L = static_cast<crd::isize>(f.size());
    for (crd::isize t = 0; t < ni; ++t)
    {
        T acc = T(0);
        for (crd::isize l = 0; l < L; ++l)
        {
            crd::isize idx = (t - dil * l) % ni;
            if (idx < 0)
            {
                idx += ni;
            }
            acc += f[static_cast<crd::usize>(l)] * in[idx];
        }
        out[static_cast<crd::usize>(t)] = acc;
    }
}

// MODWT inverse conv (adjoint): out[t] += Σ_l f[l]·in[(t + dil·l) mod n].
template <typename T>
void modwt_inv_add(const T* in, crd::usize n, crd::containers::ConstSpan<T> f, crd::isize dil, T* out) noexcept
{
    const crd::isize ni = static_cast<crd::isize>(n);
    const crd::isize L = static_cast<crd::isize>(f.size());
    for (crd::isize t = 0; t < ni; ++t)
    {
        T acc = T(0);
        for (crd::isize l = 0; l < L; ++l)
        {
            crd::isize idx = (t + dil * l) % ni;
            if (idx < 0)
            {
                idx += ni;
            }
            acc += f[static_cast<crd::usize>(l)] * in[idx];
        }
        out[static_cast<crd::usize>(t)] += acc;
    }
}
} // namespace detail

template <typename T>
[[nodiscard]] ModwtResult<T> modwt(crd::memory::IAllocator* alloc, crd::containers::ConstSpan<T> x, const Wavelet& w,
                                   crd::usize level)
{
    const crd::usize n = x.size();
    const crd::usize f = w.len();
    const T inv_sqrt2 = static_cast<T>(1.0 / std::numbers::sqrt2_v<double>);
    crd::containers::Array<T> gt(alloc), ht(alloc); // g̃ = dec_lo/√2, h̃ = dec_hi/√2
    gt.resize(f);
    ht.resize(f);
    for (crd::usize k = 0; k < f; ++k)
    {
        gt[k] = static_cast<T>(w.dec_lo[k]) * inv_sqrt2;
        ht[k] = static_cast<T>(w.dec_hi[k]) * inv_sqrt2;
    }
    const crd::containers::ConstSpan<T> gts(gt.data(), f), hts(ht.data(), f);

    ModwtResult<T> out(alloc);
    out.n = n;
    out.levels = level;
    out.w.reserve(level);
    crd::containers::Array<T> v(alloc);
    v.resize(n);
    for (crd::usize i = 0; i < n; ++i)
    {
        v[i] = x[i];
    }
    for (crd::usize j = 1; j <= level; ++j)
    {
        const crd::isize dil = static_cast<crd::isize>(crd::usize{1} << (j - 1));
        crd::containers::Array<T> wj(alloc), vj(alloc);
        wj.resize(n);
        vj.resize(n);
        detail::modwt_fwd<T>(v.data(), n, hts, dil, wj.data());
        detail::modwt_fwd<T>(v.data(), n, gts, dil, vj.data());
        out.w.push_back(std::move(wj));
        v = std::move(vj);
    }
    out.v = std::move(v);
    return out;
}

template <typename T>
[[nodiscard]] crd::containers::Array<T> imodwt(crd::memory::IAllocator* alloc, const ModwtResult<T>& m,
                                               const Wavelet& w)
{
    const crd::usize n = m.n;
    const crd::usize f = w.len();
    const T inv_sqrt2 = static_cast<T>(1.0 / std::numbers::sqrt2_v<double>);
    crd::containers::Array<T> gt(alloc), ht(alloc);
    gt.resize(f);
    ht.resize(f);
    for (crd::usize k = 0; k < f; ++k)
    {
        gt[k] = static_cast<T>(w.dec_lo[k]) * inv_sqrt2;
        ht[k] = static_cast<T>(w.dec_hi[k]) * inv_sqrt2;
    }
    const crd::containers::ConstSpan<T> gts(gt.data(), f), hts(ht.data(), f);

    crd::containers::Array<T> v(alloc);
    v.resize(n);
    for (crd::usize i = 0; i < n; ++i)
    {
        v[i] = m.v[i];
    }
    for (crd::usize jj = m.levels; jj >= 1; --jj)
    {
        const crd::isize dil = static_cast<crd::isize>(crd::usize{1} << (jj - 1));
        crd::containers::Array<T> vprev(alloc);
        vprev.resize(n);
        for (crd::usize i = 0; i < n; ++i)
        {
            vprev[i] = T(0);
        }
        detail::modwt_inv_add<T>(m.w[jj - 1].data(), n, hts, dil, vprev.data());
        detail::modwt_inv_add<T>(v.data(), n, gts, dil, vprev.data());
        v = std::move(vprev);
        if (jj == 1)
        {
            break;
        }
    }
    return v;
}

} // namespace crd::hesap::wavelet
