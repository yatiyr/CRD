#pragma once

#include <crd/renderer/frame_graph.hpp>
#include <crd/rhi/image.hpp>
#include <crd/rhi/types.hpp>

namespace crd::renderer
{

// Add a swapchain-blit pass pair to the frame graph.
//
// Adds two passes:
//   "swapchain-blit"   — reads render_output as TransferSrc, writes sc_image as TransferDst,
//                        executes blit_image with linear filtering.
//   "present-barrier"  — transitions sc_image to the Present layout (empty execute; barrier only).
//
// After FrameGraph::execute() completes, sc_image is in the correct layout for vkQueuePresentKHR.
//
// Parameters:
//   render_output  — output_image() from the active IRenderPath (must outlive execute()).
//   sc_image       — current swapchain image (from Swapchain::current_image()).
//   render_extent  — source image dimensions (render resolution).
//   display_extent — destination image dimensions (swapchain surface size).
//
// Returns the ImageHandle of the imported swapchain image.
//
// NOTE: Call after IRenderPath::build() and before FrameGraph::build().
[[nodiscard]] ImageHandle add_swapchain_blit_pass(FrameGraph& fg,
                                                   ImageHandle render_output,
                                                   rhi::Image& sc_image,
                                                   rhi::Extent2D render_extent,
                                                   rhi::Extent2D display_extent);

} // namespace crd::renderer
