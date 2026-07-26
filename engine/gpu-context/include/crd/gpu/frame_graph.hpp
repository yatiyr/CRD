#pragma once

// frame_graph.hpp — REN-1 (D-007 row 98): the FRAME GRAPH on gpu-context — typed resource handles, a pass
// DAG with declared reads/writes, automatic barriers, ASYNC single-submission, AND graph-owned TRANSIENT
// resources with lifetime analysis + memory ALIASING. Replaces the synchronous submit+wait+readback-PER-DRAW
// substrate every `draw_*` uses today. Ports the retired rhi/renderer v1 frame-graph design (ADR-0032 — the
// DESIGN REFERENCE) onto the ONE graphics layer (ADR-0105). Backends implement; consumers depend on THIS.
//
// ── the load-bearing idea: the frame graph is a RECORDING MODE of the raster context ────────────────────────────
// A pass's execute callback records draws through the SAME `IRasterContext` it always used — but while a graph
// is executing, `draw_storage_depth` / `draw_overlay` / … RECORD into the frame's one shared command buffer
// instead of each doing its own submit+wait+readback. So passes reuse the entire tested draw vocabulary; the
// graph is the thin scheduler that orders passes by declared access, inserts barriers, and submits ONCE.
//
// ── transient resources + aliasing ──────────────────────────────────────────────────────────────────────────────
// The graph OWNS transients (`create_transient_image`/`create_transient_buffer`) — used as an attachment in one
// pass, sampled in a later pass, then dead. build() computes each transient's LIFETIME [first-write pass ..
// last-read pass] and ALIASES the backing memory of transients whose lifetimes are DISJOINT (a shadow map and
// a bloom mip that never coexist share bytes), inserting an aliasing barrier where a physical slot is reused.
// Existing resources join the graph via `import_*` (identity — not aliased).
//
// ── frame loop ──────────────────────────────────────────────────────────────────────────────────────────────────
//   auto fg = raster->create_frame_graph();
//   const FgImage  scene = fg->import_target(*color_depth_target);
//   const FgBuffer geom  = fg->import_storage(*group_buffer);
//   fg->add_pass("scene").reads(geom).writes(scene).execute(&record_scene, &state);
//   fg->add_pass("overlay").read_writes(scene).execute(&record_overlay, &state);
//   fg->add_pass("present", FgPassKind::Present).reads(scene).present(*surface);
//   if (fg->build()) { fg->execute(); }   // topo-order + barriers + transient aliasing + ONE submission
//
// REN-1 lands the graphics-queue path + transient aliasing + the async-compute-queue SEAM (FgPassKind::Compute
// schedules onto the compute queue when independent). Direct-to-backbuffer present is REN-8. The gate: the
// SceneRenderer composes N mesh groups + the overlay in ONE submission, transient aliasing proven, validation-
// silent, and a multi-pass readback BIT-IDENTICAL to the synchronous path.

#include <crd/core/types.hpp>

namespace crd::gpu
{

class IRasterContext;
class IRasterTarget;
class IStorageBuffer;
class IPresentSurface;
class ITexture;

// Typed, opaque handles into ONE frame graph (invalid across graphs / after reset). id 0 = invalid. Structs
// (not enums) so the reads()/writes() overloads resolve by type without an enum-size penalty.
struct FgImage
{
    crd::u32 id = 0;
    [[nodiscard]] constexpr bool valid() const noexcept { return id != 0; }
    [[nodiscard]] constexpr bool operator==(const FgImage& o) const noexcept { return id == o.id; }
};
struct FgBuffer
{
    crd::u32 id = 0;
    [[nodiscard]] constexpr bool valid() const noexcept { return id != 0; }
    [[nodiscard]] constexpr bool operator==(const FgBuffer& o) const noexcept { return id == o.id; }
};

// How a pass touches a resource — drives BOTH ordering (a reader waits for its writer) AND the barrier the
// graph inserts before the pass (the access/layout transition).
enum class FgAccess : crd::u8
{
    Read = 0,  // sampled / vertex-pulled / read-only attachment
    Write,     // clears + writes a colour/depth attachment or a storage buffer (no prior contents needed)
    ReadWrite, // loads-modifies-stores — a depth-LOAD scene draw, an overlay composite, a compute RMW
};

// Which queue a pass records onto. Raster/Present = graphics; Compute = the async-compute queue when the
// graph proves its access is independent of the graphics work it overlaps (else it serializes on graphics).
enum class FgPassKind : crd::u8
{
    Raster = 0,
    Compute,
    Present,
};

// The pixel format of a transient image.
enum class FgImageFormat : crd::u8
{
    RGBA8Unorm = 0,
    RGBA8Srgb,
    RGBA16F, // WBOIT accum / HDR
    R16F,    // revealage
    R32F,    // depth-as-colour / distance
    R32Uint, // visibility / id buffer
    D32Float, // depth attachment
};

// The most array layers one transient may declare. A cap, and therefore stated rather than hidden: 4 CSM
// cascades, 6 cube faces, 2 stereo views, 8 point-light faces-per-atlas all fit; `create_transient_image`
// returns an INVALID handle above it (a checkable rejection, never a truncated allocation), and the asset
// validator rejects it by name (`FrameCookError::LayersOutOfRange`) before it can ever reach a device.
inline constexpr crd::u32 kFgMaxImageLayers = 16;

// A transient image the graph creates + owns + aliases. `sampled` adds SAMPLED usage (a later pass reads it
// as a texture via IFrameContext::texture); `storage` adds STORAGE usage (a compute pass writes it).
//
// REN-3.2: `layers > 1` makes it a 2D **ARRAY** — one image, N slices. A pass renders into ONE slice via
// `IFrameContext::image_layer(handle, layer)`; a later pass samples the WHOLE array through `texture(handle)`
// (a 2D-array view) and selects the slice in the shader. That is the CSM cascade atlas: N depth slices written
// by N depth-only passes, then one lighting pass picking a cascade per pixel. Cube faces and stereo views are
// the same shape. The graph tracks a layered image as ONE node, so its barriers cover every slice at once
// (VK_REMAINING_ARRAY_LAYERS / a whole-resource DX12 transition) — conservative and always correct, and the
// per-slice writes are already ordered by the declared DAG.
struct FgImageDesc
{
    crd::u32      width   = 0;
    crd::u32      height  = 0;
    FgImageFormat format  = FgImageFormat::RGBA8Unorm;
    crd::u32      samples = 1;
    bool          sampled = false;
    bool          storage = false;
    crd::u32      layers  = 1; // REN-3.2: >1 ⇒ 2D array (CSM cascades / cube faces / stereo). Appended at END.
};

// The recording surface handed to a pass's execute callback. `raster()` is the raster context IN FRAME
// RECORDING MODE — its `draw_*` calls record into the frame's one command buffer. `image()` resolves a handle
// to its concrete render target (imported or transient); `texture()` resolves a `sampled` transient to a
// texture view for sampling; `buffer()` resolves a storage handle.
class IFrameContext
{
public:
    IFrameContext()                                = default;
    virtual ~IFrameContext()                       = default;
    IFrameContext(const IFrameContext&)            = delete;
    IFrameContext& operator=(const IFrameContext&) = delete;
    IFrameContext(IFrameContext&&)                 = delete;
    IFrameContext& operator=(IFrameContext&&)      = delete;

    [[nodiscard]] virtual IRasterContext& raster() noexcept                 = 0;
    [[nodiscard]] virtual IRasterTarget*  image(FgImage handle) noexcept    = 0;
    [[nodiscard]] virtual ITexture*       texture(FgImage handle) noexcept  = 0;
    [[nodiscard]] virtual IStorageBuffer* buffer(FgBuffer handle) noexcept  = 0;

    // REN-3.2: resolve ONE slice of a layered transient as a render target — the per-cascade shadow write.
    // `layer == 0` on a non-layered image is exactly `image(handle)`; out of range returns nullptr. Appended at
    // the END of the vtable (the D135 scar: inserting mid-vtable mis-dispatches under win-release LTCG).
    [[nodiscard]] virtual IRasterTarget* image_layer(FgImage handle, crd::u32 layer) noexcept = 0;
};

// A pass records via a C function + user pointer (the gpu-context callback idiom — no std::function heap
// churn; a per-frame handful of passes, never a hot path). The user pointer carries the pass's state, so a
// single execute records an arbitrary loop of draws.
using FgExecuteFn = void (*)(IFrameContext& ctx, void* user);

// Declares one pass's resource access + its recording callback. Fluent; every setter returns *this.
class IFramePassBuilder
{
public:
    IFramePassBuilder()                                     = default;
    virtual ~IFramePassBuilder()                            = default;
    IFramePassBuilder(const IFramePassBuilder&)            = delete;
    IFramePassBuilder& operator=(const IFramePassBuilder&) = delete;
    IFramePassBuilder(IFramePassBuilder&&)                 = delete;
    IFramePassBuilder& operator=(IFramePassBuilder&&)      = delete;

    virtual IFramePassBuilder& reads(FgImage handle)        = 0;
    virtual IFramePassBuilder& reads(FgBuffer handle)       = 0;
    virtual IFramePassBuilder& writes(FgImage handle)       = 0;
    virtual IFramePassBuilder& writes(FgBuffer handle)      = 0;
    virtual IFramePassBuilder& read_writes(FgImage handle)  = 0;
    virtual IFramePassBuilder& read_writes(FgBuffer handle) = 0;

    // Attach the recording callback. `user` is passed back to `fn` verbatim.
    virtual IFramePassBuilder& execute(FgExecuteFn fn, void* user) = 0;

    // A Present pass: after its declared reads, blit `surface`'s target into the backbuffer + present — the
    // terminal node. Mutually exclusive with execute() on the same pass.
    virtual IFramePassBuilder& present(IPresentSurface& surface) = 0;
};

// One frame's graph. import/create → add_pass(access + execute) → build → execute (ONE submission). Reusable
// across frames via reset(). Obtained from `IRasterContext::create_frame_graph`.
class IFrameGraph
{
public:
    IFrameGraph()                              = default;
    virtual ~IFrameGraph()                     = default;
    IFrameGraph(const IFrameGraph&)            = delete;
    IFrameGraph& operator=(const IFrameGraph&) = delete;
    IFrameGraph(IFrameGraph&&)                 = delete;
    IFrameGraph& operator=(IFrameGraph&&)      = delete;

    // Import an existing resource as a tracked node (its live access state is threaded from here; NOT aliased).
    // A resource imported twice returns the same handle. Handles are stable until reset().
    [[nodiscard]] virtual FgImage  import_target(IRasterTarget& target)   = 0;
    [[nodiscard]] virtual FgBuffer import_storage(IStorageBuffer& buffer) = 0;

    // Declare a TRANSIENT the graph owns + aliases (its memory may be shared with another disjoint-lifetime
    // transient). Invalid handle on an unsupported format/desc. `texture()` resolves it only if `sampled`.
    [[nodiscard]] virtual FgImage  create_transient_image(const FgImageDesc& desc) = 0;
    [[nodiscard]] virtual FgBuffer create_transient_buffer(crd::u32 size_bytes)    = 0;

    // Declare a pass; the returned builder attaches reads/writes/execute (borrowed — valid until reset()).
    [[nodiscard]] virtual IFramePassBuilder& add_pass(const char* name, FgPassKind kind = FgPassKind::Raster) = 0;

    // Compile: topo-sort passes by declared read/write dependencies, run transient lifetime analysis +
    // aliasing, and compute the barrier schedule. Returns false on a dependency CYCLE, an unresolved handle,
    // or a transient that no pass writes (never a partial schedule).
    [[nodiscard]] virtual bool build() = 0;

    // Record every pass into ONE command buffer in dependency order (inserting the scheduled barriers, incl.
    // aliasing barriers), then submit ONCE and wait (REN-1 keeps the wait; async-across-frames + direct
    // present land at REN-8). Recorded draws are bit-identical to the sync path's, minus the per-draw submit.
    virtual void execute() = 0;

    // Clear all passes + imports + transients for the next frame (the graph object, its pool, its fence, and
    // its transient memory pool are REUSED — aliasing keeps the pool small).
    virtual void reset() = 0;

    // Diagnostics (the RET pattern — gates assert on counters, never eyeballed logs).
    [[nodiscard]] virtual crd::u32 last_barrier_count() const noexcept  = 0; // barriers the last build scheduled
    [[nodiscard]] virtual crd::u32 last_submit_count() const noexcept   = 0; // submits the last execute did (==1)
    [[nodiscard]] virtual crd::u32 transient_memory_bytes() const noexcept = 0; // physical bytes after aliasing
    [[nodiscard]] virtual crd::u32 transient_logical_bytes() const noexcept = 0; // bytes WITHOUT aliasing (≥ physical)

    // ── REN-8: GPU TIMING. Appended at the END of the vtable (D135). ──
    // Per-pass GPU cost, from device TIMESTAMP queries written around each pass in the frame's one command
    // buffer and read back after the submit's fence. Valid only after `execute()`; zero before it.
    //
    // ⛔ Why this exists before any optimization: the sandbox costs ~12 ms/frame that neither full optimization
    // (release ≈ debug) nor removing the vsync cap (immediate ≈ fifo) moves. Those two measurements ELIMINATE
    // cpu-bound and vsync-bound, but they cannot say where the time goes — nothing in the engine could time a
    // GPU pass. Optimizing before this lands would be guesswork, and the honest order is measure → attribute →
    // fix. `gpu_ms_total()` vs the CPU wall-clock of `execute()` is the specific question: if the GPU is idle
    // for most of the frame, the cost is the submit-and-WAIT that REN-1 deliberately kept, not the rendering.
    //
    // ⛔ Timestamps are only comparable WITHIN one submission (the SAME-PASS timing doctrine) — across submits
    // the queue can idle between them and the delta measures wall-clock, not work.
    [[nodiscard]] virtual crd::u32 pass_count() const noexcept { return 0; }        // passes the last execute ran
    [[nodiscard]] virtual const char* pass_name(crd::u32 /*i*/) const noexcept { return nullptr; }
    [[nodiscard]] virtual double pass_gpu_ms(crd::u32 /*i*/) const noexcept { return 0.0; } // one pass's GPU time
    [[nodiscard]] virtual double gpu_ms_total() const noexcept { return 0.0; }      // first pass start → last end
    [[nodiscard]] virtual bool   gpu_timing_available() const noexcept { return false; } // device supports it

    // REN-8: does `execute()` copy every imported colour target back to host-visible memory at end of frame?
    // ⛔ This copy exists ONLY so `read_pixel` stays bit-identical to the synchronous path — it is a TEST
    // affordance. A presenting application never reads those pixels back, yet it paid for a full-target
    // host copy every single frame: at 1280x720 RGBA8 that is ~3.7 MB over PCIe per frame, landing AFTER the
    // last timed pass (so per-pass timestamps cannot see it) and stalling the fence wait that follows.
    // Measured in the sandbox: 1.8 ms of actual pass work sat behind a 7.1 ms stall.
    // Default TRUE so every existing gate keeps its readback semantics unchanged; a real-time consumer opts out.
    virtual void set_readback_enabled(bool /*on*/) noexcept {}

    // ── REN-37.5: PERSISTENT resources. Appended at the END of the vtable (D135). ──
    // Everything the graph owns today is a TRANSIENT whose entire purpose is to be aliased away and destroyed at
    // `reset()`. A whole class of technique needs the opposite: TAA history, SSR / DDGI / ReSTIR temporal reuse,
    // auto-exposure adaptation, and (REN-37.9) a cached viewport thumbnail all need data that SURVIVES the frame.
    //
    // `key` is a caller-chosen STABLE IDENTITY (the authored resource's name hash). Calling this with the same
    // key and the same desc across frames returns a handle to the SAME image, contents and layout intact. A desc
    // that no longer matches (a resize) recreates it — the history is genuinely invalid then, so silently reusing
    // a differently-sized image would be worse than losing it.
    //
    // ⛔ A persistent image is EXCLUDED from transient aliasing and from the retire queue, and that exclusion is
    // the whole point: `retire_transients_to` frees graph-owned images once their fence signals, which is exactly
    // wrong for a resource whose VALUE IS ITS HISTORY. It is otherwise an ordinary tracked node — barriers,
    // ordering and layer slices all work identically.
    //
    // PING-PONG is built ON this rather than beside it: two persistent images under two keys, with the executor
    // swapping which is `$prev` and which is `$curr` each frame. No author ever hand-manages a frame-parity bit
    // (the classic source of one-frame-stale bugs).
    //
    // Returns an invalid handle on an unsupported desc or on a backend without support.
    [[nodiscard]] virtual FgImage create_persistent_image(crd::u32 /*key*/, const FgImageDesc& /*desc*/)
    {
        return FgImage{0U};
    }

    // Was `key`'s image ALREADY LIVE when `create_persistent_image` was called this frame — i.e. does it carry
    // real history, or was it created (or recreated after a resize) just now? A temporal pass MUST branch on
    // this: reprojecting into an uninitialized history buffer is how TAA ghosts garbage on frame 0 and after
    // every resize. Reported rather than inferred, because "is this the first frame" is not something a shader
    // can work out for itself.
    [[nodiscard]] virtual bool persistent_image_was_live(crd::u32 /*key*/) const noexcept { return false; }
};

} // namespace crd::gpu
