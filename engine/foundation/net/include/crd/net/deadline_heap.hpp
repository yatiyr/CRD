#pragma once

// crd-net -- which connection of a fixed pool is due next, what every host's turn asks (4.2a; contract:
// docs/design/network-foundation.md#quic-server). A binary min-heap over indices 0 to capacity - 1, each index present
// at most once with its own deadline: schedule and unschedule are O(log n), and a host never scans its pool.
//
//   memory    the arrays come from the injected allocator in init(); nothing is allocated after.
//   threads   the owner's.

#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/net/quota.hpp>

namespace crd::memory
{
class IAllocator;
}

namespace crd::net
{
namespace cont = crd::containers;

class DeadlineHeap
{
public:
    explicit DeadlineHeap(memory::IAllocator& allocator) noexcept : m_allocator(allocator) {}
    ~DeadlineHeap();
    DeadlineHeap(const DeadlineHeap&) = delete;
    DeadlineHeap& operator=(const DeadlineHeap&) = delete;
    DeadlineHeap(DeadlineHeap&&) = delete;
    DeadlineHeap& operator=(DeadlineHeap&&) = delete;

    // Room for indices below `capacity`; false, holding nothing, for a capacity of 0, a second call or a refusal.
    [[nodiscard]] bool init(u32 capacity) noexcept;

    // Sets the deadline of `index`, adding it when absent.
    void schedule(u32 index, u64 due_ns) noexcept;
    // Removes `index`; nothing when absent.
    void unschedule(u32 index) noexcept;

    // The earliest deadline; kNeverNs when empty.
    [[nodiscard]] u64 next_due() const noexcept;
    // Removes every index due at `now_ns` into `out` (room for the capacity, so one served again waits for the next
    // turn); the count.
    [[nodiscard]] u32 take_due(u64 now_ns, cont::Span<u32> out) noexcept;

    [[nodiscard]] u32 size() const noexcept { return m_size; }

private:
    static constexpr u32 kAbsent = ~u32{0};

    struct Entry
    {
        u64 due_ns = kNeverNs;
        u32 place = kAbsent; // its place in the heap
    };

    void sift_up(u32 place) noexcept;
    void sift_down(u32 place) noexcept;
    void put(u32 place, u32 index) noexcept;

    memory::IAllocator& m_allocator;
    Entry* m_entries = nullptr; // one per index
    u32* m_heap = nullptr;      // indices, the earliest first
    u32 m_capacity = 0U;
    u32 m_size = 0U;
};

} // namespace crd::net
