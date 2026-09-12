#pragma once

// minibatch.hpp — Phase 3.1.6 v7-i: the REPRODUCIBLE minibatch sampler, built on the Philox counter-based RNG
// (crd-hesap-stats, the v12-pull). The reproducibility contract is BY CONSTRUCTION, not by careful state
// management: the epoch-e permutation is the Fisher-Yates shuffle driven by `PhiloxRng(seed, stream = e)` — a
// pure function of (seed, epoch) — so the SAME (seed, epoch) yields the SAME batches no matter how many
// workers run, what ran before, or whether epochs are visited in order (replay can jump straight to epoch 17).
// Combined with the serial steppers (stochastic.hpp) and a bit-exact gradient, the whole TRAINING TRAJECTORY
// is bit-identical across {1..16} workers — the moat no mainstream training loop carries. ADR-0090.

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/stats/philox.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::hesap::opt
{

class MinibatchSampler
{
public:
    MinibatchSampler(crd::memory::IAllocator* alloc, crd::usize num_samples, crd::usize batch_size, crd::u64 seed)
        : m_indices(alloc), m_num_samples(num_samples), m_batch_size(batch_size), m_seed(seed)
    {
        CRD_ASSERT_MSG(batch_size >= 1, "MinibatchSampler: batch_size must be >= 1");
        m_indices.resize(num_samples);
        for (crd::usize i = 0; i < num_samples; ++i)
        {
            m_indices[i] = static_cast<crd::u32>(i);
        }
    }

    // Build the epoch-`epoch` permutation — a pure function of (seed, epoch); any epoch, in any order.
    void begin_epoch(crd::u64 epoch)
    {
        for (crd::usize i = 0; i < m_num_samples; ++i)
        {
            m_indices[i] = static_cast<crd::u32>(i);
        }
        crd::hesap::stats::PhiloxRng rng(m_seed, /*stream=*/epoch);
        crd::hesap::stats::shuffle<crd::u32>({m_indices.data(), m_num_samples}, rng);
    }

    [[nodiscard]] crd::usize num_batches() const noexcept
    {
        return (m_num_samples + m_batch_size - 1) / m_batch_size; // the last batch may be short
    }

    // Sample indices of batch k within the current epoch's permutation.
    [[nodiscard]] crd::containers::ConstSpan<crd::u32> batch(crd::usize k) const noexcept
    {
        CRD_ASSERT_MSG(k < num_batches(), "MinibatchSampler::batch: batch index out of range");
        const crd::usize lo = k * m_batch_size;
        const crd::usize hi = lo + m_batch_size < m_num_samples ? lo + m_batch_size : m_num_samples;
        return {m_indices.data() + lo, hi - lo};
    }

    [[nodiscard]] crd::usize num_samples() const noexcept { return m_num_samples; }
    [[nodiscard]] crd::usize batch_size() const noexcept { return m_batch_size; }

private:
    crd::containers::Array<crd::u32> m_indices;
    crd::usize m_num_samples;
    crd::usize m_batch_size;
    crd::u64 m_seed;
};

} // namespace crd::hesap::opt
