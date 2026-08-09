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

// An interned attribute-VALUE handle (CEIR-1c). Identical attribute values share one `AttrId` (the Context dedups),
// so attribute equality is a `u32` compare and repeated values cost no storage. 0 = none/invalid.
struct AttrId
{
    u32 value = 0;
    [[nodiscard]] constexpr bool valid() const noexcept { return value != 0; }
    friend constexpr bool operator==(AttrId, AttrId) noexcept = default;
    friend constexpr std::strong_ordering operator<=>(AttrId, AttrId) noexcept = default;
};

// An interned TYPE-CLASS identity (CEIR-8a, ADR-0111): the FNV-1a hash of "dialect.class" — the OPEN-WORLD type
// vocabulary, mirroring `OpId` for ops. A `Type` of `TypeKind::Extern` stores THIS (never a string at runtime); the
// class string survives serialization via the STRP pool so an unregistered decoder round-trips it. 0 = none/invalid.
struct TypeClassId
{
    u64 value = 0;
    [[nodiscard]] constexpr bool valid() const noexcept { return value != 0; }
    friend constexpr bool operator==(TypeClassId, TypeClassId) noexcept = default;
    friend constexpr std::strong_ordering operator<=>(TypeClassId, TypeClassId) noexcept = default;
};

// An interned ATTRIBUTE-CLASS identity (CEIR-8b, ADR-0112): the FNV-1a hash of "dialect.attr" — the OPEN-WORLD
// attribute vocabulary, mirroring `TypeClassId`/`OpId`. An `AttrValue` of `AttrKind::Extern` stores THIS; the class
// string survives serialization via STRP so an unregistered decoder round-trips it. 0 = none/invalid.
struct AttrClassId
{
    u64 value = 0;
    [[nodiscard]] constexpr bool valid() const noexcept { return value != 0; }
    friend constexpr bool operator==(AttrClassId, AttrClassId) noexcept = default;
    friend constexpr std::strong_ordering operator<=>(AttrClassId, AttrClassId) noexcept = default;
};

// An interned EFFECT-LOCATION-CLASS identity (CEIR-8c, ADR-0113): the FNV-1a hash of "dialect.location" — the
// OPEN-WORLD effect-location vocabulary, mirroring `TypeClassId`/`AttrClassId`. An `EffectRecord` whose
// `target == EffectTarget::Extern` stores THIS (never a string at runtime); an UNREGISTERED class is treated as
// maximally-conflicting (ResourceClass::Universe) by the hazard analysis. 0 = none/invalid.
struct LocationClassId
{
    u64 value = 0;
    [[nodiscard]] constexpr bool valid() const noexcept { return value != 0; }
    friend constexpr bool operator==(LocationClassId, LocationClassId) noexcept = default;
    friend constexpr std::strong_ordering operator<=>(LocationClassId, LocationClassId) noexcept = default;
};

// An interned OP-INTERFACE identity (CEIR-1d; CEIR-8e promoted it to the FNV model) — a dynamic capability an op-kind
// implements, so analyses dispatch through the interface instead of a central `switch(op.kind)`. The FNV-1a hash of
// the interface name (like `TypeClassId`/`AttrClassId`/`LocationClassId`), so a typed interface's `kId` is a
// COMPILE-TIME constant and a query mints it with zero registry work. 0 = none/invalid.
struct InterfaceId
{
    u64 value = 0;
    [[nodiscard]] constexpr bool valid() const noexcept { return value != 0; }
    friend constexpr bool operator==(InterfaceId, InterfaceId) noexcept = default;
    friend constexpr std::strong_ordering operator<=>(InterfaceId, InterfaceId) noexcept = default;
};

// A content-INDEPENDENT stable semantic identity for an operation (CEIR-8d, ADR-0114) — the ADR-0109 §6 IOU. Assigned
// ONE-TIME in module pre-order (module-scoped, NOT a Context counter — so ids are a pure function of content); survives
// edits/reorders/renames + serialize/deserialize (a `STID` binary chunk). ONE id space: a function/state-slot/visual
// node's identity IS its op's stable id. 0 = unassigned/invalid. ⛔ NOT part of the content hash (identity ≠ content).
struct StableId
{
    u64 value = 0;
    [[nodiscard]] constexpr bool valid() const noexcept { return value != 0; }
    friend constexpr bool operator==(StableId, StableId) noexcept = default;
    friend constexpr std::strong_ordering operator<=>(StableId, StableId) noexcept = default;
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
