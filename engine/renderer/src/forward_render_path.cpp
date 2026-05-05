#include <crd/renderer/forward_render_path.hpp>
#include <crd/renderer/per_frame_data.hpp>
#include <crd/math/mat.hpp>
#include <crd/core/assert.hpp>

namespace crd::renderer
{

std::unique_ptr<ForwardRenderPath>
ForwardRenderPath::create(rhi::Device& device, PipelineResolver& resolver,
                          rhi::DescriptorAllocator& allocator, rhi::Extent2D initial_extent,
                          crd::u32 frames_in_flight)
{
    auto path = std::unique_ptr<ForwardRenderPath>(new ForwardRenderPath());
    path->m_device           = &device;
    path->m_resolver         = &resolver;
    path->m_allocator        = &allocator;
    path->m_extent           = initial_extent;
    path->m_frames_in_flight = frames_in_flight;

    // Set 0: per-frame camera UBO (binding 0, Vertex|Fragment stages).
    const rhi::DescriptorBinding per_frame_binding{0, rhi::DescriptorType::UniformBuffer, 1,
                                                   rhi::ShaderStage::Vertex | rhi::ShaderStage::Fragment};
    path->m_per_frame_set_layout = device.create_descriptor_set_layout(
        {crd::containers::make_span(&per_frame_binding, 1U)});
    if (!path->m_per_frame_set_layout)
        return nullptr;

    // Pipeline layout: set 0 (per-frame) + push constants (model matrix, 64 bytes, Vertex).
    const rhi::DescriptorSetLayout* set_layouts_arr[] = {path->m_per_frame_set_layout.get()};
    const rhi::PushConstantRange push_range{rhi::ShaderStage::Vertex, 0,
                                            static_cast<crd::u32>(sizeof(PerDrawPush))};
    path->m_pipeline_layout = device.create_pipeline_layout({
        crd::containers::make_span(set_layouts_arr),
        crd::containers::make_span(&push_range, 1U),
    });
    if (!path->m_pipeline_layout)
        return nullptr;

    // Allocate per-frame UBO ring and pre-size descriptor set ring.
    path->m_per_frame_ubos.resize(frames_in_flight);
    path->m_per_frame_sets.resize(frames_in_flight);
    for (crd::u32 i = 0; i < frames_in_flight; ++i)
    {
        rhi::BufferDesc ubo_desc{sizeof(PerFrameUbo),
                                 static_cast<crd::u32>(rhi::BufferUsage::Uniform),
                                 rhi::MemoryUsage::CpuToGpu};
        path->m_per_frame_ubos[i] = device.create_buffer(ubo_desc);
        if (!path->m_per_frame_ubos[i])
            return nullptr;
    }

    path->recreate_render_targets();
    if (!path->m_color_image || !path->m_depth_image)
        return nullptr;

    return path;
}

void ForwardRenderPath::recreate_render_targets()
{
    m_color_image = m_device->create_image(
        {m_extent, rhi::Format::B8G8R8A8Unorm,
         rhi::ImageUsage::ColorAttachment | rhi::ImageUsage::TransferSrc, 1, 1});
    m_depth_image = m_device->create_image(
        {m_extent, rhi::Format::D32Sfloat,
         rhi::enum_bits(rhi::ImageUsage::DepthStencilAttachment), 1, 1});
}

void ForwardRenderPath::resize(rhi::Extent2D new_extent)
{
    if (new_extent.width == m_extent.width && new_extent.height == m_extent.height)
        return;
    m_extent = new_extent;
    recreate_render_targets();
}

// Standard 48-byte interleaved vertex layout for all surface geometry.
static const rhi::VertexBindingDesc   k_surface_binding{0, 48, rhi::VertexInputRate::Vertex};
static const rhi::VertexAttributeDesc k_surface_attrs[4] = {
    {0, 0, rhi::Format::R32G32B32Sfloat,    0},   // position
    {1, 0, rhi::Format::R32G32B32Sfloat,    12},  // normal
    {2, 0, rhi::Format::R32G32Sfloat,       24},  // uv0
    {3, 0, rhi::Format::R32G32B32A32Sfloat, 32},  // tangent
};

ForwardRenderPath::MatPipelineEntry&
ForwardRenderPath::get_or_compile_mat_pipelines(const MaterialInstance& mat)
{
    const MaterialTemplate* tmpl = mat.tmpl().get();

    // Search by template pointer identity.
    for (auto& entry : m_mat_cache)
    {
        if (entry.tmpl == tmpl)
            return entry;
    }

    MatPipelineEntry entry;
    entry.tmpl = tmpl;

    if (tmpl == nullptr || tmpl->domain != MaterialDomain::Surface)
    {
        m_mat_cache.push_back(entry);
        return m_mat_cache[m_mat_cache.size() - 1U];
    }

    // Depth prepass: vertex-only pipeline using the depth prepass (or Forward fallback) vert shader.
    const PassShaderPair& depth_pair = mat.variant_for_pass(PassType::DepthPrepass);
    const crd::shader::ShaderResource* depth_vert_res = depth_pair.vert.get();
    if (depth_vert_res != nullptr && !depth_vert_res->spirv.empty())
    {
        auto vert_mod = m_device->create_shader_module(
            {rhi::ShaderStage::Vertex, "main",
             crd::containers::make_span(depth_vert_res->spirv.data(), depth_vert_res->spirv.size())});
        if (vert_mod)
        {
            rhi::GraphicsPipelineDesc desc;
            desc.vertex_shader        = vert_mod.get();
            desc.fragment_shader      = nullptr;
            desc.topology             = rhi::PrimitiveTopology::TriangleList;
            desc.viewport_extent      = m_extent;
            desc.color_format         = rhi::Format::Undefined;
            desc.depth_format         = rhi::Format::D32Sfloat;
            desc.vertex_bindings      = crd::containers::make_span(&k_surface_binding, 1U);
            desc.vertex_attributes    = crd::containers::make_span(k_surface_attrs, 4U);
            desc.enable_depth_test    = true;
            desc.enable_blend         = false;
            desc.use_dynamic_viewport = true;
            desc.pipeline_layout      = m_pipeline_layout.get();

            auto pipeline = m_device->create_graphics_pipeline(desc);
            if (pipeline)
            {
                entry.depth = pipeline.get();
                m_owned_mat_pipelines.push_back(std::move(pipeline));
            }
        }
    }

    // Color pass: full vert+frag pipeline using the Forward pair.
    const PassShaderPair& color_pair = mat.variant_for_pass(PassType::Forward);
    const crd::shader::ShaderResource* color_vert_res = color_pair.vert.get();
    const crd::shader::ShaderResource* color_frag_res = color_pair.frag.get();
    if (color_vert_res != nullptr && !color_vert_res->spirv.empty() &&
        color_frag_res != nullptr && !color_frag_res->spirv.empty())
    {
        auto vert_mod = m_device->create_shader_module(
            {rhi::ShaderStage::Vertex, "main",
             crd::containers::make_span(color_vert_res->spirv.data(), color_vert_res->spirv.size())});
        auto frag_mod = m_device->create_shader_module(
            {rhi::ShaderStage::Fragment, "main",
             crd::containers::make_span(color_frag_res->spirv.data(), color_frag_res->spirv.size())});
        if (vert_mod && frag_mod)
        {
            rhi::GraphicsPipelineDesc desc;
            desc.vertex_shader        = vert_mod.get();
            desc.fragment_shader      = frag_mod.get();
            desc.topology             = rhi::PrimitiveTopology::TriangleList;
            desc.viewport_extent      = m_extent;
            desc.color_format         = rhi::Format::B8G8R8A8Unorm;
            desc.depth_format         = rhi::Format::D32Sfloat;
            desc.vertex_bindings      = crd::containers::make_span(&k_surface_binding, 1U);
            desc.vertex_attributes    = crd::containers::make_span(k_surface_attrs, 4U);
            desc.enable_depth_test    = true;
            desc.enable_blend         = false;
            desc.use_dynamic_viewport = true;
            desc.pipeline_layout      = m_pipeline_layout.get();

            auto pipeline = m_device->create_graphics_pipeline(desc);
            if (pipeline)
            {
                entry.color = pipeline.get();
                m_owned_mat_pipelines.push_back(std::move(pipeline));
            }
        }
    }

    m_mat_cache.push_back(entry);
    return m_mat_cache[m_mat_cache.size() - 1U];
}

void ForwardRenderPath::build(FrameGraph& fg, const DrawList& draw_list, const FrameContext& ctx)
{
    m_draw_list = &draw_list;
    const crd::u32 slot = ctx.frame_index % m_frames_in_flight;

    // Update per-frame UBO.
    auto& ubo_buf = *m_per_frame_ubos[slot];
    auto* ubo = static_cast<PerFrameUbo*>(ubo_buf.map());
    if (ubo)
    {
        const crd::math::Mat4f vp = ctx.camera.projection * ctx.camera.view;
        ubo->view          = ctx.camera.view;
        ubo->proj          = ctx.camera.projection;
        ubo->view_proj     = vp;
        ubo->inv_view_proj = crd::math::inverse(vp);
        ubo->camera_pos_ws = crd::math::Vec4f(ctx.camera_position.x, ctx.camera_position.y,
                                              ctx.camera_position.z, 0.0F);
        ubo->viewport_width  = static_cast<crd::f32>(m_extent.width);
        ubo->viewport_height = static_cast<crd::f32>(m_extent.height);
        ubo->time_seconds    = 0.0F;
        ubo->_pad            = 0.0F;
        ubo_buf.unmap();
    }

    // Allocate per-frame descriptor set and wire binding 0 to the camera UBO.
    m_per_frame_sets[slot] = m_allocator->allocate(*m_per_frame_set_layout);
    if (m_per_frame_sets[slot])
        m_per_frame_sets[slot]->update_buffer(0, ubo_buf, 0, sizeof(PerFrameUbo));

    // Import render targets; both start Undefined (cleared on first use each frame).
    m_color_handle = fg.import(m_color_image.get(), rhi::ImageAccess::Undefined);
    m_depth_handle = fg.import(m_depth_image.get(), rhi::ImageAccess::Undefined);

    // --- Pass 1: depth prepass ---
    // Depth-only: no color attachment. Clears and fills the depth buffer with opaque geometry.
    {
        auto builder = fg.add_pass("depth-prepass");
        builder.write(m_depth_handle, rhi::ImageAccess::DepthWrite);
        builder.set_execute([this, slot](FrameResources& res, rhi::CommandBuffer& cmd)
        {
            auto* depth = res.get(m_depth_handle);
            // Reverse-Z: clear to 0.0 (the "far" sentinel; GREATER_OR_EQUAL lets anything closer pass).
            const rhi::RenderingDepthAttachmentInfo depth_att{depth, rhi::LoadOp::Clear,
                                                              rhi::StoreOp::Store, {0.0F, 0}};
            cmd.begin_rendering({m_extent,
                                 {nullptr, rhi::LoadOp::DontCare, rhi::StoreOp::DontCare, {}},
                                 &depth_att});
            cmd.set_viewport(m_extent);
            cmd.set_scissor({0, 0, m_extent.width, m_extent.height});

            // Bind per-frame descriptor set (set 0: camera matrices) — vertex shader uses it.
            if (m_per_frame_sets[slot])
            {
                rhi::DescriptorSet* sets[] = {m_per_frame_sets[slot].get()};
                cmd.bind_descriptor_sets(*m_pipeline_layout, 0,
                                         crd::containers::make_span(sets, 1U));
            }

            m_resolver->begin_pass(PassType::DepthPrepass);

            for (const auto& item : m_draw_list->opaque)
            {
                rhi::Pipeline* pipeline = nullptr;
                if (item.material != nullptr)
                {
                    auto& mat_entry = get_or_compile_mat_pipelines(*item.material);
                    pipeline = mat_entry.depth;
                }
                else
                {
                    pipeline = m_resolver->resolve_pipeline(item.handoff);
                }
                if (!pipeline)
                    continue;
                cmd.bind_pipeline(*pipeline);
                const PerDrawPush push{item.model};
                cmd.push_constants(*m_pipeline_layout, rhi::ShaderStage::Vertex,
                                   0, static_cast<crd::u32>(sizeof(PerDrawPush)), &push);
                cmd.bind_vertex_buffer(*item.vertex_buffer, 0);
                if (item.index_buffer)
                {
                    cmd.bind_index_buffer(*item.index_buffer, 0, item.index_type);
                    cmd.draw_indexed(item.index_count, 0, 0);
                }
                else
                {
                    cmd.draw(item.vertex_count, 0);
                }
            }

            cmd.end_rendering();
        });
    }

    // --- Pass 2: main color pass ---
    // Clears color; loads depth from prepass (same DepthWrite access — no barrier inserted).
    {
        auto builder = fg.add_pass("main-color");
        builder.write(m_color_handle, rhi::ImageAccess::ColorWrite);
        builder.write(m_depth_handle, rhi::ImageAccess::DepthWrite);
        builder.set_execute([this, slot](FrameResources& res, rhi::CommandBuffer& cmd)
        {
            auto* color = res.get(m_color_handle);
            auto* depth = res.get(m_depth_handle);

            const rhi::RenderingColorAttachmentInfo color_att{color, rhi::LoadOp::Clear,
                                                              rhi::StoreOp::Store,
                                                              {0.07F, 0.08F, 0.12F, 1.0F}};
            const rhi::RenderingDepthAttachmentInfo depth_att{depth, rhi::LoadOp::Load,
                                                              rhi::StoreOp::Store, {}};

            cmd.begin_rendering({m_extent, color_att, &depth_att});
            cmd.set_viewport(m_extent);
            cmd.set_scissor({0, 0, m_extent.width, m_extent.height});

            // Bind per-frame descriptor set (set 0: camera matrices).
            if (m_per_frame_sets[slot])
            {
                rhi::DescriptorSet* sets[] = {m_per_frame_sets[slot].get()};
                cmd.bind_descriptor_sets(*m_pipeline_layout, 0,
                                         crd::containers::make_span(sets, 1U));
            }

            m_resolver->begin_pass(PassType::Forward);

            auto draw_items = [&](const crd::containers::Array<DrawItem>& items)
            {
                for (const auto& item : items)
                {
                    rhi::Pipeline* pipeline = nullptr;
                    if (item.material != nullptr)
                    {
                        auto& mat_entry = get_or_compile_mat_pipelines(*item.material);
                        pipeline = mat_entry.color;
                    }
                    else
                    {
                        pipeline = m_resolver->resolve_pipeline(item.handoff);
                    }
                    if (!pipeline)
                        continue;
                    cmd.bind_pipeline(*pipeline);
                    const PerDrawPush push{item.model};
                    cmd.push_constants(*m_pipeline_layout, rhi::ShaderStage::Vertex,
                                       0, static_cast<crd::u32>(sizeof(PerDrawPush)), &push);
                    cmd.bind_vertex_buffer(*item.vertex_buffer, 0);
                    if (item.index_buffer)
                    {
                        cmd.bind_index_buffer(*item.index_buffer, 0, item.index_type);
                        cmd.draw_indexed(item.index_count, 0, 0);
                    }
                    else
                    {
                        cmd.draw(item.vertex_count, 0);
                    }
                }
            };

            draw_items(m_draw_list->opaque);
            draw_items(m_draw_list->masked);

            cmd.end_rendering();
        });
    }
}

} // namespace crd::renderer
