#pragma once

// scene_renderer.hpp — GEO-7 (D-007 row 72): scene ↔ ECS ↔ renderer INTEGRATION on the ONE graphics layer
// (ADR-0105). The world-class ECS earns its keep: extraction, change tracking, culling, and submission all walk
// ARCHETYPE CHUNKS (SoA) through the GEO-7 ChunkView table — never per-entity handle chasing on the render path.
//
// The pipeline, per frame:
//   sync(world)    — chunk-grain extract of (Transform, MeshRenderer): a structural change (spawn/despawn/archetype
//                    move) rebuilds the instance tables; otherwise ONLY chunks whose Transform chunk-version moved
//                    re-extract, and ONLY their byte ranges re-upload (the change-detection partial re-upload gate).
//   render(target) — frustum culling (6 world-space planes from view_proj; an optional SpatialBVHIndex prunes the
//                    broad phase — the crd-geometry LooseOctree under ADR-0053's index slot) → per mesh group ONE
//                    vertex-pulling instanced draw (`draw_storage_depth`): the CKIR VS fetches index → vertex →
//                    instance matrix from the group's storage buffer by VertexIndex (the GEO-1 idiom, scaled to
//                    instancing), the FS lights N·L + ambient with the instance's material colour.
//
// Data contract per mesh group — ONE u32 storage buffer at set 0 / binding 0 (VERTEX+FRAGMENT visible):
//   words [0..31]  HEADER: [0] index_count · [1] visible_count · [2] indices_off · [3] vertices_off ·
//                  [4] instances_off · [5] visible_off (all in words) · [6..21] view_proj (column-major f32 bits) ·
//                  [22..24] light_dir · [25..31] reserved
//   [indices_off..)   u32 indices
//   [vertices_off..)  12 words per vertex (the cooked 48-byte layout: pos3 · normal3 · uv2 · tangent4)
//   [instances_off..) 20 words per instance: world matrix (column-major, 16) + linear colour (4)
//   [visible_off..)   u32 visible slots (per-frame, culled)
// Geometry uploads ONCE per group; instances upload by dirty run; header + visible list upload per frame.

#include <crd/containers/array.hpp>
#include <crd/containers/hash_map.hpp>
#include <crd/core/types.hpp>
#include <crd/geometry/primitives/primitives.hpp>
#include <crd/gpu/frame_graph.hpp> // REN-37.8: contribute() records into a caller-owned graph
#include <crd/gpu/raster_context.hpp>
#include <crd/math/mat.hpp>
#include <crd/math/vec.hpp>
#include <crd/memory/allocator.hpp>
#include <crd/scenerender/csm.hpp> // REN-3.2-b: CsmConfig / CsmCascades in the public API
#include <crd/resources/mesh_resource.hpp>
#include <crd/resources/resource_handle.hpp>
#include <crd/resources/resource_id.hpp>
#include <crd/scene/entity.hpp>

#include <memory>

namespace crd::gpu
{
class IGpuContext;
}
namespace crd::resources
{
class ResourceManager;
}
namespace crd::scene
{
class World;
class SpatialBVHIndex;
}

namespace crd::scenerender
{

// The per-draw header the pull shaders read from word 0 of the group's storage buffer.
//   [0..5]   counts + section offsets      [6..21]  camera view_proj      [22..24] light dir
//   [25..27] skin/palette/joint sections
//   [28..31] REN-3.2-b: the four CSM split FAR distances (view space) — the FS selects its cascade from these
//   [32..95] REN-3.2-b: the four cascade light_vp matrices, 16 floats each
// ⛔ Grown at the END (32 -> 96). Every existing word index is unchanged, so no shader needed touching — the
// same append-only discipline the vtables use, for the same reason: a renumbered header would silently feed
// every pull shader the wrong field.
//   [96..98] REN-37.3: the FRAME-frequency CAMERA POSITION (world space) — the real view vector
// ⭐⭐ REN-38-E7: the LIGHT SECTION. The lighting technique is an authored declaration now, and a declaration
// that names a light array needs one to exist — before this the header carried a bare `light_dir` at [22..24]
// and nothing else, which is exactly why every technique body could only ever be one directional light.
// Word [99] (the old pad) holds the light section OFFSET; [100..115] is one 16-word light record.
// ⛔ APPENDED, never renumbered — every existing word index is unchanged, the same discipline the header has
// followed since it grew from 32 to 96. A renumbered header silently feeds every pull shader the wrong field.
inline constexpr crd::u32 kHdrLightOff        = 99U;  // holds the word offset of the light section
inline constexpr crd::u32 kLightSectionWords  = 16U;  // one record: position@0 falloff@3 color@4 direction@8
// ⭐ REN-38-F6+: the header grows 116 → 120 (4-word aligned). The LIGHT SECTION is derived
// (`header[99] = kHeaderWords − 16`), so the record slides to [104..119] and every consumer follows the offset
// word; the freed [100] holds the group's TOTAL INSTANCE COUNT — the GPU cull kernel's range guard (threads
// past the instance section read garbage transforms, and their verdicts polluted the indirect-args atomic).
// [101..103] are the new pad. Every existing index is unchanged — the append-only discipline.
inline constexpr crd::u32 kHdrInstanceCount   = 100U;
// ⭐⭐ 38-G1 perf: the INSTANCE CAPACITY — the stride between the per-cascade visible lists that follow the
// camera's. Cascade c reads its list at `visible_off + (1 + c) * this`. Takes one of the [101..103] pad words,
// so every existing index is unchanged (the append-only discipline this header has always followed).
inline constexpr crd::u32 kHdrInstanceCapacity = 101U;
// ⭐⭐ REN-40-A: the word offset of the per-instance WORLD AABB section (6 floats each) the GPU cull reads.
// Takes another of the [101..103] pad words, so every existing index is unchanged — the append-only discipline
// this header has always followed. ⛔ Declared rather than computed in the kernel: the cull and the CPU's
// `aabb_in_frustum` must read ONE truth, and a shader that derived the offset itself would be a second one.
inline constexpr crd::u32 kHdrBoundsOff       = 102U;
// ⭐⭐ REN-40-F: GPU skinning sections — the compute kernel's per-group inputs.
// Words 103-105, from the [101..119] padding between kHdrBoundsOff and kHdrLodCount.
inline constexpr crd::u32 kHdrSkelOff         = 103U; // skeleton data: parents + IBMs + rest pose
inline constexpr crd::u32 kHdrClipOff         = 104U; // pre-baked clip frames
inline constexpr crd::u32 kHdrAnimStateOff    = 105U; // per-instance (clip_local_off, time) pairs
// ⭐⭐ REN-40-A: the PARAMS BLOCK at the head of a group's `cull_args` buffer, before the indirect commands.
// ⛔⛔ IT EXISTS BECAUSE A CONSOLIDATED GROUP'S HEADER IS NOT AT WORD 0. Under REN-38 scene-buffer consolidation
// a group's region sits at `region_base` and its header offsets are region-RELATIVE — the vertex programs add the
// base from the draw table via DrawIndex. A cull kernel has no DrawIndex, so without this it read GROUP 0's
// header for every group: group 0's bounds offset, group 0's instance count, group 0's visible-list stride. The
// counts came back plausible and WRONG (1918 device vs 1379 CPU on a 2000-instance frame) and no shader failed.
// Word 0 of the params block carries the group's base; the commands start after it.
// ⭐⭐ REN-40-C2: it also carries EACH VIEW'S HEIGHT IN PIXELS (f32 bits), which the LOD selector needs and
// which has nowhere else to live: it is per VIEW, while the scene header is per GROUP and a cook-time constant
// cannot survive a window resize. Word 0 is the base; words [1..1+kMaxCascades] are the camera's and each
// cascade's pixel height; the commands start after the block.
inline constexpr crd::u32 kCullArgsHeaderWords = 8U; // 32 B — keeps the first command 16-byte aligned
inline constexpr crd::u32 kCullArgsPixelHeight = 1U; // + view index (words 1..1+kMaxCascades)
// ⭐⭐ REN-40-C2 / D3D12: THE GROUP'S FIRST DRAW-LIST ROW. D3D12 has no `gl_DrawID`; its command signature
// prepends a DrawIndex root constant, so each command carries its OWN row and the PRODUCER has to write it.
// ⛔ `scene_cull_reset` used a cook-time `draw_index` of 0, which is the right row only for the FIRST group —
// every later group's draws then read group 0's region base and group 0's LOD slot. Vulkan never saw it
// (there the row is a push constant the verb issues), which is the exact shape of a backend-specific silent
// wrong answer this repo has been bitten by before. The host writes the row here, beside the base.
inline constexpr crd::u32 kCullArgsBaseRow    = 6U;
// ⭐⭐ REN-40-C2: THE LOD TABLE, in the header every pull shader already reads.
// ⛔ The cull kernel has to answer "which level, and what are its draw parameters"
// entirely on the device, so the chain has to BE there — not in a side buffer the
// kernel would need a second binding for, and not as a cook-time constant, because
// the chain is a property of the MESH and one kernel serves every mesh group.
// ⛔⛔ APPENDED, and the light section still sits at the END (`header[kHdrLightOff]
// = kHeaderWords - kLightSectionWords`), so this table has to fit BELOW it. Growing
// the header without leaving that room would overwrite the light record with LOD
// entries — the scene would go black and the cause would look like a lighting bug.
inline constexpr crd::u32 kMaxLodSlots      = 8U;
inline constexpr crd::u32 kHdrLodCount      = 120U; // levels in the chain (0 or 1 = no chain)
inline constexpr crd::u32 kHdrLodTable      = 121U; // kMaxLodSlots x (first_index, index_count)
inline constexpr crd::u32 kHdrLodHeight     = 137U; // kMaxLodSlots x screen height, f32 BITS
// ⭐⭐ REN-40-C2: the word offset of the per-instance LOD-OVERRIDE section (2 words each: the screen bias as
// f32 bits, then `min_level | (max_level << 8)`), which the cull kernel reads beside the world AABBs.
// ⛔ A SECTION, not extra words on the instance record: the instance stride is 20 and every `.crdv` DECLARES
// it, so growing it would be a silent disagreement with every vertex program in the pack. Only the CULL needs
// this data, so it lives where the cull's other per-instance input lives.
inline constexpr crd::u32 kHdrLodOverrideOff = 145U;
// ⭐⭐ REN-40-C5: the impostor atlas section — packed RGBA8 texels in the buffer.
inline constexpr crd::u32 kHdrAtlasOff       = 146U;
inline constexpr crd::u32 kHdrAtlasDims      = 147U; // (grid << 16) | tile
// the 16-word light record then occupies 148..163.
inline constexpr crd::u32 kHeaderWords        = 164U;
inline constexpr crd::u32 kHdrCsmSplits       = 28U; // 4 floats
inline constexpr crd::u32 kHdrCsmLightVp      = 32U; // 4 x 16 floats
// ⭐ REN-37.3: the camera position, appended at 96. `shade_forward`'s `view_dir` was a PLACEHOLDER CONSTANT
// (0,1,0) until this landed, which degenerates NoV and with it the Smith visibility term, `env_brdf_approx` and
// the energy compensation — the entire specular chain evaluated against a view vector that had nothing to do
// with the camera.
// ⛔ kHeaderWords is ALSO the first section offset (`group.indices_off = kHeaderWords`), so growing it shifts
// the WHOLE buffer layout. Both consumers derive from the constant (the sizing in `ensure_group_buffer` and the
// per-frame `crd::u32 header[kHeaderWords]` upload), which is why this is a one-line change rather than a hunt —
// but it is exactly the kind of edit that silently corrupts every shader if one consumer hardcodes 96.
inline constexpr crd::u32 kHdrCameraPos       = 96U; // 3 floats (+1 pad, keeping the header 4-word aligned)
inline constexpr crd::u32 kVertexWords        = 12U; // the cooked 48-byte vertex
// ⭐⭐ REN-38 (scene-buffer consolidation): the ONE scene buffer's fixed prefix. Words [0..119] hold THE frame
// header (what the FS reads absolutely — every group's header carries the same frame fields, so one canonical
// copy serves); [120..375] is the DRAW TABLE (one word per draw-list row: that draw's region base); group
// regions start at 384 (4-word aligned), each an exact image of the group's historical private buffer, so the
// region-relative offsets stored in its header keep working under the VS's DrawIndex rebase unchanged.
// ⛔ DERIVED, never repeated. This was the literal 120 while `kHeaderWords` was also 120, so the two agreed
// by coincidence; growing the header for the LOD table would have slid the draw table INTO it and every
// rebased group would have read a LOD entry as its region base.
inline constexpr crd::u32 kSceneDrawTableOff  = kHeaderWords; // the table sits right after the header
// ⭐⭐ REN-40-C2: A DRAW-TABLE ROW IS A RECORD, NOT A WORD. Word 0 stays the region base; word 1 carries the
// draw's LOD SLOT. ⛔⛔ WHY IT HAS TO LIVE HERE: a frame-graph PASS binds ONE program for its whole draw list, so
// the slot cannot be a cook-time constant the way the CASCADE is (each cascade already gets its own pass, hence
// its own program). It must be PER DRAW ITEM, and a GPU-written multi-draw's only per-draw channel is the draw
// index — which both backends already carry and this engine already uses for the region base (`gl_DrawIDARB` on
// Vulkan, a command-signature root constant on D3D12, whose `INCREMENTING_CONSTANT` argument type formalises the
// same idea). This is the standard GPU-driven indirection; frontier engines call the row a "render item" — an
// atomic (mesh x material x LOD) unit, one indirect draw each.
// ⛔ The row stores the SLOT rather than a finished list address because the table is uploaded ONCE and shared by
// every pass, while the address depends on the VIEW. The stage combines a cook-time view with a per-row slot.
inline constexpr crd::u32 kSceneDrawRowWords  = 2U;
inline constexpr crd::u32 kSceneDrawRows      = 256U; // draw-list rows the table can describe
inline constexpr crd::u32 kSceneDrawTableWords = kSceneDrawRows * kSceneDrawRowWords;
// ⭐⭐ REN-40-C5: the IMPOSTOR draw table — separate from the mesh table because the impostor pass has its own
// draw list and therefore its own DrawIndex space (0..M-1). The mesh table cannot serve both: cascade passes use
// `program_is_instance = true` (the shadow VS overrides per-item programs), so impostor items drawn through a
// cascade pass would rasterise an identity IB with a mesh VS — garbage. A dedicated table and draw list is the
// only correct separation. 64 rows is generous (one per group with an impostor slot).
inline constexpr crd::u32 kImpostorTableOff    = kSceneDrawTableOff + kSceneDrawTableWords;
inline constexpr crd::u32 kImpostorDrawRows    = 64U;
inline constexpr crd::u32 kImpostorTableWords  = kImpostorDrawRows * kSceneDrawRowWords;
// ⛔⛔ DERIVED TOO. This was the literal 384 (= the old 120-word header + 256), so growing the header for the LOD
// table slid the draw table forward INTO the first group's region: every rebased group would have read a draw-table
// row as its geometry and drawn noise, and the frame would still have rendered. Three constants, one arithmetic.
// ⚠ A 2026-07-30 revision carried an 8-word "slack" here with a comment claiming that removing it "broke four
// shadow gates". That was a MISATTRIBUTION: those four gates were red for an unrelated reason (the CSM technique
// read a matrix ELEMENT where it needed a row NORM — see SCAR 5 in `ckir_technique.hpp`), and once that was fixed
// at the root the derived base with NO slack passes all 33 scene-render gates. Magic constants justified by a
// correlation nobody re-tested are how a wrong number survives a review, so it is gone rather than preserved.
inline constexpr crd::u32 kSceneFirstRegion   = ((kImpostorTableOff + kImpostorTableWords) + 3U) & ~3U;
// ⭐⭐ REN-40-C2: WHERE A GROUP'S SECTIONS START — after its header AND after its own copy of the draw table.
// ⛔⛔ EVERY buffer carries the table now, at the SAME offset, which is what lets ONE cooked `rebase_table`
// address both the consolidated scene buffer and a private per-group one. The private path had no table at all,
// so a vertex program on it could not read its LOD slot and silently drew level 0's (empty) list — levels 1 and
// coarser rendered NOTHING while every count still reconciled. In a consolidated region this range is unused
// padding: the region is an exact image of the private layout, which is precisely why the rebase works.
inline constexpr crd::u32 kGroupSectionsOff   = kSceneFirstRegion;
inline constexpr crd::u32 kInstanceWords      = 20U; // world matrix 16 + colour 4

// One per-instance GPU record (kInstanceWords * 4 bytes).
struct InstanceGpu
{
    crd::f32 world[16]; // column-major
    crd::f32 color[4];  // linear RGBA
};
static_assert(sizeof(InstanceGpu) == kInstanceWords * 4U, "GPU layout pinned");

// ⭐⭐ REN-40-B: the per-(chunk × group) RUN — one chunk's contiguous slot range inside a group's instance
// array, still the partial-re-upload grain (the ECS is chunk-grain; so is the renderer's dirt). It used to live
// here as `MeshGroup::runs` and be searched linearly; it now lives in the renderer's CHUNK INDEX
// (`Impl::RunEntry`), reachable from a chunk key in O(1), because finding the runs of a moved chunk by scanning
// every run of every group is O(chunks × runs) and that quadratic was most of a 171 ms extract at 1M instances.

// All instances of one cooked mesh — ONE buffer, ONE draw.
struct MeshGroup
{
    crd::resources::ResourceId                            mesh_id;
    crd::resources::ResourceId                            material; // REN-2 Half B: representative material (drives the base-color map)
    crd::resources::ResourceHandle<crd::resources::MeshResource> mesh; // keeps the payload resident
    crd::u32                                              index_count = 0;

    crd::containers::Array<InstanceGpu>                             instances;
    crd::containers::Array<crd::scene::EntityId>                    slot_entity;
    crd::containers::Array<crd::geometry::primitives::AABB3<crd::f32>> world_bounds; // per slot — the cull input
    // per slot: [0] screen bias f32 bits, [1] min_level | (max_level << 8) — the REN-40-C2 override
    crd::containers::Array<crd::u32>                                lod_override;
    crd::containers::Array<crd::u32>                                visible; // per-frame culled slot list

    std::unique_ptr<crd::gpu::IStorageBuffer> buffer;
    // ⭐⭐ REN-40-A: this group's GPU-written indirect commands — one per view (camera + each cascade), laid out
    // at the BACKEND'S command stride. ⛔ Per GROUP, not one global buffer: each group has its own storage
    // buffer, so a multi-draw cannot span groups anyway, and a per-group command keeps the kernel's write offset
    // a COOK-TIME constant instead of a runtime multiply.
    std::unique_ptr<crd::gpu::IStorageBuffer> cull_args;
    // REN-40-A: the base last written into the args params block — so the 4-byte write happens only when it
    // CHANGES. ⛔ A per-frame `upload_storage` per group is not free: the per-call queue-idle scar measured
    // 8.3 ms/frame from exactly this shape.
    crd::u32 cull_base_uploaded = 0xFFFFFFFFU;
    // ⭐⭐ REN-40-C2: the params block now carries the per-view PIXEL HEIGHTS beside the base, so the
    // "only when it changes" test has to cover all of them — a resize that moved the camera's height while
    // the base stayed put would otherwise leave the selector using the OLD viewport forever.
    crd::u64 cull_params_sig    = 0xFFFFFFFFFFFFFFFFULL;
    crd::u32 indices_off = 0; // word offsets into `buffer`
    crd::u32 vertices_off = 0;
    crd::u32 instances_off = 0;
    crd::u32 visible_off = 0;
    // ⭐⭐ REN-40-A: the per-instance WORLD AABB section (6 floats each), right after the visible lists. The GPU
    // cull kernel reads it through a DECLARED header word so it and the CPU's `aabb_in_frustum` read ONE truth.
    crd::u32 bounds_off = 0;
    // ⭐⭐ REN-40-C2: the per-instance LOD OVERRIDE section (2 words each), right after the bounds. Default
    // (bias 1.0, levels 0..7) for every slot whose entity carries no `MeshLodOverride`, so an entity that
    // does not care costs one write of a constant and nothing else.
    crd::u32 lod_override_off = 0;
    crd::u32 atlas_off        = 0; // REN-40-C5: word offset of the impostor atlas texels (RGBA8, one u32 each)
    // ⭐⭐ 38-G1 perf: the CASCADE visible lists live right after the camera's, one `capacity` block each.
    // Cascade c's list is at `visible_off + (1 + c) * capacity`. Shadow passes were drawing the CAMERA's list
    // four times — cascade 0 covers a few metres of a 110-unit field, so most of that vertex work was
    // transformed and then clipped. Measured cost of the waste: 8 ms of GPU, 130 fps -> 53.
    crd::u32 cascade_visible_count[kMaxCascades] = {};
    // ⭐⭐ REN-40-A: the CAMERA's CPU visible count for this frame, kept so the GPU cull's readback can be
    // compared against it (see `SceneRenderer::GpuCullCounts`). ⛔ Under the GPU cull the CPU list is no longer
    // what the draws use — but it is still the REFERENCE the device answer has to match, so it stays computed.
    crd::u32 visible_count_cpu = 0;
    // ⭐⭐ REN-40-C2: the mesh's LOD chain, as this group publishes it. `lod_count <= 1` means a one-level mesh
    // and the renderer behaves exactly as it did before chains existed.
    crd::u32 lod_count = 0;
    crd::u32 lod_first[kMaxLodSlots]  = {}; // first index of each level, RELATIVE to `indices_off`
    crd::u32 lod_indices[kMaxLodSlots] = {};
    crd::f32 lod_height[kMaxLodSlots]  = {};
    // ⭐⭐ REN-40-C4: the policy's hysteresis, mirrored here so `write_slot` can jitter each instance's
    // switch threshold without reaching back into the renderer's policy on the hot extract path.
    crd::f32 lod_hysteresis            = 0.0F;
    crd::u32 capacity = 0;           // instance slots the buffer holds
    crd::u32 region_base = 0;        // REN-38: this group's word base inside the ONE scene buffer (0 = unassigned)
    bool     geometry_uploaded = false;
    bool     has_impostor      = false; // REN-40-C5: this group has an impostor slot (the last LOD slot)

    // GEO-8: the SKINNED path — groups whose mesh carries the SKNV stream draw through the skinned program:
    // a packed skin stream (6 words/vertex: 2×(u16 pair) joints + 4 weights, uploaded once) and a per-instance
    // BONE PALETTE section (joint_count 4×4 matrices per slot, re-sampled + re-uploaded every frame — animation
    // is always dirty by definition). Per-slot animator state mirrors the ECS component at extract time.
    bool     skinned      = false;
    crd::u32 joint_count  = 0;
    crd::u32 skin_off     = 0; // word offset of the packed skin stream
    crd::u32 palette_off  = 0; // word offset of the palette section
    // REN-40-F: GPU skinning sections — uploaded once with geometry, read by the compute kernel.
    crd::u32 skel_off      = 0; // parents[jc] + inverse_binds[jc*16] + rest_pose[jc*10]
    crd::u32 clip_off      = 0; // pre-baked uniform-rate TRS frames
    crd::u32 anim_state_off = 0; // per-instance (clip_local_off, time) — 2 words each, per frame
    bool     skel_uploaded  = false;
    crd::containers::HashMap<crd::resources::ResourceId, crd::u32> baked_clip_off; // clip ID → word offset in clip section
    crd::containers::Array<crd::resources::ResourceId> slot_skeleton; // per slot (null = static instance)
    crd::containers::Array<crd::resources::ResourceId> slot_clip;
    crd::containers::Array<crd::f32>                    slot_time;

    explicit MeshGroup(crd::memory::IAllocator* a)
        : instances(a), slot_entity(a), world_bounds(a), lod_override(a), visible(a), baked_clip_off(a),
          slot_skeleton(a), slot_clip(a), slot_time(a)
    {
    }
    MeshGroup(MeshGroup&&) noexcept            = default;
    MeshGroup& operator=(MeshGroup&&) noexcept = default;
};

struct SyncStats
{
    crd::u32 groups            = 0;
    crd::u32 total_instances   = 0;
    bool     structural_rebuild = false;
    crd::u32 dirty_runs        = 0; // chunk runs re-extracted this sync
    crd::u64 uploaded_bytes    = 0; // instance-payload bytes uploaded (the partial-re-upload gate metric)
    crd::u32 meshes_pending    = 0; // MeshRenderers whose mesh resource is not loadable yet
    // 38-G1 perf: where the CPU milliseconds of sync() actually go. A single "sync 8.5 ms" number cannot be
    // optimized — the split names the hotspot instead of inviting a guess.
    double   extract_ms        = 0.0; // the ECS walks (signature + update/rebuild + animators)
    double   upload_ms         = 0.0; // geometry + instance-payload storage uploads
    double   palette_ms        = 0.0; // skinned palette sampling + its upload
    // ── ⭐⭐ REN-40-B: THE COMPLEXITY OF THE EXTRACT, COUNTED — not timed. ─────────────────────────────────
    // ⛔ A millisecond threshold is the wrong gate for an asymptotic claim: it is noisy, it is machine-specific,
    // and it passes on a debug build only by luck. These four counters make "a static frame costs O(chunks), not
    // O(entities)" a statement a test can assert EXACTLY, and make a regression to the O(entities) walk
    // impossible to land silently.
    crd::u32 chunks_visited     = 0; // chunks the extract walked (the irreducible cost)
    crd::u32 chunks_reextracted = 0; // of those, the ones whose Transform chunk-version had moved
    crd::u64 entities_extracted = 0; // per-entity slot writes — 0 on a frame in which nothing moved
    crd::u64 signature_bytes    = 0; // bytes fed to the structure signature: O(1) per CHUNK, not per entity
    crd::u64 runs_visited       = 0; // run records examined — O(runs of the dirty chunks), never a full scan
};

struct RenderStats
{
    crd::u32 draws            = 0;
    crd::u32 drawn_instances  = 0;
    crd::u32 culled_instances = 0;
    crd::u64 uploaded_bytes   = 0; // per-frame header + visible-list bytes
    // REN-8: what the DEVICE actually spent, from frame-graph timestamp queries, vs the CPU wall-clock of the
    // whole render call. The GAP between them is the answer to "why is the sandbox 12 ms/frame when neither
    // optimization nor unlocking vsync moves it" — a large gap means the frame is dominated by the
    // submit-and-WAIT that REN-1 deliberately kept, not by rendering work.
    double   gpu_ms           = 0.0; // first pass start → last pass end, one submission
    double   cpu_ms           = 0.0; // wall-clock of render(), including the fence wait
    crd::u32 timed_passes     = 0;
};

class SceneRenderer
{
public:
    explicit SceneRenderer(crd::memory::IAllocator* alloc);
    ~SceneRenderer();

    SceneRenderer(const SceneRenderer&)            = delete;
    SceneRenderer& operator=(const SceneRenderer&) = delete;

    // Capture the raster + resource seams (sync()'s buffers/uploads work after this). render() additionally
    // needs init_programs(). Split deliberately: extraction/upload logic is testable against a stub raster
    // context with no shader compiler in the loop.
    [[nodiscard]] bool init(crd::gpu::IRasterContext& raster, crd::resources::ResourceManager& rm);

    // Compile the CKIR forward VS/FS + assemble the raster program. False when the backend cannot build them.
    [[nodiscard]] bool init_programs(crd::gpu::IGpuContext& ctx);

    // REN-8: does the frame graph copy the rendered target back to host memory every frame? That copy exists
    // only so `read_pixel` works, so a PRESENTING app should turn it off — it costs a full-target PCIe transfer
    // per frame that nothing reads. Default true, because every readback-asserting gate depends on it and a
    // silent default-off would turn those gates green-on-nothing.
    void set_readback_enabled(bool on) noexcept;

    // ── ⭐⭐ REN-39-C1: the INDEXED-PULL switch. ON (the default) draws every scene pass INDEXED against the
    // storage buffer's own index sections — the measured 3–6× vertex-work cut (the frame was vertex-bound:
    // the non-indexed pull re-shaded every triangle corner with zero post-transform reuse). OFF keeps the
    // classic pull path. Exists as a RUNTIME switch so the parity gate can render the SAME scene both ways and
    // assert bit-identical frames — the pull path is the indexed path's reference. Call before init_programs
    // to skip cooking the indexed set entirely, or after it to flip per frame (both sets stay resident).
    void set_indexed_pull(bool on) noexcept;

    // ⭐⭐ REN-40-A: run the frustum cull ON THE DEVICE — the camera and every cascade — instead of on the CPU.
    // ⛔ DEFAULT OFF, and that is deliberate: it is a PERFORMANCE change, so it ships behind a switch the
    // parity gate can A/B on one build (the readback-A/B rule). Measured motivation at 1M instances: the CPU
    // cull + visible-list uploads were ~160 ms of a 337 ms frame
    // (`docs/bench/2026-07-29-ren40-million-instance-baseline.md`).
    void set_gpu_cull(bool on) noexcept;
    [[nodiscard]] bool gpu_cull() const noexcept;

    // ⭐⭐ REN-40-A: keep the CPU cull running ALONGSIDE the device cull, purely so the two verdicts can be
    // compared (`read_gpu_cull_counts`). ⛔ It costs the whole thing the slice exists to remove, so it is a GATE
    // mode, never a shipping one — and it exists because the alternative was worse: with the CPU cull skipped
    // there is no reference in the same frame, and "the counts look plausible" is exactly how a cull that tested
    // boxes made of light-colour bits survived (see `CullDesc::frustum_off` / the `bounds_off` scar).
    void set_gpu_cull_verify(bool on) noexcept;
    [[nodiscard]] bool gpu_cull_verify() const noexcept;

    // ⭐⭐ REN-40-F: compute the bone palette ON THE DEVICE instead of on the CPU.
    // Pre-bakes each clip to uniform-rate TRS frames (uploaded once), uploads per-instance
    // (clip_offset, time) per frame (2 words each), and dispatches a compute kernel that
    // samples → FK → IBM → writes the palette section. Default OFF.
    void set_gpu_skinning(bool on) noexcept;
    [[nodiscard]] bool gpu_skinning() const noexcept;

    // ⭐⭐ REN-40-C2: install an authored `.crdlod` policy BY NAME and start building
    // LOD chains. Returns false and changes nothing if the asset is missing or the
    // policy is refused — a rejected policy must not leave chains half-built with
    // switch distances nobody declared.
    // ⛔ Call BEFORE the first sync: chains are built when a mesh first becomes a
    // group, and `build_lod_chain` refuses a second build on the same resource.
    [[nodiscard]] bool set_lod_policy_asset(const char* asset_name);
    [[nodiscard]] bool lod_enabled() const noexcept;
    // What the chains actually came out as — levels and triangle counts per group,
    // so a run can say out loud what it built instead of implying it from the fps.
    struct LodChainInfo
    {
        crd::u32 groups          = 0U;
        crd::u32 groups_with_lod = 0U;
        crd::u32 levels_max      = 0U;
        crd::u32 tris_level0     = 0U; // summed over groups
        crd::u32 tris_coarsest   = 0U;
    };
    [[nodiscard]] LodChainInfo lod_chain_info() const noexcept;

    // ⭐⭐ REN-40-A: WHAT THE DEVICE ACTUALLY DECIDED, read back. A GPU cull hides its own result by design —
    // nothing on the CPU learns the count — so "fast" and "silently drew nothing" look identical from the outside.
    // ⛔ That makes this readback part of the FEATURE, not a debugging aid bolted on after: it is how the parity
    // gate asserts the device's per-view survivor counts EQUAL the CPU cull's, and how a live run can say out loud
    // what it drew. One `download_storage` per mesh group, so it is a diagnostic call, not a per-frame path.
    struct GpuCullCounts
    {
        crd::u32 views                  = 0U; // 1 (camera) + cascades
        crd::u32 groups                 = 0U; // mesh groups summed
        crd::u32 instances[1U + 4U]     = {}; // per view: Σ instance_count over groups — what the draws will run
        crd::u32 indices[1U + 4U]       = {}; // per view: group 0's index_count (0 here ⇒ the draw renders NOTHING)
        crd::u32 first_index[1U + 4U]   = {}; // per view: group 0's first_index
        // ⭐⭐ THE CPU'S ANSWER TO THE SAME QUESTION, from the same frame. The GPU cull is only correct if it
        // agrees with `aabb_in_frustum` over the same boxes and the same planes — so the comparison ships WITH
        // the readback rather than living in a test's head. View 0 is the camera's visible count; view c+1 is
        // cascade c's. ⛔ A cull that drops geometry is not "faster", it is broken, and this is the line that says
        // which.
        crd::u32 cpu_instances[1U + 4U] = {};
        // ⭐⭐ THE INPUT the device tested, checked against the input the CPU tested. A frustum cull is a function
        // of two things — the matrix and the boxes — so when the counts disagree the only useful next question is
        // WHICH input differs. `bounds_checked` is how many per-instance AABBs were compared (group 0);
        // `bounds_mismatch` how many differed bit-for-bit. ⛔ Non-zero here means the kernel is culling STALE
        // GEOMETRY, which no amount of staring at the plane maths would ever explain.
        // ⭐⭐ REN-40-C2: VIEW 0's COMMANDS, SLOT BY SLOT. A per-view total cannot tell "the cull chose slot 2"
        // from "slot 2's command is empty", and those two have completely different causes — one is the
        // selector, the other the reset's LOD-table read. Summed over groups, like `instances`.
        crd::u32 slot_instances[8]      = {};
        crd::u32 slot_indices[8]        = {};
        crd::u32 slot_first[8]          = {};
        crd::u32 bounds_checked         = 0U;
        crd::u32 bounds_mismatch        = 0U;
    };
    [[nodiscard]] bool read_gpu_cull_counts(GpuCullCounts& out) const;

    // ⛔ HARD RULE (AGENTS.md): EVERY render pass goes through our own frame-graph machinery. An overlay — the
    // infinite grid, gizmos, debug viz, editor chrome — is a RENDER PASS, so it belongs in the frame's graph as
    // a pass, not as a separate `draw_*` sequence with its own submit.
    //
    // Registering it here makes it the second pass of the SAME graph, sharing the one command buffer and the one
    // submission with the scene. Before this the sandbox's grid submitted independently and cost ~2.6 ms/frame —
    // nearly twice what the entire 4 100-instance scene cost on the GPU (~1.5 ms) — because each `draw_overlay`
    // outside a pass does its own submit+wait. Inside a pass the same call RECORDS instead (`frame_recording()`).
    //
    // The callback receives the frame context; `ctx.raster()` is in recording mode, so any existing draw helper
    // (`crd::draw::submit_overlay`, …) works unchanged from inside it.
    using FramePassFn = void (*)(crd::gpu::IFrameContext& ctx, void* user);
    void set_overlay_pass(FramePassFn fn, void* user) noexcept;

    // ── ⭐⭐ REN-39 (the gizmo fix): THE OVERLAY'S TARGET IS THE FRAME'S DECISION, NOT THE APP'S. ──
    // The woven overlay pass declares the SCENE image (a frame with a post chain routes the scene through an
    // HDR transient — the canvas the app captured is the POST pass's output, whose depth the scene never
    // wrote). The callback resolves its target HERE, per frame; null falls back to the app's own target (a
    // graph shape with no resolvable scene image). Drawing a captured raw pointer instead renders an image
    // the graph never barriered — a layout-validation storm and chrome that escapes the display transform.
    [[nodiscard]] crd::gpu::IRasterTarget* overlay_target(crd::gpu::IFrameContext& ctx) const noexcept;

    // REN-3.2-b: cascaded shadow maps. Rendering cascades costs one extra full pass over the draw list PER
    // CASCADE, so this is OFF by default — a consumer that does not want shadows must not pay for them. Returns
    // false if the cascade shaders did not compile, in which case shadows stay off rather than half-rendering.
    bool set_shadows_enabled(bool on) noexcept;
    void set_csm_config(const CsmConfig& cfg) noexcept;
    [[nodiscard]] const CsmCascades& cascades() const noexcept;

    // ── REN-37.2: WHICH LIGHTING TECHNIQUE shades this scene. ──
    // A NAME, not a code path — the name of a `.crdt` technique asset ("standard_forward", "unlit",
    // "forward_csm", or anything an app registers). Call before `init_programs`; the programs are cooked there.
    // ⛔ A name that does not resolve makes `init_programs` FAIL. It does not fall back to a default: rendering a
    // plausible frame with the WRONG technique is exactly the class of lie the magenta error graph exists to
    // prevent, and it would make "swap the technique" untestable (a typo would look like success).
    void set_forward_technique(const char* name) noexcept;
    void set_shadow_technique(const char* name) noexcept;

    // ── ⭐⭐ REN-38-F6: the ADVANCED-GRAPH seams. ──
    // Install a DIFFERENT authored frame graph (a `.frame.toml` text) as this renderer's frame. The shipped
    // advanced families (`frame/scene_tess|scene_mesh|scene_visbuffer|scene_cull|scene_rt.frame.toml`, via
    // `builtin_asset_text`) name programs the host cooks from the authored stage declarations. ⛔ Returns false
    // and KEEPS the previous graph on any parse/validation failure — never a half-installed frame.
    [[nodiscard]] bool set_frame_graph_toml(const char* toml_text);
    // ⭐⭐ 38-G1: install a frame BY ASSET NAME — `"frame/forward_csm_agx.frame.toml"` — resolved through the
    // SAME disk-first path every other asset uses (a shipped file under the asset root shadows the built-in
    // pack). ⛔ This, not a TOML string an app pastes into C++, is how an application selects a frame: the
    // moment the text lives in a caller, the frame stops being editable content and becomes code again.
    [[nodiscard]] bool set_frame_graph_asset(const char* asset_name);
    // ── ⭐ REN-38-F15: DISK-FIRST asset loading. ──
    // A file under `dir` shadows the embedded copy for every authored asset this renderer cooks (frame graphs,
    // stage declarations, materials) — editing `assets/` then changes the frame without a rebuild. ⛔ A disk
    // copy that exists but does not parse REFUSES the root (returns false); it never silently falls back.
    [[nodiscard]] bool set_asset_root(const char* dir);
    // The scene TLAS for `raytrace.*` passes. The graph NAMES an acceleration structure; whoever owns the
    // geometry's device form installs it here (B4: the asset format stays free of engine types).
    void set_scene_accel(crd::gpu::IAccelerationStructure* accel) noexcept;
    // The renderer-owned result buffers of the compute/RT scene graphs ("cull_flags", "hits") — the seam the
    // device gates read back through. Null until the graph that fills the buffer has executed once.
    [[nodiscard]] crd::gpu::IStorageBuffer* debug_scene_buffer(const char* name) noexcept;
    // REN-38: the ENGINE-FILLED vertex axis of the variant identity (folded from the live .crdv; 0 before
    // init_programs). The D5 row overclaimed this once; the accessor is what the gate asserts against.
    [[nodiscard]] crd::u32 debug_variant_vertex() const noexcept;

    // 38-G1 perf: the OWNED frame graph, read-only — the per-pass GPU board (`pass_name`/`pass_gpu_ms`) is
    // what turns "gpu 8.4 ms" into an attribution. Null until the first owner-path render.
    [[nodiscard]] const crd::gpu::IFrameGraph* debug_frame_graph() const noexcept;
    // The `pcf_taps` option value handed to the shadow technique (1 | 4 | 8 | 16). A DECLARED option, so each
    // choice cooks to its own fully-unrolled variant rather than a dynamic loop.
    void set_pcf_taps(crd::u32 taps) noexcept;

    // ⭐⭐ REN-40-D: the CASCADE CROSS-FADE width, in percent of a cascade's own footprint. Cascades are
    // fitted as spheres and selected by CONTAINMENT, so a fragment leaves one and enters the next at a hard
    // line — and the two sides differ in texel size, bias and filter footprint, so that line is VISIBLE as a
    // step in shadow softness even when both sides are individually correct. In the outer `pct` of a
    // cascade's footprint the shadow is resolved from BOTH it and the next coarser one and lerped.
    // ⛔ 0 (the default) cooks the byte-identical hard-select graph — a DECLARED option, so the parity arm is
    // structural rather than a tolerance. Call before `init_programs`.
    void set_cascade_blend_pct(crd::u32 pct) noexcept;

    // ⭐⭐ REN-40-D: CONTACT-HARDENING SOFT SHADOWS (PCSS). Fixed-radius PCF gives every shadow the same
    // softness, so a box resting ON the floor has the same blurry edge as one ten metres above it — and that
    // edge is the single cue the eye uses to read contact. PCSS searches the map for what is actually blocking
    // each fragment and sets the filter radius from how far away it is.
    // ⛔ `angle_x100` is the light's angular RADIUS in hundredths of a degree (the sun is ~27), NOT a "light
    // size" in some unspecified unit: a directional light has no size, it has an angular diameter, and a
    // penumbra of `distance · tan(theta)` stays physically correct at every cascade scale. Call before
    // `init_programs`; OFF by default, and off cooks the byte-identical fixed-radius graph.
    void set_soft_shadows(bool pcss, crd::u32 angle_x100 = 27U) noexcept;

    // ⭐⭐ REN-40-D: the full softness-model axis. `Off` = fixed-radius PCF; `Pcss` = per-fragment blocker search
    // (contact hardening); `Evsm` / `Msm` = the FILTERABLE tier — the frame graph gains a moment pipeline
    // (depth -> 4-channel moments -> separable Gaussian) and the technique reconstructs visibility from ONE
    // bilinear read. ⛔ Selecting Evsm/Msm swaps the DEFAULT frame graph for `forward_csm_moment` (unless an
    // explicit graph was installed — an explicit graph is the caller's contract and is never edited from here).
    // Call before `init_programs`, like every cook-affecting setter.
    enum class SoftShadow : crd::u8
    {
        Off  = 0,
        Pcss = 1,
        Evsm = 2,
        Msm  = 3,
    };
    void set_soft_shadows(SoftShadow mode, crd::u32 angle_x100 = 27U) noexcept;

    // ⭐⭐ REN-40-D: the two knobs the blocker search used to hardcode.
    // `max_texels` bounds BOTH the search and the filter, so they can never disagree about how far a blocker was
    // looked for — a filter wider than its own search asserts a penumbra from evidence nobody gathered, and the
    // error grows with blocker distance, which is exactly where the estimate matters. It exists because a
    // fixed-tap filter bands once its taps spread past a texel; a technique wanting unbounded softness wants a
    // filterable representation, not a bigger cap.
    // `search_taps` (4, 8 or 16) is the size of the search DISC — separate from `pcf_taps` because the search
    // runs once per fragment to produce one number while the filter runs per tap to produce the shadow.
    void set_soft_shadow_quality(crd::u32 max_texels, crd::u32 search_taps) noexcept;

    // Chunk-grain extract + change-driven upload. Call once per frame AFTER transform propagation (world.step).
    SyncStats sync(crd::scene::World& world);

    // Cull + submit into `target` (a create_color_depth_target). Reverse-Z: depth clears to 0, GreaterEqual.
    // `bvh`: an optional configured SpatialBVHIndex — its overlap query prunes the broad phase before the exact
    // 6-plane test (pass nullptr to plane-test every instance).
    RenderStats render(crd::gpu::IRasterTarget& target, const crd::math::Mat4f& view_proj,
                       const crd::math::Vec3f& light_dir, crd::gpu::ClearColor clear,
                       const crd::scene::SpatialBVHIndex* bvh = nullptr);

    // ── REN-37.8: CONTRIBUTE this scene's passes to a graph the CALLER owns. ──
    // Identical recording to `render()`, minus reset / build / execute — those belong to whoever owns the frame.
    // ⭐ THIS IS WHAT MAKES MULTIPLE VIEWPORTS ONE SUBMISSION: a host resets once, calls this per viewport, then
    // builds and executes once. Before the split, an editor with a main viewport, an animation preview and 12
    // dirty thumbnails SUBMITTED FOURTEEN TIMES, allocated every viewport's transients separately (peak VRAM =
    // SUM instead of MAX), could not order one viewport against another, and rebuilt shared work per viewport.
    //
    // `render()` is now exactly this plus ownership, so the two paths cannot drift.
    // ⛔ Call ONCE per frame, right after the host's `fg.reset()`, BEFORE the first `contribute()`. It recycles
    // the contribution arena — the fixed-address storage the frame graph's recorded user pointers refer to until
    // `execute()`. `render()` (which owns its graph) does this for you; a multi-viewport host must do it itself,
    // and the arena's cap is CHECKED, so forgetting it surfaces as viewports reporting zero draws rather than as
    // a use-after-free.
    void begin_frame() noexcept;

    RenderStats contribute(crd::gpu::IFrameGraph& fg, crd::gpu::IRasterTarget& target,
                           const crd::math::Mat4f& view_proj, const crd::math::Vec3f& light_dir,
                           crd::gpu::ClearColor clear, const crd::scene::SpatialBVHIndex* bvh = nullptr);

    [[nodiscard]] const crd::containers::Array<MeshGroup>& mesh_groups() const noexcept { return m_groups; }

    struct Impl; // opaque (defined in scene_renderer.cpp); public so the file-local extraction visitors reach it

private:
    std::unique_ptr<Impl> m_impl;
    crd::containers::Array<MeshGroup> m_groups;
};

// ── ⭐ REN-38 audit: the BUILT-IN AUTHORED PACK, exposed. ────────────────────────────────────────────────────
// The renderer's default assets are embedded TEXT (an engine default an app overrides by name, no file IO on
// the init path) and the same declarations ship as editable files under `assets/`. ⛔ TWO COPIES OF ONE
// DECLARATION DRIFT — that is the two-vocabularies disease one level down — so the copies are PINNED: the
// drift gate parses both sides to their canonical form and refuses a mismatch. This accessor is that gate's
// seam, and the disk-first loader's fallback source. Names mirror the shipped paths ("frame/forward_csm.frame.toml",
// "material/scene.crdm", "vertex/scene.crdv", "lighting/scene_forward.crdl", ...). Returns false for a name
// the pack does not hold.
[[nodiscard]] bool builtin_asset_text(const char* name, crd::containers::String& out);

// Extract the 6 world-space frustum planes (ax+by+cz+d ≥ 0 = inside) from a view-projection matrix
// (Gribb–Hartmann; clip z in [0,1] — the Vulkan/reverse-Z convention). Plane order: L R B T N F.
void frustum_planes(const crd::math::Mat4f& view_proj, crd::math::Vec4f out_planes[6]);

// Positive-vertex AABB-vs-plane-set test: true iff `box` intersects the frustum described by 6 planes.
[[nodiscard]] bool aabb_in_frustum(const crd::geometry::primitives::AABB3<crd::f32>& box,
                                   const crd::math::Vec4f planes[6]) noexcept;

// The world-space AABB of the frustum (the BVH broad-phase query box): the 8 clip corners through the inverse
// view-projection. Degenerate matrices yield an everything-box (the broad phase then prunes nothing — safe).
[[nodiscard]] crd::geometry::primitives::AABB3<crd::f32> frustum_aabb(const crd::math::Mat4f& view_proj);

} // namespace crd::scenerender
