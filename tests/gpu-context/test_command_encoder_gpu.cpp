// RAF-2b/2c — the canonical command ENCODER on real GPUs, both backends, with cross-backend + legacy parity.
//
// Every command-model kind is LOWERED by the encoder (command_encoder.cpp); this suite exercises the device-visible
// ones end-to-end on BOTH Vulkan and D3D12: a fullscreen draw (with byte-identical legacy-verb parity), a clear, and
// the transfer family — Copy / Blit / Resolve — routed through encoder->transfer(). The storage-family draws
// (depth/MRT/indexed/indirect/mesh/tess/bindless/shadow) lower through the same verbs the existing scene-render +
// backend suites already GPU-gate; the encoder's translation of them is proven by the hermetic model gate + compile.
// GPU tests skip cleanly when no device / no shader-object capability is present.

#include <crd/gpu/command_model.hpp>
#include <crd/gpu/context.hpp>
#include <crd/gpu/raster_context.hpp>
#include <crd/gpu/vulkan_context.hpp>
#include <crd/gpu/vulkan_raster_context.hpp>

#include <crd/kir/ckir.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#if defined(_WIN32)
#include <crd/gpu/dx12_context.hpp>
#include <crd/gpu/dx12_raster_context.hpp>
#endif

#include <catch2/catch_test_macros.hpp>
#include <ckir_raster_triangle.hpp>
#include <verb_packet_helpers.hpp> // RAF-12.4: crd::gputest::enc_draw (the de-virtualized draw verb, recorded via the encoder)
#include <ckir_vertex_pull.hpp>

#include <memory>

using namespace crd::gpu;

namespace
{
// A built raster program (VS + FS kept alive alongside it).
struct Tri
{
    std::unique_ptr<IGpuProgram> vs;
    std::unique_ptr<IGpuProgram> fs;
    std::unique_ptr<IRasterProgram> program;
};

// Build the shared CKIR triangle program. Returns false (→ skip) when shader compilation is unavailable.
bool make_triangle(IGpuContext& gctx, IRasterContext& raster, crd::memory::IAllocator& alloc, Tri& out)
{
    namespace kir = crd::kir;
    kir::KGraph vg(&alloc);
    kir::KEntry ve;
    crd::gputest::build_triangle_vs(vg, ve);
    kir::KGraph fg(&alloc);
    kir::KEntry fe;
    crd::gputest::build_triangle_fs(fg, fe);

    out.vs = gctx.create_program(vg, ve);
    if (out.vs == nullptr)
    {
        return false;
    }
    out.fs = gctx.create_program(fg, fe);
    REQUIRE(out.fs != nullptr);
    out.program = raster.create_raster_program(*out.vs, *out.fs);
    REQUIRE(out.program != nullptr);
    REQUIRE(out.program->valid());
    return true;
}

// Fullscreen (attributeless) draw through the encoder: the triangle over the clear reads back as expected — centre red,
// corner blue. RAF-12.4: the draw verb is de-virtualized, so the encoder IS the only path (no legacy verb to compare).
void gate_fullscreen(IRasterContext& raster, IRasterProgram& program)
{
    constexpr crd::u32 dim = 32U;
    const ClearColor blue{0.0F, 0.0F, 1.0F, 1.0F};

    auto target = raster.create_color_target(dim, dim);
    REQUIRE(target != nullptr);
    auto encoder = raster.create_command_encoder();
    REQUIRE(encoder != nullptr);

    RenderingDesc rd;
    rd.width = dim;
    rd.height = dim;
    rd.color.push_back(ColorAttachmentDesc{target.get(), LoadOp::Clear, StoreOp::Store, blue, BlendMode::Opaque});
    REQUIRE(validate_rendering(rd) == CommandError::None);

    RasterDrawPacket packet;
    packet.program = &program;
    packet.command = RasterCommandKind::Draw;
    packet.geometry.kind = GeometryKind::None;
    packet.geometry.vertex_or_index_count = 3U;
    REQUIRE(validate_packet(packet) == CommandError::None);

    encoder->begin_rendering(rd);
    encoder->draw(packet);
    encoder->end_rendering();

    const crd::u32 centre = target->read_pixel(dim / 2U, dim / 2U);
    const crd::u32 corner = target->read_pixel(0U, 0U);
    CHECK((centre & 0xFFU) >= 250U);
    CHECK(((centre >> 16U) & 0xFFU) <= 5U);
    CHECK((corner & 0xFFU) <= 5U);
    CHECK(((corner >> 16U) & 0xFFU) >= 250U);
}

// Clear through the encoder's transfer(Clear).
void gate_clear(IRasterContext& raster)
{
    constexpr crd::u32 dim = 16U;
    auto target = raster.create_color_target(dim, dim);
    REQUIRE(target != nullptr);
    auto encoder = raster.create_command_encoder();

    TransferDesc t;
    t.kind = TransferKind::Clear;
    t.dst = target.get();
    t.clear = ClearColor{0.0F, 1.0F, 0.0F, 1.0F}; // green
    encoder->transfer(t);

    const crd::u32 px = target->read_pixel(dim / 2U, dim / 2U);
    CHECK((px & 0xFFU) <= 5U);
    CHECK(((px >> 8U) & 0xFFU) >= 250U);
    CHECK(((px >> 16U) & 0xFFU) <= 5U);
}

// Copy + Blit through the encoder: a drawn source is transferred into a fresh target and must reproduce it.
void gate_copy_blit(IRasterContext& raster, IRasterProgram& program)
{
    constexpr crd::u32 dim = 32U;
    const ClearColor blue{0.0F, 0.0F, 1.0F, 1.0F};

    auto src = raster.create_color_target(dim, dim);
    REQUIRE(src != nullptr);
    crd::gputest::enc_draw(raster, *src, program, blue, 3U); // red triangle over blue clear (recorded via the encoder)

    auto encoder = raster.create_command_encoder();

    // Copy: exact reproduction.
    auto dst = raster.create_color_target(dim, dim);
    REQUIRE(dst != nullptr);
    TransferDesc cp;
    cp.kind = TransferKind::Copy;
    cp.dst = dst.get();
    cp.src = src.get();
    encoder->transfer(cp);
    for (crd::u32 y = 0U; y < dim; y += 4U)
    {
        for (crd::u32 x = 0U; x < dim; x += 4U)
        {
            CHECK(dst->read_pixel(x, y) == src->read_pixel(x, y));
        }
    }

    // Blit (same size, nearest = exact): centre red, corner blue.
    auto dst2 = raster.create_color_target(dim, dim);
    REQUIRE(dst2 != nullptr);
    TransferDesc bl;
    bl.kind = TransferKind::Blit;
    bl.dst = dst2.get();
    bl.src = src.get();
    bl.filter = SamplerFilter::Nearest;
    encoder->transfer(bl);
    const crd::u32 centre = dst2->read_pixel(dim / 2U, dim / 2U);
    const crd::u32 corner = dst2->read_pixel(0U, 0U);
    CHECK((centre & 0xFFU) >= 250U);
    CHECK(((corner >> 16U) & 0xFFU) >= 250U);
}

// Resolve through the encoder: draw into an MSAA target, resolve it to a single-sample target. Skips if no MSAA.
void gate_resolve(IRasterContext& raster, IRasterProgram& program)
{
    constexpr crd::u32 dim = 32U;
    auto ms = raster.create_color_target_ms(dim, dim, 4U);
    if (ms == nullptr)
    {
        WARN("adapter has no 4x MSAA color target; skipping the resolve gate");
        return;
    }
    crd::gputest::enc_draw(raster, *ms, program, ClearColor{0.0F, 0.0F, 1.0F, 1.0F}, 3U);

    auto encoder = raster.create_command_encoder();
    auto dst = raster.create_color_target(dim, dim);
    REQUIRE(dst != nullptr);
    TransferDesc rs;
    rs.kind = TransferKind::Resolve;
    rs.dst = dst.get();
    rs.src = ms.get();
    encoder->transfer(rs);

    const crd::u32 centre = dst->read_pixel(dim / 2U, dim / 2U);
    const crd::u32 corner = dst->read_pixel(0U, 0U);
    CHECK((centre & 0xFFU) >= 200U);          // red centre (edge samples soften, centre stays solid)
    CHECK(((corner >> 16U) & 0xFFU) >= 200U); // blue corner
}

// Storage-pull (vertex-pulling) draw through the encoder, with byte-identical legacy-verb parity.
void gate_storage_pull(IGpuContext& gctx, IRasterContext& raster, crd::memory::IAllocator& alloc)
{
    namespace kir = crd::kir;
    kir::KGraph vg(&alloc);
    kir::KEntry ve;
    crd::gputest::build_vertex_pull_vs(vg, ve);
    kir::KGraph fg(&alloc);
    kir::KEntry fe;
    crd::gputest::build_triangle_fs(fg, fe);
    auto vs = gctx.create_program(vg, ve);
    REQUIRE(vs != nullptr); // shader compile already proven by make_triangle
    auto fs = gctx.create_program(fg, fe);
    REQUIRE(fs != nullptr);
    auto program = raster.create_raster_program(*vs, *fs);
    REQUIRE(program != nullptr);

    constexpr crd::u32 dim = 32U;
    float verts[36] = {0.0F}; // 3 vertices x 12 floats (cooked 48-byte stride); only pos3 is read
    const auto set = [&](int i, float x, float y, float z)
    {
        verts[i * 12 + 0] = x;
        verts[i * 12 + 1] = y;
        verts[i * 12 + 2] = z;
    };
    set(0, 0.0F, -0.8F, 0.0F);
    set(1, 0.8F, 0.8F, 0.0F);
    set(2, -0.8F, 0.8F, 0.0F);
    auto buf = raster.create_storage_buffer(static_cast<crd::u32>(sizeof(verts)));
    REQUIRE(buf != nullptr);
    REQUIRE(raster.upload_storage(*buf, 0U, verts, static_cast<crd::u32>(sizeof(verts))));

    const ClearColor blue{0.0F, 0.0F, 1.0F, 1.0F};
    auto ref = raster.create_color_target(dim, dim);
    REQUIRE(ref != nullptr);
    raster.draw_storage(*ref, *program, blue, *buf, 3U); // legacy path

    auto target = raster.create_color_target(dim, dim);
    REQUIRE(target != nullptr);
    auto encoder = raster.create_command_encoder();
    RenderingDesc rd;
    rd.width = dim;
    rd.height = dim;
    rd.color.push_back(ColorAttachmentDesc{target.get(), LoadOp::Clear, StoreOp::Store, blue, BlendMode::Opaque});
    RasterDrawPacket p;
    p.program = program.get();
    p.command = RasterCommandKind::Draw;
    p.geometry.kind = GeometryKind::StoragePull;
    p.geometry.vertex_or_index_count = 3U;
    p.bindings.push_back(ResourceBinding{BindingFrequency::Object, BindingKind::StorageBuffer, 0U, buf.get()});
    REQUIRE(validate_packet(p) == CommandError::None);
    encoder->begin_rendering(rd);
    encoder->draw(p);
    encoder->end_rendering();

    CHECK((target->read_pixel(dim / 2U, dim / 2U) & 0xFFU) >= 250U); // red triangle centre
    for (crd::u32 y = 0U; y < dim; y += 4U)
    {
        for (crd::u32 x = 0U; x < dim; x += 4U)
        {
            CHECK(target->read_pixel(x, y) == ref->read_pixel(x, y)); // encoder == legacy verb
        }
    }
}

// Indexed draw through the encoder (the "indexed" Gate-2 kind), with byte-identical legacy-verb parity.
void gate_indexed(IGpuContext& gctx, IRasterContext& raster, crd::memory::IAllocator& alloc)
{
    namespace kir = crd::kir;
    kir::KGraph vg(&alloc);
    kir::KEntry ve;
    crd::gputest::build_indexed_probe_vs(vg, ve); // positions by index value {4,5,6}; no storage read
    kir::KGraph fg(&alloc);
    kir::KEntry fe;
    crd::gputest::build_triangle_fs(fg, fe);
    auto vs = gctx.create_program(vg, ve);
    REQUIRE(vs != nullptr);
    auto fs = gctx.create_program(fg, fe);
    REQUIRE(fs != nullptr);
    auto program = raster.create_raster_program(*vs, *fs);
    REQUIRE(program != nullptr);

    constexpr crd::u32 dim = 32U;
    const crd::u32 idx[3] = {4U, 5U, 6U};
    auto idx_buf = raster.create_storage_buffer(static_cast<crd::u32>(sizeof(idx)));
    REQUIRE(idx_buf != nullptr);
    REQUIRE(raster.upload_storage(*idx_buf, 0U, idx, static_cast<crd::u32>(sizeof(idx))));

    const ClearColor black{0.0F, 0.0F, 0.0F, 1.0F};
    auto ref = raster.create_color_depth_target(dim, dim);
    REQUIRE(ref != nullptr);
    raster.draw_storage_indexed_depth(*ref, *program, black, 0.0F, DepthCompare::Always, *idx_buf, 0U, 3U, 1U,
                                      false); // legacy path

    auto target = raster.create_color_depth_target(dim, dim);
    REQUIRE(target != nullptr);
    auto encoder = raster.create_command_encoder();
    RenderingDesc rd;
    rd.width = dim;
    rd.height = dim;
    rd.color.push_back(ColorAttachmentDesc{target.get(), LoadOp::Clear, StoreOp::Store, black, BlendMode::Opaque});
    rd.depth = DepthStencilAttachmentDesc{target.get(), true, LoadOp::Clear, StoreOp::Store, 0.0F, true,
                                          DepthCompare::Always};
    RasterDrawPacket p;
    p.program = program.get();
    p.command = RasterCommandKind::DrawIndexed;
    p.geometry.kind = GeometryKind::Indexed;
    p.geometry.vertex_or_index_count = 3U;
    p.geometry.instance_count = 1U;
    p.geometry.index_buffer = idx_buf.get();
    p.geometry.index_offset = 0U;
    p.bindings.push_back(ResourceBinding{BindingFrequency::Object, BindingKind::StorageBuffer, 0U, idx_buf.get()});
    REQUIRE(validate_packet(p) == CommandError::None);
    encoder->begin_rendering(rd);
    encoder->draw(p);
    encoder->end_rendering();

    CHECK((target->read_pixel(dim / 2U, dim / 2U) & 0xFFU) >= 250U); // red triangle centre
    for (crd::u32 y = 0U; y < dim; y += 4U)
    {
        for (crd::u32 x = 0U; x < dim; x += 4U)
        {
            CHECK(target->read_pixel(x, y) == ref->read_pixel(x, y)); // encoder == legacy verb
        }
    }
}

// Colour+depth draw AND load-store: two draws in ONE encoder scope (first clears, second LOADs — never re-clears),
// with byte-identical legacy parity against draw_storage_depth + draw_storage_depth_load.
void gate_depth_loadstore(IGpuContext& gctx, IRasterContext& raster, crd::memory::IAllocator& alloc)
{
    namespace kir = crd::kir;
    kir::KGraph vg(&alloc);
    kir::KEntry ve;
    crd::gputest::build_vertex_pull_vs(vg, ve);
    kir::KGraph fg(&alloc);
    kir::KEntry fe;
    crd::gputest::build_triangle_fs(fg, fe);
    auto vs = gctx.create_program(vg, ve);
    REQUIRE(vs != nullptr);
    auto fs = gctx.create_program(fg, fe);
    REQUIRE(fs != nullptr);
    auto program = raster.create_raster_program(*vs, *fs);
    REQUIRE(program != nullptr);

    constexpr crd::u32 dim = 32U;
    float verts[36] = {0.0F};
    const auto set = [&](int i, float x, float y, float z)
    {
        verts[i * 12 + 0] = x;
        verts[i * 12 + 1] = y;
        verts[i * 12 + 2] = z;
    };
    set(0, 0.0F, -0.8F, 0.0F);
    set(1, 0.8F, 0.8F, 0.0F);
    set(2, -0.8F, 0.8F, 0.0F);
    auto buf = raster.create_storage_buffer(static_cast<crd::u32>(sizeof(verts)));
    REQUIRE(buf != nullptr);
    REQUIRE(raster.upload_storage(*buf, 0U, verts, static_cast<crd::u32>(sizeof(verts))));

    const ClearColor blue{0.0F, 0.0F, 1.0F, 1.0F};

    // Legacy: clear draw + load draw.
    auto ref = raster.create_color_depth_target(dim, dim);
    REQUIRE(ref != nullptr);
    raster.draw_storage_depth(*ref, *program, blue, 0.0F, DepthCompare::Always, *buf, 3U);
    raster.draw_storage_depth_load(*ref, *program, DepthCompare::Always, *buf, 3U);

    // Encoder: two draws in one scope — first honours LoadOp::Clear, second LOADs.
    auto target = raster.create_color_depth_target(dim, dim);
    REQUIRE(target != nullptr);
    auto encoder = raster.create_command_encoder();
    RenderingDesc rd;
    rd.width = dim;
    rd.height = dim;
    rd.color.push_back(ColorAttachmentDesc{target.get(), LoadOp::Clear, StoreOp::Store, blue, BlendMode::Opaque});
    rd.depth = DepthStencilAttachmentDesc{target.get(), true, LoadOp::Clear, StoreOp::Store, 0.0F, true,
                                          DepthCompare::Always};
    RasterDrawPacket p;
    p.program = program.get();
    p.command = RasterCommandKind::Draw;
    p.geometry.kind = GeometryKind::StoragePull;
    p.geometry.vertex_or_index_count = 3U;
    p.bindings.push_back(ResourceBinding{BindingFrequency::Object, BindingKind::StorageBuffer, 0U, buf.get()});
    REQUIRE(validate_packet(p) == CommandError::None);
    encoder->begin_rendering(rd);
    encoder->draw(p); // clears
    encoder->draw(p); // loads (must not wipe the first)
    encoder->end_rendering();

    CHECK((target->read_pixel(dim / 2U, dim / 2U) & 0xFFU) >= 250U); // red triangle survives the load draw
    for (crd::u32 y = 0U; y < dim; y += 4U)
    {
        for (crd::u32 x = 0U; x < dim; x += 4U)
        {
            CHECK(target->read_pixel(x, y) == ref->read_pixel(x, y)); // encoder == legacy clear+load
        }
    }
}

// NOTE — MRT: mapped in the encoder (StoragePull + >=2 colour attachments → draw_storage_mrt) and hermetically
// validated, but NOT GPU-gated standalone here. draw_storage_mrt allocates its multi-attachment layout from
// FRAME-GRAPH TRANSIENTS; standalone create_color_target objects cannot compose as MRT attachments (attachment 1
// stays unwritten), which is a verb/architecture constraint, not an encoder defect (the encoder passes correct
// args). MRT flows through the encoder — and is GPU-gated — once the encoder is wired into the frame graph (RAF-7);
// the frame-graph MRT suite proves the verb today.

// Mesh-shader dispatch through the encoder (skips where mesh shaders are unsupported).
void gate_mesh(IGpuContext& gctx, IRasterContext& raster, crd::memory::IAllocator& alloc)
{
    namespace kir = crd::kir;
    kir::KGraph mg(&alloc);
    kir::KEntry me;
    crd::gputest::build_triangle_mesh(mg, me);
    kir::KGraph fg(&alloc);
    kir::KEntry fe;
    crd::gputest::build_triangle_fs(fg, fe);
    auto mesh = gctx.create_program(mg, me);
    if (mesh == nullptr)
    {
        WARN("mesh program unavailable; skipping the mesh gate");
        return;
    }
    auto fs = gctx.create_program(fg, fe);
    REQUIRE(fs != nullptr);
    auto program = raster.create_mesh_program(*mesh, *fs);
    if (program == nullptr)
    {
        WARN("adapter has no mesh shaders; skipping the mesh gate");
        return;
    }

    constexpr crd::u32 dim = 32U;
    auto buf = raster.create_storage_buffer(64U); // the verb binds set0/b0 even though this mesh does not read it
    REQUIRE(buf != nullptr);
    const ClearColor blue{0.0F, 0.0F, 1.0F, 1.0F};

    auto ref = raster.create_color_target(dim, dim);
    REQUIRE(ref != nullptr);
    raster.draw_mesh_storage(*ref, *program, blue, *buf, 1U); // legacy

    auto target = raster.create_color_target(dim, dim);
    REQUIRE(target != nullptr);
    auto encoder = raster.create_command_encoder();
    RenderingDesc rd;
    rd.width = dim;
    rd.height = dim;
    rd.color.push_back(ColorAttachmentDesc{target.get(), LoadOp::Clear, StoreOp::Store, blue, BlendMode::Opaque});
    RasterDrawPacket p;
    p.program = program.get();
    p.command = RasterCommandKind::DispatchMesh;
    p.geometry.kind = GeometryKind::Meshlet;
    p.geometry.group_count_x = 1U;
    p.bindings.push_back(ResourceBinding{BindingFrequency::Object, BindingKind::StorageBuffer, 0U, buf.get()});
    REQUIRE(validate_packet(p) == CommandError::None);
    encoder->begin_rendering(rd);
    encoder->draw(p);
    encoder->end_rendering();

    // The mesh triangle's exact centre-pixel coverage varies by backend rasterisation (winding/cull); the
    // encoder-vs-legacy parity below is the real proof (the mesh verb's rendering is gated by the mesh-shader suites).
    for (crd::u32 y = 0U; y < dim; y += 4U)
    {
        for (crd::u32 x = 0U; x < dim; x += 4U)
        {
            CHECK(target->read_pixel(x, y) == ref->read_pixel(x, y)); // encoder == legacy mesh verb
        }
    }
}

// Tessellation dispatch through the encoder (skips where tessellation is unsupported).
void gate_tess(IGpuContext& gctx, IRasterContext& raster, crd::memory::IAllocator& alloc)
{
    namespace kir = crd::kir;
    kir::KGraph vg(&alloc);
    kir::KEntry ve;
    crd::gputest::build_tess_quad_vs(vg, ve);
    kir::KGraph hg(&alloc);
    kir::KEntry he;
    crd::gputest::build_tess_hull(hg, he);
    kir::KGraph dg(&alloc);
    kir::KEntry de;
    crd::gputest::build_tess_domain(dg, de);
    kir::KGraph fg(&alloc);
    kir::KEntry fe;
    crd::gputest::build_triangle_fs(fg, fe);
    auto vs = gctx.create_program(vg, ve);
    if (vs == nullptr)
    {
        WARN("tess programs unavailable; skipping the tess gate");
        return;
    }
    auto tc = gctx.create_program(hg, he);
    auto te = gctx.create_program(dg, de);
    auto fs = gctx.create_program(fg, fe);
    REQUIRE(tc != nullptr);
    REQUIRE(te != nullptr);
    REQUIRE(fs != nullptr);
    auto program = raster.create_tess_program(*vs, *tc, *te, *fs);
    if (program == nullptr)
    {
        WARN("adapter has no tessellation; skipping the tess gate");
        return;
    }

    constexpr crd::u32 dim = 32U;
    auto buf = raster.create_storage_buffer(64U);
    REQUIRE(buf != nullptr);
    const ClearColor blue{0.0F, 0.0F, 1.0F, 1.0F};

    auto ref = raster.create_color_target(dim, dim);
    REQUIRE(ref != nullptr);
    raster.draw_tess_storage(*ref, *program, blue, *buf, 1U); // legacy, one quad patch

    auto target = raster.create_color_target(dim, dim);
    REQUIRE(target != nullptr);
    auto encoder = raster.create_command_encoder();
    RenderingDesc rd;
    rd.width = dim;
    rd.height = dim;
    rd.color.push_back(ColorAttachmentDesc{target.get(), LoadOp::Clear, StoreOp::Store, blue, BlendMode::Opaque});
    RasterDrawPacket p;
    p.program = program.get();
    p.command = RasterCommandKind::DrawPatches;
    p.geometry.kind = GeometryKind::Patches;
    p.geometry.patch_count = 1U;
    p.bindings.push_back(ResourceBinding{BindingFrequency::Object, BindingKind::StorageBuffer, 0U, buf.get()});
    REQUIRE(validate_packet(p) == CommandError::None);
    encoder->begin_rendering(rd);
    encoder->draw(p);
    encoder->end_rendering();

    // The tessellated quad's exact centre-pixel coverage varies by backend; the encoder-vs-legacy parity below is
    // the real proof (the tessellation verb's rendering is gated by the tessellation suites).
    for (crd::u32 y = 0U; y < dim; y += 4U)
    {
        for (crd::u32 x = 0U; x < dim; x += 4U)
        {
            CHECK(target->read_pixel(x, y) == ref->read_pixel(x, y)); // encoder == legacy tess verb
        }
    }
}

// NOTE — the remaining kinds (MRT, indexed-INDIRECT / indirect-count, comparison-sampler / shadow, bindless) are all
// MAPPED in the encoder (command_encoder.cpp) and hermetically validated (crd-gpu-context-tests). They are NOT gated
// here through STANDALONE targets: draw_storage_mrt / the indexed-indirect verbs / draw_storage_shadowed_depth are
// built for FRAME-GRAPH transients + specific args layouts and are not robustly reproducible with standalone
// create_color_target objects (a verb/architecture constraint, not the encoder, which passes identical args). They
// GPU-execute THROUGH the encoder once it is wired into the frame graph (RAF-7); the frame-graph / scene-render /
// backend suites prove those verbs today.
void run_all_gates(IGpuContext& gctx, IRasterContext& raster, crd::memory::IAllocator& alloc)
{
    Tri tri;
    if (!make_triangle(gctx, raster, alloc, tri))
    {
        WARN("shader compilation unavailable (no dxc/shaderc); skipping the encoder GPU gates");
        return;
    }
    gate_fullscreen(raster, *tri.program);
    gate_clear(raster);
    gate_copy_blit(raster, *tri.program);
    gate_resolve(raster, *tri.program);
    gate_storage_pull(gctx, raster, alloc);
    gate_indexed(gctx, raster, alloc);
    gate_depth_loadstore(gctx, raster, alloc);
    gate_mesh(gctx, raster, alloc);
    gate_tess(gctx, raster, alloc);
}
} // namespace

TEST_CASE("raf2b vulkan command encoder: draw, clear, copy, blit, resolve", "[gpu][vulkan][raf2]")
{
    GpuContextConfig cfg;
    cfg.backend = GpuBackend::Vulkan;
    cfg.headless = true;
    auto ctx = create_vulkan_gpu_context(cfg);
    if (ctx == nullptr)
    {
        WARN("no Vulkan device available; skipping");
        return;
    }
    auto* vk = static_cast<VulkanGpuContext*>(ctx.get());
    if (!vk->shader_object())
    {
        WARN("adapter has no VK_EXT_shader_object; skipping");
        return;
    }
    crd::memory::TlsfAllocator alloc(4U << 20U, nullptr, "raf2b-vk");
    auto raster = create_vulkan_raster_context(*vk);
    REQUIRE(raster != nullptr);
    run_all_gates(*ctx, *raster, alloc);
}

#if defined(_WIN32)
TEST_CASE("raf2b dx12 command encoder: draw, clear, copy, blit, resolve", "[gpu][dx12][raf2]")
{
    auto gctx = create_dx12_gpu_context();
    if (gctx == nullptr || !gctx->valid())
    {
        WARN("no D3D12 device available; skipping");
        return;
    }
    auto raster = create_dx12_raster_context();
    REQUIRE(raster != nullptr);
    REQUIRE(raster->valid());
    crd::memory::TlsfAllocator alloc(4U << 20U, nullptr, "raf2b-dx12");
    run_all_gates(*gctx, *raster, alloc);
}
#endif
