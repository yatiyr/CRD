#pragma once

// ---------------------------------------------------------------------------
// crd-hesap-wavelet v11w-a — wavelet families + filter banks.
//
// The complete discrete-wavelet catalog (Haar / Daubechies db1-20 / Symlets
// sym2-20 / Coiflets coif1-5 / biorthogonal bior + reverse rbio / Discrete
// Meyer dmey). Each wavelet carries its four filters — the analysis pair
// (dec_lo, dec_hi) and the synthesis pair (rec_lo, rec_hi) — sourced verbatim
// from PyWavelets (the generated detail/wavelet_coeffs.hpp), so the engine
// matches pywt to machine precision.
//
//   wavelet_by_name(name)        look up a Wavelet by its pywt name.
//   qmf(h, g)                    quadrature-mirror filter g[k] = (-1)^k h[L-1-k].
//   orthogonal_filter_bank(...)  build the 4 filters from one scaling filter.
//
// Gate (self-contained + vs pywt): the orthonormality conditions (Σh = √2,
// Σh² = 1, double-shift orthogonality) + the QMF builder reproduces the stored
// bank for orthogonal wavelets + the coefficients match pywt. The QMF math is
// the slice's self-contained deliverable; the stored table pins pywt-exactness.
// ---------------------------------------------------------------------------

#include <crd/containers/span.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/wavelet/detail/wavelet_coeffs.hpp>

#include <optional>

namespace crd::hesap::wavelet
{

// A wavelet = its four FIR filters (non-owning views into the static generated table) + metadata.
// dec_lo/dec_hi: the analysis (decomposition) low/high-pass. rec_lo/rec_hi: the synthesis (reconstruction) pair.
struct Wavelet
{
    crd::containers::StringView name;
    crd::containers::ConstSpan<double> dec_lo;
    crd::containers::ConstSpan<double> dec_hi;
    crd::containers::ConstSpan<double> rec_lo;
    crd::containers::ConstSpan<double> rec_hi;
    bool orthogonal = false;

    [[nodiscard]] crd::usize len() const noexcept { return dec_lo.size(); }
};

// Look up a wavelet by its pywt name ("haar", "db4", "sym8", "coif2", "bior2.2", "rbio3.5", "dmey", ...).
// Returns std::nullopt for an unknown name. The returned views point into static storage (always valid).
[[nodiscard]] inline std::optional<Wavelet> wavelet_by_name(crd::containers::StringView name) noexcept
{
    for (int i = 0; i < detail::kWaveletTableSize; ++i)
    {
        const detail::WaveletTableEntry& e = detail::kWaveletTable[i];
        if (name == e.name)
        {
            const crd::usize n = static_cast<crd::usize>(e.len);
            return Wavelet{crd::containers::StringView(e.name),
                           crd::containers::ConstSpan<double>(e.dec_lo, n),
                           crd::containers::ConstSpan<double>(e.dec_hi, n),
                           crd::containers::ConstSpan<double>(e.rec_lo, n),
                           crd::containers::ConstSpan<double>(e.rec_hi, n),
                           e.orthogonal};
        }
    }
    return std::nullopt;
}

// The number of wavelets in the catalog, and the i-th wavelet (for enumeration / CLI listing).
[[nodiscard]] inline int wavelet_count() noexcept { return detail::kWaveletTableSize; }

[[nodiscard]] inline Wavelet wavelet_at(int i) noexcept
{
    const detail::WaveletTableEntry& e = detail::kWaveletTable[i];
    const crd::usize n = static_cast<crd::usize>(e.len);
    return Wavelet{crd::containers::StringView(e.name),
                   crd::containers::ConstSpan<double>(e.dec_lo, n),
                   crd::containers::ConstSpan<double>(e.dec_hi, n),
                   crd::containers::ConstSpan<double>(e.rec_lo, n),
                   crd::containers::ConstSpan<double>(e.rec_hi, n),
                   e.orthogonal};
}

// Quadrature-mirror filter (pywt convention): g[k] = (-1)^k · h[L-1-k]. |h| == |g|.
template <typename T> void qmf(crd::containers::ConstSpan<T> h, crd::containers::Span<T> g) noexcept
{
    const crd::usize l = h.size();
    for (crd::usize k = 0; k < l; ++k)
    {
        const T s = (k & 1U) ? T(-1) : T(1);
        g[k] = s * h[l - 1 - k];
    }
}

// Build the orthogonal filter bank from the scaling (synthesis low-pass) filter `rec_lo`:
//   dec_lo[k] = rec_lo[L-1-k]              (time reverse)
//   rec_hi    = qmf(rec_lo)               (g[k] = (-1)^k rec_lo[L-1-k])
//   dec_hi[k] = rec_hi[L-1-k]              (time reverse)
// Reproduces pywt's orthogonal_filter_bank exactly (the v11w-a self-gate feeds a stored rec_lo back through this).
template <typename T>
void orthogonal_filter_bank(crd::containers::ConstSpan<T> scaling_rec_lo, crd::containers::Span<T> dec_lo,
                            crd::containers::Span<T> dec_hi, crd::containers::Span<T> rec_lo,
                            crd::containers::Span<T> rec_hi) noexcept
{
    const crd::usize l = scaling_rec_lo.size();
    for (crd::usize k = 0; k < l; ++k)
    {
        rec_lo[k] = scaling_rec_lo[k];
        dec_lo[k] = scaling_rec_lo[l - 1 - k];
    }
    qmf<T>(crd::containers::ConstSpan<T>(rec_lo.data(), l), rec_hi);
    for (crd::usize k = 0; k < l; ++k)
    {
        dec_hi[k] = rec_hi[l - 1 - k];
    }
}

} // namespace crd::hesap::wavelet
