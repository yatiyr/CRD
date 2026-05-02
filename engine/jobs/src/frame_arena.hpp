#pragma once

#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>

#include <cstdlib>

namespace crd::jobs::detail
{

// Per-thread linear bump allocator backing frame_alloc() / frame_reset().
//
// Allocation is not thread-safe: each thread has its own FrameArena instance and
// must only call alloc() on its own arena. WorkerPool owns all instances and calls
// reset() on each at frame_reset() time — callers must ensure no concurrent alloc
// is in flight during reset (i.e. call frame_reset() only after all jobs of the
// previous frame have completed via wait() / run_and_wait()).
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
        CRD_ASSERT_MSG(capacity > 0u, "FrameArena::init: capacity must be > 0");

        m_data     = static_cast<crd::u8*>(std::malloc(capacity)); // NOLINT(cppcoreguidelines-no-malloc,cppcoreguidelines-owning-memory)
        m_capacity = capacity;
        m_cursor   = 0u;
        return m_data != nullptr;
    }

    void shutdown() noexcept
    {
        if (m_data == nullptr)
            return;
        std::free(m_data); // NOLINT(cppcoreguidelines-no-malloc,cppcoreguidelines-owning-memory)
        m_data     = nullptr;
        m_capacity = 0u;
        m_cursor   = 0u;
    }

    [[nodiscard]] void* alloc(crd::usize size, crd::usize alignment)
    {
        CRD_ASSERT_MSG(m_data != nullptr, "FrameArena::alloc called before init");
        CRD_ASSERT_MSG(size > 0u, "FrameArena::alloc: size must be > 0");
        CRD_ASSERT_MSG(alignment > 0u, "FrameArena::alloc: alignment must be > 0");
        CRD_ASSERT_MSG((alignment & (alignment - 1u)) == 0u,
                       "FrameArena::alloc: alignment must be a power of two");

        const crd::usize aligned = (m_cursor + alignment - 1u) & ~(alignment - 1u);
        CRD_ASSERT_MSG(aligned + size <= m_capacity,
                       "FrameArena::alloc: arena exhausted — increase frame_alloc_bytes in Config");

        m_cursor = aligned + size;
        return m_data + aligned;
    }

    void reset() noexcept { m_cursor = 0u; }

    [[nodiscard]] crd::usize cursor()   const noexcept { return m_cursor;   }
    [[nodiscard]] crd::usize capacity() const noexcept { return m_capacity; }
    [[nodiscard]] bool is_initialized() const noexcept { return m_data != nullptr; }

private:
    crd::u8*   m_data     = nullptr;
    crd::usize m_capacity = 0u;
    crd::usize m_cursor   = 0u;
};

} // namespace crd::jobs::detail
