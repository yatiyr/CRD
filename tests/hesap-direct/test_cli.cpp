// crd-hesap-direct v5a-2 — CLI registration + invocation tests for the
// supernodal Cholesky (hesap.direct.chol.{f32,f64,c32,c64}). Pulls the module
// anchor so the static-init registration block survives the static-lib link,
// then drives a real SPD solve and a complex Hermitian solve through the
// registry's CommandArgs/CommandResult wire shape.

#include <crd/hesap/cli/arg_value.hpp>
#include <crd/hesap/cli/command_registry.hpp>
#include <crd/hesap/cli/command_result.hpp>
#include <crd/hesap/complex.hpp>
#include <crd/hesap/direct/cli_anchor.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <variant>

namespace cli = crd::hesap::cli;
using C = crd::hesap::Complex64;

namespace
{
// Force the linker to keep cli_register_direct.cpp's static-init block.
const bool kPullDirect = (crd::hesap::direct::register_direct_cli_anchor(), true);

const cli::ResultBinaryBlob* as_blob(const cli::CommandResult& r)
{
    return std::get_if<cli::ResultBinaryBlob>(&r.value);
}
} // namespace

TEST_CASE("CLI: all four hesap.direct.chol type variants are registered", "[hesap][direct][v5a-2][cli]")
{
    REQUIRE(kPullDirect);
    auto& reg = cli::CommandRegistry::global();
    for (const char* name :
         {"hesap.direct.chol.f32", "hesap.direct.chol.f64", "hesap.direct.chol.c32", "hesap.direct.chol.c64"})
    {
        const auto* rec = reg.find(name);
        REQUIRE(rec != nullptr);
        REQUIRE(rec->impl != nullptr);
    }
}

TEST_CASE("CLI: hesap.direct.chol.f64 solves a real SPD system", "[hesap][direct][v5a-2][cli]")
{
    REQUIRE(kPullDirect);
    crd::memory::TlsfAllocator alloc(8 << 20);
    const crd::u32 n = 5;

    // Full SPD matrix A[i][i]=6, A[i][j]=1 (diagonally dominant) → all-pairs triplets.
    crd::containers::Array<crd::i64> tr(&alloc);
    crd::containers::Array<crd::i64> tc(&alloc);
    crd::containers::Array<crd::f64> vals(&alloc);
    auto aij = [](crd::u32 i, crd::u32 j)
    {
        return i == j ? static_cast<crd::f64>(n + 2) : 1.0;
    };
    for (crd::u32 i = 0; i < n; ++i)
    {
        for (crd::u32 j = 0; j < n; ++j)
        {
            tr.push_back(static_cast<crd::i64>(i));
            tc.push_back(static_cast<crd::i64>(j));
            vals.push_back(aij(i, j));
        }
    }
    crd::containers::Array<crd::f64> xtrue(&alloc);
    crd::containers::Array<crd::f64> b(&alloc);
    xtrue.resize(n);
    b.resize(n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        xtrue[i] = 1.0 + static_cast<crd::f64>(i);
    }
    for (crd::u32 i = 0; i < n; ++i)
    {
        crd::f64 acc = 0.0;
        for (crd::u32 j = 0; j < n; ++j)
        {
            acc += aij(i, j) * xtrue[j];
        }
        b[i] = acc;
    }

    cli::CommandArgs args{&alloc};
    args.set_u64("rows", n);
    args.set_u64("cols", n);
    args.set_i64_array("triplet_rows", {tr.data(), tr.size()});
    args.set_i64_array("triplet_cols", {tc.data(), tc.size()});
    args.set_f64_array("values", {vals.data(), vals.size()});
    args.set_f64_array("b", {b.data(), b.size()});

    const auto* rec = cli::CommandRegistry::global().find("hesap.direct.chol.f64");
    REQUIRE(rec != nullptr);
    const cli::CommandResult r = rec->impl(args);
    REQUIRE(r.ok);
    const auto* blob = as_blob(r);
    REQUIRE(blob != nullptr);
    REQUIRE(blob->bytes.size() == (static_cast<crd::usize>(n) + 1) * sizeof(crd::f64));
    const auto* out = reinterpret_cast<const crd::f64*>(blob->bytes.data());
    CHECK(out[0] == 0.0); // info: SPD ok
    for (crd::u32 i = 0; i < n; ++i)
    {
        CHECK(std::abs(out[1 + i] - xtrue[i]) < 1e-9);
    }
}

TEST_CASE("CLI: hesap.direct.chol.c64 solves a complex Hermitian system", "[hesap][direct][v5a-2][cli][complex]")
{
    REQUIRE(kPullDirect);
    crd::memory::TlsfAllocator alloc(8 << 20);
    const crd::u32 n = 4;

    // Full Hermitian HPD: diag real-dominant, A[i][j]=conj(A[j][i]).
    crd::containers::Array<crd::i64> tr(&alloc);
    crd::containers::Array<crd::i64> tc(&alloc);
    crd::containers::Array<crd::f64> vals(&alloc); // {re,im} interleaved
    auto push = [&](crd::u32 i, crd::u32 j, C v)
    {
        tr.push_back(static_cast<crd::i64>(i));
        tc.push_back(static_cast<crd::i64>(j));
        vals.push_back(v.re);
        vals.push_back(v.im);
    };
    crd::containers::Array<C> dense(&alloc);
    dense.resize(static_cast<crd::usize>(n) * n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        for (crd::u32 j = 0; j < n; ++j)
        {
            C v;
            if (i == j)
            {
                v = C{static_cast<crd::f64>(2 * n), 0.0};
            }
            else if (i < j)
            {
                v = C{0.4, 0.25};
            }
            else
            {
                v = C{0.4, -0.25}; // conj of the (j,i) upper entry
            }
            dense[i * n + j] = v;
            push(i, j, v);
        }
    }
    crd::containers::Array<C> xtrue(&alloc);
    crd::containers::Array<crd::f64> b(&alloc); // {re,im} interleaved
    xtrue.resize(n);
    b.resize(static_cast<crd::usize>(n) * 2);
    for (crd::u32 i = 0; i < n; ++i)
    {
        xtrue[i] = C{1.0 + 0.3 * static_cast<crd::f64>(i), -0.2 * static_cast<crd::f64>(i)};
    }
    for (crd::u32 i = 0; i < n; ++i)
    {
        C acc{0.0, 0.0};
        for (crd::u32 j = 0; j < n; ++j)
        {
            acc += dense[i * n + j] * xtrue[j];
        }
        b[2 * i] = acc.re;
        b[2 * i + 1] = acc.im;
    }

    cli::CommandArgs args{&alloc};
    args.set_u64("rows", n);
    args.set_u64("cols", n);
    args.set_i64_array("triplet_rows", {tr.data(), tr.size()});
    args.set_i64_array("triplet_cols", {tc.data(), tc.size()});
    args.set_f64_array("values", {vals.data(), vals.size()});
    args.set_f64_array("b", {b.data(), b.size()});

    const auto* rec = cli::CommandRegistry::global().find("hesap.direct.chol.c64");
    REQUIRE(rec != nullptr);
    const cli::CommandResult r = rec->impl(args);
    REQUIRE(r.ok);
    const auto* blob = as_blob(r);
    REQUIRE(blob != nullptr);
    REQUIRE(blob->bytes.size() == (2 * static_cast<crd::usize>(n) + 1) * sizeof(crd::f64));
    const auto* out = reinterpret_cast<const crd::f64*>(blob->bytes.data());
    CHECK(out[0] == 0.0); // info: HPD ok
    for (crd::u32 i = 0; i < n; ++i)
    {
        CHECK(std::abs(out[1 + 2 * i] - xtrue[i].re) < 1e-9);
        CHECK(std::abs(out[1 + 2 * i + 1] - xtrue[i].im) < 1e-9);
    }
}

TEST_CASE("CLI: all four hesap.direct.lu_gp type variants are registered", "[hesap][direct][v5b-1][cli]")
{
    REQUIRE(kPullDirect);
    auto& reg = cli::CommandRegistry::global();
    for (const char* name :
         {"hesap.direct.lu_gp.f32", "hesap.direct.lu_gp.f64", "hesap.direct.lu_gp.c32", "hesap.direct.lu_gp.c64"})
    {
        const auto* rec = reg.find(name);
        REQUIRE(rec != nullptr);
        REQUIRE(rec->impl != nullptr);
    }
}

TEST_CASE("CLI: hesap.direct.lu_gp.f64 solves a general unsymmetric system", "[hesap][direct][v5b-1][cli]")
{
    REQUIRE(kPullDirect);
    crd::memory::TlsfAllocator alloc(8 << 20);
    const crd::u32 n = 6;
    // General UNSYMMETRIC, diagonally dominant: diag n+2, super +1, sub -2 (super != sub).
    auto aij = [](crd::u32 i, crd::u32 j) -> crd::f64 // n is a constant expression — no capture
    {
        if (i == j)
            return static_cast<crd::f64>(n + 2);
        if (j == i + 1)
            return 1.0;
        if (i == j + 1)
            return -2.0;
        return 0.0;
    };
    crd::containers::Array<crd::i64> tr(&alloc);
    crd::containers::Array<crd::i64> tc(&alloc);
    crd::containers::Array<crd::f64> vals(&alloc);
    for (crd::u32 i = 0; i < n; ++i)
    {
        for (crd::u32 j = 0; j < n; ++j)
        {
            const crd::f64 v = aij(i, j);
            if (v != 0.0)
            {
                tr.push_back(static_cast<crd::i64>(i));
                tc.push_back(static_cast<crd::i64>(j));
                vals.push_back(v);
            }
        }
    }
    crd::containers::Array<crd::f64> xtrue(&alloc);
    crd::containers::Array<crd::f64> b(&alloc);
    xtrue.resize(n);
    b.resize(n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        xtrue[i] = 1.0 + static_cast<crd::f64>(i);
    }
    for (crd::u32 i = 0; i < n; ++i)
    {
        crd::f64 acc = 0.0;
        for (crd::u32 j = 0; j < n; ++j)
        {
            acc += aij(i, j) * xtrue[j];
        }
        b[i] = acc;
    }

    cli::CommandArgs args{&alloc};
    args.set_u64("rows", n);
    args.set_u64("cols", n);
    args.set_i64_array("triplet_rows", {tr.data(), tr.size()});
    args.set_i64_array("triplet_cols", {tc.data(), tc.size()});
    args.set_f64_array("values", {vals.data(), vals.size()});
    args.set_f64_array("b", {b.data(), b.size()});

    const auto* rec = cli::CommandRegistry::global().find("hesap.direct.lu_gp.f64");
    REQUIRE(rec != nullptr);
    const cli::CommandResult r = rec->impl(args);
    REQUIRE(r.ok);
    const auto* blob = as_blob(r);
    REQUIRE(blob != nullptr);
    REQUIRE(blob->bytes.size() == (static_cast<crd::usize>(n) + 1) * sizeof(crd::f64));
    const auto* out = reinterpret_cast<const crd::f64*>(blob->bytes.data());
    CHECK(out[0] == 0.0); // info: nonsingular
    for (crd::u32 i = 0; i < n; ++i)
    {
        CHECK(std::abs(out[1 + i] - xtrue[i]) < 1e-9);
    }
}

TEST_CASE("CLI: all four hesap.direct.lu type variants are registered", "[hesap][direct][v5b-3][cli]")
{
    REQUIRE(kPullDirect);
    auto& reg = cli::CommandRegistry::global();
    for (const char* name :
         {"hesap.direct.lu.f32", "hesap.direct.lu.f64", "hesap.direct.lu.c32", "hesap.direct.lu.c64"})
    {
        const auto* rec = reg.find(name);
        REQUIRE(rec != nullptr);
        REQUIRE(rec->impl != nullptr);
    }
}

TEST_CASE("CLI: hesap.direct.lu.f64 solves a general unsymmetric system (multifrontal dispatch)",
          "[hesap][direct][v5b-3][cli]")
{
    REQUIRE(kPullDirect);
    crd::memory::TlsfAllocator alloc(16 << 20);
    const crd::u32 n = 8;
    // General UNSYMMETRIC, diagonally dominant: diag n+3, super +1, sub -2, far-super +0.5 (super != sub).
    auto aij = [](crd::u32 i, crd::u32 j) -> crd::f64
    {
        if (i == j)
            return static_cast<crd::f64>(n + 3);
        if (j == i + 1)
            return 1.0;
        if (i == j + 1)
            return -2.0;
        if (j == i + 2)
            return 0.5;
        return 0.0;
    };
    crd::containers::Array<crd::i64> tr(&alloc);
    crd::containers::Array<crd::i64> tc(&alloc);
    crd::containers::Array<crd::f64> vals(&alloc);
    for (crd::u32 i = 0; i < n; ++i)
    {
        for (crd::u32 j = 0; j < n; ++j)
        {
            const crd::f64 v = aij(i, j);
            if (v != 0.0)
            {
                tr.push_back(static_cast<crd::i64>(i));
                tc.push_back(static_cast<crd::i64>(j));
                vals.push_back(v);
            }
        }
    }
    crd::containers::Array<crd::f64> xtrue(&alloc);
    crd::containers::Array<crd::f64> b(&alloc);
    xtrue.resize(n);
    b.resize(n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        xtrue[i] = 1.0 + static_cast<crd::f64>(i);
    }
    for (crd::u32 i = 0; i < n; ++i)
    {
        crd::f64 acc = 0.0;
        for (crd::u32 j = 0; j < n; ++j)
        {
            acc += aij(i, j) * xtrue[j];
        }
        b[i] = acc;
    }

    cli::CommandArgs args{&alloc};
    args.set_u64("rows", n);
    args.set_u64("cols", n);
    args.set_i64_array("triplet_rows", {tr.data(), tr.size()});
    args.set_i64_array("triplet_cols", {tc.data(), tc.size()});
    args.set_f64_array("values", {vals.data(), vals.size()});
    args.set_f64_array("b", {b.data(), b.size()});

    const auto* rec = cli::CommandRegistry::global().find("hesap.direct.lu.f64");
    REQUIRE(rec != nullptr);
    const cli::CommandResult r = rec->impl(args);
    REQUIRE(r.ok);
    const auto* blob = as_blob(r);
    REQUIRE(blob != nullptr);
    REQUIRE(blob->bytes.size() == (static_cast<crd::usize>(n) + 1) * sizeof(crd::f64));
    const auto* out = reinterpret_cast<const crd::f64*>(blob->bytes.data());
    CHECK(out[0] == 0.0); // info: nonsingular
    for (crd::u32 i = 0; i < n; ++i)
    {
        CHECK(std::abs(out[1 + i] - xtrue[i]) < 1e-9);
    }
}

TEST_CASE("CLI: hesap.direct.lu.c64 solves a complex unsymmetric system", "[hesap][direct][v5b-3][cli][complex]")
{
    REQUIRE(kPullDirect);
    crd::memory::TlsfAllocator alloc(16 << 20);
    const crd::u32 n = 6;
    // Complex unsymmetric, diagonally dominant. aij returns {re,im}; super != sub (unsymmetric).
    auto aij = [](crd::u32 i, crd::u32 j) -> crd::hesap::Complex<crd::f64>
    {
        if (i == j)
            return {static_cast<crd::f64>(n + 4), 1.0};
        if (j == i + 1)
            return {1.0, 0.5};
        if (i == j + 1)
            return {-2.0, 0.25};
        return {0.0, 0.0};
    };
    crd::containers::Array<crd::i64> tr(&alloc);
    crd::containers::Array<crd::i64> tc(&alloc);
    crd::containers::Array<crd::f64> vals(&alloc); // flattened {re,im,...}
    for (crd::u32 i = 0; i < n; ++i)
    {
        for (crd::u32 j = 0; j < n; ++j)
        {
            const auto v = aij(i, j);
            if (v.re != 0.0 || v.im != 0.0)
            {
                tr.push_back(static_cast<crd::i64>(i));
                tc.push_back(static_cast<crd::i64>(j));
                vals.push_back(v.re);
                vals.push_back(v.im);
            }
        }
    }
    crd::containers::Array<crd::hesap::Complex<crd::f64>> xtrue(&alloc);
    crd::containers::Array<crd::f64> b(&alloc); // flattened {re,im,...}
    xtrue.resize(n);
    b.resize(static_cast<crd::usize>(n) * 2);
    for (crd::u32 i = 0; i < n; ++i)
    {
        xtrue[i] = {1.0 + static_cast<crd::f64>(i), 0.5 * static_cast<crd::f64>(i)};
    }
    for (crd::u32 i = 0; i < n; ++i)
    {
        crd::hesap::Complex<crd::f64> acc{0.0, 0.0};
        for (crd::u32 j = 0; j < n; ++j)
        {
            acc = acc + aij(i, j) * xtrue[j];
        }
        b[2 * i] = acc.re;
        b[2 * i + 1] = acc.im;
    }

    cli::CommandArgs args{&alloc};
    args.set_u64("rows", n);
    args.set_u64("cols", n);
    args.set_i64_array("triplet_rows", {tr.data(), tr.size()});
    args.set_i64_array("triplet_cols", {tc.data(), tc.size()});
    args.set_f64_array("values", {vals.data(), vals.size()});
    args.set_f64_array("b", {b.data(), b.size()});

    const auto* rec = cli::CommandRegistry::global().find("hesap.direct.lu.c64");
    REQUIRE(rec != nullptr);
    const cli::CommandResult r = rec->impl(args);
    REQUIRE(r.ok);
    const auto* blob = as_blob(r);
    REQUIRE(blob != nullptr);
    REQUIRE(blob->bytes.size() == (static_cast<crd::usize>(n) * 2 + 1) * sizeof(crd::f64));
    const auto* out = reinterpret_cast<const crd::f64*>(blob->bytes.data());
    CHECK(out[0] == 0.0); // info: nonsingular
    for (crd::u32 i = 0; i < n; ++i)
    {
        CHECK(std::abs(out[1 + 2 * i] - xtrue[i].re) < 1e-9);
        CHECK(std::abs(out[1 + 2 * i + 1] - xtrue[i].im) < 1e-9);
    }
}

// v5c-2c — multifrontal QR CLI: hesap.direct.qr.{f32,f64,c32,c64}. Output is [info, rank, x...] (QR is
// rank-revealing). Dispatcher: square (m==n) → solve, over-determined (m>n) → least_squares.
TEST_CASE("CLI: all four hesap.direct.qr type variants are registered", "[hesap][direct][v5c-2c][cli]")
{
    REQUIRE(kPullDirect);
    auto& reg = cli::CommandRegistry::global();
    for (const char* name :
         {"hesap.direct.qr.f32", "hesap.direct.qr.f64", "hesap.direct.qr.c32", "hesap.direct.qr.c64"})
    {
        const auto* rec = reg.find(name);
        REQUIRE(rec != nullptr);
        REQUIRE(rec->impl != nullptr);
    }
}

TEST_CASE("CLI: hesap.direct.qr.f64 solves a square system (m==n dispatch)", "[hesap][direct][v5c-2c][cli]")
{
    REQUIRE(kPullDirect);
    crd::memory::TlsfAllocator alloc(8 << 20);
    const crd::u32 n = 5;
    auto aij = [](crd::u32 i, crd::u32 j) -> crd::f64
    {
        if (i == j)
        {
            return static_cast<crd::f64>(n + 2);
        }
        if (j == i + 1)
        {
            return 1.0;
        }
        if (i == j + 1)
        {
            return -1.0;
        }
        return 0.0;
    };
    crd::containers::Array<crd::i64> tr(&alloc);
    crd::containers::Array<crd::i64> tc(&alloc);
    crd::containers::Array<crd::f64> vals(&alloc);
    for (crd::u32 i = 0; i < n; ++i)
    {
        for (crd::u32 j = 0; j < n; ++j)
        {
            const crd::f64 v = aij(i, j);
            if (v != 0.0)
            {
                tr.push_back(static_cast<crd::i64>(i));
                tc.push_back(static_cast<crd::i64>(j));
                vals.push_back(v);
            }
        }
    }
    crd::containers::Array<crd::f64> xtrue(&alloc);
    crd::containers::Array<crd::f64> b(&alloc);
    xtrue.resize(n);
    b.resize(n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        xtrue[i] = 1.0 + static_cast<crd::f64>(i);
    }
    for (crd::u32 i = 0; i < n; ++i)
    {
        crd::f64 acc = 0.0;
        for (crd::u32 j = 0; j < n; ++j)
        {
            acc += aij(i, j) * xtrue[j];
        }
        b[i] = acc;
    }
    cli::CommandArgs args{&alloc};
    args.set_u64("rows", n);
    args.set_u64("cols", n);
    args.set_i64_array("triplet_rows", {tr.data(), tr.size()});
    args.set_i64_array("triplet_cols", {tc.data(), tc.size()});
    args.set_f64_array("values", {vals.data(), vals.size()});
    args.set_f64_array("b", {b.data(), b.size()});

    const auto* rec = cli::CommandRegistry::global().find("hesap.direct.qr.f64");
    REQUIRE(rec != nullptr);
    const cli::CommandResult r = rec->impl(args);
    REQUIRE(r.ok);
    const auto* blob = as_blob(r);
    REQUIRE(blob != nullptr);
    REQUIRE(blob->bytes.size() == (static_cast<crd::usize>(n) + 2) * sizeof(crd::f64)); // [info, rank, x...]
    const auto* out = reinterpret_cast<const crd::f64*>(blob->bytes.data());
    CHECK(out[0] == 0.0);                      // info
    CHECK(out[1] == static_cast<crd::f64>(n)); // full rank
    for (crd::u32 i = 0; i < n; ++i)
    {
        CHECK(std::abs(out[2 + i] - xtrue[i]) < 1e-9);
    }
}

TEST_CASE("CLI: hesap.direct.qr.f64 over-determined least squares (m>n dispatch)", "[hesap][direct][v5c-2c][cli]")
{
    REQUIRE(kPullDirect);
    crd::memory::TlsfAllocator alloc(8 << 20);
    const crd::u32 m = 12;
    const crd::u32 n = 8;
    // banded m×n, full column rank: column k touches rows k, k+1, k+4 (clamped) with distinct values.
    crd::containers::Array<crd::i64> tr(&alloc);
    crd::containers::Array<crd::i64> tc(&alloc);
    crd::containers::Array<crd::f64> vals(&alloc);
    auto push = [&](crd::u32 i, crd::u32 j, crd::f64 v)
    {
        tr.push_back(static_cast<crd::i64>(i));
        tc.push_back(static_cast<crd::i64>(j));
        vals.push_back(v);
    };
    for (crd::u32 k = 0; k < n; ++k)
    {
        const crd::u32 rs[3] = {k, k + 1, k + 4};
        const crd::f64 vs[3] = {5.0, -1.0, -1.0};
        for (crd::u32 t = 0; t < 3; ++t)
        {
            if (rs[t] < m)
            {
                push(rs[t], k, vs[t]);
            }
        }
    }
    // x_true over n columns; b = A·x_true (length m, consistent ⇒ least-squares recovers x_true).
    crd::containers::Array<crd::f64> xtrue(&alloc);
    crd::containers::Array<crd::f64> b(&alloc);
    xtrue.resize(n);
    b.resize(m);
    for (crd::u32 j = 0; j < n; ++j)
    {
        xtrue[j] = 1.0 + 0.5 * static_cast<crd::f64>(j);
    }
    for (crd::u32 i = 0; i < m; ++i)
    {
        b[i] = 0.0;
    }
    for (crd::usize k = 0; k < tr.size(); ++k)
    {
        b[static_cast<crd::u32>(tr[k])] += vals[k] * xtrue[static_cast<crd::u32>(tc[k])];
    }
    cli::CommandArgs args{&alloc};
    args.set_u64("rows", m);
    args.set_u64("cols", n);
    args.set_i64_array("triplet_rows", {tr.data(), tr.size()});
    args.set_i64_array("triplet_cols", {tc.data(), tc.size()});
    args.set_f64_array("values", {vals.data(), vals.size()});
    args.set_f64_array("b", {b.data(), b.size()});

    const auto* rec = cli::CommandRegistry::global().find("hesap.direct.qr.f64");
    REQUIRE(rec != nullptr);
    const cli::CommandResult r = rec->impl(args);
    REQUIRE(r.ok);
    const auto* blob = as_blob(r);
    REQUIRE(blob != nullptr);
    REQUIRE(blob->bytes.size() == (static_cast<crd::usize>(n) + 2) * sizeof(crd::f64));
    const auto* out = reinterpret_cast<const crd::f64*>(blob->bytes.data());
    CHECK(out[0] == 0.0);
    CHECK(out[1] == static_cast<crd::f64>(n)); // full column rank
    for (crd::u32 j = 0; j < n; ++j)
    {
        CHECK(std::abs(out[2 + j] - xtrue[j]) < 1e-9);
    }
}

TEST_CASE("CLI: hesap.direct.qr.c64 solves a complex square system", "[hesap][direct][v5c-2c][cli][complex]")
{
    REQUIRE(kPullDirect);
    crd::memory::TlsfAllocator alloc(8 << 20);
    const crd::u32 n = 4;
    auto aij = [](crd::u32 i, crd::u32 j) -> C
    {
        if (i == j)
            return C{static_cast<crd::f64>(n + 3), 0.5};
        if (j == i + 1)
            return C{1.0, 0.3};
        if (i == j + 1)
            return C{-1.0, 0.2};
        return C{0.0, 0.0};
    };
    crd::containers::Array<crd::i64> tr(&alloc);
    crd::containers::Array<crd::i64> tc(&alloc);
    crd::containers::Array<crd::f64> vals(&alloc); // flattened {re,im,...}
    for (crd::u32 i = 0; i < n; ++i)
    {
        for (crd::u32 j = 0; j < n; ++j)
        {
            const C v = aij(i, j);
            if (v.re != 0.0 || v.im != 0.0)
            {
                tr.push_back(static_cast<crd::i64>(i));
                tc.push_back(static_cast<crd::i64>(j));
                vals.push_back(v.re);
                vals.push_back(v.im);
            }
        }
    }
    crd::containers::Array<C> xtrue(&alloc);
    crd::containers::Array<crd::f64> b(&alloc); // flattened {re,im,...}
    xtrue.resize(n);
    b.resize(static_cast<crd::usize>(n) * 2);
    for (crd::u32 i = 0; i < n; ++i)
    {
        xtrue[i] = C{1.0 + static_cast<crd::f64>(i), -0.4 * static_cast<crd::f64>(i)};
    }
    for (crd::u32 i = 0; i < n; ++i)
    {
        C acc{0.0, 0.0};
        for (crd::u32 j = 0; j < n; ++j)
        {
            acc = acc + aij(i, j) * xtrue[j];
        }
        b[2 * i] = acc.re;
        b[2 * i + 1] = acc.im;
    }
    cli::CommandArgs args{&alloc};
    args.set_u64("rows", n);
    args.set_u64("cols", n);
    args.set_i64_array("triplet_rows", {tr.data(), tr.size()});
    args.set_i64_array("triplet_cols", {tc.data(), tc.size()});
    args.set_f64_array("values", {vals.data(), vals.size()});
    args.set_f64_array("b", {b.data(), b.size()});

    const auto* rec = cli::CommandRegistry::global().find("hesap.direct.qr.c64");
    REQUIRE(rec != nullptr);
    const cli::CommandResult r = rec->impl(args);
    REQUIRE(r.ok);
    const auto* blob = as_blob(r);
    REQUIRE(blob != nullptr);
    REQUIRE(blob->bytes.size() == (2 * static_cast<crd::usize>(n) + 2) * sizeof(crd::f64));
    const auto* out = reinterpret_cast<const crd::f64*>(blob->bytes.data());
    CHECK(out[0] == 0.0);
    CHECK(out[1] == static_cast<crd::f64>(n));
    for (crd::u32 i = 0; i < n; ++i)
    {
        CHECK(std::abs(out[2 + 2 * i] - xtrue[i].re) < 1e-9);
        CHECK(std::abs(out[2 + 2 * i + 1] - xtrue[i].im) < 1e-9);
    }
}

TEST_CASE("CLI: all six hesap.direct.ldlt/ldlh commands are registered", "[hesap][direct][v5d-g][cli]")
{
    REQUIRE(kPullDirect);
    auto& reg = cli::CommandRegistry::global();
    for (const char* name : {"hesap.direct.ldlt.f32", "hesap.direct.ldlt.f64", "hesap.direct.ldlt.c32",
                             "hesap.direct.ldlt.c64", "hesap.direct.ldlh.c32", "hesap.direct.ldlh.c64"})
    {
        const auto* rec = reg.find(name);
        REQUIRE(rec != nullptr);
        REQUIRE(rec->impl != nullptr);
    }
}

TEST_CASE("CLI: hesap.direct.ldlt.f64 solves a symmetric indefinite system", "[hesap][direct][v5d-g][cli]")
{
    REQUIRE(kPullDirect);
    crd::memory::TlsfAllocator alloc(8 << 20);
    const crd::u32 n = 4;
    // Dense symmetric INDEFINITE (negative diagonals) — Bunch-Kaufman handles it where Cholesky can't.
    const crd::f64 diag[4] = {2.0, -3.0, 4.0, -2.0};
    auto aij = [&](crd::u32 i, crd::u32 j) -> crd::f64 { return (i == j) ? diag[i] : 1.0; };
    crd::containers::Array<crd::i64> tr(&alloc);
    crd::containers::Array<crd::i64> tc(&alloc);
    crd::containers::Array<crd::f64> vals(&alloc);
    for (crd::u32 i = 0; i < n; ++i)
    {
        for (crd::u32 j = 0; j < n; ++j)
        {
            tr.push_back(static_cast<crd::i64>(i));
            tc.push_back(static_cast<crd::i64>(j));
            vals.push_back(aij(i, j));
        }
    }
    crd::containers::Array<crd::f64> xtrue(&alloc);
    crd::containers::Array<crd::f64> b(&alloc);
    xtrue.resize(n);
    b.resize(n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        xtrue[i] = 1.0 + static_cast<crd::f64>(i);
    }
    for (crd::u32 i = 0; i < n; ++i)
    {
        crd::f64 acc = 0.0;
        for (crd::u32 j = 0; j < n; ++j)
        {
            acc += aij(i, j) * xtrue[j];
        }
        b[i] = acc;
    }
    cli::CommandArgs args{&alloc};
    args.set_u64("rows", n);
    args.set_u64("cols", n);
    args.set_i64_array("triplet_rows", {tr.data(), tr.size()});
    args.set_i64_array("triplet_cols", {tc.data(), tc.size()});
    args.set_f64_array("values", {vals.data(), vals.size()});
    args.set_f64_array("b", {b.data(), b.size()});

    const auto* rec = cli::CommandRegistry::global().find("hesap.direct.ldlt.f64");
    REQUIRE(rec != nullptr);
    const cli::CommandResult r = rec->impl(args);
    REQUIRE(r.ok);
    const auto* blob = as_blob(r);
    REQUIRE(blob != nullptr);
    REQUIRE(blob->bytes.size() == (static_cast<crd::usize>(n) + 1) * sizeof(crd::f64)); // [info, x...]
    const auto* out = reinterpret_cast<const crd::f64*>(blob->bytes.data());
    CHECK(out[0] == 0.0); // info
    for (crd::u32 i = 0; i < n; ++i)
    {
        CHECK(std::abs(out[1 + i] - xtrue[i]) < 1e-9);
    }
}

TEST_CASE("CLI: hesap.direct.ldlh.c64 solves a Hermitian system; ldlt.c64 (symmetric) differs",
          "[hesap][direct][v5d-g][cli]")
{
    REQUIRE(kPullDirect);
    crd::memory::TlsfAllocator alloc(8 << 20);
    const crd::u32 n = 3;
    // A 3×3 HERMITIAN indefinite matrix (real diagonal; zero leading diagonals ⇒ a 2×2; nonzero imaginary).
    crd::containers::Array<C> full(&alloc);
    full.resize(static_cast<crd::usize>(n) * n);
    for (auto& z : full)
    {
        z = C{0, 0};
    }
    auto seth = [&](crd::u32 i, crd::u32 j, C v)
    {
        full[static_cast<crd::usize>(i) * n + j] = v;
        full[static_cast<crd::usize>(j) * n + i] = crd::hesap::conj(v);
    };
    full[static_cast<crd::usize>(2) * n + 2] = C{5, 0}; // real diagonal (others 0)
    seth(0, 1, C{2, 1});
    seth(0, 2, C{1, 0.5});
    seth(1, 2, C{1, -0.25});

    crd::containers::Array<C> xtrue(&alloc);
    xtrue.resize(n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        xtrue[i] = C{1.0 + 0.3 * static_cast<crd::f64>(i), 0.5 - 0.2 * static_cast<crd::f64>(i)};
    }
    crd::containers::Array<crd::i64> tr(&alloc);
    crd::containers::Array<crd::i64> tc(&alloc);
    crd::containers::Array<crd::f64> vals(&alloc);
    for (crd::u32 i = 0; i < n; ++i)
    {
        for (crd::u32 j = 0; j < n; ++j)
        {
            const C v = full[static_cast<crd::usize>(i) * n + j];
            if (v.re != 0 || v.im != 0)
            {
                tr.push_back(static_cast<crd::i64>(i));
                tc.push_back(static_cast<crd::i64>(j));
                vals.push_back(v.re);
                vals.push_back(v.im);
            }
        }
    }
    crd::containers::Array<crd::f64> b(&alloc);
    b.resize(2 * static_cast<crd::usize>(n));
    for (crd::u32 i = 0; i < n; ++i)
    {
        C acc{0, 0};
        for (crd::u32 j = 0; j < n; ++j)
        {
            acc = acc + full[static_cast<crd::usize>(i) * n + j] * xtrue[j];
        }
        b[2 * i] = acc.re;
        b[2 * i + 1] = acc.im;
    }
    auto make_args = [&]()
    {
        cli::CommandArgs args{&alloc};
        args.set_u64("rows", n);
        args.set_u64("cols", n);
        args.set_i64_array("triplet_rows", {tr.data(), tr.size()});
        args.set_i64_array("triplet_cols", {tc.data(), tc.size()});
        args.set_f64_array("values", {vals.data(), vals.size()});
        args.set_f64_array("b", {b.data(), b.size()});
        return args;
    };

    // LDLᴴ (Hermitian) recovers x_true.
    const auto* rec_h = cli::CommandRegistry::global().find("hesap.direct.ldlh.c64");
    REQUIRE(rec_h != nullptr);
    const cli::CommandResult rh = rec_h->impl(make_args());
    REQUIRE(rh.ok);
    const auto* blob_h = as_blob(rh);
    REQUIRE(blob_h != nullptr);
    REQUIRE(blob_h->bytes.size() == (2 * static_cast<crd::usize>(n) + 1) * sizeof(crd::f64)); // [info, x...]
    const auto* oh = reinterpret_cast<const crd::f64*>(blob_h->bytes.data());
    CHECK(oh[0] == 0.0);
    crd::f64 herm_err = 0.0;
    for (crd::u32 i = 0; i < n; ++i)
    {
        herm_err = std::max(herm_err, std::abs(oh[1 + 2 * i] - xtrue[i].re));
        herm_err = std::max(herm_err, std::abs(oh[1 + 2 * i + 1] - xtrue[i].im));
    }
    CHECK(herm_err < 1e-9);

    // LDLᵀ (complex-symmetric) on the SAME Hermitian input solves a DIFFERENT system ⇒ NOT x_true. This is
    // the CLI-level proof that ldlt vs ldlh are distinct, mode-selecting commands (the advisor's check).
    const auto* rec_s = cli::CommandRegistry::global().find("hesap.direct.ldlt.c64");
    REQUIRE(rec_s != nullptr);
    const cli::CommandResult rs = rec_s->impl(make_args());
    REQUIRE(rs.ok);
    const auto* blob_s = as_blob(rs);
    REQUIRE(blob_s != nullptr);
    const auto* os = reinterpret_cast<const crd::f64*>(blob_s->bytes.data());
    crd::f64 sym_diff = 0.0;
    for (crd::u32 i = 0; i < n; ++i)
    {
        sym_diff = std::max(sym_diff, std::abs(os[1 + 2 * i] - xtrue[i].re));
        sym_diff = std::max(sym_diff, std::abs(os[1 + 2 * i + 1] - xtrue[i].im));
    }
    CHECK(sym_diff > 1e-3); // the symmetric interpretation ≠ the Hermitian solution ⇒ the mode flag is real
}
