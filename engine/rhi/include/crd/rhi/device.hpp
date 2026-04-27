#pragma once

#include <crd/rhi/buffer.hpp>
#include <crd/rhi/command_buffer.hpp>
#include <crd/rhi/pipeline.hpp>
#include <crd/rhi/queue.hpp>
#include <crd/rhi/shader_module.hpp>
#include <crd/rhi/swapchain.hpp>

#include <memory>

namespace crd::rhi
{
class Device
{
public:
    virtual ~Device() = default;

    [[nodiscard]] virtual BackendApi api() const noexcept = 0;

    [[nodiscard]] virtual std::unique_ptr<Swapchain> create_swapchain(const SwapchainDesc& desc) = 0;
    [[nodiscard]] virtual std::unique_ptr<Buffer> create_buffer(const BufferDesc& desc) = 0;
    [[nodiscard]] virtual std::unique_ptr<Image> create_image(const ImageDesc& desc) = 0;
    [[nodiscard]] virtual std::unique_ptr<ShaderModule> create_shader_module(const ShaderModuleDesc& desc) = 0;
    [[nodiscard]] virtual std::unique_ptr<Pipeline> create_graphics_pipeline(const GraphicsPipelineDesc& desc) = 0;
    [[nodiscard]] virtual std::unique_ptr<CommandBuffer> create_command_buffer() = 0;

    [[nodiscard]] virtual Queue& graphics_queue() noexcept = 0;
    virtual void wait_idle() = 0;
};
} // namespace crd::rhi
