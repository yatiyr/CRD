#pragma once

// Descriptor set layout, pipeline layout, descriptor set, and ring-buffer allocator.
//
// Frequency convention (low index = lowest rebind frequency):
//   Set 0  per-frame      (camera, lights, time)     — bound once per frame
//   Set 1  per-material   (textures, material params) — bound per material batch
//   Push   per-draw       (model matrix, draw ID)     — written per draw call
//
// Allocation strategy — ring-buffer allocator (DescriptorAllocator):
//   The allocator owns N descriptor pools, one per frame-in-flight.
//   begin_frame(i) resets pool[i], reclaiming all sets allocated from it.
//   This is safe because the GPU finished frame (i - N) long before begin_frame(i)
//   is called for frame i + N. No individual set is freed; the whole pool recycles.
//   Size `DescriptorAllocatorDesc` counts to the maximum you expect per frame —
//   overshooting a little is fine; hitting the limit causes an allocation failure.
//
//   See docs/sessions/2026-05-01-renderer-v1ef-descriptors.md for the full design rationale.

#include <crd/rhi/buffer.hpp>
#include <crd/rhi/types.hpp>

#include <memory>

namespace crd::rhi
{

// Describes the binding layout for one descriptor set (one frequency tier).
// Immutable after creation; shared across all pipelines that use the same layout.
class DescriptorSetLayout
{
public:
    virtual ~DescriptorSetLayout() = default;

    [[nodiscard]] virtual const DescriptorSetLayoutDesc& desc() const noexcept = 0;
};

// Combines N DescriptorSetLayouts with push constant ranges into a full pipeline layout.
// One PipelineLayout is shared by all pipeline variants that have the same binding shape.
// Must outlive every Pipeline that references it.
class PipelineLayout
{
public:
    virtual ~PipelineLayout() = default;

    [[nodiscard]] virtual const PipelineLayoutDesc& desc() const noexcept = 0;
};

// One allocated descriptor set — a concrete binding table for a single frequency tier.
//
// Lifetime: must not outlive the DescriptorAllocator frame that produced it.
// The pool is reset en-masse by DescriptorAllocator::begin_frame(); do not hold
// DescriptorSet references across frame boundaries.
class DescriptorSet
{
public:
    virtual ~DescriptorSet() = default;

    // Bind a (uniform or storage) buffer at the given binding slot.
    // size_bytes == 0 means the full buffer range (VK_WHOLE_SIZE).
    virtual void update_buffer(crd::u32 binding, Buffer& buffer,
                               crd::u64 offset_bytes = 0,
                               crd::u64 size_bytes   = 0) = 0;
};

// Per-frame ring-buffer descriptor allocator.
//
// Usage pattern:
//   // Once, at startup:
//   auto alloc = device.create_descriptor_allocator({...});
//
//   // Each frame:
//   alloc->begin_frame(frame_index);                          // reset oldest pool
//   auto set = alloc->allocate(*my_layout);                  // allocate from current pool
//   set->update_buffer(0, *my_ubo);                          // write bindings
//   cmd.bind_descriptor_sets(*pipeline_layout, 1, {set.get()}); // bind at set 1
class DescriptorAllocator
{
public:
    virtual ~DescriptorAllocator() = default;

    // Advance to the next frame, resetting the pool for `frame_index % frames_in_flight`.
    // Call once per frame before any allocate() calls. frame_index is the monotonically
    // increasing frame counter (not clamped to frames_in_flight).
    virtual void begin_frame(crd::u32 frame_index) = 0;

    // Allocate one descriptor set from the current frame's pool.
    // Returns nullptr if the pool is exhausted — increase max_sets_per_frame.
    [[nodiscard]] virtual std::unique_ptr<DescriptorSet>
    allocate(const DescriptorSetLayout& layout) = 0;
};

} // namespace crd::rhi
