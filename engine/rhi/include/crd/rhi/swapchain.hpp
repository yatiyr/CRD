#pragma once

#include <crd/rhi/image.hpp>

namespace crd::rhi
{
class Swapchain
{
public:
    virtual ~Swapchain() = default;

    [[nodiscard]] virtual const SwapchainDesc& desc() const noexcept = 0;
    [[nodiscard]] virtual bool acquire_next_image() = 0;
    [[nodiscard]] virtual crd::u32 current_image_index() const noexcept = 0;
    [[nodiscard]] virtual Image& current_image() noexcept = 0;

    // Recreate the swapchain for a new window size. Must be called only when the GPU is idle.
    virtual void resize(Extent2D new_extent) noexcept = 0;
};
} // namespace crd::rhi
