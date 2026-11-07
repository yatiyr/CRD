// RFC 6330's constraint matrix as §5.3.3.3 and Figure 6 write it, dense, and a plain elimination: what the RaptorQ
// tests and the fuzz target hold the solver's answers and verdicts to (HESAP-NET).

#pragma once

#include <crd/containers/array.hpp>
#include <crd/hesap/net/gf256.hpp>
#include <crd/hesap/net/raptorq.hpp>

#include <cstring>
#include <span>

namespace raptorq_literal
{

using crd::u32;
using crd::u8;
using crd::usize;
using crd::hesap::net::gf256_alpha_pow;
using crd::hesap::net::gf256_inv;
using crd::hesap::net::gf256_mul;
using crd::hesap::net::raptorq_rand;
using crd::hesap::net::raptorq_tuple;
using crd::hesap::net::RaptorqParameters;
using crd::hesap::net::RaptorqTuple;
using Bytes = crd::containers::Array<u8>;

// The dense matrix A of Figure 6 for the rows `isis`, each row L octets: §5.3.3.3's loops and products as written.
inline Bytes matrix(const RaptorqParameters& p, std::span<const u32> isis)
{
    const usize rows = usize{p.s} + p.h + isis.size();
    Bytes a;
    a.resize(rows * p.l);
    std::memset(a.data(), 0, a.size());
    const auto entry = [&](usize row, u32 column) -> u8&
    {
        return a[(row * p.l) + column];
    };

    for (u32 i = 0U; i < p.b; ++i)
    {
        const u32 step = 1U + (i / p.s);
        u32 b = i % p.s;
        entry(b, i) ^= 1U;
        b = (b + step) % p.s;
        entry(b, i) ^= 1U;
        b = (b + step) % p.s;
        entry(b, i) ^= 1U;
    }
    for (u32 i = 0U; i < p.s; ++i)
    {
        entry(i, p.b + i) ^= 1U;
        entry(i, p.w + (i % p.p)) ^= 1U;
        entry(i, p.w + ((i + 1U) % p.p)) ^= 1U;
    }

    const u32 columns = p.k_prime + p.s;
    for (u32 c = 0U; c < columns; ++c)
    {
        for (u32 i = c; i < columns; ++i)
        {
            const u8 gamma = gf256_alpha_pow(i - c);
            if (i + 1U == columns)
            {
                for (u32 h = 0U; h < p.h; ++h)
                {
                    entry(usize{p.s} + h, c) ^= gf256_mul(gf256_alpha_pow(h), gamma);
                }
            }
            else
            {
                const u32 h1 = raptorq_rand(i + 1U, 6U, p.h);
                const u32 h2 = (h1 + raptorq_rand(i + 1U, 7U, p.h - 1U) + 1U) % p.h;
                entry(usize{p.s} + h1, c) ^= gamma;
                entry(usize{p.s} + h2, c) ^= gamma;
            }
        }
    }
    for (u32 h = 0U; h < p.h; ++h)
    {
        entry(usize{p.s} + h, columns + h) ^= 1U;
    }

    for (usize n = 0U; n < isis.size(); ++n)
    {
        const usize row = usize{p.s} + p.h + n;
        RaptorqTuple tuple = raptorq_tuple(p, isis[n]);
        u32 b = tuple.b;
        entry(row, b) ^= 1U;
        for (u32 j = 1U; j < tuple.d; ++j)
        {
            b = (b + tuple.a) % p.w;
            entry(row, b) ^= 1U;
        }
        u32 b1 = tuple.b1;
        while (b1 >= p.p)
        {
            b1 = (b1 + tuple.a1) % p.p1;
        }
        entry(row, p.w + b1) ^= 1U;
        for (u32 j = 1U; j < tuple.d1; ++j)
        {
            b1 = (b1 + tuple.a1) % p.p1;
            while (b1 >= p.p)
            {
                b1 = (b1 + tuple.a1) % p.p1;
            }
            entry(row, p.w + b1) ^= 1U;
        }
    }
    return a;
}

// The rank of a dense octet matrix, by plain elimination.
inline u32 rank(Bytes a, usize rows, u32 columns)
{
    u32 found = 0U;
    for (u32 c = 0U; c < columns && found < rows; ++c)
    {
        usize pivot = found;
        while (pivot < rows && a[(pivot * columns) + c] == 0U)
        {
            ++pivot;
        }
        if (pivot == rows)
        {
            continue;
        }
        for (u32 j = 0U; j < columns; ++j)
        {
            const u8 swap = a[(pivot * columns) + j];
            a[(pivot * columns) + j] = a[(usize{found} * columns) + j];
            a[(usize{found} * columns) + j] = swap;
        }
        const u8 inverse = gf256_inv(a[(usize{found} * columns) + c]);
        for (usize r = usize{found} + 1U; r < rows; ++r)
        {
            const u8 factor = gf256_mul(a[(r * columns) + c], inverse);
            for (u32 j = c; factor != 0U && j < columns; ++j)
            {
                a[(r * columns) + j] ^= gf256_mul(factor, a[(usize{found} * columns) + j]);
            }
        }
        ++found;
    }
    return found;
}

} // namespace raptorq_literal
