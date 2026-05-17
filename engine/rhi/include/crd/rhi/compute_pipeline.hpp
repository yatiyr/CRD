#pragma once

#include <crd/rhi/types.hpp>

namespace crd::rhi
{
// ComputePipeline — opaque GPU compute pipeline handle.
//
// Phase 3.1.7.6 v0a (ADR-0080 D1 additive-only). Sibling of `Pipeline`
// (graphics). Kept as a separate concrete type rather than a shared
// PipelineBase abstraction because graphics and compute bind to
// different Vulkan pipeline bind points (GRAPHICS vs COMPUTE) — these
// are different operations, not polymorphic ones.
//
// v0a ships only the type + the `Device::create_compute_pipeline`
// factory. Binding (`CommandBuffer::bind_compute_pipeline`), dispatch,
// push constants for compute, and storage-buffer descriptor sets ship
// at v0b–v0c.
class ComputePipeline
{
public:
    virtual ~ComputePipeline() = default;

    [[nodiscard]] virtual const ComputePipelineDesc& desc() const noexcept = 0;
};
} // namespace crd::rhi
