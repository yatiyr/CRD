#pragma once

// gltf.hpp — GEO-3: OUR OWN glTF 2.0 parser (the delivery-format edge of the decompose-on-import pipeline). This layer
// covers the GEOMETRY + MATERIAL-PARAMETER surface: GLB container · buffers (embedded / base64 data-URI / one external)
// · bufferViews (strided) · accessors (all component types, normalized, SPARSE substitution) · triangle primitives
// (POSITION/NORMAL/TEXCOORD_0/authored TANGENT + u8/u16/u32 indices) · pbrMetallicRoughness + the KHR extensions that
// map straight onto the OpenPBR slab (emissive_strength · ior · transmission) · images (stage 2b: embedded bufferView /
// data-URI bytes or percent-decoded external uris — ENCODED bytes only, the cook decodes via ldr_decode) · material
// texture SLOTS (baseColor/metallicRoughness/normal+scale/occlusion+strength/emissive → ImportedImage indices; the slot
// decides sRGB-vs-linear at cook). The scene-graph → SCEN decompose is the next GEO-3 stage; nodes/scenes are
// intentionally NOT baked into geometry — meshes import as the un-transformed mesh LIBRARY (per the decompose
// philosophy: the scene REFERENCES meshes).
//
// One ImportedMesh per glTF primitive (a primitive is the material-binding granularity — our render-submission unit).
// Non-triangle modes skip with a warning. Zero 3rd-party; span-based; allocator-only (replaces the cgltf path).

#include <crd/assetio/imported_asset.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::assetio
{

// Parse a GLB (binary glTF) container: 12-byte header + JSON chunk + optional BIN chunk. Self-contained.
[[nodiscard]] ImportStatus parse_glb(crd::containers::ConstSpan<crd::u8> bytes, crd::memory::IAllocator* alloc,
                                     ImportedAsset& out);

// Parse .gltf JSON text. `external_bin` backs `buffers[0]` when its uri is a plain file reference (the CALLER does the
// file I/O — the codec posture); base64 data-URIs decode internally. More than one external buffer → Malformed (honest:
// multi-bin assets are rare; widen when a real one appears).
[[nodiscard]] ImportStatus parse_gltf(crd::containers::ConstSpan<crd::u8> json_bytes,
                                      crd::containers::ConstSpan<crd::u8> external_bin, crd::memory::IAllocator* alloc,
                                      ImportedAsset& out);

} // namespace crd::assetio
