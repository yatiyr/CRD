#pragma once

// crd-ceir — CEIR (Cerid Execution IR), ADR-0109. Identity types.
//
// `OpId` is an interned OP-KIND identity (a hash of "dialect.op", the `ExecutorTypeId` precedent) — NOT a handle to
// an `Operation` node (node handles are stable arena pointers `Operation*`/`Value*`/… since the arena never frees
// individually). `TypeId` is an interned type handle; the real type system is CEIR-3, so here it is an opaque
// Context-interned id (0 = none/unknown).

#include <crd/core/types.hpp>

#include <compare>

namespace crd::ceir
{
// An interned op-kind identity: the FNV-1a hash of "dialect.op". Records/ops store THIS, never a string at runtime.
struct OpId
{
    u64 value = 0;
    [[nodiscard]] constexpr bool valid() const noexcept { return value != 0; }
    friend constexpr bool operator==(OpId, OpId) noexcept = default;
    friend constexpr std::strong_ordering operator<=>(OpId, OpId) noexcept = default;
};

// An interned type handle. CEIR-1a treats it opaquely (0 = none); the typed type system lands at CEIR-3.
struct TypeId
{
    u32 value = 0;
    [[nodiscard]] constexpr bool valid() const noexcept { return value != 0; }
    friend constexpr bool operator==(TypeId, TypeId) noexcept = default;
    friend constexpr std::strong_ordering operator<=>(TypeId, TypeId) noexcept = default;
};

// Provenance carried on EVERY operation from day one (ADR-0109 §6). CEIR-1c fills `file_id` from the source-map
// table; CEIR-1a reserves the field so no later slice retrofits it into an arena-packed struct.
struct SourceLoc
{
    u32 file_id = 0;
    u32 line    = 0;
    u32 col     = 0;
};
} // namespace crd::ceir
