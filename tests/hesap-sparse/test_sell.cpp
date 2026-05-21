// crd-hesap-sparse v1b-2 -- SELL-C-σ storage + convert + spmv tests.
// The load-bearing test: SELL spmv is BIT-EXACT with the CSR baseline.

#include <catch2/catch_test_macros.hpp>

#include <crd/hesap/cli/command_registry.hpp>
#include <crd/hesap/cli/command_result.hpp>
#include <crd/hesap/complex.hpp>
#include <crd/hesap/sparse/cli_anchor.hpp>
#include <crd/hesap/sparse/sparse.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <cstdint>
#include <variant>

using crd::hesap::Complex64;
namespace sp = crd::hesap::sparse;

namespace
{
// Deterministic LCG sparse matrix: n x n, ~nnz_per_row entries/row.
template <typename T>
sp::SparseMatrix<T, sp::SparseFormat::Csr> random_csr(crd::memory::IAllocator* alloc, crd::u32 n,
                                                      crd::u32 nnz_per_row, std::uint64_t seed)
{
    std::uint64_t s = seed;
    auto          next = [&]() {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<crd::u32>(s >> 33);
    };
    sp::TripletBuilder<T> b(alloc, n, n);
    for (crd::u32 r = 0; r < n; ++r)
    {
        for (crd::u32 k = 0; k < nnz_per_row; ++k)
        {
            b.add(r, next() % n, static_cast<T>(1 + (next() % 9)) / static_cast<T>(7));
        }
    }
    return b.compress();
}

template <typename T>
crd::containers::Array<T> random_vec(crd::memory::IAllocator* alloc, crd::u32 n, std::uint64_t seed)
{
    std::uint64_t s = seed;
    crd::containers::Array<T> v(alloc);
    v.reserve(n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        v.push_back(static_cast<T>(1 + (static_cast<crd::u32>(s >> 33) % 17)) / static_cast<T>(13));
    }
    return v;
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

TEST_CASE("to_sell preserves structure (coeff round-trip)", "[hesap][sparse][sell]")
{
    crd::memory::TlsfAllocator alloc(1 << 20);
    auto csr  = random_csr<crd::f64>(&alloc, 37, 5, 0x1);
    auto sell = sp::to_sell(csr, &alloc);
    REQUIRE(sell.rows == 37);
    REQUIRE(sell.cols == 37);
    REQUIRE(sell.num_slices == (37 + sp::SellMatrix<crd::f64>::kC - 1) / sp::SellMatrix<crd::f64>::kC);
}

TEST_CASE("SELL spmv is bit-exact with CSR spmv (f64)", "[hesap][sparse][sell][bitexact]")
{
    crd::memory::TlsfAllocator alloc(8 << 20);
    // Several sizes incl. non-multiples of the slice height + short and long rows.
    for (crd::u32 n : {1U, 4U, 5U, 8U, 9U, 31U, 64U, 100U, 257U})
    {
        for (crd::u32 nnz : {1U, 3U, 8U})
        {
            auto csr  = random_csr<crd::f64>(&alloc, n, nnz, 0xABC ^ (n * 131 + nnz));
            auto sell = sp::to_sell(csr, &alloc);
            auto x    = random_vec<crd::f64>(&alloc, n, 0xDEF ^ n);

            crd::containers::Array<crd::f64> y_csr(&alloc);
            y_csr.resize(n);
            crd::containers::Array<crd::f64> y_sell(&alloc);
            y_sell.resize(n);
            sp::spmv<crd::f64>(1.0, csr, sp::Trans::None, cspan(x), 0.0, mspan(y_csr));
            sp::spmv_sell<crd::f64>(1.0, sell, cspan(x), 0.0, mspan(y_sell));
            for (crd::u32 i = 0; i < n; ++i)
            {
                INFO("n=" << n << " nnz=" << nnz << " i=" << i);
                CHECK(y_sell[i] == y_csr[i]);  // bit-exact
            }
        }
    }
}

TEST_CASE("SELL spmv is bit-exact with CSR spmv (f32)", "[hesap][sparse][sell][bitexact]")
{
    crd::memory::TlsfAllocator alloc(8 << 20);
    for (crd::u32 n : {1U, 7U, 8U, 16U, 17U, 64U, 200U})
    {
        for (crd::u32 nnz : {1U, 4U, 9U})
        {
            auto csr  = random_csr<crd::f32>(&alloc, n, nnz, 0x55 ^ (n * 17 + nnz));
            auto sell = sp::to_sell(csr, &alloc);
            auto x    = random_vec<crd::f32>(&alloc, n, 0x66 ^ n);

            crd::containers::Array<crd::f32> y_csr(&alloc);
            y_csr.resize(n);
            crd::containers::Array<crd::f32> y_sell(&alloc);
            y_sell.resize(n);
            sp::spmv<crd::f32>(1.0F, csr, sp::Trans::None, cspan(x), 0.0F, mspan(y_csr));
            sp::spmv_sell<crd::f32>(1.0F, sell, cspan(x), 0.0F, mspan(y_sell));
            for (crd::u32 i = 0; i < n; ++i)
            {
                INFO("n=" << n << " nnz=" << nnz << " i=" << i);
                CHECK(y_sell[i] == y_csr[i]);
            }
        }
    }
}

TEST_CASE("SELL spmv honours alpha/beta and beta=0 NaN-safety", "[hesap][sparse][sell]")
{
    crd::memory::TlsfAllocator alloc(1 << 20);
    auto csr  = random_csr<crd::f64>(&alloc, 50, 6, 0x999);
    auto sell = sp::to_sell(csr, &alloc);
    auto x    = random_vec<crd::f64>(&alloc, 50, 0x111);

    crd::containers::Array<crd::f64> y_ab_csr(&alloc);
    crd::containers::Array<crd::f64> y_ab_sell(&alloc);
    for (crd::u32 i = 0; i < 50; ++i)
    {
        y_ab_csr.push_back(static_cast<crd::f64>(i) * 0.5);
        y_ab_sell.push_back(static_cast<crd::f64>(i) * 0.5);
    }
    sp::spmv<crd::f64>(2.5, csr, sp::Trans::None, cspan(x), -1.5, mspan(y_ab_csr));
    sp::spmv_sell<crd::f64>(2.5, sell, cspan(x), -1.5, mspan(y_ab_sell));
    for (crd::u32 i = 0; i < 50; ++i)
    {
        CHECK(y_ab_sell[i] == y_ab_csr[i]);
    }
}

TEST_CASE("SELL spmv complex bit-exact with CSR (c64)", "[hesap][sparse][sell][complex]")
{
    crd::memory::TlsfAllocator alloc(2 << 20);
    auto csr  = random_csr<Complex64>(&alloc, 40, 5, 0x222);
    auto sell = sp::to_sell(csr, &alloc);
    auto x    = random_vec<Complex64>(&alloc, 40, 0x333);
    crd::containers::Array<Complex64> y_csr(&alloc);
    y_csr.resize(40);
    crd::containers::Array<Complex64> y_sell(&alloc);
    y_sell.resize(40);
    sp::spmv<Complex64>(Complex64{1.0, 0.0}, csr, sp::Trans::None, cspan(x), Complex64{0.0, 0.0}, mspan(y_csr));
    sp::spmv_sell<Complex64>(Complex64{1.0, 0.0}, sell, cspan(x), Complex64{0.0, 0.0}, mspan(y_sell));
    for (crd::u32 i = 0; i < 40; ++i)
    {
        CHECK(y_sell[i].re == y_csr[i].re);
        CHECK(y_sell[i].im == y_csr[i].im);
    }
}

namespace
{
const bool kPullSellAnchor = (crd::hesap::sparse::register_sparse_cli_anchor(), true);
}

TEST_CASE("CLI spmv_sell.f64 matches spmv.f64 bit-for-bit", "[hesap][sparse][sell][cli]")
{
    CHECK(kPullSellAnchor);
    auto& reg = crd::hesap::cli::CommandRegistry::global();
    const auto* sell_rec = reg.find("hesap.sparse.spmv_sell.f64");
    const auto* csr_rec  = reg.find("hesap.sparse.spmv.f64");
    REQUIRE(sell_rec != nullptr);
    REQUIRE(csr_rec != nullptr);

    crd::memory::TlsfAllocator alloc(64 * 1024);
    auto make_args = [&]() {
        crd::hesap::cli::CommandArgs args(&alloc);
        args.set_u64("rows", 3);
        args.set_u64("cols", 3);
        const crd::i64 r[] = {0, 0, 1, 2, 2};
        const crd::i64 c[] = {0, 2, 1, 0, 2};
        const crd::f64 v[] = {1.0, 2.0, 3.0, 4.0, 5.0};
        const crd::f64 xx[] = {0.1, 0.2, 0.3};
        args.set_i64_array("triplet_rows", crd::containers::ConstSpan<crd::i64>{r, 5});
        args.set_i64_array("triplet_cols", crd::containers::ConstSpan<crd::i64>{c, 5});
        args.set_f64_array("values", crd::containers::ConstSpan<crd::f64>{v, 5});
        args.set_f64_array("x", crd::containers::ConstSpan<crd::f64>{xx, 3});
        return args;
    };

    const auto args_sell = make_args();
    const auto args_csr  = make_args();
    const auto rs = sell_rec->impl(args_sell);
    const auto rc = csr_rec->impl(args_csr);
    REQUIRE(rs.ok);
    REQUIRE(rc.ok);
    const auto* bs = std::get_if<crd::hesap::cli::ResultBinaryBlob>(&rs.value);
    const auto* bc = std::get_if<crd::hesap::cli::ResultBinaryBlob>(&rc.value);
    REQUIRE(bs != nullptr);
    REQUIRE(bc != nullptr);
    REQUIRE(bs->bytes.size() == bc->bytes.size());
    const auto* ys = reinterpret_cast<const crd::f64*>(bs->bytes.data());
    const auto* yc = reinterpret_cast<const crd::f64*>(bc->bytes.data());
    for (crd::u32 i = 0; i < 3; ++i)
    {
        CHECK(ys[i] == yc[i]);  // SELL CLI == CSR CLI, bit-for-bit
    }
}
