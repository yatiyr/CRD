#pragma once

#include <crd/containers/array.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>
#include <crd/scene/entity.hpp>

namespace crd::scene
{
// Slot — backing storage entry for an EntityId. See ADR-0049.
//
//   generation : bumped on free; matched against EntityId::generation() at lookup.
//   next_free  : when alive == false, links into the SlotMap free list.
//                kInvalidIdx terminates the chain.
//   alive      : presence flag; live slots have alive == true.
//
// Slots only carry identity metadata. Component data lives in the storage
// layer (Phase 3.0 v1c/v1d), not here.
struct Slot
{
    crd::u32 generation = 0;
    crd::u32 next_free = 0;
    bool alive = false;
};

// Sentinel for free-list terminator and "no free slot".
inline constexpr crd::u32 kInvalidSlotIndex = static_cast<crd::u32>(-1);

// SlotMap — dense slot table with a free list and generation-tagged handles.
//
// Layout: Array<Slot>, indexed directly by EntityId::index().
//   - Slot 0 is reserved permanently as the null sentinel; allocate() never
//     returns index 0.
//   - Free list is threaded through Slot::next_free of vacated slots; head
//     is m_free_head (kInvalidSlotIndex when empty).
//   - alive_count tracks live slots for O(1) entity_count().
//
// allocate()/free() are O(1). is_alive() is O(1) with one bounds check + one
// generation compare. Iteration yields alive entities only and skips holes.
//
// See ADR-0049.
class SlotMap
{
public:
    explicit SlotMap(crd::memory::IAllocator* alloc = crd::memory::default_allocator());

    SlotMap(const SlotMap&) = delete;
    SlotMap& operator=(const SlotMap&) = delete;
    SlotMap(SlotMap&&) = default;
    SlotMap& operator=(SlotMap&&) = default;

    // Mint a new EntityId. Pops a slot from the free list if available; otherwise
    // pushes a new slot. The returned handle's index is never 0.
    [[nodiscard]] EntityId allocate();

    // Free the slot referenced by `e`. Precondition: is_alive(e). Bumps the slot's
    // generation so any stale handle will fail subsequent is_alive() checks.
    void free(EntityId e);

    // Returns true iff `e` references a live slot whose generation matches.
    // is_alive(EntityId::null()) is false by construction.
    [[nodiscard]] bool is_alive(EntityId e) const noexcept;

    [[nodiscard]] crd::u32 alive_count() const noexcept { return m_alive_count; }

    // Total slot count, including holes and the reserved sentinel slot.
    [[nodiscard]] crd::u32 slot_count() const noexcept { return static_cast<crd::u32>(m_slots.size()); }

    // Iterator over alive entities. Skips dead slots. Yields EntityId values.
    class Iterator
    {
    public:
        Iterator(const SlotMap* map, crd::u32 index) noexcept;

        [[nodiscard]] EntityId operator*() const noexcept;
        Iterator& operator++() noexcept;
        [[nodiscard]] bool operator==(const Iterator&) const noexcept = default;

    private:
        void advance_to_alive() noexcept;

        const SlotMap* m_map = nullptr;
        crd::u32 m_index = 0;
    };

    [[nodiscard]] Iterator begin() const noexcept;
    [[nodiscard]] Iterator end() const noexcept;

private:
    crd::containers::Array<Slot> m_slots;
    crd::u32 m_free_head = kInvalidSlotIndex;
    crd::u32 m_alive_count = 0;
};

} // namespace crd::scene
