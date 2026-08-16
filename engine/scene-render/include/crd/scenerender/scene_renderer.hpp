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
class IRasterProgram; // RAF-9: program-registry provider return types
class IGpuProgram;
class ICommandEncoder; // RAF-10: the record fn of a custom pass executor takes the canonical encoder
}
namespace crd::renderpass
{
struct PassPayload; // RAF-10: custom-executor record-fn parameter (the resolved pass payload)
}
namespace crd::rendergraph
{
class RecordContext; // RAF-10: custom-executor record-fn parameter (the slot/program resolver)
}
namespace crd::kir::technique
{
struct Technique; // RAF-10: define_technique() argument — an app supplies its own shading technique by struct
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

// ⭐⭐ REN-41 (velocity): the ENCODE/DECODE constant shared by the motion-vector DEBUG VIEW and its correctness
// gate. The `crd://scene/velocity_debug` fullscreen pass writes `out.rg = velocity.xy * this + 0.5` into an RGBA8
// image; a reader recovers the signed UV motion delta as `(read/255 − 0.5) / this`. ONE home so the shader and
// the gate can never drift. 1.0 keeps a full-screen mover's delta (~0.2 UV) comfortably inside [0,1] with 0-motion
// landing at mid-grey (0.5), so either sign of motion is visible and decodable.
inline constexpr crd::f32 kVelocityDebugScale = 1.0F;

// ── ⭐⭐ REN-41 Stage 4 (S4-0): the CLUSTER buffer layout — the device home for a 40-I packed cluster DAG. ────
// ONE u32 storage buffer holds a header + the four packed arrays the Nanite mesh shader unpacks. The header keeps
// `view_proj` at word 6 so it reuses the scene VS's `Gx::mul_view_proj` convention verbatim; the four section
// offsets (all in u32 words) point past the header. `positions` is f32 stored as u32 bits.
//   [0] clusters_off  · [1] cluster_count · [2] vertices_off · [3] triangles_off · [4] positions_off · [5] pad
//   [6..21] view_proj (column-major f32 bits) · [22..23] pad → data begins at kClusterHeaderWords.
inline constexpr crd::u32 kClusterHdrClustersOff  = 0U;  // word offset of the packed_clusters array (10 u32/cluster)
inline constexpr crd::u32 kClusterHdrClusterCount = 1U;  // number of clusters (== the mesh-workgroup dispatch count)
inline constexpr crd::u32 kClusterHdrVerticesOff  = 2U;  // word offset of cluster_vertices (u32 global indices)
inline constexpr crd::u32 kClusterHdrTrianglesOff = 3U;  // word offset of cluster_triangles_packed (4 u8/u32)
inline constexpr crd::u32 kClusterHdrPositionsOff = 4U;  // word offset of positions (3 f32 bits / vertex)
inline constexpr crd::u32 kClusterHeaderWords     = 24U; // header size; the packed arrays follow
// FIXED meshlet caps → one static mesh-workgroup size + degenerate-primitive culling for the tail. 128/128 keeps
// the workgroup within every desktop device's mesh limits and matches the 40-I meshlet builder's targets.
inline constexpr crd::u32 kClusterMaxVerts = 128U;
inline constexpr crd::u32 kClusterMaxPrims = 128U;

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
// ⭐ CEIR-18a-2 (B8-m): the light section holds 1 directional + kMaxScenePointLights point records. Point counts are
// COOK-TIME (they bake the FS unroll — scene_forward.crdl [counts]), so the runtime declares a FIXED capacity and
// null-fills the unused slots. The section still sits at the END of the header (kHeaderWords − kLightSectionWords).
inline constexpr crd::u32 kMaxScenePointLights = 4U;  // the forward FS's declared point-light capacity (scene_forward.crdl)
inline constexpr crd::u32 kLightRecordWords    = 16U; // one record's stride — MUST match scene_forward.crdl [record] stride
inline constexpr crd::u32 kLightSectionWords   = (1U + kMaxScenePointLights) * kLightRecordWords; // 1 directional + N point
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
// ⭐⭐⭐ REN-41 (Stage 3): a monotonic FRAME INDEX, uploaded every frame. It exists so the LOD cross-dissolve
// dither can be TEMPORAL (interleaved-gradient-noise offset by the frame) rather than a fixed spatial Bayer — the
// difference between a crawling checkerboard and a seamless dissolve once TAA averages the two levels. A FREE
// word (106) in the existing header — kHeaderWords is unchanged, so no section moves.
inline constexpr crd::u32 kHdrFrameIndex      = 106U;
// ⭐⭐ REN-41 (velocity / motion vectors): FREE header words carrying the two previous-frame section offsets the
// velocity prepass reads. Both are region-RELATIVE like every other section offset (the VS rebases by DrawIndex),
// and both live in the previously-free 107..119 band, so `kHeaderWords` is unchanged and no section moves.
inline constexpr crd::u32 kHdrPrevWorldOff    = 107U; // per-instance previous world transform (16 words each)
inline constexpr crd::u32 kHdrPrevPaletteOff  = 108U; // per-instance previous bone palette (skinned; 0 if static)
// ⭐⭐ REN-41 (velocity, skinned): 1 when GPU skinning is active, else 0. The `palette_snapshot` device pass copies
// palette_off → prev_palette_off ONLY when this is set — under CPU skinning the renderer already CPU-uploads
// prev_palette, and an unconditional device copy would clobber that with the current pose (prev == cur, no motion).
inline constexpr crd::u32 kHdrGpuSkinActive   = 109U;
// ⭐⭐ CEIR-18a-2 Stage 2 (Forward+ clustered): the word offset (into the group buffer) of the per-tile/cluster
// LIGHT-INDEX LIST — the E5 clustered forward FS reads its per-pixel run at `header[kHdrClusterOff] +
// cluster_index*max_per_cluster + flat_index` (lighting_asset.cpp:800; `LightingDesc.header.cluster_off`, whose default
// 21 COLLIDED with view_proj). A FREE word (110) in the 107..119 band — kHeaderWords is unchanged, no section moves.
inline constexpr crd::u32 kHdrClusterOff      = 110U;
// ⭐⭐ CEIR-18a-2 Stage 2 (Forward+): the VIEWPORT dimensions in PIXELS (the render target's width/height), written raw
// each frame. The clustered forward FS reads them to NORMALIZE FragCoord.xy into [0,1) before multiplying by the froxel
// grid (lighting_asset.cpp cluster block) — `frag_xy` stays PIXEL coords (its documented meaning; the PCF/ign rotation
// needs the per-pixel value), so the tile index divides by these here instead of corrupting frag_xy's unit. Words 111
// (pre-reserved above for a cluster dims word) and 112 — both in the free 107..119 band, nothing below 120 moves.
inline constexpr crd::u32 kHdrViewportW       = 111U;
inline constexpr crd::u32 kHdrViewportH       = 112U;
// ⭐⭐ CEIR-18a-2 Stage 2b (Forward+ GPU producer): the ABSOLUTE bases of the two light-cull INPUT sections in the group
// buffer — the froxel-AABB table (per-cluster view→world AABBs, min.xyz/max.xyz) and the light-input ({position,radius}
// per point light). The `engine://scene/light_cull` compute kernel reads them through THESE header words (the same
// header-indirection the FS uses for the list). 113/114 are the last free words below the LOD table (120).
inline constexpr crd::u32 kHdrFroxelAabbOff   = 113U;
inline constexpr crd::u32 kHdrLightViewOff    = 114U;
// ⭐⭐ CEIR-18b (clustered 3D froxel): the N+1 EXPONENTIAL z-slice boundaries, in clip.w (view-depth) terms, f32 BITS.
// The renderer computes them CPU-side each frame (unproject near/far, pick `w_i = w_near·(w_far/w_near)^(i/N)`) and the
// clustered FS bins each fragment by counting how many INTERIOR boundaries its `clip.w` exceeds — the froxel AABBs are cut
// at the SAME boundaries, so binning is bit-consistent by CONSTRUCTION (the "publish a table, never a formula" discipline
// that kills the 2-derivation drift that bit frag_xy / the Y-flip / the viewport words). Words 115..119 are the LAST free
// band below the LOD table (120) — exactly N+1=5 slots, which HARD-CAPS N=4 (grid[2]≤4) on the current header. Only the 3D
// clustered technique writes/reads them; the 2D tiled path (grid[2]==1) never touches them and cooks byte-identical.
inline constexpr crd::u32 kHdrSliceBoundsOff  = 115U;
inline constexpr crd::u32 kNumSliceBounds     = 5U;  // N+1 = 5 (grid[2] up to 4 slices)
// ⭐⭐ CEIR-18a-2 Stage 2 / CEIR-18b: the MAX cluster count the group buffer reserves for — 4×4×4 = 64 (the 3D clustered
// grid; also = the light-cull workgroup size, so `cluster = LocalInvocationIndex` covers every cluster with ONE dispatch).
// ⛔ The 2D tiled config (grid 4×4×1 = 16) uses a PREFIX of the same sections — it writes/reads only its first 16 clusters,
// leaving the tail unread. A grid > 64 forces the cluster-id restructure (a named follow-up), which this pins as the ceiling.
inline constexpr crd::u32 kMaxSceneClusters = 64U;
// ⭐⭐ CEIR-18a-2 Stage 2: the size (u32 words) of the per-group cluster LIGHT-LIST section — kMaxSceneClusters ×
// max_per_cluster (64 × 8 = 512). ⛔ Sized for the 3D MAX; the 2D crdl (4×4×1) fills only its 128-word prefix. A value
// spec-const can't change the FS's unroll, so `cap` stays compile-time until the cull-asset generalization slice
// (unroll → stmt_for). The renderer reserves this in every group buffer; header [kHdrClusterOff] points at it, and Stage 2a
// fills it CPU-side (set_cluster_light_list), Stage 2b via the cull.
inline constexpr crd::u32 kSceneClusterListWords = kMaxSceneClusters * 8U;
// ⭐⭐ CEIR-18a-2 Stage 2b / CEIR-18b: the light-cull INPUT sections. The froxel-AABB table is one min.xyz/max.xyz box per
// cluster (kMaxSceneClusters × 6 = 384; the 2D config writes its first 16 = 96-word prefix, the 3D config all 64 per-slice
// boxes), CPU-computed each frame by unprojecting the tile corners; the light-input is {position.xyz, radius} per point
// light (kMaxScenePointLights × 4 = 16). Both reserved INSIDE the region (after the cluster list), published at
// kHdrFroxelAabbOff / kHdrLightViewOff.
inline constexpr crd::u32 kSceneFroxelAabbWords = kMaxSceneClusters * 6U;
inline constexpr crd::u32 kSceneLightViewWords  = kMaxScenePointLights * 4U;
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
inline constexpr crd::u32 kCullArgsHeaderWords = 12U; // 48 B — keeps the first command 16-byte aligned
inline constexpr crd::u32 kCullArgsPixelHeight = 1U;  // + view index (words 1..1+kMaxCascades)
// The HZB's texel dimensions (f32 bits, words 7/8) — what makes the occlusion test CONSERVATIVE. The kernel
// may only claim "occluded" for an instance whose screen rect spans ≤ 1 HZB texel (its 4 corner taps then
// cover every touched texel); anything larger is accepted, because a single-point test rejects instances
// that are visible at their edges — whole meshes blinking in and out as the camera moves.
inline constexpr crd::u32 kCullArgsHzbSize     = 7U;  // w at 7, h at 8; 9..11 pad
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
// ⛔⛔ CEIR-18b: the z-slice boundary band [115..119] must stay strictly BELOW the LOD table — growing grid[2] past 4 (or
// sliding the band up) would overwrite kHdrLodCount and the scene would read a slice boundary as its LOD level. A grid > 64
// (the cluster-id restructure) is a named follow-up; this pins the header budget that makes N=4 the current ceiling.
static_assert(kHdrSliceBoundsOff + kNumSliceBounds <= kHdrLodCount, "z-slice boundary band must fit below the LOD table");
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
// the light section (1 directional + kMaxScenePointLights point records) occupies [kLightSectionStart, kHeaderWords).
// ⛔⛔ kLightSectionStart is LOAD-BEARING: every non-light header word lives BELOW it (the highest is kHdrAtlasDims=147),
// so growing kMaxScenePointLights grows the header UPWARD only — nothing below 148 moves. The static_assert pins the
// invariant (`header[kHdrLightOff] = kHeaderWords − kLightSectionWords` must still land on kLightSectionStart).
inline constexpr crd::u32 kLightSectionStart  = 148U;
inline constexpr crd::u32 kHeaderWords        = kLightSectionStart + kLightSectionWords;
static_assert(kHeaderWords - kLightSectionWords == kLightSectionStart, "light section must start at kLightSectionStart");
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
// ⭐⭐ CEIR-18p: the AUTHORED impostor FS's D12 SPEC-CONSTANT ids. The FS structure is config-independent (a fixed
// `kImpostorMaxMips`-way select chain); these carry the live LOD-policy VALUES, set via `KGraph::set_spec_const` after
// `ckir_read` and BEFORE `lower_entry`/`create_program`. `gt` (F32) = grid*tile (level-0 atlas edge); `mips_m1` (F32) =
// live mip count − 1 (the trilinear clamp, one node, two clamp sites); `lvl0..15` (U32) = each level's texel offset
// (`impostor_level_offset`), the unreachable tail (k ≥ live mips) patched to 0. Ids are per-program → no cross-program
// collision. The VS needs NONE — it reads grid/tile from the `kHdrAtlasDims` header word at runtime.
inline constexpr crd::u32 kImpostorGtSpec      = 0U;
inline constexpr crd::u32 kImpostorMipsM1Spec  = 1U;
inline constexpr crd::u32 kImpostorLvl0Spec    = 2U; // ids 2..17 = the 16 level offsets (kImpostorLvl0Spec + level)
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
    // ⭐⭐ REN-41 (velocity / motion vectors): the PREVIOUS frame's world transform per slot (16 floats each),
    // snapshotted in `write_slot` before the current transform overwrites it. The velocity prepass emits
    // `prev_clip = prev_vp · prev_world · pos`, so a static instance (prev == cur) yields pure camera motion and a
    // mover carries its transform delta. Uploaded on the SAME dirty grain as the instances it shadows.
    crd::containers::Array<crd::f32>                                prev_world; // 16 words/slot
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
    // ⭐⭐ REN-41 (velocity): word offset of the per-instance PREVIOUS-WORLD section (16 floats each). Read by the
    // velocity prepass to form `prev_clip`. Rides the instance dirty grain so it never goes stale against `world`.
    crd::u32 prev_world_off   = 0;
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
    // ⭐⭐ REN-41 (velocity, skinned): the PREVIOUS frame's bone palette (joint_count 4×4 per slot). Copied from
    // `palette_off` by the `palette_snapshot` pass BEFORE `gpu_skin` overwrites it, so the velocity prepass can
    // deform each vertex by last frame's pose and reproject skinned deformation exactly (not just the rigid move).
    crd::u32 prev_palette_off = 0;
    // ⭐⭐ REN-41 (velocity, skinned, CPU path): last frame's CPU-computed bone palette. On the CPU-skin path the
    // device palette_snapshot pass never runs, so the renderer uploads THIS to prev_palette_off each frame BEFORE
    // the current palette overwrites palette_off — the CPU-skin analog of the GPU snapshot. First frame seeds it
    // with the current palette (prev == cur → zero velocity on spawn).
    crd::containers::Array<crd::f32> prev_palette;
    // ⭐⭐ CEIR-18a-2 Stage 2 (Forward+): the PER-TILE light-index run (kSceneClusterListWords, grid-linear:
    // cluster_index * max_per_cluster + flat, survivors then null_index=num_lights). Lives in the group buffer at
    // `cluster_list_off`; the header publishes it as `header[kHdrClusterOff]` and the clustered forward FS walks it
    // per fragment. Provisioned by the CPU (Stage 2a: set_cluster_light_list) or a light-cull compute pass (2b).
    crd::u32 cluster_list_off = 0;
    // ⭐⭐ CEIR-18a-2 Stage 2b (Forward+ GPU producer): the light-cull INPUT sections, chained after the cluster list
    // (also INSIDE the consolidated region). froxel_aabb = per-cluster view→world AABB (kSceneFroxelAabbWords), light_view
    // = {position,radius} per point light (kSceneLightViewWords), CPU-filled each frame; the light-cull kernel reads them
    // through kHdrFroxelAabbOff / kHdrLightViewOff and writes the cluster list in place.
    crd::u32 froxel_aabb_off = 0;
    crd::u32 light_view_off  = 0;
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
        : instances(a), slot_entity(a), world_bounds(a), prev_world(a), lod_override(a), visible(a),
          prev_palette(a), baked_clip_off(a), slot_skeleton(a), slot_clip(a), slot_time(a)
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
    crd::u32 impostor_draws   = 0; // REN-40-C5 / CEIR-18p: groups drawn through the impostor pass (billboard VS + impostor FS)
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

    // ⭐⭐ REN-41 Stage 4: draw a device CLUSTER buffer through the Nanite mesh path. `cluster_buf` holds a header
    // (`view_proj` at word 6 + the kClusterHdr* section offsets) followed by the four 40-I packed arrays; the mesh
    // shader unpacks ONE cluster per workgroup (dispatch `cluster_count` workgroups) and, for S4-0, self-selects
    // the level-0 leaves. This is the direct verb the authored frame-graph `mesh_draw` pass (S4-3) builds on
    // (`draw_mesh_storage`). No-op on a device without mesh shaders. Requires `init_programs`.
    void draw_clusters(crd::gpu::IRasterTarget& target, crd::gpu::IStorageBuffer& cluster_buf,
                       crd::u32 cluster_count, crd::gpu::ClearColor clear);

    // ⭐⭐ REN-41 Stage 4: does this device + backend support the cluster (Nanite) mesh path? True iff the cluster
    // mesh program COOKS — a device with mesh shaders and a working SPIR-V/DXIL mesh emitter. Backend-agnostic
    // (no per-backend capability query needed at the call site). Call after `init_programs`.
    [[nodiscard]] bool supports_clusters();

    // ⭐⭐ REN-40-F: compute the bone palette ON THE DEVICE instead of on the CPU.
    // Pre-bakes each clip to uniform-rate TRS frames (uploaded once), uploads per-instance
    // (clip_offset, time) per frame (2 words each), and dispatches a compute kernel that
    // samples → FK → IBM → writes the palette section. Default OFF.
    void set_gpu_skinning(bool on) noexcept;
    [[nodiscard]] bool gpu_skinning() const noexcept;

    // ⭐⭐ CEIR-18a-2 (B8-m): install the scene's POINT LIGHTS. The forward FS loops over the declared point count
    // (kMaxScenePointLights) every fragment; these fill records [1..N] of the per-frame light section (record 0 is the
    // directional key), and the rest stay NULL (provably-zero: far position + zero colour). `radius` is the light's
    // REACH — the FS derives Filament's smooth-window falloff = 1/radius² from it, and STAGE 2's cluster cull will use
    // the SAME radius directly (one source ⇒ the cull and the shading can never disagree on a light's extent; a
    // radius ≤ 0 renders the slot null). ⛔ More than kMaxScenePointLights is REFUSED (false + a logged error): the
    // point count is a COOK-TIME declared constant, not a runtime capacity, so an overflow could only be silently
    // dropped. This is per-frame header state, NOT cook state — any call before `render()` takes effect that frame;
    // there is no init/sync ordering hazard.
    struct ScenePointLight
    {
        crd::math::Vec3f position{0.0F, 0.0F, 0.0F};
        crd::f32         radius = 1.0F; // reach; falloff = 1/radius² (≤ 0 ⇒ the slot renders null)
        crd::math::Vec3f color{1.0F, 1.0F, 1.0F};
    };
    [[nodiscard]] bool set_point_lights(crd::containers::ConstSpan<ScenePointLight> lights);

    // ⭐⭐ CEIR-18a-2 Stage 2a (Forward+): install the CPU-written per-tile light-index run consumed by the CLUSTERED
    // forward technique (scene_forward_clustered.crdl). Layout is grid-linear: `cluster_index * max_per_cluster + flat`,
    // holding light-array indices for the survivors in each tile, then the sentinel `null_index = point-light count`.
    // The words are uploaded into the group buffer's cluster section each frame; the clustered FS reads them through
    // `hdru(cluster_off)`. ⛔ This is the Stage-2a CPU stand-in; Stage 2b replaces it with a light-cull compute pass
    // writing the SAME layout. Refuses (LOUD) above kSceneClusterListWords. Only the clustered technique reads it.
    [[nodiscard]] bool set_cluster_light_list(crd::containers::ConstSpan<crd::u32> list);

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

    // ⭐⭐ REN-41 (velocity, TEST): the max |prev_world − world| over every instance, in world units. It is the
    // residual the velocity prepass turns into a motion vector: for a frame where NOTHING moved it MUST be 0, and a
    // mover's residual must decay to 0 the frame after it stops (the intermittent-update advance). Non-zero on a
    // static frame is the stale-prev_world smear regressing.
    [[nodiscard]] crd::f32 debug_max_velocity_residual() const noexcept;

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
        crd::u32 header_w0              = 0U; // group buffer word 0 (index_count) on the device
        crd::u32 header_w2              = 0U; // group buffer word 2 (indices_off) on the device
        crd::u32 raw_args[16]           = {}; // first 16 words of cull_args on the device
        crd::u32 args_size              = 0U; // cull_args buffer size in bytes
        crd::u32 fill_cull_gates        = 0U; // how many times fill()'s GPU cull gate fired
        crd::u32 fill_dispatch_max      = 0U; // largest dispatch_groups set in fill()
        crd::u32 fill_total_items       = 0U; // total items produced across all fill() calls
        crd::u32 fill_index_count_0     = 0U; // d.index_count of the first draw (0 ⇒ gate cannot fire)
        crd::u32 fill_record_ok        = 0U;
        crd::u32 fill_build_ok         = 0U;
        crd::u32 fill_pass_count       = 0U;
        crd::u32 occ_step              = 0U;
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
    // advanced families (`frame/scene_tess|scene_mesh|scene_visbuffer|scene_cull|scene_rt.frame.toml`, loaded by
    // name from the asset root) name programs the host cooks from the authored stage declarations. ⛔ Returns
    // false and KEEPS the previous graph on any parse/validation failure — never a half-installed frame.
    [[nodiscard]] bool set_frame_graph_toml(const char* toml_text);
    // ⭐⭐ RAF-9: install a frame BY CANONICAL ASSET ID — `"engine://frame/forward_csm_agx"` — resolved through the
    // SAME public registry/resolver an app uses (`engine://` mounts the asset root; `crd://` folds to it). This is the
    // Gate-9 selector: a default is chosen by id, not by a relative path or an embedded TOML. ⛔ A missing/unparseable
    // id REFUSES (returns false, KEEPS the previous frame) and reports a NAMED diagnostic. (RAF-12 retired the
    // relative-name `set_frame_graph_asset` wrapper — a frame is selected by canonical id, never a relative path.)
    [[nodiscard]] bool set_frame_graph(const char* canonical_id);
    // ⭐⭐ RAF-11: HOT RELOAD a render asset by canonical id — re-read its source, re-cook it, and (if the cooked
    // content changed) install a new immutable GENERATION at the next frame boundary, rebuilding every dependent in
    // dependency order. On any cook/validate failure — including an interface change a dependent can no longer bind —
    // the whole set is REJECTED: the previous valid generation of every asset stands (rendering never breaks) and a
    // NAMED diagnostic reports the failing asset + reason. Retired GPU objects are freed only after the in-flight
    // frames that referenced them complete. Call at a frame boundary (between `render`s). Returns false if the id is
    // not registered/reloadable or the reload was rejected; the reason is logged. This is the §13 live-replacement seam.
    [[nodiscard]] bool reload(const char* canonical_id);
    // The current runtime GENERATION of a reloadable asset (0 if unknown). Bumps on every successful content change, so
    // a consumer comparing generations detects that the asset was replaced out from under it (RAF-3 staleness).
    [[nodiscard]] crd::u64 asset_generation(const char* canonical_id) const;
    // ⭐⭐ RAF-9: a PROGRAM PROVIDER — the fn-ptr + `void* user` idiom (like FramePassFn), NO std::function. Resolves a
    // canonical program/kernel id to a device program. Engine defaults register captureless thunks over the internal
    // builders at `init_programs`; an app registers its OWN under `app://…` the SAME way — so an app frame graph and an
    // engine default resolve their programs through ONE public registry, with no privileged hidden engine path.
    using RasterProgramProvider = crd::gpu::IRasterProgram* (*)(void* user);
    using KernelProgramProvider = crd::gpu::IGpuProgram* (*)(void* user);
    // Register a raster / compute program under its canonical id (`engine://…` / `app://…`; `crd://` folds to engine).
    // The id is parsed to an AssetId; `program()`/`kernel()` resolution is a registry lookup, never a hard-coded branch.
    // Returns false on a malformed id or null provider.
    [[nodiscard]] bool register_raster_program(const char* canonical_id, RasterProgramProvider provider, void* user);
    [[nodiscard]] bool register_kernel_program(const char* canonical_id, KernelProgramProvider provider, void* user);
    // ⭐⭐ RAF-10: register an APPLICATION pass EXECUTOR — a custom C++ record fn under a canonical id. A `kind =
    // "custom"` frame pass naming this id (its `executor = "app://executor/…"`) records through `fn`, which joins the
    // SAME executor table a builtin pass uses. This is how an app adds a genuinely new pass MECHANIC — recording
    // arbitrary canonical commands — with NO engine edit: not a new FramePassKind, not a backend virtual, not a central
    // enum. `fn` receives the resolved (PassPayload, RecordContext, ICommandEncoder); it may touch ONLY the slots the
    // pass declared (RecordContext diagnoses an undeclared one). Returns false on a null fn or an id already taken (a
    // builtin executor's id, or a second registration of the same app id). Call before the first `render`.
    using PassExecutorFn = void (*)(const crd::renderpass::PassPayload& payload, crd::rendergraph::RecordContext& ctx,
                                    crd::gpu::ICommandEncoder& encoder);
    [[nodiscard]] bool register_pass_executor(const char* canonical_id, PassExecutorFn fn);
    // ⭐⭐ RAF-10: supply the SCENE MATERIAL as canonical ids (`app://material/…`). Defaults are the shipped engine
    // relative names, so unset behaviour is byte-identical. Two ids because a group WITH a base-colour map cooks a
    // different (textured) surface than one without; pass the same id twice if the app authors one material for both.
    // An id with a `://` scheme resolves through the mount table (so an app material under `set_app_asset_root`
    // resolves); a bare relative name stays on the engine mount. Call before `init_programs`. Returns false if impl is
    // unset or both ids are null. This is the ONE seam that re-surfaces the scene with no engine rendering-code edit.
    [[nodiscard]] bool set_scene_material(const char* opaque_id, const char* textured_id);
    // ⭐⭐ RAF-10: register an APPLICATION shading TECHNIQUE — the SAME `crd::kir::technique::Technique` struct the engine
    // registers its own with. Replayed after the built-ins in `init_programs`, so a same-named app technique SHADOWS the
    // engine's (the library resolves by last-match). Select it with `set_forward_technique(name)`. The `Technique`'s
    // `body`/`bindings`/`options` must outlive the renderer (static app data, exactly like the engine's). Call before
    // `init_programs`. Returns false if impl is unset or the technique is invalid (no name, or neither body nor blob).
    [[nodiscard]] bool define_technique(const crd::kir::technique::Technique& technique);
    // ⭐⭐ RAF-10: register an APPLICATION post/display-transform program — a `.crdp` the app authored — under a canonical
    // id (`app://post/…`). A frame graph's post pass names that id in `shader = …`; the renderer cooks `crdp_asset_name`
    // (a `://` id resolves through the mount table, a bare name through the engine mount) into a fullscreen program the
    // SAME way the engine cooks its own tonemap/sRGB. This is how an app REPLACES the tonemap with no engine edit and no
    // bespoke cook. Call before `init_programs`. Returns false on a null/malformed id or null `crdp_asset_name`.
    [[nodiscard]] bool register_post_asset(const char* canonical_id, const char* crdp_asset_name);
    // ⭐⭐ RAF-10: query a capability by name — the SAME predicate the frame-graph `requires`/`fallback` step-down
    // consults. `"shadows"` reflects the shadow tier; device features (`"bindless"`, `"vrs"`,
    // `"conservative_raster"`, `"inner_coverage"`, `"fragment_interlock"`, `"ray_tracing_pipeline"`) reflect the
    // adapter; every other name is UNSUPPORTED (false) by rule. Lets an app make its capability fallback EXPLICIT —
    // inspect what the device offers and select accordingly — rather than discovering it through a silent step-down.
    [[nodiscard]] bool capability(const char* name);
    // ── ⭐ REN-38-F15 / REN-41: the ASSET ROOT is the single source of truth. ──
    // Every authored asset this renderer cooks (frame graphs, stage declarations, materials, lighting) is read
    // from a file under `dir` — editing `assets/` changes the frame without a rebuild. There is no in-binary
    // pack; an application authors its OWN pipelines under its OWN root. ⛔ A file that exists but does not parse
    // REFUSES the root (returns false, nothing half-installed). If the host never calls this, `init` honours the
    // `CRD_ASSETS_DIR` convention so the shipped default assets still resolve.
    [[nodiscard]] bool set_asset_root(const char* dir);
    // ⭐⭐ RAF-10: mount the APPLICATION's own asset tree at `app://` (frames · materials · techniques · post). An app
    // authors its own content under this root; `set_frame_graph("app://frame/…")` and app-registered `app://…` programs
    // then resolve through the SAME public resolver/registry as engine defaults — no privileged engine-only path.
    // ⛔ `app://` and `engine://` are structurally distinct namespaces: an app asset can never SHADOW an engine one.
    [[nodiscard]] bool set_app_asset_root(const char* dir);
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
    // ⛔ 0 cooks the byte-identical hard-select graph — a DECLARED option, so the parity arm is
    // structural rather than a tolerance. Default 15. Call before `init_programs`.
    void set_cascade_blend_pct(crd::u32 pct) noexcept;

    // Shadow distance fade: the outermost cascade smoothly dissolves to fully lit over the last `pct` percent
    // of its footprint. 0 = the historical hard cutoff; 30 (the default) matches the UE5/Unity/Frostbite norm.
    // ⛔ 0 emits no extra nodes (bit-identical cook). Call before `init_programs`.
    void set_shadow_fade_pct(crd::u32 pct) noexcept;

    // Shadow-caster screen-size cull (UE5's "Min Screen Radius For Shadows"): an instance whose CAMERA-projected
    // height is below `px` pixels never enters a cascade's caster list — at that size the atlas cannot resolve
    // its shadow into anything but a flickering dot. Applied identically by the CPU cascade cull and the device
    // cull kernels. Default 16; 0 disables. Call before `set_gpu_cull` graphs cook (it changes the kernels).
    void set_shadow_caster_min_px(crd::f32 px) noexcept;

    // Minimum CAMERA-projected height, in pixels, below which an instance does not draw AT ALL (camera view +
    // impostor list; the caster threshold above governs the cascades). Sub-pixel geometry cannot contribute
    // anything but aliasing energy — a triangle smaller than a pixel is pure shimmer — so every GPU-driven
    // engine culls it. Default 1.5; 0 disables. Applied by the CPU camera cull and the device kernels alike.
    void set_min_draw_px(crd::f32 px) noexcept;

    // ⭐⭐ REN-41 (TAA): supply this frame's reproject matrix R = prev_UNJITTERED_vp · inv(cur_JITTERED_vp) and
    // whether a previous frame exists. The caller owns both projections (it applies the sub-pixel jitter), so it
    // computes R; the renderer only uploads it for the TAA resolve pass. Call before render() each frame.
    void set_taa_reproj(const crd::math::Mat4f& reproj, bool has_history) noexcept;

    // REN-40-E2's round-robin far-cascade scheduling (cascades 2-3 alternate updates on even/odd frames).
    // ⛔ OFF by default: under continuous camera motion it serves a one-frame-STALE matrix every other frame,
    // which strobes every shadow in the far half of the range — an artifact, not an optimization. The
    // exact-match matrix cache (40-E1) already skips static cascades for free; enable this only for scenes
    // whose camera is mostly stationary and whose far-field shadow budget matters more than motion quality.
    void set_cascade_round_robin(bool on) noexcept;

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

// Extract the 6 world-space frustum planes (ax+by+cz+d ≥ 0 = inside) from a view-projection matrix
// (Gribb–Hartmann; clip z in [0,1] — the Vulkan/reverse-Z convention). Plane order: L R B T N F.
void frustum_planes(const crd::math::Mat4f& view_proj, crd::math::Vec4f out_planes[6]);

// Positive-vertex AABB-vs-plane-set test: true iff `box` intersects the frustum described by 6 planes.
[[nodiscard]] bool aabb_in_frustum(const crd::geometry::primitives::AABB3<crd::f32>& box,
                                   const crd::math::Vec4f planes[6]) noexcept;

// The world-space AABB of the frustum (the BVH broad-phase query box): the 8 clip corners through the inverse
// view-projection. Degenerate matrices yield an everything-box (the broad phase then prunes nothing — safe).
[[nodiscard]] crd::geometry::primitives::AABB3<crd::f32> frustum_aabb(const crd::math::Mat4f& view_proj);

// ⭐⭐ CEIR-18a-2 Stage 2b (Forward+ GPU producer): the FROXEL-AABB table — the world-space AABB of each screen tile's
// frustum wedge, filled into `out` (gx*gy*6 floats: per tile min.xyz then max.xyz, cluster index = ty*gx + tx). Each
// tile's 4 screen corners are unprojected at TWO ndc depths through inverse(view_proj): the near plane (ndc_z = 1, the
// reverse-Z convention) and a FINITE far (ndc_z = `ndc_z_far`) — a substitute for perspective_reverse_z's INFINITE far
// (ndc_z = 0 there unprojects to a degenerate w≈0), so `ndc_z_far` must be a small positive value whose world far plane
// lies beyond the scene's lights. ⛔⛔ THE BACKEND NDC±Y MIRROR: the FS tiles by FragCoord (post-viewport), whose y↔ndc
// sign DIFFERS per backend (Vulkan ndc_y=-1 top, DX12 ndc_y=+1 top), so the froxel's tile→ndc-y mapping needs the SAME
// per-backend sign — `flip_y` negates it (pass `!IRasterContext::ndc_y_points_down()`, the velocity-spec-const precedent).
// The 2b-iv DX12 gate PROVED it: one sign lit the top on Vulkan and the bottom on DX12. X is consistent (no flip there).
void compute_froxel_aabbs(const crd::math::Mat4f& view_proj, crd::u32 gx, crd::u32 gy, crd::f32 ndc_z_far, bool flip_y,
                          crd::f32* out) noexcept;

// ⭐⭐ CEIR-18b: the 3D CLUSTERED extension — per-slice froxel AABBs (out_aabb: gx*gy*gz*6 floats, cluster c = tz*gx*gy +
// ty*gx + tx) PLUS the N+1 EXPONENTIAL z-slice boundary table (out_bounds: gz+1 floats, in clip.w terms) the renderer
// publishes to header words kHdrSliceBoundsOff..+gz. The FS bins by counting how many interior boundaries its clip.w exceeds
// (the Step-sum), so the AABB cuts and the FS binning use the SAME table BY CONSTRUCTION. z needs NO per-backend flip (clip.w
// is backend-neutral); x/y carry the SAME flip_y as compute_froxel_aabbs. See the .cpp for the clip.w-lerp construction.
void compute_froxel_slices(const crd::math::Mat4f& view_proj, crd::u32 gx, crd::u32 gy, crd::u32 gz, crd::f32 ndc_z_far,
                           bool flip_y, crd::f32* out_aabb, crd::f32* out_bounds) noexcept;

} // namespace crd::scenerender
