#pragma once

// crd-hesap-stats v12-g — quasi-Monte-Carlo low-discrepancy sequences: Sobol (Joe-Kuo 2008 direction numbers, 32-bit
// Gray-code) · Halton (radical inverse, optional digit scramble) · rank-1 lattice · Latin Hypercube. These fill the
// unit cube far more evenly than pseudo-random ⇒ O(1/N) integration error vs O(1/√N). Gated vs scipy.stats.qmc
// (Sobol/Halton points) + star-discrepancy + known-integral convergence.

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/hesap/stats/bitgen.hpp>
#include <crd/core/types.hpp>

#include <cmath>

namespace crd::hesap::stats
{
namespace detail
{
// First 32 primes (Halton bases).
inline constexpr crd::u32 kPrimes[32] = {2,  3,  5,  7,  11, 13, 17, 19, 23, 29, 31, 37, 41, 43, 47, 53,
                                         59, 61, 67, 71, 73, 79, 83, 89, 97, 101, 103, 107, 109, 113, 127, 131};

// Joe-Kuo new-joe-kuo-6.21201 direction data for dims 2..21 (dim 1 = van der Corput, no data). {degree s, poly a}.
inline constexpr crd::u32 kSobolS[20] = {1, 2, 3, 3, 4, 4, 5, 5, 5, 5, 5, 5, 6, 6, 6, 6, 6, 6, 7, 7};
inline constexpr crd::u32 kSobolA[20] = {0, 1, 1, 2, 1, 4, 2, 4, 7, 11, 13, 14, 1, 13, 16, 19, 22, 25, 1, 4};
// initial direction integers m_{j,1..s_j} (row-padded to 7).
inline constexpr crd::u32 kSobolM[20][7] = {
    {1, 0, 0, 0, 0, 0, 0},     {1, 3, 0, 0, 0, 0, 0},      {1, 3, 1, 0, 0, 0, 0},     {1, 1, 1, 0, 0, 0, 0},
    {1, 1, 3, 3, 0, 0, 0},     {1, 3, 5, 13, 0, 0, 0},     {1, 1, 5, 5, 17, 0, 0},    {1, 1, 5, 5, 5, 0, 0},
    {1, 1, 7, 11, 19, 0, 0},   {1, 1, 5, 1, 1, 0, 0},      {1, 1, 1, 3, 11, 0, 0},    {1, 3, 5, 5, 31, 0, 0},
    {1, 3, 3, 9, 7, 49, 0},    {1, 1, 1, 15, 21, 21, 0},   {1, 3, 1, 13, 27, 49, 0},  {1, 1, 1, 15, 7, 5, 0},
    {1, 3, 1, 15, 13, 25, 0},  {1, 1, 5, 5, 19, 61, 0},    {1, 3, 7, 11, 23, 15, 103}, {1, 3, 7, 13, 13, 15, 69}};

[[nodiscard]] inline double radical_inverse(crd::u64 i, crd::u32 base) noexcept
{
    double f = 1.0;
    double r = 0.0;
    const double inv = 1.0 / static_cast<double>(base);
    while (i > 0)
    {
        f *= inv;
        r += f * static_cast<double>(i % base);
        i /= base;
    }
    return r;
}
} // namespace detail

// Sobol' sequence (dimension ≤ 21). point(i) returns the i-th point (i=0 ⇒ origin). Direction numbers built once.
class SobolSequence
{
public:
    explicit SobolSequence(crd::u32 dim, crd::memory::IAllocator* alloc) noexcept : m_dim(dim), m_v(alloc)
    {
        m_v.resize(static_cast<crd::usize>(dim) * 32U);
        for (crd::u32 j = 0; j < dim; ++j)
        {
            crd::u32* v = &m_v[static_cast<crd::usize>(j) * 32U];
            if (j == 0)
            {
                for (int k = 0; k < 32; ++k)
                {
                    v[k] = crd::u32{1} << (31 - k);
                }
            }
            else
            {
                const crd::u32 s = detail::kSobolS[j - 1];
                const crd::u32 a = detail::kSobolA[j - 1];
                const crd::u32* m = detail::kSobolM[j - 1];
                for (crd::u32 k = 1; k <= s; ++k)
                {
                    v[k - 1] = m[k - 1] << (32 - k);
                }
                for (crd::u32 k = s + 1; k <= 32; ++k)
                {
                    crd::u32 val = v[k - 1 - s] ^ (v[k - 1 - s] >> s);
                    for (crd::u32 i = 1; i < s; ++i)
                    {
                        if (((a >> (s - 1 - i)) & 1U) != 0U)
                        {
                            val ^= v[k - 1 - i];
                        }
                    }
                    v[k - 1] = val;
                }
            }
        }
    }

    // Write the i-th Sobol point (m_dim coordinates in [0,1)) into out.
    void point(crd::u64 i, crd::containers::Span<double> out) const noexcept
    {
        const crd::u64 gray = i ^ (i >> 1);
        for (crd::u32 j = 0; j < m_dim; ++j)
        {
            const crd::u32* v = &m_v[static_cast<crd::usize>(j) * 32U];
            crd::u32 x = 0;
            for (int k = 0; k < 32; ++k)
            {
                if (((gray >> k) & 1U) != 0U)
                {
                    x ^= v[k];
                }
            }
            out[j] = static_cast<double>(x) * (1.0 / 4294967296.0); // / 2^32
        }
    }

private:
    crd::u32 m_dim;
    crd::containers::Array<crd::u32> m_v;
};

// Halton sequence (dimension ≤ 32). Coordinate j uses the j-th prime base.
class HaltonSequence
{
public:
    explicit HaltonSequence(crd::u32 dim) noexcept : m_dim(dim) {}

    void point(crd::u64 i, crd::containers::Span<double> out) const noexcept
    {
        for (crd::u32 j = 0; j < m_dim; ++j)
        {
            out[j] = detail::radical_inverse(i, detail::kPrimes[j]);
        }
    }

private:
    crd::u32 m_dim;
};

// Rank-1 lattice rule: point i = frac(i · z / n), z a generating vector (Korobov form z_j = g^j mod n by default).
inline void lattice_point(crd::u64 i, crd::u64 n, crd::containers::Span<const crd::u64> z,
                          crd::containers::Span<double> out) noexcept
{
    for (crd::usize j = 0; j < z.size(); ++j)
    {
        const crd::u64 num = (i * z[j]) % n;
        out[j] = static_cast<double>(num) / static_cast<double>(n);
    }
}

// Latin Hypercube: n stratified samples in d dims; each 1-D projection has exactly one point per 1/n stratum.
template <BitGenerator G>
inline void latin_hypercube(G& g, crd::u32 n, crd::u32 d, crd::containers::Span<double> out,
                            crd::memory::IAllocator* alloc) noexcept
{
    crd::containers::Array<crd::u32> perm(alloc);
    perm.resize(n);
    for (crd::u32 j = 0; j < d; ++j)
    {
        for (crd::u32 i = 0; i < n; ++i)
        {
            perm[i] = i;
        }
        for (crd::u32 i = n; i > 1; --i) // Fisher-Yates
        {
            const auto k = static_cast<crd::u32>(bounded(g, i));
            const crd::u32 t = perm[i - 1];
            perm[i - 1] = perm[k];
            perm[k] = t;
        }
        for (crd::u32 i = 0; i < n; ++i)
        {
            out[static_cast<crd::usize>(i) * d + j] = (static_cast<double>(perm[i]) + next_double(g)) / n;
        }
    }
}

} // namespace crd::hesap::stats
