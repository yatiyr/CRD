// v7-z — CLI registration + invocation tests for hesap.opt.* (the data-defined families: QP/LP/MIP/conic).
// Pulls the module anchor so the static-init registration survives the static-lib link, then drives each
// command through the registry's CommandArgs/CommandResult wire shape against ANALYTIC answers already
// pinned by the per-slice batteries (the box-projection QP, the vertex LP, the fractional-relaxation MIP,
// the norm-ball SOCP).

#include <crd/hesap/cli/arg_value.hpp>
#include <crd/hesap/cli/command_registry.hpp>
#include <crd/hesap/cli/command_result.hpp>
#include <crd/hesap/opt/cli_anchor.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstring>
#include <limits>
#include <variant>

namespace cli = crd::hesap::cli;

namespace
{
constexpr crd::f64 kInf = std::numeric_limits<crd::f64>::infinity();

// Force the linker to keep cli_register_opt.cpp's static-init block.
const bool kPullOpt = (crd::hesap::opt::register_opt_cli_anchor(), true);

// Decode the output blob into f64s.
crd::usize blob_f64(const cli::CommandResult& r, crd::f64* out, crd::usize cap)
{
    const auto* blob = std::get_if<cli::ResultBinaryBlob>(&r.value);
    REQUIRE(blob != nullptr);
    const crd::usize count = blob->bytes.size() / sizeof(crd::f64);
    REQUIRE(count <= cap);
    std::memcpy(out, blob->bytes.data(), blob->bytes.size());
    return count;
}
} // namespace

TEST_CASE("CLI opt: all hesap.opt.* commands are registered", "[hesap-opt][cli]")
{
    REQUIRE(kPullOpt);
    auto& reg = cli::CommandRegistry::global();
    for (const char* name : {"hesap.opt.qp.f64", "hesap.opt.lp.f64", "hesap.opt.mip.f64", "hesap.opt.conic.f64"})
    {
        const auto* rec = reg.find(name);
        REQUIRE(rec != nullptr);
        REQUIRE(rec->impl != nullptr);
    }
}

TEST_CASE("CLI hesap.opt.qp.f64: the box projection", "[hesap-opt][cli]")
{
    REQUIRE(kPullOpt);
    crd::memory::TlsfAllocator alloc(8 << 20);
    // min 0.5||x - z||^2 s.t. 0 <= x <= 1 with z = (2, -1): x* = (1, 0).
    const crd::f64 p[] = {1.0, 0.0, 0.0, 1.0};
    const crd::f64 q[] = {-2.0, 1.0};
    const crd::f64 a[] = {1.0, 0.0, 0.0, 1.0};
    const crd::f64 l[] = {0.0, 0.0};
    const crd::f64 u[] = {1.0, 1.0};

    cli::CommandArgs args{&alloc};
    args.set_u64("n", 2);
    args.set_u64("m", 2);
    args.set_f64_array("p", {p, 4});
    args.set_f64_array("q", {q, 2});
    args.set_f64_array("a", {a, 4});
    args.set_f64_array("l", {l, 2});
    args.set_f64_array("u", {u, 2});
    args.set_u64("method", 1); // Mehrotra (the accuracy reference)

    auto& reg = cli::CommandRegistry::global();
    const auto* rec = reg.find("hesap.opt.qp.f64");
    REQUIRE(rec != nullptr);
    const cli::CommandResult r = rec->impl(args);
    REQUIRE(r.ok);
    crd::f64 out[8];
    const crd::usize count = blob_f64(r, out, 8);
    REQUIRE(count == 2 + 2 + 2);
    CHECK(out[0] == 0.0); // Solved
    CHECK(std::fabs(out[2] - 1.0) < 1e-6);
    CHECK(std::fabs(out[3] - 0.0) < 1e-6);
}

TEST_CASE("CLI hesap.opt.lp.f64: the analytic vertex (both methods)", "[hesap-opt][cli]")
{
    REQUIRE(kPullOpt);
    crd::memory::TlsfAllocator alloc(8 << 20);
    // min -x - 2y s.t. x+y <= 4, x <= 3, y <= 2, x,y >= 0 -> (2, 2), obj -6.
    const crd::f64 c[] = {-1.0, -2.0};
    const crd::f64 a[] = {1.0, 1.0, 1.0, 0.0, 0.0, 1.0};
    const crd::f64 l[] = {-kInf, -kInf, -kInf};
    const crd::f64 u[] = {4.0, 3.0, 2.0};
    const crd::f64 xlo[] = {0.0, 0.0};
    const crd::f64 xup[] = {kInf, kInf};

    auto& reg = cli::CommandRegistry::global();
    const auto* rec = reg.find("hesap.opt.lp.f64");
    REQUIRE(rec != nullptr);
    for (crd::u64 method = 0; method <= 1; ++method)
    {
        cli::CommandArgs args{&alloc};
        args.set_u64("n", 2);
        args.set_u64("m", 3);
        args.set_f64_array("c", {c, 2});
        args.set_f64_array("a", {a, 6});
        args.set_f64_array("l", {l, 3});
        args.set_f64_array("u", {u, 3});
        args.set_f64_array("xlo", {xlo, 2});
        args.set_f64_array("xup", {xup, 2});
        args.set_u64("method", method);
        const cli::CommandResult r = rec->impl(args);
        REQUIRE(r.ok);
        crd::f64 out[8];
        const crd::usize count = blob_f64(r, out, 8);
        REQUIRE(count == 2 + 2 + 3);
        CHECK(out[0] == 0.0); // Solved
        CHECK(std::fabs(out[1] - (-6.0)) < 1e-5);
        CHECK(std::fabs(out[2] - 2.0) < 1e-5);
        CHECK(std::fabs(out[3] - 2.0) < 1e-5);
    }
}

TEST_CASE("CLI hesap.opt.mip.f64: the fractional relaxation forced integral", "[hesap-opt][cli]")
{
    REQUIRE(kPullOpt);
    crd::memory::TlsfAllocator alloc(8 << 20);
    // max x + y s.t. 2x+3y <= 12, 6x+5y <= 30, x,y in Z, 0 <= x,y <= 10 -> obj -(-5) = 5.
    const crd::f64 c[] = {-1.0, -1.0};
    const crd::f64 a[] = {2.0, 3.0, 6.0, 5.0};
    const crd::f64 l[] = {-kInf, -kInf};
    const crd::f64 u[] = {12.0, 30.0};
    const crd::f64 xlo[] = {0.0, 0.0};
    const crd::f64 xup[] = {10.0, 10.0};
    const crd::i64 integer[] = {1, 1};

    cli::CommandArgs args{&alloc};
    args.set_u64("n", 2);
    args.set_u64("m", 2);
    args.set_f64_array("c", {c, 2});
    args.set_f64_array("a", {a, 4});
    args.set_f64_array("l", {l, 2});
    args.set_f64_array("u", {u, 2});
    args.set_f64_array("xlo", {xlo, 2});
    args.set_f64_array("xup", {xup, 2});
    args.set_i64_array("integer", {integer, 2});

    auto& reg = cli::CommandRegistry::global();
    const auto* rec = reg.find("hesap.opt.mip.f64");
    REQUIRE(rec != nullptr);
    const cli::CommandResult r = rec->impl(args);
    REQUIRE(r.ok);
    crd::f64 out[8];
    const crd::usize count = blob_f64(r, out, 8);
    REQUIRE(count == 3 + 2);
    CHECK(out[0] == 0.0); // Solved (the PROVEN optimum)
    CHECK(std::fabs(out[1] - (-5.0)) < 1e-9);
    CHECK(std::fabs(out[3] - std::floor(out[3] + 0.5)) < 1e-9); // integral
    CHECK(std::fabs(out[4] - std::floor(out[4] + 0.5)) < 1e-9);
}

TEST_CASE("CLI hesap.opt.conic.f64: the norm-ball SOCP", "[hesap-opt][cli]")
{
    REQUIRE(kPullOpt);
    crd::memory::TlsfAllocator alloc(8 << 20);
    // min c'x s.t. ||x - p|| <= r, p = (1,2), r = 0.5, c = (1,1): obj = 3 - 0.5*sqrt(2).
    const crd::f64 c[] = {1.0, 1.0};
    const crd::f64 a[] = {0.0, 0.0, -1.0, 0.0, 0.0, -1.0};
    const crd::f64 b[] = {0.5, -1.0, -2.0};
    const crd::i64 types[] = {2}; // Soc
    const crd::i64 dims[] = {3};

    cli::CommandArgs args{&alloc};
    args.set_u64("n", 2);
    args.set_u64("m", 3);
    args.set_f64_array("c", {c, 2});
    args.set_f64_array("a", {a, 6});
    args.set_f64_array("b", {b, 3});
    args.set_i64_array("cone_types", {types, 1});
    args.set_i64_array("cone_dims", {dims, 1});

    auto& reg = cli::CommandRegistry::global();
    const auto* rec = reg.find("hesap.opt.conic.f64");
    REQUIRE(rec != nullptr);
    const cli::CommandResult r = rec->impl(args);
    REQUIRE(r.ok);
    crd::f64 out[12];
    const crd::usize count = blob_f64(r, out, 12);
    REQUIRE(count == 2 + 2 + 3 + 3);
    CHECK(out[0] == 0.0); // Solved
    CHECK(std::fabs(out[1] - (3.0 - 0.5 * std::sqrt(2.0))) < 1e-5);
}

TEST_CASE("CLI hesap.opt: malformed requests return errors", "[hesap-opt][cli]")
{
    REQUIRE(kPullOpt);
    crd::memory::TlsfAllocator alloc(8 << 20);
    cli::CommandArgs args{&alloc}; // missing everything
    auto& reg = cli::CommandRegistry::global();
    for (const char* name : {"hesap.opt.qp.f64", "hesap.opt.lp.f64", "hesap.opt.mip.f64", "hesap.opt.conic.f64"})
    {
        const auto* rec = reg.find(name);
        REQUIRE(rec != nullptr);
        const cli::CommandResult r = rec->impl(args);
        CHECK(!r.ok);
    }
}
