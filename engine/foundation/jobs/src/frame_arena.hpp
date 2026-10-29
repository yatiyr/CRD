#pragma once

#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>

#include <atomic>
#include <cstdlib>

namespace crd::jobs::detail
{

// Per-thread linear bump allocator backing frame_alloc() / frame_try_alloc() / frame_reset().
//
// Allocation is not thread-safe: each thread has its own FrameArena instance and
// must only call try_alloc() on its own arena. WorkerPool owns all instances and calls
// reset() on each at frame_reset() time, when no job is in flight (jobs.hpp states the contract).
// The cursor, its high water and the refusals are relaxed atomics only so jobs::pressure_snapshot() may read them
// from another thread; the owner is their one writer.
class FrameArena
{
public:
    FrameArena()  = default;
    ~FrameArena() { shutdown(); }

    FrameArena(const FrameArena&)            = delete;
    FrameArena& operator=(const FrameArena&) = delete;
    FrameArena(FrameArena&&)                 = delete;
    FrameArena& operator=(FrameArena&&)      = delete;

    [[nodiscard]] bool init(crd::usize capacity)
    {
        CRD_ASSERT_MSG(m_data == nullptr, "FrameArena::init called twice");
        CRD_ASSERT_MSG(capacity > 0U, "FrameArena::init: capacity must be > 0");

        m_data     = static_cast<crd::u8*>(std::malloc(capacity)); // NOLINT(cppcoreguidelines-no-malloc,cppcoreguidelines-owning-memory)
        m_capacity = capacity;
        m_cursor.store(0U, std::memory_order_relaxed);
        return m_data != nullptr;
    }

    void shutdown() noexcept
    {
        if (m_data == nullptr)
        {
            return;
        }
        std::free(m_data); // NOLINT(cppcoreguidelines-no-malloc,cppcoreguidelines-owning-memory)
        m_data     = nullptr;
        m_capacity = 0U;
        m_cursor.store(0U, std::memory_order_relaxed);
    }

    // nullptr, with the cursor unchanged and the refusal counted, when the request does not fit. The padding aligns
    // the address, not the offset (malloc aligns the base only to max_align_t), and the comparisons are written so no
    // sum can wrap past the capacity, whatever size and alignment a caller passes.
    [[nodiscard]] void* try_alloc(crd::usize size, crd::usize alignment) noexcept
    {
        CRD_ASSERT_MSG(m_data != nullptr, "FrameArena::try_alloc called before init");
        CRD_ASSERT_MSG(size > 0U, "FrameArena::try_alloc: size must be > 0");
        CRD_ASSERT_MSG(alignment > 0U, "FrameArena::try_alloc: alignment must be > 0");
        CRD_ASSERT_MSG((alignment & (alignment - 1U)) == 0U, "FrameArena::try_alloc: alignment must be a power of two");

        const crd::usize cursor = m_cursor.load(std::memory_order_relaxed);
        const crd::usize address = reinterpret_cast<crd::usize>(m_data) + cursor;
        const crd::usize padding = (alignment - (address & (alignment - 1U))) & (alignment - 1U);
        if (padding > m_capacity - cursor || size > m_capacity - cursor - padding)
        {
            m_refusals.store(m_refusals.load(std::memory_order_relaxed) + 1U, std::memory_order_relaxed);
            return nullptr;
        }
        const crd::usize aligned = cursor + padding;
        const crd::usize next = aligned + size;
        m_cursor.store(next, std::memory_order_relaxed);
        // Raised after the cursor, so a reader that takes the cursor first never sees a high water below it.
        if (next > m_peak.load(std::memory_order_relaxed))
        {
            m_peak.store(next, std::memory_order_relaxed);
        }
        return m_data + aligned;
    }

    void reset() noexcept { m_cursor.store(0U, std::memory_order_relaxed); }

    // Restore the bump cursor to a previously-saved mark (from cursor()). Reclaims only the allocations made
    // since the mark, leaving earlier ones intact — the scoped reclaim backing frame_set_mark(). Same
    // not-thread-safe contract as try_alloc(): only the owning thread may call it.
    void set_cursor(crd::usize mark) noexcept
    {
        CRD_ASSERT_MSG(mark <= m_cursor.load(std::memory_order_relaxed),
                       "FrameArena::set_cursor: mark must not exceed the current cursor");
        m_cursor.store(mark, std::memory_order_relaxed);
    }

    [[nodiscard]] crd::usize cursor() const noexcept { return m_cursor.load(std::memory_order_relaxed); }
    [[nodiscard]] crd::usize peak() const noexcept { return m_peak.load(std::memory_order_relaxed); }
    [[nodiscard]] crd::u64 refusals() const noexcept { return m_refusals.load(std::memory_order_relaxed); }
    [[nodiscard]] crd::usize capacity() const noexcept { return m_capacity; }
    [[nodiscard]] bool is_initialized() const noexcept { return m_data != nullptr; }

private:
    crd::u8* m_data = nullptr;
    crd::usize m_capacity = 0U;
    std::atomic<crd::usize> m_cursor{0U};
    std::atomic<crd::usize> m_peak{0U};   // the highest cursor since init()
    std::atomic<crd::u64> m_refusals{0U}; // requests try_alloc() refused since init()
};

} // namespace crd::jobs::detail
