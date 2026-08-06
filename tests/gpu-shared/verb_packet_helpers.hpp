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

// ── F6 storage-scene: vertex/index-pulled scene draws recorded through the encoder. The encoder's StoragePull/Indexed
// lowering picks the specific draw_storage_* verb from the packet's geometry + depth attachment + bindings. ──

// the storage buffer the VS pulls geometry from — one StorageBuffer binding the encoder resolves via first_storage().
inline g::ResourceBinding storage_binding(g::IStorageBuffer& storage)
{
    g::ResourceBinding b{};
    b.kind   = g::BindingKind::StorageBuffer;
    b.buffer = &storage;
    return b;
}

// RET-6 / REN-39: the debug OVERLAY compose draw -> draw_overlay. A single Alpha-blended, colour-LOAD StoragePull
// draw with a read-only depth test; the encoder recognises the overlay by (ONE colour attachment, blend==Alpha).
// `compare`==Always requests no depth test (mirrors the verb: depth_on = has_depth && compare != Always).
inline void enc_draw_overlay(g::IRasterContext& r, g::IRasterTarget& target, g::IRasterProgram& prog,
                             g::IStorageBuffer& storage, g::DepthCompare compare, crd::u32 vertex_count)
{
    auto                   enc = r.create_command_encoder();
    g::RenderingDesc       rd{};
    g::ColorAttachmentDesc c{};
    c.target = &target;
    c.load   = g::LoadOp::Load;     // compose OVER the existing contents
    c.blend  = g::BlendMode::Alpha; // srcAlpha·(1-srcAlpha) -> the overlay signal
    rd.color.push_back(c);
    rd.depth.enabled = (compare != g::DepthCompare::Always); // read-only depth test carried by `compare`
    rd.depth.compare = compare;
    enc->begin_rendering(rd);
    g::RasterDrawPacket pk{};
    pk.program                        = &prog;
    pk.geometry.kind                  = g::GeometryKind::StoragePull;
    pk.geometry.vertex_or_index_count = vertex_count;
    pk.bindings.push_back(storage_binding(storage));
    enc->draw(pk);
    enc->end_rendering();
}

// REN-39: the ranged twin -> draw_overlay_range (first_vertex > 0 selects it in the encoder; ==0 is draw_overlay,
// which the backend defines as draw_overlay_range at offset 0 — byte-identical).
inline void enc_draw_overlay_range(g::IRasterContext& r, g::IRasterTarget& target, g::IRasterProgram& prog,
                                   g::IStorageBuffer& storage, g::DepthCompare compare, crd::u32 first_vertex,
                                   crd::u32 vertex_count)
{
    auto                   enc = r.create_command_encoder();
    g::RenderingDesc       rd{};
    g::ColorAttachmentDesc c{};
    c.target = &target;
    c.load   = g::LoadOp::Load;
    c.blend  = g::BlendMode::Alpha;
    rd.color.push_back(c);
    rd.depth.enabled = (compare != g::DepthCompare::Always);
    rd.depth.compare = compare;
    enc->begin_rendering(rd);
    g::RasterDrawPacket pk{};
    pk.program                        = &prog;
    pk.geometry.kind                  = g::GeometryKind::StoragePull;
    pk.geometry.vertex_or_index_count = vertex_count;
    pk.geometry.first_vertex          = first_vertex;
    pk.bindings.push_back(storage_binding(storage));
    enc->draw(pk);
    enc->end_rendering();
}

// B5 deferred G-buffer MRT draw -> draw_gbuffer. The IGBufferTarget bundles N host-readable RGBA8 attachments; it is
// NOT an IRasterTarget, so it rides RenderingDesc::gbuffer. A single clear-carrier colour entry (null target) supplies
// the clear applied to every attachment. Plain-vertex geometry (None) — no storage pull.
inline void enc_draw_gbuffer(g::IRasterContext& r, g::IGBufferTarget& target, g::IRasterProgram& prog,
                             g::ClearColor clear, crd::u32 vertex_count)
{
    auto                   enc = r.create_command_encoder();
    g::RenderingDesc       rd{};
    rd.gbuffer = &target;
    g::ColorAttachmentDesc c{}; // clear-carrier: null target, load=Clear (default), carries the all-attachment clear
    c.clear = clear;
    rd.color.push_back(c);
    enc->begin_rendering(rd);
    g::RasterDrawPacket pk{};
    pk.program                        = &prog;
    pk.geometry.kind                  = g::GeometryKind::None;
    pk.geometry.vertex_or_index_count = vertex_count;
    enc->draw(pk);
    enc->end_rendering();
}

// plain storage-pull colour draw (no depth) -> draw_storage
inline void enc_draw_storage(g::IRasterContext& r, g::IRasterTarget& target, g::IRasterProgram& prog, g::ClearColor clear,
                             g::IStorageBuffer& storage, crd::u32 vertex_count)
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
    pk.geometry.kind                  = g::GeometryKind::StoragePull;
    pk.geometry.vertex_or_index_count = vertex_count;
    pk.bindings.push_back(storage_binding(storage));
    enc->draw(pk);
    enc->end_rendering();
}

// storage-pull colour+DEPTH scene draw (clear) -> draw_storage_depth; (load, no clear) -> draw_storage_depth_load
inline void enc_draw_storage_depth(g::IRasterContext& r, g::IRasterTarget& target, g::IRasterProgram& prog,
                                   g::ClearColor clear, float clear_depth, g::DepthCompare compare,
                                   g::IStorageBuffer& storage, crd::u32 vertex_count)
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
    pk.geometry.kind                  = g::GeometryKind::StoragePull;
    pk.geometry.vertex_or_index_count = vertex_count;
    pk.bindings.push_back(storage_binding(storage));
    enc->draw(pk);
    enc->end_rendering();
}

// the CONTINUING scene draw (colour+depth LOAD, no clear) -> draw_storage_depth_load
inline void enc_draw_storage_depth_load(g::IRasterContext& r, g::IRasterTarget& target, g::IRasterProgram& prog,
                                        g::DepthCompare compare, g::IStorageBuffer& storage, crd::u32 vertex_count)
{
    auto                   enc = r.create_command_encoder();
    g::RenderingDesc       rd{};
    g::ColorAttachmentDesc c{};
    c.target = &target;
    c.load   = g::LoadOp::Load; // LOAD colour — no clear, so the encoder picks the _load verb
    rd.color.push_back(c);
    rd.depth.enabled = true;
    rd.depth.target  = &target;
    rd.depth.load    = g::LoadOp::Load;
    rd.depth.compare = compare;
    enc->begin_rendering(rd);
    g::RasterDrawPacket pk{};
    pk.program                        = &prog;
    pk.geometry.kind                  = g::GeometryKind::StoragePull;
    pk.geometry.vertex_or_index_count = vertex_count;
    pk.bindings.push_back(storage_binding(storage));
    enc->draw(pk);
    enc->end_rendering();
}

// storage-pull DEPTH-ONLY draw (no colour attachment) -> draw_storage_depth_only (clear) / _load (no clear)
inline void enc_draw_storage_depth_only(g::IRasterContext& r, g::IRasterTarget& depth_target, g::IRasterProgram& prog,
                                        float clear_depth, g::DepthCompare compare, g::IStorageBuffer& storage,
                                        crd::u32 vertex_count)
{
    auto             enc = r.create_command_encoder();
    g::RenderingDesc rd{}; // NO colour attachment -> the depth-only arm
    rd.depth.enabled     = true;
    rd.depth.target      = &depth_target;
    rd.depth.load        = g::LoadOp::Clear;
    rd.depth.clear_depth = clear_depth;
    rd.depth.compare     = compare;
    enc->begin_rendering(rd);
    g::RasterDrawPacket pk{};
    pk.program                        = &prog;
    pk.geometry.kind                  = g::GeometryKind::StoragePull;
    pk.geometry.vertex_or_index_count = vertex_count;
    pk.bindings.push_back(storage_binding(storage));
    enc->draw(pk);
    enc->end_rendering();
}

// the CONTINUING depth-only draw (no clear) -> draw_storage_depth_only_load
inline void enc_draw_storage_depth_only_load(g::IRasterContext& r, g::IRasterTarget& depth_target,
                                             g::IRasterProgram& prog, g::DepthCompare compare, g::IStorageBuffer& storage,
                                             crd::u32 vertex_count)
{
    auto             enc = r.create_command_encoder();
    g::RenderingDesc rd{};
    rd.depth.enabled = true;
    rd.depth.target  = &depth_target;
    rd.depth.load    = g::LoadOp::Load;
    rd.depth.compare = compare;
    enc->begin_rendering(rd);
    g::RasterDrawPacket pk{};
    pk.program                        = &prog;
    pk.geometry.kind                  = g::GeometryKind::StoragePull;
    pk.geometry.vertex_or_index_count = vertex_count;
    pk.bindings.push_back(storage_binding(storage));
    enc->draw(pk);
    enc->end_rendering();
}

// storage-pull MRT scene draw (N colour attachments + depth) -> draw_storage_mrt
inline void enc_draw_storage_mrt(g::IRasterContext& r, g::IRasterTarget* const* targets, crd::u32 count,
                                 g::IRasterProgram& prog, g::ClearColor clear, float clear_depth, g::DepthCompare compare,
                                 g::IStorageBuffer& storage, crd::u32 vertex_count, const g::BlendMode* blend = nullptr)
{
    auto             enc = r.create_command_encoder();
    g::RenderingDesc rd{};
    for (crd::u32 i = 0; i < count; ++i)
    {
        g::ColorAttachmentDesc c{};
        c.target = targets[i];
        c.load   = g::LoadOp::Clear;
        c.clear  = clear;
        c.blend  = (blend != nullptr) ? blend[i] : g::BlendMode::Opaque;
        rd.color.push_back(c);
    }
    rd.depth.enabled     = true;
    rd.depth.target      = (count > 0) ? targets[0] : nullptr;
    rd.depth.load        = g::LoadOp::Clear;
    rd.depth.clear_depth = clear_depth;
    rd.depth.compare     = compare;
    enc->begin_rendering(rd);
    g::RasterDrawPacket pk{};
    pk.program                        = &prog;
    pk.geometry.kind                  = g::GeometryKind::StoragePull;
    pk.geometry.vertex_or_index_count = vertex_count;
    pk.bindings.push_back(storage_binding(storage));
    enc->draw(pk);
    enc->end_rendering();
}

// storage-pull TEXTURED scene draw (a base-colour map at slot 1) -> draw_storage_textured_depth
inline void enc_draw_storage_textured_depth(g::IRasterContext& r, g::IRasterTarget& target, g::IRasterProgram& prog,
                                            g::ClearColor clear, float clear_depth, g::DepthCompare compare,
                                            g::IStorageBuffer& storage, g::ITexture& texture, crd::u32 vertex_count)
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
    pk.geometry.kind                  = g::GeometryKind::StoragePull;
    pk.geometry.vertex_or_index_count = vertex_count;
    pk.bindings.push_back(storage_binding(storage));
    g::ResourceBinding mb{};
    mb.kind    = g::BindingKind::SampledTexture;
    mb.texture = &texture;
    mb.slot    = kMapSlot; // slot 1 — the base-colour MAP the encoder keys the textured verb off
    pk.bindings.push_back(mb);
    enc->draw(pk);
    enc->end_rendering();
}

// INDEXED storage-pull scene draw -> draw_storage_indexed_depth (load_target ⇒ colour+depth LOAD, else clear)
inline void enc_draw_storage_indexed_depth(g::IRasterContext& r, g::IRasterTarget& target, g::IRasterProgram& prog,
                                           g::ClearColor clear, float clear_depth, g::DepthCompare compare,
                                           g::IStorageBuffer& storage, crd::u32 index_offset_bytes, crd::u32 index_count,
                                           crd::u32 instance_count, bool load_target)
{
    auto                   enc = r.create_command_encoder();
    const g::LoadOp        lo  = load_target ? g::LoadOp::Load : g::LoadOp::Clear;
    g::RenderingDesc       rd{};
    g::ColorAttachmentDesc c{};
    c.target = &target;
    c.load   = lo;
    c.clear  = clear;
    rd.color.push_back(c);
    rd.depth.enabled     = true;
    rd.depth.target      = &target;
    rd.depth.load        = lo;
    rd.depth.clear_depth = clear_depth;
    rd.depth.compare     = compare;
    enc->begin_rendering(rd);
    g::RasterDrawPacket pk{};
    pk.program                        = &prog;
    pk.geometry.kind                  = g::GeometryKind::Indexed;
    pk.geometry.vertex_or_index_count = index_count;
    pk.geometry.instance_count        = instance_count;
    pk.geometry.index_offset          = index_offset_bytes;
    pk.bindings.push_back(storage_binding(storage));
    enc->draw(pk);
    enc->end_rendering();
}

// ── F7 CPU multi-draw + GPU-driven indirect: N draws in ONE device command. MultiStoragePull / MultiIndexed geometry
// (CPU count arrays) or Indirect / IndirectCount (device args + count buffer) selects the draw_storage_multi_* verb. ──

// N vertex counts, one buffer -> draw_storage_multi_depth (load_target ⇒ colour+depth LOAD, else clear)
inline void enc_draw_storage_multi_depth(g::IRasterContext& r, g::IRasterTarget& target, g::IRasterProgram& prog,
                                         g::ClearColor clear, float clear_depth, g::DepthCompare compare,
                                         g::IStorageBuffer& storage, const crd::u32* vertex_counts, crd::u32 count,
                                         crd::u32 first_draw_index, bool load_target)
{
    auto                   enc = r.create_command_encoder();
    const g::LoadOp        lo  = load_target ? g::LoadOp::Load : g::LoadOp::Clear;
    g::RenderingDesc       rd{};
    g::ColorAttachmentDesc c{};
    c.target = &target;
    c.load   = lo;
    c.clear  = clear;
    rd.color.push_back(c);
    rd.depth.enabled     = true;
    rd.depth.target      = &target;
    rd.depth.load        = lo;
    rd.depth.clear_depth = clear_depth;
    rd.depth.compare     = compare;
    enc->begin_rendering(rd);
    g::RasterDrawPacket pk{};
    pk.program                   = &prog;
    pk.geometry.kind             = g::GeometryKind::MultiStoragePull;
    pk.geometry.multi_counts     = vertex_counts;
    pk.geometry.draw_count       = count;
    pk.geometry.first_draw_index = first_draw_index;
    pk.bindings.push_back(storage_binding(storage));
    enc->draw(pk);
    enc->end_rendering();
}

// N IndexedDraw records, one buffer -> draw_storage_multi_indexed_depth
inline void enc_draw_storage_multi_indexed_depth(g::IRasterContext& r, g::IRasterTarget& target, g::IRasterProgram& prog,
                                                 g::ClearColor clear, float clear_depth, g::DepthCompare compare,
                                                 g::IStorageBuffer& storage, crd::u32 index_offset_bytes,
                                                 const g::IRasterContext::IndexedDraw* draws, crd::u32 count,
                                                 crd::u32 first_draw_index, bool load_target)
{
    auto                   enc = r.create_command_encoder();
    const g::LoadOp        lo  = load_target ? g::LoadOp::Load : g::LoadOp::Clear;
    g::RenderingDesc       rd{};
    g::ColorAttachmentDesc c{};
    c.target = &target;
    c.load   = lo;
    c.clear  = clear;
    rd.color.push_back(c);
    rd.depth.enabled     = true;
    rd.depth.target      = &target;
    rd.depth.load        = lo;
    rd.depth.clear_depth = clear_depth;
    rd.depth.compare     = compare;
    enc->begin_rendering(rd);
    g::RasterDrawPacket pk{};
    pk.program                   = &prog;
    pk.geometry.kind             = g::GeometryKind::MultiIndexed;
    pk.geometry.multi_indexed    = draws;
    pk.geometry.draw_count       = count;
    pk.geometry.index_offset     = index_offset_bytes;
    pk.geometry.first_draw_index = first_draw_index;
    pk.bindings.push_back(storage_binding(storage));
    enc->draw(pk);
    enc->end_rendering();
}

// GPU-driven indexed indirect (colour + depth; optional map/atlas) -> draw_storage_multi_indexed_indirect
inline void enc_draw_storage_multi_indexed_indirect(g::IRasterContext& r, g::IRasterTarget& target,
                                                    g::IRasterProgram& prog, g::ClearColor clear, float clear_depth,
                                                    g::DepthCompare compare, g::IStorageBuffer& storage,
                                                    crd::u32 index_offset_bytes, g::ITexture* map, g::ITexture* atlas,
                                                    g::IStorageBuffer& args, crd::u32 args_offset_bytes,
                                                    g::IStorageBuffer* count_buf, crd::u32 count_offset_bytes,
                                                    crd::u32 max_draws, bool load_target, crd::u32 first_draw_index = 0U)
{
    auto                   enc = r.create_command_encoder();
    const g::LoadOp        lo  = load_target ? g::LoadOp::Load : g::LoadOp::Clear;
    g::RenderingDesc       rd{};
    g::ColorAttachmentDesc c{};
    c.target = &target;
    c.load   = lo;
    c.clear  = clear;
    rd.color.push_back(c);
    rd.depth.enabled     = true;
    rd.depth.target      = &target;
    rd.depth.load        = lo;
    rd.depth.clear_depth = clear_depth;
    rd.depth.compare     = compare;
    enc->begin_rendering(rd);
    g::RasterDrawPacket pk{};
    pk.program                   = &prog;
    pk.geometry.kind             = (count_buf != nullptr) ? g::GeometryKind::IndirectCount : g::GeometryKind::Indirect;
    pk.geometry.index_offset     = index_offset_bytes;
    pk.geometry.args_buffer      = &args;
    pk.geometry.args_offset      = args_offset_bytes;
    pk.geometry.count_buffer     = count_buf;
    pk.geometry.count_offset     = count_offset_bytes;
    pk.geometry.max_draws        = max_draws;
    pk.geometry.first_draw_index = first_draw_index;
    pk.bindings.push_back(storage_binding(storage));
    if (map != nullptr)
    {
        g::ResourceBinding mb{};
        mb.kind    = g::BindingKind::SampledTexture;
        mb.texture = map;
        mb.slot    = kMapSlot;
        pk.bindings.push_back(mb);
    }
    if (atlas != nullptr)
    {
        g::ResourceBinding ab{};
        ab.kind    = g::BindingKind::SampledTexture;
        ab.texture = atlas;
        ab.slot    = kAtlasSlot;
        pk.bindings.push_back(ab);
    }
    enc->draw(pk);
    enc->end_rendering();
}

// GPU-driven indexed indirect, DEPTH-ONLY (no colour attachment) -> draw_storage_multi_indexed_depth_only_indirect
inline void enc_draw_storage_multi_indexed_depth_only_indirect(
    g::IRasterContext& r, g::IRasterTarget& depth_target, g::IRasterProgram& prog, float clear_depth,
    g::DepthCompare compare, g::IStorageBuffer& storage, crd::u32 index_offset_bytes, g::IStorageBuffer& args,
    crd::u32 args_offset_bytes, g::IStorageBuffer* count_buf, crd::u32 count_offset_bytes, crd::u32 max_draws,
    bool load_target, crd::u32 first_draw_index = 0U)
{
    auto             enc = r.create_command_encoder();
    const g::LoadOp  lo  = load_target ? g::LoadOp::Load : g::LoadOp::Clear;
    g::RenderingDesc rd{}; // NO colour attachment -> the depth-only-indirect arm
    rd.depth.enabled     = true;
    rd.depth.target      = &depth_target;
    rd.depth.load        = lo;
    rd.depth.clear_depth = clear_depth;
    rd.depth.compare     = compare;
    enc->begin_rendering(rd);
    g::RasterDrawPacket pk{};
    pk.program                   = &prog;
    pk.geometry.kind             = (count_buf != nullptr) ? g::GeometryKind::IndirectCount : g::GeometryKind::Indirect;
    pk.geometry.index_offset     = index_offset_bytes;
    pk.geometry.args_buffer      = &args;
    pk.geometry.args_offset      = args_offset_bytes;
    pk.geometry.count_buffer     = count_buf;
    pk.geometry.count_offset     = count_offset_bytes;
    pk.geometry.max_draws        = max_draws;
    pk.geometry.first_draw_index = first_draw_index;
    pk.bindings.push_back(storage_binding(storage));
    enc->draw(pk);
    enc->end_rendering();
}

// ── F5 amplification: mesh-shader / tessellation draws. Meshlet / MeshletIndirect / Patches geometry selects the verb. ──

// helper: a one-colour rendering scope cleared to `clear`, optional depth attachment.
inline void enc_amp_scope(g::RenderingDesc& rd, g::IRasterTarget& target, g::ClearColor clear)
{
    g::ColorAttachmentDesc c{};
    c.target = &target;
    c.load   = g::LoadOp::Clear;
    c.clear  = clear;
    rd.color.push_back(c);
}

// procedural mesh dispatch -> draw_mesh
inline void enc_draw_mesh(g::IRasterContext& r, g::IRasterTarget& target, g::IRasterProgram& prog, g::ClearColor clear,
                          crd::u32 group_count)
{
    auto             enc = r.create_command_encoder();
    g::RenderingDesc rd{};
    enc_amp_scope(rd, target, clear);
    enc->begin_rendering(rd);
    g::RasterDrawPacket pk{};
    pk.program                = &prog;
    pk.geometry.kind          = g::GeometryKind::Meshlet;
    pk.geometry.group_count_x = group_count;
    enc->draw(pk);
    enc->end_rendering();
}

// storage-pull mesh dispatch -> draw_mesh_storage
inline void enc_draw_mesh_storage(g::IRasterContext& r, g::IRasterTarget& target, g::IRasterProgram& prog,
                                  g::ClearColor clear, g::IStorageBuffer& storage, crd::u32 group_count)
{
    auto             enc = r.create_command_encoder();
    g::RenderingDesc rd{};
    enc_amp_scope(rd, target, clear);
    enc->begin_rendering(rd);
    g::RasterDrawPacket pk{};
    pk.program                = &prog;
    pk.geometry.kind          = g::GeometryKind::Meshlet;
    pk.geometry.group_count_x = group_count;
    pk.bindings.push_back(storage_binding(storage));
    enc->draw(pk);
    enc->end_rendering();
}

// GPU-driven indirect mesh dispatch (native args) -> draw_mesh_indirect
inline void enc_draw_mesh_indirect(g::IRasterContext& r, g::IRasterTarget& target, g::IRasterProgram& prog,
                                   g::ClearColor clear, void* native_args, crd::u64 args_offset)
{
    auto             enc = r.create_command_encoder();
    g::RenderingDesc rd{};
    enc_amp_scope(rd, target, clear);
    enc->begin_rendering(rd);
    g::RasterDrawPacket pk{};
    pk.program              = &prog;
    pk.geometry.kind        = g::GeometryKind::MeshletIndirect;
    pk.geometry.native_args = native_args;
    pk.geometry.args_offset = args_offset;
    enc->draw(pk);
    enc->end_rendering();
}

// mesh dispatch + bindless cascades + depth -> draw_mesh_bindless_depth (the ocean's mesh-shader displaced grid)
inline void enc_draw_mesh_bindless_depth(g::IRasterContext& r, g::IRasterTarget& target, g::IRasterProgram& prog,
                                         g::ClearColor clear, float clear_depth, g::DepthCompare compare,
                                         g::ITexture* const* textures, crd::u32 count, crd::u32 group_count)
{
    auto             enc = r.create_command_encoder();
    g::RenderingDesc rd{};
    enc_amp_scope(rd, target, clear);
    rd.depth.enabled     = true;
    rd.depth.target      = &target;
    rd.depth.load        = g::LoadOp::Clear;
    rd.depth.clear_depth = clear_depth;
    rd.depth.compare     = compare;
    enc->begin_rendering(rd);
    g::RasterDrawPacket pk{};
    pk.program                = &prog;
    pk.geometry.kind          = g::GeometryKind::Meshlet;
    pk.geometry.group_count_x = group_count;
    g::ResourceBinding b{};
    b.kind          = g::BindingKind::BindlessTextureArray;
    b.texture_array = textures;
    b.array_count   = count;
    pk.bindings.push_back(b);
    enc->draw(pk);
    enc->end_rendering();
}

// mesh dispatch with per-primitive VRS -> draw_mesh_vrs (the shading_rate_attachment signals the VRS mesh draw)
inline void enc_draw_mesh_vrs(g::IRasterContext& r, g::IRasterTarget& target, g::IRasterProgram& prog, g::ClearColor clear,
                              crd::u32 group_count)
{
    auto             enc = r.create_command_encoder();
    g::RenderingDesc rd{};
    enc_amp_scope(rd, target, clear);
    rd.shading_rate_attachment = &target; // signal: VRS mesh draw (the rate rides the mesh shader's per-primitive output)
    enc->begin_rendering(rd);
    g::RasterDrawPacket pk{};
    pk.program                = &prog;
    pk.geometry.kind          = g::GeometryKind::Meshlet;
    pk.geometry.group_count_x = group_count;
    enc->draw(pk);
    enc->end_rendering();
}

// procedural tessellation patch grid -> draw_tess
inline void enc_draw_tess(g::IRasterContext& r, g::IRasterTarget& target, g::IRasterProgram& prog, g::ClearColor clear,
                          crd::u32 patch_count)
{
    auto             enc = r.create_command_encoder();
    g::RenderingDesc rd{};
    enc_amp_scope(rd, target, clear);
    enc->begin_rendering(rd);
    g::RasterDrawPacket pk{};
    pk.program             = &prog;
    pk.geometry.kind       = g::GeometryKind::Patches;
    pk.geometry.patch_count = patch_count;
    enc->draw(pk);
    enc->end_rendering();
}

// storage-pull tessellation -> draw_tess_storage
inline void enc_draw_tess_storage(g::IRasterContext& r, g::IRasterTarget& target, g::IRasterProgram& prog,
                                  g::ClearColor clear, g::IStorageBuffer& storage, crd::u32 patch_count)
{
    auto             enc = r.create_command_encoder();
    g::RenderingDesc rd{};
    enc_amp_scope(rd, target, clear);
    enc->begin_rendering(rd);
    g::RasterDrawPacket pk{};
    pk.program              = &prog;
    pk.geometry.kind        = g::GeometryKind::Patches;
    pk.geometry.patch_count = patch_count;
    pk.bindings.push_back(storage_binding(storage));
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
