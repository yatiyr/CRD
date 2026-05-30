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
    for (const char* name : {"hesap.direct.chol.f32", "hesap.direct.chol.f64", "hesap.direct.chol.c32",
                             "hesap.direct.chol.c64"})
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
    auto aij = [](crd::u32 i, crd::u32 j) { return i == j ? static_cast<crd::f64>(n + 2) : 1.0; };
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
