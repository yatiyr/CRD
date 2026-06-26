#pragma once

// ---------------------------------------------------------------------------
// crd-hesap-wavelet v11w-e — wavelet denoising (thresholding).
//
//   threshold            soft / hard / garrote coefficient shrinkage (pywt
//                        pywt.threshold conventions; hard/garrote use |x| >= t).
//   mad_sigma            robust noise estimate σ = median(|cD1|)/0.6745.
//   universal_threshold  VisuShrink t = σ·√(2 ln N).
//   bayes_threshold      BayesShrink per-subband t = σ²/σ_x.
//   denoise              wavedec → threshold details → waverec (the wrapper).
//
// Gate (ADR-0093): threshold coefficients vs pywt.threshold (exact) + denoise
// improves SNR on a noisy signal (self-contained) + run-twice bit-identical.
// (SureShrink + MODWT-based denoising = follow-ons; SWT already provides the
// undecimated transform.)
// ---------------------------------------------------------------------------

#include <crd/containers/array.hpp>
#include <crd/containers/sort.hpp> // crd::containers::nth_element (deterministic, ADR-0063)
#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/wavelet/dwt.hpp>
#include <crd/hesap/wavelet/families.hpp>
#include <crd/memory/allocator.hpp>

#include <algorithm>
#include <crd/math/cmath.hpp>

namespace crd::hesap::wavelet
{

enum class ThresholdMode : crd::u8
{
    Soft,    // sign(x)·max(|x|-t, 0)
    Hard,    // x if |x| >= t else 0
    Garrote  // x - t²/x if |x| >= t else 0  (non-negative garrote)
};

template <typename T> [[nodiscard]] T threshold_value(T x, T t, ThresholdMode mode) noexcept
{
    const T ax = std::abs(x);
    switch (mode)
    {
    case ThresholdMode::Soft:
    {
        const T s = ax - t;
        return (s > T(0)) ? ((x >= T(0)) ? s : -s) : T(0);
    }
    case ThresholdMode::Hard:
        return (ax >= t) ? x : T(0);
    case ThresholdMode::Garrote:
        return (ax >= t) ? (x - (t * t) / x) : T(0);
    }
    return x;
}

template <typename T> void threshold_inplace(crd::containers::Span<T> data, T t, ThresholdMode mode) noexcept
{
    for (crd::usize i = 0; i < data.size(); ++i)
    {
        data[i] = threshold_value<T>(data[i], t, mode);
    }
}

// Robust noise σ from the finest detail level: median(|cD1|)/0.6745 (MAD estimator).
template <typename T> [[nodiscard]] T mad_sigma(crd::memory::IAllocator* alloc, crd::containers::ConstSpan<T> cd1)
{
    const crd::usize n = cd1.size();
    if (n == 0)
    {
        return T(0);
    }
    crd::containers::Array<T> a(alloc);
    a.resize(n);
    for (crd::usize i = 0; i < n; ++i)
    {
        a[i] = std::abs(cd1[i]);
    }
    const crd::usize mid = n / 2;
    crd::containers::nth_element(a.data(), a.data() + mid, a.data() + n);
    T med = a[mid];
    if ((n & 1U) == 0) // even: average the two central order statistics
    {
        crd::containers::nth_element(a.data(), a.data() + mid - 1, a.data() + n);
        med = (med + a[mid - 1]) / T(2);
    }
    return med / static_cast<T>(0.6745);
}

// VisuShrink (universal) threshold: σ·√(2 ln N).
template <typename T> [[nodiscard]] T universal_threshold(T sigma, crd::usize n) noexcept
{
    return sigma * static_cast<T>(crd::math::sqrt(2.0 * crd::math::log(static_cast<double>(n))));
}

// BayesShrink per-subband threshold: σ²/σ_x, σ_x = √(max(var(subband)-σ², 0)). If σ_x → 0, threshold everything.
template <typename T> [[nodiscard]] T bayes_threshold(crd::containers::ConstSpan<T> subband, T sigma) noexcept
{
    const crd::usize n = subband.size();
    if (n == 0)
    {
        return T(0);
    }
    T mean = T(0);
    for (crd::usize i = 0; i < n; ++i)
    {
        mean += subband[i];
    }
    mean /= static_cast<T>(n);
    T var = T(0);
    for (crd::usize i = 0; i < n; ++i)
    {
        const T d = subband[i] - mean;
        var += d * d;
    }
    var /= static_cast<T>(n);
    const T sig2 = sigma * sigma;
    const T sx2 = var - sig2;
    if (sx2 <= T(0))
    {
        T mx = T(0); // pure noise ⇒ threshold at the subband max
        for (crd::usize i = 0; i < n; ++i)
        {
            mx = std::max(mx, std::abs(subband[i]));
        }
        return mx;
    }
    return sig2 / crd::math::sqrt(sx2);
}

// SureShrink (Donoho-Johnstone) per-subband threshold: the hybrid that uses the universal threshold when the
// subband is "sparse" and otherwise the SURE (Stein Unbiased Risk Estimate) minimizer over the candidate set
// {|d_i|}. Works on d normalized by σ. Returns the threshold in original (un-normalized) units.
template <typename T>
[[nodiscard]] T sure_threshold(crd::memory::IAllocator* alloc, crd::containers::ConstSpan<T> subband, T sigma)
{
    const crd::usize n = subband.size();
    if (n == 0 || sigma <= T(0))
    {
        return T(0);
    }
    const T uthr = static_cast<T>(crd::math::sqrt(2.0 * crd::math::log(static_cast<double>(n)))); // universal (normalized units)
    // sparsity test: if the normalized energy excess is small, the universal threshold is better than SURE.
    T s2 = T(0);
    for (crd::usize i = 0; i < n; ++i)
    {
        const T y = subband[i] / sigma;
        s2 += y * y;
    }
    const T eta = (s2 - static_cast<T>(n)) / static_cast<T>(n);
    const T gamma = static_cast<T>(crd::math::pow(crd::math::log2(static_cast<double>(n)), 1.5) / crd::math::sqrt(static_cast<double>(n)));
    if (eta <= gamma)
    {
        return sigma * uthr;
    }
    // SURE: minimize over t ∈ {|y_i|} (sorted): SURE(t) = n - 2·#{|y_i|<=t} + Σ min(y_i², t²).
    crd::containers::Array<T> a(alloc);
    a.resize(n);
    for (crd::usize i = 0; i < n; ++i)
    {
        a[i] = std::abs(subband[i]) / sigma;
    }
    crd::containers::sort(a.data(), a.data() + n);
    // prefix sums of y² over the sorted |y| let us evaluate SURE in O(n).
    T best_risk = T(0);
    T best_t = a[0];
    T cumsq = T(0); // Σ_{i: |y_i| <= t} y_i²  for the current t (sorted scan)
    for (crd::usize k = 0; k < n; ++k)
    {
        const T t = a[k];
        // elements 0..k have |y|<=t (count k+1); their y² contribute as min=y²; the rest contribute t².
        cumsq += a[k] * a[k];
        const T num_le = static_cast<T>(k + 1);
        const T risk = static_cast<T>(n) - T(2) * num_le + cumsq + (static_cast<T>(n) - num_le) * t * t;
        if (k == 0 || risk < best_risk)
        {
            best_risk = risk;
            best_t = t;
        }
    }
    const T t_final = (best_t < uthr) ? best_t : uthr; // SureShrink caps at the universal threshold
    return sigma * t_final;
}

enum class DenoiseRule : crd::u8
{
    VisuShrink, // universal threshold (one σ·√(2 ln N) for all detail levels)
    BayesShrink, // per-subband BayesShrink threshold
    SureShrink   // per-subband hybrid SURE / universal threshold
};

// Denoise: wavedec (periodization) → estimate σ from cD1 → threshold every detail level → waverec.
template <typename T>
[[nodiscard]] crd::containers::Array<T> denoise(crd::memory::IAllocator* alloc, crd::containers::ConstSpan<T> signal,
                                                const Wavelet& w, crd::usize level, ThresholdMode tmode,
                                                DenoiseRule rule, SignalExtensionMode mode = SignalExtensionMode::Periodization)
{
    auto coeffs = wavedec<T>(alloc, signal, w, mode, level);
    // coeffs = [cA_level, cD_level, ..., cD_1]; the finest detail is the last.
    const crd::usize last = coeffs.size() - 1;
    const T sigma = mad_sigma<T>(alloc, crd::containers::ConstSpan<T>(coeffs[last].data(), coeffs[last].size()));
    const T uthr = universal_threshold<T>(sigma, signal.size());
    for (crd::usize d = 1; d < coeffs.size(); ++d) // threshold every detail subband (not the approximation)
    {
        crd::containers::Span<T> sub(coeffs[d].data(), coeffs[d].size());
        const crd::containers::ConstSpan<T> csub(sub.data(), sub.size());
        T t = uthr;
        if (rule == DenoiseRule::BayesShrink)
        {
            t = bayes_threshold<T>(csub, sigma);
        }
        else if (rule == DenoiseRule::SureShrink)
        {
            t = sure_threshold<T>(alloc, csub, sigma);
        }
        threshold_inplace<T>(sub, t, tmode);
    }
    return waverec<T>(alloc, coeffs, w, mode);
}

} // namespace crd::hesap::wavelet
