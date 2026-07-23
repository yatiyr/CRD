// mesh_wave1.cpp — GEO-1 (D-007 row 66): the Wave-1 OWN-PARSER mesh cook handler (.stl / .obj / .ply, GEO-3 adds
// .glb / .gltf, GEO-5 adds .3mf).
//
// crd-asset-io parses (OUR parsers, zero 3rd-party) → the crd-geometry VALIDATE hook (validate_triangle_mesh) → the
// 48-byte interleave (position × .meta position_scale into SI metres, ADR-0078) → the SAME MESH CRDR artifact layout the
// glTF path emits (VERT / INDX / PRIM), so the EXISTING mesh_resource_loader + ResourceManager load it unchanged.
// Multi-mesh sources (OBJ `o`/`usemtl` splits) mirror the glTF sidecar-.meta extra-artifact pattern so ids stay STABLE
// across recooks. Tangents are ZERO (w=+1) until GEO-2 lands our own MikkTSpace-compatible generator — the cgltf +
// mikktspace.c usage in mesh.cpp is the 3rd-party legacy the GEO band retires (GEO-2/GEO-3 replace it; it stays only as
// the comparison oracle).
//
// Validate policy (the GEO-1 "hook", not yet the GEO-2 "repair"): OutOfBoundsIndex → HARD FAIL (defensive; the parser
// contract already forbids it). Degenerate/zero-area triangles → warn + cook (repair is GEO-2 conditioning). Boundary
// edges are NOT reported — STL soup + open scans make them the norm pre-weld, not a defect.

#include <crd/assetio/condition.hpp>
#include <crd/assetio/gltf.hpp>
#include <crd/assetio/imported_asset.hpp>
#include <crd/assetio/obj.hpp>
#include <crd/assetio/ply.hpp>
#include <crd/assetio/stl.hpp>
#include <crd/assetio/threemf.hpp>
#include <crd/assetio/xml.hpp>
#include <crd/resources/zip_archive.hpp>

#include <crd/cooker/cook_handler.hpp>
#include <crd/cooker/cook_io.hpp>
#include <crd/cooker/mesh_cook_options.hpp>
#include <crd/cooker/texture_cook.hpp>

#include <crd/math/quat.hpp>
#include <crd/resources/ldr_image.hpp>
#include <crd/resources/openpbr_material.hpp>
#include <crd/scene/relation.hpp>
#include <crd/scene/render_components.hpp>
#include <crd/scene/scene_resource.hpp>
#include <crd/scene/serialize.hpp>
#include <crd/scene/system.hpp>
#include <crd/scene/transform.hpp>
#include <crd/scene/transform_propagation.hpp>
#include <crd/scene/world.hpp>

#include <memory>

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/containers/string.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/geometry/mesh/mesh_validate.hpp>
#include <crd/memory/allocators/malloc_allocator.hpp>
#include <crd/resources/crdr.hpp>
#include <crd/resources/resource_id.hpp>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string_view>

namespace crd::cooker
{

namespace
{

// v2 (GEO-6): reads flow through CookIO — sidecar ids via stable_id, external inputs recorded as edges
constexpr crd::u32 kWave1HandlerVersion = 2U;
constexpr crd::u32 kWave1VertexStride   = 48U; // float3 pos + float3 norm + float2 uv + float4 tan (ADR-0043)

crd::containers::String sanitize_name(const char* name, crd::memory::IAllocator* alloc, const char* fallback = "mesh")
{
    crd::containers::String s(alloc);
    if (name == nullptr || name[0] == '\0')
    {
        s.append(fallback);
        return s;
    }
    for (const char* p = name; *p != '\0'; ++p)
    {
        const char c       = *p;
        const bool keep_it = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
        s.push_back(keep_it ? c : '_');
    }
    return s;
}

// ── source-format dispatch ──────────────────────────────────────────────────────────────────────────────────────────────

enum class Wave1Format : crd::u8
{
    Stl,
    Obj,
    Ply,
    Glb,  // GEO-3: OUR glTF parser (replaces the cgltf path — first-wins registration)
    Gltf,
    ThreeMf, // GEO-5 pt 3: 3MF (OPC/ZIP package; the cooker bridges package discovery to the model-part parser)
    Unknown,
};

[[nodiscard]] bool ends_with_icase(crd::containers::StringView path, const char* suffix) noexcept
{
    const crd::usize sn = std::strlen(suffix);
    if (path.size() < sn) { return false; }
    const char* p = path.data() + (path.size() - sn);
    for (crd::usize i = 0; i < sn; ++i)
    {
        char a = p[i];
        if (a >= 'A' && a <= 'Z') { a = static_cast<char>(a + 32); }
        if (a != suffix[i]) { return false; }
    }
    return true;
}

[[nodiscard]] Wave1Format detect_format(crd::containers::StringView path) noexcept
{
    if (ends_with_icase(path, ".stl")) { return Wave1Format::Stl; }
    if (ends_with_icase(path, ".obj")) { return Wave1Format::Obj; }
    if (ends_with_icase(path, ".ply")) { return Wave1Format::Ply; }
    if (ends_with_icase(path, ".glb")) { return Wave1Format::Glb; }
    if (ends_with_icase(path, ".gltf")) { return Wave1Format::Gltf; }
    if (ends_with_icase(path, ".3mf")) { return Wave1Format::ThreeMf; }
    return Wave1Format::Unknown;
}

// ── GEO-5 pt 3: the 3MF OPC bridge (the cooker owns package discovery; the parser takes the model part's bytes) ────────
//
// `_rels/.rels` names the 3D Model start part (the OPC root relationship — Type suffix "/3dmodel"); a missing or
// unhelpful rels part falls back to the canonical "3D/3dmodel.model" name every mainstream producer emits.
[[nodiscard]] bool extract_3mf_model_part(crd::containers::ConstSpan<crd::u8> zip_bytes, crd::memory::IAllocator* alloc,
                                          crd::containers::Array<crd::u8>& out_model)
{
    crd::resources::ZipReader zip(alloc);
    if (zip.open(zip_bytes) != crd::resources::ZipError::Ok) { return false; }

    crd::containers::String part_name(alloc);
    const crd::i64          rels_index = zip.find(crd::assetio::k3mfRels);
    if (rels_index >= 0)
    {
        crd::containers::Array<crd::u8> rels_bytes(alloc);
        if (zip.extract(static_cast<crd::usize>(rels_index), rels_bytes) == crd::resources::ZipError::Ok)
        {
            crd::assetio::XmlDoc rels(alloc);
            if (rels.parse(crd::containers::as_const_span(rels_bytes)) == crd::assetio::XmlError::Ok)
            {
                for (crd::i32 rel = rels.child(rels.root(), "Relationship"); rel != crd::assetio::kXmlInvalid;
                     rel = rels.sibling(rel, "Relationship"))
                {
                    const char* type   = rels.attr(rel, "Type");
                    const char* target = rels.attr(rel, "Target");
                    if (type == nullptr || target == nullptr) { continue; }
                    if (!ends_with_icase(crd::containers::StringView(type, std::strlen(type)), "/3dmodel"))
                    {
                        continue;
                    }
                    if (target[0] == '/') { ++target; } // OPC part names are package-absolute; ZIP names are not
                    part_name.append(target);
                    break;
                }
            }
        }
    }
    if (part_name.size() == 0U) { part_name.append(crd::assetio::k3mfModelPart); }

    const crd::i64 model_index = zip.find(part_name.c_str());
    if (model_index < 0) { return false; }
    return zip.extract(static_cast<crd::usize>(model_index), out_model) == crd::resources::ZipError::Ok;
}

// ── the artifact build: ImportedMesh → validate → interleave → MESH CRDR ───────────────────────────────────────────────

crd::containers::Array<crd::u8> build_wave1_artifact(crd::assetio::ImportedMesh& mesh,
                                                     const crd::resources::ResourceId& id,
                                                     crd::memory::IAllocator* alloc, const MeshCookOptions& options)
{
    crd::containers::Array<crd::u8> empty(alloc);
    if (mesh.positions.size() == 0U || mesh.indices.size() == 0U) { return empty; }

    // GEO-2 CONDITIONING: weld (soup → indexed; hard edges preserved by the exact tuple key) → generate normals when the
    // source has none (crease-angle smoothing, `.meta smooth_angle_deg`, production default 30°) → OUR MikkTSpace-
    // compatible tangents when UVs exist (with mirror-seam splitting). All bit-stable under face reordering.
    // GEO-3 rule: FULLY-AUTHORED meshes (glTF with TANGENT) pass through UNTOUCHED — authored data is never regenerated
    // (welding drops derived tangents, so an authored frame must skip the whole conditioning chain).
    if (mesh.tangent.size() != mesh.positions.size())
    {
        (void)crd::assetio::weld_exact(mesh, alloc);
        if (!mesh.has_normals())
        {
            constexpr crd::f32 pi = 3.14159265358979F;
            crd::assetio::generate_normals(mesh, alloc, options.smooth_angle_deg * pi / 180.0F);
        }
        if (mesh.has_uv0() && mesh.has_normals()) { (void)crd::assetio::generate_tangents(mesh, alloc); }
    }

    const crd::u32 vc = static_cast<crd::u32>(mesh.positions.size());
    const crd::u32 ic = static_cast<crd::u32>(mesh.indices.size());
    if (vc == 0U || ic == 0U) { return empty; }

    // the crd-geometry VALIDATE hook: hard-fail on out-of-bounds; warn-and-cook on authoring smells (repair = GEO-2)
    {
        crd::geometry::mesh::MeshValidationOptions vopts;
        vopts.report_boundary_edges = false; // soup/open scans: boundary is the pre-weld norm, not a defect
        const crd::geometry::mesh::MeshValidationReport report =
            crd::geometry::mesh::validate_triangle_mesh(mesh.as_view(), alloc, vopts);
        crd::u32 degenerate = 0U;
        for (crd::usize d = 0; d < report.defects.size(); ++d)
        {
            const crd::geometry::mesh::MeshDefect& def = report.defects[d];
            if (def.kind == crd::geometry::mesh::MeshDefectKind::OutOfBoundsIndex)
            {
                std::fprintf(stderr, "mesh_wave1: OUT-OF-BOUNDS index (tri %u -> vertex %u) — refusing to cook\n", def.a,
                             def.b);
                return empty;
            }
            if (def.kind == crd::geometry::mesh::MeshDefectKind::DegenerateTriangle
                || def.kind == crd::geometry::mesh::MeshDefectKind::ZeroAreaTriangle)
            {
                ++degenerate;
            }
        }
        if (degenerate > 0U)
        {
            std::fprintf(stderr, "mesh_wave1: %u degenerate/zero-area triangle(s) — cooked as-is (GEO-2 repairs)\n",
                         degenerate);
        }
    }

    const bool has_n  = mesh.has_normals();
    const bool has_uv = mesh.has_uv0();

    crd::containers::Array<crd::u8> vert_buf(alloc);
    vert_buf.resize(static_cast<crd::usize>(vc) * kWave1VertexStride);
    crd::u8*       dst    = vert_buf.data();
    const crd::f32 pscale = options.position_scale;
    bool           si_warned = false;
    for (crd::u32 vi = 0U; vi < vc; ++vi, dst += kWave1VertexStride)
    {
        const crd::f32 pos[3] = {mesh.positions[vi].x * pscale, mesh.positions[vi].y * pscale,
                                 mesh.positions[vi].z * pscale};
        if (!si_warned
            && (std::fabs(pos[0]) > kSiPositionSanityMeters || std::fabs(pos[1]) > kSiPositionSanityMeters
                || std::fabs(pos[2]) > kSiPositionSanityMeters))
        {
            std::fprintf(stderr, "mesh_wave1: position exceeds %g m — source likely non-SI (mm/cm). Set [cook] "
                                 "position_scale in .meta.\n",
                         static_cast<double>(kSiPositionSanityMeters));
            si_warned = true;
        }
        const crd::f32 nrm[3] = {has_n ? mesh.normals[vi].x : 0.0F, has_n ? mesh.normals[vi].y : 0.0F,
                                 has_n ? mesh.normals[vi].z : 0.0F};
        const crd::f32 uv[2]  = {has_uv ? mesh.uv0[vi].x : 0.0F, has_uv ? mesh.uv0[vi].y : 0.0F};
        const bool     has_tan = mesh.tangent.size() == mesh.positions.size();
        const crd::f32 tan[4]  = {has_tan ? mesh.tangent[vi].x : 0.0F, has_tan ? mesh.tangent[vi].y : 0.0F,
                                  has_tan ? mesh.tangent[vi].z : 0.0F, has_tan ? mesh.tangent[vi].w : 1.0F};
        std::memcpy(dst + 0, pos, 12U);
        std::memcpy(dst + 12, nrm, 12U);
        std::memcpy(dst + 24, uv, 8U);
        std::memcpy(dst + 32, tan, 16U);
    }

    crd::containers::Array<crd::u8> indx_buf(alloc);
    indx_buf.resize(static_cast<crd::usize>(ic) * 4U);
    std::memcpy(indx_buf.data(), mesh.indices.data(), indx_buf.size());

    // PRIM: u32 count + one 32-byte entry (vertex_count, index_count, vbo=0, ibo=0, material null — GEO-3/4 wires it)
    crd::containers::Array<crd::u8> prim_buf(alloc);
    prim_buf.resize(4U + 32U);
    const crd::u32 prim_count = 1U;
    std::memcpy(prim_buf.data(), &prim_count, 4U);
    crd::u8* entry = prim_buf.data() + 4U;
    std::memset(entry, 0, 32U);
    std::memcpy(entry + 0, &vc, 4U);
    std::memcpy(entry + 4, &ic, 4U);

    crd::resources::CrdrWriter writer(alloc, id, crd::resources::kFourCC_MESH);
    writer.add_chunk(crd::resources::kFourCC_VERT, crd::containers::as_const_span(vert_buf));
    writer.add_chunk(crd::resources::kFourCC_INDX, crd::containers::as_const_span(indx_buf));
    writer.add_chunk(crd::resources::kFourCC_PRIM, crd::containers::as_const_span(prim_buf));
    return writer.finish();
}

// ── GEO-3 stage 2b: the glTF TEXTURE decompose (images → TXTR extra artifacts) ─────────────────────────────────────────
//
// Every ImportedImage cooks through the SHARED texture core (texture_cook.hpp) into its OWN TXTR artifact with a
// stable sidecar-.meta id (the mesh extras pattern). The color space is DERIVED FROM SLOT USAGE across all materials —
// baseColor/emissive ⇒ sRGB color, metallicRoughness/occlusion ⇒ linear data, normal ⇒ linear + renormalized mips —
// never guessed from pixels (the classic silent bug made structural). Conflicting usage warns, color wins (a color
// misread as linear washes out; a normal misread as color is caught by the louder artifact). A missing/undecodable
// image warns and skips — the mesh cook survives dirty references (GEO-6's dependency graph later makes them tracked).

struct ImageUsage
{
    bool color  = false; // baseColor / emissive
    bool linear = false; // metallicRoughness / occlusion
    bool normal = false;
};

void classify_image_usage(const crd::assetio::ImportedAsset& asset, crd::containers::Array<ImageUsage>& usage)
{
    const auto mark = [&](crd::i32 idx, bool c, bool l, bool n) {
        if (idx < 0 || static_cast<crd::usize>(idx) >= usage.size()) { return; }
        ImageUsage& u = usage[static_cast<crd::usize>(idx)];
        u.color  = u.color || c;
        u.linear = u.linear || l;
        u.normal = u.normal || n;
    };
    for (crd::usize m = 0; m < asset.materials.size(); ++m)
    {
        const crd::assetio::ImportedMaterial& mat = asset.materials[m];
        mark(mat.base_color_image, true, false, false);
        mark(mat.emissive_image, true, false, false);
        mark(mat.mr_image, false, true, false);
        mark(mat.occlusion_image, false, true, false);
        mark(mat.normal_image, false, false, true);
    }
}

void cook_gltf_images(const CookContext& ctx, const crd::assetio::ImportedAsset& asset, CookResult& result,
                      crd::containers::Array<crd::resources::ResourceId>& image_ids)
{
    image_ids.resize(asset.images.size()); // null = not cooked (skipped/undecodable)
    if (asset.images.size() == 0U) { return; }

    crd::containers::Array<ImageUsage> usage(ctx.allocator);
    usage.resize(asset.images.size());
    classify_image_usage(asset, usage);

    for (crd::usize ii = 0; ii < asset.images.size(); ++ii)
    {
        const crd::assetio::ImportedImage& img = asset.images[ii];
        const ImageUsage&                  use = usage[ii];

        // slot-derived options; conflicts warn and COLOR wins
        TextureCookOptions options;
        options.srgb       = use.color || (!use.linear && !use.normal); // unreferenced images default to color
        options.normal_map = use.normal && !use.color && !use.linear;
        if ((use.color && (use.linear || use.normal)) || (use.linear && use.normal))
        {
            std::fprintf(stderr, "mesh_wave1: image %zu ('%s') referenced with CONFLICTING color spaces — cooking %s\n",
                         ii, img.name.c_str(), options.srgb ? "sRGB" : "linear");
        }

        // bytes: embedded, or an external uri read through CookIO (sibling-resolved, escape-refused, RECORDED —
        // touching the referenced image recooks this source)
        crd::containers::ConstSpan<crd::u8> encoded;
        crd::containers::Array<crd::u8>     file_bytes(ctx.allocator);
        if (img.bytes.size() > 0U) { encoded = crd::containers::as_const_span(img.bytes); }
        else
        {
            if (!ctx.io->read_input(crd::containers::StringView(img.uri.data(), img.uri.size()), file_bytes))
            {
                std::fprintf(stderr, "mesh_wave1: image %zu uri '%s' unresolvable — SKIPPED (texture missing)\n", ii,
                             img.uri.c_str());
                continue;
            }
            encoded = crd::containers::as_const_span(file_bytes);
        }

        crd::resources::LdrImage decoded(ctx.allocator);
        const crd::resources::LdrError derr = crd::resources::ldr_decode(encoded, decoded, ctx.allocator);
        if (derr != crd::resources::LdrError::Ok)
        {
            std::fprintf(stderr, "mesh_wave1: image %zu ('%s') decode failed (LdrError %u) — SKIPPED\n", ii,
                         img.name.c_str(), static_cast<unsigned>(derr));
            continue;
        }

        // stable id: sidecar "<source>.tex.<idx>_<name>.meta" (index-keyed — names may be empty or collide),
        // minted-or-replayed by CookIO::stable_id (recorded as a dependency edge)
        char idx_buf[16];
        std::snprintf(idx_buf, sizeof(idx_buf), "%zu", ii);
        const crd::containers::String safe_name = sanitize_name(img.name.c_str(), ctx.allocator, "img");
        crd::containers::String       suffix(ctx.allocator);
        suffix.append(".tex.");
        suffix.append(idx_buf);
        suffix.append("_");
        suffix.append(safe_name.c_str());

        crd::resources::ResourceId tex_id;
        if (!ctx.io->stable_id(crd::containers::StringView(suffix.data(), suffix.size()), tex_id))
        {
            std::fprintf(stderr, "mesh_wave1: cannot persist sidecar id '%s' — image SKIPPED\n", suffix.c_str());
            continue;
        }

        auto cooked = cook_texture_rgba(decoded, options, tex_id, ctx.allocator);
        if (cooked.empty())
        {
            std::fprintf(stderr, "mesh_wave1: image %zu ('%s') texture cook failed — SKIPPED\n", ii, img.name.c_str());
            continue;
        }
        ExtraArtifact extra(ctx.allocator);
        extra.id           = tex_id;
        extra.type_fourcc  = crd::resources::kFourCC_TXTR;
        extra.cooked_bytes = static_cast<crd::containers::Array<crd::u8>&&>(cooked);
        extra.name.append(ctx.source_path.data(), ctx.source_path.size());
        extra.name.push_back('#');
        extra.name.append("tex_");
        extra.name.append(safe_name.c_str());
        result.extra_artifacts.push_back(static_cast<ExtraArtifact&&>(extra));
        image_ids[ii] = tex_id;
    }
}

// ── GEO-3 stage 4: material AUTHORING (ImportedMaterial → the 'PBRM' OpenPBR resource) ─────────────────────────────────
//
// Every imported material AUTHORS into a native OpenPbrMaterial artifact (crd-resources, ADR-0105's new world —
// foreign material representations die at the import seam): the OpenPBR parameter surface verbatim + texture SLOTS
// resolved to the cooked TXTR ResourceIds. Sidecar `.mtl.<idx>_<name>.meta` ids stay stable across recooks.

void cook_gltf_materials(const CookContext& ctx, const crd::assetio::ImportedAsset& asset,
                         const crd::containers::Array<crd::resources::ResourceId>& image_ids, CookResult& result,
                         crd::containers::Array<crd::resources::ResourceId>& material_ids)
{
    material_ids.resize(asset.materials.size());
    const auto slot_id = [&](crd::i32 image_index) -> crd::resources::ResourceId {
        if (image_index < 0 || static_cast<crd::usize>(image_index) >= image_ids.size()) { return {}; }
        return image_ids[static_cast<crd::usize>(image_index)]; // null when the image failed to cook — honest unbound
    };

    for (crd::usize mi = 0; mi < asset.materials.size(); ++mi)
    {
        const crd::assetio::ImportedMaterial& mat = asset.materials[mi];

        crd::resources::PbrmParams params;
        params.base_color[0]      = mat.base_color.x;
        params.base_color[1]      = mat.base_color.y;
        params.base_color[2]      = mat.base_color.z;
        params.base_alpha         = mat.base_alpha;
        params.metallic           = mat.metallic;
        params.roughness          = mat.roughness;
        params.emissive[0]        = mat.emissive.x;
        params.emissive[1]        = mat.emissive.y;
        params.emissive[2]        = mat.emissive.z;
        params.emissive_strength  = mat.emissive_strength;
        params.ior                = mat.ior;
        params.transmission       = mat.transmission;
        params.normal_scale       = mat.normal_scale;
        params.occlusion_strength = mat.occlusion_strength;

        crd::resources::PbrmTextures textures;
        textures.base_color         = slot_id(mat.base_color_image);
        textures.metallic_roughness = slot_id(mat.mr_image);
        textures.normal             = slot_id(mat.normal_image);
        textures.occlusion          = slot_id(mat.occlusion_image);
        textures.emissive           = slot_id(mat.emissive_image);

        char idx_buf[16];
        std::snprintf(idx_buf, sizeof(idx_buf), "%zu", mi);
        const crd::containers::String safe_name = sanitize_name(mat.name.c_str(), ctx.allocator, "mtl");
        crd::containers::String       suffix(ctx.allocator);
        suffix.append(".mtl.");
        suffix.append(idx_buf);
        suffix.append("_");
        suffix.append(safe_name.c_str());

        crd::resources::ResourceId mat_id;
        if (!ctx.io->stable_id(crd::containers::StringView(suffix.data(), suffix.size()), mat_id))
        {
            std::fprintf(stderr, "mesh_wave1: cannot persist sidecar id '%s' — material SKIPPED\n", suffix.c_str());
            continue;
        }

        auto cooked = crd::resources::pbrm_build(params, textures, mat_id, ctx.allocator);
        if (cooked.empty())
        {
            std::fprintf(stderr, "mesh_wave1: material %zu ('%s') PBRM build failed — SKIPPED\n", mi, mat.name.c_str());
            continue;
        }
        ExtraArtifact extra(ctx.allocator);
        extra.id           = mat_id;
        extra.type_fourcc  = crd::resources::kFourCC_PBRM;
        extra.cooked_bytes = static_cast<crd::containers::Array<crd::u8>&&>(cooked);
        extra.name.append(ctx.source_path.data(), ctx.source_path.size());
        extra.name.push_back('#');
        extra.name.append("mtl_");
        extra.name.append(safe_name.c_str());
        result.extra_artifacts.push_back(static_cast<ExtraArtifact&&>(extra));
        material_ids[mi] = mat_id;
    }
}

// ── GEO-3 stage 3: the glTF SCENE decompose (nodes → a World → the SCEN artifact) ──────────────────────────────────────
//
// The decompose philosophy made real for scenes: the glTF node graph BUILDS a real ECS World (Transform + ChildOf +
// the render components, ADR-0050/0055 machinery — never a hand-written chunk format) and `SceneArtifactBuilder`
// emits the deterministic SCEN artifact. Entities REFERENCE the cooked per-type resources by ResourceId (meshes via
// the stage-1/2 artifacts; materials stay null until stage 4 authors them). Node translations get the SAME `.meta
// position_scale` as vertices (a mm-authored scene scales its offsets too — SI everywhere, ADR-0078). A node whose
// glTF mesh fanned out into N primitives spawns N child entities (one MeshRenderer each — the render-submission
// granularity); a single-primitive mesh rides the node entity itself. The cooker runs TransformPropagation once so
// the SCEN bytes carry composed world matrices (the v1l baked-path convention).

// a fresh sidecar-stable id (the mesh/texture pattern) for the scene artifact
[[nodiscard]] bool scene_sidecar_id(const CookContext& ctx, crd::resources::ResourceId& out_id)
{
    return ctx.io->stable_id(".scen", out_id);
}

void cook_gltf_scene(const CookContext& ctx, const crd::assetio::ImportedAsset& asset,
                     const crd::containers::Array<crd::resources::ResourceId>& mesh_ids,
                     const crd::containers::Array<crd::resources::ResourceId>& material_ids, crd::f32 position_scale,
                     CookResult& result)
{
    if (asset.nodes.size() == 0U) { return; }

    crd::scene::World world{ctx.allocator};
    world.register_component<crd::scene::Transform>(crd::scene::transform_serialize_trait());
    world.register_component<crd::scene::TransformDirtyFlag>(crd::scene::StorageHint::SparseSet);
    world.register_builtin_relations();
    world.register_system(std::make_unique<crd::scene::TransformPropagation>());
    crd::scene::register_render_components(world);

    // pass 1: one entity per node, Transform from the authored TRS (translation × position_scale → SI metres)
    crd::containers::Array<crd::scene::EntityId> node_entities(ctx.allocator);
    node_entities.reserve(asset.nodes.size());
    for (crd::usize ni = 0; ni < asset.nodes.size(); ++ni)
    {
        const crd::assetio::ImportedNode& node = asset.nodes[ni];
        const crd::scene::EntityId        e    = world.spawn();
        crd::scene::Transform             t;
        t.translation = crd::math::from_raw_vec<crd::units::dim::Length>(
            crd::math::Vec3f{node.translation.x * position_scale, node.translation.y * position_scale,
                             node.translation.z * position_scale});
        t.rotation = crd::math::Quatf(node.rotation.x, node.rotation.y, node.rotation.z, node.rotation.w);
        t.scale    = crd::math::Vec3f{node.scale.x, node.scale.y, node.scale.z};
        world.add_component(e, t);

        if (node.camera >= 0)
        {
            const crd::assetio::ImportedCamera& cam = asset.cameras[static_cast<crd::usize>(node.camera)];
            crd::scene::SceneCamera             sc;
            sc.is_ortho   = cam.is_ortho ? 1 : 0;
            sc.yfov_rad   = cam.yfov;
            sc.aspect     = cam.aspect;
            sc.znear      = cam.znear;
            sc.zfar       = cam.zfar;
            sc.ortho_xmag = cam.xmag;
            sc.ortho_ymag = cam.ymag;
            world.add_component(e, sc);
        }
        if (node.light >= 0)
        {
            const crd::assetio::ImportedLight& light = asset.lights[static_cast<crd::usize>(node.light)];
            crd::scene::SceneLight             sl;
            sl.type           = light.type;
            sl.color          = crd::math::Vec3f{light.color.x, light.color.y, light.color.z};
            sl.intensity      = light.intensity;
            sl.range          = light.range;
            sl.inner_cone_rad = light.inner_cone;
            sl.outer_cone_rad = light.outer_cone;
            world.add_component(e, sl);
        }
        node_entities.push_back(e);
    }

    // pass 2: hierarchy (ChildOf: src = child, target = parent) + drawables (per-primitive fan-out)
    for (crd::usize ni = 0; ni < asset.nodes.size(); ++ni)
    {
        const crd::assetio::ImportedNode& node = asset.nodes[ni];
        for (crd::usize ci = 0; ci < node.children.size(); ++ci)
        {
            world.add_relation<crd::scene::relations::ChildOf>(node_entities[node.children[ci]], node_entities[ni]);
        }
        if (node.mesh < 0) { continue; }

        // the primitives this node's LIBRARY mesh fanned out into (cooked ones only), each with ITS OWN authored
        // material (the primitive is the material-binding granularity — stage 4)
        crd::containers::Array<crd::scene::MeshRenderer> drawables(ctx.allocator);
        for (crd::usize mi = 0; mi < asset.meshes.size(); ++mi)
        {
            if (asset.meshes[mi].source_mesh != node.mesh || mesh_ids[mi].is_null()) { continue; }
            crd::resources::ResourceId mat_id{};
            const crd::i32             mat_index = asset.meshes[mi].material;
            if (mat_index >= 0 && static_cast<crd::usize>(mat_index) < material_ids.size())
            {
                mat_id = material_ids[static_cast<crd::usize>(mat_index)];
            }
            drawables.push_back(crd::scene::MeshRenderer{mesh_ids[mi], mat_id});
        }
        if (drawables.size() == 0U)
        {
            std::fprintf(stderr, "mesh_wave1: node %zu references mesh %d with no cooked primitives — no drawable\n",
                         ni, node.mesh);
            continue;
        }
        if (drawables.size() == 1U)
        {
            world.add_component(node_entities[ni], drawables[0]);
            continue;
        }
        for (crd::usize pi = 0; pi < drawables.size(); ++pi) // multi-primitive → one child entity per drawable
        {
            const crd::scene::EntityId child = world.spawn();
            world.add_component(child, crd::scene::Transform{});
            world.add_component(child, drawables[pi]);
            world.add_relation<crd::scene::relations::ChildOf>(child, node_entities[ni]);
        }
    }

    // baked world matrices (the v1l convention): mark every Transform dirty, propagate once
    for (crd::usize ni = 0; ni < node_entities.size(); ++ni)
    {
        world.mark_transform_subtree_dirty(node_entities[ni]);
    }
    world.step(1.0 / 60.0);

    crd::resources::ResourceId scen_id;
    if (!scene_sidecar_id(ctx, scen_id))
    {
        std::fprintf(stderr, "mesh_wave1: cannot write scene sidecar .meta — scene artifact SKIPPED\n");
        return;
    }
    crd::scene::SceneArtifactBuilder builder{ctx.allocator, scen_id};
    auto                             scen_bytes = builder.build(world);
    if (scen_bytes.empty())
    {
        std::fprintf(stderr, "mesh_wave1: SCEN build produced no bytes — scene artifact SKIPPED\n");
        return;
    }
    ExtraArtifact extra(ctx.allocator);
    extra.id           = scen_id;
    extra.type_fourcc  = crd::scene::kFourCC_SCEN;
    extra.cooked_bytes = static_cast<crd::containers::Array<crd::u8>&&>(scen_bytes);
    extra.name.append(ctx.source_path.data(), ctx.source_path.size());
    extra.name.append("#scene");
    result.extra_artifacts.push_back(static_cast<ExtraArtifact&&>(extra));
}

// ── the handler ─────────────────────────────────────────────────────────────────────────────────────────────────────────

CookResult wave1_handler(const CookContext& ctx)
{
    CookResult result(ctx.allocator);

    const Wave1Format fmt = detect_format(ctx.source_path);
    if (fmt == Wave1Format::Unknown) { return result; }

    crd::containers::Array<crd::u8> bytes(ctx.allocator);
    if (!ctx.io->read_source(bytes))
    {
        std::fprintf(stderr, "mesh_wave1: cannot read %.*s\n", static_cast<int>(ctx.source_path.size()),
                     ctx.source_path.data());
        return result;
    }

    // OUR parser (crd-asset-io) fills the ImportedAsset seam. (OBJ: materials resolve at GEO-3/4 — geometry cooks now;
    // an absent .mtl downgrades usemtl to material -1 inside the parser, never kills the import.)
    crd::assetio::ImportedAsset asset(ctx.allocator);
    crd::assetio::ImportStatus  st = crd::assetio::ImportStatus::NotRecognized;
    const auto                  span = crd::containers::as_const_span(bytes);
    if (fmt == Wave1Format::Stl) { st = crd::assetio::parse_stl(span, ctx.allocator, asset); }
    else if (fmt == Wave1Format::Obj) { st = crd::assetio::parse_obj(span, ctx.allocator, asset); }
    else if (fmt == Wave1Format::Ply) { st = crd::assetio::parse_ply(span, ctx.allocator, asset); }
    else if (fmt == Wave1Format::Glb) { st = crd::assetio::parse_glb(span, ctx.allocator, asset); }
    else if (fmt == Wave1Format::ThreeMf) // the package is unzipped HERE; the parser sees only the model part
    {
        crd::containers::Array<crd::u8> model_bytes(ctx.allocator);
        if (extract_3mf_model_part(span, ctx.allocator, model_bytes))
        {
            st = crd::assetio::parse_3mf_model(crd::containers::as_const_span(model_bytes), ctx.allocator, asset);
        }
    }
    else // .gltf: data-URI buffers resolve internally; an external .bin resolves by its REPORTED uri (the GEO-6
    {    // uri-general case), read through CookIO — the .bin is a RECORDED dependency edge
        st = crd::assetio::parse_gltf(span, crd::containers::ConstSpan<crd::u8>{}, ctx.allocator, asset);
        if (st == crd::assetio::ImportStatus::Malformed)
        {
            // the parser reports the buffer's percent-decoded uri; fall back to the sibling "<stem>.bin" for
            // uri-less legacy exports
            crd::containers::String bin_rel(asset.buffer_uri.data(), asset.buffer_uri.size(), ctx.allocator);
            if (bin_rel.size() == 0U)
            {
                const crd::containers::StringView sp    = ctx.source_path;
                const auto                        slash = sp.rfind('/');
                const crd::containers::StringView fname =
                    (slash != crd::containers::StringView::npos) ? sp.substr(slash + 1U) : sp;
                if (fname.size() > 5U)
                {
                    bin_rel.append(fname.data(), fname.size() - 5U);
                    bin_rel.append(".bin");
                }
            }
            crd::containers::Array<crd::u8> bin_bytes(ctx.allocator);
            if (bin_rel.size() > 0U
                && ctx.io->read_input(crd::containers::StringView(bin_rel.data(), bin_rel.size()), bin_bytes))
            {
                asset = crd::assetio::ImportedAsset(ctx.allocator); // reset any partial state
                st    = crd::assetio::parse_gltf(span, crd::containers::as_const_span(bin_bytes), ctx.allocator,
                                                 asset);
            }
        }
    }
    if (st != crd::assetio::ImportStatus::Ok)
    {
        std::fprintf(stderr, "mesh_wave1: parse failed (%s) for %.*s\n", crd::assetio::import_status_name(st),
                     static_cast<int>(ctx.source_path.size()), ctx.source_path.data());
        return result;
    }
    if (asset.warning_count > 0U)
    {
        std::fprintf(stderr, "mesh_wave1: %u import warning(s) for %.*s\n", asset.warning_count,
                     static_cast<int>(ctx.source_path.size()), ctx.source_path.data());
    }

    // .meta [cook] options (position_scale — the SI-unit boundary, ADR-0078); a recorded input edge
    MeshCookOptions cook_options{};
    {
        crd::containers::String meta_text(ctx.allocator);
        if (ctx.io->read_meta(meta_text))
        {
            cook_options = parse_mesh_cook_options(crd::containers::StringView(meta_text.data(), meta_text.size()));
        }
    }

    // first mesh WITH triangles → the main artifact (ctx.id); the rest → sidecar-.meta extra artifacts (stable ids)
    crd::i32 main_index = -1;
    for (crd::usize mi = 0; mi < asset.meshes.size(); ++mi)
    {
        if (asset.meshes[mi].triangle_count() > 0U)
        {
            main_index = static_cast<crd::i32>(mi);
            break;
        }
    }
    if (main_index < 0)
    {
        std::fprintf(stderr, "mesh_wave1: no triangle geometry in %.*s (point cloud / lines only?)\n",
                     static_cast<int>(ctx.source_path.size()), ctx.source_path.data());
        return result;
    }

    // stage 3: the per-mesh artifact ids, collected as we cook (null = not cooked) — the scene's MeshRenderers
    // reference these
    crd::containers::Array<crd::resources::ResourceId> mesh_ids(ctx.allocator);
    mesh_ids.resize(asset.meshes.size());

    {
        auto artifact = build_wave1_artifact(asset.meshes[static_cast<crd::usize>(main_index)], ctx.id, ctx.allocator,
                                             cook_options);
        if (artifact.empty()) { return result; }
        result.cooked_bytes    = std::move(artifact);
        result.type_fourcc     = crd::resources::kFourCC_MESH;
        result.handler_version = kWave1HandlerVersion;
        result.ok              = true;
        mesh_ids[static_cast<crd::usize>(main_index)] = ctx.id;
    }

    for (crd::usize mi = static_cast<crd::usize>(main_index) + 1U; mi < asset.meshes.size(); ++mi)
    {
        crd::assetio::ImportedMesh& mesh = asset.meshes[mi];
        if (mesh.triangle_count() == 0U) { continue; }

        const crd::containers::String safe_name = sanitize_name(mesh.name.c_str(), ctx.allocator);
        crd::containers::String       suffix(ctx.allocator);
        suffix.append(".mesh.");
        suffix.append(safe_name.c_str());

        crd::resources::ResourceId extra_id;
        if (!ctx.io->stable_id(crd::containers::StringView(suffix.data(), suffix.size()), extra_id))
        {
            std::fprintf(stderr, "mesh_wave1: cannot persist sidecar id '%s'\n", suffix.c_str());
            result.ok = false;
            return result;
        }

        auto artifact = build_wave1_artifact(mesh, extra_id, ctx.allocator, cook_options);
        if (artifact.empty())
        {
            result.ok = false;
            return result;
        }
        ExtraArtifact extra(ctx.allocator);
        extra.id           = extra_id;
        extra.type_fourcc  = crd::resources::kFourCC_MESH;
        extra.cooked_bytes = std::move(artifact);
        extra.name.append(ctx.source_path.data(), ctx.source_path.size());
        extra.name.push_back('#');
        extra.name.append(safe_name.c_str());
        result.extra_artifacts.push_back(std::move(extra));
        mesh_ids[mi] = extra_id;
    }

    // GEO-3 stages 2b + 3 + 4: the decompose — images → TXTR artifacts, materials → authored PBRM artifacts (slots
    // wired to the cooked textures), the node graph → a SCEN artifact (drawables wired to meshes + materials).
    // 3MF rides the SAME seam: basematerials → PBRM, build items → SCEN (core 3MF has no images — the stage no-ops).
    if (fmt == Wave1Format::Glb || fmt == Wave1Format::Gltf || fmt == Wave1Format::ThreeMf)
    {
        crd::containers::Array<crd::resources::ResourceId> image_ids(ctx.allocator);
        crd::containers::Array<crd::resources::ResourceId> material_ids(ctx.allocator);
        cook_gltf_images(ctx, asset, result, image_ids);
        cook_gltf_materials(ctx, asset, image_ids, result, material_ids);
        cook_gltf_scene(ctx, asset, mesh_ids, material_ids, cook_options.position_scale, result);
    }

    return result;
}

} // anonymous namespace

void register_wave1_mesh_handler()
{
    register_cook_handler(".stl", wave1_handler, kWave1HandlerVersion);
    register_cook_handler(".obj", wave1_handler, kWave1HandlerVersion);
    register_cook_handler(".ply", wave1_handler, kWave1HandlerVersion);
    // GEO-3: OUR glTF parser owns .glb/.gltf (the legacy cgltf handler is DELETED — GEO-6 honored the disposition)
    register_cook_handler(".glb", wave1_handler, kWave1HandlerVersion);
    register_cook_handler(".gltf", wave1_handler, kWave1HandlerVersion);
    // GEO-5 pt 3: 3MF — the manufacturing pillar (OUR ZIP + OUR XML + OUR model parser; zero 3rd-party)
    register_cook_handler(".3mf", wave1_handler, kWave1HandlerVersion);
}

} // namespace crd::cooker
