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
    virtual void present(Swapchain& swapchain) = 0;
    virtual void wait_idle() = 0;
};
} // namespace crd::rhi
