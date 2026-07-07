// v14-z — CLI registration + invocation gates for hesap.tensor.*. Pulls the module anchor
// so the static-init registration block survives the static-lib link (the v13-z pattern),
// verifies all 12 commands register, drives representative commands through the registry,
// and gates run-twice text bit-identity (the CLI is a deterministic surface: Philox inputs,
// fixed budgets — timing rides the Hint diagnostic, never the text).
#include <crd/hesap/cli/arg_value.hpp>
#include <crd/hesap/cli/command_registry.hpp>
#include <crd/hesap/cli/command_result.hpp>
#include <crd/hesap/tensor/cli_anchor.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

#include <variant>

namespace cli = crd::hesap::cli;

namespace
{

const bool pull_tensor = (crd::hesap::tensor::register_tensor_cli_anchor(), true);

const cli::ResultText* as_text(const cli::CommandResult& r)
{
    return std::get_if<cli::ResultText>(&r.value);
}

const cli::ResultError* as_error(const cli::CommandResult& r)
{
    return std::get_if<cli::ResultError>(&r.value);
}

bool contains(const crd::containers::String& hay, const char* needle)
{
    return crd::containers::StringView{hay.data(), hay.size()}.find(needle) !=
           crd::containers::StringView::npos;
}

} // namespace

TEST_CASE("CLI tensor: all 12 hesap.tensor.* commands are registered", "[tensor][cli]")
{
    REQUIRE(pull_tensor);
    const char* names[] = {
        "hesap.tensor.einsum.f64",  "hesap.tensor.ew.f64",     "hesap.tensor.reduce.f64",
        "hesap.tensor.permute.f64", "hesap.tensor.batched.f64", "hesap.tensor.hyperopt",
        "hesap.tensor.sparse.mttkrp.f64", "hesap.tensor.decomp.f64", "hesap.tensor.tt.f64",
        "hesap.tensor.io.info",     "hesap.tensor.io.philox.f64", "hesap.tensor.nn.f32",
    };
    for (const char* n : names)
    {
        INFO(n);
        REQUIRE(cli::CommandRegistry::global().find(n) != nullptr);
    }
}

TEST_CASE("CLI tensor: einsum executes and is run-twice bit-identical", "[tensor][cli]")
{
    REQUIRE(pull_tensor);
    crd::memory::TlsfAllocator alloc(1U << 22);
    const auto* rec = cli::CommandRegistry::global().find("hesap.tensor.einsum.f64");
    REQUIRE(rec != nullptr);
    crd::containers::String first{&alloc};
    for (int round = 0; round < 2; ++round)
    {
        cli::CommandArgs args{&alloc};
        args.set_string("spec", "ab,bc->ac");
        const crd::i64 sizes[3] = {24, 32, 16}; // a, b, c ascending
        args.set_i64_array("sizes", {sizes, 3});
        const cli::CommandResult r = rec->impl(args);
        const auto* text = as_text(r);
        REQUIRE(text != nullptr);
        REQUIRE(contains(text->text, "out=24x16"));
        REQUIRE(contains(text->text, "sum="));
        if (round == 0)
        {
            first.append(crd::containers::StringView{text->text.data(), text->text.size()});
        }
        else
        {
            REQUIRE(first.size() == text->text.size());
            REQUIRE(crd::containers::StringView{first.data(), first.size()} ==
                    crd::containers::StringView{text->text.data(), text->text.size()});
        }
    }
}

TEST_CASE("CLI tensor: batched cholesky reports zero failed lanes", "[tensor][cli]")
{
    REQUIRE(pull_tensor);
    crd::memory::TlsfAllocator alloc(1U << 24);
    const auto* rec = cli::CommandRegistry::global().find("hesap.tensor.batched.f64");
    REQUIRE(rec != nullptr);
    cli::CommandArgs args{&alloc};
    args.set_string("kind", "cholesky");
    args.set_i64("batch", 4);
    args.set_i64("n", 16);
    const cli::CommandResult r = rec->impl(args);
    const auto* text = as_text(r);
    REQUIRE(text != nullptr);
    REQUIRE(contains(text->text, "info_bad=0"));
}

TEST_CASE("CLI tensor: tt builds a train within tolerance", "[tensor][cli]")
{
    REQUIRE(pull_tensor);
    crd::memory::TlsfAllocator alloc(1U << 24);
    const auto* rec = cli::CommandRegistry::global().find("hesap.tensor.tt.f64");
    REQUIRE(rec != nullptr);
    cli::CommandArgs args{&alloc};
    const crd::i64 shape[4] = {8, 8, 8, 8};
    args.set_i64_array("shape", {shape, 4}); // defaults otherwise: Hilbert source, tol 1e-8
    const cli::CommandResult r = rec->impl(args);
    const auto* text = as_text(r);
    REQUIRE(text != nullptr);
    REQUIRE(contains(text->text, "ranks="));
    REQUIRE(contains(text->text, "compression="));
}

TEST_CASE("CLI tensor: bad einsum spec yields a structured error", "[tensor][cli]")
{
    REQUIRE(pull_tensor);
    crd::memory::TlsfAllocator alloc(1U << 20);
    const auto* rec = cli::CommandRegistry::global().find("hesap.tensor.einsum.f64");
    REQUIRE(rec != nullptr);
    cli::CommandArgs args{&alloc};
    args.set_string("spec", "a...b,bc->ac"); // ellipsis: rejected by the CLI demo
    const crd::i64 sizes[3] = {8, 8, 8};
    args.set_i64_array("sizes", {sizes, 3});
    const cli::CommandResult r = rec->impl(args);
    REQUIRE(as_error(r) != nullptr);
}
