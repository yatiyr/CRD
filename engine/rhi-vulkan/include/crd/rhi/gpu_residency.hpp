#pragma once

#include <crd/core/types.hpp>

// crd::rhi GPU defrag + residency policy seams (ADR-0085 S7).
//
// The Vulkan GpuAllocator (ADR-0085 S6) provides the MECHANISM — relocate an
// allocation (recreate + transfer-copy + swap the resource's internal handle,
// idle-gated) and move it device-local<->host-visible. The DECISION of what to
// relocate / evict is injected via these policy interfaces (ADR-0085 D4), so the
// eventual renderer-streaming consumer tunes policy without touching the mechanism.
//
// No Vulkan types appear here — the seams are backend-neutral (Buffer/Image/u64).

namespace crd::rhi
{
class Buffer;
class Image;

// Passed to a residency policy so it can act on the allocator without depending on
// the concrete backend type. The backend (VulkanDevice) implements it.
class IGpuResidencyContext
{
public:
    virtual ~IGpuResidencyContext() = default;

    // Device-local bytes currently resident, and the soft budget (0 == unlimited).
    [[nodiscard]] virtual crd::u64 device_local_used() const noexcept   = 0;
    [[nodiscard]] virtual crd::u64 device_local_budget() const noexcept = 0;

    // Live device-local buffers the policy may choose to evict. Index in
    // [0, resident_buffer_count()); a snapshot that races with concurrent creates
    // (drive eviction from a single thread). resident_buffer_at returns nullptr for
    // an out-of-range or already-evicted index.
    [[nodiscard]] virtual crd::u32 resident_buffer_count() const noexcept = 0;
    [[nodiscard]] virtual Buffer*  resident_buffer_at(crd::u32 index) noexcept = 0;

    // Move a resource's backing memory device-local -> host-visible (recreate +
    // transfer-copy + swap, idle-gated). Returns the device-local bytes freed (0 if
    // it could not, e.g. no host-visible heap). Data is preserved.
    [[nodiscard]] virtual crd::u64 evict_to_host(Buffer& buffer) = 0;
};

// Chooses what to relocate during a defragmentation pass. The null default
// relocates nothing.
class IDefragPolicy
{
public:
    virtual ~IDefragPolicy() = default;

    // Return true to relocate this resource (to a more-compact location) this pass.
    [[nodiscard]] virtual bool should_defrag(const Buffer& /*buffer*/) { return false; }
    [[nodiscard]] virtual bool should_defrag(const Image& /*image*/) { return false; }

    // Fired AFTER a resource was relocated (its handle/view swapped; generation
    // bumped). Called outside the allocator lock. Consumers re-bind descriptors that
    // referenced the resource. `new_generation` matches the resource's generation().
    // The old handle is invalid at this point: any previously-recorded command buffer
    // referencing it MUST be re-recorded before submission (defrag is idle-gated, so
    // no in-flight work holds it, but the next frame's recording must use the new one).
    virtual void on_relocated(const Buffer& /*buffer*/, crd::u32 /*new_generation*/) {}
    virtual void on_relocated(const Image& /*image*/, crd::u32 /*new_generation*/) {}
};

// Sheds device-local memory under pressure. The null default sheds nothing (so an
// over-budget allocation simply fails — no automatic eviction).
class IResidencyPolicy
{
public:
    virtual ~IResidencyPolicy() = default;

    // Free at least `needed_bytes` of device-local memory via ctx.evict_to_host(...).
    // Return the bytes actually freed; 0 means "nothing left to shed" (the allocation
    // then fails gracefully). MUST return 0 when it cannot free more (no livelock).
    [[nodiscard]] virtual crd::u64 evict(IGpuResidencyContext& ctx, crd::u64 needed_bytes) = 0;
};

class NullDefragPolicy final : public IDefragPolicy
{
};

class NullResidencyPolicy final : public IResidencyPolicy
{
public:
    [[nodiscard]] crd::u64 evict(IGpuResidencyContext& /*ctx*/, crd::u64 /*needed*/) override { return 0; }
};
} // namespace crd::rhi
