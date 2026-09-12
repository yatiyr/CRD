// RAF-2a — the canonical declarative GPU command model: host-side (device-free) validation gate.
//
// Gates the STRUCTURE of the model that replaces the 53 combinatorial draw_* verbs: rendering scopes
// (color / depth / color+depth / MRT / depth-only, load-store, sizes), raster draw packets (program,
// geometry/command agreement, indexed/indirect, bindings, comparison-sampler pairing, bindless bounds),
// and compute dispatch. The no-per-draw-allocation contract is proven at COMPILE time: the binding table
// stores its bindings INLINE (FixedArray, no allocator), so building/validating a packet allocates nothing.
// Backend lowering + cross-backend GPU parity land in RAF-2b/2c (a device is required for those).

#include <crd/gpu/command_model.hpp>

#include <catch2/catch_test_macros.hpp>

using crd::u32;
using namespace crd::gpu;

namespace
{
// Minimal fakes — structural validation never touches device state.
class FakeTarget final : public IRasterTarget
{
public:
    FakeTarget(u32 w, u32 h) noexcept : m_w(w), m_h(h) {}
    [[nodiscard]] u32 width() const noexcept override { return m_w; }
    [[nodiscard]] u32 height() const noexcept override { return m_h; }
    [[nodiscard]] u32 read_pixel(u32, u32) const noexcept override { return 0U; }

private:
    u32 m_w;
    u32 m_h;
};

class FakeProgram final : public IRasterProgram
{
public:
    [[nodiscard]] bool valid() const noexcept override { return true; }
};

// Non-null sentinels for incomplete opaque types — validation only checks null-vs-non-null, never dereferences.
int g_sentinel = 0;
IStorageBuffer* dummy_buffer() noexcept { return reinterpret_cast<IStorageBuffer*>(&g_sentinel); }
IGpuProgram* dummy_kernel() noexcept { return reinterpret_cast<IGpuProgram*>(&g_sentinel); }
IAccelerationStructure* dummy_as() noexcept { return reinterpret_cast<IAccelerationStructure*>(&g_sentinel); }

RenderingDesc color_only(FakeTarget& t)
{
    RenderingDesc r;
    r.width = t.width();
    r.height = t.height();
    r.color.push_back(ColorAttachmentDesc{&t, LoadOp::Clear, StoreOp::Store, ClearColor{}, BlendMode::Opaque});
    return r;
}

RasterDrawPacket basic_draw(FakeProgram& p)
{
    RasterDrawPacket packet;
    packet.program = &p;
    packet.command = RasterCommandKind::Draw;
    packet.geometry.kind = GeometryKind::StoragePull;
    packet.geometry.vertex_or_index_count = 3U;
    packet.geometry.instance_count = 1U;
    return packet;
}
} // namespace

// ── No-per-draw-allocation: the binding table (and thus the packet) stores bindings INLINE. A heap-backed
//    container would shrink this to a pointer; the inline FixedArray makes it at least N*sizeof(binding). ──
static_assert(sizeof(ResourceBindingTable) >= kMaxBindings * sizeof(ResourceBinding),
              "binding table must store bindings inline (no per-draw heap allocation)");

TEST_CASE("raf2 rendering scope validation")
{
    FakeTarget t(64U, 64U);
    FakeTarget dt(64U, 64U);
    FakeTarget wrong(32U, 32U);

    SECTION("color only is valid")
    {
        REQUIRE(validate_rendering(color_only(t)) == CommandError::None);
    }
    SECTION("color plus depth is valid")
    {
        RenderingDesc r = color_only(t);
        r.depth = DepthStencilAttachmentDesc{&dt, true, LoadOp::Clear, StoreOp::Store, 1.0F, true, DepthCompare::LessEqual};
        REQUIRE(validate_rendering(r) == CommandError::None);
    }
    SECTION("depth only (no color) is valid")
    {
        RenderingDesc r;
        r.width = 64U;
        r.height = 64U;
        r.depth = DepthStencilAttachmentDesc{&dt, true, LoadOp::Clear, StoreOp::Store, 1.0F, true, DepthCompare::LessEqual};
        REQUIRE(validate_rendering(r) == CommandError::None);
    }
    SECTION("MRT with several color attachments is valid")
    {
        RenderingDesc r = color_only(t);
        FakeTarget t2(64U, 64U);
        FakeTarget t3(64U, 64U);
        r.color.push_back(ColorAttachmentDesc{&t2, LoadOp::Clear, StoreOp::Store, ClearColor{}, BlendMode::Additive});
        r.color.push_back(ColorAttachmentDesc{&t3, LoadOp::Clear, StoreOp::Store, ClearColor{}, BlendMode::RevealageMultiply});
        REQUIRE(r.color.size() == 3U);
        REQUIRE(validate_rendering(r) == CommandError::None);
    }
    SECTION("no attachments is rejected")
    {
        RenderingDesc r;
        r.width = 64U;
        r.height = 64U;
        REQUIRE(validate_rendering(r) == CommandError::NoAttachments);
    }
    SECTION("zero render area is rejected")
    {
        RenderingDesc r = color_only(t);
        r.width = 0U;
        REQUIRE(validate_rendering(r) == CommandError::ZeroRenderArea);
    }
    SECTION("mismatched attachment size is rejected")
    {
        RenderingDesc r;
        r.width = 64U;
        r.height = 64U;
        r.color.push_back(ColorAttachmentDesc{&wrong, LoadOp::Clear, StoreOp::Store, ClearColor{}, BlendMode::Opaque});
        REQUIRE(validate_rendering(r) == CommandError::MismatchedAttachmentSize);
    }
}

TEST_CASE("raf2 raster packet validation")
{
    FakeProgram p;

    SECTION("basic storage-pull draw is valid")
    {
        REQUIRE(validate_packet(basic_draw(p)) == CommandError::None);
    }
    SECTION("null program is rejected")
    {
        RasterDrawPacket packet = basic_draw(p);
        packet.program = nullptr;
        REQUIRE(validate_packet(packet) == CommandError::NullProgram);
    }
    SECTION("zero-vertex draw is rejected")
    {
        RasterDrawPacket packet = basic_draw(p);
        packet.geometry.vertex_or_index_count = 0U;
        REQUIRE(validate_packet(packet) == CommandError::ZeroDraw);
    }
    SECTION("indexed draw needs an index buffer")
    {
        RasterDrawPacket packet;
        packet.program = &p;
        packet.command = RasterCommandKind::DrawIndexed;
        packet.geometry.kind = GeometryKind::Indexed;
        packet.geometry.vertex_or_index_count = 6U;
        REQUIRE(validate_packet(packet) == CommandError::MissingIndexBuffer);
        packet.geometry.index_buffer = dummy_buffer();
        REQUIRE(validate_packet(packet) == CommandError::None);
    }
    SECTION("command and geometry must agree")
    {
        RasterDrawPacket packet = basic_draw(p);
        packet.geometry.kind = GeometryKind::Indexed; // Draw command with Indexed geometry
        REQUIRE(validate_packet(packet) == CommandError::GeometryCommandMismatch);
    }
    SECTION("mesh dispatch needs non-zero groups")
    {
        RasterDrawPacket packet;
        packet.program = &p;
        packet.command = RasterCommandKind::DispatchMesh;
        packet.geometry.kind = GeometryKind::Meshlet;
        REQUIRE(validate_packet(packet) == CommandError::None); // default groups 1,1,1
        packet.geometry.group_count_x = 0U;
        REQUIRE(validate_packet(packet) == CommandError::ZeroDraw);
    }
    SECTION("indirect draw needs args")
    {
        RasterDrawPacket packet;
        packet.program = &p;
        packet.command = RasterCommandKind::DrawIndirect;
        packet.geometry.kind = GeometryKind::Indirect;
        REQUIRE(validate_packet(packet) == CommandError::MissingIndirectArgs);
        packet.geometry.args_buffer = dummy_buffer();
        REQUIRE(validate_packet(packet) == CommandError::None);
    }
    SECTION("tessellation patches need a non-zero count")
    {
        RasterDrawPacket packet;
        packet.program = &p;
        packet.command = RasterCommandKind::DrawPatches;
        packet.geometry.kind = GeometryKind::Patches;
        REQUIRE(validate_packet(packet) == CommandError::ZeroDraw);
        packet.geometry.patch_count = 2U;
        REQUIRE(validate_packet(packet) == CommandError::None);
    }
    SECTION("indexed-indirect needs args then an index buffer")
    {
        RasterDrawPacket packet;
        packet.program = &p;
        packet.command = RasterCommandKind::DrawIndexedIndirect;
        packet.geometry.kind = GeometryKind::Indirect;
        REQUIRE(validate_packet(packet) == CommandError::MissingIndirectArgs);
        packet.geometry.args_buffer = dummy_buffer();
        REQUIRE(validate_packet(packet) == CommandError::MissingIndexBuffer);
        packet.geometry.index_buffer = dummy_buffer();
        REQUIRE(validate_packet(packet) == CommandError::None);
    }
    SECTION("indexed-indirect-count needs args, count buffer, then an index buffer")
    {
        RasterDrawPacket packet;
        packet.program = &p;
        packet.command = RasterCommandKind::DrawIndexedIndirectCount;
        packet.geometry.kind = GeometryKind::IndirectCount;
        REQUIRE(validate_packet(packet) == CommandError::MissingIndirectArgs);
        packet.geometry.args_buffer = dummy_buffer();
        REQUIRE(validate_packet(packet) == CommandError::MissingCountBuffer);
        packet.geometry.count_buffer = dummy_buffer();
        REQUIRE(validate_packet(packet) == CommandError::MissingIndexBuffer);
        packet.geometry.index_buffer = dummy_buffer();
        REQUIRE(validate_packet(packet) == CommandError::None);
    }
}

TEST_CASE("raf2 ray trace validation")
{
    TraceDesc t;
    REQUIRE(validate_trace(t) == CommandError::NullProgram); // no raygen/miss/closest-hit
    t.raygen = dummy_kernel();
    t.miss = dummy_kernel();
    t.closest_hit = dummy_kernel();
    REQUIRE(validate_trace(t) == CommandError::MissingAccelerationStructure);
    t.acceleration_structure = dummy_as();
    REQUIRE(validate_trace(t) == CommandError::None);
    t.height = 0U;
    REQUIRE(validate_trace(t) == CommandError::ZeroDraw);
}

TEST_CASE("raf2 binding table validation")
{
    FakeProgram p;

    SECTION("duplicate frequency+slot is rejected")
    {
        RasterDrawPacket packet = basic_draw(p);
        packet.bindings.push_back(ResourceBinding{BindingFrequency::Material, BindingKind::StorageBuffer, 0U});
        packet.bindings.push_back(ResourceBinding{BindingFrequency::Material, BindingKind::SampledTexture, 0U});
        REQUIRE(validate_packet(packet) == CommandError::DuplicateBinding);
    }
    SECTION("comparison sampler requires a paired sampled texture")
    {
        RasterDrawPacket packet = basic_draw(p);
        packet.bindings.push_back(ResourceBinding{BindingFrequency::Material, BindingKind::ComparisonSampler, 2U});
        REQUIRE(validate_packet(packet) == CommandError::ComparisonSamplerWithoutTexture);
        packet.bindings.push_back(ResourceBinding{BindingFrequency::Material, BindingKind::SampledTexture, 1U});
        REQUIRE(validate_packet(packet) == CommandError::None);
    }
    SECTION("bindless array bounds are enforced")
    {
        RasterDrawPacket packet = basic_draw(p);
        ResourceBinding b{BindingFrequency::Material, BindingKind::BindlessTextureArray, 3U};
        b.array_count = 0U;
        packet.bindings.push_back(b);
        REQUIRE(validate_packet(packet) == CommandError::BindlessCountExceeded);

        RasterDrawPacket over = basic_draw(p);
        ResourceBinding b2{BindingFrequency::Material, BindingKind::BindlessTextureArray, 3U};
        b2.array_count = kMaxBindlessTextures + 1U;
        over.bindings.push_back(b2);
        REQUIRE(validate_packet(over) == CommandError::BindlessCountExceeded);

        RasterDrawPacket ok = basic_draw(p);
        ResourceBinding b3{BindingFrequency::Material, BindingKind::BindlessTextureArray, 3U};
        b3.array_count = 4U;
        ok.bindings.push_back(b3);
        REQUIRE(validate_packet(ok) == CommandError::None);
    }
}

TEST_CASE("raf2 compute dispatch validation")
{
    SECTION("direct dispatch is valid")
    {
        DispatchDesc d;
        d.kernel = dummy_kernel();
        REQUIRE(validate_dispatch(d) == CommandError::None);
    }
    SECTION("null kernel is rejected")
    {
        DispatchDesc d;
        REQUIRE(validate_dispatch(d) == CommandError::NullProgram);
    }
    SECTION("zero groups are rejected")
    {
        DispatchDesc d;
        d.kernel = dummy_kernel();
        d.groups_y = 0U;
        REQUIRE(validate_dispatch(d) == CommandError::ZeroDraw);
    }
    SECTION("indirect dispatch needs args")
    {
        DispatchDesc d;
        d.kernel = dummy_kernel();
        d.kind = DispatchKind::Indirect;
        REQUIRE(validate_dispatch(d) == CommandError::MissingIndirectArgs);
        d.args_buffer = dummy_buffer();
        REQUIRE(validate_dispatch(d) == CommandError::None);
    }
}

TEST_CASE("raf2 error names are stable")
{
    REQUIRE(command_error_name(CommandError::None) == "none");
    REQUIRE(command_error_name(CommandError::GeometryCommandMismatch) == "geometry-command-mismatch");
    REQUIRE(command_error_name(CommandError::DuplicateBinding) == "duplicate-binding");
}
