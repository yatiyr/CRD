#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <crd/hesap/cli/arg_value.hpp>
#include <crd/hesap/cli/command_registry.hpp>
#include <crd/hesap/dense/cli_anchor.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <cstring>

namespace
{
// Force the cli_register.cpp TU to be pulled in by the linker so its
// static-init block runs and registers the BLAS L1 commands. The actual
// call happens during static initialization of this anonymous-namespace
// constant. ADR-0081 §7 anchor pattern.
struct AnchorPull
{
    AnchorPull() noexcept { crd::hesap::dense::register_blas1_cli_anchor(); }
};
const AnchorPull kAnchorPull;
} // namespace

using crd::hesap::cli::CommandArgs;
using crd::hesap::cli::CommandRegistry;
using crd::hesap::cli::ResultBinaryBlob;
using crd::hesap::cli::ResultError;
using crd::hesap::cli::ResultScalarF64;

namespace
{
const crd::hesap::cli::CommandRecord* find(const char* name)
{
    return CommandRegistry::global().find(name);
}

crd::containers::ConstSpan<crd::f64> as_f64_array(const ResultBinaryBlob& blob)
{
    return crd::containers::ConstSpan<crd::f64>{
        reinterpret_cast<const crd::f64*>(blob.bytes.data()),
        blob.bytes.size() / sizeof(crd::f64)};
}
} // namespace

TEST_CASE("CLI: all 28 BLAS L1 commands are registered", "[hesap][blas1][cli]")
{
    const char* names[] = {
        "hesap.dense.blas1.axpy.f32",  "hesap.dense.blas1.dot.f32",   "hesap.dense.blas1.nrm2.f32",
        "hesap.dense.blas1.scal.f32",  "hesap.dense.blas1.copy.f32",  "hesap.dense.blas1.asum.f32",
        "hesap.dense.blas1.iamax.f32",
        "hesap.dense.blas1.axpy.f64",  "hesap.dense.blas1.dot.f64",   "hesap.dense.blas1.nrm2.f64",
        "hesap.dense.blas1.scal.f64",  "hesap.dense.blas1.copy.f64",  "hesap.dense.blas1.asum.f64",
        "hesap.dense.blas1.iamax.f64",
        "hesap.dense.blas1.axpy.c32",  "hesap.dense.blas1.dotu.c32",  "hesap.dense.blas1.dotc.c32",
        "hesap.dense.blas1.nrm2.c32",  "hesap.dense.blas1.scal.c32",  "hesap.dense.blas1.asum.c32",
        "hesap.dense.blas1.iamax.c32",
        "hesap.dense.blas1.axpy.c64",  "hesap.dense.blas1.dotu.c64",  "hesap.dense.blas1.dotc.c64",
        "hesap.dense.blas1.nrm2.c64",  "hesap.dense.blas1.scal.c64",  "hesap.dense.blas1.asum.c64",
        "hesap.dense.blas1.iamax.c64",
    };
    for (const char* n : names)
    {
        INFO("missing command: " << n);
        REQUIRE(find(n) != nullptr);
    }
}

TEST_CASE("CLI: hesap.dense.blas1.dot.f64 returns the dot product", "[hesap][blas1][cli][dot]")
{
    crd::memory::TlsfAllocator alloc(64 * 1024);
    CommandArgs args{&alloc};
    const crd::f64 x_data[] = {1.0, 2.0, 3.0, 4.0};
    const crd::f64 y_data[] = {5.0, 6.0, 7.0, 8.0};
    args.set_f64_array("x", crd::containers::ConstSpan<crd::f64>{x_data, 4});
    args.set_f64_array("y", crd::containers::ConstSpan<crd::f64>{y_data, 4});
    const auto* rec = find("hesap.dense.blas1.dot.f64");
    REQUIRE(rec != nullptr);
    const auto r = rec->impl(args);
    REQUIRE(r.ok);
    const auto* s = std::get_if<ResultScalarF64>(&r.value);
    REQUIRE(s != nullptr);
    REQUIRE(s->value == 70.0);
}

TEST_CASE("CLI: hesap.dense.blas1.axpy.f64 returns the updated y array", "[hesap][blas1][cli][axpy]")
{
    crd::memory::TlsfAllocator alloc(64 * 1024);
    CommandArgs args{&alloc};
    const crd::f64 x_data[] = {1.0, 2.0, 3.0};
    const crd::f64 y_data[] = {10.0, 20.0, 30.0};
    args.set_f64("alpha", 2.0);
    args.set_f64_array("x", crd::containers::ConstSpan<crd::f64>{x_data, 3});
    args.set_f64_array("y", crd::containers::ConstSpan<crd::f64>{y_data, 3});
    const auto* rec = find("hesap.dense.blas1.axpy.f64");
    const auto r = rec->impl(args);
    REQUIRE(r.ok);
    const auto* b = std::get_if<ResultBinaryBlob>(&r.value);
    REQUIRE(b != nullptr);
    const auto out = as_f64_array(*b);
    REQUIRE(out.size() == 3);
    REQUIRE(out[0] == 12.0);
    REQUIRE(out[1] == 24.0);
    REQUIRE(out[2] == 36.0);
}

TEST_CASE("CLI: hesap.dense.blas1.nrm2.f64 returns the Euclidean norm", "[hesap][blas1][cli][nrm2]")
{
    crd::memory::TlsfAllocator alloc(64 * 1024);
    CommandArgs args{&alloc};
    const crd::f64 x_data[] = {3.0, 4.0};
    args.set_f64_array("x", crd::containers::ConstSpan<crd::f64>{x_data, 2});
    const auto r = find("hesap.dense.blas1.nrm2.f64")->impl(args);
    REQUIRE(r.ok);
    REQUIRE(std::get<ResultScalarF64>(r.value).value == 5.0);
}

TEST_CASE("CLI: hesap.dense.blas1.iamax.f64 returns the argmax index", "[hesap][blas1][cli][iamax]")
{
    crd::memory::TlsfAllocator alloc(64 * 1024);
    CommandArgs args{&alloc};
    const crd::f64 x_data[] = {1.0, -5.0, 3.0, -2.0};
    args.set_f64_array("x", crd::containers::ConstSpan<crd::f64>{x_data, 4});
    const auto r = find("hesap.dense.blas1.iamax.f64")->impl(args);
    REQUIRE(r.ok);
    REQUIRE(std::get<ResultScalarF64>(r.value).value == 1.0);
}

TEST_CASE("CLI: empty input returns InvalidArgument error", "[hesap][blas1][cli][err]")
{
    crd::memory::TlsfAllocator alloc(64 * 1024);
    CommandArgs args{&alloc};
    const auto r = find("hesap.dense.blas1.nrm2.f64")->impl(args);
    REQUIRE_FALSE(r.ok);
    const auto* e = std::get_if<ResultError>(&r.value);
    REQUIRE(e != nullptr);
    REQUIRE(crd::containers::StringView{e->error_kind.c_str()} ==
            crd::containers::StringView{"InvalidArgument"});
}

TEST_CASE("CLI: hesap.dense.blas1.dotu.c64 returns flattened complex result", "[hesap][blas1][cli][complex]")
{
    crd::memory::TlsfAllocator alloc(64 * 1024);
    CommandArgs args{&alloc};
    // x = [(1,2), (3,4)] -> flat [1, 2, 3, 4]
    const crd::f64 x_flat[] = {1.0, 2.0, 3.0, 4.0};
    const crd::f64 y_flat[] = {5.0, 6.0, 7.0, 8.0};
    args.set_f64_array("x", crd::containers::ConstSpan<crd::f64>{x_flat, 4});
    args.set_f64_array("y", crd::containers::ConstSpan<crd::f64>{y_flat, 4});
    const auto r = find("hesap.dense.blas1.dotu.c64")->impl(args);
    REQUIRE(r.ok);
    const auto out = as_f64_array(std::get<ResultBinaryBlob>(r.value));
    REQUIRE(out.size() == 2);
    REQUIRE(out[0] == -18.0);
    REQUIRE(out[1] == 68.0);
}

TEST_CASE("CLI: hesap.dense.blas1.dotc.c64 differs from dotu", "[hesap][blas1][cli][complex]")
{
    crd::memory::TlsfAllocator alloc(64 * 1024);
    CommandArgs args{&alloc};
    const crd::f64 x_flat[] = {1.0, 2.0, 3.0, 4.0};
    const crd::f64 y_flat[] = {5.0, 6.0, 7.0, 8.0};
    args.set_f64_array("x", crd::containers::ConstSpan<crd::f64>{x_flat, 4});
    args.set_f64_array("y", crd::containers::ConstSpan<crd::f64>{y_flat, 4});
    const auto r = find("hesap.dense.blas1.dotc.c64")->impl(args);
    REQUIRE(r.ok);
    const auto out = as_f64_array(std::get<ResultBinaryBlob>(r.value));
    REQUIRE(out.size() == 2);
    // conj(1+2i)*(5+6i) + conj(3+4i)*(7+8i)
    // = (1-2i)(5+6i) + (3-4i)(7+8i)
    // = (5+12) + (6-10)i + (21+32) + (24-28)i = 17-4i + 53-4i = 70-8i
    REQUIRE(out[0] == 70.0);
    REQUIRE(out[1] == -8.0);
}

TEST_CASE("CLI: hesap.dense.blas1.scal.f64 returns scaled array", "[hesap][blas1][cli][scal]")
{
    crd::memory::TlsfAllocator alloc(64 * 1024);
    CommandArgs args{&alloc};
    args.set_f64("alpha", 3.0);
    const crd::f64 x_data[] = {1.0, 2.0, 4.0};
    args.set_f64_array("x", crd::containers::ConstSpan<crd::f64>{x_data, 3});
    const auto r = find("hesap.dense.blas1.scal.f64")->impl(args);
    REQUIRE(r.ok);
    const auto out = as_f64_array(std::get<ResultBinaryBlob>(r.value));
    REQUIRE(out.size() == 3);
    REQUIRE(out[0] == 3.0);
    REQUIRE(out[2] == 12.0);
}

TEST_CASE("CLI: idempotent BLAS L1 ops all flag idempotent=true", "[hesap][blas1][cli][meta]")
{
    const auto& reg = CommandRegistry::global();
    crd::usize blas1_count = 0;
    for (const auto* rec : reg.all())
    {
        const auto name = crd::containers::StringView{rec->schema.name.c_str(), rec->schema.name.size()};
        if (name.starts_with("hesap.dense.blas1."))
        {
            ++blas1_count;
            REQUIRE(rec->schema.idempotent);
        }
    }
    REQUIRE(blas1_count == 28);
}
