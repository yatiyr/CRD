#pragma once

// ---------------------------------------------------------------------------
// crd-hesap-comms v11c-a — digital modulation (the constellation layer).
//
//   gray_encode / gray_decode    reflected binary Gray code.
//   Modem<T>                      PSK / QAM (square) / PAM constellations,
//                                 Gray-mapped + unit average energy. modulate
//                                 (symbol → point), demodulate (point → symbol,
//                                 nearest), soft LLRs (max-log), block helpers.
//   bits_to_symbols / symbols_to_bits   MSB-first bit packing.
//   fsk_modulate / fsk_demodulate_noncoherent   orthogonal M-FSK over samples.
//
// Gate (ADR-0093): the Gray property (nearest-neighbour symbols differ by 1
// bit) + noise-free round trip + unit average energy + BER vs the theoretical
// AWGN curve (the real modem spec) + constellation-set cross-check vs
// liquid-dsp. Lower-layer raw Complex<T>. Deterministic ⇒ run-twice moat.
// ---------------------------------------------------------------------------

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/complex.hpp>
#include <crd/memory/allocator.hpp>

#include <crd/math/cmath.hpp>
#include <limits>
#include <numbers>

namespace crd::hesap::comms
{

// Reflected-binary Gray code (consecutive values differ by exactly one bit).
[[nodiscard]] inline crd::u32 gray_encode(crd::u32 k) noexcept { return k ^ (k >> 1U); }
[[nodiscard]] inline crd::u32 gray_decode(crd::u32 g) noexcept
{
    crd::u32 k = g;
    while (g >>= 1U)
    {
        k ^= g;
    }
    return k;
}

enum class ModFamily : crd::u8
{
    Psk, // M-PSK (points at 2πk/M; BPSK = ±1, QPSK = {1, j, -1, -j})
    Qam, // square M-QAM (M a perfect square ≥ 4)
    Pam  // M-PAM (real levels ±1, ±3, …)
};

// A linear modem: its Gray-mapped, unit-average-energy constellation (indexed by symbol value 0..M-1) + map/demap.
template <typename T> class Modem
{
public:
    Modem(crd::memory::IAllocator* alloc, ModFamily family, crd::u32 order)
        : m_constel(alloc), m_family(family), m_m(order)
    {
        m_bps = 0;
        for (crd::u32 t = order; t > 1U; t >>= 1U)
        {
            ++m_bps;
        }
        m_constel.resize(order);
        build();
    }

    [[nodiscard]] crd::u32 order() const noexcept { return m_m; }
    [[nodiscard]] crd::u32 bits_per_symbol() const noexcept { return m_bps; }
    [[nodiscard]] Complex<T> constellation(crd::u32 sym) const noexcept { return m_constel[sym]; }

    [[nodiscard]] Complex<T> modulate(crd::u32 sym) const noexcept { return m_constel[sym]; }

    // Hard demodulation — O(1) per symbol via per-family slicing (PSK angle, PAM/QAM axis quantization). Gives the
    // exact nearest constellation point for these regular Gray constellations (gated == demodulate_nearest).
    [[nodiscard]] crd::u32 demodulate(Complex<T> r) const noexcept
    {
        if (m_family == ModFamily::Psk)
        {
            const T two_pi = static_cast<T>(2.0 * std::numbers::pi_v<double>);
            const T step = two_pi / static_cast<T>(m_m);
            T ang = crd::math::atan2(r.im, r.re);
            crd::i64 k = static_cast<crd::i64>(crd::math::lround(ang / step));
            k = ((k % static_cast<crd::i64>(m_m)) + static_cast<crd::i64>(m_m)) % static_cast<crd::i64>(m_m);
            return gray_encode(static_cast<crd::u32>(k));
        }
        if (m_family == ModFamily::Pam)
        {
            const crd::u32 k = slice_axis(r.re, m_m);
            return gray_encode(k);
        }
        // square QAM: slice I and Q independently to √M levels.
        const crd::u32 half = m_bps / 2U;
        const crd::u32 ki = slice_axis(r.re, m_l);
        const crd::u32 kq = slice_axis(r.im, m_l);
        return (gray_encode(ki) << half) | gray_encode(kq);
    }

    // Reference O(M) nearest-point demodulator (the clear-correct baseline that `demodulate` is gated against).
    [[nodiscard]] crd::u32 demodulate_nearest(Complex<T> r) const noexcept
    {
        crd::u32 best = 0;
        T bestd = std::numeric_limits<T>::max();
        for (crd::u32 s = 0; s < m_m; ++s)
        {
            const T dr = r.re - m_constel[s].re;
            const T di = r.im - m_constel[s].im;
            const T d = dr * dr + di * di;
            if (d < bestd)
            {
                bestd = d;
                best = s;
            }
        }
        return best;
    }

    void modulate_block(crd::containers::ConstSpan<crd::u32> syms, crd::containers::Span<Complex<T>> out) const noexcept
    {
        for (crd::usize i = 0; i < syms.size(); ++i)
        {
            out[i] = m_constel[syms[i]];
        }
    }
    void demodulate_block(crd::containers::ConstSpan<Complex<T>> r, crd::containers::Span<crd::u32> out) const noexcept
    {
        for (crd::usize i = 0; i < r.size(); ++i)
        {
            out[i] = demodulate(r[i]);
        }
    }

    // Max-log soft demod: LLR for each of the bps bits of the symbol at r (MSB first). LLR_b = (min_{bit=1} d² −
    // min_{bit=0} d²)/N0 ⇒ positive favours bit 0. n0 = noise variance per complex dimension (×2 for total N0).
    void demodulate_soft(Complex<T> r, T n0, crd::containers::Span<T> llr_out) const noexcept
    {
        for (crd::u32 b = 0; b < m_bps; ++b)
        {
            T min0 = std::numeric_limits<T>::max(), min1 = std::numeric_limits<T>::max();
            const crd::u32 mask = 1U << (m_bps - 1U - b); // MSB first
            for (crd::u32 s = 0; s < m_m; ++s)
            {
                const T dr = r.re - m_constel[s].re;
                const T di = r.im - m_constel[s].im;
                const T d = dr * dr + di * di;
                if (s & mask)
                {
                    min1 = (d < min1) ? d : min1;
                }
                else
                {
                    min0 = (d < min0) ? d : min0;
                }
            }
            llr_out[b] = (min1 - min0) / n0;
        }
    }

private:
    void build()
    {
        if (m_family == ModFamily::Psk)
        {
            const T two_pi = static_cast<T>(2.0 * std::numbers::pi_v<double>);
            for (crd::u32 v = 0; v < m_m; ++v)
            {
                const crd::u32 k = gray_decode(v); // position on the circle (adjacent positions = 1-bit Gray apart)
                const T th = two_pi * static_cast<T>(k) / static_cast<T>(m_m);
                m_constel[v] = Complex<T>{crd::math::cos(th), crd::math::sin(th)};
            }
        }
        else if (m_family == ModFamily::Pam)
        {
            for (crd::u32 v = 0; v < m_m; ++v)
            {
                const crd::u32 k = gray_decode(v);
                m_constel[v] = Complex<T>{static_cast<T>(2 * static_cast<int>(k) - static_cast<int>(m_m) + 1), T(0)};
            }
        }
        else // Qam (square)
        {
            crd::u32 l = 1; // √M
            while (l * l < m_m)
            {
                ++l;
            }
            m_l = l;
            const crd::u32 half = m_bps / 2U;
            for (crd::u32 v = 0; v < m_m; ++v)
            {
                const crd::u32 vi = v >> half;        // I bits (high)
                const crd::u32 vq = v & ((1U << half) - 1U); // Q bits (low)
                const crd::u32 ki = gray_decode(vi);
                const crd::u32 kq = gray_decode(vq);
                const T iv = static_cast<T>(2 * static_cast<int>(ki) - static_cast<int>(l) + 1);
                const T qv = static_cast<T>(2 * static_cast<int>(kq) - static_cast<int>(l) + 1);
                m_constel[v] = Complex<T>{iv, qv};
            }
        }
        // normalize to unit average energy.
        T e = T(0);
        for (crd::u32 v = 0; v < m_m; ++v)
        {
            e += m_constel[v].re * m_constel[v].re + m_constel[v].im * m_constel[v].im;
        }
        const T inv = T(1) / crd::math::sqrt(e / static_cast<T>(m_m));
        for (crd::u32 v = 0; v < m_m; ++v)
        {
            m_constel[v] = Complex<T>{m_constel[v].re * inv, m_constel[v].im * inv};
        }
        m_scale = inv; // the per-axis level spacing is 2·m_scale (levels ±1,±3,… × m_scale)
    }

    // Quantize an axis value to the nearest of `levels` PAM positions {0..levels-1} (levels {-(L-1)..(L-1)}·m_scale).
    [[nodiscard]] crd::u32 slice_axis(T v, crd::u32 levels) const noexcept
    {
        crd::i64 k = static_cast<crd::i64>(crd::math::lround((v / m_scale + static_cast<T>(levels) - T(1)) / T(2)));
        if (k < 0)
        {
            k = 0;
        }
        if (k > static_cast<crd::i64>(levels) - 1)
        {
            k = static_cast<crd::i64>(levels) - 1;
        }
        return static_cast<crd::u32>(k);
    }

    crd::containers::Array<Complex<T>> m_constel;
    ModFamily m_family;
    crd::u32 m_m;
    crd::u32 m_bps;
    crd::u32 m_l = 0; // √M for square QAM
    T m_scale = T(1); // normalization (axis spacing = 2·m_scale)
};

// Pack a bit stream (0/1, MSB first per symbol) into symbol values. bits.size() must be a multiple of bps.
inline void bits_to_symbols(crd::containers::ConstSpan<crd::u8> bits, crd::u32 bps,
                            crd::containers::Span<crd::u32> syms) noexcept
{
    const crd::usize nsym = bits.size() / bps;
    for (crd::usize s = 0; s < nsym; ++s)
    {
        crd::u32 v = 0;
        for (crd::u32 b = 0; b < bps; ++b)
        {
            v = (v << 1U) | (bits[s * bps + b] & 1U);
        }
        syms[s] = v;
    }
}

inline void symbols_to_bits(crd::containers::ConstSpan<crd::u32> syms, crd::u32 bps,
                            crd::containers::Span<crd::u8> bits) noexcept
{
    for (crd::usize s = 0; s < syms.size(); ++s)
    {
        for (crd::u32 b = 0; b < bps; ++b)
        {
            bits[s * bps + b] = static_cast<crd::u8>((syms[s] >> (bps - 1U - b)) & 1U);
        }
    }
}

// Orthogonal M-FSK: symbol m → a tone exp(j·2π·(m·Δf)·n) over sps samples (Δf = 1/sps ⇒ orthogonal). Coherent
// (phase reset per symbol). Output length = syms.size()·sps.
template <typename T>
void fsk_modulate(crd::containers::ConstSpan<crd::u32> syms, crd::u32 sps, crd::containers::Span<Complex<T>> out) noexcept
{
    const T two_pi = static_cast<T>(2.0 * std::numbers::pi_v<double>);
    const T df = T(1) / static_cast<T>(sps);
    for (crd::usize s = 0; s < syms.size(); ++s)
    {
        const T f = static_cast<T>(syms[s]) * df;
        for (crd::u32 n = 0; n < sps; ++n)
        {
            const T ph = two_pi * f * static_cast<T>(n);
            out[s * sps + n] = Complex<T>{crd::math::cos(ph), crd::math::sin(ph)};
        }
    }
}

// Non-coherent M-FSK demod: per symbol period, correlate with each of M tones, pick the max-magnitude.
template <typename T>
void fsk_demodulate_noncoherent(crd::containers::ConstSpan<Complex<T>> r, crd::u32 m_order, crd::u32 sps,
                                crd::containers::Span<crd::u32> out) noexcept
{
    const T two_pi = static_cast<T>(2.0 * std::numbers::pi_v<double>);
    const T df = T(1) / static_cast<T>(sps);
    const crd::usize nsym = r.size() / sps;
    for (crd::usize s = 0; s < nsym; ++s)
    {
        crd::u32 best = 0;
        T bestmag = T(-1);
        for (crd::u32 mm = 0; mm < m_order; ++mm)
        {
            const T f = static_cast<T>(mm) * df;
            T cr = T(0), ci = T(0);
            for (crd::u32 n = 0; n < sps; ++n)
            {
                const T ph = -two_pi * f * static_cast<T>(n);
                const Complex<T> rn = r[s * sps + n];
                cr += rn.re * crd::math::cos(ph) - rn.im * crd::math::sin(ph);
                ci += rn.re * crd::math::sin(ph) + rn.im * crd::math::cos(ph);
            }
            const T mag = cr * cr + ci * ci;
            if (mag > bestmag)
            {
                bestmag = mag;
                best = mm;
            }
        }
        out[s] = best;
    }
}

} // namespace crd::hesap::comms
