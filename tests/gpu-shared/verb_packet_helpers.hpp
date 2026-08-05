#pragma once

// verb_packet_helpers.hpp — SHARED helpers that record the fullscreen raster verbs through the canonical command
// encoder (RAF-12.4, D-007). The verbs these wrap — draw / draw_textured / draw_shadow / draw_bindless — are being
// de-virtualized off IRasterContext into each backend's private methods, reachable ONLY through the encoder. Both
// backend frame-graph suites (crd-gpu-context-vulkan-tests + crd-gpu-context-dx12-tests) drive the SAME packet shapes
// through this one home, so a test that used to poke a verb directly now proves the encoder's lowering instead — the
// exact live RasterFullscreen precedence (0 reads -> procedural draw, 1 plain texture -> draw_textured, a DEPTH atlas
// at slot 4 -> draw_shadow, a bindless array -> draw_bindless).

#include <crd/gpu/command_model.hpp> // RasterDrawPacket / RenderingDesc / ResourceBinding / GeometryKind
#include <crd/gpu/raster_context.hpp>

namespace crd::gputest
{
namespace g = crd::gpu;

// The scene binding SLOTS the render-graph `bind_map` / `bind_atlas` write, mirrored here so the encoder keys the verb
// SHAPE off them exactly as in production (crd::gpu::detail::kSceneMapSlot / kSceneAtlasSlot): a plain base-colour MAP
// rides slot 1, the per-frame ATLAS (shadow / moment) rides slot 4. Only the encoder's verb-selection reads these; the
// downstream verb still hard-codes its own GPU binding, so the recorded draw is byte-identical to the old direct call.
inline constexpr crd::u32 kMapSlot   = 1U;
inline constexpr crd::u32 kAtlasSlot = 4U;

// One fullscreen (GeometryKind::None) draw into a single colour attachment, recorded through a fresh encoder. The
// `bindings` table selects which specialised verb the encoder lowers to. `clear` becomes the attachment's clear value
// (LoadOp::Clear — the first draw clears, matching the old verbs which always took a ClearColor).
inline void enc_fullscreen(g::IRasterContext& r, g::IRasterTarget& target, g::IRasterProgram& prog, g::ClearColor clear,
                           const g::ResourceBindingTable& bindings, crd::u32 vertex_count)
{
    auto                    enc = r.create_command_encoder();
    g::RenderingDesc        rd{};
    g::ColorAttachmentDesc  c{};
    c.target = &target;
    c.load   = g::LoadOp::Clear;
    c.clear  = clear;
    rd.color.push_back(c);
    enc->begin_rendering(rd);
    g::RasterDrawPacket pk{};
    pk.program                        = &prog;
    pk.command                        = g::RasterCommandKind::Draw;
    pk.geometry.kind                  = g::GeometryKind::None;
    pk.geometry.vertex_or_index_count = vertex_count;
    pk.bindings                       = bindings;
    enc->draw(pk);
    enc->end_rendering();
}

// procedural fullscreen (no reads) -> draw
inline void enc_draw(g::IRasterContext& r, g::IRasterTarget& target, g::IRasterProgram& prog, g::ClearColor clear,
                     crd::u32 vertex_count)
{
    enc_fullscreen(r, target, prog, clear, g::ResourceBindingTable{}, vertex_count);
}

// one plain SampledTexture (at the MAP slot, NOT the atlas slot) -> draw_textured
inline void enc_draw_textured(g::IRasterContext& r, g::IRasterTarget& target, g::IRasterProgram& prog,
                              g::ClearColor clear, g::ITexture& tex, crd::u32 vertex_count)
{
    g::ResourceBindingTable b{};
    g::ResourceBinding      sb{};
    sb.kind    = g::BindingKind::SampledTexture;
    sb.texture = &tex;
    sb.slot    = kMapSlot; // slot 1 — a plain texture; keeps shadow_atlas_from (slot 4) from claiming it
    b.push_back(sb);
    enc_fullscreen(r, target, prog, clear, b, vertex_count);
}

// a DEPTH texture at the ATLAS slot (4) -> draw_shadow (the comparison sampler is chosen from the texture's format)
inline void enc_draw_shadow(g::IRasterContext& r, g::IRasterTarget& target, g::IRasterProgram& prog, g::ClearColor clear,
                            g::ITexture& depth, crd::u32 vertex_count)
{
    g::ResourceBindingTable b{};
    g::ResourceBinding      sb{};
    sb.kind    = g::BindingKind::SampledTexture;
    sb.texture = &depth;
    sb.slot    = kAtlasSlot; // slot 4 — shadow_atlas_from keys on the SLOT, not the sampler kind (REN-40-D)
    b.push_back(sb);
    enc_fullscreen(r, target, prog, clear, b, vertex_count);
}

// a bindless texture array -> draw_bindless
inline void enc_draw_bindless(g::IRasterContext& r, g::IRasterTarget& target, g::IRasterProgram& prog,
                              g::ClearColor clear, g::ITexture* const* texture_array, crd::u32 array_count,
                              crd::u32 vertex_count)
{
    g::ResourceBindingTable b{};
    g::ResourceBinding      sb{};
    sb.kind          = g::BindingKind::BindlessTextureArray;
    sb.texture_array = texture_array;
    sb.array_count   = array_count;
    b.push_back(sb);
    enc_fullscreen(r, target, prog, clear, b, vertex_count);
}

// VRS: pipeline rate + primitive combiner. The scope's shading_rate_attachment is set to the target so the encoder
// routes to draw_vrs EVEN at pipeline rate 1x1 — the per-primitive (Replace combiner) and per-tile attachment rate
// sources still coarsen when the pipeline rate itself is 1x1 (the draw-vs-draw_vrs decision must not hinge on it alone).
inline void enc_draw_vrs(g::IRasterContext& r, g::IRasterTarget& target, g::IRasterProgram& prog, g::ClearColor clear,
                         g::ShadingRate pipeline_rate, g::ShadingRateCombiner primitive_combiner, crd::u32 vertex_count)
{
    auto                   enc = r.create_command_encoder();
    g::RenderingDesc       rd{};
    g::ColorAttachmentDesc c{};
    c.target = &target;
    c.load   = g::LoadOp::Clear;
    c.clear  = clear;
    rd.color.push_back(c);
    rd.shading_rate_attachment = &target; // a VRS draw — the rate sources (primitive / per-tile attachment) ride the target
    enc->begin_rendering(rd);
    g::RasterDrawPacket pk{};
    pk.program                        = &prog;
    pk.geometry.kind                  = g::GeometryKind::None;
    pk.geometry.vertex_or_index_count = vertex_count;
    pk.state.vrs_pipeline_rate        = pipeline_rate;
    pk.state.vrs_primitive_combiner   = primitive_combiner;
    enc->draw(pk);
    enc->end_rendering();
}

// conservative rasterization at `mode` (Off falls through to the ordinary draw — an identical result).
inline void enc_draw_conservative(g::IRasterContext& r, g::IRasterTarget& target, g::IRasterProgram& prog,
                                  g::ClearColor clear, g::ConservativeMode mode, crd::u32 vertex_count)
{
    auto                   enc = r.create_command_encoder();
    g::RenderingDesc       rd{};
    g::ColorAttachmentDesc c{};
    c.target = &target;
    c.load   = g::LoadOp::Clear;
    c.clear  = clear;
    rd.color.push_back(c);
    enc->begin_rendering(rd);
    g::RasterDrawPacket pk{};
    pk.program                        = &prog;
    pk.geometry.kind                  = g::GeometryKind::None;
    pk.geometry.vertex_or_index_count = vertex_count;
    pk.state.conservative             = mode;
    enc->draw(pk);
    enc->end_rendering();
}

// a visibility-buffer draw: an R32_UINT id target cleared to the INTEGER `clear_id` (not a float ClearColor).
inline void enc_draw_visbuffer(g::IRasterContext& r, g::IRasterTarget& target, g::IRasterProgram& prog,
                               crd::u32 clear_id, crd::u32 vertex_count)
{
    auto                   enc = r.create_command_encoder();
    g::RenderingDesc       rd{};
    g::ColorAttachmentDesc c{};
    c.target = &target;
    c.load   = g::LoadOp::Clear;
    rd.color.push_back(c);
    rd.visbuffer = true;
    rd.clear_id  = clear_id;
    enc->begin_rendering(rd);
    g::RasterDrawPacket pk{};
    pk.program                        = &prog;
    pk.geometry.kind                  = g::GeometryKind::None;
    pk.geometry.vertex_or_index_count = vertex_count;
    enc->draw(pk);
    enc->end_rendering();
}

// a depth-tested fullscreen draw: clear colour + depth, then draw with the depth test at `compare` (the target is a
// create_color_depth_target — it carries the depth buffer). None geometry + an enabled depth attachment → draw_depth.
inline void enc_draw_depth(g::IRasterContext& r, g::IRasterTarget& target, g::IRasterProgram& prog, g::ClearColor clear,
                           float clear_depth, g::DepthCompare compare, crd::u32 vertex_count)
{
    auto                   enc = r.create_command_encoder();
    g::RenderingDesc       rd{};
    g::ColorAttachmentDesc c{};
    c.target = &target;
    c.load   = g::LoadOp::Clear;
    c.clear  = clear;
    rd.color.push_back(c);
    rd.depth.enabled     = true;
    rd.depth.target      = &target; // the combined colour+depth target carries the depth buffer
    rd.depth.load        = g::LoadOp::Clear;
    rd.depth.clear_depth = clear_depth;
    rd.depth.compare     = compare;
    enc->begin_rendering(rd);
    g::RasterDrawPacket pk{};
    pk.program                        = &prog;
    pk.geometry.kind                  = g::GeometryKind::None;
    pk.geometry.vertex_or_index_count = vertex_count;
    enc->draw(pk);
    enc->end_rendering();
}

// a bindless + depth fullscreen draw (the depth-occluded displaced-geometry shape — the ocean grid). A bindless array
// AND an enabled depth attachment → draw_bindless_depth.
inline void enc_draw_bindless_depth(g::IRasterContext& r, g::IRasterTarget& target, g::IRasterProgram& prog,
                                    g::ClearColor clear, float clear_depth, g::DepthCompare compare,
                                    g::ITexture* const* textures, crd::u32 count, crd::u32 vertex_count)
{
    auto                   enc = r.create_command_encoder();
    g::RenderingDesc       rd{};
    g::ColorAttachmentDesc c{};
    c.target = &target;
    c.load   = g::LoadOp::Clear;
    c.clear  = clear;
    rd.color.push_back(c);
    rd.depth.enabled     = true;
    rd.depth.target      = &target;
    rd.depth.load        = g::LoadOp::Clear;
    rd.depth.clear_depth = clear_depth;
    rd.depth.compare     = compare;
    enc->begin_rendering(rd);
    g::RasterDrawPacket pk{};
    pk.program                        = &prog;
    pk.geometry.kind                  = g::GeometryKind::None;
    pk.geometry.vertex_or_index_count = vertex_count;
    g::ResourceBinding b{};
    b.kind          = g::BindingKind::BindlessTextureArray;
    b.texture_array = textures;
    b.array_count   = count;
    pk.bindings.push_back(b);
    enc->draw(pk);
    enc->end_rendering();
}

// a compute dispatch (the `dispatch_kernel` verb, F2) recorded through the encoder. `bufs` binds N storage buffers at
// the dispatch's binding table; the encoder lowers to each backend's private dispatch method.
inline void enc_dispatch(g::IRasterContext& r, g::IGpuProgram& kernel, crd::u32 gx, crd::u32 gy, crd::u32 gz,
                         g::IStorageBuffer* const* bufs, crd::u32 n)
{
    auto            enc = r.create_command_encoder();
    g::DispatchDesc d{};
    d.kernel   = &kernel;
    d.kind     = g::DispatchKind::Direct;
    d.groups_x = gx;
    d.groups_y = gy;
    d.groups_z = gz;
    for (crd::u32 i = 0; i < n; ++i)
    {
        g::ResourceBinding b{};
        b.kind   = g::BindingKind::StorageBuffer;
        b.buffer = bufs[i];
        d.bindings.push_back(b);
    }
    enc->dispatch(d);
}

} // namespace crd::gputest
