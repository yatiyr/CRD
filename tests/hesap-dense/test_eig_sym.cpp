#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <crd/containers/array.hpp>
#include <crd/hesap/cli/arg_value.hpp>
#include <crd/hesap/cli/command_registry.hpp>
#include <crd/hesap/dense/cli_anchor.hpp>
#include <crd/hesap/dense/detail/householder.hpp>
#include <crd/hesap/dense/eig_sym.hpp>
#include <crd/hesap/dense/matrix_types.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include "random_matrix.hpp"

#include <cmath>

using crd::hesap::dense::eig_sym;
using crd::hesap::dense::EigSym;
using crd::hesap::dense::Symmetric;
using crd_hesap_dense_tests::random_spd;
using crd_hesap_dense_tests::random_symmetric_indefinite;

namespace
{
// Force cli_register_eig.cpp's TU (static-init) to be linked in.
struct EigAnchorPull
{
    EigAnchorPull() noexcept { crd::hesap::dense::register_eig_cli_anchor(); }
};
const EigAnchorPull kEigAnchorPull;
} // namespace

namespace
{
// ||V^T V - I||_inf  (orthonormality of eigenvectors).
template <typename T>
double orthonormality_error(const EigSym<T>& eig, crd::usize n)
{
    const T* v = eig.vectors.data();
    const crd::usize ld = eig.vectors.ld();
    double worst = 0.0;
    for (crd::usize i = 0; i < n; ++i)
    {
        for (crd::usize j = 0; j < n; ++j)
        {
            T dot = T{0};
            for (crd::usize r = 0; r < n; ++r)
            {
                dot += v[r * ld + i] * v[r * ld + j];  // column i . column j
            }
            const double target = (i == j) ? 1.0 : 0.0;
            worst = std::max(worst, std::abs(static_cast<double>(dot) - target));
        }
    }
    return worst;
}

// max_k ||A v_k - lambda_k v_k||_inf  (eigenpair residual).
template <typename T>
double residual_error(const Symmetric<T>& a, const EigSym<T>& eig, crd::usize n)
{
    const T* v = eig.vectors.data();
    const crd::usize ld = eig.vectors.ld();
    double worst = 0.0;
    for (crd::usize k = 0; k < n; ++k)
    {
        const T lam = eig.values.data()[k];
        for (crd::usize i = 0; i < n; ++i)
        {
            T av = T{0};
            for (crd::usize j = 0; j < n; ++j)
            {
                av += a.at(i, j) * v[j * ld + k];
            }
            worst = std::max(worst, std::abs(static_cast<double>(av - lam * v[i * ld + k])));
        }
    }
    return worst;
}

template <typename T>
void fill_symmetric(Symmetric<T>& a, std::initializer_list<T> lower_rowmajor_full)
{
    // Expect a full n*n row-major list; only the lower triangle is read.
    const crd::usize n = a.n();
    const T* p = lower_rowmajor_full.begin();
    for (crd::usize i = 0; i < n; ++i)
    {
        for (crd::usize j = 0; j <= i; ++j)
        {
            a.at(i, j) = p[i * n + j];
        }
    }
}
} // namespace

TEST_CASE("make_householder: H*x = beta*e0", "[hesap][eig][householder]")
{
    namespace detail = crd::hesap::dense::detail;
    // Stack arrays (make_householder needs no allocator).
    double xs[5] = {3.0, 1.0, -2.0, 4.0, 0.5};
    double work[5];
    for (int i = 0; i < 5; ++i)
    {
        work[i] = xs[i];
    }
    const auto h = detail::make_householder<double>(work, 5);
    // Reconstruct v (v[0]=1, tail in work[1..4]) and apply H = I - tau v v^T.
    double v[5] = {1.0, work[1], work[2], work[3], work[4]};
    double vx = 0.0;
    for (int i = 0; i < 5; ++i)
    {
        vx += v[i] * xs[i];
    }
    double hx[5];
    for (int i = 0; i < 5; ++i)
    {
        hx[i] = xs[i] - h.tau * v[i] * vx;
    }
    CHECK(std::abs(hx[0] - h.beta) < 1e-12);
    for (int i = 1; i < 5; ++i)
    {
        CHECK(std::abs(hx[i]) < 1e-12);
    }
}

TEST_CASE("eig_sym: diagonal matrix recovers diagonal (ascending)", "[hesap][eig][real]")
{
    crd::memory::TlsfAllocator alloc(256U * 1024U);
    Symmetric<double> a(&alloc, 4);
    a.at(0, 0) = 4.0;
    a.at(1, 1) = 1.0;
    a.at(2, 2) = 3.0;
    a.at(3, 3) = 2.0;
    const auto eig = eig_sym<double>(&alloc, a);
    CHECK(std::abs(eig.values.data()[0] - 1.0) < 1e-12);
    CHECK(std::abs(eig.values.data()[1] - 2.0) < 1e-12);
    CHECK(std::abs(eig.values.data()[2] - 3.0) < 1e-12);
    CHECK(std::abs(eig.values.data()[3] - 4.0) < 1e-12);
    CHECK(orthonormality_error(eig, 4) < 1e-12);
}

TEST_CASE("eig_sym: 2x2 [[2,1],[1,2]] -> {1,3}", "[hesap][eig][real]")
{
    crd::memory::TlsfAllocator alloc(64U * 1024U);
    Symmetric<double> a(&alloc, 2);
    a.at(0, 0) = 2.0;
    a.at(1, 0) = 1.0;
    a.at(1, 1) = 2.0;
    const auto eig = eig_sym<double>(&alloc, a);
    CHECK(std::abs(eig.values.data()[0] - 1.0) < 1e-12);
    CHECK(std::abs(eig.values.data()[1] - 3.0) < 1e-12);
    CHECK(orthonormality_error(eig, 2) < 1e-12);
    CHECK(residual_error(a, eig, 2) < 1e-12);
}

TEST_CASE("eig_sym: 3x3 tridiagonal residual + orthogonality", "[hesap][eig][real]")
{
    crd::memory::TlsfAllocator alloc(256U * 1024U);
    Symmetric<double> a(&alloc, 3);
    fill_symmetric<double>(a, {2.0, 0.0, 0.0, 1.0, 2.0, 0.0, 0.0, 1.0, 2.0});
    const auto eig = eig_sym<double>(&alloc, a);
    CHECK(eig.values.data()[0] <= eig.values.data()[1]);
    CHECK(eig.values.data()[1] <= eig.values.data()[2]);
    CHECK(orthonormality_error(eig, 3) < 1e-12);
    CHECK(residual_error(a, eig, 3) < 1e-11);
}

TEST_CASE("eig_sym: SPD N=16/32/64/100/200 residual + orthogonality + positivity (blocked)",
          "[hesap][eig][real]")
{
    // N=100/200 exceed 2*kTridiagBlock (=64) so they exercise the blocked
    // dsytrd path (dlatrd panels + gemm_parallel trailing update).
    for (crd::usize n : {crd::usize{16}, crd::usize{32}, crd::usize{64}, crd::usize{100},
                         crd::usize{200}})
    {
        crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(64U * 1024U * 1024U));
        Symmetric<double> a(&alloc, n);
        random_spd<double>(a, 12345U + static_cast<crd::u32>(n));
        const auto eig = eig_sym<double>(&alloc, a);
        // SPD => all eigenvalues > 0, ascending.
        for (crd::usize k = 0; k < n; ++k)
        {
            CHECK(eig.values.data()[k] > 0.0);
            if (k > 0)
            {
                CHECK(eig.values.data()[k - 1] <= eig.values.data()[k] + 1e-9);
            }
        }
        CHECK(orthonormality_error(eig, n) < 1e-10);
        CHECK(residual_error(a, eig, n) < 1e-8);
    }
}

TEST_CASE("eig_sym: indefinite N=32 residual + orthogonality", "[hesap][eig][real]")
{
    const crd::usize n = 32;
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(4U * 1024U * 1024U));
    Symmetric<double> a(&alloc, n);
    random_symmetric_indefinite<double>(a, 777U);
    const auto eig = eig_sym<double>(&alloc, a);
    for (crd::usize k = 1; k < n; ++k)
    {
        CHECK(eig.values.data()[k - 1] <= eig.values.data()[k] + 1e-9);
    }
    CHECK(orthonormality_error(eig, n) < 1e-10);
    CHECK(residual_error(a, eig, n) < 1e-8);
}

TEST_CASE("eig_sym: deterministic (repeat run bit-identical)", "[hesap][eig][real]")
{
    const crd::usize n = 24;
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(4U * 1024U * 1024U));
    Symmetric<double> a(&alloc, n);
    random_symmetric_indefinite<double>(a, 4242U);
    const auto e1 = eig_sym<double>(&alloc, a);
    const auto e2 = eig_sym<double>(&alloc, a);
    for (crd::usize k = 0; k < n; ++k)
    {
        CHECK(e1.values.data()[k] == e2.values.data()[k]);
    }
    const crd::usize ld = e1.vectors.ld();
    for (crd::usize i = 0; i < n * ld; ++i)
    {
        CHECK(e1.vectors.data()[i] == e2.vectors.data()[i]);
    }
}

TEST_CASE("eig_sym: f32 SPD N=32 residual", "[hesap][eig][real]")
{
    const crd::usize n = 32;
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(2U * 1024U * 1024U));
    Symmetric<float> a(&alloc, n);
    random_spd<float>(a, 99U);
    const auto eig = eig_sym<float>(&alloc, a);
    for (crd::usize k = 0; k < n; ++k)
    {
        CHECK(eig.values.data()[k] > 0.0F);
    }
    CHECK(orthonormality_error(eig, n) < 1e-3);
    CHECK(residual_error(a, eig, n) < 1e-2);
}

TEST_CASE("eig_sym: general symmetric N=512 (D&C scale repro)", "[hesap][eig][real]")
{
    const crd::usize n = 512;
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(256U * 1024U * 1024U));
    Symmetric<double> a(&alloc, n);
    crd::u32 s = 1234567U + static_cast<crd::u32>(n);
    for (crd::usize i = 0; i < n; ++i)
    {
        for (crd::usize j = 0; j <= i; ++j)
        {
            s = s * 1664525U + 1013904223U;
            a.at(i, j) = static_cast<double>(static_cast<crd::i32>(s >> 8) % 2000 - 1000) * 0.001;
        }
    }
    const auto eig = eig_sym<double>(&alloc, a);
    // No NaNs.
    for (crd::usize k = 0; k < n; ++k)
    {
        CHECK(std::isfinite(eig.values.data()[k]));
    }
    CHECK(orthonormality_error(eig, n) < 1e-9);
    CHECK(residual_error(a, eig, n) < 1e-7);
}

TEST_CASE("eig_sym CLI: commands registered + correct eigenvalues", "[hesap][eig][cli]")
{
    using crd::hesap::cli::CommandArgs;
    using crd::hesap::cli::CommandRegistry;
    using crd::hesap::cli::ResultBinaryBlob;

    REQUIRE(CommandRegistry::global().find("hesap.dense.eig.sym.f32") != nullptr);
    const auto* rec = CommandRegistry::global().find("hesap.dense.eig.sym.f64");
    REQUIRE(rec != nullptr);

    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(1U * 1024U * 1024U));
    // 3x3 diagonal {4,1,3} flattened row-major (lower half used).
    const crd::f64 a_flat[] = {4.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 3.0};
    CommandArgs args{&alloc};
    args.set_u64("n", 3);
    args.set_f64_array("A", crd::containers::ConstSpan<crd::f64>{a_flat, 9});
    const auto result = rec->impl(args);
    REQUIRE(result.ok);
    const auto* blob = std::get_if<ResultBinaryBlob>(&result.value);
    REQUIRE(blob != nullptr);
    const auto* vals = reinterpret_cast<const crd::f64*>(blob->bytes.data());
    REQUIRE(blob->bytes.size() == 3 * sizeof(crd::f64));
    CHECK(std::abs(vals[0] - 1.0) < 1e-12);
    CHECK(std::abs(vals[1] - 3.0) < 1e-12);
    CHECK(std::abs(vals[2] - 4.0) < 1e-12);
}

TEST_CASE("eig_herm CLI: commands registered + correct eigenvalues", "[hesap][eig][herm][cli]")
{
    using crd::hesap::cli::CommandArgs;
    using crd::hesap::cli::CommandRegistry;
    using crd::hesap::cli::ResultBinaryBlob;

    REQUIRE(CommandRegistry::global().find("hesap.dense.eig.herm.c32") != nullptr);
    const auto* rec = CommandRegistry::global().find("hesap.dense.eig.herm.c64");
    REQUIRE(rec != nullptr);

    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(1U * 1024U * 1024U));
    // Hermitian [[2, 1+i], [1-i, 3]] (eigenvalues {1, 4}); A as interleaved
    // [re,im] of n*n entries, lower triangle used: (0,0)=2, (1,0)=1-i, (1,1)=3.
    const crd::f64 a_flat[] = {2.0, 0.0, /*(0,1) upper, ignored*/ 0.0, 0.0,
                               1.0, -1.0, 3.0, 0.0};
    CommandArgs args{&alloc};
    args.set_u64("n", 2);
    args.set_f64_array("A", crd::containers::ConstSpan<crd::f64>{a_flat, 8});
    const auto result = rec->impl(args);
    REQUIRE(result.ok);
    const auto* blob = std::get_if<ResultBinaryBlob>(&result.value);
    REQUIRE(blob != nullptr);
    REQUIRE(blob->bytes.size() == 2 * sizeof(crd::f64));
    const auto* vals = reinterpret_cast<const crd::f64*>(blob->bytes.data());
    CHECK(std::abs(vals[0] - 1.0) < 1e-12);
    CHECK(std::abs(vals[1] - 4.0) < 1e-12);
}
