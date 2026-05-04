// NOLINTBEGIN(misc-include-cleaner)
#define CGLTF_IMPLEMENTATION
#include <cgltf.h>
// NOLINTEND(misc-include-cleaner)

// MikkTSpace: include declarations AND implementation inline as C++ (no separate .c target).
// genTangSpaceDefault() gets C linkage from the extern "C" block.
extern "C"
{
// NOLINTBEGIN(misc-include-cleaner)
#include <mikktspace.h>
#include "mikktspace.c" // NOLINT — implementation compiled as C++ within this TU
// NOLINTEND(misc-include-cleaner)
}

#include <crd/cooker/cook_handler.hpp>

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/containers/string.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/memory/allocators/malloc_allocator.hpp>
#include <crd/platform/filesystem.hpp>
#include <crd/resources/crdr.hpp>
#include <crd/resources/resource_id.hpp>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace fs = crd::platform::fs;

namespace crd::cooker
{

namespace
{

constexpr crd::u32 kMeshHandlerVersion = 1U;
constexpr crd::u32 kMeshVertexStride   = 48U; // float3 pos + float3 norm + float2 uv + float4 tan

// ── Meta file helpers ──────────────────────────────────────────────────────
// Duplicated from cook_command.cpp (which keeps them file-local).

// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables)
crd::memory::MallocAllocator g_mesh_alloc;
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables)

bool mesh_meta_read(const fs::Path& meta_path, crd::resources::ResourceId& out_id)
{
    crd::containers::String text(&g_mesh_alloc);
    if (!fs::read_file_text(meta_path, text))
    {
        return false;
    }
    const std::string_view sv(text.data(), text.size());
    const std::string_view key = "uuid = \"";
    auto pos = sv.find(key);
    if (pos == std::string_view::npos)
    {
        return false;
    }
    pos += key.size();
    const auto end = sv.find('"', pos);
    if (end == std::string_view::npos)
    {
        return false;
    }
    out_id = crd::resources::ResourceId::parse(sv.substr(pos, end - pos));
    return !out_id.is_null();
}

bool mesh_meta_write(const fs::Path& meta_path, const crd::resources::ResourceId& id)
{
    const auto id_str = id.to_string(&g_mesh_alloc);
    crd::containers::String content(&g_mesh_alloc);
    content.append("[id]\n");
    content.append("uuid = \"");
    content.append(id_str.c_str());
    content.append("\"\n");
    return fs::write_file_text(
        meta_path, crd::containers::StringView(content.data(), content.size()));
}

// ── MikkTSpace integration ─────────────────────────────────────────────────

struct MikkPrimData
{
    const float*      positions;   // vertex_count * 3 floats
    const float*      normals;     // vertex_count * 3 floats
    const float*      uvs;         // vertex_count * 2 floats
    float*            tangents;    // vertex_count * 4 floats (output)
    const crd::u32*   indices;     // face_count * 3 u32
    crd::u32          face_count;
};

// Callbacks are static functions assigned to the C struct.
// Calling convention is compatible on all supported platforms (x86-64 MSVC/clang-cl).

static int mikk_get_num_faces(const SMikkTSpaceContext* ctx)
{
    const auto* d = static_cast<const MikkPrimData*>(ctx->m_pUserData);
    return static_cast<int>(d->face_count);
}

static int mikk_get_num_verts_of_face(const SMikkTSpaceContext*, const int)
{
    return 3;
}

static void mikk_get_position(const SMikkTSpaceContext* ctx,
                               float* pos_out,
                               const int face, const int vert)
{
    const auto* d  = static_cast<const MikkPrimData*>(ctx->m_pUserData);
    const crd::u32 vi = d->indices[static_cast<crd::u32>(face) * 3U
                                   + static_cast<crd::u32>(vert)];
    pos_out[0] = d->positions[vi * 3U + 0U];
    pos_out[1] = d->positions[vi * 3U + 1U];
    pos_out[2] = d->positions[vi * 3U + 2U];
}

static void mikk_get_normal(const SMikkTSpaceContext* ctx,
                             float* norm_out,
                             const int face, const int vert)
{
    const auto* d  = static_cast<const MikkPrimData*>(ctx->m_pUserData);
    const crd::u32 vi = d->indices[static_cast<crd::u32>(face) * 3U
                                   + static_cast<crd::u32>(vert)];
    norm_out[0] = d->normals[vi * 3U + 0U];
    norm_out[1] = d->normals[vi * 3U + 1U];
    norm_out[2] = d->normals[vi * 3U + 2U];
}

static void mikk_get_tex_coord(const SMikkTSpaceContext* ctx,
                                float* uv_out,
                                const int face, const int vert)
{
    const auto* d  = static_cast<const MikkPrimData*>(ctx->m_pUserData);
    const crd::u32 vi = d->indices[static_cast<crd::u32>(face) * 3U
                                   + static_cast<crd::u32>(vert)];
    uv_out[0] = d->uvs[vi * 2U + 0U];
    uv_out[1] = d->uvs[vi * 2U + 1U];
}

static void mikk_set_tspace_basic(const SMikkTSpaceContext* ctx,
                                   const float* tangent,
                                   const float  sign,
                                   const int face, const int vert)
{
    const auto* d  = static_cast<const MikkPrimData*>(ctx->m_pUserData);
    const crd::u32 vi = d->indices[static_cast<crd::u32>(face) * 3U
                                   + static_cast<crd::u32>(vert)];
    d->tangents[vi * 4U + 0U] = tangent[0];
    d->tangents[vi * 4U + 1U] = tangent[1];
    d->tangents[vi * 4U + 2U] = tangent[2];
    d->tangents[vi * 4U + 3U] = sign;
}

static bool generate_tangents(const float*     positions,
                               const float*     normals,
                               const float*     uvs,
                               const crd::u32*  indices,
                               crd::u32         vertex_count,
                               crd::u32         face_count,
                               float*           tangents_out)
{
    // Zero-init tangents: any vertex not reachable via MikkTSpace gets (1,0,0,1).
    for (crd::u32 vi = 0U; vi < vertex_count; ++vi)
    {
        tangents_out[vi * 4U + 0U] = 1.0F;
        tangents_out[vi * 4U + 1U] = 0.0F;
        tangents_out[vi * 4U + 2U] = 0.0F;
        tangents_out[vi * 4U + 3U] = 1.0F;
    }

    MikkPrimData user_data;
    user_data.positions  = positions;
    user_data.normals    = normals;
    user_data.uvs        = uvs;
    user_data.tangents   = tangents_out;
    user_data.indices    = indices;
    user_data.face_count = face_count;

    SMikkTSpaceInterface iface;
    std::memset(&iface, 0, sizeof(iface));
    iface.m_getNumFaces          = mikk_get_num_faces;
    iface.m_getNumVerticesOfFace = mikk_get_num_verts_of_face;
    iface.m_getPosition          = mikk_get_position;
    iface.m_getNormal            = mikk_get_normal;
    iface.m_getTexCoord          = mikk_get_tex_coord;
    iface.m_setTSpaceBasic       = mikk_set_tspace_basic;
    iface.m_setTSpace            = nullptr;

    SMikkTSpaceContext mikk_ctx;
    mikk_ctx.m_pInterface = &iface;
    mikk_ctx.m_pUserData  = &user_data;

    return genTangSpaceDefault(&mikk_ctx) != 0;
}

// ── Primitive cook ─────────────────────────────────────────────────────────

struct PrimResult
{
    crd::containers::Array<crd::u8> vertex_bytes; // interleaved, kMeshVertexStride per vertex
    crd::containers::Array<crd::u8> index_bytes;  // u32 indices
    crd::u32                        vertex_count = 0;
    crd::u32                        index_count  = 0;
    bool                            ok           = false;
};

PrimResult cook_primitive(const cgltf_primitive& prim, crd::memory::IAllocator* alloc)
{
    PrimResult result;
    result.vertex_bytes = crd::containers::Array<crd::u8>(alloc);
    result.index_bytes  = crd::containers::Array<crd::u8>(alloc);

    if (prim.type != cgltf_primitive_type_triangles)
    {
        std::fprintf(stderr, "mesh_cook: skipping non-triangle primitive\n");
        result.ok = true; // non-error skip
        return result;
    }

    if (prim.indices == nullptr)
    {
        std::fprintf(stderr, "mesh_cook: primitive has no index accessor\n");
        return result;
    }

    // Find accessors.
    const cgltf_accessor* pos_acc  = nullptr;
    const cgltf_accessor* norm_acc = nullptr;
    const cgltf_accessor* uv_acc   = nullptr;
    const cgltf_accessor* tan_acc  = nullptr;

    for (cgltf_size ai = 0U; ai < prim.attributes_count; ++ai)
    {
        const cgltf_attribute& attr = prim.attributes[ai];
        if (attr.type == cgltf_attribute_type_position)  { pos_acc  = attr.data; }
        if (attr.type == cgltf_attribute_type_normal)    { norm_acc = attr.data; }
        if (attr.type == cgltf_attribute_type_texcoord && attr.index == 0) { uv_acc  = attr.data; }
        if (attr.type == cgltf_attribute_type_tangent)   { tan_acc  = attr.data; }
    }

    if (pos_acc == nullptr)
    {
        std::fprintf(stderr, "mesh_cook: primitive missing POSITION accessor\n");
        return result;
    }

    const crd::u32 vc = static_cast<crd::u32>(pos_acc->count);
    const crd::u32 ic = static_cast<crd::u32>(prim.indices->count);

    if (vc == 0U || ic == 0U || (ic % 3U) != 0U)
    {
        std::fprintf(stderr, "mesh_cook: invalid vertex/index counts\n");
        return result;
    }

    // Read positions.
    crd::containers::Array<float> positions(alloc);
    positions.resize(static_cast<crd::usize>(vc) * 3U);
    for (crd::u32 vi = 0U; vi < vc; ++vi)
    {
        (void)cgltf_accessor_read_float(pos_acc, static_cast<cgltf_size>(vi),
                                         &positions[vi * 3U], 3U);
    }

    // Read normals (default to (0,0,1) if absent).
    crd::containers::Array<float> normals(alloc);
    normals.resize(static_cast<crd::usize>(vc) * 3U);
    if (norm_acc != nullptr)
    {
        for (crd::u32 vi = 0U; vi < vc; ++vi)
        {
            (void)cgltf_accessor_read_float(norm_acc, static_cast<cgltf_size>(vi),
                                             &normals[vi * 3U], 3U);
        }
    }
    else
    {
        for (crd::u32 vi = 0U; vi < vc; ++vi)
        {
            normals[vi * 3U + 2U] = 1.0F; // (0,0,1)
        }
    }

    // Read UV0 (default to (0,0) if absent).
    crd::containers::Array<float> uvs(alloc);
    uvs.resize(static_cast<crd::usize>(vc) * 2U);
    if (uv_acc != nullptr)
    {
        for (crd::u32 vi = 0U; vi < vc; ++vi)
        {
            (void)cgltf_accessor_read_float(uv_acc, static_cast<cgltf_size>(vi),
                                             &uvs[vi * 2U], 2U);
        }
    }

    // Read or generate tangents.
    crd::containers::Array<float> tangents(alloc);
    tangents.resize(static_cast<crd::usize>(vc) * 4U);

    if (tan_acc != nullptr)
    {
        for (crd::u32 vi = 0U; vi < vc; ++vi)
        {
            (void)cgltf_accessor_read_float(tan_acc, static_cast<cgltf_size>(vi),
                                             &tangents[vi * 4U], 4U);
        }
    }
    else
    {
        // Read indices first (needed for MikkTSpace).
        crd::containers::Array<crd::u32> idx_u32(alloc);
        idx_u32.resize(static_cast<crd::usize>(ic));
        for (crd::u32 ii = 0U; ii < ic; ++ii)
        {
            crd::u32 val = 0U;
            (void)cgltf_accessor_read_uint(prim.indices, static_cast<cgltf_size>(ii), &val, 1U);
            idx_u32[ii] = val;
        }

        (void)generate_tangents(positions.data(), normals.data(), uvs.data(),
                                 idx_u32.data(), vc, ic / 3U, tangents.data());

        // Write index bytes and return early to avoid re-reading indices below.
        result.index_bytes.resize(static_cast<crd::usize>(ic) * sizeof(crd::u32));
        std::memcpy(result.index_bytes.data(), idx_u32.data(),
                    static_cast<crd::usize>(ic) * sizeof(crd::u32));

        // Build interleaved vertex buffer.
        result.vertex_bytes.resize(static_cast<crd::usize>(vc) * kMeshVertexStride);
        crd::u8* dst = result.vertex_bytes.data();
        for (crd::u32 vi = 0U; vi < vc; ++vi, dst += kMeshVertexStride)
        {
            std::memcpy(dst +  0, &positions[vi * 3U], 12U);
            std::memcpy(dst + 12, &normals[vi * 3U],   12U);
            std::memcpy(dst + 24, &uvs[vi * 2U],        8U);
            std::memcpy(dst + 32, &tangents[vi * 4U],  16U);
        }

        result.vertex_count = vc;
        result.index_count  = ic;
        result.ok           = true;
        return result;
    }

    // Read indices (TANGENT was present — read indices here).
    result.index_bytes.resize(static_cast<crd::usize>(ic) * sizeof(crd::u32));
    auto* idx_dst = reinterpret_cast<crd::u32*>(result.index_bytes.data()); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
    for (crd::u32 ii = 0U; ii < ic; ++ii)
    {
        crd::u32 val = 0U;
        (void)cgltf_accessor_read_uint(prim.indices, static_cast<cgltf_size>(ii), &val, 1U);
        idx_dst[ii] = val;
    }

    // Build interleaved vertex buffer.
    result.vertex_bytes.resize(static_cast<crd::usize>(vc) * kMeshVertexStride);
    crd::u8* dst = result.vertex_bytes.data();
    for (crd::u32 vi = 0U; vi < vc; ++vi, dst += kMeshVertexStride)
    {
        std::memcpy(dst +  0, &positions[vi * 3U], 12U);
        std::memcpy(dst + 12, &normals[vi * 3U],   12U);
        std::memcpy(dst + 24, &uvs[vi * 2U],        8U);
        std::memcpy(dst + 32, &tangents[vi * 4U],  16U);
    }

    result.vertex_count = vc;
    result.index_count  = ic;
    result.ok           = true;
    return result;
}

// ── MESH artifact assembly ─────────────────────────────────────────────────

// Builds a CRDR MESH artifact from one cgltf_mesh.
// Returns empty Array on failure (no valid TRIANGLE primitives).
crd::containers::Array<crd::u8> build_mesh_artifact(
    const cgltf_mesh&              mesh,
    const crd::resources::ResourceId& id,
    crd::memory::IAllocator*       alloc)
{
    crd::containers::Array<crd::u8> vert_buf(alloc);
    crd::containers::Array<crd::u8> indx_buf(alloc);

    // PRIM table: 4-byte count + N * 32-byte entries.
    crd::containers::Array<crd::u8> prim_buf(alloc);

    crd::u32 prim_count = 0U;

    for (cgltf_size pi = 0U; pi < mesh.primitives_count; ++pi)
    {
        PrimResult pr = cook_primitive(mesh.primitives[pi], alloc);
        if (!pr.ok)
        {
            return crd::containers::Array<crd::u8>(alloc); // hard fail
        }
        if (pr.vertex_count == 0U)
        {
            continue; // skip (e.g. non-triangle, soft skip)
        }

        const crd::u32 vbo = static_cast<crd::u32>(vert_buf.size());
        const crd::u32 ibo = static_cast<crd::u32>(indx_buf.size());

        // Append vertex and index bytes.
        for (crd::usize bi = 0U; bi < pr.vertex_bytes.size(); ++bi)
        {
            vert_buf.push_back(pr.vertex_bytes[bi]);
        }
        for (crd::usize bi = 0U; bi < pr.index_bytes.size(); ++bi)
        {
            indx_buf.push_back(pr.index_bytes[bi]);
        }

        // Write 32-byte primitive entry into prim_buf.
        crd::u8 entry[32] = {};
        std::memcpy(entry +  0, &pr.vertex_count, 4U);
        std::memcpy(entry +  4, &pr.index_count,  4U);
        std::memcpy(entry +  8, &vbo,              4U);
        std::memcpy(entry + 12, &ibo,              4U);
        // material_id: null UUID (all zeros) — wired in Phase 2.7 v1c.

        for (crd::u8 b : entry)
        {
            prim_buf.push_back(b);
        }

        ++prim_count;
    }

    if (prim_count == 0U)
    {
        std::fprintf(stderr, "mesh_cook: mesh '%s' has no valid TRIANGLE primitives\n",
                     (mesh.name != nullptr) ? mesh.name : "<unnamed>");
        return crd::containers::Array<crd::u8>(alloc);
    }

    // Prepend primitive count to prim_buf.
    crd::containers::Array<crd::u8> prim_final(alloc);
    prim_final.resize(4U);
    std::memcpy(prim_final.data(), &prim_count, 4U);
    for (crd::u8 b : prim_buf)
    {
        prim_final.push_back(b);
    }

    crd::resources::CrdrWriter writer(alloc, id, crd::resources::kFourCC_MESH);
    writer.add_chunk(crd::resources::kFourCC_VERT,
                     crd::containers::as_const_span(vert_buf));
    writer.add_chunk(crd::resources::kFourCC_INDX,
                     crd::containers::as_const_span(indx_buf));
    writer.add_chunk(crd::resources::kFourCC_PRIM,
                     crd::containers::as_const_span(prim_final));

    return writer.finish();
}

// ── Sanitize mesh name for use in filesystem paths ─────────────────────────

crd::containers::String sanitize_for_path(const char* name, crd::memory::IAllocator* alloc)
{
    crd::containers::String s(alloc);
    if (name == nullptr || name[0] == '\0')
    {
        s.append("mesh");
        return s;
    }
    for (const char* p = name; *p != '\0'; ++p)
    {
        const char c = *p;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
            || (c >= '0' && c <= '9') || c == '_' || c == '-')
        {
            s.push_back(c);
        }
        else
        {
            s.push_back('_');
        }
    }
    return s;
}

// ── Cook handler entry point ───────────────────────────────────────────────

CookResult gltf_handler(const CookContext& ctx)
{
    CookResult result(ctx.allocator);

    // Parse the glTF file.
    cgltf_options options;
    std::memset(&options, 0, sizeof(options));
    cgltf_data* data = nullptr;

    // cgltf_parse_file expects a null-terminated C string path.
    crd::containers::String path_str(ctx.source_path.data(),
                                     ctx.source_path.size(),
                                     ctx.allocator);
    const cgltf_result parse_res = cgltf_parse_file(&options, path_str.c_str(), &data);
    if (parse_res != cgltf_result_success || data == nullptr)
    {
        std::fprintf(stderr, "mesh_cook: cgltf_parse_file failed for %s\n",
                     path_str.c_str());
        return result;
    }

    const cgltf_result buf_res = cgltf_load_buffers(&options, data, path_str.c_str());
    if (buf_res != cgltf_result_success)
    {
        std::fprintf(stderr, "mesh_cook: cgltf_load_buffers failed for %s\n",
                     path_str.c_str());
        cgltf_free(data);
        return result;
    }

    if (data->meshes_count == 0U)
    {
        std::fprintf(stderr, "mesh_cook: no meshes in %s\n", path_str.c_str());
        cgltf_free(data);
        return result;
    }

    // Mesh 0 → main result (uses ctx.id from the source .meta file).
    {
        auto bytes = build_mesh_artifact(data->meshes[0], ctx.id, ctx.allocator);
        if (bytes.empty())
        {
            cgltf_free(data);
            return result;
        }
        result.cooked_bytes    = std::move(bytes);
        result.type_fourcc     = crd::resources::kFourCC_MESH;
        result.handler_version = kMeshHandlerVersion;
        result.ok              = true;
    }

    // Meshes 1..N-1 → extra_artifacts, each with a per-mesh .meta sidecar.
    for (cgltf_size mi = 1U; mi < data->meshes_count; ++mi)
    {
        const cgltf_mesh& mesh = data->meshes[mi];

        // Build the meta path: <source_path>.mesh.<sanitized_name>.meta
        const crd::containers::String safe_name =
            sanitize_for_path(mesh.name, ctx.allocator);

        crd::containers::String meta_path_str(ctx.source_path.data(),
                                               ctx.source_path.size(),
                                               ctx.allocator);
        meta_path_str.append(".mesh.");
        meta_path_str.append(safe_name.c_str());
        meta_path_str.append(".meta");

        const fs::Path meta_path(
            crd::containers::StringView(meta_path_str.data(), meta_path_str.size()));

        crd::resources::ResourceId extra_id;
        if (fs::is_file(meta_path))
        {
            if (!mesh_meta_read(meta_path, extra_id))
            {
                std::fprintf(stderr,
                             "mesh_cook: malformed extra .meta for mesh '%s', regenerating\n",
                             (mesh.name != nullptr) ? mesh.name : "<unnamed>");
                extra_id = crd::resources::ResourceId::mint_random();
                (void)mesh_meta_write(meta_path, extra_id);
            }
        }
        else
        {
            extra_id = crd::resources::ResourceId::mint_random();
            if (!mesh_meta_write(meta_path, extra_id))
            {
                std::fprintf(stderr, "mesh_cook: failed to write extra .meta for mesh '%s'\n",
                             (mesh.name != nullptr) ? mesh.name : "<unnamed>");
                cgltf_free(data);
                result.ok = false;
                return result;
            }
        }

        auto bytes = build_mesh_artifact(mesh, extra_id, ctx.allocator);
        if (bytes.empty())
        {
            cgltf_free(data);
            result.ok = false;
            return result;
        }

        ExtraArtifact extra(ctx.allocator);
        extra.id          = extra_id;
        extra.type_fourcc = crd::resources::kFourCC_MESH;
        extra.cooked_bytes = std::move(bytes);

        // Display name: "<rel_path>#<mesh_name>"
        extra.name.append(ctx.source_path.data(), ctx.source_path.size());
        extra.name.push_back('#');
        if (mesh.name != nullptr)
        {
            extra.name.append(mesh.name);
        }
        else
        {
            extra.name.append("mesh_");
            crd::containers::String idx_str(ctx.allocator);
            // Simple integer-to-string for the mesh index.
            char buf[32] = {};
            (void)std::snprintf(buf, sizeof(buf), "%zu", static_cast<crd::usize>(mi));
            extra.name.append(buf);
        }

        result.extra_artifacts.push_back(std::move(extra));
    }

    cgltf_free(data);
    return result;
}

} // anonymous namespace

void register_mesh_handler()
{
    register_cook_handler(".glb",  gltf_handler);
    register_cook_handler(".gltf", gltf_handler);
}

} // namespace crd::cooker
