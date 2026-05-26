// bench_hesap_matrix_resource_vs_reference.cpp -- Phase 3.1.6 v4-corpus.
//
// Proves REAL SuiteSparse matrices (fetched from the internet at build time
// into CRD_SUITESPARSE_DIR) flow through the cooked-resource pipeline, and
// times the resource path:
//   - reference   : read_matrix_market (direct text -> CSR).
//   - resource    : cook (text -> 'HMTX' bytes) + read_matrix_resource +
//                   build_csr (bytes -> CSR) -- the deserialize cost.
// Each matrix is ALSO loaded once through the real ResourceManager mount +
// load_sync<SparseMatrixResource> path and verified (nnz + topology_hash),
// proving "real internet matrix loaded as a resource" on real data.
//
// Gated behind CRD_BUILD_HESAP_VS_REFERENCE (CRD_SUITESPARSE_DIR). No Eigen /
// OpenBLAS needed -- the reference here is our own direct reader.

#include <crd/containers/array.hpp>
#include <crd/hesap/resources/matrix_artifact_builder.hpp>
#include <crd/hesap/resources/matrix_resource.hpp>
#include <crd/hesap/resources/matrix_resource_loader.hpp>
#include <crd/hesap/sparse/matrix_market.hpp>
#include <crd/hesap/sparse/sparse_matrix.hpp>
#include <crd/memory/allocators/malloc_allocator.hpp>
#include <crd/platform/filesystem.hpp>
#include <crd/resources/crdr.hpp>
#include <crd/resources/resource_handle.hpp>
#include <crd/resources/resource_id.hpp>
#include <crd/resources/resource_manager.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#ifndef CRD_SUITESPARSE_DIR
#define CRD_SUITESPARSE_DIR "."
#endif

using namespace crd::resources;
namespace hr = crd::hesap::resources;
namespace hs = crd::hesap::sparse;
using Clock = std::chrono::steady_clock;

static crd::memory::MallocAllocator g_alloc;

static std::string slurp(const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f)
    {
        return std::string{};
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

template <typename Fn> static double best_ms(Fn&& fn)
{
    fn(); // warmup
    double best = 1e30;
    for (int rep = 0; rep < 5; ++rep)
    {
        const auto t0 = Clock::now();
        fn();
        const auto t1 = Clock::now();
        best = std::min(best, std::chrono::duration<double, std::milli>(t1 - t0).count());
    }
    return best;
}

// One-shot real ResourceManager path: cook -> pack -> mount -> load_sync.
static bool verify_resource_manager_path(const hs::SparseMatrix<crd::f64, hs::SparseFormat::Csr>& m)
{
    const ResourceId mid_res = ResourceId::mint_random();
    auto art = hr::cook_sparse_matrix<crd::f64>(&g_alloc, mid_res, m);

    const ResourceId pack_id = ResourceId::mint_random();
    crd::containers::Array<crd::u8> pool(&g_alloc);
    pool.push_back(0);
    crd::containers::Array<ManifestEntry> entries(&g_alloc);
    ManifestEntry e;
    e.id = mid_res;
    e.type_fourcc = hr::kFourCC_HMTX;
    e.blob_size = static_cast<crd::u64>(art.size());
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
    const auto str_id = pack_id.to_string(&g_alloc);
    crd::containers::String tmp("bench_matrix_", &g_alloc);
    tmp.append(str_id);
    tmp.append(".crdr");
    const crd::platform::fs::Path path(tmp);
    if (!crd::platform::fs::write_file_binary(path, crd::containers::as_const_span(pack_bytes)))
    {
        return false;
    }

    bool ok = false;
    {
        ResourceManager rm(&g_alloc);
        hr::register_hesap_matrix_loader(&rm);
        const MountId mid = rm.mount_manifest(path.generic());
        if (mid.is_valid())
        {
            auto h = rm.load_sync<hr::SparseMatrixResource>(mid_res);
            if (h.state() == LoadState::Ready && h.get() != nullptr &&
                h.get()->nnz() == static_cast<crd::u64>(m.nnz()) &&
                h.get()->topology_hash() == m.pattern().topology_hash)
            {
                // Real-data + typed-materialize round-trip: build_csr<f64> on the
                // loaded resource and spot-check against the source matrix.
                auto built = h.get()->build_csr<crd::f64>(&g_alloc);
                ok = (built.nnz() == m.nnz()) && (built.coeff(0, 0) == m.coeff(0, 0));
            }
        }
    }
    (void)crd::platform::fs::remove_file(path);
    return ok;
}

static void run(const char* name)
{
    const std::string path = std::string(CRD_SUITESPARSE_DIR) + "/" + name + "/" + name + ".mtx";
    const std::string text = slurp(path);
    if (text.empty())
    {
        std::printf("  %-12s SKIP (not found: %s)\n", name, path.c_str());
        return;
    }
    const crd::containers::StringView sv{text.data(), text.size()};

    // Reference: direct text -> CSR.
    crd::usize ref_nnz = 0;
    const double t_read = best_ms(
        [&]()
        {
            hs::MatrixMarketError err{&g_alloc};
            auto m = hs::read_matrix_market<crd::f64>(sv, &g_alloc, err);
            ref_nnz = m.nnz();
        });

    // Resource path: cook once, then time deserialize (read_matrix_resource + build_csr).
    hs::MatrixMarketError err{&g_alloc};
    auto ref = hs::read_matrix_market<crd::f64>(sv, &g_alloc, err);
    auto cooked = hr::cook_sparse_matrix<crd::f64>(&g_alloc, ResourceId::mint_random(), ref);

    crd::usize res_nnz = 0;
    const double t_load = best_ms(
        [&]()
        {
            hr::SparseMatrixResource res{&g_alloc};
            (void)hr::read_matrix_resource(crd::containers::ConstSpan<crd::u8>{cooked.data(), cooked.size()}, res,
                                           &g_alloc);
            auto m = res.build_csr<crd::f64>(&g_alloc);
            res_nnz = m.nnz();
        });

    const bool rm_ok = verify_resource_manager_path(ref);
    const bool nnz_ok = (ref_nnz == res_nnz) && (ref_nnz == ref.nnz());
    std::printf("  %-12s rows=%-7u nnz=%-9lld | read=%7.2f resource(cook+load)=%7.2f ms  cooked=%lld B  RM=%s %s\n",
                name, ref.rows(), static_cast<long long>(ref.nnz()), t_read, t_load,
                static_cast<long long>(cooked.size()), rm_ok ? "OK" : "FAIL", (nnz_ok && rm_ok) ? "PASS" : "MISMATCH");
}

int main()
{
    std::printf("hesap matrix-resource corpus bench (real SuiteSparse via cooked 'HMTX' resources)\n");
    const char* names[] = {"bcsstk13", "bcsstk24", "bcsstk25", "gemat11", "sherman3", "west2021"};
    for (const char* n : names)
    {
        run(n);
    }
    return 0;
}
