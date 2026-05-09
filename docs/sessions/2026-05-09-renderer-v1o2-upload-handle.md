# 2026-05-09 — Phase 3.0 v1o2: `UploadHandle` + async `GpuUploader` + `RenderUploadSystem` (ADR-0061 Layer 2 + Layer 3)

**Status at start:** v1o1 shipped earlier today (Fence interface + non-waiting `Queue::submit(cmd, fence)`). v1o is the active slice; ADR-0061 §"Layer 2" + §"Layer 3" carve the renderer + ECS surface into v1o2.

**Status at end:** v1o2 shipped — `UploadHandle` (move-only, owns the in-flight Fence + staging buffers + recorded CommandBuffer + pending GpuMesh/GpuTexture); `GpuUploader::upload_mesh_async` / `upload_texture_async`; `PendingMeshUpload` ECS component; `RenderUploadSystem` running in `RenderExtract` phase. **Full 12-config sweep all green at 849/849** (846 in optimised configs). +6 cases / +26 assertions over the 843/843 v1o1 baseline.

---

## What shipped

### `UploadHandle` (`engine/renderer/include/crd/renderer/gpu_uploader.hpp`)

```cpp
class UploadHandle
{
public:
    UploadHandle() noexcept = default;
    UploadHandle(UploadHandle&&) noexcept = default;
    UploadHandle& operator=(UploadHandle&&) noexcept = default;
    UploadHandle(const UploadHandle&)            = delete;
    UploadHandle& operator=(const UploadHandle&) = delete;

    [[nodiscard]] bool is_valid() const noexcept { return fence != nullptr; }
    [[nodiscard]] bool is_ready() const noexcept
    {
        return fence != nullptr && fence->is_signaled();
    }
    void wait() { if (fence != nullptr) fence->wait(); }

    [[nodiscard]] GpuMesh    take_mesh()    noexcept { return std::move(pending_mesh); }
    [[nodiscard]] GpuTexture take_texture() noexcept { return std::move(pending_texture); }

    // Public members so synthetic test handles can be assembled without
    // running a real GpuUploader. The runtime path always populates
    // these via upload_*_async().
    std::unique_ptr<crd::rhi::Fence>          fence;
    std::unique_ptr<crd::rhi::CommandBuffer>  cmd;
    std::unique_ptr<crd::rhi::Buffer>         staging_a;
    std::unique_ptr<crd::rhi::Buffer>         staging_b;
    GpuMesh                                   pending_mesh;
    GpuTexture                                pending_texture;
};
```

**Lifetime contract:** the handle owns every resource the GPU is reading from until the fence signals. Dropping a not-yet-ready handle is a logic bug — the destructor does NOT call `fence->wait()` on its own (would hide ownership errors); consumers must poll or wait. The handle is move-only and freely move-stored inside ECS components.

**Threading contract:** not thread-safe; one handle ↔ one logical upload. `is_ready()` is non-blocking and safe to poll once per frame from the schedule.

### `GpuUploader::upload_*_async` (`engine/renderer/src/gpu_uploader.cpp`)

```cpp
[[nodiscard]] UploadHandle upload_mesh_async(
    crd::rhi::Device&    device,
    const MeshData&      mesh,
    crd::memory::IAllocator* alloc);

[[nodiscard]] UploadHandle upload_texture_async(
    crd::rhi::Device&    device,
    const TextureData&   texture,
    crd::memory::IAllocator* alloc);
```

Implementation mirrors the existing blocking `upload_*` paths but uses the v1o1 non-waiting submit:

1. Create the destination `Buffer`/`Image` via the device.
2. Allocate one or two staging buffers (vertex+index for mesh, single staging for texture); map → memcpy → unmap.
3. Acquire a `CommandBuffer`, record the copy(s), end recording.
4. Create a fresh `Fence` via `device.create_fence()` (unsignalled).
5. `device.graphics_queue().submit(*cmd, *fence)` (the v1o1 non-waiting variant).
6. Move-construct an `UploadHandle` taking ownership of fence + cmd + staging + the produced GpuMesh/GpuTexture; return it.

The function does NOT call `fence.wait()` — that's the entire point. Consumers poll `is_ready()` on the schedule and consume only when signalled.

### `PendingMeshUpload` ECS component (`engine/renderer/include/crd/renderer/render_upload_system.hpp`)

```cpp
struct PendingMeshUpload
{
    UploadHandle handle;
};
```

Move-only POD attached alongside `Renderable` while a GPU mesh upload is in flight. The component carries the entire `UploadHandle` (transitively the fence + staging + pending GpuMesh) inline — no out-of-line allocation, the handle's `unique_ptr` members handle GPU-resource ownership.

### `RenderUploadSystem` (`engine/renderer/src/render_upload_system.cpp`)

```cpp
class RenderUploadSystem final : public crd::scene::ISystem
{
public:
    using Reads  = crd::scene::ComponentSet<>;
    using Writes = crd::scene::ComponentSet<Renderable, PendingMeshUpload>;

    explicit RenderUploadSystem(crd::memory::IAllocator* alloc);

    [[nodiscard]] crd::scene::SchedulePhase phase() const override
    {
        return crd::scene::SchedulePhase::RenderExtract;
    }
    [[nodiscard]] crd::containers::StringView name() const override
    {
        return crd::containers::StringView{"RenderUploadSystem"};
    }

    void run(crd::scene::World& world) override;

    [[nodiscard]] crd::usize owned_mesh_count() const noexcept
    {
        return m_owned_meshes.size();
    }

private:
    crd::containers::HashMap<crd::u64, GpuMesh> m_owned_meshes;
};
```

**Run loop (per RenderExtract step):**

```cpp
void RenderUploadSystem::run(crd::scene::World& world)
{
    const auto cid_renderable = world.component_id<Renderable>();
    if (cid_renderable.is_null()) return; // not registered → no work

    auto* async_aware = world.find_index<crd::scene::AsyncAwareIndex>();
    auto& cmds        = world.commands();

    auto query = world.query<Renderable, PendingMeshUpload>();
    for (auto&& [entity, renderable, pending] : query)
    {
        if (!pending.handle.is_valid() || !pending.handle.is_ready())
            continue;

        // Move the GpuMesh into our owned store, keyed by EntityId.raw.
        [[maybe_unused]] const bool inserted =
            m_owned_meshes.emplace(entity.raw, pending.handle.take_mesh());
        CRD_ASSERT_MSG(inserted, "duplicate upload for the same entity");
        GpuMesh* stored = m_owned_meshes.find(entity.raw);
        CRD_ASSERT_MSG(stored != nullptr, "failed to retrieve just-inserted mesh");

        // Patch Renderable to the resident buffers.
        renderable.vertex_buffer = stored->vertex_buffer.get();
        renderable.index_buffer  = stored->index_buffer.get();

        // Mark AsyncAware Loaded → next-frame `query<>().skip_pending<Renderable>()`
        // starts including the entity. AsyncAware index pointer is non-null when
        // Renderable was registered with the AsyncAware{} trait.
        if (async_aware != nullptr)
            async_aware->mark_loaded(entity, cid_renderable);

        // Drop the marker — upload state has migrated to m_owned_meshes.
        cmds.remove_component<PendingMeshUpload>(entity);
    }
}
```

Three-step promotion: **take** (move ownership of GpuMesh out of the handle), **patch** (rewire the Renderable to the resident buffers), **flip + drop** (mark the AsyncAware state Loaded, remove the marker). The Commands buffer flushes at the phase boundary, so the marker is gone before the next phase reads it.

### Tests (`tests/renderer/test_render_upload_system.cpp`)

6 cases / 26 assertions on a synthetic-handle pathway (no real Vulkan). Local `FakeFence` and `FakeBuffer` plus a `make_synthetic_mesh_handle(bool pre_signaled)` helper — lets every test exercise the system in <1 ms without a GPU.

1. **`UploadHandle: default-constructed is invalid and not ready`** — `UploadHandle{}` has `fence == nullptr`; `is_valid()` false; `is_ready()` false; `wait()` is a no-op.
2. **`UploadHandle: synthetic populate + take_mesh moves out the GpuMesh`** — populate fence/cmd/staging/pending_mesh; before `take_mesh` the GpuMesh is held inline; after `take_mesh` the moved-from `pending_mesh` is empty.
3. **`UploadHandle: take_texture path is independent of mesh slot`** — same pattern for `take_texture`; mesh and texture slots don't alias.
4. **`PendingMeshUpload component is move-only and survives storage`** — assert non-copyable; assert moves preserve the handle's fence pointer.
5. **`RenderUploadSystem promotes ready uploads + marks AsyncAware Loaded`** — register Renderable + PendingMeshUpload with AsyncAware trait; spawn entity; install ready handle; step RenderExtract; assert: `m_owned_meshes.size() == 1`; Renderable buffers point at the stored GpuMesh's buffers; `AsyncAwareIndex::is_loaded` returns true; PendingMeshUpload component removed.
6. **`RenderUploadSystem skips entities whose upload is not yet ready`** — same as (5) but with `is_signaled = false`; assert no promotion happened, marker still attached, AsyncAware still Loading.

`tests/renderer/CMakeLists.txt` was extended to link `crd-scene` (was already pulled in transitively but now used explicitly for `World`/`AsyncAwareIndex`/`Commands`).

### Architectural pins

1. **The handle is the single ownership root.** Fence + staging + cmd + pending GpuMesh/GpuTexture all live inside the same `UploadHandle`. Move it freely (into the ECS component, out of the system, into a queue). When it's destroyed, all GPU resources are released. There's no "manager" tracking these — the type system handles it.

2. **Three states, no enum.** `is_valid()` / `is_ready()` are independent booleans:
   - default-constructed → `!is_valid()`
   - in-flight → `is_valid() && !is_ready()`
   - completed → `is_valid() && is_ready()`
   No "Failed" state at this layer — ADR-0061 §"Layer 2" pinned that submission failures surface as exceptions/aborts at the `upload_*_async` boundary. v1o2 does not yet wire a recoverable Failed transition; that's a follow-up if a consumer needs it.

3. **`m_owned_meshes` is keyed by `EntityId.raw`, not `EntityId`.** HashMap requires `Hashable<u64>`; `EntityId` doesn't have a `Hashable` overload yet (could be added; not required for v1o2). The 64-bit raw representation is stable across destroy+respawn (gen bumps invalidate stale handles via the slotmap, so a duplicate raw value implies the prior entity was already gone — but v1o2's monotonic-grow store doesn't yet free on destroy — see point 5 below).

4. **Patches the `Renderable` directly, no copy through Commands.** This is intentional. Commands is for spawn/destroy/component add+remove (structural changes that affect storage layout). In-place mutation of an already-resident component during a phase is just a member assignment — same as any system updating a Transform. The mutation is visible inside the same RenderExtract phase, but no other system in `RenderExtract` reads `Renderable.vertex_buffer`, so there's no ordering hazard.

5. **`m_owned_meshes` grows monotonically until World destruction.** Sandbox-scale (a few dozen meshes) is fine. v1o3 (sandbox) drives the lifetime contract refinement: an entity-destroyed event needs to evict the matching mesh. The cleanest hook will be a per-component drop callback registered on Renderable, but pinning that depends on what v1o3 actually needs. Tracked in followups; not blocking phase progress.

6. **Re-using the v1o1 fence path one-fence-per-upload.** No fence pool yet (per v1o1 §"Architectural pins" #5). At sandbox scale this is ~10–50 outstanding `vkCreateFence` calls per minute, well within the noise floor. If Phase 3.5+ streaming fans out to thousands of concurrent uploads, a pool layers on inside `Device::create_fence()` without changing the public API.

7. **The `[[maybe_unused]] const bool inserted` is intentional.** `CRD_ASSERT_MSG` compiles out under `NDEBUG`, so the bool would otherwise trip MSVC C4189 (unused-variable) under `/WX` in win-release / win-clang-cl / win-shipping. The convention across the codebase is `[[maybe_unused]]` on assert-only locals; codified after this slice's first 12-config sweep caught the warning.

### Six-configuration green — full sweep

**Windows (7/7):**
| Config | Status | Tests |
|---|---|---|
| win-debug | ✅ | 849/849 |
| win-asan | ✅ | 849/849 |
| win-relwithdebinfo | ✅ | 849/849 |
| win-release | ✅ | 846/846 |
| win-tidy | ✅ | build clean |
| win-clang-cl | ✅ | 849/849 |
| win-shipping | ✅ | build clean |

**Linux (5/5):**
| Config | Status | Tests |
|---|---|---|
| linux-gcc-debug | ✅ | 849/849 |
| linux-gcc-release | ✅ | 846/846 |
| linux-gcc-relwithdebinfo | ✅ | 849/849 |
| linux-gcc-asan | ✅ | 849/849 |
| linux-gcc-shipping | ✅ | build clean |

(849 = 843 v1o1 baseline + 6 v1o2 upload tests. 846 in optimised configs minus 3 debug-only `FiberState` tests.)

### Build infrastructure note

The first attempt at a parallel Linux sweep collided on `scripts/wsl-build.ps1`'s shared `.wsl-build-tmp.sh` path — two streams writing the bash script to the same file before either's `wsl bash` call ran. The fix in this slice: per-preset temp filename (`.wsl-build-tmp-{preset}.sh`), making future parallel sweeps safe. Documented in `wsl-build.ps1`'s comments.

---

## What's deliberately NOT in v1o2

- **No texture-side ECS component.** v1o2 ships `upload_texture_async` + `take_texture()` on the handle, but there's no `PendingTextureUpload` component yet. The first consumer (probably MaterialResource hot-load in v1o3 / Phase 3.x) drives the symmetric component+system. Until then, callers wait synchronously on the texture handle.
- **No transfer queue.** Carried over from v1o1 (graphics queue serialises uploads).
- **No fence pool.** Carried over from v1o1.
- **No on-entity-destroyed cleanup of `m_owned_meshes`.** Sandbox-safe; v1o3 drives the lifetime contract refinement (see Architectural pin #5).
- **No `.skip_pending<>()` plumbing changes.** The v1i implementation already does the right thing because v1o2 calls `AsyncAwareIndex::mark_loaded` at promotion — the existing query operator picks it up next frame.
- **No batch upload API.** Each `upload_*_async` call gets its own command buffer + fence + submission. AAAA-tier streaming will batch these per-frame; not blocking.

---

## Files touched

```
engine/renderer/include/crd/renderer/gpu_uploader.hpp        +35 lines (UploadHandle class + async signatures)
engine/renderer/include/crd/renderer/render_upload_system.hpp created (~80 lines)
engine/renderer/src/gpu_uploader.cpp                         +50 lines (upload_*_async impls)
engine/renderer/src/render_upload_system.cpp                 created (~70 lines)
engine/renderer/CMakeLists.txt                               +1 line   (crd-scene PUBLIC link)

tests/renderer/test_render_upload_system.cpp                 created (~280 lines, 6 cases)
tests/renderer/CMakeLists.txt                                +1 line   (new test source)

scripts/wsl-build.ps1                                        +3 lines  (per-preset temp file fix)
docs/phases/phase-3.0-scene-ecs.md                           v1o2 row added; ADR-0061 marked realised
docs/sessions/2026-05-09-renderer-v1o2-upload-handle.md      this file
context.md                                                   dashboard updated
```

---

## Next: v1o3 — sandbox integration

Sandbox loads a `.scene.toml` referencing öbeks; profile-driven preset application; ImGui live override panel; the full async-upload path goes end-to-end with a real Vulkan device. v1o3 is also the slice where the on-entity-destroyed cleanup contract for `RenderUploadSystem::m_owned_meshes` gets pinned, because the sandbox is the first place where entities actually come and go.

After v1o3, v1p (reserved-slot freeze) closes Phase 3.0.
