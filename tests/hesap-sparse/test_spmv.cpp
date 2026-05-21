// crd-hesap-sparse v1b-1 -- CSR spmv + transpose + SparseLinearOp tests.

#include <catch2/catch_test_macros.hpp>

#include <crd/hesap/cli/command_registry.hpp>
#include <crd/hesap/cli/command_result.hpp>
#include <crd/hesap/complex.hpp>
#include <crd/hesap/sparse/cli_anchor.hpp>
#include <crd/hesap/sparse/sparse.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <limits>
#include <variant>

using crd::hesap::Complex64;
namespace sp = crd::hesap::sparse;

namespace
{
// Canonical 3x3: row0 (0)=1 (2)=2 ; row1 (1)=3 ; row2 (0)=4 (2)=5.
//   A   = [[1,0,2],[0,3,0],[4,0,5]]
//   A^T = [[1,0,4],[0,3,0],[2,0,5]]
template <typename T>
sp::SparseMatrix<T, sp::SparseFormat::Csr> canonical(crd::memory::IAllocator* alloc)
{
    sp::TripletBuilder<T> b(alloc, 3, 3);
    b.add(0, 0, static_cast<T>(1));
    b.add(0, 2, static_cast<T>(2));
    b.add(1, 1, static_cast<T>(3));
    b.add(2, 0, static_cast<T>(4));
    b.add(2, 2, static_cast<T>(5));
    return b.compress();
}

template <typename T>
crd::containers::ConstSpan<T> cspan(const crd::containers::Array<T>& a)
{
    return crd::containers::ConstSpan<T>{a.data(), a.size()};
}
template <typename T>
crd::containers::Span<T> mspan(crd::containers::Array<T>& a)
{
    return crd::containers::Span<T>{a.data(), a.size()};
}
} // namespace

TEST_CASE("spmv y = A*x matches the dense oracle", "[hesap][sparse][spmv]")
{
    crd::memory::TlsfAllocator alloc(64 * 1024);
    auto m = canonical<crd::f64>(&alloc);

    crd::containers::Array<crd::f64> x(&alloc);
    for (crd::f64 v : {1.0, 1.0, 1.0})
    {
        x.push_back(v);
    }
    crd::containers::Array<crd::f64> y(&alloc);
    y.resize(3);
    sp::spmv<crd::f64>(1.0, m, sp::Trans::None, cspan(x), 0.0, mspan(y));
    CHECK(y[0] == 3.0);  // 1 + 2
    CHECK(y[1] == 3.0);  // 3
    CHECK(y[2] == 9.0);  // 4 + 5
}

TEST_CASE("spmv applies alpha and beta", "[hesap][sparse][spmv]")
{
    crd::memory::TlsfAllocator alloc(64 * 1024);
    auto m = canonical<crd::f64>(&alloc);
    crd::containers::Array<crd::f64> x(&alloc);
    for (crd::f64 v : {1.0, 1.0, 1.0})
    {
        x.push_back(v);
    }
    crd::containers::Array<crd::f64> y(&alloc);
    for (crd::f64 v : {10.0, 20.0, 30.0})
    {
        y.push_back(v);
    }
    sp::spmv<crd::f64>(2.0, m, sp::Trans::None, cspan(x), 1.0, mspan(y));
    CHECK(y[0] == 16.0);  // 2*3 + 10
    CHECK(y[1] == 26.0);  // 2*3 + 20
    CHECK(y[2] == 48.0);  // 2*9 + 30
}

TEST_CASE("spmv beta=0 does not read y (NaN-safe output)", "[hesap][sparse][spmv]")
{
    crd::memory::TlsfAllocator alloc(64 * 1024);
    auto m = canonical<crd::f64>(&alloc);
    crd::containers::Array<crd::f64> x(&alloc);
    for (crd::f64 v : {1.0, 1.0, 1.0})
    {
        x.push_back(v);
    }
    crd::containers::Array<crd::f64> y(&alloc);
    const crd::f64 nan = std::numeric_limits<crd::f64>::quiet_NaN();
    for (int i = 0; i < 3; ++i)
    {
        y.push_back(nan);
    }
    sp::spmv<crd::f64>(1.0, m, sp::Trans::None, cspan(x), 0.0, mspan(y));
    CHECK(y[0] == 3.0);  // NaN must NOT propagate through beta=0
    CHECK(y[1] == 3.0);
    CHECK(y[2] == 9.0);
}

TEST_CASE("spmv transpose matches the dense oracle (real)", "[hesap][sparse][spmv][transpose]")
{
    crd::memory::TlsfAllocator alloc(64 * 1024);
    auto m = canonical<crd::f64>(&alloc);
    crd::containers::Array<crd::f64> x(&alloc);
    for (crd::f64 v : {1.0, 1.0, 1.0})
    {
        x.push_back(v);
    }
    crd::containers::Array<crd::f64> y(&alloc);
    y.resize(3);
    sp::spmv<crd::f64>(1.0, m, sp::Trans::Transpose, cspan(x), 0.0, mspan(y));
    CHECK(y[0] == 5.0);  // 1 + 4
    CHECK(y[1] == 3.0);  // 3
    CHECK(y[2] == 7.0);  // 2 + 5
}

TEST_CASE("spmv conjugate-transpose conjugates entries (complex)", "[hesap][sparse][spmv][complex]")
{
    crd::memory::TlsfAllocator alloc(64 * 1024);
    // 2x2: (0,0)=1+2i, (1,0)=3-1i. A^H*x with x=[1,1] => column 0 gets conj(1+2i)+conj(3-1i).
    sp::TripletBuilder<Complex64> b(&alloc, 2, 2);
    b.add(0, 0, Complex64{1.0, 2.0});
    b.add(1, 0, Complex64{3.0, -1.0});
    auto m = b.compress();

    crd::containers::Array<Complex64> x(&alloc);
    x.push_back(Complex64{1.0, 0.0});
    x.push_back(Complex64{1.0, 0.0});
    crd::containers::Array<Complex64> y(&alloc);
    y.resize(2);
    sp::spmv<Complex64>(Complex64{1.0, 0.0}, m, sp::Trans::ConjTranspose, cspan(x), Complex64{0.0, 0.0}, mspan(y));
    // y[0] = conj(1+2i)*1 + conj(3-1i)*1 = (1-2i)+(3+1i) = 4 - 1i
    CHECK(y[0].re == 4.0);
    CHECK(y[0].im == -1.0);
    CHECK(y[1].re == 0.0);  // column 1 empty
    CHECK(y[1].im == 0.0);
}

TEST_CASE("spmv cross-platform y golden", "[hesap][sparse][spmv][golden]")
{
    crd::memory::TlsfAllocator alloc(64 * 1024);
    auto m = canonical<crd::f64>(&alloc);
    crd::containers::Array<crd::f64> x(&alloc);
    for (crd::f64 v : {0.1, 0.2, 0.3})  // non-power-of-two => exercises rounding
    {
        x.push_back(v);
    }
    crd::containers::Array<crd::f64> y(&alloc);
    y.resize(3);
    sp::spmv<crd::f64>(1.0, m, sp::Trans::None, cspan(x), 0.0, mspan(y));
    // Golden baked on win-debug; CI linux-gcc proves two-rounded reproducibility.
    CHECK(y[0] == 0.69999999999999996);  // 1*0.1 + 2*0.3
    CHECK(y[1] == 0.60000000000000009);  // 3*0.2
    CHECK(y[2] == 1.89999999999999991);  // 4*0.1 + 5*0.3
}

TEST_CASE("SparseLinearOp::apply is bit-identical to the spmv kernel", "[hesap][sparse][spmv][linearop]")
{
    crd::memory::TlsfAllocator alloc(64 * 1024);
    auto m = canonical<crd::f64>(&alloc);
    crd::containers::Array<crd::f64> x(&alloc);
    for (crd::f64 v : {0.1, 0.2, 0.3})
    {
        x.push_back(v);
    }

    crd::containers::Array<crd::f64> y_kernel(&alloc);
    y_kernel.resize(3);
    sp::spmv<crd::f64>(1.0, m, sp::Trans::None, cspan(x), 0.0, mspan(y_kernel));

    sp::SparseLinearOp<crd::f64> op(m);
    REQUIRE(op.n_rows() == 3);
    REQUIRE(op.n_cols() == 3);
    REQUIRE(op.has_transpose());
    REQUIRE(op.has_adjoint());
    crd::containers::Array<crd::f64> y_op(&alloc);
    y_op.resize(3);
    REQUIRE(op.apply(cspan(x), mspan(y_op)));
    for (int i = 0; i < 3; ++i)
    {
        CHECK(y_op[i] == y_kernel[i]);  // bit-identical, no wrapper drift
    }

    // apply_transpose matches the transpose kernel.
    crd::containers::Array<crd::f64> yt_kernel(&alloc);
    yt_kernel.resize(3);
    sp::spmv<crd::f64>(1.0, m, sp::Trans::Transpose, cspan(x), 0.0, mspan(yt_kernel));
    crd::containers::Array<crd::f64> yt_op(&alloc);
    yt_op.resize(3);
    REQUIRE(op.apply_transpose(cspan(x), mspan(yt_op)));
    for (int i = 0; i < 3; ++i)
    {
        CHECK(yt_op[i] == yt_kernel[i]);
    }
}

TEST_CASE("spmv is bit-reproducible across runs", "[hesap][sparse][spmv][determinism]")
{
    crd::memory::TlsfAllocator alloc(64 * 1024);
    auto run = [&]() {
        auto m = canonical<crd::f64>(&alloc);
        crd::containers::Array<crd::f64> x(&alloc);
        for (crd::f64 v : {0.1, 0.2, 0.3})
        {
            x.push_back(v);
        }
        crd::containers::Array<crd::f64> y(&alloc);
        y.resize(3);
        sp::spmv<crd::f64>(1.0, m, sp::Trans::None, cspan(x), 0.0, mspan(y));
        return y;
    };
    const auto a = run();
    const auto b = run();
    for (int i = 0; i < 3; ++i)
    {
        CHECK(a[i] == b[i]);
    }
}

namespace
{
const bool kPullSpmvAnchor = (crd::hesap::sparse::register_sparse_cli_anchor(), true);
}

TEST_CASE("spmv CLI dispatches and returns y", "[hesap][sparse][spmv][cli]")
{
    CHECK(kPullSpmvAnchor);
    auto& reg = crd::hesap::cli::CommandRegistry::global();
    const auto* rec = reg.find("hesap.sparse.spmv.f64");
    REQUIRE(rec != nullptr);

    crd::memory::TlsfAllocator alloc(64 * 1024);
    crd::hesap::cli::CommandArgs args(&alloc);
    args.set_u64("rows", 3);
    args.set_u64("cols", 3);
    const crd::i64 r[] = {0, 0, 1, 2, 2};
    const crd::i64 c[] = {0, 2, 1, 0, 2};
    const crd::f64 v[] = {1.0, 2.0, 3.0, 4.0, 5.0};
    const crd::f64 xx[] = {1.0, 1.0, 1.0};
    args.set_i64_array("triplet_rows", crd::containers::ConstSpan<crd::i64>{r, 5});
    args.set_i64_array("triplet_cols", crd::containers::ConstSpan<crd::i64>{c, 5});
    args.set_f64_array("values", crd::containers::ConstSpan<crd::f64>{v, 5});
    args.set_f64_array("x", crd::containers::ConstSpan<crd::f64>{xx, 3});

    const auto res = rec->impl(args);
    REQUIRE(res.ok);
    const auto* blob = std::get_if<crd::hesap::cli::ResultBinaryBlob>(&res.value);
    REQUIRE(blob != nullptr);
    REQUIRE(blob->bytes.size() == 3 * sizeof(crd::f64));
    const auto* y = reinterpret_cast<const crd::f64*>(blob->bytes.data());
    CHECK(y[0] == 3.0);
    CHECK(y[1] == 3.0);
    CHECK(y[2] == 9.0);
}
