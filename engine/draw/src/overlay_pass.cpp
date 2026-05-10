// crd-draw -- add_draw_overlay_pass impl (Phase 3.1 v1a-draw d0c, ADR-0066 sec 9).
//
// Frame-graph pass that composes the contents of a `RenderBuffer` over an
// imported scene_color attachment, with optional depth integration via
// scene_depth (read-only). Lines are rendered as instanced screen-space
// quads using the `line_aa` shader pair created by `crd::draw::init`.

#include <crd/draw/overlay_pass.hpp>

#include <crd/draw/detail/gpu_types.hpp>
#include <crd/draw/render_buffer.hpp>
#include <crd/draw/renderer.hpp>
#include <crd/log/log.hpp>
#include <crd/rhi/buffer.hpp>
#include <crd/rhi/command_buffer.hpp>
#include <crd/rhi/image.hpp>
#include <crd/rhi/pipeline.hpp>

#include <cstring>

CRD_DEFINE_LOG_CHANNEL(g_log_overlay, "DrawOverlay", crd::log::LogLevel::Info)

namespace crd::draw
{
void add_draw_overlay_pass(crd::renderer::FrameGraph&  fg,
                           crd::renderer::ImageHandle  scene_color,
                           crd::renderer::ImageHandle  scene_depth,
                           const RenderBuffer&         buffer,
                           const OverlayPassConfig&    config,
                           const char*                 name)
{
    // Early-out (no-op) if the renderer wasn't initialised. Lets consumers
    // wire add_draw_overlay_pass unconditionally during development without
    // crashing on a missing init() call.
    if (!is_initialised())
    {
        return;
    }

    auto builder = fg.add_pass(name);

    // We compose ON TOP of scene_color (alpha-blend), so the access mode is
    // ColorWrite. Depth is read for Depth_Test mode primitives (the line
    // pipeline has depth_write=false; we never modify depth ourselves).
    builder.write(scene_color, crd::rhi::ImageAccess::ColorWrite);
    if (scene_depth.is_valid())
    {
        builder.read(scene_depth, crd::rhi::ImageAccess::DepthRead);
    }

    // Capture the buffer + config by value into the lambda. The buffer is
    // the consumer's responsibility to keep alive across the frame; we
    // store a const-pointer to it here. Same for the FrameContext-derived
    // config. Both are read-only from inside the execute callback.
    const auto* buf_ptr = &buffer;

    builder.set_execute(
        [buf_ptr, config, scene_color, scene_depth](crd::renderer::FrameResources& res,
                                                     crd::rhi::CommandBuffer&       cmd)
        {
            auto& s = detail::renderer_state();

            const auto lines     = buf_ptr->lines();
            const auto triangles = buf_ptr->triangles();
            const crd::usize line_count = lines.size();
            const crd::usize tri_count  = triangles.size();

            // Resolve attachments.
            auto* color_img = res.get(scene_color);
            auto* depth_img = scene_depth.is_valid() ? res.get(scene_depth) : nullptr;
            if (color_img == nullptr)
            {
                CRD_LOG_ERROR(g_log_overlay, "scene_color image is null -- skipping overlay");
                return;
            }

            // Begin overlay rendering -- load existing color (composite over),
            // store result; depth load+read-only (we don't modify depth).
            crd::rhi::RenderingDepthAttachmentInfo depth_att{};
            const crd::rhi::RenderingDepthAttachmentInfo* depth_att_ptr = nullptr;
            if (depth_img != nullptr)
            {
                depth_att.image    = depth_img;
                depth_att.load_op  = crd::rhi::LoadOp::Load;
                depth_att.store_op = crd::rhi::StoreOp::Store;
                depth_att_ptr      = &depth_att;
            }

            const crd::rhi::Extent2D extent{
                static_cast<crd::u32>(config.viewport_px.x),
                static_cast<crd::u32>(config.viewport_px.y),
            };
            cmd.begin_rendering({extent,
                                 {color_img, crd::rhi::LoadOp::Load, crd::rhi::StoreOp::Store, {}},
                                 depth_att_ptr});
            cmd.set_viewport(extent);
            cmd.set_scissor({0, 0, extent.width, extent.height});

            // Push constants are shared by all pipelines (single 128-byte
            // layout per ADR-0066 sec 19.1).
            detail::DrawPushConstants pc{};
            pc.view_proj       = config.view_proj;
            pc.viewport_px     = config.viewport_px;
            pc.category_mask   = config.category_mask;
            pc.time_s          = config.time_s;
            pc.camera_pos      = config.grid.camera_pos;
            pc.primary_color   = config.grid.primary_color;
            pc.secondary_color = config.grid.secondary_color;
            pc.plane_y         = config.grid.plane_y;
            pc.primary_cell    = config.grid.primary_cell;
            pc.secondary_cell  = config.grid.secondary_cell;
            pc.fade_distance   = config.grid.fade_distance;
            pc.axis_x_color    = config.grid.axis_x_color;
            pc.axis_z_color    = config.grid.axis_z_color;

            const crd::u32 slot = config.frame_in_flight_index % s.config.frames_in_flight;

            // -- d2-grid: infinite floor grid (renders first, under everything) --
            if (config.grid.enabled && s.grid_pipeline)
            {
                cmd.bind_pipeline(*s.grid_pipeline);
                cmd.push_constants(*s.pipeline_layout,
                                   crd::rhi::ShaderStage::Vertex | crd::rhi::ShaderStage::Fragment,
                                   0, static_cast<crd::u32>(sizeof(pc)), &pc);
                // 2 triangles (6 vertices) emitted by gl_VertexIndex; no vertex buffer.
                cmd.draw_instanced(6, 1, 0, 0);
            }

            // d2-depth: bin primitives by DepthMode into 3 buckets, draw
            // each via the corresponding pipeline. XRay primitives appear
            // in TWO buckets simultaneously: full-color into Test (visible
            // portion shows) and dimmed-color into GreaterDimmed (occluded
            // portion shows). Per ADR-0066 sec 19.1.
            constexpr crd::u32 kVariantTest          = 0;
            constexpr crd::u32 kVariantAlways        = 1;
            constexpr crd::u32 kVariantGreaterDimmed = 2;
            constexpr crd::u32 kVariantCount         = 3;

            // Pre-bin per (primitive_kind, variant). Use indices into the
            // source spans + a paired "is_dimmed" flag so XRay's two emissions
            // stay distinguishable when packing GPU instance data.
            crd::containers::Array<crd::u32> tri_bins[kVariantCount];
            crd::containers::Array<crd::u32> line_bins[kVariantCount];

            auto variant_of = [](DepthMode m) noexcept -> crd::u32 {
                switch (m)
                {
                    case DepthMode::Test:   return kVariantTest;
                    case DepthMode::Always: return kVariantAlways;
                    case DepthMode::XRay:   return kVariantTest; // primary; second emission added explicitly
                }
                return kVariantAlways;
            };

            for (crd::usize i = 0; i < tri_count; ++i)
            {
                const auto m = triangles[i].flags.depth();
                tri_bins[variant_of(m)].push_back(static_cast<crd::u32>(i));
                if (m == DepthMode::XRay)
                {
                    tri_bins[kVariantGreaterDimmed].push_back(static_cast<crd::u32>(i));
                }
            }
            for (crd::usize i = 0; i < line_count; ++i)
            {
                const auto m = lines[i].flags.depth();
                line_bins[variant_of(m)].push_back(static_cast<crd::u32>(i));
                if (m == DepthMode::XRay)
                {
                    line_bins[kVariantGreaterDimmed].push_back(static_cast<crd::u32>(i));
                }
            }

            // -- Solid triangles per variant -- compose-on-top order:
            //    Test (depth-tested visible) -> Always (everywhere) ->
            //    GreaterDimmed (XRay occluded). Within each variant, the
            //    multi-batch loop from d2-overflow handles unbounded counts.
            constexpr crd::u32 kVariantOrder[kVariantCount] = {
                kVariantTest, kVariantAlways, kVariantGreaterDimmed};

            auto dim_color = [](crd::u32 packed) noexcept -> crd::u32 {
                // Multiply alpha by ~0.3 for the GreaterDimmed variant (XRay).
                const crd::u32 a = (packed >> 24) & 0xFFu;
                const crd::u32 dimmed_a = (a * 77u) >> 8; // 77/256 ≈ 0.30
                return (packed & 0x00FF'FFFFu) | (dimmed_a << 24);
            };

            for (const crd::u32 v : kVariantOrder)
            {
                const auto& bin = tri_bins[v];
                if (bin.empty()) continue;
                auto* tri_buf            = s.triangle_instance_buffers[slot].get();
                const crd::u32 batch_cap = s.config.max_triangles_per_frame;
                cmd.bind_pipeline(*s.triangle_pipelines[v]);
                cmd.push_constants(*s.pipeline_layout,
                                   crd::rhi::ShaderStage::Vertex | crd::rhi::ShaderStage::Fragment,
                                   0, static_cast<crd::u32>(sizeof(pc)), &pc);
                cmd.bind_vertex_buffer(*tri_buf, 0);

                const crd::usize n = bin.size();
                const bool dim_alpha = (v == kVariantGreaterDimmed);
                for (crd::usize off = 0; off < n; off += batch_cap)
                {
                    const crd::usize batch_n = (n - off > batch_cap) ? batch_cap : (n - off);
                    if (void* mapped = tri_buf->map())
                    {
                        auto* gpu = static_cast<detail::TriangleInstanceGpu*>(mapped);
                        for (crd::usize i = 0; i < batch_n; ++i)
                        {
                            const auto& t = triangles[bin[off + i]];
                            gpu[i].v0           = t.a;
                            gpu[i].v1           = t.b;
                            gpu[i].v2           = t.c;
                            gpu[i].color_packed = dim_alpha ? dim_color(t.color) : t.color;
                            gpu[i].flags_raw    = t.flags.raw;
                        }
                        tri_buf->unmap();
                        cmd.draw_instanced(3, static_cast<crd::u32>(batch_n), 0, 0);
                    }
                }
            }

            // -- Lines per variant (same compose-on-top order) --
            for (const crd::u32 v : kVariantOrder)
            {
                const auto& bin = line_bins[v];
                if (bin.empty()) continue;
                auto* line_buf           = s.line_instance_buffers[slot].get();
                const crd::u32 batch_cap = s.config.max_lines_per_frame;
                cmd.bind_pipeline(*s.line_pipelines[v]);
                cmd.push_constants(*s.pipeline_layout,
                                   crd::rhi::ShaderStage::Vertex | crd::rhi::ShaderStage::Fragment,
                                   0, static_cast<crd::u32>(sizeof(pc)), &pc);
                cmd.bind_vertex_buffer(*line_buf, 0);

                const crd::usize n = bin.size();
                const bool dim_alpha = (v == kVariantGreaterDimmed);
                for (crd::usize off = 0; off < n; off += batch_cap)
                {
                    const crd::usize batch_n = (n - off > batch_cap) ? batch_cap : (n - off);
                    if (void* mapped = line_buf->map())
                    {
                        auto* gpu = static_cast<detail::LineInstanceGpu*>(mapped);
                        for (crd::usize i = 0; i < batch_n; ++i)
                        {
                            const auto& l = lines[bin[off + i]];
                            gpu[i].start        = l.a;
                            gpu[i].end          = l.b;
                            gpu[i].color_packed = dim_alpha ? dim_color(l.color) : l.color;
                            gpu[i].flags_raw    = l.flags.raw;
                            gpu[i].width        = l.width;
                        }
                        line_buf->unmap();
                        cmd.draw_instanced(6, static_cast<crd::u32>(batch_n), 0, 0);
                    }
                }
            }

            cmd.end_rendering();
        });
}

} // namespace crd::draw
