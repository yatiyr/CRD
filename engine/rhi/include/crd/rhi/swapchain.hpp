#pragma once

#include <crd/rhi/image.hpp>

namespace crd::rhi
{
class Swapchain
{
public:
    virtual ~Swapchain() = default;

    [[nodiscard]] virtual const SwapchainDesc& desc() const noexcept = 0;
    virtual void acquire_next_image() = 0;
    [[nodiscard]] virtual crd::u32 current_image_index() const noexcept = 0;
    [[nodiscard]] virtual Image& current_image() noexcept = 0;
};
} // namespace crd::rhi
