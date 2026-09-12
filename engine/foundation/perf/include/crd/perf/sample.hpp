#pragma once

// ---------------------------------------------------------------------------
// crd-perf -- the canonical 32-byte Sample POD (Detour D-003).
//
// One Sample = one scope (begin + end paired). Fiber migration is captured
// by the two thread fields:
//
//   begin_thread  -- OS-thread index when ScopedRegion was constructed
//   end_thread    -- OS-thread index when ScopedRegion was destroyed
//
// If they differ, the scope migrated across a fiber yield. UI renders the
// region with a split-gap. The per-fiber assembly is reconstructed from
// fiber-yield events emitted by the JobObserver (v0c).
//
// Layout is fixed -- the on-disk capture format (v0f) memcpy's Sample arrays
// verbatim. Any change to this struct bumps the CPROF version.
// ---------------------------------------------------------------------------

#include <crd/core/types.hpp>

namespace crd::perf
{

// Strongly-typed name handle. Returned by intern_name(); cached by the
// CRD_PERF_SCOPE macro in a TU-local static so the lookup happens once
// per call site at first hit.
struct NameId
{
    crd::u32 value = 0xFFFF'FFFFU;

    [[nodiscard]] constexpr bool is_valid() const noexcept { return value != 0xFFFF'FFFFU; }
};

inline constexpr NameId kInvalidNameId{0xFFFF'FFFFU};

// Strongly-typed category for region color-coding. Reserved for v0c when
// the auto-instrumentation hooks tag jobs / scene-systems / frame-graph /
// rhi-cmd regions with their domain.
enum class Category : crd::u8
{
    User       = 0,
    Job        = 1,
    System     = 2,
    Pass       = 3,
    Render     = 4,
    Gpu        = 5,
    Memory     = 6,
    Io         = 7,
    Wait       = 8,
};

// 32-byte paired Sample. POD; trivially-copyable for memcpy into the
// capture buffer. Layout is verified by static_assert in sample.cpp.
struct Sample
{
    crd::i64 begin_ns;        // 8  -- MonotonicClock-relative ns since epoch
    crd::i64 end_ns;          // 8  -- ditto
    crd::u32 name_id;         // 4  -- index into the interned name table
    crd::u32 color_rgba;      // 4  -- premultiplied RGBA; 0 = inherit-from-category
    crd::u8  begin_thread;    // 1  -- index into kMaxThreads
    crd::u8  end_thread;      // 1  -- migrated when begin != end
    crd::u8  depth;           // 1  -- nesting depth at scope-begin (0-based)
    crd::u8  category;        // 1  -- Category enum
    crd::u32 fiber_id;        // 4  -- 0 = no fiber / OS-thread context only (v0c sets this)
};

static_assert(sizeof(Sample) == 32, "Sample is the canonical 32-byte POD; on-disk format depends on this");
static_assert(alignof(Sample) == 8, "Sample alignment must be 8 (i64 fields)");

} // namespace crd::perf
