#pragma once

#include <crd/renderer/frame_graph.hpp>
#include <crd/renderer/renderer.hpp>
#include <crd/rhi/types.hpp>

namespace crd::renderer
{

// IRenderPath implements a complete rendering technique as a declared set of frame graph passes.
//
// Contract:
//   Each frame: build() registers all passes into the frame graph via PassBuilder, then the
//   engine calls FrameGraph::build() and FrameGraph::execute(). The render path does not
//   record commands directly — pass execute callbacks do that after barriers are inserted.
//
// Ownership:
//   A render path owns all internal render targets (color, depth, G-buffer, ping-pong
//   buffers, etc.) as transient frame graph resources. Swapchain images are imported
//   externally by the engine before build() is called each frame.
//
// Resize:
//   Viewport changes invalidate size-dependent state (render target dimensions, cached
//   descriptor data, cluster grid parameters, etc.). Call resize() before the next build()
//   when the viewport changes. The frame graph automatically reallocates transient images
//   at the new size on the next execute() call.
//
// Extension points (planned render paths, all via frame graph passes):
//   v1g  ForwardRenderPath         — depth prepass + main color pass
//   v1i  ForwardPlusRenderPath     — + clustered light compute pass
//   5.3a DeferredRenderPath        — G-buffer + lighting + composite
//   5.3b VisibilityBufferRenderPath — triangle-ID + material evaluation
class IRenderPath
{
public:
    virtual ~IRenderPath() = default;

    // Declare all frame graph passes for this frame.
    // Called once per frame after Renderer::build_frame(), before FrameGraph::build().
    // draw_list is pre-bucketed and depth-sorted by Renderer::build_frame().
    virtual void build(FrameGraph& fg, const DrawList& draw_list, const FrameContext& ctx) = 0;

    // Return the ImageHandle of the composited output image for this frame.
    // Used by the engine to blit the final result onto the swapchain.
    // Only valid after a successful build() call.
    [[nodiscard]] virtual ImageHandle output_image() const noexcept = 0;

    // Notify the render path that the viewport has changed.
    // Must be called before the next build() when the swapchain is resized.
    virtual void resize(rhi::Extent2D new_extent) = 0;
};

} // namespace crd::renderer
