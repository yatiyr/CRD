// Phase 3.0 v1o2/v1o3 — UploadHandle + RenderUploadSystem + RenderMeshIndex tests.
// (ADR-0061 Layer 2/3, with the v1o3 RenderMeshIndex drop-callback split.)
//
// Eight cases covering the substrate end-to-end without needing a real GPU:
//
//   1. UploadHandle: default-constructed is invalid + is_ready() false.
//   2. UploadHandle: synthetic populate (fake fence pre-signaled, fake mesh
//      with fake buffers) → is_valid + is_ready true → take_mesh moves out
//      and resets pending_mesh.
//   3. UploadHandle: take_texture path (independent of mesh slot).
//   4. PendingMeshUpload component is move-only and survives an Array store.
//   5. RenderUploadSystem dispatches: register Renderable + PendingMeshUpload
//      + RenderMeshIndex; pre-signaled handle → run system → Renderable
//      buffers populated; AsyncAware marked Loaded; PendingMeshUpload
//      removed; mesh-index count = 1.
//   6. RenderUploadSystem skips entities whose handle is_ready() returns
//      false (still in flight).
//   7. RenderMeshIndex: World::destroy(e) evicts the resident GpuMesh
//      via the drop-callback path (on_remove(Renderable) → erase).
//   8. RenderMeshIndex::release manual eviction works for tests/consumers
//      that need to drop a mesh without destroying the entity.

#include <crd/memory/allocator.hpp>
#include <crd/renderer/gpu_uploader.hpp>
#include <crd/renderer/render_mesh_index.hpp>
#include <crd/renderer/render_upload_system.hpp>
#include <crd/renderer/renderer.hpp>
#include <crd/rhi/buffer.hpp>
#include <crd/rhi/fence.hpp>
#include <crd/scene/async_aware_index.hpp>
#include <crd/scene/component_registry.hpp>
#include <crd/scene/world.hpp>

#include <catch2/catch_test_macros.hpp>

#include <utility>

namespace
{
// Local copies of the rhi-tests fakes (we can't reach across translation
// units; this is the cost of keeping each tests/ binary self-contained).
class FakeFence final : public crd::rhi::Fence
{
public:
    explicit FakeFence(bool initial_signaled = false) noexcept : m_signaled(initial_signaled) {}

    [[nodiscard]] bool is_signaled() const noexcept override { return m_signaled; }
    void               wait()                       override { m_signaled = true; }
    void               reset()                      override { m_signaled = false; }

private:
    bool m_signaled = false;
};

class FakeBuffer final : public crd::rhi::Buffer
{
public:
    explicit FakeBuffer(crd::rhi::BufferDesc desc) : m_desc(std::move(desc)) {}

    [[nodiscard]] const crd::rhi::BufferDesc& desc() const noexcept override { return m_desc; }
    [[nodiscard]] void*       map()             noexcept override { return nullptr; }
    void                      unmap()           noexcept override {}

private:
    crd::rhi::BufferDesc m_desc;
};

[[nodiscard]] crd::renderer::UploadHandle make_synthetic_mesh_handle(bool pre_signaled)
{
    crd::renderer::UploadHandle h;
    h.fence = std::make_unique<FakeFence>(pre_signaled);
    h.pending_mesh.vertex_buffer = std::make_unique<FakeBuffer>(crd::rhi::BufferDesc{
        .size_bytes = 64U,
        .usage      = crd::rhi::enum_bits(crd::rhi::BufferUsage::Vertex)});
    h.pending_mesh.index_buffer  = std::make_unique<FakeBuffer>(crd::rhi::BufferDesc{
        .size_bytes = 32U,
        .usage      = crd::rhi::enum_bits(crd::rhi::BufferUsage::Index)});
    return h;
}

// Helper: stand up a World with the Renderable + PendingMeshUpload
// component registry, a RenderMeshIndex bound to Renderable, and the
// RenderUploadSystem in the schedule. Returns the index pointer for
// post-step assertions.
crd::renderer::RenderMeshIndex* setup_world_for_upload(crd::scene::World& world)
{
    world.register_component<crd::renderer::Renderable>(
        crd::scene::AsyncAware{},
        crd::scene::StorageHint::SparseSet);
    world.register_component<crd::renderer::PendingMeshUpload>(
        crd::scene::StorageHint::SparseSet);

    auto* mesh_idx = world.register_index<crd::renderer::RenderMeshIndex>(
        crd::memory::default_allocator());
    mesh_idx->watch(world.component_id<crd::renderer::Renderable>());

    world.register_system(std::make_unique<crd::renderer::RenderUploadSystem>());
    return mesh_idx;
}

} // namespace

TEST_CASE("UploadHandle: default-constructed is invalid and not ready",
          "[renderer][upload-handle][v1o2]")
{
    crd::renderer::UploadHandle h;
    CHECK_FALSE(h.is_valid());
    CHECK_FALSE(h.is_ready());
}

TEST_CASE("UploadHandle: synthetic populate + take_mesh moves out the GpuMesh",
          "[renderer][upload-handle][v1o2]")
{
    auto h = make_synthetic_mesh_handle(/*pre_signaled=*/true);
    REQUIRE(h.is_valid());
    REQUIRE(h.is_ready());

    auto* original_vb = h.pending_mesh.vertex_buffer.get();
    auto* original_ib = h.pending_mesh.index_buffer.get();
    REQUIRE(original_vb != nullptr);
    REQUIRE(original_ib != nullptr);

    auto mesh = h.take_mesh();
    CHECK(mesh.vertex_buffer.get() == original_vb);
    CHECK(mesh.index_buffer.get()  == original_ib);
    // pending_mesh's pointers were moved-from; they are now null.
    CHECK(h.pending_mesh.vertex_buffer == nullptr);
    CHECK(h.pending_mesh.index_buffer  == nullptr);
}

TEST_CASE("UploadHandle: take_texture path is independent of mesh slot",
          "[renderer][upload-handle][v1o2]")
{
    crd::renderer::UploadHandle h;
    h.fence = std::make_unique<FakeFence>(/*pre_signaled=*/true);
    REQUIRE(h.is_ready());
    auto tex = h.take_texture();
    CHECK(tex.image == nullptr); // synthetic handle didn't populate it
}

TEST_CASE("PendingMeshUpload component is move-only and survives storage",
          "[renderer][upload-handle][v1o2][component]")
{
    crd::renderer::PendingMeshUpload p1{make_synthetic_mesh_handle(true)};
    REQUIRE(p1.handle.is_valid());

    // Move-construct.
    crd::renderer::PendingMeshUpload p2{std::move(p1)};
    CHECK(p2.handle.is_valid());
    CHECK_FALSE(p1.handle.is_valid()); // NOLINT(bugprone-use-after-move)

    // Move-assign.
    crd::renderer::PendingMeshUpload p3;
    p3 = std::move(p2);
    CHECK(p3.handle.is_valid());

    // Compile-time: the type is non-copyable.
    static_assert(!std::is_copy_constructible_v<crd::renderer::PendingMeshUpload>);
    static_assert(!std::is_copy_assignable_v<crd::renderer::PendingMeshUpload>);
}

TEST_CASE("RenderUploadSystem promotes ready uploads + marks AsyncAware Loaded",
          "[renderer][render-upload-system][v1o2]")
{
    crd::scene::World world{crd::memory::default_allocator()};
    auto* mesh_idx = setup_world_for_upload(world);

    const auto e = world.spawn();
    world.add_component<crd::renderer::Renderable>(e, crd::renderer::Renderable{});
    world.add_component<crd::renderer::PendingMeshUpload>(
        e, crd::renderer::PendingMeshUpload{make_synthetic_mesh_handle(/*pre_signaled=*/true)});

    // Pre-step: AsyncAware marks the Renderable as Loading on insert.
    const auto cid_renderable = world.component_id<crd::renderer::Renderable>();
    REQUIRE((*world.find_index<crd::scene::AsyncAwareIndex>()).is_pending(e, cid_renderable));

    // Drive the schedule once. RenderExtract phase fires our system.
    world.step(0.016);

    // Renderable buffers were patched from the upload handle's pending_mesh.
    const auto* r = world.get_component<crd::renderer::Renderable>(e);
    REQUIRE(r != nullptr);
    CHECK(r->vertex_buffer != nullptr);
    CHECK(r->index_buffer  != nullptr);

    // AsyncAware flipped to Loaded.
    CHECK_FALSE((*world.find_index<crd::scene::AsyncAwareIndex>()).is_pending(e, cid_renderable));
    CHECK((*world.find_index<crd::scene::AsyncAwareIndex>()).is_loaded(e, cid_renderable));

    // PendingMeshUpload was removed via Commands flush.
    CHECK_FALSE(world.has_component<crd::renderer::PendingMeshUpload>(e));

    // RenderMeshIndex retained ownership of the GpuMesh.
    CHECK(mesh_idx->count() == 1U);
    CHECK(mesh_idx->find(e) != nullptr);
}

TEST_CASE("RenderUploadSystem skips entities whose upload is not yet ready",
          "[renderer][render-upload-system][v1o2]")
{
    crd::scene::World world{crd::memory::default_allocator()};
    auto* mesh_idx = setup_world_for_upload(world);

    const auto e = world.spawn();
    world.add_component<crd::renderer::Renderable>(e, crd::renderer::Renderable{});
    world.add_component<crd::renderer::PendingMeshUpload>(
        e, crd::renderer::PendingMeshUpload{make_synthetic_mesh_handle(/*pre_signaled=*/false)});

    world.step(0.016);

    // Renderable buffers untouched.
    const auto* r = world.get_component<crd::renderer::Renderable>(e);
    REQUIRE(r != nullptr);
    CHECK(r->vertex_buffer == nullptr);
    CHECK(r->index_buffer  == nullptr);

    // PendingMeshUpload still attached.
    CHECK(world.has_component<crd::renderer::PendingMeshUpload>(e));

    // Still loading.
    const auto cid_renderable = world.component_id<crd::renderer::Renderable>();
    CHECK((*world.find_index<crd::scene::AsyncAwareIndex>()).is_pending(e, cid_renderable));

    // No mesh transferred to the index.
    CHECK(mesh_idx->count() == 0U);
}

TEST_CASE("RenderMeshIndex: World::destroy(e) evicts the resident GpuMesh "
          "via the drop-callback path",
          "[renderer][render-mesh-index][v1o3]")
{
    crd::scene::World world{crd::memory::default_allocator()};
    auto* mesh_idx = setup_world_for_upload(world);

    const auto e = world.spawn();
    world.add_component<crd::renderer::Renderable>(e, crd::renderer::Renderable{});
    world.add_component<crd::renderer::PendingMeshUpload>(
        e, crd::renderer::PendingMeshUpload{make_synthetic_mesh_handle(/*pre_signaled=*/true)});

    world.step(0.016);
    REQUIRE(mesh_idx->count() == 1U);
    REQUIRE(mesh_idx->find(e) != nullptr);

    // Destroy the entity. The deferred-destroy queue is drained at the
    // end of the next step (or via flush_destroys); both paths fan
    // on_remove(Renderable) + on_entity_destroyed events to every
    // observer index — including RenderMeshIndex — which evicts the
    // resident GpuMesh automatically.
    world.destroy(e);
    world.flush_destroys();

    CHECK(mesh_idx->count() == 0U);
    CHECK(mesh_idx->find(e) == nullptr);
}

TEST_CASE("RenderMeshIndex::release manual eviction",
          "[renderer][render-mesh-index][v1o3]")
{
    crd::scene::World world{crd::memory::default_allocator()};
    auto* mesh_idx = setup_world_for_upload(world);

    const auto e = world.spawn();
    world.add_component<crd::renderer::Renderable>(e, crd::renderer::Renderable{});
    world.add_component<crd::renderer::PendingMeshUpload>(
        e, crd::renderer::PendingMeshUpload{make_synthetic_mesh_handle(/*pre_signaled=*/true)});

    world.step(0.016);
    REQUIRE(mesh_idx->count() == 1U);

    // release() returns true when an entry was erased, false otherwise.
    CHECK(mesh_idx->release(e));
    CHECK(mesh_idx->count() == 0U);
    CHECK_FALSE(mesh_idx->release(e));
}
