// crd-hesap-sparse v1a-1 -- substrate tests.
//
// Discriminating set (per advisor): empty pattern; golden-hex topology hash;
// format-collision regression (CSR vs CSC, identical indices); capacity-
// invariance (logical content only); AnalysisHandle staleness; complex
// SparseValues sizeof + 4-type instantiation.

#include <catch2/catch_test_macros.hpp>

#include <crd/hesap/complex.hpp>
#include <crd/hesap/crdr_format.hpp>
#include <crd/hesap/sparse/sparse.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <utility>

using crd::hesap::Complex32;
using crd::hesap::Complex64;
namespace sp = crd::hesap::sparse;

namespace
{

// Build the canonical 3x3 identity in CSR: outer_ptr = [0,1,2,3], inner = [0,1,2].
sp::SparsePattern make_3x3_csr_identity(crd::memory::IAllocator* alloc)
{
    sp::SparsePattern p(alloc);
    p.rows = 3;
    p.cols = 3;
    p.format = sp::SparseFormat::Csr;
    p.block_size = 1;
    for (crd::u32 v : {0U, 1U, 2U, 3U})
    {
        p.outer_ptr.push_back(v);
    }
    for (crd::u32 v : {0U, 1U, 2U})
    {
        p.inner_idx.push_back(v);
    }
    p.recompute_topology_hash();
    return p;
}

} // namespace

TEST_CASE("SparsePattern default is empty", "[hesap][sparse][substrate]")
{
    crd::memory::TlsfAllocator alloc(64 * 1024);
    sp::SparsePattern p(&alloc);
    CHECK(p.rows == 0);
    CHECK(p.cols == 0);
    CHECK(p.nnz() == 0);
    CHECK(p.format == sp::SparseFormat::Csr);
    CHECK(p.block_size == 1);
}

TEST_CASE("topology_hash is a stable golden for the canonical 3x3 CSR identity",
          "[hesap][sparse][substrate][hash]")
{
    crd::memory::TlsfAllocator alloc(64 * 1024);
    sp::SparsePattern p = make_3x3_csr_identity(&alloc);

    // Golden computed once on win-debug; CI's linux-gcc re-run proves the
    // little-endian explicit-byte feed is cross-platform reproducible (D1).
    // Value reflects the v1a-2 canonical (slack-invariant) hash refinement.
    constexpr crd::u64 golden3x3_csr_identity = 0x9978E97C37B7D174ULL;

    CHECK(p.topology_hash == golden3x3_csr_identity);
    CHECK(sp::topology_hash(p) == golden3x3_csr_identity);  // free fn == cached
}

TEST_CASE("topology_hash differs when format flips CSR<->CSC at identical indices",
          "[hesap][sparse][substrate][hash]")
{
    crd::memory::TlsfAllocator alloc(64 * 1024);
    sp::SparsePattern csr = make_3x3_csr_identity(&alloc);

    sp::SparsePattern csc = make_3x3_csr_identity(&alloc);
    csc.format = sp::SparseFormat::Csc;
    csc.recompute_topology_hash();

    // Same outer_ptr + inner_idx, different orientation => different matrix
    // => must hash differently (analysis-cache correctness invariant).
    CHECK(csr.topology_hash != csc.topology_hash);
}

TEST_CASE("topology_hash depends on logical content only, not Array capacity",
          "[hesap][sparse][substrate][hash]")
{
    crd::memory::TlsfAllocator alloc(64 * 1024);
    sp::SparsePattern a = make_3x3_csr_identity(&alloc);

    sp::SparsePattern b(&alloc);
    b.rows = 3;
    b.cols = 3;
    b.format = sp::SparseFormat::Csr;
    b.block_size = 1;
    b.outer_ptr.reserve(128);  // extra capacity, same logical content
    b.inner_idx.reserve(256);
    for (crd::u32 v : {0U, 1U, 2U, 3U})
    {
        b.outer_ptr.push_back(v);
    }
    for (crd::u32 v : {0U, 1U, 2U})
    {
        b.inner_idx.push_back(v);
    }
    b.recompute_topology_hash();

    CHECK(a.topology_hash == b.topology_hash);
}

TEST_CASE("AnalysisHandle::is_valid_for tracks structural mutation",
          "[hesap][sparse][substrate][analysis]")
{
    crd::memory::TlsfAllocator alloc(64 * 1024);
    sp::SparsePattern p = make_3x3_csr_identity(&alloc);

    sp::AnalysisHandle handle;
    handle.topology_hash = p.topology_hash;
    handle.recommended_format = sp::SparseFormat::Csr;
    CHECK(handle.is_valid_for(p));

    // Mutate structure: append a column to row 2 (a 4th stored entry).
    p.outer_ptr[3] = 4;
    p.inner_idx.push_back(0U);
    p.recompute_topology_hash();

    CHECK_FALSE(handle.is_valid_for(p));

    // A default (zero-hash) handle is never valid, even against a zero-hash
    // pattern -- guards against "uninitialised handle accidentally matches".
    sp::AnalysisHandle fresh;
    sp::SparsePattern empty(&alloc);
    empty.recompute_topology_hash();
    CHECK_FALSE(fresh.is_valid_for(empty));
}

TEST_CASE("SparseValues holds 4 type variants; Complex64 packs to 16 bytes",
          "[hesap][sparse][substrate][values]")
{
    crd::memory::TlsfAllocator alloc(64 * 1024);

    sp::SparseValues<crd::f32> vf32(&alloc);
    sp::SparseValues<crd::f64> vf64(&alloc);
    sp::SparseValues<Complex32> vc32(&alloc);
    sp::SparseValues<Complex64> vc64(&alloc);
    CHECK(vf32.size() == 0);
    CHECK(vf64.size() == 0);
    CHECK(vc32.size() == 0);
    CHECK(vc64.size() == 0);

    vc64.values.push_back(Complex64{1.0, -2.0});
    vc64.frame_stamp = 7;
    CHECK(vc64.size() == 1);
    CHECK(vc64.values[0].re == 1.0);
    CHECK(vc64.values[0].im == -2.0);
    CHECK(vc64.frame_stamp == 7);

    static_assert(sizeof(Complex64) == 16, "Complex64 must pack to 16 bytes");
}

TEST_CASE("SparseId packs to 8 bytes and round-trips index/generation",
          "[hesap][sparse][substrate][id]")
{
    const sp::SparseId id = sp::SparseId::make(/*idx=*/42, /*gen=*/7);
    CHECK(id.index() == 42);
    CHECK(id.generation() == 7);
    CHECK_FALSE(id.is_null());
    CHECK(sp::SparseId::null().is_null());
    static_assert(sizeof(sp::SparseId) == 8, "SparseId must pack to 8 bytes");
}

TEST_CASE("SparseMatrix adopts a moved pattern+values and reports density",
          "[hesap][sparse][substrate][matrix]")
{
    crd::memory::TlsfAllocator alloc(64 * 1024);
    sp::SparsePattern p = make_3x3_csr_identity(&alloc);

    sp::SparseValues<crd::f64> vals(&alloc);
    for (int i = 0; i < 3; ++i)
    {
        vals.values.push_back(1.0);  // identity diagonal
    }

    sp::SparseMatrix<crd::f64, sp::SparseFormat::Csr> m(std::move(p), std::move(vals));
    CHECK(m.rows() == 3);
    CHECK(m.cols() == 3);
    CHECK(m.nnz() == 3);
    CHECK(m.format == sp::SparseFormat::Csr);
    CHECK(m.density() == (3.0 / 9.0));

    sp::SparseMatrix<crd::f64, sp::SparseFormat::Csr> empty(&alloc);
    CHECK(empty.density() == 0.0);
}

TEST_CASE("'HSPM' CRDR FourCC spells the right little-endian bytes",
          "[hesap][sparse][substrate][format]")
{
    CHECK((crd::hesap::kHesapSparseFourCC & 0xFF) == 'H');
    CHECK(((crd::hesap::kHesapSparseFourCC >> 8) & 0xFF) == 'S');
    CHECK(((crd::hesap::kHesapSparseFourCC >> 16) & 0xFF) == 'P');
    CHECK(((crd::hesap::kHesapSparseFourCC >> 24) & 0xFF) == 'M');
}
