// smoke_hesap_solve_cli.cpp -- Phase 3.1.6 v4a guardrail.
//
// The v4a thesis end-to-end: a matrix LOADED AS A RESOURCE drives a solver.
// (1) cook an SPD 1D-Laplacian into an 'HMTX' resource, (2) mount + load it
// through ResourceManager::load_sync<SparseMatrixResource>, (3) build_csr<f64>,
// (4) run CG on the resulting SparseLinearOp and verify convergence.
// Exits 0 on success.

#include <crd/containers/array.hpp>
#include <crd/hesap/dense/blas1.hpp>
#include <crd/hesap/dense/vector.hpp>
#include <crd/hesap/iterative/cg.hpp>
#include <crd/hesap/resources/matrix_artifact_builder.hpp>
#include <crd/hesap/resources/matrix_resource.hpp>
#include <crd/hesap/resources/matrix_resource_loader.hpp>
#include <crd/hesap/sparse/sparse_linear_op.hpp>
#include <crd/hesap/sparse/triplet_builder.hpp>
#include <crd/memory/allocators/malloc_allocator.hpp>
#include <crd/platform/filesystem.hpp>
#include <crd/resources/crdr.hpp>
#include <crd/resources/resource_handle.hpp>
#include <crd/resources/resource_id.hpp>
#include <crd/resources/resource_manager.hpp>

#include <cstdio>
#include <cstring>

using namespace crd::resources;
namespace hr   = crd::hesap::resources;
namespace hs   = crd::hesap::sparse;
namespace hi   = crd::hesap::iterative;
namespace hd   = crd::hesap::dense;

static crd::memory::MallocAllocator g_alloc;

static hs::SparseMatrix<crd::f64, hs::SparseFormat::Csr> laplacian(crd::u32 n)
{
    hs::TripletBuilder<crd::f64> b(&g_alloc, n, n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        b.add(i, i, 2.0);
        if (i > 0)
        {
            b.add(i, i - 1, -1.0);
        }
        if (i + 1 < n)
        {
            b.add(i, i + 1, -1.0);
        }
    }
    return b.compress();
}

static crd::platform::fs::Path write_pack(ResourceId mid, crd::containers::ConstSpan<crd::u8> art)
{
    const ResourceId pack_id = ResourceId::mint_random();
    crd::containers::Array<crd::u8> pool(&g_alloc);
    pool.push_back(0);
    crd::containers::Array<ManifestEntry> entries(&g_alloc);
    ManifestEntry                         e;
    e.id          = mid;
    e.type_fourcc = hr::kFourCC_HMTX;
    e.blob_size   = static_cast<crd::u64>(art.size());
    entries.push_back(e);
    {
        CrdrWriter p1(&g_alloc, pack_id, kFourCC_PACK);
        manifest_write(p1, crd::containers::as_const_span(entries), crd::containers::as_const_span(pool));
        entries[0].blob_offset = static_cast<crd::u64>(p1.finish().size());
    }
    CrdrWriter p2(&g_alloc, pack_id, kFourCC_PACK);
    manifest_write(p2, crd::containers::as_const_span(entries), crd::containers::as_const_span(pool));
    auto pack_bytes = p2.finish();
    for (crd::usize i = 0; i < art.size(); ++i)
    {
        pack_bytes.push_back(art[i]);
    }
    const auto              str_id = pack_id.to_string(&g_alloc);
    crd::containers::String tmp("smoke_solve_", &g_alloc);
    tmp.append(str_id);
    tmp.append(".crdr");
    const crd::platform::fs::Path path(tmp);
    if (!crd::platform::fs::write_file_binary(path, crd::containers::as_const_span(pack_bytes)))
    {
        std::fprintf(stderr, "smoke_hesap_solve_cli: write pack failed\n");
        std::exit(1);
    }
    return path;
}

int main()
{
    const crd::u32 n = 64;
    auto           a = laplacian(n);

    // (1) cook -> (2) pack/mount/load_sync as a resource.
    const ResourceId mid = ResourceId::mint_random();
    auto             art = hr::cook_sparse_matrix<crd::f64>(&g_alloc, mid, a);
    const auto       path = write_pack(mid, crd::containers::ConstSpan<crd::u8>{art.data(), art.size()});

    ResourceManager rm(&g_alloc);
    hr::register_hesap_matrix_loader(&rm);
    if (!rm.mount_manifest(path.generic()).is_valid())
    {
        std::fprintf(stderr, "smoke_hesap_solve_cli: mount failed\n");
        (void)crd::platform::fs::remove_file(path);
        return 1;
    }
    auto handle = rm.load_sync<hr::SparseMatrixResource>(mid);
    if (handle.state() != LoadState::Ready || handle.get() == nullptr)
    {
        std::fprintf(stderr, "smoke_hesap_solve_cli: load_sync failed\n");
        (void)crd::platform::fs::remove_file(path);
        return 1;
    }

    // (3) build_csr -> (4) run CG on the resource-loaded matrix.
    auto                          loaded = handle.get()->build_csr<crd::f64>(&g_alloc);
    hs::SparseLinearOp<crd::f64>  op(loaded);
    hd::Vector<crd::f64>          b(&g_alloc, n);
    b.fill(1.0);
    hd::Vector<crd::f64>          x(&g_alloc, n);
    hi::IterativeOptions<crd::f64> opts;
    opts.rel_tol = 1e-12;
    hi::KrylovWorkspace<crd::f64> ws(&g_alloc, n);
    auto                          res = hi::cg<crd::f64>(op, b.span(), x.span(), opts, ws, &g_alloc);

    (void)crd::platform::fs::remove_file(path);

    if (!res.converged)
    {
        std::fprintf(stderr, "smoke_hesap_solve_cli: CG did not converge\n");
        return 1;
    }
    // residual ‖A x - b‖ / ‖b‖
    hd::Vector<crd::f64> ax(&g_alloc, n);
    (void)op.apply(x.span(), ax.span());
    for (crd::u32 i = 0; i < n; ++i)
    {
        ax(i) = ax(i) - b(i);
    }
    const crd::f64 rel = hd::nrm2<crd::f64>(ax.span()) / hd::nrm2<crd::f64>(b.span());
    if (rel > 1e-9)
    {
        std::fprintf(stderr, "smoke_hesap_solve_cli: residual too large (%g)\n", rel);
        return 1;
    }

    std::printf("smoke_hesap_solve_cli: OK -- resource-loaded matrix solved by CG in %zu iters (rel resid %.2e)\n",
                res.iterations, rel);
    return 0;
}
