#pragma once

// gltf_export.hpp — GEO-4 (D-007): the FIRST EXPORTER — native per-type resources RE-BUNDLE into glTF 2.0 (.glb),
// the delivery half of the industry's USD-authoring/glTF-delivery split. The mirror of the import seam: callers FILL
// an `ExportAsset` view from LOADED native resources (MeshResource 48-byte streams · PbrmParams · TXTR pixels · the
// SCEN node table) and the writer re-bundles — asset-io never loads resources itself (the same "parsers only fill,
// cook only reads" discipline, reversed).
//
// Geometry contract: meshes arrive as the COOKED interleaved 48-byte vertex stream (position f32x3 · normal f32x3 ·
// uv f32x2 · tangent f32x4, ADR-0078 — SI metres, glTF's own unit) + u32 indices; the writer de-interleaves into
// tightly-packed accessors (POSITION/NORMAL/TEXCOORD_0/TANGENT + min/max as the spec requires). Materials emit
// pbrMetallicRoughness + KHR_materials_{ior, transmission, emissive_strength} exactly as the importer reads them —
// OUR importer is the exporter's round-trip validator, and vice versa.

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::assetio
{

struct ExportMaterial
{
    const char* name = nullptr; // optional

    float base_color[4]      = {1.0F, 1.0F, 1.0F, 1.0F};
    float metallic           = 1.0F;
    float roughness          = 1.0F;
    float emissive[3]        = {0.0F, 0.0F, 0.0F};
    float emissive_strength  = 1.0F; // KHR_materials_emissive_strength when != 1
    float ior                = 1.5F; // KHR_materials_ior when != 1.5
    float transmission       = 0.0F; // KHR_materials_transmission when > 0

    // image indices into ExportAsset::images (-1 = unbound). GEO-4 pt 2 embeds them as PNG.
    int   base_color_image   = -1;
    int   metallic_roughness_image = -1;
    int   normal_image       = -1;
    float normal_scale       = 1.0F;
    int   occlusion_image    = -1;
    float occlusion_strength = 1.0F;
    int   emissive_image     = -1;
};

// One drawable primitive: the cooked 48-byte interleaved vertex stream + a u32 index stream.
struct ExportMesh
{
    const char*                     name = nullptr;
    crd::containers::ConstSpan<crd::u8> vertices; // N × 48 bytes (kMeshVertexStride)
    crd::containers::ConstSpan<crd::u8> indices;  // M × 4 bytes (u32)
    int                             material = -1; // index into ExportAsset::materials
};

// A scene node. `parent` = index into ExportAsset::nodes (-1 = root); the writer derives children arrays.
struct ExportNode
{
    const char* name           = nullptr;
    float       translation[3] = {0.0F, 0.0F, 0.0F};
    float       rotation[4]    = {0.0F, 0.0F, 0.0F, 1.0F}; // xyzw
    float       scale[3]       = {1.0F, 1.0F, 1.0F};
    int         mesh           = -1; // index into ExportAsset::meshes
    int         parent         = -1;
};

// An image to embed: PRE-ENCODED PNG bytes (the caller encodes via crd::resources::png_encode_rgba — asset-io stays
// the pure I/O layer and never links the codec stack; the same "callers fill, the writer re-bundles" discipline).
struct ExportImage
{
    const char*                         name = nullptr;
    crd::containers::ConstSpan<crd::u8> png; // a complete PNG file's bytes, embedded verbatim into the BIN chunk
};

struct ExportAsset
{
    crd::containers::Array<ExportMesh>     meshes;
    crd::containers::Array<ExportMaterial> materials;
    crd::containers::Array<ExportNode>     nodes;
    crd::containers::Array<ExportImage>    images;

    explicit ExportAsset(crd::memory::IAllocator* a) : meshes(a), materials(a), nodes(a), images(a) {}
};

// Write `asset` as a single self-contained .glb (JSON chunk + BIN chunk). Returns false on structural invalidity
// (a vertex span not a multiple of 48, an index span not a multiple of 4, an out-of-range material/mesh/parent or
// image reference, a non-finite float — refusal over a corrupt file). `out` is replaced.
[[nodiscard]] bool gltf_export_glb(const ExportAsset& asset, crd::containers::Array<crd::u8>& out,
                                   crd::memory::IAllocator* alloc);

} // namespace crd::assetio
