#pragma once

// imported_asset.hpp — GEO-1 (D-007 row 66): the IMPORT SEAM. `ImportedAsset` is the in-memory intermediate model every
// external-format parser fills and everything downstream (decompose → condition → cook) reads — parsers only FILL it,
// consumers only READ it, so adding a format changes nothing downstream. Per the GEO-band doctrine
// (docs/research/2026-07-23-geometry-resource-pipeline.md + -resource-scene-world-class.md): bundle formats are INTERCHANGE
// ONLY — an import DECOMPOSES into native per-type resources; this struct is the staging ground for that decomposition,
// never a storage format. All parsers are OUR OWN (zero 3rd-party), allocator-aware, span-based (no filesystem here — the
// caller does file I/O and hands us bytes, the same posture as the HDR/EXR codecs).
//
// Units/axis policy: parsers deliver values AS AUTHORED (STL has no units; OBJ/PLY rarely declare them). The SI-unit
// scale + axis convention is applied at COOK time from the per-source `.meta` (ADR-0078 §2 D18 `position_scale`), never
// silently inside a parser.

#include <crd/containers/array.hpp>
#include <crd/containers/string.hpp>
#include <crd/core/types.hpp>
#include <crd/geometry/mesh/triangle_mesh.hpp>
#include <crd/math/vec.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::assetio
{

// Parse outcome. `Ok` may still carry warnings (see ImportedAsset::warning_count) — a dirty-but-recoverable file imports
// with warnings; an unrecoverable one gets a precise failure class (the caller can report WHICH way a file is broken).
enum class ImportStatus : crd::u8
{
    Ok = 0,
    NotRecognized, // the bytes are not this format at all (magic/structure mismatch) — try another parser
    Truncated,     // the structure is right but the bytes end early (a partial download / corrupt file)
    Malformed,     // the structure is violated mid-stream (bad token, index out of range, count mismatch)
    NonFiniteData, // a position/normal contains NaN/Inf — geometry poison, never let it into the pipeline
};

[[nodiscard]] constexpr const char* import_status_name(ImportStatus s) noexcept
{
    switch (s)
    {
    case ImportStatus::Ok: return "Ok";
    case ImportStatus::NotRecognized: return "NotRecognized";
    case ImportStatus::Truncated: return "Truncated";
    case ImportStatus::Malformed: return "Malformed";
    case ImportStatus::NonFiniteData: return "NonFiniteData";
    default: return "?";
    }
}

// One imported mesh: indexed triangles + optional per-vertex attributes. Attribute arrays are either EMPTY or exactly
// `positions.size()` long (the parser guarantees it; `is_consistent` checks it). Triangulation is the PARSER's job —
// downstream only ever sees triangles. No default ctor (Array<ImportedMesh> uses push_back, the TextureResource pattern).
struct ImportedMesh
{
    crd::containers::String                           name;
    crd::containers::Array<crd::math::Vec3<crd::f32>> positions;
    crd::containers::Array<crd::math::Vec3<crd::f32>> normals; // empty or per-vertex
    crd::containers::Array<crd::math::Vec2<crd::f32>> uv0;     // empty or per-vertex
    crd::containers::Array<crd::u32>                  indices;  // 3 per triangle, into positions
    crd::i32                                          material = -1; // index into ImportedAsset::materials, -1 = none
    // GEO-2 (appended): the MikkTSpace-compatible tangent frame (xyz + bitangent-sign w), produced by
    // `generate_tangents` — a DERIVED attribute (conditioning owns it; weld/normal passes drop it for regeneration).
    crd::containers::Array<crd::math::Vec4<crd::f32>> tangent; // empty or per-vertex
    // GEO-3 stage 3 (appended): the source-format mesh-library index this primitive came from (glTF: meshes[i] —
    // one glTF mesh fans out into one ImportedMesh per primitive; nodes reference the LIBRARY index). -1 = the
    // source has no mesh library (STL/OBJ/PLY).
    crd::i32 source_mesh = -1;

    explicit ImportedMesh(crd::memory::IAllocator* a) : name(a), positions(a), normals(a), uv0(a), indices(a), tangent(a) {}

    ImportedMesh(const ImportedMesh&)            = delete;
    ImportedMesh& operator=(const ImportedMesh&) = delete;
    ImportedMesh(ImportedMesh&&)                 = default;
    ImportedMesh& operator=(ImportedMesh&&)      = default;

    [[nodiscard]] crd::u32 triangle_count() const noexcept { return static_cast<crd::u32>(indices.size() / 3U); }
    [[nodiscard]] bool     has_normals() const noexcept { return normals.size() == positions.size() && positions.size() > 0U; }
    [[nodiscard]] bool     has_uv0() const noexcept { return uv0.size() == positions.size() && positions.size() > 0U; }

    // Attribute arrays are per-vertex-or-absent, and every index is in range. Parsers must return meshes for which this
    // holds; the import tests gate on it.
    [[nodiscard]] bool is_consistent() const noexcept
    {
        if (normals.size() != 0U && normals.size() != positions.size()) { return false; }
        if (uv0.size() != 0U && uv0.size() != positions.size()) { return false; }
        if (tangent.size() != 0U && tangent.size() != positions.size()) { return false; }
        if ((indices.size() % 3U) != 0U) { return false; }
        for (crd::usize i = 0; i < indices.size(); ++i)
        {
            if (indices[i] >= positions.size()) { return false; }
        }
        return true;
    }

    // The crd-geometry view — the seam into the validate/repair/processing substrate (mesh_validate, winding number, BVH…).
    [[nodiscard]] crd::geometry::mesh::TriangleMeshViewf as_view() const noexcept
    {
        return crd::geometry::mesh::TriangleMeshViewf{
            crd::containers::ConstSpan<crd::math::Vec3<crd::f32>>(positions.data(), positions.size()),
            crd::containers::ConstSpan<crd::u32>(indices.data(), indices.size())};
    }
};

// A material as AUTHORED by the source file (OBJ/MTL now; glTF PBR at GEO-3 widens this). Downstream AUTHORS a native
// OpenPBR MaterialTemplate from these parameters — foreign material representations never travel past the import seam.
struct ImportedMaterial
{
    crd::containers::String   name;
    crd::math::Vec3<crd::f32> base_color{1.0F, 1.0F, 1.0F};
    crd::f32                  metallic  = 0.0F;
    crd::f32                  roughness = 1.0F;
    // GEO-3 (appended): the glTF PBR + KHR-extension parameters that map DIRECTLY onto the OpenPBR slab (B5) — captured
    // here so material AUTHORING (onto our MaterialTemplates) sees the full surface, never a flattened subset.
    crd::f32                  base_alpha        = 1.0F; // baseColorFactor.a
    crd::math::Vec3<crd::f32> emissive{0.0F, 0.0F, 0.0F};
    crd::f32                  emissive_strength = 1.0F; // KHR_materials_emissive_strength
    crd::f32                  ior               = 1.5F; // KHR_materials_ior
    crd::f32                  transmission      = 0.0F; // KHR_materials_transmission

    // GEO-3 stage 2b (appended): texture SLOT references — indices into ImportedAsset::images, -1 = none. The SLOT
    // decides the color space at cook time (baseColor/emissive = sRGB-authored COLOR; normal/metallicRoughness/
    // occlusion = LINEAR data) — the classic silent-decode bug made structural: usage is recorded here, the cook
    // derives the transfer function from it, nothing ever guesses from file contents.
    crd::i32 base_color_image = -1;
    crd::i32 mr_image         = -1; // metallicRoughness (glTF packing: G=roughness, B=metallic)
    crd::i32 normal_image     = -1;
    crd::i32 occlusion_image  = -1;
    crd::i32 emissive_image   = -1;
    crd::f32 normal_scale       = 1.0F; // glTF normalTexture.scale
    crd::f32 occlusion_strength = 1.0F; // glTF occlusionTexture.strength

    explicit ImportedMaterial(crd::memory::IAllocator* a) : name(a) {}

    ImportedMaterial(const ImportedMaterial&)            = delete;
    ImportedMaterial& operator=(const ImportedMaterial&) = delete;
    ImportedMaterial(ImportedMaterial&&)                 = default;
    ImportedMaterial& operator=(ImportedMaterial&&)      = default;
};

// GEO-3 stage 2b (appended): an image REFERENCED by the source (glTF images[] today; other formats join as they land).
// EXACTLY ONE of `bytes` (embedded: GLB bufferView / base64 data-URI) or `uri` (external file, PERCENT-DECODED) is
// non-empty. Parsers never do file I/O and never decode pixels — the ENCODED bytes travel to the cook, where
// `ldr_decode` (our owned codec family) turns them into a TextureResource with the slot-derived color space.
struct ImportedImage
{
    crd::containers::String         name;
    crd::containers::String         uri;   // external reference relative to the source file; EMPTY when embedded
    crd::containers::Array<crd::u8> bytes; // encoded image file bytes (PNG/JPEG/…); EMPTY when external

    explicit ImportedImage(crd::memory::IAllocator* a) : name(a), uri(a), bytes(a) {}

    ImportedImage(const ImportedImage&)            = delete;
    ImportedImage& operator=(const ImportedImage&) = delete;
    ImportedImage(ImportedImage&&)                 = default;
    ImportedImage& operator=(ImportedImage&&)      = default;
};

// GEO-3 stage 3 (appended): one scene-graph node. TRS as authored (quaternion xyzw; matrix nodes are DECOMPOSED by
// the parser — downstream only ever sees TRS). Indices reference the LIBRARY tables: `mesh` = the source mesh-library
// index (ImportedMesh::source_mesh matches it), `camera`/`light` index ImportedAsset::cameras/lights, -1 = none.
struct ImportedNode
{
    crd::containers::String   name;
    crd::math::Vec3<crd::f32> translation{0.0F, 0.0F, 0.0F};
    crd::math::Vec4<crd::f32> rotation{0.0F, 0.0F, 0.0F, 1.0F}; // quaternion xyzw, identity default
    crd::math::Vec3<crd::f32> scale{1.0F, 1.0F, 1.0F};
    crd::i32                  mesh   = -1;
    crd::i32                  camera = -1;
    crd::i32                  light  = -1;
    crd::containers::Array<crd::u32> children; // node indices

    explicit ImportedNode(crd::memory::IAllocator* a) : name(a), children(a) {}

    ImportedNode(const ImportedNode&)            = delete;
    ImportedNode& operator=(const ImportedNode&) = delete;
    ImportedNode(ImportedNode&&)                 = default;
    ImportedNode& operator=(ImportedNode&&)      = default;
};

// GEO-3 stage 3 (appended): a camera as authored (glTF surface). Angles RADIANS; zfar 0 = infinite; aspect 0 = viewport.
struct ImportedCamera
{
    crd::containers::String name;
    bool                    is_ortho = false;
    crd::f32                yfov     = 1.0F;
    crd::f32                aspect   = 0.0F;
    crd::f32                znear    = 0.1F;
    crd::f32                zfar     = 0.0F;
    crd::f32                xmag     = 0.0F;
    crd::f32                ymag     = 0.0F;

    explicit ImportedCamera(crd::memory::IAllocator* a) : name(a) {}

    ImportedCamera(const ImportedCamera&)            = delete;
    ImportedCamera& operator=(const ImportedCamera&) = delete;
    ImportedCamera(ImportedCamera&&)                 = default;
    ImportedCamera& operator=(ImportedCamera&&)      = default;
};

// GEO-3 stage 3 (appended): a punctual light as authored (KHR_lights_punctual surface). Linear RGB; KHR intensity
// units (candela for point/spot, lux for directional); range 0 = unlimited; cone angles RADIANS.
struct ImportedLight
{
    crd::containers::String   name;
    crd::u8                   type = 0; // 0 directional · 1 point · 2 spot
    crd::math::Vec3<crd::f32> color{1.0F, 1.0F, 1.0F};
    crd::f32                  intensity  = 1.0F;
    crd::f32                  range      = 0.0F;
    crd::f32                  inner_cone = 0.0F;
    crd::f32                  outer_cone = 0.78539816339F; // KHR default π/4

    explicit ImportedLight(crd::memory::IAllocator* a) : name(a) {}

    ImportedLight(const ImportedLight&)            = delete;
    ImportedLight& operator=(const ImportedLight&) = delete;
    ImportedLight(ImportedLight&&)                 = default;
    ImportedLight& operator=(ImportedLight&&)      = default;
};

// The parse product: meshes + materials + a warning tally. Scenes/lights/cameras/skins join at GEO-3 (glTF) — appended
// fields, never reordered (the append-only stability rule).
struct ImportedAsset
{
    crd::containers::Array<ImportedMesh>     meshes;
    crd::containers::Array<ImportedMaterial> materials;
    crd::u32                                 warning_count = 0; // recoverable oddities the parser papered over
    // GEO-3 stage 2b (appended): the image library the material slots index into.
    crd::containers::Array<ImportedImage> images;
    // GEO-3 stage 3 (appended): the scene graph — nodes + the default scene's roots + camera/light libraries.
    // Meshes stay an untransformed LIBRARY (the decompose philosophy): nodes REFERENCE them via source_mesh.
    crd::containers::Array<ImportedNode>   nodes;
    crd::containers::Array<crd::u32>       roots; // node indices of the default scene
    crd::containers::Array<ImportedCamera> cameras;
    crd::containers::Array<ImportedLight>  lights;

    explicit ImportedAsset(crd::memory::IAllocator* a)
        : meshes(a), materials(a), images(a), nodes(a), roots(a), cameras(a), lights(a)
    {
    }

    ImportedAsset(const ImportedAsset&)            = delete;
    ImportedAsset& operator=(const ImportedAsset&) = delete;
    ImportedAsset(ImportedAsset&&)                 = default;
    ImportedAsset& operator=(ImportedAsset&&)      = default;
};

} // namespace crd::assetio
