#pragma once

// crd-hesap-stats v12-e — Threefry-4×64-20 (Salmon-Moraes-Dror-Shaw, Random123): a counter-based RNG built from the
// Threefish-256 mix (no S-boxes; pure ARX). Like Philox it is PURE — output depends only on (counter, key) — so
// parallel streams are bit-identical and trivially seekable (the determinism moat). MATLAB's `rng(...,'threefry')`.
// Gated vs the published Random123 known-answer vectors.

#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/stats/counter_rng_core.hpp> // ThreefryBlock, threefry4x64, threefry_counter_add
#include <crd/hesap/stats/stats_dispatch.hpp>

namespace crd::hesap::stats
{

// Counter-RNG wrapper: a seekable u64 stream (key = seed/stream, counter auto-incremented; 4 u64 per block).
class ThreefryRng
{
public:
    explicit ThreefryRng(crd::u64 seed, crd::u64 stream = 0) noexcept
    {
        m_key[0] = seed;
        m_key[1] = stream;
    }

    [[nodiscard]] crd::u64 next_u64() noexcept
    {
        if (m_idx == 4U)
        {
            refill();
        }
        return m_block.v[m_idx++];
    }

    // Bulk fill, bit-identical to repeated next_u64(): whole blocks come from the stats kernel level the process runs
    // (crd/hesap/stats/stats_dispatch.hpp). The crush path vs NumPy/MATLAB.
    void fill(crd::containers::Span<crd::u64> out) noexcept
    {
        crd::usize i = 0;
        while (m_idx != 4U && i < out.size()) // drain the partial buffer first
        {
            out[i++] = next_u64();
        }
        const crd::usize blocks = (out.size() - i) / 4U;
        if (blocks != 0U)
        {
            stats_kernels().table->threefry4x64_blocks(m_key, m_counter, &out[i], blocks);
            threefry_counter_add(m_counter, blocks);
            i += 4U * blocks;
        }
        while (i < out.size()) // the tail: the start of the next block
        {
            out[i++] = next_u64();
        }
    }

    // Seek to the n-th u64 (O(1) — counter-based). The counter names the block after the buffered one.
    void seek(crd::u64 n) noexcept
    {
        m_counter[0] = n >> 2;
        m_counter[1] = 0;
        m_counter[2] = 0;
        m_counter[3] = 0;
        refill_at(static_cast<crd::u32>(n & 3U));
    }

private:
    void refill() noexcept { refill_at(0U); }

    void refill_at(crd::u32 idx) noexcept
    {
        m_block = threefry4x64(m_counter, m_key);
        threefry_counter_add(m_counter, 1U);
        m_idx = idx;
    }

    crd::u64 m_key[4] = {0U, 0U, 0U, 0U}; // (seed, stream, 0, 0)
    crd::u64 m_counter[4] = {0U, 0U, 0U, 0U};
    ThreefryBlock m_block{{0U, 0U, 0U, 0U}};
    crd::u32 m_idx = 4U;
};

} // namespace crd::hesap::stats
