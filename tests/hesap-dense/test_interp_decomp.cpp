#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <crd/containers/array.hpp>
#include <crd/hesap/cli/command_registry.hpp>
#include <crd/hesap/dense/cli_anchor.hpp>
#include <crd/hesap/dense/interp_decomp.hpp>
#include <crd/hesap/dense/matrix.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <algorithm>
#include <cmath>

using crd::hesap::dense::interp_decomp;
using crd::hesap::dense::InterpDecomp;
using crd::hesap::dense::Layout;
using crd::hesap::dense::Matrix;
using Catch::Matchers::WithinAbs;

namespace
{
// Force cli_register_svd.cpp's TU (which also registers the ID commands) to link.
struct IdAnchorPull
{
    IdAnchorPull() noexcept { crd::hesap::dense::register_svd_cli_anchor(); }
};
const IdAnchorPull kIdAnchorPull;

template <typename T>
using Mat = Matrix<T, Layout::RowMajor>;

// Deterministic pseudo-random fill (same trick as test_cod / test_qr).
template <typename T>
T prand(crd::usize i, crd::usize j, T scale) noexcept
{
    return static_cast<T>(std::sin(static_cast<double>(i * 13 + j * 7 + 1) * 0.37) +
                          std::cos(static_cast<double>(i * 5 + j * 11 + 3) * 0.21)) *
           scale;
}

// A = B · C with B (m×k), C (k×n) ⇒ exact rank min(k, m, n).
template <typename T>
Mat<T> make_low_rank(crd::memory::IAllocator* alloc, crd::usize m, crd::usize n, crd::usize k)
{
    Mat<T> b(alloc, m, k);
    Mat<T> c(alloc, k, n);
    for (crd::usize i = 0; i < m; ++i)
    {
        for (crd::usize t = 0; t < k; ++t)
        {
            b.at(i, t) = prand<T>(i, t, static_cast<T>(1));
        }
    }
    for (crd::usize t = 0; t < k; ++t)
    {
        for (crd::usize j = 0; j < n; ++j)
        {
            c.at(t, j) = prand<T>(t + 100, j, static_cast<T>(1));
        }
    }
    Mat<T> a(alloc, m, n);
    for (crd::usize i = 0; i < m; ++i)
    {
        for (crd::usize j = 0; j < n; ++j)
        {
            T s = T{0};
            for (crd::usize t = 0; t < k; ++t)
            {
                s += b.at(i, t) * c.at(t, j);
            }
            a.at(i, j) = s;
        }
    }
    return a;
}

// ‖A − cols·proj‖_F (Frobenius). cols is m×rank, proj rank×n.
template <typename T>
T recon_error(const Mat<T>& a, const InterpDecomp<T, Layout::RowMajor>& id) noexcept
{
    const crd::usize m = a.rows();
    const crd::usize n = a.cols();
    T acc = T{0};
    for (crd::usize i = 0; i < m; ++i)
    {
        for (crd::usize j = 0; j < n; ++j)
        {
            T s = T{0};
            for (crd::usize r = 0; r < id.rank; ++r)
            {
                s += id.cols.at(i, r) * id.proj.at(r, j);
            }
            const T d = a.at(i, j) - s;
            acc += d * d;
        }
    }
    return std::sqrt(acc);
}

template <typename T>
T frob(const Mat<T>& a) noexcept
{
    T acc = T{0};
    for (crd::usize i = 0; i < a.size(); ++i)
    {
        acc += a.data()[i] * a.data()[i];
    }
    return std::sqrt(acc);
}

template <typename T>
T max_abs_proj(const InterpDecomp<T, Layout::RowMajor>& id) noexcept
{
    T mx = T{0};
    for (crd::usize i = 0; i < id.proj.size(); ++i)
    {
        const T v = std::abs(id.proj.data()[i]);
        if (v > mx)
        {
            mx = v;
        }
    }
    return mx;
}
} // namespace

TEST_CASE("interp_decomp: exact low-rank recovery (f64)", "[hesap][interp][real]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(8U * 1024U * 1024U));
    constexpr crd::usize m = 9;
    constexpr crd::usize n = 7;
    constexpr crd::usize k = 3;
    Mat<double> a = make_low_rank<double>(&alloc, m, n, k);

    auto id = interp_decomp<double, Layout::RowMajor>(&alloc, a);

    REQUIRE(id.rank == k);
    REQUIRE(id.skeleton.size() == k);
    REQUIRE(id.cols.rows() == m);
    REQUIRE(id.cols.cols() == k);
    REQUIRE(id.proj.rows() == k);
    REQUIRE(id.proj.cols() == n);
    // A is exactly rank-3 ⇒ A = cols·proj to rounding.
    CHECK(recon_error<double>(a, id) < 1e-11);
    // Soft sanity: column-pivoted QR keeps the interpolation matrix modest on
    // benign inputs (NOT a guaranteed strong-RRQR |Z| <= 2 bound — that needs
    // Gu-Eisenstat; this only catches a blow-up / NaN regression).
    CHECK(max_abs_proj<double>(id) < 1e2);
}

TEST_CASE("interp_decomp: interpolation property proj[:,J] = I", "[hesap][interp][real]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(8U * 1024U * 1024U));
    Mat<double> a = make_low_rank<double>(&alloc, 9, 7, 3);
    auto id = interp_decomp<double, Layout::RowMajor>(&alloc, a);
    REQUIRE(id.rank == 3);
    // The skeleton columns are reproduced exactly: proj[s, J[s]] = 1, others 0.
    for (crd::usize s = 0; s < id.rank; ++s)
    {
        for (crd::usize t = 0; t < id.rank; ++t)
        {
            const double expect = (s == t) ? 1.0 : 0.0;
            CHECK_THAT(id.proj.at(t, id.skeleton[s]), WithinAbs(expect, 1e-14));
        }
    }
}

TEST_CASE("interp_decomp: rank/accuracy tradeoff via rcond", "[hesap][interp][real]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(16U * 1024U * 1024U));
    constexpr crd::usize m = 24;
    constexpr crd::usize n = 20;
    // Hilbert-section A_ij = 1/(i+j+1): smooth geometric singular-value decay
    // across many orders of magnitude ⇒ a genuine rank/accuracy tradeoff.
    Mat<double> a(&alloc, m, n);
    for (crd::usize i = 0; i < m; ++i)
    {
        for (crd::usize j = 0; j < n; ++j)
        {
            a.at(i, j) = 1.0 / static_cast<double>(i + j + 1);
        }
    }
    const double anorm = frob<double>(a);

    // Tight tolerance ⇒ near-exact reconstruction.
    auto id_tight = interp_decomp<double, Layout::RowMajor>(&alloc, a, 1e-13);
    const double err_tight = recon_error<double>(a, id_tight);
    CHECK(err_tight < 1e-9 * anorm);

    // Coarse tolerance ⇒ smaller rank, still a controlled error.
    auto id_coarse = interp_decomp<double, Layout::RowMajor>(&alloc, a, 1e-4);
    const double err_coarse = recon_error<double>(a, id_coarse);
    CHECK(id_coarse.rank < id_tight.rank);
    CHECK(err_coarse < 1e-2 * anorm);  // loose: col-piv QR error ~ sqrt(...)·σ_{r+1}
    CHECK(err_coarse >= err_tight);    // coarser ⇒ no better than tight
}

TEST_CASE("interp_decomp: max_rank cap", "[hesap][interp][real]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(8U * 1024U * 1024U));
    Mat<double> a = make_low_rank<double>(&alloc, 10, 8, 5);
    auto id = interp_decomp<double, Layout::RowMajor>(&alloc, a, -1.0, 2);  // default rcond, cap rank 2
    REQUIRE(id.rank == 2);
    REQUIRE(id.skeleton.size() == 2);
    REQUIRE(id.cols.cols() == 2);
    REQUIRE(id.proj.rows() == 2);
    REQUIRE(id.proj.cols() == 8);
    // Still a valid (truncated) factorization — finite, bounded error.
    const double err = recon_error<double>(a, id);
    CHECK(std::isfinite(err));
}

TEST_CASE("interp_decomp: full column rank reconstructs exactly", "[hesap][interp][real]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(8U * 1024U * 1024U));
    constexpr crd::usize nn = 6;
    Mat<double> a(&alloc, nn, nn);
    // Diagonally dominant ⇒ full rank, well conditioned.
    for (crd::usize i = 0; i < nn; ++i)
    {
        for (crd::usize j = 0; j < nn; ++j)
        {
            a.at(i, j) = prand<double>(i, j, 0.3) + (i == j ? 10.0 : 0.0);
        }
    }
    auto id = interp_decomp<double, Layout::RowMajor>(&alloc, a);
    REQUIRE(id.rank == nn);
    CHECK(recon_error<double>(a, id) < 1e-11);
}

TEST_CASE("interp_decomp: f32 low-rank recovery", "[hesap][interp][real]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(8U * 1024U * 1024U));
    Mat<float> a = make_low_rank<float>(&alloc, 9, 7, 3);
    auto id = interp_decomp<float, Layout::RowMajor>(&alloc, a);
    REQUIRE(id.rank == 3);
    CHECK(recon_error<float>(a, id) < 1e-3F);
}

TEST_CASE("interp_decomp: reproducible (bit-identical re-run)", "[hesap][interp][real]")
{
    // Reproducibility, NOT the cross-thread moat (v5e-1a is RNG-free; the
    // counter-based-RNG moat lands in v5e-1b).
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(8U * 1024U * 1024U));
    Mat<double> a = make_low_rank<double>(&alloc, 12, 10, 4);
    auto id1 = interp_decomp<double, Layout::RowMajor>(&alloc, a);
    auto id2 = interp_decomp<double, Layout::RowMajor>(&alloc, a);
    REQUIRE(id1.rank == id2.rank);
    REQUIRE(id1.skeleton.size() == id2.skeleton.size());
    for (crd::usize s = 0; s < id1.skeleton.size(); ++s)
    {
        CHECK(id1.skeleton[s] == id2.skeleton[s]);
    }
    REQUIRE(id1.proj.size() == id2.proj.size());
    for (crd::usize i = 0; i < id1.proj.size(); ++i)
    {
        CHECK(id1.proj.data()[i] == id2.proj.data()[i]);  // bit-identical
    }
}

TEST_CASE("interp_decomp: tall and wide shapes", "[hesap][interp][real]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(8U * 1024U * 1024U));
    SECTION("tall m > n")
    {
        Mat<double> a = make_low_rank<double>(&alloc, 20, 6, 4);
        auto id = interp_decomp<double, Layout::RowMajor>(&alloc, a);
        REQUIRE(id.rank == 4);
        REQUIRE(id.rank <= 6);
        CHECK(recon_error<double>(a, id) < 1e-10);
    }
    SECTION("wide m < n")
    {
        Mat<double> a = make_low_rank<double>(&alloc, 6, 20, 4);
        auto id = interp_decomp<double, Layout::RowMajor>(&alloc, a);
        REQUIRE(id.rank == 4);
        REQUIRE(id.rank <= 6);
        CHECK(recon_error<double>(a, id) < 1e-10);
    }
}

TEST_CASE("interp_decomp: row ID via transpose", "[hesap][interp][real]")
{
    // Row ID of A == column ID of Aᵀ (the two-sided primitive HSS uses).
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(8U * 1024U * 1024U));
    constexpr crd::usize m = 11;
    constexpr crd::usize n = 8;
    Mat<double> a = make_low_rank<double>(&alloc, m, n, 3);
    Mat<double> at(&alloc, n, m);
    for (crd::usize i = 0; i < m; ++i)
    {
        for (crd::usize j = 0; j < n; ++j)
        {
            at.at(j, i) = a.at(i, j);
        }
    }
    auto id = interp_decomp<double, Layout::RowMajor>(&alloc, at);
    REQUIRE(id.rank == 3);
    // Aᵀ ≈ cols·proj ⇒ A ≈ projᵀ·colsᵀ (row ID of A). Verify the transpose form.
    CHECK(recon_error<double>(at, id) < 1e-10);
}

TEST_CASE("interp_decomp: zero matrix gives rank 0 (edge guard)", "[hesap][interp][real]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(1U * 1024U * 1024U));
    constexpr crd::usize m = 4;
    constexpr crd::usize n = 5;
    Mat<double> a(&alloc, m, n);  // all zeros
    auto id = interp_decomp<double, Layout::RowMajor>(&alloc, a);
    REQUIRE(id.rank == 0);
    REQUIRE(id.skeleton.size() == 0);
    REQUIRE(id.cols.cols() == 0);
    REQUIRE(id.proj.rows() == 0);
    REQUIRE(id.proj.cols() == n);
    CHECK(recon_error<double>(a, id) == 0.0);  // empty product = 0 = A
}

TEST_CASE("interp_decomp CLI: registered + reconstructs A", "[hesap][interp][cli]")
{
    using crd::hesap::cli::CommandArgs;
    using crd::hesap::cli::CommandRegistry;
    using crd::hesap::cli::ResultBinaryBlob;

    REQUIRE(CommandRegistry::global().find("hesap.dense.id.f32") != nullptr);
    const auto* rec = CommandRegistry::global().find("hesap.dense.id.f64");
    REQUIRE(rec != nullptr);

    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(4U * 1024U * 1024U));
    // Exactly-rank-2 4x4: rows/cols are integer combinations ⇒ ID picks rank 2.
    constexpr crd::usize m = 4;
    constexpr crd::usize n = 4;
    crd::f64 a_flat[m * n];
    for (crd::usize i = 0; i < m; ++i)
    {
        for (crd::usize j = 0; j < n; ++j)
        {
            // a_ij = u_i0*v_0j + u_i1*v_1j  (rank 2)
            const double u0 = static_cast<double>(i + 1);
            const double u1 = static_cast<double>((i % 2) + 1);
            const double v0 = static_cast<double>(j + 2);
            const double v1 = static_cast<double>((j % 3) + 1);
            a_flat[i * n + j] = u0 * v0 + u1 * v1;
        }
    }
    CommandArgs args{&alloc};
    args.set_u64("m", m);
    args.set_u64("n", n);
    args.set_f64_array("A", crd::containers::ConstSpan<crd::f64>{a_flat, m * n});
    const auto result = rec->impl(args);
    REQUIRE(result.ok);
    const auto* blob = std::get_if<ResultBinaryBlob>(&result.value);
    REQUIRE(blob != nullptr);
    const auto* d = reinterpret_cast<const crd::f64*>(blob->bytes.data());
    const crd::usize rank = static_cast<crd::usize>(d[0]);
    CHECK(rank == 2);
    REQUIRE(blob->bytes.size() == (1 + rank + rank * n) * sizeof(crd::f64));
    // Reconstruct A from the CLI output: cols = A[:, skeleton], proj follows.
    const crd::f64* skel = d + 1;
    const crd::f64* proj = d + 1 + rank;  // rank x n RowMajor
    double worst = 0.0;
    for (crd::usize i = 0; i < m; ++i)
    {
        for (crd::usize j = 0; j < n; ++j)
        {
            double s = 0.0;
            for (crd::usize r = 0; r < rank; ++r)
            {
                const crd::usize js = static_cast<crd::usize>(skel[r]);  // skeleton column
                s += a_flat[i * n + js] * proj[r * n + j];
            }
            worst = std::max(worst, std::abs(a_flat[i * n + j] - s));
        }
    }
    CHECK(worst < 1e-10);
}
