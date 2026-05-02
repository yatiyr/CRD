#pragma once

#include <crd/core/types.hpp>

// Job declaration types for crd-jobs.
// No implementation; intentionally dependency-free beyond crd-core.
// Full public API (run/wait/parallel_for) ships in v1h.
// See docs/phases/phase-2.5-jobs.md for the complete design.

namespace crd::jobs
{

enum class Priority : crd::u8
{
    High,
    Normal,
    Low,
};

enum class StackSize : crd::u8
{
    Small,
    Medium,
    Large,
};

// One-cache-line job descriptor.
// fn(data) is the calling convention. Use make_job<F> (v1i) for lambda wrapping
// via 48-byte SBO without heap allocation.
//
// Member layout — no implicit padding:
//   [ 0.. 7]  fn         function pointer              (8 bytes)
//   [ 8..15]  data       argument pointer              (8 bytes)
//   [16..19]  pin_thread target thread, -1 = any       (4 bytes)
//   [20]      stack      stack-tier hint               (1 byte)
//   [21]      priority   dispatch priority             (1 byte)
//   [22..63]  _pad       reserved / future             (42 bytes)
struct alignas(64) JobDecl
{
    void     (*fn)(void*) = nullptr;
    void*      data       = nullptr;
    crd::i32   pin_thread = -1;
    StackSize  stack      = StackSize::Small;
    Priority   priority   = Priority::Normal;
    crd::u8    _pad[42]   = {}; // NOLINT(modernize-avoid-c-arrays)
};

static_assert(sizeof(JobDecl) == 64,
              "JobDecl must be exactly one cache line (64 bytes)");
static_assert(alignof(JobDecl) == 64,
              "JobDecl must be 64-byte aligned");

} // namespace crd::jobs
