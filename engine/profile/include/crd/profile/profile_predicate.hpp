#pragma once

#include <crd/core/types.hpp>

namespace crd::profile
{
// Closed predicate field schema (ADR-0060 §2). Append-only; never insert.
// New domain-specific predicates land in Phase 4+ via a registration hook
// reserved on the registry but not exposed in v1n.
enum class PredicateField : crd::u8
{
    Os        = 0, // matches against ProfileContext::os
    GpuTier   = 1, // matches against ProfileContext::gpu_tier
    Domain    = 2, // matches against ProfileContext::domain
    Mode      = 3, // matches against ProfileContext::mode
    TargetFps = 4, // matches against ProfileContext::target_fps (i32)
    CpuCores  = 5, // matches against ProfileContext::cpu_cores  (i32)
};

// Operators (ADR-0060 §2 table). v1n6's resolver implements the comparison
// semantics; v1n5 ships only the enum + the byte layout for round-trip.
//
//   Equal       — exact equality (all enum + integer fields).
//   GreaterEq   — context.field >= predicate.value (integer / ordered enum).
//   LessEq      — context.field <= predicate.value (integer / ordered enum).
//   InMask      — context.field's enum value is in the bitmask
//                 (predicate.value is a u32 bitmask, bit n set = enum
//                 value n is allowed). Only valid for enum fields whose
//                 value range fits in 32 bits (all current fields do —
//                 closed enums of <= 5 values).
enum class PredicateOp : crd::u8
{
    Equal     = 0,
    GreaterEq = 1,
    LessEq    = 2,
    InMask    = 3,
};

// One predicate record — 8 bytes, pinned for binary stability across
// profile-schema versions. Stored in FRLE as a flat array per rule.
//
// Field interpretation:
//   - For Equal/GreaterEq/LessEq on enum fields:
//       value = static_cast<u32>(enum_value)
//   - For Equal/GreaterEq/LessEq on integer fields (TargetFps, CpuCores):
//       value = static_cast<u32>(i32 comparand)  // bitcast preserves sign
//   - For InMask on enum fields:
//       value = bitmask, bit `n` set ⇔ enum value `n` is allowed
struct PredicateRecord
{
    PredicateField field;        // 1 byte
    PredicateOp    op;           // 1 byte
    crd::u8        _reserved[2]; // 2 bytes — must be zero on disk
    crd::u32       value;        // 4 bytes — see field interpretation above
};
static_assert(sizeof(PredicateRecord)  == 8,
              "PredicateRecord size pinned at 8 bytes for profile schema v1");
static_assert(alignof(PredicateRecord) == 4,
              "PredicateRecord alignment pinned at 4 bytes");

} // namespace crd::profile
