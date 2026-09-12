// gltf_export.cpp — GEO-4 (D-007): the glTF 2.0 (.glb) writer. See gltf_export.hpp for the contract.

#include <crd/assetio/gltf_export.hpp>

#include <crd/assetio/json_write.hpp>
#include <crd/containers/string.hpp>
#include <cmath> // std::isfinite — the classification fn (the gltf.cpp idiom; the transcendental ban does not cover it)

#include <cstring>

namespace crd::assetio
{
namespace
{

constexpr crd::u32 kStride = 48U; // the ADR-0078 cooked vertex: pos f32x3 · nrm f32x3 · uv f32x2 · tan f32x4

[[nodiscard]] float read_f32(const crd::u8* p) noexcept
{
    float f = 0.0F;
    std::memcpy(&f, p, 4U);
    return f;
}

void put_u32(crd::containers::Array<crd::u8>& out, crd::u32 v)
{
    out.push_back(static_cast<crd::u8>(v & 0xFFU));
    out.push_back(static_cast<crd::u8>((v >> 8U) & 0xFFU));
    out.push_back(static_cast<crd::u8>((v >> 16U) & 0xFFU));
    out.push_back(static_cast<crd::u8>((v >> 24U) & 0xFFU));
}

void put_bytes(crd::containers::Array<crd::u8>& out, const void* p, crd::usize n)
{
    const auto* b = static_cast<const crd::u8*>(p);
    for (crd::usize i = 0; i < n; ++i) { out.push_back(b[i]); }
}

void pad_to_4(crd::containers::Array<crd::u8>& out, crd::u8 fill)
{
    while ((out.size() & 3U) != 0U) { out.push_back(fill); }
}

// One de-interleaved attribute stream appended to the BIN blob → (byteOffset, byteLength).
struct ViewRange
{
    crd::u32 offset = 0;
    crd::u32 length = 0;
};

struct MeshViews
{
    ViewRange pos;
    ViewRange nrm;
    ViewRange uv;
    ViewRange tan;
    ViewRange idx;
    crd::u32  vertex_count = 0;
    crd::u32  index_count  = 0;
    float     pos_min[3]   = {0.0F, 0.0F, 0.0F};
    float     pos_max[3]   = {0.0F, 0.0F, 0.0F};
};

[[nodiscard]] bool all_finite(const float* v, crd::u32 n) noexcept
{
    for (crd::u32 i = 0; i < n; ++i)
    {
        if (!std::isfinite(static_cast<crd::f64>(v[i]))) { return false; }
    }
    return true;
}

} // namespace

bool gltf_export_glb(const ExportAsset& asset, crd::containers::Array<crd::u8>& out, crd::memory::IAllocator* alloc)
{
    out.clear();

    // ── validate the structural contract up front (refusal over a corrupt file) ──────────────────────────────────
    const auto image_ok = [&](int idx) { return idx < static_cast<int>(asset.images.size()); };
    for (crd::usize ii = 0; ii < asset.images.size(); ++ii)
    {
        if (asset.images[ii].png.size() < 8U) { return false; } // not even a PNG signature
    }
    for (crd::usize m = 0; m < asset.meshes.size(); ++m)
    {
        const ExportMesh& em = asset.meshes[m];
        if (em.vertices.size() == 0U || (em.vertices.size() % kStride) != 0U) { return false; }
        if (em.indices.size() == 0U || (em.indices.size() % 4U) != 0U) { return false; }
        if (em.material >= static_cast<int>(asset.materials.size())) { return false; }
        const crd::u32 vcount = static_cast<crd::u32>(em.vertices.size() / kStride);
        const crd::u32 icount = static_cast<crd::u32>(em.indices.size() / 4U);
        for (crd::u32 i = 0; i < icount; ++i)
        {
            crd::u32 iv = 0;
            std::memcpy(&iv, em.indices.data() + static_cast<crd::usize>(i) * 4U, 4U);
            if (iv >= vcount) { return false; }
        }
    }
    for (crd::usize n = 0; n < asset.nodes.size(); ++n)
    {
        const ExportNode& en = asset.nodes[n];
        if (en.mesh >= static_cast<int>(asset.meshes.size())) { return false; }
        if (en.parent >= static_cast<int>(asset.nodes.size()) || en.parent == static_cast<int>(n)) { return false; }
        if (!all_finite(en.translation, 3U) || !all_finite(en.rotation, 4U) || !all_finite(en.scale, 3U)) { return false; }
    }
    for (crd::usize mi = 0; mi < asset.materials.size(); ++mi)
    {
        const ExportMaterial& em = asset.materials[mi];
        if (!all_finite(em.base_color, 4U) || !all_finite(&em.metallic, 1U) || !all_finite(&em.roughness, 1U)
            || !all_finite(em.emissive, 3U) || !all_finite(&em.emissive_strength, 1U) || !all_finite(&em.ior, 1U)
            || !all_finite(&em.transmission, 1U))
        {
            return false;
        }
        if (!image_ok(em.base_color_image) || !image_ok(em.metallic_roughness_image) || !image_ok(em.normal_image)
            || !image_ok(em.occlusion_image) || !image_ok(em.emissive_image))
        {
            return false;
        }
    }

    // ── the BIN blob: de-interleave each mesh into tightly-packed streams ────────────────────────────────────────
    crd::containers::Array<crd::u8>   bin(alloc);
    crd::containers::Array<MeshViews> views(alloc);
    for (crd::usize m = 0; m < asset.meshes.size(); ++m)
    {
        const ExportMesh& em     = asset.meshes[m];
        MeshViews         mv;
        mv.vertex_count = static_cast<crd::u32>(em.vertices.size() / kStride);
        mv.index_count  = static_cast<crd::u32>(em.indices.size() / 4U);
        const crd::u8* verts = em.vertices.data();

        const auto stream = [&](crd::u32 field_offset, crd::u32 field_bytes) {
            ViewRange r;
            r.offset = static_cast<crd::u32>(bin.size());
            for (crd::u32 v = 0; v < mv.vertex_count; ++v)
            {
                put_bytes(bin, verts + static_cast<crd::usize>(v) * kStride + field_offset, field_bytes);
            }
            r.length = static_cast<crd::u32>(bin.size()) - r.offset;
            return r;
        };
        mv.pos = stream(0U, 12U);
        mv.nrm = stream(12U, 12U);
        mv.uv  = stream(24U, 8U);
        mv.tan = stream(32U, 16U);

        mv.idx.offset = static_cast<crd::u32>(bin.size());
        put_bytes(bin, em.indices.data(), em.indices.size());
        mv.idx.length = static_cast<crd::u32>(bin.size()) - mv.idx.offset;

        for (crd::u32 v = 0; v < mv.vertex_count; ++v) // POSITION min/max (spec-required)
        {
            const crd::u8* rec = verts + static_cast<crd::usize>(v) * kStride;
            for (crd::u32 c = 0; c < 3U; ++c)
            {
                const float f = read_f32(rec + c * 4U);
                if (!std::isfinite(static_cast<crd::f64>(f))) { return false; }
                if (v == 0U || f < mv.pos_min[c]) { mv.pos_min[c] = f; }
                if (v == 0U || f > mv.pos_max[c]) { mv.pos_max[c] = f; }
            }
        }
        views.push_back(mv);
    }

    // image PNGs embed verbatim after the mesh streams (4-aligned; view index = meshes·5 + image index)
    crd::containers::Array<ViewRange> image_views(alloc);
    for (crd::usize ii = 0; ii < asset.images.size(); ++ii)
    {
        pad_to_4(bin, 0U);
        ViewRange r;
        r.offset = static_cast<crd::u32>(bin.size());
        put_bytes(bin, asset.images[ii].png.data(), asset.images[ii].png.size());
        r.length = static_cast<crd::u32>(bin.size()) - r.offset;
        image_views.push_back(r);
    }

    // ── the JSON chunk ───────────────────────────────────────────────────────────────────────────────────────────
    JsonWriter j(alloc);
    j.begin_object();
    j.key("asset");
    j.begin_object();
    j.kv("version", "2.0");
    j.kv("generator", "cerid");
    j.end_object();

    const bool any_ior = [&] {
        for (crd::usize i = 0; i < asset.materials.size(); ++i) { if (asset.materials[i].ior != 1.5F) { return true; } }
        return false;
    }();
    const bool any_transmission = [&] {
        for (crd::usize i = 0; i < asset.materials.size(); ++i) { if (asset.materials[i].transmission > 0.0F) { return true; } }
        return false;
    }();
    const bool any_emissive_strength = [&] {
        for (crd::usize i = 0; i < asset.materials.size(); ++i) { if (asset.materials[i].emissive_strength != 1.0F) { return true; } }
        return false;
    }();
    if (any_ior || any_transmission || any_emissive_strength)
    {
        j.key("extensionsUsed");
        j.begin_array();
        if (any_ior) { j.value_string("KHR_materials_ior"); }
        if (any_transmission) { j.value_string("KHR_materials_transmission"); }
        if (any_emissive_strength) { j.value_string("KHR_materials_emissive_strength"); }
        j.end_array();
    }

    if (bin.size() > 0U)
    {
        j.key("buffers");
        j.begin_array();
        j.begin_object();
        j.kv("byteLength", static_cast<crd::u64>(bin.size()));
        j.end_object();
        j.end_array();
    }

    // bufferViews: 5 per mesh in a fixed order (pos, nrm, uv, tan, idx — view i and accessor i align), then one per
    // embedded image (no target — images are not vertex data).
    if (views.size() > 0U || image_views.size() > 0U)
    {
        j.key("bufferViews");
        j.begin_array();
        for (crd::usize m = 0; m < views.size(); ++m)
        {
            const MeshViews& mv        = views[m];
            const ViewRange  ranges[5] = {mv.pos, mv.nrm, mv.uv, mv.tan, mv.idx};
            for (crd::u32 r = 0; r < 5U; ++r)
            {
                j.begin_object();
                j.kv("buffer", static_cast<crd::u64>(0U));
                j.kv("byteOffset", static_cast<crd::u64>(ranges[r].offset));
                j.kv("byteLength", static_cast<crd::u64>(ranges[r].length));
                j.kv("target", static_cast<crd::u64>(r == 4U ? 34963U : 34962U)); // ELEMENT_ARRAY / ARRAY
                j.end_object();
            }
        }
        for (crd::usize ii = 0; ii < image_views.size(); ++ii)
        {
            j.begin_object();
            j.kv("buffer", static_cast<crd::u64>(0U));
            j.kv("byteOffset", static_cast<crd::u64>(image_views[ii].offset));
            j.kv("byteLength", static_cast<crd::u64>(image_views[ii].length));
            j.end_object();
        }
        j.end_array();
    }
    if (image_views.size() > 0U)
    {
        const crd::u32 image_view_base = static_cast<crd::u32>(views.size()) * 5U;
        j.key("images");
        j.begin_array();
        for (crd::usize ii = 0; ii < asset.images.size(); ++ii)
        {
            j.begin_object();
            if (asset.images[ii].name != nullptr) { j.kv("name", asset.images[ii].name); }
            j.kv("bufferView", image_view_base + static_cast<crd::u32>(ii));
            j.kv("mimeType", "image/png");
            j.end_object();
        }
        j.end_array();
        j.key("textures"); // 1:1 with images — texture index == image index
        j.begin_array();
        for (crd::usize ii = 0; ii < asset.images.size(); ++ii)
        {
            j.begin_object();
            j.kv("source", static_cast<crd::u32>(ii));
            j.end_object();
        }
        j.end_array();
    }
    if (views.size() > 0U)
    {
        j.key("accessors");
        j.begin_array();
        for (crd::usize m = 0; m < views.size(); ++m)
        {
            const MeshViews& mv   = views[m];
            const crd::u32   base = static_cast<crd::u32>(m) * 5U;
            const auto attr = [&](crd::u32 view, const char* type, crd::u32 count, bool with_minmax) {
                j.begin_object();
                j.kv("bufferView", view);
                j.kv("componentType", static_cast<crd::u64>(5126U)); // FLOAT
                j.kv("count", static_cast<crd::u64>(count));
                j.kv("type", type);
                if (with_minmax)
                {
                    j.key("min");
                    j.begin_array();
                    for (crd::u32 c = 0; c < 3U; ++c) { j.value_f64(static_cast<crd::f64>(mv.pos_min[c])); }
                    j.end_array();
                    j.key("max");
                    j.begin_array();
                    for (crd::u32 c = 0; c < 3U; ++c) { j.value_f64(static_cast<crd::f64>(mv.pos_max[c])); }
                    j.end_array();
                }
                j.end_object();
            };
            attr(base + 0U, "VEC3", mv.vertex_count, true);
            attr(base + 1U, "VEC3", mv.vertex_count, false);
            attr(base + 2U, "VEC2", mv.vertex_count, false);
            attr(base + 3U, "VEC4", mv.vertex_count, false);
            j.begin_object(); // indices
            j.kv("bufferView", base + 4U);
            j.kv("componentType", static_cast<crd::u64>(5125U)); // UNSIGNED_INT
            j.kv("count", static_cast<crd::u64>(mv.index_count));
            j.kv("type", "SCALAR");
            j.end_object();
        }
        j.end_array();

        j.key("meshes");
        j.begin_array();
        for (crd::usize m = 0; m < asset.meshes.size(); ++m)
        {
            const crd::u32 base = static_cast<crd::u32>(m) * 5U;
            j.begin_object();
            if (asset.meshes[m].name != nullptr) { j.kv("name", asset.meshes[m].name); }
            j.key("primitives");
            j.begin_array();
            j.begin_object();
            j.key("attributes");
            j.begin_object();
            j.kv("POSITION", base + 0U);
            j.kv("NORMAL", base + 1U);
            j.kv("TEXCOORD_0", base + 2U);
            j.kv("TANGENT", base + 3U);
            j.end_object();
            j.kv("indices", base + 4U);
            if (asset.meshes[m].material >= 0) { j.kv("material", static_cast<crd::u64>(asset.meshes[m].material)); }
            j.end_object();
            j.end_array();
            j.end_object();
        }
        j.end_array();
    }

    if (asset.materials.size() > 0U)
    {
        j.key("materials");
        j.begin_array();
        for (crd::usize mi = 0; mi < asset.materials.size(); ++mi)
        {
            const ExportMaterial& em = asset.materials[mi];
            j.begin_object();
            if (em.name != nullptr) { j.kv("name", em.name); }
            j.key("pbrMetallicRoughness");
            j.begin_object();
            j.key("baseColorFactor");
            j.begin_array();
            for (crd::u32 c = 0; c < 4U; ++c) { j.value_f64(static_cast<crd::f64>(em.base_color[c])); }
            j.end_array();
            j.kv("metallicFactor", static_cast<crd::f64>(em.metallic));
            j.kv("roughnessFactor", static_cast<crd::f64>(em.roughness));
            if (em.base_color_image >= 0)
            {
                j.key("baseColorTexture");
                j.begin_object();
                j.kv("index", static_cast<crd::u32>(em.base_color_image)); // texture index == image index (1:1)
                j.end_object();
            }
            if (em.metallic_roughness_image >= 0)
            {
                j.key("metallicRoughnessTexture");
                j.begin_object();
                j.kv("index", static_cast<crd::u32>(em.metallic_roughness_image));
                j.end_object();
            }
            j.end_object();
            if (em.normal_image >= 0)
            {
                j.key("normalTexture");
                j.begin_object();
                j.kv("index", static_cast<crd::u32>(em.normal_image));
                if (em.normal_scale != 1.0F) { j.kv("scale", static_cast<crd::f64>(em.normal_scale)); }
                j.end_object();
            }
            if (em.occlusion_image >= 0)
            {
                j.key("occlusionTexture");
                j.begin_object();
                j.kv("index", static_cast<crd::u32>(em.occlusion_image));
                if (em.occlusion_strength != 1.0F) { j.kv("strength", static_cast<crd::f64>(em.occlusion_strength)); }
                j.end_object();
            }
            if (em.emissive_image >= 0)
            {
                j.key("emissiveTexture");
                j.begin_object();
                j.kv("index", static_cast<crd::u32>(em.emissive_image));
                j.end_object();
            }
            if (em.emissive[0] != 0.0F || em.emissive[1] != 0.0F || em.emissive[2] != 0.0F)
            {
                j.key("emissiveFactor");
                j.begin_array();
                for (crd::u32 c = 0; c < 3U; ++c) { j.value_f64(static_cast<crd::f64>(em.emissive[c])); }
                j.end_array();
            }
            if (em.ior != 1.5F || em.transmission > 0.0F || em.emissive_strength != 1.0F)
            {
                j.key("extensions");
                j.begin_object();
                if (em.ior != 1.5F)
                {
                    j.key("KHR_materials_ior");
                    j.begin_object();
                    j.kv("ior", static_cast<crd::f64>(em.ior));
                    j.end_object();
                }
                if (em.transmission > 0.0F)
                {
                    j.key("KHR_materials_transmission");
                    j.begin_object();
                    j.kv("transmissionFactor", static_cast<crd::f64>(em.transmission));
                    j.end_object();
                }
                if (em.emissive_strength != 1.0F)
                {
                    j.key("KHR_materials_emissive_strength");
                    j.begin_object();
                    j.kv("emissiveStrength", static_cast<crd::f64>(em.emissive_strength));
                    j.end_object();
                }
                j.end_object();
            }
            j.end_object();
        }
        j.end_array();
    }

    if (asset.nodes.size() > 0U)
    {
        j.key("nodes");
        j.begin_array();
        for (crd::usize n = 0; n < asset.nodes.size(); ++n)
        {
            const ExportNode& en = asset.nodes[n];
            j.begin_object();
            if (en.name != nullptr) { j.kv("name", en.name); }
            if (en.mesh >= 0) { j.kv("mesh", static_cast<crd::u64>(en.mesh)); }
            const bool has_t = en.translation[0] != 0.0F || en.translation[1] != 0.0F || en.translation[2] != 0.0F;
            const bool has_r = en.rotation[0] != 0.0F || en.rotation[1] != 0.0F || en.rotation[2] != 0.0F || en.rotation[3] != 1.0F;
            const bool has_s = en.scale[0] != 1.0F || en.scale[1] != 1.0F || en.scale[2] != 1.0F;
            if (has_t)
            {
                j.key("translation");
                j.begin_array();
                for (crd::u32 c = 0; c < 3U; ++c) { j.value_f64(static_cast<crd::f64>(en.translation[c])); }
                j.end_array();
            }
            if (has_r)
            {
                j.key("rotation");
                j.begin_array();
                for (crd::u32 c = 0; c < 4U; ++c) { j.value_f64(static_cast<crd::f64>(en.rotation[c])); }
                j.end_array();
            }
            if (has_s)
            {
                j.key("scale");
                j.begin_array();
                for (crd::u32 c = 0; c < 3U; ++c) { j.value_f64(static_cast<crd::f64>(en.scale[c])); }
                j.end_array();
            }
            // children derived from the parent indices
            bool any_child = false;
            for (crd::usize c = 0; c < asset.nodes.size(); ++c)
            {
                if (asset.nodes[c].parent == static_cast<int>(n)) { any_child = true; break; }
            }
            if (any_child)
            {
                j.key("children");
                j.begin_array();
                for (crd::usize c = 0; c < asset.nodes.size(); ++c)
                {
                    if (asset.nodes[c].parent == static_cast<int>(n)) { j.value_u64(static_cast<crd::u64>(c)); }
                }
                j.end_array();
            }
            j.end_object();
        }
        j.end_array();

        j.key("scenes");
        j.begin_array();
        j.begin_object();
        j.key("nodes");
        j.begin_array();
        for (crd::usize n = 0; n < asset.nodes.size(); ++n)
        {
            if (asset.nodes[n].parent < 0) { j.value_u64(static_cast<crd::u64>(n)); }
        }
        j.end_array();
        j.end_object();
        j.end_array();
        j.kv("scene", static_cast<crd::u64>(0U));
    }

    j.end_object();

    // ── GLB assembly: header + JSON chunk (space-padded) + BIN chunk (zero-padded) ───────────────────────────────
    crd::containers::Array<crd::u8> json_chunk(alloc);
    put_bytes(json_chunk, j.str().c_str(), j.str().size());
    pad_to_4(json_chunk, static_cast<crd::u8>(' '));
    crd::containers::Array<crd::u8> bin_chunk(alloc);
    put_bytes(bin_chunk, bin.data(), bin.size());
    pad_to_4(bin_chunk, 0U);

    const bool     has_bin = bin_chunk.size() > 0U;
    const crd::u32 total   = 12U + 8U + static_cast<crd::u32>(json_chunk.size())
                         + (has_bin ? 8U + static_cast<crd::u32>(bin_chunk.size()) : 0U);
    put_u32(out, 0x46546C67U); // "glTF"
    put_u32(out, 2U);
    put_u32(out, total);
    put_u32(out, static_cast<crd::u32>(json_chunk.size()));
    put_u32(out, 0x4E4F534AU); // "JSON"
    put_bytes(out, json_chunk.data(), json_chunk.size());
    if (has_bin)
    {
        put_u32(out, static_cast<crd::u32>(bin_chunk.size()));
        put_u32(out, 0x004E4942U); // "BIN\0"
        put_bytes(out, bin_chunk.data(), bin_chunk.size());
    }
    return true;
}

} // namespace crd::assetio
