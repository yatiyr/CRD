#pragma once

#include <crd/containers/array.hpp>
#include <crd/preset/preset_target.hpp>
#include <crd/preset/quality_preset.hpp>
#include <crd/renderer/frame_graph.hpp>
#include <crd/renderer/material_template.hpp>
#include <crd/renderer/render_path.hpp>
#include <crd/renderer/renderer.hpp>
#include <crd/rhi/buffer.hpp>
#include <crd/rhi/descriptor.hpp>
#include <crd/rhi/device.hpp>
#include <crd/rhi/image.hpp>
#include <crd/rhi/types.hpp>

#include <memory>

namespace crd::renderer
{

// First concrete IRenderPath: single forward pass with optional depth prepass.
//
// Pass layout (declared into the FrameGraph each frame):
//   1. "depth-prepass"   — writes depth; draws opaque items vertex-only to populate depth buffer
//   2. "main-color"      — writes color + reads depth; draws opaque + masked items with full shading
//
// Frequency layout (matches the global convention):
//   Set 0  = per-frame    (camera UBO — PerFrameUbo, binding 0, Vertex|Fragment)
//   Push   = per-draw     (model matrix — PerDrawPush, 64 bytes, Vertex stage)
//
// Lifetime rules:
//   - Color and depth render targets are owned by ForwardRenderPath and recreated on resize().
//   - Per-frame UBOs and descriptor sets are indexed by (frame_index % frames_in_flight).
//   - The DescriptorAllocator is NOT owned; callers manage its begin_frame() lifecycle.
//   - The PipelineResolver is NOT owned; must outlive this object.
class ForwardRenderPath final : public IRenderPath, public crd::preset::IPresetTarget
{
public:
    // Create a ForwardRenderPath. Returns nullptr if any allocation fails.
    //
    // device          — RHI device; must outlive this object.
    // resolver        — pipeline resolver used in both passes; must outlive this object.
    // allocator       — descriptor allocator for per-frame sets; must outlive this object.
    // initial_extent  — initial viewport size; call resize() when the swapchain resizes.
    // frames_in_flight — ring size for per-frame UBO and descriptor set buffers.
    [[nodiscard]] static std::unique_ptr<ForwardRenderPath>
    create(rhi::Device& device, PipelineResolver& resolver, rhi::DescriptorAllocator& allocator,
           rhi::Extent2D initial_extent, crd::u32 frames_in_flight = 2);

    // Access the pipeline layout created by this render path.
    // Pass this to PipelineResolver implementations so pipelines are compatible.
    [[nodiscard]] rhi::PipelineLayout& pipeline_layout() noexcept { return *m_pipeline_layout; }

    // Direct access to the color render target — used by sandbox to blit to swapchain.
    [[nodiscard]] rhi::Image& color_image() noexcept { return *m_color_image; }

    // IRenderPath interface --------------------------------------------------

    // Register depth-prepass + main-color-pass into fg for the current frame.
    // draw_list must remain valid until FrameGraph::execute() completes.
    void build(FrameGraph& fg, const DrawList& draw_list, const FrameContext& ctx) override;

    // Return the color image handle produced by the main-color pass.
    // Only valid after build() has been called for the current frame.
    [[nodiscard]] ImageHandle output_image() const noexcept override { return m_color_handle; }

    // Recreate size-dependent resources (render targets). Call before build() after a resize.
    void resize(rhi::Extent2D new_extent) override;

    // IPresetTarget interface ------------------------------------------------
    //
    // Phase 3.0 v1o3: ForwardRenderPath consumes `QualityPreset` directly,
    // making it a real `IPresetTarget` rather than a stub recipient.
    // The first wired field is `enable_depth_prepass` — toggling it skips
    // the depth-prepass DRAWS inside an always-declared depth-prepass.
    // Other fields (shadow_resolution, msaa_samples, ssr_quality,
    // ssao_quality, post_fx[]) are cached on the path and read back by
    // observability surfaces (sandbox ImGui panel) until their consuming
    // systems (shadow path, MSAA target, SSR/SSAO/post-fx passes) ship in
    // Phase 3.5+. The pass-declaration topology never changes — only the
    // draw count does — so framegraph barriers / transient aliasing /
    // tests stay invariant under preset changes.
    //
    // `using IPresetTarget::apply;` imports the un-overridden `CameraPreset`
    // overload so GCC's `-Woverloaded-virtual` doesn't fire under the
    // partial-override convention pinned in `preset_target.hpp`.
    using crd::preset::IPresetTarget::apply;
    void apply(const crd::preset::QualityPreset& preset) override;

    // Read-back the most-recently-applied QualityPreset. Default-
    // constructed before the first `apply()` call (matches schema
    // defaults, observably equivalent to a fresh QualityPreset{}).
    [[nodiscard]] const crd::preset::QualityPreset& quality_preset() const noexcept
    {
        return m_quality;
    }

private:
    ForwardRenderPath() = default;

    void recreate_render_targets();

    // Per-template pipeline pair compiled by get_or_compile_mat_pipelines().
    struct MatPipelineEntry
    {
        const MaterialTemplate* tmpl  = nullptr; // pointer-identity cache key
        rhi::Pipeline*          depth = nullptr; // non-owning; points into m_owned_mat_pipelines
        rhi::Pipeline*          color = nullptr;
    };

    MatPipelineEntry& get_or_compile_mat_pipelines(const MaterialInstance& mat);

    rhi::Device*              m_device            = nullptr;
    PipelineResolver*         m_resolver          = nullptr;
    rhi::DescriptorAllocator* m_allocator         = nullptr;
    rhi::Extent2D             m_extent{};
    crd::u32                  m_frames_in_flight  = 2;

    std::unique_ptr<rhi::Image> m_color_image; // B8G8R8A8Unorm, ColorAttachment
    std::unique_ptr<rhi::Image> m_depth_image; // D32Sfloat, DepthStencilAttachment

    crd::containers::Array<std::unique_ptr<rhi::Buffer>>        m_per_frame_ubos;
    crd::containers::Array<std::unique_ptr<rhi::DescriptorSet>> m_per_frame_sets;

    crd::containers::Array<MatPipelineEntry>                    m_mat_cache;
    crd::containers::Array<std::unique_ptr<rhi::Pipeline>>      m_owned_mat_pipelines;

    std::unique_ptr<rhi::DescriptorSetLayout> m_per_frame_set_layout;
    std::unique_ptr<rhi::PipelineLayout>       m_pipeline_layout;

    // Set each frame by build(); used by output_image() and execute callbacks.
    ImageHandle        m_color_handle{};
    ImageHandle        m_depth_handle{};
    const DrawList*    m_draw_list    = nullptr; // non-owning; valid only between build() and execute()

    // Most-recently-applied QualityPreset. Default = schema defaults so
    // build() works without an explicit apply() call (the v1 behaviour
    // — depth prepass enabled).
    crd::preset::QualityPreset m_quality{};
};

} // namespace crd::renderer
