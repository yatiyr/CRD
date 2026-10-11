#pragma once

// Shared by the resource manager's fault-injection cases (DIAG.9b) and its workloads (DIAG.10c): one-artifact PACK
// files, a one-triangle MESH artifact whose first x marks its content, a parsed fault plan and a scoped I/O hook.

#include <crd/containers/array.hpp>
#include <crd/containers/string.hpp>
#include <crd/core/fault_seam.hpp>
#include <crd/memory/allocator.hpp>
#include <crd/perf/fault_injection.hpp>
#include <crd/platform/filesystem.hpp>
#include <crd/resources/crdr.hpp>
#include <crd/resources/mesh_resource.hpp>
#include <crd/resources/resource_id.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstring>

namespace crd_test::fault_pack
{
namespace fs   = crd::platform::fs;
namespace cp   = crd::perf;
namespace cont = crd::containers;
namespace res  = crd::resources;

inline constexpr cp::DiagAuthoritySet kInject = cp::authority_bit(cp::DiagAuthority::Inject);

inline cp::FaultPlan plan_of(const char* text)
{
    cp::FaultPlan plan;
    cont::String  reason;
    const bool    parsed = cp::parse_fault_plan(cont::StringView{text}, plan, reason);
    INFO(reason.c_str());
    REQUIRE(parsed);
    return plan;
}

// <temp>/<stem><id>.crdr
inline fs::Path temp_pack_path(crd::memory::IAllocator* alloc, const char* stem, res::ResourceId id)
{
    cont::String name(stem, alloc);
    name.append(id.to_string(alloc));
    name.append(".crdr");
    return fs::temp_directory() / cont::StringView{name.data(), name.size()};
}

// One artifact in a PACK: the manifest, then the artifact bytes. False when the file could not be written.
inline bool try_write_pack(crd::memory::IAllocator* alloc, const fs::Path& path, res::ResourceId id, crd::u32 fourcc,
                           const cont::Array<crd::u8>& artifact)
{
    cont::Array<crd::u8> pool(alloc);
    for (const char c : cont::StringView{"fault.art"})
    {
        pool.push_back(static_cast<crd::u8>(c));
    }
    pool.push_back(0U);

    cont::Array<res::ManifestEntry> entries(alloc);
    res::ManifestEntry              e;
    e.id            = id;
    e.type_fourcc   = fourcc;
    e.flags         = 0U;
    e.blob_offset   = 0U;
    e.blob_size     = static_cast<crd::u64>(artifact.size());
    e.name_strp_idx = 0U;
    entries.push_back(e);

    const res::ResourceId pack_id = res::ResourceId::mint_random();
    {
        res::CrdrWriter measure(alloc, pack_id, res::kFourCC_PACK);
        res::manifest_write(measure, cont::as_const_span(entries), cont::as_const_span(pool));
        entries[0].blob_offset = static_cast<crd::u64>(measure.finish().size());
    }
    res::CrdrWriter writer(alloc, pack_id, res::kFourCC_PACK);
    res::manifest_write(writer, cont::as_const_span(entries), cont::as_const_span(pool));
    cont::Array<crd::u8> bytes = writer.finish();
    for (const crd::u8 b : artifact)
    {
        bytes.push_back(b);
    }
    return fs::write_file_binary(path, cont::as_const_span(bytes));
}

inline void write_pack(crd::memory::IAllocator* alloc, const fs::Path& path, res::ResourceId id, crd::u32 fourcc,
                       const cont::Array<crd::u8>& artifact)
{
    REQUIRE(try_write_pack(alloc, path, id, fourcc, artifact));
}

// A one-triangle MESH artifact whose first vertex's x is `x` (so a reload's new content is visible).
inline cont::Array<crd::u8> mesh_artifact(crd::memory::IAllocator* alloc, res::ResourceId id, crd::f32 x)
{
    constexpr crd::u32 stride = 48U;
    crd::u8            vertices[3U * stride] = {};
    for (crd::u32 v = 0U; v < 3U; ++v)
    {
        const crd::f32 pos[3] = {v == 0U ? x : static_cast<crd::f32>(v), 0.0F, 0.0F};
        std::memcpy(vertices + v * stride, pos, sizeof(pos));
    }
    const crd::u32 indices[3] = {0U, 1U, 2U};
    crd::u8        prim[4U + 32U] = {};
    const crd::u32 prim_header[5] = {1U, 3U, 3U, 0U, 0U}; // count; vertex count, index count, both offsets 0
    std::memcpy(prim, prim_header, sizeof(prim_header));

    res::CrdrWriter writer(alloc, id, res::kFourCC_MESH);
    writer.add_chunk(res::kFourCC_VERT, cont::ConstSpan<crd::u8>{vertices, sizeof(vertices)});
    writer.add_chunk(res::kFourCC_INDX,
                     cont::ConstSpan<crd::u8>{reinterpret_cast<const crd::u8*>(indices), sizeof(indices)});
    writer.add_chunk(res::kFourCC_PRIM, cont::ConstSpan<crd::u8>{prim, sizeof(prim)});
    return writer.finish();
}

inline crd::f32 first_x(const res::MeshResource& mesh)
{
    crd::f32 x = 0.0F;
    std::memcpy(&x, mesh.vertices.data(), sizeof(x));
    return x;
}

// Installs the injector on the I/O fault seam for one scope.
struct ScopedIoHook
{
    bool installed = false;

    explicit ScopedIoHook(cp::FaultInjector& injector)
        : installed(crd::install_io_fault_hook(&cp::FaultInjector::io_hook, &injector))
    {
    }
    ~ScopedIoHook()
    {
        if (installed)
        {
            crd::uninstall_io_fault_hook();
        }
    }
    ScopedIoHook(const ScopedIoHook&)            = delete;
    ScopedIoHook& operator=(const ScopedIoHook&) = delete;
    ScopedIoHook(ScopedIoHook&&)                 = delete;
    ScopedIoHook& operator=(ScopedIoHook&&)      = delete;
};

} // namespace crd_test::fault_pack
