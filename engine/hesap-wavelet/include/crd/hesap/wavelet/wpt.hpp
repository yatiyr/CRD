#pragma once

// ---------------------------------------------------------------------------
// crd-hesap-wavelet v11w-c — wavelet packet transform (full binary tree).
//
//   WaveletPacket   decompose x into the full packet tree to `maxlevel`: each
//                   node is split by BOTH the low ('a') and high ('d') filters
//                   (unlike the DWT, which only recurses on the approximation).
//                   node("ad") = dwt(dwt(x).cA).cD, etc. (pywt naming).
//   best_basis      Coifman-Wickerhauser best-basis selection by an additive
//                   Shannon entropy cost (the minimal-cost disjoint cover).
//   reconstruct     inverse from the deepest level (idwt up the tree).
//
// Gate (ADR-0093): node coefficients vs pywt.WaveletPacket + reconstruct round
// trip (perfect reconstruction) + best-basis cost optimality (≤ any fixed level)
// + run-twice bit-identical. Periodization mode (lengths halve each level).
// ---------------------------------------------------------------------------

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/wavelet/dwt.hpp>
#include <crd/hesap/wavelet/families.hpp>
#include <crd/memory/allocator.hpp>

#include <cmath>

namespace crd::hesap::wavelet
{

// Additive Shannon entropy cost (MATLAB wentropy 'shannon'): -Σ s²·log(s²), with 0·log0 := 0. Additive across
// a partition ⇒ usable for best-basis. Lower = more concentrated (a better basis for the data).
template <typename T> [[nodiscard]] T shannon_entropy_cost(crd::containers::ConstSpan<T> s) noexcept
{
    T cost = T(0);
    for (crd::usize i = 0; i < s.size(); ++i)
    {
        const T e = s[i] * s[i];
        if (e > T(0))
        {
            cost -= e * std::log(e);
        }
    }
    return cost;
}

template <typename T> class WaveletPacket
{
public:
    // Full-tree decomposition of x to maxlevel. n must be a multiple of 2^maxlevel (periodization).
    WaveletPacket(crd::memory::IAllocator* alloc, crd::containers::ConstSpan<T> x, const Wavelet& w,
                  SignalExtensionMode mode, crd::usize maxlevel)
        : m_nodes(alloc), m_maxlevel(maxlevel), m_mode(mode)
    {
        const crd::usize total = (crd::usize{1} << (maxlevel + 1)) - 1; // heap-indexed nodes
        m_nodes.reserve(total);
        for (crd::usize i = 0; i < total; ++i)
        {
            m_nodes.push_back(crd::containers::Array<T>(alloc));
        }
        m_nodes[0].resize(x.size()); // root = x
        for (crd::usize i = 0; i < x.size(); ++i)
        {
            m_nodes[0][i] = x[i];
        }
        for (crd::usize level = 0; level < maxlevel; ++level)
        {
            const crd::usize count = crd::usize{1} << level;
            for (crd::usize idx = 0; idx < count; ++idx)
            {
                const crd::usize pid = node_id(level, idx);
                crd::containers::Array<T> cA(alloc), cD(alloc);
                dwt<T>(alloc, crd::containers::ConstSpan<T>(m_nodes[pid].data(), m_nodes[pid].size()), w, mode, cA, cD);
                m_nodes[node_id(level + 1, 2 * idx)] = std::move(cA);
                m_nodes[node_id(level + 1, 2 * idx + 1)] = std::move(cD);
            }
        }
    }

    [[nodiscard]] crd::usize max_level() const noexcept { return m_maxlevel; }

    // Coefficients of a node by path ("a","d","aa","ad","aaa",...). Empty path = the root signal.
    [[nodiscard]] crd::containers::ConstSpan<T> node(crd::containers::StringView path) const noexcept
    {
        crd::usize level = 0, idx = 0;
        for (char c : path)
        {
            idx = idx * 2 + (c == 'd' ? 1U : 0U);
            ++level;
        }
        const crd::containers::Array<T>& a = m_nodes[node_id(level, idx)];
        return crd::containers::ConstSpan<T>(a.data(), a.size());
    }

    // Best-basis: the minimal-Shannon-cost disjoint set of nodes covering the signal. Returns the chosen node
    // (level, idx) pairs via the out-arrays; also returns the total best cost.
    [[nodiscard]] T best_basis(crd::memory::IAllocator* alloc, crd::containers::Array<crd::usize>& out_levels,
                               crd::containers::Array<crd::usize>& out_indices) const
    {
        const crd::usize total = m_nodes.size();
        crd::containers::Array<T> best(alloc);
        crd::containers::Array<crd::u8> split(alloc); // 1 = use children, 0 = keep this node
        best.resize(total);
        split.resize(total);
        // bottom level: keep the leaves.
        const crd::usize leaf_count = crd::usize{1} << m_maxlevel;
        for (crd::usize idx = 0; idx < leaf_count; ++idx)
        {
            const crd::usize id = node_id(m_maxlevel, idx);
            best[id] = node_cost(id);
            split[id] = 0;
        }
        // upward: compare own cost vs children's best total.
        for (crd::usize level = m_maxlevel; level-- > 0;)
        {
            const crd::usize count = crd::usize{1} << level;
            for (crd::usize idx = 0; idx < count; ++idx)
            {
                const crd::usize id = node_id(level, idx);
                const T own = node_cost(id);
                const T child = best[node_id(level + 1, 2 * idx)] + best[node_id(level + 1, 2 * idx + 1)];
                if (own <= child)
                {
                    best[id] = own;
                    split[id] = 0;
                }
                else
                {
                    best[id] = child;
                    split[id] = 1;
                }
            }
        }
        out_levels.clear();
        out_indices.clear();
        collect_basis(0, 0, split, out_levels, out_indices);
        return best[0];
    }

    // Full reconstruction from the deepest level up (idwt the children back into the parent). Round-trip recovers x.
    [[nodiscard]] crd::containers::Array<T> reconstruct(crd::memory::IAllocator* alloc, const Wavelet& w) const
    {
        crd::containers::Array<crd::containers::Array<T>> cur(alloc); // current level's node arrays
        const crd::usize leaf_count = crd::usize{1} << m_maxlevel;
        cur.reserve(leaf_count);
        for (crd::usize idx = 0; idx < leaf_count; ++idx)
        {
            const crd::containers::Array<T>& src = m_nodes[node_id(m_maxlevel, idx)];
            crd::containers::Array<T> a(alloc);
            a.resize(src.size());
            for (crd::usize i = 0; i < src.size(); ++i)
            {
                a[i] = src[i];
            }
            cur.push_back(std::move(a));
        }
        for (crd::usize level = m_maxlevel; level-- > 0;)
        {
            const crd::usize count = crd::usize{1} << level;
            crd::containers::Array<crd::containers::Array<T>> next(alloc);
            next.reserve(count);
            for (crd::usize idx = 0; idx < count; ++idx)
            {
                crd::containers::Array<T> rec(alloc);
                idwt<T>(alloc, crd::containers::ConstSpan<T>(cur[2 * idx].data(), cur[2 * idx].size()),
                        crd::containers::ConstSpan<T>(cur[2 * idx + 1].data(), cur[2 * idx + 1].size()), w, m_mode,
                        rec);
                next.push_back(std::move(rec));
            }
            cur = std::move(next);
        }
        return std::move(cur[0]);
    }

private:
    [[nodiscard]] static crd::usize node_id(crd::usize level, crd::usize idx) noexcept
    {
        return ((crd::usize{1} << level) - 1) + idx;
    }
    [[nodiscard]] T node_cost(crd::usize id) const noexcept
    {
        return shannon_entropy_cost<T>(crd::containers::ConstSpan<T>(m_nodes[id].data(), m_nodes[id].size()));
    }
    void collect_basis(crd::usize level, crd::usize idx, const crd::containers::Array<crd::u8>& split,
                       crd::containers::Array<crd::usize>& levels, crd::containers::Array<crd::usize>& indices) const
    {
        const crd::usize id = node_id(level, idx);
        if (level == m_maxlevel || split[id] == 0)
        {
            levels.push_back(level);
            indices.push_back(idx);
            return;
        }
        collect_basis(level + 1, 2 * idx, split, levels, indices);
        collect_basis(level + 1, 2 * idx + 1, split, levels, indices);
    }

    crd::containers::Array<crd::containers::Array<T>> m_nodes;
    crd::usize m_maxlevel;
    SignalExtensionMode m_mode;
};

} // namespace crd::hesap::wavelet
