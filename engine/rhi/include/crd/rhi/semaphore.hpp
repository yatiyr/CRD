#pragma once

#include <crd/rhi/types.hpp>

namespace crd::rhi
{
// Semaphore — binary GPU-GPU synchronization primitive.
//
// Phase 3.1.7.6 v0d (ADR-0080 D10). Used for cross-queue handoff:
// queue A signals on submit completion, queue B waits at a specific
// pipeline stage before continuing. Reset implicit on wait — Vulkan
// binary semaphore semantics.
//
// **Binary only.** Timeline semaphores (Vulkan 1.2 typed timeline with
// monotonic 64-bit value) ship when a real consumer needs ordered
// multi-step async work — e.g. eylem v8 GPU frame-graph multi-stage
// pipelines. Until then, this is the binary `VkSemaphore` type.
//
// Lifetime: callers own the unique_ptr returned by
// `Device::create_semaphore`. Must outlive any in-flight submission
// that references the semaphore.
class Semaphore
{
public:
    virtual ~Semaphore() = default;
};
} // namespace crd::rhi
