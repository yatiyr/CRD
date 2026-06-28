#pragma once

// v12-p — t-digest (Dunning) online quantile sketch. The merging variant: incoming points buffer, then merge into
// mean/weight centroids bounded by the arcsin scale function k(q) = (compression/2pi) asin(2q-1), so each centroid spans
// at most Δk = 1 of scale. Tail centroids stay weight-~1 → high accuracy at extreme quantiles. A bounded-memory,
// mergeable sketch; gated within its accuracy guarantee vs the true quantiles. Gold: the `tdigest` package / Dunning ref.

#include <crd/hesap/stats/descriptive.hpp> // Real, detail::kPi

#include <crd/containers/array.hpp>
#include <crd/containers/sort.hpp>
#include <crd/core/types.hpp>
#include <crd/math/cmath.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::hesap::stats
{

template <Real T> struct TdCentroid
{
    T mean;
    T weight;
};

template <Real T> class TDigest
{
public:
    explicit TDigest(crd::memory::IAllocator* alloc, T compression = static_cast<T>(100))
        : m_cent(alloc), m_buf(alloc), m_scratch(alloc), m_compression(compression), m_alloc(alloc)
    {
    }

    void add(T x, T w = static_cast<T>(1))
    {
        m_buf.push_back(TdCentroid<T>{x, w});
        m_total += w;
        if (static_cast<T>(m_buf.size() + m_cent.size()) > static_cast<T>(10) * m_compression)
        {
            compress();
        }
    }

    [[nodiscard]] T quantile(T q)
    {
        compress();
        if (m_cent.empty())
        {
            return static_cast<T>(0);
        }
        if (m_cent.size() == 1)
        {
            return m_cent[0].mean;
        }
        const T target = q * m_total;
        T cum = static_cast<T>(0);
        for (crd::usize i = 0; i < m_cent.size(); ++i)
        {
            const T w = m_cent[i].weight;
            const T center = cum + w / static_cast<T>(2);
            if (target < center)
            {
                if (i == 0)
                {
                    return m_cent[0].mean;
                }
                const T prev_center = cum - m_cent[i - 1].weight / static_cast<T>(2);
                const T frac = (target - prev_center) / (center - prev_center);
                return m_cent[i - 1].mean + frac * (m_cent[i].mean - m_cent[i - 1].mean);
            }
            cum += w;
        }
        return m_cent[m_cent.size() - 1].mean;
    }

private:
    [[nodiscard]] T scale(T q) const
    {
        T u = static_cast<T>(2) * q - static_cast<T>(1);
        u = (u < static_cast<T>(-1)) ? static_cast<T>(-1) : ((u > static_cast<T>(1)) ? static_cast<T>(1) : u);
        return m_compression / (static_cast<T>(2) * detail::kPi<T>) * crd::math::asin(u);
    }

    void compress()
    {
        if (m_buf.empty())
        {
            return;
        }
        for (crd::usize i = 0; i < m_buf.size(); ++i)
        {
            m_cent.push_back(m_buf[i]);
        }
        m_buf.clear();
        crd::containers::sort(m_cent.data(), m_cent.data() + m_cent.size(),
                              [](const TdCentroid<T>& a, const TdCentroid<T>& b) { return a.mean < b.mean; });
        m_scratch.clear();
        const T total = m_total;
        T cum = static_cast<T>(0);
        TdCentroid<T> cur = m_cent[0];
        for (crd::usize i = 1; i < m_cent.size(); ++i)
        {
            const T proposed = cur.weight + m_cent[i].weight;
            const T q_left = cum / total;
            const T q_right = (cum + proposed) / total;
            if (scale(q_right) - scale(q_left) <= static_cast<T>(1))
            {
                cur.mean = (cur.mean * cur.weight + m_cent[i].mean * m_cent[i].weight) / proposed;
                cur.weight = proposed;
            }
            else
            {
                m_scratch.push_back(cur);
                cum += cur.weight;
                cur = m_cent[i];
            }
        }
        m_scratch.push_back(cur);
        m_cent.clear();
        for (crd::usize i = 0; i < m_scratch.size(); ++i)
        {
            m_cent.push_back(m_scratch[i]);
        }
    }

    crd::containers::Array<TdCentroid<T>> m_cent;
    crd::containers::Array<TdCentroid<T>> m_buf;
    crd::containers::Array<TdCentroid<T>> m_scratch;
    T m_compression;
    T m_total = static_cast<T>(0);
    crd::memory::IAllocator* m_alloc;
};

} // namespace crd::hesap::stats
