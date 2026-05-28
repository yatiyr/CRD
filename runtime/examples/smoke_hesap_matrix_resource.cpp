// smoke_hesap_matrix_resource.cpp -- end-to-end smoke for crd-hesap-resources
// (Phase 3.1.6 v4-corpus).
//
// Cooks a sparse matrix into an 'HMTX' CRDR artifact, writes a PACK file,
// mounts it, loads it through ResourceManager::load_sync<SparseMatrixResource>,
// rebuilds a typed SparseMatrix<f64, Csr>, and verifies the values. This is the
// REAL resource path (mount + load_sync), not the free-function round-trip the
// unit tests use. Exits 0 on success.

#include <crd/containers/array.hpp>
#include <crd/hesap/resources/matrix_artifact_builder.hpp>
#include <crd/hesap/resources/matrix_resource.hpp>
#include <crd/hesap/resources/matrix_resource_loader.hpp>
#include <crd/hesap/sparse/sparse_matrix.hpp>
#include <crd/hesap/sparse/triplet_builder.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>
#include <crd/platform/filesystem.hpp>
#include <crd/resources/crdr.hpp>
#include <crd/resources/resource_handle.hpp>
#include <crd/resources/resource_id.hpp>
#include <crd/resources/resource_manager.hpp>

#include <cstdio>

using namespace crd::resources;
namespace hr = crd::hesap::resources;
namespace hs = crd::hesap::sparse;

static crd::memory::TlsfAllocator g_alloc{256ULL << 20};

// Embed an already-cooked artifact CRDR into a single-entry PACK (two-pass to
// fix the blob offset), mirroring smoke_resources::assemble_pack.
static crd::platform::fs::Path assemble_pack(ResourceId artifact_id, crd::u32 type_fourcc,
                                             crd::containers::ConstSpan<crd::u8> art_bytes)
{
    const ResourceId pack_id = ResourceId::mint_random();

    crd::containers::Array<crd::u8> pool(&g_alloc);
    const char name[] = "smoke_matrix";
    for (char c : name)
    {
        pool.push_back(static_cast<crd::u8>(c));
    }

    crd::containers::Array<ManifestEntry> entries(&g_alloc);
    ManifestEntry e;
    e.id = artifact_id;
    e.type_fourcc = type_fourcc;
    e.flags = 0U;
    e.blob_offset = 0U;
    e.blob_size = static_cast<crd::u64>(art_bytes.size());
    e.name_strp_idx = 0U;
    entries.push_back(e);

    {
        CrdrWriter p1(&g_alloc, pack_id, kFourCC_PACK);
        manifest_write(p1, crd::containers::as_const_span(entries), crd::containers::as_const_span(pool));
        const auto b1 = p1.finish();
        entries[0].blob_offset = static_cast<crd::u64>(b1.size());
    }

    CrdrWriter p2(&g_alloc, pack_id, kFourCC_PACK);
    manifest_write(p2, crd::containers::as_const_span(entries), crd::containers::as_const_span(pool));
    auto pack_bytes = p2.finish();
    for (crd::usize i = 0; i < art_bytes.size(); ++i)
    {
        pack_bytes.push_back(art_bytes[i]);
    }

    const auto str_id = pack_id.to_string(&g_alloc);
    crd::containers::String tmp("smoke_hesap_matrix_", &g_alloc);
    tmp.append(str_id);
    tmp.append(".crdr");
    const crd::platform::fs::Path path(tmp);
    if (!crd::platform::fs::write_file_binary(path, crd::containers::as_const_span(pack_bytes)))
    {
        std::fprintf(stderr, "smoke_hesap_matrix_resource: failed to write pack\n");
        std::exit(1);
    }
    return path;
}

int main()
{
    // Build a small reference matrix and remember its expected CSR values.
    hs::TripletBuilder<crd::f64> b(&g_alloc, 4, 4);
    b.add(0, 0, 4.0);
    b.add(0, 2, 1.0);
    b.add(1, 1, 3.0);
    b.add(2, 0, 1.0);
    b.add(2, 2, 5.0);
    b.add(3, 3, 2.0);
    auto original = b.compress();
    const crd::usize nnz = original.nnz();

    const ResourceId matrix_id = ResourceId::mint_random();
    auto art = hr::cook_sparse_matrix<crd::f64>(&g_alloc, matrix_id, original);
    const auto pack_path =
        assemble_pack(matrix_id, hr::kFourCC_HMTX, crd::containers::ConstSpan<crd::u8>{art.data(), art.size()});

    ResourceManager rm(&g_alloc);
    hr::register_hesap_matrix_loader(&rm);

    const MountId mid = rm.mount_manifest(pack_path.generic());
    if (!mid.is_valid())
    {
        std::fprintf(stderr, "smoke_hesap_matrix_resource: mount_manifest failed\n");
        (void)crd::platform::fs::remove_file(pack_path);
        return 1;
    }

    auto handle = rm.load_sync<hr::SparseMatrixResource>(matrix_id);
    if (handle.state() != LoadState::Ready || handle.get() == nullptr)
    {
        std::fprintf(stderr, "smoke_hesap_matrix_resource: load_sync failed (state=%d)\n",
                     static_cast<int>(handle.state()));
        (void)crd::platform::fs::remove_file(pack_path);
        return 1;
    }

    const hr::SparseMatrixResource* res = handle.get();
    if (res->rows() != 4 || res->cols() != 4 || res->nnz() != static_cast<crd::u64>(nnz) ||
        res->topology_hash() != original.pattern().topology_hash)
    {
        std::fprintf(stderr, "smoke_hesap_matrix_resource: header mismatch\n");
        (void)crd::platform::fs::remove_file(pack_path);
        return 1;
    }

    auto rebuilt = res->build_csr<crd::f64>(&g_alloc);
    bool ok = (rebuilt.coeff(0, 0) == 4.0) && (rebuilt.coeff(0, 2) == 1.0) && (rebuilt.coeff(1, 1) == 3.0) &&
              (rebuilt.coeff(2, 0) == 1.0) && (rebuilt.coeff(2, 2) == 5.0) && (rebuilt.coeff(3, 3) == 2.0);

    (void)crd::platform::fs::remove_file(pack_path);
    if (!ok)
    {
        std::fprintf(stderr, "smoke_hesap_matrix_resource: value mismatch after build_csr\n");
        return 1;
    }
    std::printf("smoke_hesap_matrix_resource: OK -- HMTX matrix loaded via ResourceManager and verified\n");
    return 0;
}
