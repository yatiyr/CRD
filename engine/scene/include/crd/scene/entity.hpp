#pragma once

#include <crd/core/types.hpp>

namespace crd::scene
{
// EntityId — 64-bit handle, layout [generation:32 | index:32].
//
// Slot index 0 is reserved as the null sentinel. EntityId{} default-initialises
// to null(). A handle whose generation does not match the live slot's generation
// resolves to a dead lookup (is_alive returns false).
//
// Trivially copyable; fits a single 64-bit register. See ADR-0049.
struct EntityId
{
    crd::u64 raw = 0;

    [[nodiscard]] constexpr crd::u32 index() const noexcept { return static_cast<crd::u32>(raw); }

    [[nodiscard]] constexpr crd::u32 generation() const noexcept { return static_cast<crd::u32>(raw >> 32); }

    [[nodiscard]] constexpr bool is_null() const noexcept { return raw == 0; }

    [[nodiscard]] static constexpr EntityId null() noexcept { return EntityId{0}; }

    [[nodiscard]] static constexpr EntityId make(crd::u32 index, crd::u32 generation) noexcept
    {
        return EntityId{(static_cast<crd::u64>(generation) << 32) | static_cast<crd::u64>(index)};
    }

    [[nodiscard]] constexpr bool operator==(const EntityId& other) const noexcept = default;
};

static_assert(sizeof(EntityId) == 8, "EntityId must pack to 8 bytes");

} // namespace crd::scene
