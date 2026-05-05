#pragma once

#include <crd/rhi/command_buffer.hpp>
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
    virtual void present(Swapchain& swapchain) = 0;
    virtual void wait_idle() = 0;
};
} // namespace crd::rhi
