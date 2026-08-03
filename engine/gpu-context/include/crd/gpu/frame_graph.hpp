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
    DepthRead, // REN-40-G3: graph ordering is READ (no backward edges), but layout is DEPTH_ATTACHMENT
};

// Which queue a pass records onto. Raster/Present = graphics; Compute = the async-compute queue when the
// graph proves its access is independent of the graphics work it overlaps (else it serializes on graphics).
enum class FgPassKind : crd::u8
{
    Raster = 0,
    Compute,
    Present,
    // ⭐ REN-38-A6: a pass that MOVES pixels (copy / blit / resolve) rather than rendering them. Appended at the
    // END of the enum (a renumbered kind silently reclassifies every serialized graph).
    //
    // ⛔ It is a distinct KIND rather than an ordinary raster pass because the barrier scheduler picks the
    // layout from it: a transfer pass's writes go to TRANSFER_DST and its reads to TRANSFER_SRC, where a raster
    // pass would have used COLOR_ATTACHMENT and SHADER_READ_ONLY. Recording a copy inside a pass declared Raster
    // would hand `vkCmdCopyImage` an image in the wrong layout — undefined contents, not a validation error, on
    // the backends that do not check.
    Transfer,
};

// ⭐ REN-38-A14: which QUEUE a pass asks for.
// ⛔ `Async` is a REQUEST, not a guarantee. A pass only reaches the compute queue when the adapter HAS a distinct
// compute family AND the pass consumes nothing a graphics pass produces — otherwise honouring it would need a
// queue-ownership transfer per resource, and getting that subtly wrong is a corruption bug that reproduces once a
// week. When the graph declines, it SAYS SO through `last_async_submit_count()`: an unhonoured async request that
// reported success would be a performance claim the hardware never delivered.
enum class FgQueue : crd::u8 { Graphics = 0, Async };

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
    // ── ⭐ REN-38-B7: THE REST OF THE VOCABULARY. Appended at the END (a renumbered format silently
    // reinterprets every already-cooked graph). Each one below blocks a NAMED technique by its absence. ──
    RG16F,      // ⛔ MOTION VECTORS — without this TAA (38-G2) is not authorable at all. Also SSR ray data.
    RG32F,      // high-precision motion / VSM-EVSM variance (moment pairs) / the split-BRDF LUT
    RGBA32F,    // ReSTIR reservoirs · path-trace accumulation · NRC weights — anywhere f16 would drift
    R11G11B10F, // the STANDARD HDR light buffer: same dynamic range as RGBA16F at HALF the bandwidth, no alpha
    RGB10A2,    // G-buffer NORMAL packing — 10 bits/axis is the accepted floor before banding shows on smooth shading
    R8,         // SSAO · coverage · single-channel masks
    RG8,        // two-channel masks · packed octahedral normals at low precision
    RGBA16Unorm,// high-precision UNORM (displacement, height, packed velocity) without float cost
    // ⛔⛔ STENCIL. There was NO stencil format at all, so decals (38-E4), portals, outlines and masked lighting
    // were not "imprecise" — they were INEXPRESSIBLE. D24S8 is the universally-supported pair; D32FloatS8 is the
    // high-precision one, and an adapter without it must be told so rather than silently given D24S8 (a depth
    // format substituted underneath an author is a precision change they never see and cannot debug).
    D24S8,
    D32FloatS8,
};

// REN-38-B7: does this format carry a STENCIL aspect? Asked in one place so no backend has to re-derive it.
[[nodiscard]] constexpr bool fg_format_has_stencil(FgImageFormat f) noexcept
{
    return f == FgImageFormat::D24S8 || f == FgImageFormat::D32FloatS8;
}

// REN-38-B7: does it carry a DEPTH aspect? ⛔ Every depth format, not just `D32Float` — the aspect mask, the
// attachment slot and the sampler choice all key on this, and a stencil format missed here would be bound as a
// COLOUR attachment, which is not an error on either API, just a wrong image.
[[nodiscard]] constexpr bool fg_format_has_depth(FgImageFormat f) noexcept
{
    return f == FgImageFormat::D32Float || f == FgImageFormat::D24S8 || f == FgImageFormat::D32FloatS8;
}

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
// ⭐ REN-38-B2: what SHAPE a transient is. ⛔ `layers > 1` alone cannot express this: a CUBE is six layers the
// hardware samples by a DIRECTION, and a 3D volume is a genuinely different resource type whose slices are
// interpolated between. Treating a cube as a 2D array is not a naming quibble — `textureLod(cube, dir)` and
// `texture(array, vec3(uv, layer))` are different instructions, and an env prefilter written against one and
// given the other reads the wrong texel with no error anywhere.
enum class FgImageKind : crd::u8
{
    Tex2D = 0,
    Tex3D,      // froxel/clustered light volumes · volumetric fog · DDGI probe volumes · the 3D scattering LUT
    Cube,       // env prefilter · point-light shadows · reflection probes — sampled by a direction
    CubeArray,  // N probes at once
};

struct FgImageDesc
{
    crd::u32      width   = 0;
    crd::u32      height  = 0;
    FgImageFormat format  = FgImageFormat::RGBA8Unorm;
    crd::u32      samples = 1;
    bool          sampled = false;
    bool          storage = false;
    crd::u32      layers  = 1; // REN-3.2: >1 ⇒ 2D array (CSM cascades / cube faces / stereo). Appended at END.
    // ── ⭐ REN-38-B2. Appended at the END (a reordered field silently reinterprets every cooked graph). ──
    FgImageKind   kind    = FgImageKind::Tex2D;
    crd::u32      depth   = 1; // Tex3D only — the slice count
    // ⭐⭐ 38-G1: a COLOUR transient that a geometry pass renders into needs its own DEPTH ATTACHMENT. The
    // graph creates a companion depth image and hands it to the target's depth slot, so an intermediate scene
    // buffer is depth-tested exactly like the swapchain canvas. Without it the pass draws with no depth test.
    bool          depth_buffer = false;
    // ⛔ A MIP CHAIN, not a mip COUNT that someone remembers to honour. Bloom's down/up chain, SSR's hi-Z and the
    // prefiltered environment map are all "one resource, N levels, each level written by one pass and read by the
    // next" — without this they must be N SEPARATE resources, which defeats the aliaser and makes the chain's
    // length an asset-authoring chore instead of a number.
    crd::u32      mips    = 1;
    // ── ⭐ REN-38-B6: EXCLUDE this transient from memory aliasing. ──
    // ⛔ The graph aliases by LIFETIME, and a lifetime is derived from the reads and writes an asset DECLARES. When
    // a pass touches a resource it did not declare — a debug overlay reading a buffer out-of-band, a tool
    // capturing an intermediate — the derivation is right and the reality is not, and the symptom is another
    // transient's pixels appearing inside this one. An author who knows that needs a way to say so, and the
    // alternative (turning aliasing off globally) trades one correct frame for the whole memory win.
    bool          no_alias = false;
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

    // ── ⭐ REN-40-G3: a render target with EXTERNAL depth — appended at the END (D135). ──
    // Returns a target whose COLOUR comes from `colour` and DEPTH from `depth` (a D32Float transient declared
    // as `shared_depth` in the frame asset). The graph creates it on demand. Null when either handle is invalid.
    // Default: falls back to `image(colour)`, so every existing host compiles unchanged.
    [[nodiscard]] virtual IRasterTarget* image_with_depth(FgImage colour, FgImage depth) noexcept
    {
        (void)depth;
        return image(colour);
    }
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
    virtual IFramePassBuilder& reads_depth(FgImage handle)  = 0;

    // Attach the recording callback. `user` is passed back to `fn` verbatim.
    virtual IFramePassBuilder& execute(FgExecuteFn fn, void* user) = 0;

    // A Present pass: after its declared reads, blit `surface`'s target into the backbuffer + present — the
    // terminal node. Mutually exclusive with execute() on the same pass.
    virtual IFramePassBuilder& present(IPresentSurface& surface) = 0;

    // REN-38-A14: ask for the async-compute queue. Ignored for a raster pass (the cooker rejects that shape) and
    // for a pass the graph declines to move — see `last_async_pass_count()`.
    virtual IFramePassBuilder& queue(FgQueue /*q*/) { return *this; }
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

    // ── ⭐ REN-38-B5: IMPORT AN APP-OWNED TEXTURE. Appended at the END of the vtable (D135). ──
    // A UI atlas, a video frame, a captured HDR, a baked LUT — content the graph READS and never produces. ⛔ It
    // is NOT `import_target`: a render target is something a pass can WRITE, and letting a UI atlas be written
    // would put the graph in charge of content the application owns and updates on its own schedule. Read-only is
    // the whole distinction, so it gets its own verb rather than a flag on the existing one.
    //
    // The graph tracks it for ORDERING only — there is no barrier to derive, because nothing in the frame writes
    // it. Returning an invalid handle on a backend without support is the honest answer; the default does.
    [[nodiscard]] virtual FgImage import_texture(class ITexture& /*texture*/) { return FgImage{0U}; }

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

    // ── REN-38-A5: THE PRESENT PASS. Appended at the END of the vtable (D135). ──
    // How many present passes actually handed a target to a surface during the last `execute()`.
    //
    // ⛔ This counter exists because `.present(surface)` was ACCEPTED AND THEN IGNORED: the builder stored the
    // surface, the barrier scheduler read the field to skip a transition, and NOTHING EVER CALLED
    // `IPresentSurface::present`. A graph declaring a present pass built, executed and reported one submission
    // — the same silent shape `FramePassKind::Compute` had. A gate that only checked `build()` and
    // `last_submit_count()` passed throughout.
    //
    // So the fact is COUNTED, and a present gate asserts on the count rather than on the absence of an error:
    // "the frame presented" is a claim that must be checkable, not inferred from nothing having gone wrong.
    [[nodiscard]] virtual crd::u32 last_present_count() const noexcept { return 0U; }

    // ── ⭐ REN-38-A14: ASYNC COMPUTE. Appended at the END of the vtable (D135). ──
    // How many passes the last `execute()` actually ran on the ASYNC COMPUTE QUEUE. 0 means everything ran on the
    // graphics queue — either because nothing asked, or because the graph DECLINED (one queue family, or the pass
    // consumes graphics output). ⛔ Counted rather than assumed, for the same reason `last_present_count` is: a
    // declaration the device quietly ignored is a lie the frame cannot expose on its own.
    [[nodiscard]] virtual crd::u32 last_async_pass_count() const noexcept { return 0U; }

    // ── ⭐ REN-38-B6: A HARD MEMORY BUDGET for graph-owned transients. Appended at the END (D135). ──
    // `build()` FAILS when the post-aliasing footprint exceeds `bytes`; 0 (the default) means unbounded, so every
    // existing graph is unchanged. ⛔ A budget that only WARNED would be useless: the failure it prevents is an
    // allocation that succeeds on the development machine and OOMs on the target, months later, in a build nobody
    // can bisect. Failing at BUILD names the graph while the author is holding it.
    virtual void set_memory_budget(crd::u64 /*bytes*/) {}

    // Did the last `build()` fail because of that budget (rather than for any other reason)? ⛔ Reported
    // separately because "too big" and "malformed" need different fixes, and a single false cannot say which.
    [[nodiscard]] virtual bool last_build_exceeded_budget() const noexcept { return false; }
};

} // namespace crd::gpu
