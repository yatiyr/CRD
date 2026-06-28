#pragma once

// v12-p — Streaming / online estimators (crd-hesap-stats). Welford running mean/variance · P-square (Jain-Chlamtac)
// online quantile · HyperLogLog cardinality · count-min frequency sketch · t-digest (in tdigest.hpp). The exact ones
// (Welford, P-square) gate bit-for-bit vs the batch / reference algorithm; the probabilistic sketches gate within their
// accuracy guarantees vs the true value. Gold: numpy · the published algorithms · datasketch.

#include <crd/hesap/stats/descriptive.hpp> // Real concept

#include <crd/containers/array.hpp>
#include <crd/containers/sort.hpp>
#include <crd/core/types.hpp>
#include <crd/math/cmath.hpp>
#include <crd/memory/allocator.hpp>

#include <bit>

namespace crd::hesap::stats
{

// Welford's online mean/variance (numerically stable, single pass). variance(ddof) = M2/(n-ddof).
template <Real T> class Welford
{
public:
    void add(T x) noexcept
    {
        ++m_n;
        const T d = x - m_mean;
        m_mean += d / static_cast<T>(m_n);
        m_m2 += d * (x - m_mean);
    }
    [[nodiscard]] crd::usize count() const noexcept { return m_n; }
    [[nodiscard]] T mean() const noexcept { return m_mean; }
    [[nodiscard]] T variance(crd::usize ddof = 1) const noexcept { return m_m2 / static_cast<T>(m_n - ddof); }
    [[nodiscard]] T stddev(crd::usize ddof = 1) const noexcept { return crd::math::sqrt(variance(ddof)); }

private:
    crd::usize m_n = 0;
    T m_mean = static_cast<T>(0);
    T m_m2 = static_cast<T>(0);
};

// P-square online single-quantile estimator (Jain & Chlamtac 1985): five markers tracked with parabolic / linear
// adjustment. O(1) memory, no data retained. Deterministic (basic ops only) — matches the reference algorithm exactly.
template <Real T> class P2Quantile
{
public:
    explicit P2Quantile(T p) noexcept : m_p(p) {}

    void add(T x) noexcept
    {
        if (m_count < 5)
        {
            m_buf[m_count] = x;
            ++m_count;
            if (m_count == 5)
            {
                crd::containers::sort(m_buf, m_buf + 5);
                for (int i = 0; i < 5; ++i)
                {
                    m_q[i] = m_buf[i];
                    m_n[i] = static_cast<T>(i + 1);
                }
                m_np[0] = static_cast<T>(1);
                m_np[1] = static_cast<T>(1) + static_cast<T>(2) * m_p;
                m_np[2] = static_cast<T>(1) + static_cast<T>(4) * m_p;
                m_np[3] = static_cast<T>(3) + static_cast<T>(2) * m_p;
                m_np[4] = static_cast<T>(5);
                m_dn[0] = static_cast<T>(0);
                m_dn[1] = m_p / static_cast<T>(2);
                m_dn[2] = m_p;
                m_dn[3] = (static_cast<T>(1) + m_p) / static_cast<T>(2);
                m_dn[4] = static_cast<T>(1);
            }
            return;
        }
        ++m_count;
        int k = 0;
        if (x < m_q[0])
        {
            m_q[0] = x;
            k = 0;
        }
        else if (x < m_q[1])
        {
            k = 0;
        }
        else if (x < m_q[2])
        {
            k = 1;
        }
        else if (x < m_q[3])
        {
            k = 2;
        }
        else if (x <= m_q[4])
        {
            k = 3;
        }
        else
        {
            m_q[4] = x;
            k = 3;
        }
        for (int i = k + 1; i < 5; ++i)
        {
            m_n[i] += static_cast<T>(1);
        }
        for (int i = 0; i < 5; ++i)
        {
            m_np[i] += m_dn[i];
        }
        for (int i = 1; i < 4; ++i)
        {
            const T d = m_np[i] - m_n[i];
            if ((d >= static_cast<T>(1) && m_n[i + 1] - m_n[i] > static_cast<T>(1)) ||
                (d <= static_cast<T>(-1) && m_n[i - 1] - m_n[i] < static_cast<T>(-1)))
            {
                const int s = d >= static_cast<T>(0) ? 1 : -1;
                const T sd = static_cast<T>(s);
                const T qp = m_q[i] + sd / (m_n[i + 1] - m_n[i - 1]) *
                                          ((m_n[i] - m_n[i - 1] + sd) * (m_q[i + 1] - m_q[i]) / (m_n[i + 1] - m_n[i]) +
                                           (m_n[i + 1] - m_n[i] - sd) * (m_q[i] - m_q[i - 1]) / (m_n[i] - m_n[i - 1]));
                if (m_q[i - 1] < qp && qp < m_q[i + 1])
                {
                    m_q[i] = qp;
                }
                else
                {
                    m_q[i] = m_q[i] + sd * (m_q[i + s] - m_q[i]) / (m_n[i + s] - m_n[i]);
                }
                m_n[i] += sd;
            }
        }
    }

    [[nodiscard]] T quantile() const noexcept { return m_count >= 5 ? m_q[2] : m_buf[m_count / 2]; }

private:
    T m_p;
    crd::usize m_count = 0;
    T m_buf[5] = {};
    T m_q[5] = {};
    T m_n[5] = {};
    T m_np[5] = {};
    T m_dn[5] = {};
};

namespace detail
{
[[nodiscard]] inline crd::u64 splitmix64_mix(crd::u64 z) noexcept
{
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}
} // namespace detail

// HyperLogLog cardinality estimator (Flajolet et al.), with linear-counting small-range and large-range corrections.
// 2^Precision registers; relative error ~ 1.04 / sqrt(2^Precision).
template <int Precision = 11> class HyperLogLog
{
public:
    explicit HyperLogLog(crd::memory::IAllocator* alloc) : m_reg(alloc)
    {
        m_reg.resize(static_cast<crd::usize>(1) << Precision);
        for (crd::usize i = 0; i < m_reg.size(); ++i)
        {
            m_reg[i] = 0;
        }
    }

    void add(crd::u64 key) noexcept
    {
        const crd::u64 h = detail::splitmix64_mix(key);
        const crd::usize idx = static_cast<crd::usize>(h >> (64 - Precision));
        const crd::u64 w = (h << Precision) | (static_cast<crd::u64>(1) << (Precision - 1));
        const crd::u8 rho = static_cast<crd::u8>(std::countl_zero(w) + 1);
        if (rho > m_reg[idx])
        {
            m_reg[idx] = rho;
        }
    }

    [[nodiscard]] crd::f64 estimate() const noexcept
    {
        constexpr crd::f64 m = static_cast<crd::f64>(static_cast<crd::u64>(1) << Precision);
        crd::f64 sum = 0.0;
        crd::usize zeros = 0;
        for (crd::usize j = 0; j < m_reg.size(); ++j)
        {
            sum += 1.0 / static_cast<crd::f64>(static_cast<crd::u64>(1) << m_reg[j]);
            if (m_reg[j] == 0)
            {
                ++zeros;
            }
        }
        const crd::f64 alpha = 0.7213 / (1.0 + 1.079 / m);
        crd::f64 e = alpha * m * m / sum;
        if (e <= 2.5 * m && zeros > 0)
        {
            e = m * crd::math::log(m / static_cast<crd::f64>(zeros)); // linear counting
        }
        else
        {
            constexpr crd::f64 two32 = 4294967296.0;
            if (e > two32 / 30.0)
            {
                e = -two32 * crd::math::log(1.0 - e / two32);
            }
        }
        return e;
    }

private:
    crd::containers::Array<crd::u8> m_reg;
};

// Count-min sketch: D x W counters, D independent hashes; query returns the min counter (an upper bound on the true
// frequency, exceeding it by at most ~e/W * total with high probability).
template <int D = 4, int W = 2048> class CountMinSketch
{
public:
    explicit CountMinSketch(crd::memory::IAllocator* alloc) : m_table(alloc)
    {
        m_table.resize(static_cast<crd::usize>(D) * W);
        for (crd::usize i = 0; i < m_table.size(); ++i)
        {
            m_table[i] = 0;
        }
        for (int d = 0; d < D; ++d)
        {
            m_seed[d] = detail::splitmix64_mix(static_cast<crd::u64>(d) + 0x9e3779b97f4a7c15ULL);
        }
    }

    void add(crd::u64 key, crd::u32 cnt = 1) noexcept
    {
        for (int d = 0; d < D; ++d)
        {
            m_table[static_cast<crd::usize>(d) * W + col(key, d)] += cnt;
        }
    }

    [[nodiscard]] crd::u32 query(crd::u64 key) const noexcept
    {
        crd::u32 m = 0xFFFFFFFFU;
        for (int d = 0; d < D; ++d)
        {
            const crd::u32 v = m_table[static_cast<crd::usize>(d) * W + col(key, d)];
            if (v < m)
            {
                m = v;
            }
        }
        return m;
    }

private:
    [[nodiscard]] crd::usize col(crd::u64 key, int d) const noexcept
    {
        return static_cast<crd::usize>(detail::splitmix64_mix(key ^ m_seed[d]) % static_cast<crd::u64>(W));
    }
    crd::containers::Array<crd::u32> m_table;
    crd::u64 m_seed[D] = {};
};

} // namespace crd::hesap::stats
