#include <crd/renderer/swapchain_blit.hpp>
#include <crd/core/assert.hpp>

namespace crd::renderer
{

ImageHandle add_swapchain_blit_pass(FrameGraph& fg,
                                    ImageHandle render_output,
                                    rhi::Image& sc_image,
                                    rhi::Extent2D render_extent,
                                    rhi::Extent2D display_extent)
{
    auto sc_handle = fg.import(&sc_image, rhi::ImageAccess::Undefined);

    // Pass 1: blit render output → swapchain image.
    // Frame graph inserts:
    //   ColorWrite → TransferSrc for render_output
    //   Undefined  → TransferDst for sc_handle
    {
        auto builder = fg.add_pass("swapchain-blit");
        builder.read(render_output, rhi::ImageAccess::TransferSrc);
        builder.write(sc_handle,   rhi::ImageAccess::TransferDst);
        builder.set_execute(
            [render_output, sc_handle, render_extent, display_extent]
            (FrameResources& res, rhi::CommandBuffer& cmd)
            {
                auto* src = res.get(render_output);
                auto* dst = res.get(sc_handle);
                CRD_ASSERT(src != nullptr && dst != nullptr);
                cmd.blit_image(*src, *dst, render_extent, display_extent);
            });
    }

    // Pass 2: empty pass whose sole purpose is triggering TransferDst → Present barrier.
    // After execute(), the swapchain image is in VK_IMAGE_LAYOUT_PRESENT_SRC_KHR.
    {
        auto builder = fg.add_pass("present-barrier");
        builder.write(sc_handle, rhi::ImageAccess::Present);
        builder.set_execute([](FrameResources&, rhi::CommandBuffer&) {});
    }

    return sc_handle;
}

} // namespace crd::renderer
