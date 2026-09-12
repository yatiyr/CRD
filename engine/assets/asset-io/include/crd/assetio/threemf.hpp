#pragma once

// threemf.hpp — GEO-5 pt 3 (D-007): OUR 3MF core-spec parser + model writer — the manufacturing/slicer pillar
// (STL's successor; the 3D-printing world's native format). Rides the owned foundations: the XML parser (xml.hpp)
// and, at the CALLER's layer, the ZIP container (crd-resources' zip_archive — this parser takes the PRE-EXTRACTED
// model part's bytes, the glTF "caller does I/O" posture; the cooker bridges OPC discovery).
//
// Import scope (core spec): <model unit> (all six legal units → SI metres) · <resources>/<object type="model">
// meshes (vertices/triangles, per-object) · <basematerials> (displaycolor #RRGGBB[AA] sRGB → LINEAR base_color —
// the color-space contract, applied at parse so downstream matches glTF's linear factors) · <build>/<item>
// (objectid + the row-major 4×3 affine `transform` → TRS via the SAME decompose the glTF importer uses — one
// extraction, zero cross-format drift) · <components> flattened as child nodes. `requiredextensions` we cannot
// honor ⇒ ImportStatus::Unsupported BY NAME (never a silently-wrong print).
//
// Export: `threemf_write_model_xml` emits the core-spec model part from cooked-layout geometry — GATED on
// crd-geometry's watertightness verdict (validate_triangle_mesh: well_formed AND zero boundary edges). Slicers
// assume outward-facing positive-volume solids; a holed mesh is REFUSED, never shipped. The caller wraps the three
// OPC parts ([Content_Types].xml · _rels/.rels · 3D/3dmodel.model) with the ZIP writer.

#include <crd/assetio/imported_asset.hpp>
#include <crd/containers/span.hpp>
#include <crd/containers/string.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::assetio
{

// Parse a 3MF model part (the XML document inside the OPC package). Positions scale to SI metres by the declared
// unit. One ImportedMesh per <object type="model"> mesh; build items become ImportedNode roots referencing them.
[[nodiscard]] ImportStatus parse_3mf_model(crd::containers::ConstSpan<crd::u8> xml_bytes,
                                           crd::memory::IAllocator* alloc, ImportedAsset& out);

// The canonical OPC part names an exporter emits (and an importer may fall back to when no .rels resolves).
inline constexpr const char* k3mfModelPart    = "3D/3dmodel.model";
inline constexpr const char* k3mfContentTypes = "[Content_Types].xml";
inline constexpr const char* k3mfRels         = "_rels/.rels";

// The fixed OPC boilerplate parts (spec-mandated content; deterministic byte-for-byte).
[[nodiscard]] const char* threemf_content_types_xml() noexcept;
[[nodiscard]] const char* threemf_rels_xml() noexcept;

// Emit the model part from cooked-layout geometry (a 48-byte interleaved vertex stream + u32 indices — positions in
// SI metres, written under unit="meter"). REFUSES (returns false, `out` empty) when the mesh is not WATERTIGHT per
// crd-geometry's validator — boundary edges or critical defects mean the solid cannot slice; refusal over a corrupt
// print. `name` labels the object (nullptr → "mesh").
[[nodiscard]] bool threemf_write_model_xml(crd::containers::ConstSpan<crd::u8> vertices48,
                                           crd::containers::ConstSpan<crd::u8> indices_u32, const char* name,
                                           crd::memory::IAllocator* alloc, crd::containers::String& out);

} // namespace crd::assetio
