#pragma once

#include <crd/rhi/command_buffer.hpp>
#include <crd/rhi/fence.hpp>
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
};
} // namespace crd::rhi
