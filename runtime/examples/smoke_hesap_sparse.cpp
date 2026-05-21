// smoke_hesap_sparse -- Phase 3.1.6 v1a-3 end-to-end smoke.
//
// Exercises the v1a sparse surface:
//   - COO TripletBuilder -> CSR compress() + CSC compress_csc()
//   - coeff() round-trip on both orientations
//   - structural_stats + topology_hash
//   - uncompressed insert path + make_compressed
//   - CLI dispatch of hesap.sparse.from_triplets.f64 through the registry
//
// Exit 0 on success; non-zero with a printed diagnostic on first failure.

#include <crd/hesap/cli/command_registry.hpp>
#include <crd/hesap/sparse/cli_anchor.hpp>
#include <crd/hesap/sparse/sparse.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <cstdio>
#include <variant>

namespace sp = crd::hesap::sparse;

namespace
{
int fail(const char* msg)
{
    std::fprintf(stderr, "[smoke_hesap_sparse] FAIL: %s\n", msg);
    return 1;
}

sp::TripletBuilder<crd::f64> make_builder(crd::memory::IAllocator* alloc)
{
    sp::TripletBuilder<crd::f64> b(alloc, 3, 3);
    b.add(0, 2, 2.0);
    b.add(0, 0, 1.0);
    b.add(1, 1, 3.0);
    b.add(2, 0, 4.0);
    b.add(2, 2, 5.0);
    return b;
}
} // namespace

int main()
{
    crd::memory::TlsfAllocator alloc(256 * 1024);

    // 1. CSR + CSC compress.
    auto csr = make_builder(&alloc).compress();
    auto csc = make_builder(&alloc).compress_csc();
    if (csr.nnz() != 5 || csc.nnz() != 5)
    {
        return fail("nnz mismatch after compress");
    }
    for (crd::u32 r = 0; r < 3; ++r)
    {
        for (crd::u32 c = 0; c < 3; ++c)
        {
            if (csr.coeff(r, c) != csc.coeff(r, c))
            {
                return fail("CSR/CSC coeff disagreement");
            }
        }
    }
    if (csr.pattern().topology_hash == csc.pattern().topology_hash)
    {
        return fail("CSR and CSC must hash differently");
    }

    // 2. Structural stats.
    const auto stats = sp::structural_stats(csr);
    if (stats.rows != 3 || stats.cols != 3 || stats.nnz != 5 || stats.max_inner_nnz != 2)
    {
        return fail("structural_stats mismatch");
    }

    // 3. Uncompressed insert path -> make_compressed equals direct compress.
    auto u = sp::SparseMatrix<crd::f64, sp::SparseFormat::Csr>::make_uncompressed(&alloc, 3, 3, 4);
    u.coeff_ref(2, 2) = 5.0;
    u.coeff_ref(0, 0) = 1.0;
    u.coeff_ref(0, 2) = 2.0;
    u.coeff_ref(1, 1) = 3.0;
    u.coeff_ref(2, 0) = 4.0;
    u.make_compressed();
    if (u.pattern().topology_hash != csr.pattern().topology_hash)
    {
        return fail("uncompressed->compressed hash != direct compress");
    }

    // 4. CLI dispatch.
    sp::register_sparse_cli_anchor();
    auto& reg = crd::hesap::cli::CommandRegistry::global();
    const auto* rec = reg.find("hesap.sparse.from_triplets.f64");
    if (rec == nullptr || rec->impl == nullptr)
    {
        return fail("hesap.sparse.from_triplets.f64 not registered");
    }
    crd::hesap::cli::CommandArgs args(&alloc);
    args.set_u64("rows", 3);
    args.set_u64("cols", 3);
    const crd::i64 rr[] = {0, 0, 1, 2, 2};
    const crd::i64 cc[] = {0, 2, 1, 0, 2};
    const crd::f64 vv[] = {1.0, 2.0, 3.0, 4.0, 5.0};
    args.set_i64_array("triplet_rows", crd::containers::ConstSpan<crd::i64>{rr, 5});
    args.set_i64_array("triplet_cols", crd::containers::ConstSpan<crd::i64>{cc, 5});
    args.set_f64_array("values", crd::containers::ConstSpan<crd::f64>{vv, 5});
    const auto res = rec->impl(args);
    const auto* scalar = std::get_if<crd::hesap::cli::ResultScalarF64>(&res.value);
    if (!res.ok || scalar == nullptr || scalar->value != 5.0)
    {
        return fail("CLI from_triplets.f64 did not return nnz=5");
    }

    std::fprintf(stdout, "[smoke_hesap_sparse] OK (CSR/CSC/uncompressed/CLI all consistent, nnz=5)\n");
    return 0;
}
