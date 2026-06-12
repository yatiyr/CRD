#pragma once

// solution.hpp — Phase 3.1.6 v9-c: `OdeSolution<T>` — the continuous trajectory record. Adaptive drivers
// append a node (t, y, f) per ACCEPTED step; evaluation interpolates between adjacent nodes with the
// v9-a cubic-Hermite fallback (dense_output.hpp). Per the ADR-0091 dense-output contract the storage is
// contiguous and caller/driver-owned (no per-step allocation beyond the Array growth). NATIVE per-method
// interpolants (RK45 quartic, DOP853 7th-order) slot in behind the same eval surface later (named in the
// v9 plan); Hermite is 3rd-order — O(h⁴) interpolation error, C¹ across steps. ADR-0091.

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/ode/dense_output.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::hesap::ode
{

template <typename T> class OdeSolution
{
public:
    explicit OdeSolution(crd::memory::IAllocator* alloc) noexcept : m_ts(alloc), m_ys(alloc), m_fs(alloc), m_n(0) {}

    void reset(crd::usize n)
    {
        m_n = n;
        m_ts.clear();
        m_ys.clear();
        m_fs.clear();
    }

    // Append a node (the driver calls this at t0 and after every accepted step). y/f sized n.
    void append(T t, crd::containers::ConstSpan<T> y, crd::containers::ConstSpan<T> f)
    {
        CRD_ASSERT(y.size() == m_n && f.size() == m_n);
        m_ts.push_back(t);
        for (crd::usize i = 0; i < m_n; ++i)
        {
            m_ys.push_back(y[i]);
        }
        for (crd::usize i = 0; i < m_n; ++i)
        {
            m_fs.push_back(f[i]);
        }
    }

    [[nodiscard]] crd::usize num_nodes() const noexcept { return m_ts.size(); }
    [[nodiscard]] crd::usize dim() const noexcept { return m_n; }
    [[nodiscard]] T t_node(crd::usize i) const { return m_ts[i]; }
    [[nodiscard]] crd::containers::ConstSpan<T> y_node(crd::usize i) const
    {
        return crd::containers::ConstSpan<T>(m_ys.data() + i * m_n, m_n);
    }
    [[nodiscard]] crd::containers::ConstSpan<T> f_node(crd::usize i) const
    {
        return crd::containers::ConstSpan<T>(m_fs.data() + i * m_n, m_n);
    }

    // Evaluate y(t). t is clamped to the recorded span (scipy OdeSolution semantics: extrapolation is
    // clamped interpolation on the boundary segment). Handles forward AND backward trajectories.
    void eval(T t, crd::containers::Span<T> y_out) const
    {
        CRD_ASSERT(y_out.size() == m_n);
        const crd::usize m = m_ts.size();
        CRD_ASSERT(m >= 1);
        if (m == 1)
        {
            const crd::containers::ConstSpan<T> y0 = y_node(0);
            for (crd::usize i = 0; i < m_n; ++i)
            {
                y_out[i] = y0[i];
            }
            return;
        }
        const crd::usize seg = locate_segment(t);
        hermite_eval(m_ts[seg], m_ts[seg + 1], y_node(seg), f_node(seg), y_node(seg + 1), f_node(seg + 1), t, y_out);
    }

    // The segment index s such that t lies in [ts[s], ts[s+1]] (direction-aware, clamped, deterministic
    // binary search).
    [[nodiscard]] crd::usize locate_segment(T t) const
    {
        const crd::usize m = m_ts.size();
        const bool ascending = m_ts[m - 1] >= m_ts[0];
        crd::usize lo = 0;
        crd::usize hi = m - 1; // search over node indices; result segment = lo of the final bracket
        while (hi - lo > 1)
        {
            const crd::usize mid = lo + (hi - lo) / 2;
            const bool go_right = ascending ? (m_ts[mid] <= t) : (m_ts[mid] >= t);
            if (go_right)
            {
                lo = mid;
            }
            else
            {
                hi = mid;
            }
        }
        return lo;
    }

private:
    crd::containers::Array<T> m_ts;
    crd::containers::Array<T> m_ys;
    crd::containers::Array<T> m_fs;
    crd::usize m_n;
};

} // namespace crd::hesap::ode
