#pragma once

// ---------------------------------------------------------------------------
// crd-hesap-wavelet v11w-b — 1-D discrete wavelet transform.
//
//   dwt        single-level DWT: x -> (cA, cD) (approximation + detail).
//   idwt       single-level inverse: (cA, cD) -> x.
//   wavedec    multilevel decomposition: x -> [cA_n, cD_n, ..., cD_1].
//   waverec    multilevel reconstruction: coeffs -> x.
//   dwt_max_level / dwt_coeff_len   sizing helpers.
//
// Convention pinned bit-for-bit against PyWavelets (probed, not guessed):
//   cA[i] = Σ_k dec_lo[k] · xext[2i+1 - k]            (downsample-from-1, step 2)
//   out length floor((N+F-1)/2)  [periodization: ceil(N/2), offset F/2 mod N]
//   idwt: upsample (zeros at odds), full-conv with rec_lo/rec_hi, sum,
//         trim [F-2 : F-2 + 2·len - F + 2].
//
// Gate (ADR-0093 application): per-mode coefficients vs pywt + perfect
// reconstruction + run-twice bit-identical (single-thread fixed order = the
// determinism moat pywt/MATLAB lack). PR alone is NOT sufficient — a self-
// consistent wrong convention reconstructs perfectly; the pywt coefficient
// gate is what pins compatibility.
// ---------------------------------------------------------------------------

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/wavelet/families.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::hesap::wavelet
{

// PyWavelets signal-extension modes (boundary handling). 'periodic' != 'periodization'.
enum class SignalExtensionMode : crd::u8
{
    Zero,          // ... 0 0 | a b c d | 0 0 ...
    Constant,      // ... a a | a b c d | d d ...  (replicate edge)
    Symmetric,     // ... b a | a b c d | d c ...  (half-sample mirror, edge repeated)
    Reflect,       // ... c b | a b c d | c b ...  (whole-sample mirror, edge not repeated)
    Periodic,      // ... c d | a b c d | a b ...  (wrap)
    Periodization, // periodic, minimal ceil(N/2) coeffs (non-redundant)
    Smooth,        // first-derivative (constant-slope) extrapolation
    Antisymmetric, // half-sample antisymmetric (mirror + negate)
    Antireflect    // whole-sample antisymmetric (point reflection about edge sample)
};

namespace detail
{

// Half-sample symmetric fold (period 2n): ... b a | a b c d | d c ...
[[nodiscard]] inline crd::isize fold_symmetric(crd::isize j, crd::isize n) noexcept
{
    const crd::isize p = 2 * n;
    crd::isize m = j % p;
    if (m < 0)
    {
        m += p;
    }
    if (m >= n)
    {
        m = p - 1 - m;
    }
    return m;
}

// Whole-sample reflect fold (period 2n-2): ... c b | a b c d | c b ...
[[nodiscard]] inline crd::isize fold_reflect(crd::isize j, crd::isize n) noexcept
{
    if (n == 1)
    {
        return 0;
    }
    const crd::isize p = 2 * n - 2;
    crd::isize m = j % p;
    if (m < 0)
    {
        m += p;
    }
    if (m >= n)
    {
        m = p - m;
    }
    return m;
}

// Boundary-mapped sample value at (possibly out-of-range) index j. Mirrors pywt's on-the-fly extension.
template <typename T>
[[nodiscard]] T boundary_value(const T* x, crd::isize j, crd::isize n, SignalExtensionMode mode) noexcept
{
    if (j >= 0 && j < n)
    {
        return x[j];
    }
    switch (mode)
    {
    case SignalExtensionMode::Zero:
    case SignalExtensionMode::Periodization: // periodization uses a dedicated path; never out-of-range here
        return T(0);
    case SignalExtensionMode::Constant:
        return (j < 0) ? x[0] : x[n - 1];
    case SignalExtensionMode::Periodic:
    {
        crd::isize m = j % n;
        if (m < 0)
        {
            m += n;
        }
        return x[m];
    }
    case SignalExtensionMode::Symmetric:
        return x[fold_symmetric(j, n)];
    case SignalExtensionMode::Reflect:
        return x[fold_reflect(j, n)];
    case SignalExtensionMode::Smooth:
    {
        if (n == 1)
        {
            return x[0];
        }
        if (j < 0)
        {
            return x[0] + static_cast<T>(j) * (x[1] - x[0]);
        }
        return x[n - 1] + static_cast<T>(j - (n - 1)) * (x[n - 1] - x[n - 2]);
    }
    case SignalExtensionMode::Antisymmetric:
    {
        const crd::isize p = 2 * n;
        crd::isize m = j % p;
        if (m < 0)
        {
            m += p;
        }
        T sign = T(1);
        if (m >= n)
        {
            m = p - 1 - m;
            sign = -sign;
        }
        return sign * x[m];
    }
    case SignalExtensionMode::Antireflect:
    {
        if (n == 1)
        {
            return x[0];
        }
        T acc = T(0);
        T sign = T(1);
        crd::isize jj = j;
        while (!(jj >= 0 && jj < n))
        {
            if (jj < 0)
            {
                acc += sign * T(2) * x[0];
                jj = -jj;
            }
            else
            {
                acc += sign * T(2) * x[n - 1];
                jj = 2 * (n - 1) - jj;
            }
            sign = -sign;
        }
        return acc + sign * x[jj];
    }
    }
    return T(0);
}

} // namespace detail

// Output coefficient length for a single DWT level.
[[nodiscard]] inline crd::usize dwt_coeff_len(crd::usize n, crd::usize filter_len, SignalExtensionMode mode) noexcept
{
    if (mode == SignalExtensionMode::Periodization)
    {
        return (n + 1) / 2; // ceil(n/2)
    }
    return (n + filter_len - 1) / 2; // floor((n+F-1)/2)
}

// Deepest useful decomposition level: floor(log2(n / (F-1))).
[[nodiscard]] inline crd::usize dwt_max_level(crd::usize n, crd::usize filter_len) noexcept
{
    if (filter_len < 2 || n < filter_len - 1)
    {
        return 0;
    }
    crd::usize level = 0;
    crd::usize m = n / (filter_len - 1);
    while (m > 1)
    {
        m >>= 1;
        ++level;
    }
    return level;
}

namespace detail
{

// Downsampling convolution: out[i] = Σ_k filt[k] · boundary(x, 2i+1 - k), i in [0, out_len). The DWT analysis core.
// `rfilt` is the TIME-REVERSED filter (rfilt[j] = filt[F-1-j]) so the support window x[base .. base+F-1] is read
// ascending-contiguous: out[i] = Σ_j rfilt[j] · x[base+j], base = 2i+2-F. The interior (where the whole window is
// in [0,N)) runs branch-free (the hot, vectorizable path, like the resample_poly reversed bank); only the few left/
// right edge outputs pay the boundary cost. This is what closes the ~8× gap vs pywt's split convolution.
template <typename T>
void downsampling_convolution(const T* x, crd::usize n, crd::containers::ConstSpan<T> rfilt,
                              SignalExtensionMode mode, T* out, crd::usize out_len) noexcept
{
    const crd::isize ni = static_cast<crd::isize>(n);
    const crd::isize f = static_cast<crd::isize>(rfilt.size());
    const T* rf = rfilt.data();
    for (crd::usize i = 0; i < out_len; ++i)
    {
        const crd::isize base = 2 * static_cast<crd::isize>(i) + 2 - f; // x index of rfilt[0]
        T acc = T(0);
        if (base >= 0 && base + f <= ni) // whole support inside [0,N): branch-free MAC
        {
            const T* xp = x + base;
            for (crd::isize j = 0; j < f; ++j)
            {
                acc += rf[j] * xp[j];
            }
        }
        else // touches an edge: boundary-mapped reads
        {
            for (crd::isize j = 0; j < f; ++j)
            {
                acc += rf[j] * boundary_value<T>(x, base + j, ni, mode);
            }
        }
        out[i] = acc;
    }
}

// Periodization analysis (reversed filter): out[i] = Σ_j rfilt[j]·xe[(base+j) mod ne], base = 2i + F/2 - (F-1),
// i in [0, ceil(n/2)). ODD n pads to even ne=n+1 by REPEATING the last sample (pywt-matched, probed); even n: ne=n.
// Interior outputs (window inside [0,N)) run branch-free; only wrap-around edges pay the mod.
template <typename T>
void downsampling_convolution_periodization(const T* x, crd::usize n, crd::containers::ConstSpan<T> rfilt, T* out,
                                            crd::usize out_len) noexcept
{
    const crd::isize ni = static_cast<crd::isize>(n);
    const crd::isize ne = (n % 2 == 0) ? ni : ni + 1;
    const crd::isize f = static_cast<crd::isize>(rfilt.size());
    const crd::isize off = f / 2;
    const T* rf = rfilt.data();
    for (crd::usize i = 0; i < out_len; ++i)
    {
        const crd::isize base = 2 * static_cast<crd::isize>(i) + off - (f - 1);
        T acc = T(0);
        if (base >= 0 && base + f <= ni) // whole window inside the real signal: branch-free
        {
            const T* xp = x + base;
            for (crd::isize j = 0; j < f; ++j)
            {
                acc += rf[j] * xp[j];
            }
        }
        else
        {
            for (crd::isize j = 0; j < f; ++j)
            {
                crd::isize idx = (base + j) % ne;
                if (idx < 0)
                {
                    idx += ne;
                }
                acc += rf[j] * ((idx >= ni) ? x[ni - 1] : x[idx]); // idx==ni is the appended (repeated) sample
            }
        }
        out[i] = acc;
    }
}

} // namespace detail

// Single-level DWT. Fills cA (approximation, dec_lo) and cD (detail, dec_hi). Both resized to dwt_coeff_len.
template <typename T>
void dwt(crd::memory::IAllocator* /*alloc*/, crd::containers::ConstSpan<T> x, const Wavelet& w,
         SignalExtensionMode mode, crd::containers::Array<T>& cA, crd::containers::Array<T>& cD)
{
    const crd::usize n = x.size();
    const crd::usize f = w.len();
    const crd::usize out_len = dwt_coeff_len(n, f, mode);
    cA.resize(out_len);
    cD.resize(out_len);
    if (out_len == 0)
    {
        return;
    }
    // dec_lo/dec_hi are double (the generated table); build the TIME-REVERSED filters in T for the kernel
    // (rlo[j] = dec_lo[F-1-j]) so the convolution reads x ascending-contiguous (vectorizable interior).
    crd::containers::Array<T> rlo(cA.allocator()), rhi(cA.allocator());
    rlo.resize(f);
    rhi.resize(f);
    for (crd::usize k = 0; k < f; ++k)
    {
        rlo[k] = static_cast<T>(w.dec_lo[f - 1 - k]);
        rhi[k] = static_cast<T>(w.dec_hi[f - 1 - k]);
    }
    const crd::containers::ConstSpan<T> rlos(rlo.data(), f), rhis(rhi.data(), f);
    if (mode == SignalExtensionMode::Periodization)
    {
        detail::downsampling_convolution_periodization<T>(x.data(), n, rlos, cA.data(), out_len);
        detail::downsampling_convolution_periodization<T>(x.data(), n, rhis, cD.data(), out_len);
    }
    else
    {
        detail::downsampling_convolution<T>(x.data(), n, rlos, mode, cA.data(), out_len);
        detail::downsampling_convolution<T>(x.data(), n, rhis, mode, cD.data(), out_len);
    }
}

namespace detail
{

// Upsampling convolution accumulator for IDWT: y += full_conv(upsample2(c), rec), trimmed to the central region.
// y has length out_len = 2·m - F + 2 (standard) or 2·m (periodization). c has length m, rec length F.
template <typename T>
void upsampling_convolution_add(const T* c, crd::usize m, crd::containers::ConstSpan<T> rec, T* y,
                                crd::usize out_len, bool periodization) noexcept
{
    const crd::isize f = static_cast<crd::isize>(rec.size());
    const crd::isize mi = static_cast<crd::isize>(m);
    if (periodization)
    {
        // Full periodic reconstruction: y[t] += Σ over upsampled periodic samples. Upsampled length 2m, period 2m.
        const crd::isize len2 = 2 * mi;
        const crd::isize shift = f / 2 - 1; // align the periodization synthesis (pywt-matched)
        for (crd::usize t = 0; t < out_len; ++t)
        {
            T acc = T(0);
            for (crd::isize k = 0; k < f; ++k)
            {
                crd::isize src = static_cast<crd::isize>(t) + shift - k; // index into the upsampled (zero-odd) stream
                crd::isize mm = src % len2;
                if (mm < 0)
                {
                    mm += len2;
                }
                if ((mm & 1) == 0) // only even positions of the upsampled stream are non-zero (= c[mm/2])
                {
                    acc += rec[static_cast<crd::usize>(k)] * c[static_cast<crd::usize>(mm / 2)];
                }
            }
            y[t] += acc;
        }
        return;
    }
    // Standard: full convolution of the zero-upsampled coefficients with rec, trimmed to [F-2 : F-2 + out_len).
    for (crd::usize t = 0; t < out_len; ++t)
    {
        const crd::isize full_idx = static_cast<crd::isize>(t) + (f - 2); // position in the full convolution
        T acc = T(0);
        for (crd::isize k = 0; k < f; ++k)
        {
            const crd::isize up = full_idx - k; // index into the upsampled stream (length 2m)
            if (up >= 0 && up < 2 * mi && (up & 1) == 0)
            {
                acc += rec[static_cast<crd::usize>(k)] * c[static_cast<crd::usize>(up / 2)];
            }
        }
        y[t] += acc;
    }
}

} // namespace detail

// Single-level inverse DWT. cA and cD must have equal length m. Output length 2m-F+2 (standard) / 2m (periodization).
template <typename T>
void idwt(crd::memory::IAllocator* /*alloc*/, crd::containers::ConstSpan<T> cA, crd::containers::ConstSpan<T> cD,
          const Wavelet& w, SignalExtensionMode mode, crd::containers::Array<T>& out)
{
    const crd::usize m = cA.size();
    const crd::usize f = w.len();
    const bool periodization = (mode == SignalExtensionMode::Periodization);
    const crd::usize out_len = periodization ? (2 * m) : (2 * m - f + 2);
    out.resize(out_len);
    for (crd::usize t = 0; t < out_len; ++t)
    {
        out[t] = T(0);
    }
    if (out_len == 0)
    {
        return;
    }
    crd::containers::Array<T> lo(out.allocator()), hi(out.allocator());
    lo.resize(f);
    hi.resize(f);
    for (crd::usize k = 0; k < f; ++k)
    {
        lo[k] = static_cast<T>(w.rec_lo[k]);
        hi[k] = static_cast<T>(w.rec_hi[k]);
    }
    detail::upsampling_convolution_add<T>(cA.data(), m, crd::containers::ConstSpan<T>(lo.data(), f), out.data(),
                                          out_len, periodization);
    detail::upsampling_convolution_add<T>(cD.data(), m, crd::containers::ConstSpan<T>(hi.data(), f), out.data(),
                                          out_len, periodization);
}

// Multilevel decomposition. Returns [cA_level, cD_level, cD_{level-1}, ..., cD_1] (pywt wavedec order).
// level==0 selects dwt_max_level. Coefficients are stored as crd Arrays (no STL).
template <typename T>
[[nodiscard]] crd::containers::Array<crd::containers::Array<T>>
wavedec(crd::memory::IAllocator* alloc, crd::containers::ConstSpan<T> x, const Wavelet& w, SignalExtensionMode mode,
        crd::usize level = 0)
{
    const crd::usize maxl = dwt_max_level(x.size(), w.len());
    if (level == 0)
    {
        level = maxl;
    }
    crd::containers::Array<crd::containers::Array<T>> result(alloc);
    result.reserve(level + 1);
    if (level == 0)
    {
        crd::containers::Array<T> a(alloc);
        a.resize(x.size());
        for (crd::usize i = 0; i < x.size(); ++i)
        {
            a[i] = x[i];
        }
        result.push_back(std::move(a));
        return result;
    }
    // Decompose iteratively; collect details in [cD_level ... cD_1] order, then prepend the final approximation.
    crd::containers::Array<crd::containers::Array<T>> details(alloc); // cD_1, cD_2, ..., cD_level (low->high level)
    details.reserve(level);
    crd::containers::Array<T> approx(alloc);
    approx.resize(x.size());
    for (crd::usize i = 0; i < x.size(); ++i)
    {
        approx[i] = x[i];
    }
    for (crd::usize l = 0; l < level; ++l)
    {
        crd::containers::Array<T> cA(alloc), cD(alloc);
        dwt<T>(alloc, crd::containers::ConstSpan<T>(approx.data(), approx.size()), w, mode, cA, cD);
        details.push_back(std::move(cD));
        approx = std::move(cA);
    }
    // result = [approx, cD_level, cD_{level-1}, ..., cD_1]
    result.push_back(std::move(approx));
    for (crd::usize l = 0; l < level; ++l)
    {
        result.push_back(std::move(details[level - 1 - l]));
    }
    return result;
}

// Multilevel reconstruction from wavedec output [cA_n, cD_n, ..., cD_1].
template <typename T>
[[nodiscard]] crd::containers::Array<T> waverec(crd::memory::IAllocator* alloc,
                                                const crd::containers::Array<crd::containers::Array<T>>& coeffs,
                                                const Wavelet& w, SignalExtensionMode mode)
{
    crd::containers::Array<T> approx(alloc);
    const crd::usize ncoef = coeffs.size();
    approx.resize(coeffs[0].size());
    for (crd::usize i = 0; i < coeffs[0].size(); ++i)
    {
        approx[i] = coeffs[0][i];
    }
    for (crd::usize d = 1; d < ncoef; ++d)
    {
        const crd::containers::Array<T>& cD = coeffs[d];
        // idwt expects equal-length cA/cD. After upsampling-conv, length may exceed the next detail by 1 (odd N);
        // trim the approximation to the detail length before combining (pywt waverec behavior).
        crd::usize m = approx.size();
        if (cD.size() < m)
        {
            m = cD.size();
        }
        crd::containers::Array<T> out(alloc);
        idwt<T>(alloc, crd::containers::ConstSpan<T>(approx.data(), m), crd::containers::ConstSpan<T>(cD.data(), m), w,
                mode, out);
        approx = std::move(out);
    }
    return approx;
}

} // namespace crd::hesap::wavelet
