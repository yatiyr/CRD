#pragma once

#include <crd/rhi/command_buffer.hpp>
#include <crd/rhi/fence.hpp>
#include <crd/rhi/semaphore.hpp>
#include <crd/rhi/swapchain.hpp>

namespace crd::rhi
{
class Queue
{
public:
    virtual ~Queue() = default;

    virtual bool submit(CommandBuffer& command_buffer, Swapchain& swapchain) = 0;
    // One-shot headless submit (no semaphores). Blocks until the GPU is idle.
    virtual void submit_and_wait(CommandBuffer& command_buffer) = 0;
    // Phase 3.0 v1o1 — non-blocking submit. The work runs on the GPU; `fence`
    // is signalled when it completes. Caller polls `fence.is_signaled()` per
    // frame and consumes downstream resources only once it returns true.
    // No swapchain semaphores are inserted — this path is for off-frame work
    // (asset upload, async compute, transfer). The fence MUST be in the
    // unsignalled state at submit time (newly created, or reset after the
    // previous submission completed).
    virtual void submit(CommandBuffer& command_buffer, Fence& fence) = 0;
    virtual void present(Swapchain& swapchain) = 0;
    virtual void wait_idle() = 0;

    // -----------------------------------------------------------------
    // Phase 3.1.7.6 v0d additions — ALL NEW VIRTUALS GO AT THE END.
    // See Device.hpp § vtable-stability discipline for why.
    // -----------------------------------------------------------------

    // Phase 3.1.7.6 v0d (ADR-0080 D10) — full submit shape with cross-queue
    // semaphores. Single source of truth for the async-compute handoff
    // path. Existing back-compat overloads above stay for v0a/b/c callers
    // that don't need semaphores. Vulkan impls may delegate them through
    // this internally.
    //
    // The submission waits on every `info.wait_semaphores[i].semaphore`
    // at its `wait_stage` before executing, then runs the command buffer,
    // signals every `info.signal_semaphores[i]` on completion, and
    // signals `info.signal_fence` (if non-null) when the GPU has fully
    // drained the submission.
    virtual void submit(const SubmitInfo& info) = 0;
};
} // namespace crd::rhi
