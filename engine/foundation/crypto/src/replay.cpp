// crd-crypto -- anti-replay (NET.2, threat T07; contract: docs/design/network-foundation.md#anti-replay).
//
// The filter of single-use identifiers; the sequence number window is a template, whole in replay.hpp.

#include <crd/crypto/replay.hpp>

#include <atomic>

namespace crd::crypto
{
namespace
{
constexpr usize kItemsPerBlock = 32U; // two bytes an item in 64-byte blocks
constexpr u64   kSlotPreparing = ~u64{0};
} // namespace

CryptoError ReplayFilter::init(usize items, u64 period, u64 now)
{
    clear();
    if (items == 0U || period == 0U)
    {
        return CryptoError::BadLength;
    }
    const u64 blocks = static_cast<u64>(items / kItemsPerBlock) + (items % kItemsPerBlock != 0U ? 1U : 0U);
    if (blocks > (u64{1} << 32U))
    {
        return CryptoError::BadLength;
    }
    m_bits.resize(static_cast<usize>(blocks) * kSlots);
    m_blocks        = static_cast<usize>(blocks);
    m_period        = period;
    const u64 epoch = now / period;
    for (u64 e = epoch == 0U ? 0U : epoch - 1U; e <= epoch + 2U; ++e)
    {
        m_tags[e % kSlots] = e + 1U;
    }
    return CryptoError::None;
}

void ReplayFilter::clear() noexcept
{
    m_bits   = cont::Array<Block>();
    m_blocks = 0U;
    m_period = 0U;
    for (u64& tag : m_tags)
    {
        tag = 0U;
    }
}

// Readies the slot of `epoch` when it holds an older period, every identifier of which has expired: one thread claims
// it, clears its bits and publishes it. A slot already ready, newer or being cleared is left as it is.
void ReplayFilter::prepare(u64 epoch) noexcept
{
    const usize          slot = static_cast<usize>(epoch % kSlots);
    std::atomic_ref<u64> tag(m_tags[slot]);
    u64                  held = tag.load(std::memory_order_acquire);
    if (held == kSlotPreparing || held >= epoch + 1U ||
        !tag.compare_exchange_strong(held, kSlotPreparing, std::memory_order_acq_rel))
    {
        return;
    }
    Block* const first = m_bits.data() + slot * m_blocks;
    for (usize b = 0U; b < m_blocks; ++b)
    {
        for (u64& word : first[b].words)
        {
            std::atomic_ref<u64>(word).store(0U, std::memory_order_relaxed);
        }
    }
    tag.store(epoch + 1U, std::memory_order_release);
}

bool ReplayFilter::first_use(cont::ConstSpan<u8> id, u64 issued, u64 now) noexcept
{
    if (!ready() || id.size() != kReplayIdBytes)
    {
        return false;
    }
    prepare(now / m_period + 2U);
    const u64            epoch = issued / m_period;
    const usize          slot  = static_cast<usize>(epoch % kSlots);
    std::atomic_ref<u64> tag(m_tags[slot]);
    u64                  held = tag.load(std::memory_order_acquire);
    if (held != kSlotPreparing && held < epoch + 1U)
    {
        prepare(epoch); // a whole period passed without a check
        held = tag.load(std::memory_order_acquire);
    }
    if (held != epoch + 1U)
    {
        return held == kSlotPreparing; // being cleared: accepted unrecorded; a newer period: too old for any slot
    }
    u64 low  = 0U;
    u64 high = 0U;
    for (usize i = 0U; i < 8U; ++i)
    {
        low |= static_cast<u64>(id[i]) << (8U * i);
        high |= static_cast<u64>(id[8U + i]) << (8U * i);
    }
    // The identifier is random and authenticated, so its bits index the filter directly: the top 32 bits of its first
    // half pick the block (a multiply-shift onto at most 2^32 blocks), 48 bits of its second half one bit in each word.
    const usize block = static_cast<usize>(((low >> 32U) * static_cast<u64>(m_blocks)) >> 32U);
    Block&      bits  = m_bits[slot * m_blocks + block];
    bool        fresh = false;
    for (usize i = 0U; i < 8U; ++i)
    {
        const u64 bit = u64{1} << ((high >> (6U * i)) & 63U);
        const u64 old = std::atomic_ref<u64>(bits.words[i]).fetch_or(bit, std::memory_order_relaxed);
        fresh         = fresh || (old & bit) == 0U;
    }
    return fresh;
}
} // namespace crd::crypto
