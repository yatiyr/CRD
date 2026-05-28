#include <crd/memory/allocators/ring_allocator.hpp>

#include <crd/core/assert.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::memory
{
namespace
{
// Sentinel for an unused mark slot. A real fence is always < this, so retire()
// never matches an empty slot and begin_epoch() treats it as free-to-reuse.
constexpr u64 kEmptyFence = ~u64{0};
} // namespace

RingAllocator::RingAllocator(usize capacity, IAllocator* parent, usize max_in_flight_epochs, const char* name)
    : m_parent(parent != nullptr ? parent : default_allocator()), m_name(name)
{
    CRD_ASSERT_MSG(capacity > 0, "RingAllocator: capacity must be > 0");
    CRD_ASSERT_MSG(is_pow2(max_in_flight_epochs), "RingAllocator: max_in_flight_epochs must be a power of two");
    CRD_ASSERT_MSG(max_in_flight_epochs >= 2 && max_in_flight_epochs <= kMaxInFlightEpochs,
                   "RingAllocator: max_in_flight_epochs out of range [2, kMaxInFlightEpochs]");

    m_capacity   = capacity;
    m_epoch_mask = max_in_flight_epochs - 1;
    // Cache-line aligned so a wrap-to-offset-0 claim is well-aligned and the buffer
    // edges don't false-share with neighbours.
    m_buffer = static_cast<u8*>(m_parent->allocate(capacity, kCachelineSize));

    for (usize i = 0; i < max_in_flight_epochs; ++i)
    {
        m_marks[i].fence.store(kEmptyFence, std::memory_order_relaxed);
        m_marks[i].end_head.store(0, std::memory_order_relaxed);
    }
}

RingAllocator::~RingAllocator()
{
    m_parent->deallocate(m_buffer);
}

void* RingAllocator::try_claim(usize size, usize alignment) noexcept
{
    if (size == 0 || size > m_capacity)
    {
        return nullptr; // zero / larger-than-the-whole-ring never fits
    }
    CRD_ASSERT(is_pow2(alignment));
    CRD_ASSERT_MSG(alignment <= kCachelineSize, "RingAllocator: alignment exceeds the buffer alignment");

    u64 head = m_head.load(std::memory_order_relaxed);
    for (;;)
    {
        const usize off       = static_cast<usize>(head % m_capacity);
        const usize aligned   = align_up(off, alignment);
        usize       payload;  // offset within the buffer where the payload starts
        u64         new_head; // head after this claim (incl. any wrap padding)
        if (aligned + size <= m_capacity)
        {
            payload  = aligned;
            new_head = head + (aligned - off) + size;
        }
        else
        {
            // Wrap: waste [off, capacity) and restart at offset 0 (cache-line aligned).
            payload  = 0;
            new_head = head + (m_capacity - off) + size;
        }

        const u64 tail = m_tail.load(std::memory_order_acquire);
        if (new_head - tail > m_capacity)
        {
            return nullptr; // not enough free space given outstanding (unretired) claims
        }
        if (m_head.compare_exchange_weak(head, new_head, std::memory_order_acq_rel, std::memory_order_relaxed))
        {
            return m_buffer + payload;
        }
        // CAS failed: `head` reloaded with the current value — recompute and retry.
    }
}

void RingAllocator::begin_epoch(u64 fence) noexcept
{
    const u64 prev = m_latest_fence.load(std::memory_order_relaxed);
    CRD_ASSERT_MSG(fence > prev, "RingAllocator::begin_epoch: fence must strictly increase");

    const u64   head = m_head.load(std::memory_order_acquire);
    const usize slot = static_cast<usize>(prev & m_epoch_mask); // the closing epoch's mark slot

    // The slot we are about to overwrite holds the epoch (prev - K) that last used
    // it. Reusing it before that epoch retired would lose its boundary -> assert.
    // Inlined into one expression so no locals go unused when CRD_ASSERT compiles out
    // in release (/WX C4189).
    CRD_ASSERT_MSG(m_marks[slot].fence.load(std::memory_order_acquire) == kEmptyFence
                       || m_tail.load(std::memory_order_acquire)
                              >= m_marks[slot].end_head.load(std::memory_order_acquire),
                   "RingAllocator: more than max_in_flight_epochs epochs unretired (raise K or retire sooner)");

    // Publish the mark: store end_head first, then fence with release. retire()
    // loads fence with acquire then end_head — the release/acquire pair on `fence`
    // makes the prior end_head write visible (it happens-before the fence release).
    m_marks[slot].end_head.store(head, std::memory_order_relaxed);
    m_marks[slot].fence.store(prev, std::memory_order_release);
    m_latest_fence.store(fence, std::memory_order_release);
}

void RingAllocator::retire(u64 completed_fence) noexcept
{
    // Free every recorded epoch with fence <= completed_fence: advance tail to the
    // furthest such end. Fences are monotonic and complete in order, so the max end
    // among matching marks is the correct reclaim point.
    u64         target = 0;
    const usize k      = m_epoch_mask + 1;
    for (usize i = 0; i < k; ++i)
    {
        const u64 f = m_marks[i].fence.load(std::memory_order_acquire);
        if (f != kEmptyFence && f <= completed_fence)
        {
            const u64 e = m_marks[i].end_head.load(std::memory_order_acquire);
            if (e > target)
            {
                target = e;
            }
        }
    }

    // Advance the tail monotonically (never retreat); safe against concurrent retire.
    u64 t = m_tail.load(std::memory_order_relaxed);
    while (t < target && !m_tail.compare_exchange_weak(t, target, std::memory_order_release, std::memory_order_relaxed))
    {
        // t reloaded on failure; retry until tail >= target.
    }
}

usize RingAllocator::in_use_bytes() const noexcept
{
    const u64 head = m_head.load(std::memory_order_acquire);
    const u64 tail = m_tail.load(std::memory_order_acquire);
    return static_cast<usize>(head - tail);
}
} // namespace crd::memory
