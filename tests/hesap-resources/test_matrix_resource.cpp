#include <crd/hesap/cli/command_registry.hpp>
#include <crd/hesap/complex.hpp>
#include <crd/hesap/resources/matrix_artifact_builder.hpp>
#include <crd/hesap/resources/matrix_resource.hpp>
#include <crd/hesap/resources/matrix_resource_loader.hpp>
#include <crd/hesap/sparse/sparse_matrix.hpp>
#include <crd/hesap/sparse/triplet_builder.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>
#include <crd/resources/resource_id.hpp>

#include <catch2/catch_test_macros.hpp>
#include <cstring>

using namespace crd::hesap::resources;
using namespace crd::hesap::sparse;
using crd::hesap::Complex;

namespace
{
// Build a small deterministic 4x4 CSR test matrix (SPD-ish pattern, but only
// the storage round-trip matters here).
template <typename T> SparseMatrix<T, SparseFormat::Csr> make_test_csr(crd::memory::IAllocator* a)
{
    TripletBuilder<T> b(a, 4, 4);
    b.add(0, 0, T{4});
    b.add(0, 2, T{1});
    b.add(1, 1, T{3});
    b.add(2, 0, T{1});
    b.add(2, 2, T{5});
    b.add(3, 3, T{2});
    return b.compress();
}

// Assert two compressed CSR matrices are byte-exact (structure + values).
template <typename T>
void require_csr_equal(const SparseMatrix<T, SparseFormat::Csr>& x, const SparseMatrix<T, SparseFormat::Csr>& y)
{
    REQUIRE(x.rows() == y.rows());
    REQUIRE(x.cols() == y.cols());
    REQUIRE(x.nnz() == y.nnz());
    REQUIRE(x.pattern().topology_hash == y.pattern().topology_hash);

    const auto& xp = x.pattern();
    const auto& yp = y.pattern();
    REQUIRE(xp.outer_ptr.size() == yp.outer_ptr.size());
    REQUIRE(std::memcmp(xp.outer_ptr.data(), yp.outer_ptr.data(), xp.outer_ptr.size() * sizeof(crd::u32)) == 0);
    REQUIRE(xp.inner_idx.size() == yp.inner_idx.size());
    REQUIRE(std::memcmp(xp.inner_idx.data(), yp.inner_idx.data(), xp.inner_idx.size() * sizeof(crd::u32)) == 0);
    REQUIRE(x.values().size() == y.values().size());
    REQUIRE(std::memcmp(x.values().values.data(), y.values().values.data(), x.values().size() * sizeof(T)) == 0);
}

template <typename T> void round_trip(crd::memory::IAllocator* a)
{
    auto original = make_test_csr<T>(a);

    const crd::resources::ResourceId id{0x1234ULL, 0x5678ULL};
    auto bytes = cook_sparse_matrix<T>(a, id, original);
    REQUIRE(bytes.size() > sizeof(MatrixFileInfo));

    SparseMatrixResource res{a};
    REQUIRE(read_matrix_resource(crd::containers::ConstSpan<crd::u8>{bytes.data(), bytes.size()}, res, a));

    REQUIRE(res.rows() == original.rows());
    REQUIRE(res.cols() == original.cols());
    REQUIRE(res.nnz() == static_cast<crd::u64>(original.nnz()));
    REQUIRE(res.variant() == matrix_variant_of<T>());
    REQUIRE(res.topology_hash() == original.pattern().topology_hash);

    auto rebuilt = res.template build_csr<T>(a);
    require_csr_equal<T>(original, rebuilt);
}
} // namespace

TEST_CASE("matrix resource cook+load round-trip is byte-exact (f64)", "[hesap-resources][matrix]")
{
    crd::memory::TlsfAllocator alloc{1U << 20};
    round_trip<crd::f64>(&alloc);
}

TEST_CASE("matrix resource cook+load round-trip is byte-exact (f32)", "[hesap-resources][matrix]")
{
    crd::memory::TlsfAllocator alloc{1U << 20};
    round_trip<crd::f32>(&alloc);
}

TEST_CASE("matrix resource cook+load round-trip is byte-exact (c64)", "[hesap-resources][matrix]")
{
    crd::memory::TlsfAllocator alloc{1U << 20};
    round_trip<Complex<crd::f64>>(&alloc);
}

TEST_CASE("matrix resource cook+load round-trip is byte-exact (c32)", "[hesap-resources][matrix]")
{
    crd::memory::TlsfAllocator alloc{1U << 20};
    round_trip<Complex<crd::f32>>(&alloc);
}

TEST_CASE("MatrixFileInfo header is pinned at 40 bytes", "[hesap-resources][matrix]")
{
    STATIC_REQUIRE(sizeof(MatrixFileInfo) == 40);
    STATIC_REQUIRE(static_cast<crd::u8>(MatrixVariant::F32) == 0);
    STATIC_REQUIRE(static_cast<crd::u8>(MatrixVariant::F64) == 1);
    STATIC_REQUIRE(static_cast<crd::u8>(MatrixVariant::C32) == 2);
    STATIC_REQUIRE(static_cast<crd::u8>(MatrixVariant::C64) == 3);
}

TEST_CASE("matrix resource rejects a malformed blob", "[hesap-resources][matrix]")
{
    crd::memory::TlsfAllocator alloc{1U << 20};
    crd::u8 garbage[16] = {0};
    SparseMatrixResource res{&alloc};
    REQUIRE_FALSE(read_matrix_resource(crd::containers::ConstSpan<crd::u8>{garbage, sizeof(garbage)}, res, &alloc));
}

TEST_CASE("matrix market text cooks into a loadable HMTX blob (f64)", "[hesap-resources][matrix]")
{
    crd::memory::TlsfAllocator alloc{1U << 20};
    const char* mtx = "%%MatrixMarket matrix coordinate real general\n"
                      "3 3 4\n"
                      "1 1 4.0\n"
                      "2 2 3.0\n"
                      "3 1 1.0\n"
                      "3 3 5.0\n";

    const crd::resources::ResourceId id{1ULL, 2ULL};
    MatrixMarketError err{&alloc};
    auto bytes = cook_matrix_market<crd::f64>(&alloc, id, crd::containers::StringView{mtx}, err);
    REQUIRE(err.ok);
    REQUIRE(bytes.size() > sizeof(MatrixFileInfo));

    SparseMatrixResource res{&alloc};
    REQUIRE(read_matrix_resource(crd::containers::ConstSpan<crd::u8>{bytes.data(), bytes.size()}, res, &alloc));
    REQUIRE(res.rows() == 3);
    REQUIRE(res.cols() == 3);
    REQUIRE(res.nnz() == 4);

    auto m = res.build_csr<crd::f64>(&alloc);
    REQUIRE(m.coeff(0, 0) == 4.0);
    REQUIRE(m.coeff(1, 1) == 3.0);
    REQUIRE(m.coeff(2, 0) == 1.0);
    REQUIRE(m.coeff(2, 2) == 5.0);
}

// ---- CLI surface (hesap.matrix.{info,cook,load}) -----------------------

namespace
{
// Force-link the CLI registration TU (static-init drops otherwise).
const bool kPullMatrixCliAnchor = (crd::hesap::resources::register_hesap_matrix_cli_anchor(), true);

const char* const kMtx3x3 = "%%MatrixMarket matrix coordinate real general\n"
                            "3 3 4\n"
                            "1 1 4.0\n"
                            "2 2 3.0\n"
                            "3 1 1.0\n"
                            "3 3 5.0\n";
} // namespace

TEST_CASE("CLI hesap.matrix.info reports structural metadata", "[hesap-resources][matrix][cli]")
{
    REQUIRE(kPullMatrixCliAnchor);
    using namespace crd::hesap::cli;
    crd::memory::TlsfAllocator alloc{1U << 20};

    const auto* rec = CommandRegistry::global().find("hesap.matrix.info");
    REQUIRE(rec != nullptr);

    CommandArgs args{&alloc};
    args.set_string("text", crd::containers::StringView{kMtx3x3});
    auto r = rec->impl(args);
    REQUIRE(r.ok);
    const auto* text = std::get_if<ResultText>(&r.value);
    REQUIRE(text != nullptr);
    const crd::containers::StringView sv{text->text.c_str()};
    REQUIRE(sv.find("rows=3") != crd::containers::StringView::npos);
    REQUIRE(sv.find("cols=3") != crd::containers::StringView::npos);
    REQUIRE(sv.find("nnz=4") != crd::containers::StringView::npos);
}

TEST_CASE("CLI hesap.matrix.cook produces a loadable HMTX blob", "[hesap-resources][matrix][cli]")
{
    REQUIRE(kPullMatrixCliAnchor);
    using namespace crd::hesap::cli;
    crd::memory::TlsfAllocator alloc{1U << 20};

    const auto* rec = CommandRegistry::global().find("hesap.matrix.cook.f64");
    REQUIRE(rec != nullptr);

    CommandArgs args{&alloc};
    args.set_string("text", crd::containers::StringView{kMtx3x3});
    auto r = rec->impl(args);
    REQUIRE(r.ok);
    const auto* blob = std::get_if<ResultBinaryBlob>(&r.value);
    REQUIRE(blob != nullptr);
    REQUIRE(blob->bytes.size() > sizeof(crd::hesap::resources::MatrixFileInfo));

    // The cooked bytes load back into a SparseMatrixResource.
    crd::hesap::resources::SparseMatrixResource res{&alloc};
    REQUIRE(crd::hesap::resources::read_matrix_resource(
        crd::containers::ConstSpan<crd::u8>{blob->bytes.data(), blob->bytes.size()}, res, &alloc));
    REQUIRE(res.rows() == 3);
    REQUIRE(res.nnz() == 4);
}

TEST_CASE("CLI hesap.matrix.load returns CSR values (f64)", "[hesap-resources][matrix][cli]")
{
    REQUIRE(kPullMatrixCliAnchor);
    using namespace crd::hesap::cli;
    crd::memory::TlsfAllocator alloc{1U << 20};

    const auto* rec = CommandRegistry::global().find("hesap.matrix.load.f64");
    REQUIRE(rec != nullptr);

    CommandArgs args{&alloc};
    args.set_string("text", crd::containers::StringView{kMtx3x3});
    auto r = rec->impl(args);
    REQUIRE(r.ok);
    const auto* blob = std::get_if<ResultBinaryBlob>(&r.value);
    REQUIRE(blob != nullptr);
    REQUIRE(blob->bytes.size() == 4U * sizeof(crd::f64));

    crd::f64 vals[4];
    std::memcpy(vals, blob->bytes.data(), sizeof(vals));
    // CSR row-major order: row0 col0=4; row1 col1=3; row2 col0=1, col2=5.
    REQUIRE(vals[0] == 4.0);
    REQUIRE(vals[1] == 3.0);
    REQUIRE(vals[2] == 1.0);
    REQUIRE(vals[3] == 5.0);
}
