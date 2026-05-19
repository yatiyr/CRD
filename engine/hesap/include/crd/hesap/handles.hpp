#pragma once

#include <crd/containers/hash.hpp>
#include <crd/core/types.hpp>

namespace crd::hesap
{
// -----------------------------------------------------------------------
// MatrixId / VectorId — 64-bit opaque handles, layout [generation:32 | index:32].
//
// Same shape as scene::EntityId (ADR-0049). Generation counter catches
// stale handles after a slot is recycled; index 0 is the null sentinel.
// A MatrixId is what crosses module / CLI / RPC boundaries when an
// agent or script wants to refer to a matrix held by hesap.
//
// Per ADR-0081 §3, CLI / RPC commands accept these handles by-value;
// the actual heap-resident matrix lives in a hesap-owned slot table
// (lands in v0b alongside the first BLAS L1 ops that need it).
//
// Trivially copyable; fits a single 64-bit register.
// -----------------------------------------------------------------------

struct MatrixId
{
    crd::u64 raw = 0;

    [[nodiscard]] constexpr crd::u32 index() const noexcept { return static_cast<crd::u32>(raw); }

    [[nodiscard]] constexpr crd::u32 generation() const noexcept { return static_cast<crd::u32>(raw >> 32); }

    [[nodiscard]] constexpr bool is_null() const noexcept { return raw == 0; }

    [[nodiscard]] static constexpr MatrixId null() noexcept { return MatrixId{0}; }

    [[nodiscard]] static constexpr MatrixId make(crd::u32 idx, crd::u32 gen) noexcept
    {
        return MatrixId{(static_cast<crd::u64>(gen) << 32) | static_cast<crd::u64>(idx)};
    }

    [[nodiscard]] constexpr bool operator==(const MatrixId&) const noexcept = default;
};

struct VectorId
{
    crd::u64 raw = 0;

    [[nodiscard]] constexpr crd::u32 index() const noexcept { return static_cast<crd::u32>(raw); }

    [[nodiscard]] constexpr crd::u32 generation() const noexcept { return static_cast<crd::u32>(raw >> 32); }

    [[nodiscard]] constexpr bool is_null() const noexcept { return raw == 0; }

    [[nodiscard]] static constexpr VectorId null() noexcept { return VectorId{0}; }

    [[nodiscard]] static constexpr VectorId make(crd::u32 idx, crd::u32 gen) noexcept
    {
        return VectorId{(static_cast<crd::u64>(gen) << 32) | static_cast<crd::u64>(idx)};
    }

    [[nodiscard]] constexpr bool operator==(const VectorId&) const noexcept = default;
};

static_assert(sizeof(MatrixId) == 8, "MatrixId must pack to 8 bytes");
static_assert(sizeof(VectorId) == 8, "VectorId must pack to 8 bytes");

} // namespace crd::hesap

// Hash specialisations so handles can key into HashMap.
namespace crd::containers
{
template <>
struct DefaultHash<crd::hesap::MatrixId>
{
    [[nodiscard]] crd::u64 operator()(const crd::hesap::MatrixId& h) const noexcept { return hash_u64(h.raw); }
};

template <>
struct DefaultHash<crd::hesap::VectorId>
{
    [[nodiscard]] crd::u64 operator()(const crd::hesap::VectorId& h) const noexcept { return hash_u64(h.raw); }
};
} // namespace crd::containers
