#pragma once

// ---------------------------------------------------------------------------
// crd-hesap-comms v11c-f — framing + forward error correction.
//
//   find_preamble       locate a known preamble by normalized cross-correlation
//                       (the frame-sync detector).
//   hamming74_encode/decode   Hamming(7,4) single-error-correcting code (the
//                       concrete FEC + the hook for stronger codes).
//
// Gate (ADR-0093): the preamble correlation peaks at the true frame offset (even
// in noise); Hamming(7,4) corrects any single-bit error. Lower-layer raw.
// ---------------------------------------------------------------------------

#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/complex.hpp>

#include <cmath>

namespace crd::hesap::comms
{

// Locate `preamble` within `rx` by the peak normalized cross-correlation magnitude. Returns the offset; writes the
// (energy-normalized) peak metric in [0,1] to `out_metric`. A metric near 1 = a confident detection.
template <typename T>
[[nodiscard]] crd::usize find_preamble(crd::containers::ConstSpan<Complex<T>> rx,
                                       crd::containers::ConstSpan<Complex<T>> preamble, T& out_metric) noexcept
{
    const crd::usize n = rx.size(), l = preamble.size();
    T pe = T(0); // preamble energy
    for (crd::usize i = 0; i < l; ++i)
    {
        pe += preamble[i].re * preamble[i].re + preamble[i].im * preamble[i].im;
    }
    crd::usize best = 0;
    T bestmetric = T(-1);
    if (n < l)
    {
        out_metric = T(0);
        return 0;
    }
    for (crd::usize off = 0; off + l <= n; ++off)
    {
        T cr = T(0), ci = T(0), re = T(0); // Σ rx·conj(pre), and rx window energy
        for (crd::usize i = 0; i < l; ++i)
        {
            const Complex<T> r = rx[off + i], p = preamble[i];
            cr += r.re * p.re + r.im * p.im; // Re/Im of rx·conj(p)
            ci += r.im * p.re - r.re * p.im;
            re += r.re * r.re + r.im * r.im;
        }
        const T num = cr * cr + ci * ci;          // |correlation|²
        const T den = pe * re + static_cast<T>(1e-30);
        const T metric = num / den;               // normalized in [0,1]
        if (metric > bestmetric)
        {
            bestmetric = metric;
            best = off;
        }
    }
    out_metric = bestmetric;
    return best;
}

// Hamming(7,4): encode 4 data bits (d0..d3, in bits[0..3]) into 7 code bits. Systematic [d0 d1 d2 d3 p0 p1 p2].
inline void hamming74_encode(const crd::u8* d4, crd::u8* c7) noexcept
{
    const crd::u8 d0 = d4[0] & 1U, d1 = d4[1] & 1U, d2 = d4[2] & 1U, d3 = d4[3] & 1U;
    c7[0] = d0;
    c7[1] = d1;
    c7[2] = d2;
    c7[3] = d3;
    c7[4] = static_cast<crd::u8>(d0 ^ d1 ^ d2); // p0
    c7[5] = static_cast<crd::u8>(d1 ^ d2 ^ d3); // p1
    c7[6] = static_cast<crd::u8>(d0 ^ d1 ^ d3); // p2
}

// Hamming(7,4) decode: correct any single-bit error via the syndrome, output 4 data bits. Returns the corrected
// bit position (0..6) or -1 if no error.
inline int hamming74_decode(const crd::u8* c7, crd::u8* d4) noexcept
{
    crd::u8 r[7];
    for (int i = 0; i < 7; ++i)
    {
        r[i] = c7[i] & 1U;
    }
    const crd::u8 s0 = r[0] ^ r[1] ^ r[2] ^ r[4]; // recompute parities
    const crd::u8 s1 = r[1] ^ r[2] ^ r[3] ^ r[5];
    const crd::u8 s2 = r[0] ^ r[1] ^ r[3] ^ r[6];
    int errpos = -1;
    // map the syndrome (s2 s1 s0) to the error position among [d0 d1 d2 d3 p0 p1 p2].
    const int syn = (s2 << 2) | (s1 << 1) | s0;
    switch (syn)
    {
    case 0b000: errpos = -1; break;
    case 0b101: errpos = 0; break; // d0 in s0,s2
    case 0b111: errpos = 1; break; // d1 in s0,s1,s2
    case 0b011: errpos = 2; break; // d2 in s0,s1
    case 0b110: errpos = 3; break; // d3 in s1,s2
    case 0b001: errpos = 4; break; // p0
    case 0b010: errpos = 5; break; // p1
    case 0b100: errpos = 6; break; // p2
    default: errpos = -1; break;
    }
    if (errpos >= 0)
    {
        r[errpos] ^= 1U;
    }
    d4[0] = r[0];
    d4[1] = r[1];
    d4[2] = r[2];
    d4[3] = r[3];
    return errpos;
}

} // namespace crd::hesap::comms
