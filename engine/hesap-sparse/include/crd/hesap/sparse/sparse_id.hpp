#pragma once

#include <crd/containers/hash.hpp>
#include <crd/core/types.hpp>

namespace crd::hesap::sparse
{
// -----------------------------------------------------------------------
// SparseId — 64-bit opaque handle, layout [generation:32 | index:32].
//
// Same shape as crd::hesap::MatrixId / VectorId (handles.hpp) and
// scene::EntityId (ADR-0049): generation catches stale handles after slot
// recycling; index 0 is the null sentinel. A SparseId is what crosses
// module / CLI / RPC boundaries when an agent or script refers to a sparse
// matrix held in a hesap-owned slot table (the registry lands with the
// CLI commands in v1a-2). Trivially copyable; one 64-bit register.
// -----------------------------------------------------------------------

struct SparseId
{
    crd::u64 raw = 0;

    [[nodiscard]] constexpr crd::u32 index() const noexcept { return static_cast<crd::u32>(raw); }

    [[nodiscard]] constexpr crd::u32 generation() const noexcept { return static_cast<crd::u32>(raw >> 32); }

    [[nodiscard]] constexpr bool is_null() const noexcept { return raw == 0; }

    [[nodiscard]] static constexpr SparseId null() noexcept { return SparseId{0}; }

    [[nodiscard]] static constexpr SparseId make(crd::u32 idx, crd::u32 gen) noexcept
    {
        return SparseId{(static_cast<crd::u64>(gen) << 32) | static_cast<crd::u64>(idx)};
    }

    [[nodiscard]] constexpr bool operator==(const SparseId&) const noexcept = default;
};

static_assert(sizeof(SparseId) == 8, "SparseId must pack to 8 bytes");

} // namespace crd::hesap::sparse

namespace crd::containers
{
template <>
struct DefaultHash<crd::hesap::sparse::SparseId>
{
    [[nodiscard]] crd::u64 operator()(const crd::hesap::sparse::SparseId& h) const noexcept { return hash_u64(h.raw); }
};
} // namespace crd::containers
