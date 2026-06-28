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
#include <crd/core/platform.hpp> // CRD_NOINLINE
#include <crd/core/types.hpp>
#include <crd/hesap/wavelet/dwt.hpp>
#include <crd/hesap/wavelet/families.hpp>
#include <crd/memory/allocator.hpp>

#include <crd/math/cmath.hpp>

#include <cstdio> // TEMP wpt-debug: stderr pinpoint markers (remove together with the wpt_dbg calls below)

namespace crd::hesap::wavelet
{

// TEMP wpt-debug: flushed-stderr pinpoint marker for the VS2022 14.51 /O2+LTCG best_basis SegFault. Remove at closeout.
inline void wpt_dbg(const char* s) noexcept
{
    std::fputs(s, stderr);
    std::fputc('\n', stderr);
    std::fflush(stderr);
}
// TEMP wpt-debug: numeric marker (loop variable values). Remove at closeout.
inline void wpt_dbg_n2(const char* s, crd::usize a, crd::usize b) noexcept
{
    std::fprintf(stderr, "WPT-MARK: %s a=%llu b=%llu\n", s, static_cast<unsigned long long>(a),
                 static_cast<unsigned long long>(b));
    std::fflush(stderr);
}

// Additive Shannon entropy cost (MATLAB wentropy 'shannon'): -Σ s²·log(s²), with 0·log0 := 0. Additive across
// a partition ⇒ usable for best-basis. Lower = more concentrated (a better basis for the data).
template <typename T> [[nodiscard]] CRD_NOINLINE T shannon_entropy_cost(crd::containers::ConstSpan<T> s) noexcept
{
    T cost = T(0);
    for (crd::usize i = 0; i < s.size(); ++i)
    {
        const T e = s[i] * s[i];
        if (e > T(0))
        {
            cost -= e * crd::math::log(e);
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
        // Decompose every internal node top-down by heap id (children 2*id+1, 2*id+2). Iterating by id avoids the
        // inline `1 << level` count, which MSVC 19.51/14.51 (/O2+LTCG) miscompiles to a garbage value here. See SANITY.md.
        const crd::usize internal_count = (crd::usize{1} << maxlevel) - 1;
        for (crd::usize id = 0; id < internal_count; ++id)
        {
            crd::containers::Array<T> cA(alloc), cD(alloc);
            dwt<T>(alloc, crd::containers::ConstSpan<T>(m_nodes[id].data(), m_nodes[id].size()), w, mode, cA, cD);
            m_nodes[2 * id + 1] = std::move(cA);
            m_nodes[2 * id + 2] = std::move(cD);
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
        wpt_dbg("WPT-MARK: bb: arrays sized"); // TEMP wpt-debug
        wpt_dbg_n2("bb: sizes total/maxlevel", total, m_maxlevel); // TEMP wpt-debug
        // bottom level: keep the leaves.
        const crd::usize leaf_count = crd::usize{1} << m_maxlevel;
        for (crd::usize idx = 0; idx < leaf_count; ++idx)
        {
            const crd::usize id = node_id(m_maxlevel, idx);
            best[id] = node_cost(id);
            split[id] = 0;
        }
        wpt_dbg("WPT-MARK: bb: leaf loop done"); // TEMP wpt-debug
        // upward: process internal nodes bottom-up by heap id (children 2*id+1, 2*id+2 are deeper, already computed).
        // Iterating by id deliberately avoids the inline `1 << level` count: MSVC 19.51/14.51 (/O2+LTCG) miscompiled
        // that shift here to a garbage value (~a stack address), so the inner loop ran ~1e14 times -> OOB -> SegFault.
        // See docs/SANITY.md.
        const crd::usize internal_count = total - leaf_count;
        for (crd::usize k = internal_count; k > 0; --k)
        {
            const crd::usize id = k - 1;
            wpt_dbg("WPT-MARK: bb: up cost"); // TEMP wpt-debug
            const T own = node_cost(id);
            const T child = best[2 * id + 1] + best[2 * id + 2];
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
        wpt_dbg("WPT-MARK: bb: upward loop done"); // TEMP wpt-debug
        out_levels.clear();
        out_indices.clear();
        wpt_dbg("WPT-MARK: bb: collect begin"); // TEMP wpt-debug
        collect_basis(0, 0, split, out_levels, out_indices);
        wpt_dbg("WPT-MARK: bb: collect done"); // TEMP wpt-debug
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
        crd::usize count = leaf_count;
        for (crd::usize l = m_maxlevel; l > 0; --l)
        {
            count /= 2; // node count at this level (2^(l-1)); avoids the MSVC-miscompiled inline `1 << level`
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
