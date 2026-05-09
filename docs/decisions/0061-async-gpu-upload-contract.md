# ADR-0061 — Async GPU upload contract: `UploadHandle` + per-module polling system

**Status:** Accepted (2026-05-09)
**Tags:** arch, renderer, rhi, scene, resources, async
**Related ADRs:** ADR-0008 (Graphics architecture), ADR-0009 (RHI v1a scaffold), ADR-0014 (Reference counting split), ADR-0022 (Open-world streaming pipeline), ADR-0039 (`ResourceHandle<T>` semantics), ADR-0042 (Texture cooked format + GPU upload strategy), ADR-0043 (MeshResource vertex layout), ADR-0053 (Scene/ECS L5: Component index slot framework — `AsyncAwareIndex`)
**Phase:** Pulled-forward prerequisite for Phase 3.0 v1o (sandbox renderer integration with Öbek + Preset + Profile)

---

## Context

Phase 2.7 v1d shipped `GpuUploader::upload_texture` / `upload_mesh` as **synchronous** staging-buffer helpers. Each call records a transient command buffer, submits it to the graphics queue, and ends with `Queue::submit_and_wait` (a `vkQueueWaitIdle` on the main thread). For BoomBox-class assets (~10 MB GLB → ~30 MB raw mesh) this produces a multi-millisecond hitch on the calling frame.

Phase 2.8 v1g closed the **CPU-side** half: the sandbox now kicks `ResourceManager::load_async<MeshResource>` on click, polls `ResourceHandle<MeshResource>::is_ready()` per frame, and only at the moment of CPU readiness calls `GpuUploader::upload_mesh`. The disk-I/O + parse hitch is gone, but the GPU upload hitch remains on the same frame.

Phase 3.0 v1i shipped the consumer-facing query operator: `query<...>().skip_pending<T>()` filters out entities whose `T` is in `LoadState::Loading` per `AsyncAwareIndex`. The renderer can already say "render only ready entities" — but **nothing today flips a Renderable from `Loading` to `Loaded`** because GPU upload is synchronous: the upload completes inside the call, before the component is even inserted into the World.

Phase 3.0 v1o (sandbox integration) is the first slice that streams scene-loaded geometry through the ECS. It is the natural moment to close the GPU-upload async contract: it provides the first real consumer, the `RenderExtract` phase already exists in the schedule, and the `Renderable` / `AsyncAwareIndex` machinery is in place to consume the new contract.

The deciding question (carried open in `phase-3.0-scene-ecs.md::Pulled-forward prerequisites`):

> Does `crd-scene` own the upload polling, or does each `RenderableComponent` carry an `UploadHandle` field that the renderer's `skip_pending<Renderable>` filter consults?

This ADR answers it.

## Decision

Adopt a **three-layer async upload contract** with explicit module ownership:

```
crd-rhi          — exposes a Fence primitive + Queue::submit(cmd, fence) (no wait)
crd-renderer     — owns UploadHandle + GpuUploader::upload_*_async + PendingMeshUpload component + RenderUploadSystem
crd-scene        — already exposes AsyncAwareIndex + skip_pending<T>(); NO new responsibility
```

**Neither** of the two original options wins outright:

| Option | Verdict |
|---|---|
| (1) Scene owns upload polling | Rejected — scene-module dependency direction would invert (scene → rhi). Scene exists to orchestrate components, not own GPU primitives. |
| (2) Renderable carries `UploadHandle` | Adopted in part — components do carry an upload tag, but ownership of the polling code stays in the renderer module. |

The hybrid below scales: when audio later needs the same async-buffer-upload pattern, a sibling `AudioUploadSystem` lives in `crd-audio` against the same `AsyncAwareIndex` interface. No cross-module rewiring.

### Layer 1 — `crd-rhi`: minimal `Fence` primitive

Add a `crd::rhi::Fence` interface (Vulkan backend wraps `VkFence`; future backends wrap their equivalents). Add `Queue::submit(CommandBuffer&, Fence&)` — a non-waiting submit that signals `fence` on completion. The existing `submit_and_wait` and the per-frame `submit(cmd, swapchain)` paths stay unchanged; the new path is additive.

```cpp
// engine/rhi/include/crd/rhi/fence.hpp (new)
namespace crd::rhi {
class Fence {
public:
    virtual ~Fence() = default;
    [[nodiscard]] virtual bool is_signaled() const noexcept = 0; // non-blocking
    virtual void wait() = 0;                                      // blocking
    virtual void reset() = 0;                                     // re-arm for reuse
};
} // namespace crd::rhi
```

Device gains `[[nodiscard]] std::unique_ptr<Fence> create_fence() noexcept;`. Vulkan impl is ~30 LOC.

**Why a fence and not a timeline semaphore (yet):** binary fences are sufficient for "is this single upload done?". Timeline semaphores are richer (multi-step ordering, batched waits) and can be added later as a non-breaking extension if a consumer needs them. Don't over-design at the first call site.

**Transfer queue:** out of scope at v1o. Today's RHI exposes one graphics queue; concurrent uploads serialise behind frame submissions. Adding a transfer queue is a pure RHI extension — `Device::transfer_queue() → Queue&` — that the upload path uses opportunistically and falls back to graphics when absent. Tracked as a follow-up; not blocking.

### Layer 2 — `crd-renderer`: `UploadHandle` + async upload methods

```cpp
// engine/renderer/include/crd/renderer/gpu_uploader.hpp (additions)
namespace crd::renderer {

// Move-only handle to an in-flight async GPU upload.
// Owns: a fence + the staging buffer pinned until the fence signals + the
// produced GPU resource (mesh/texture). Single-thread ownership; no atomic
// state — the polling system is the sole reader.
class UploadHandle {
public:
    UploadHandle() noexcept = default;
    UploadHandle(UploadHandle&&) noexcept = default;
    UploadHandle& operator=(UploadHandle&&) noexcept = default;
    UploadHandle(const UploadHandle&) = delete;
    UploadHandle& operator=(const UploadHandle&) = delete;
    ~UploadHandle() noexcept;

    [[nodiscard]] bool is_valid() const noexcept;
    [[nodiscard]] bool is_ready() const noexcept;   // non-blocking; calls fence.is_signaled()
    bool wait();                                     // blocking; calls fence.wait()

    // Consume on completion. Asserts is_ready(). After this the handle is empty.
    [[nodiscard]] GpuMesh    take_mesh()    noexcept;
    [[nodiscard]] GpuTexture take_texture() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    friend class GpuUploader;
};

class GpuUploader {
public:
    // Existing synchronous helpers — unchanged contract.
    [[nodiscard]] static GpuTexture upload_texture(const TextureResource&, crd::rhi::Device&);
    [[nodiscard]] static GpuMesh    upload_mesh   (const MeshResource&,    crd::rhi::Device&);

    // New: record commands + submit without wait + return a handle.
    // Caller polls handle.is_ready() per frame and consumes via take_*().
    [[nodiscard]] static UploadHandle upload_texture_async(const TextureResource&, crd::rhi::Device&);
    [[nodiscard]] static UploadHandle upload_mesh_async   (const MeshResource&,    crd::rhi::Device&);
};

} // namespace crd::renderer
```

### Layer 3 — `crd-renderer`: `PendingMeshUpload` component + `RenderUploadSystem`

```cpp
// engine/renderer/include/crd/renderer/components.hpp (new)
namespace crd::renderer {

// Attached to an entity alongside Renderable while a GPU upload is in flight.
// Cleared by RenderUploadSystem when the upload completes; at the same instant
// the system writes the resolved buffer pointers into the Renderable and calls
// world.async_aware().mark_loaded(entity, ComponentTypeTag<Renderable>).
struct PendingMeshUpload {
    UploadHandle handle;
};

} // namespace crd::renderer
```

```cpp
// engine/renderer/src/render_upload_system.cpp (new)
class RenderUploadSystem : public crd::scene::ISystem {
public:
    using Reads  = crd::scene::ComponentSet<>;
    using Writes = crd::scene::ComponentSet<Renderable, PendingMeshUpload>;

    crd::scene::SchedulePhase phase() const override {
        return crd::scene::SchedulePhase::RenderExtract;
    }
    crd::containers::StringView name() const override { return "RenderUploadSystem"; }

    void run(crd::scene::World& w) override {
        const auto cid_renderable = w.component_id<Renderable>();
        for (auto&& [e, r, p] : w.query<Renderable, PendingMeshUpload>()) {
            if (!p.handle.is_valid() || !p.handle.is_ready()) continue;
            auto gpu = p.handle.take_mesh();           // moves out, handle now empty
            r.vertex_buffer = gpu.vertex_buffer.get(); // populate the Renderable
            r.index_buffer  = gpu.index_buffer.get();
            // ownership of the buffers transfers to a side store the renderer
            // owns (e.g. an Array<GpuMesh> keyed by entity); details internal.
            w.commands().remove<PendingMeshUpload>(e);
            w.async_aware().mark_loaded(e, cid_renderable);
        }
    }
};
```

The system is registered once at `IRenderPath` startup. Every `Renderable` that was inserted from an async upload follows the same lifecycle: insert → `Loading` → poll → `Loaded`. Existing renderer code doing `world.query<Transform, Renderable>().skip_pending<Renderable>()` automatically excludes pending entities and starts including them on the very next frame after upload.

### Caller pattern (sandbox v1o)

```cpp
// On asset-pick click:
auto cpu_handle = m_resource_mgr->load_async<MeshResource>(uuid);
// ... per-frame poll until cpu_handle.is_ready() ...
// On CPU ready:
auto upload = crd::renderer::GpuUploader::upload_mesh_async(*cpu_handle.get(), m_device);
EntityId e = w.spawn();
w.add_component<Transform>(e, Transform{});
w.add_component<Renderable>(e, Renderable{ /* shader, material; buffers null until ready */ });
w.add_component<PendingMeshUpload>(e, PendingMeshUpload{ std::move(upload) });
// async_aware automatically marks Renderable as Loading on insert (AsyncAware{} trait).
// RenderUploadSystem flips Renderable to Loaded when fence signals; Renderable is rendered next frame.
```

## Implementation slicing (v1o)

The async upload contract becomes the first half of v1o:

| Sub-slice | What | Size |
|---|---|---|
| **v1o1** | `crd::rhi::Fence` + Vulkan impl + `Queue::submit(cmd, fence)` non-waiting variant | ~150 LOC, 4 tests |
| **v1o2** | `UploadHandle` + `GpuUploader::upload_*_async` + `PendingMeshUpload` + `RenderUploadSystem` | ~250 LOC, 6 tests |
| **v1o3** | Sandbox integration — replace synchronous upload with async path; profile picker; quality slider; override window; `default.profile.toml` | ~400 LOC |

v1o1+v1o2 are independent of Preset/Profile work — they could in principle land before v1n. Pinned to v1o because:
1. Sandbox is the first real consumer; designing without one bakes in a single-callsite assumption (already a closed call against v1m's principles).
2. Phase 3.0 v1n (Preset+Profile) needs no GPU upload changes; locking the contract early but implementing it at v1o keeps dependencies clean.

## Consequences

**Positive:**
- Single contract used by every async-uploadable resource (mesh now, texture in v1o2 if needed, audio buffer when `crd-audio` lands, BVH/AS in Phase 5 RT).
- No scene-module GPU dependency; clean module DAG.
- `skip_pending<Renderable>()` becomes the canonical "is it on the GPU yet?" query — works for any asset class as long as the producing module marks the load state.
- Frame-graph-friendly: the renderer's existing `RenderExtract` phase is the natural integration point.

**Negative:**
- Two paths (sync + async). Sync is kept because some smokes / tests need immediate readiness. Documented at the API.
- Each consumer module repeats a ~30-line polling system. Acceptable: each module gets to choose phase + filter + state-transition policy. A generic templated system could be added later but would reduce per-module clarity.
- `UploadHandle` is move-only and not safe to share across threads. Polling system runs on the schedule's RenderExtract thread (single-threaded today). When auto-parallel scheduling lands (Phase 3.5+), the `Writes<Renderable, PendingMeshUpload>` declaration prevents conflicts.

**Reserved for follow-up (not blocking v1o):**
- `crd::rhi::Device::transfer_queue()` — opportunistic dedicated transfer queue (Vulkan: a separate queue family with `VK_QUEUE_TRANSFER_BIT`); falls back to graphics queue when absent.
- Timeline semaphores — replace binary fences when a consumer needs multi-step ordering or batched waits.
- Streaming budget — at most N concurrent uploads; queue the rest. Phase 3.5+ when terrain/LOD streaming arrives.
- Texture upload async path — same machinery, sibling `PendingTextureUpload` component. Lands when a texture-streaming consumer materialises (probably Phase 3.5 IBL or Phase 3.8 GPU-driven rendering).

## References

- `docs/debt.md` → "Async GPU upload (`GpuUploader`)" — original debt entry, this ADR closes the design half.
- `engine/renderer/src/gpu_uploader.cpp` — current synchronous implementation; v1o1+v1o2 add async siblings.
- `engine/scene/include/crd/scene/async_aware_index.hpp` — consumer-facing index this contract feeds into.
- `engine/rhi/include/crd/rhi/queue.hpp` — current `submit_and_wait` and per-frame `submit` paths; v1o1 adds the third variant.
- `docs/phases/phase-3.0-scene-ecs.md` — Pulled-forward prerequisites section now points here.
- ADR-0042 — texture cooked format + GPU upload strategy (synchronous variant).
