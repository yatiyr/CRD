#pragma once

// ---------------------------------------------------------------------------
// crd-hesap-dsp v11-r (sequences) — spreading / pseudo-noise sequences.
//
//   mls     maximum-length sequence (m-sequence) via a Fibonacci LFSR with a
//           primitive feedback polynomial. Length 2ⁿ−1, values ±1. The defining
//           property: a two-valued periodic autocorrelation (2ⁿ−1 at lag 0, −1
//           everywhere else) — the impulse-like sequence used for MLS-based
//           system identification + DSSS spreading.
//   gold    Gold code = elementwise product of a preferred pair of m-sequences
//           (one decimated by 2^(n/2)+1) — a large family with bounded
//           cross-correlation (CDMA / GPS).
//   kasami  small Kasami set from u and its decimation by 2^(n/2)+1 (n even) —
//           the optimal cross-correlation bound √(2ⁿ).
//
// Lower-layer raw scalars; outputs are ±1 (int8). Self-contained gates.
// ---------------------------------------------------------------------------

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>

#include <initializer_list>

namespace crd::hesap::dsp
{

// Default primitive-polynomial recurrence LAGS for n = 2..10: s[k] = XOR_{L ∈ lags} s[k-L]. (From the primitive
// trinomial/pentanomial x^n + … + 1 — e.g. x^7+x+1 ⇒ lags {6,7}; x^8+x^4+x^3+x^2+1 ⇒ {4,5,6,8}.)
[[nodiscard]] inline crd::containers::Array<crd::usize> mls_default_lags(crd::memory::IAllocator* alloc, crd::usize n)
{
    crd::containers::Array<crd::usize> t(alloc);
    auto add = [&](std::initializer_list<crd::usize> ps)
    {
        for (crd::usize p : ps)
        {
            t.push_back(p);
        }
    };
    switch (n)
    {
    case 2: add({1, 2}); break;
    case 3: add({2, 3}); break;
    case 4: add({3, 4}); break;
    case 5: add({3, 5}); break;
    case 6: add({5, 6}); break;
    case 7: add({6, 7}); break;
    case 8: add({4, 5, 6, 8}); break;
    case 9: add({5, 9}); break;
    case 10: add({7, 10}); break;
    default: add({n - 1, n}); break;
    }
    return t;
}

// m-sequence via the GF(2) recurrence s[k] = XOR_{L ∈ lags} s[k-L], length 2ⁿ−1, values ±1 (bit 0 → +1, 1 → −1).
// `lags` from a primitive polynomial; the n-bit initial state is seeded nonzero.
template <typename Int = crd::i8>
[[nodiscard]] crd::containers::Array<Int> mls(crd::memory::IAllocator* alloc, crd::usize n,
                                              crd::containers::ConstSpan<crd::usize> lags, crd::u32 seed = 1)
{
    crd::containers::Array<Int> out(alloc);
    if (n == 0)
    {
        return out;
    }
    const crd::usize len = (static_cast<crd::usize>(1) << n) - 1;
    crd::containers::Array<crd::u8> s(alloc);
    s.resize(len);
    for (crd::usize k = 0; k < n; ++k) // n-bit initial state from the seed (force nonzero)
    {
        s[k] = static_cast<crd::u8>((seed >> (k % 32)) & 1u);
    }
    bool any = false;
    for (crd::usize k = 0; k < n; ++k)
    {
        any = any || (s[k] != 0);
    }
    if (!any)
    {
        s[0] = 1;
    }
    for (crd::usize k = n; k < len; ++k)
    {
        crd::u8 b = 0;
        for (crd::usize j = 0; j < lags.size(); ++j)
        {
            b ^= s[k - lags[j]];
        }
        s[k] = b;
    }
    out.resize(len);
    for (crd::usize i = 0; i < len; ++i)
    {
        out[i] = s[i] ? static_cast<Int>(-1) : static_cast<Int>(1);
    }
    return out;
}

// m-sequence with the default primitive recurrence for n.
template <typename Int = crd::i8>
[[nodiscard]] crd::containers::Array<Int> mls(crd::memory::IAllocator* alloc, crd::usize n, crd::u32 seed = 1)
{
    const auto lags = mls_default_lags(alloc, n);
    return mls<Int>(alloc, n, crd::containers::ConstSpan<crd::usize>(lags.data(), lags.size()), seed);
}

// Gold code: elementwise product (±1) of two equal-length m-sequences (a preferred pair).
template <typename Int = crd::i8>
[[nodiscard]] crd::containers::Array<Int> gold(crd::memory::IAllocator* alloc, crd::containers::ConstSpan<Int> a,
                                               crd::containers::ConstSpan<Int> b)
{
    crd::containers::Array<Int> out(alloc);
    const crd::usize len = (a.size() < b.size()) ? a.size() : b.size();
    out.resize(len);
    for (crd::usize i = 0; i < len; ++i)
    {
        out[i] = static_cast<Int>(a[i] * b[i]);
    }
    return out;
}

// Decimate a ±1 sequence by `q` (sample every q-th element, modulo length) — used to form Gold/Kasami pairs.
template <typename Int = crd::i8>
[[nodiscard]] crd::containers::Array<Int> decimate_seq(crd::memory::IAllocator* alloc, crd::containers::ConstSpan<Int> s,
                                                       crd::usize q)
{
    crd::containers::Array<Int> out(alloc);
    const crd::usize len = s.size();
    out.resize(len);
    for (crd::usize i = 0; i < len; ++i)
    {
        out[i] = s[(i * q) % len];
    }
    return out;
}

// Kasami small set member: u ⊙ shift_k(w), w = u decimated by 2^(n/2)+1 (n even). `shift` selects the family member.
template <typename Int = crd::i8>
[[nodiscard]] crd::containers::Array<Int> kasami(crd::memory::IAllocator* alloc, crd::usize n, crd::usize shift,
                                                 crd::u32 seed = 1)
{
    const auto u = mls<Int>(alloc, n, seed);
    const crd::usize q = (static_cast<crd::usize>(1) << (n / 2)) + 1;
    const auto w = decimate_seq<Int>(alloc, crd::containers::ConstSpan<Int>(u.data(), u.size()), q);
    crd::containers::Array<Int> out(alloc);
    const crd::usize len = u.size();
    out.resize(len);
    for (crd::usize i = 0; i < len; ++i)
    {
        out[i] = static_cast<Int>(u[i] * w[(i + shift) % len]);
    }
    return out;
}

} // namespace crd::hesap::dsp
