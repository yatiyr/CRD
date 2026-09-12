#include <catch2/catch_test_macros.hpp>

#include <crd/hesap/cli/command_registry.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <utility>

using crd::hesap::cli::CommandArgs;
using crd::hesap::cli::CommandRegistry;
using crd::hesap::cli::CommandResult;
using crd::hesap::cli::CommandSchema;
using crd::hesap::cli::OutputKind;
using crd::hesap::cli::ResultText;

namespace
{
CommandResult echo_impl(const CommandArgs& args)
{
    CommandResult r{args.alloc};
    r.ok = true;
    ResultText t{args.alloc};
    t.text = crd::containers::String{"pong", args.alloc};
    r.value = std::move(t);
    return r;
}

CommandSchema make_echo_schema(crd::memory::IAllocator* alloc)
{
    CommandSchema s{alloc};
    s.name = crd::containers::String{"hesap.test.echo", alloc};
    s.description = crd::containers::String{"Returns the literal string 'pong'.", alloc};
    s.output.kind = OutputKind::Text;
    s.idempotent = true;
    s.reversible = false;
    return s;
}
} // namespace

TEST_CASE("CommandRegistry::global returns the same instance", "[hesap][cli][registry]")
{
    CommandRegistry& a = CommandRegistry::global();
    CommandRegistry& b = CommandRegistry::global();
    REQUIRE(&a == &b);
}

TEST_CASE("CommandRegistry registers, finds, and dispatches a command", "[hesap][cli][registry]")
{
    // Use a LOCAL registry to avoid stomping the global registry's static-init
    // populated entries (hesap.dense.blas1.* are registered before main runs).
    crd::memory::TlsfAllocator alloc(128 * 1024);
    CommandRegistry reg;

    REQUIRE(reg.register_command(make_echo_schema(&alloc), &echo_impl));
    REQUIRE(reg.size() == 1);

    const auto* rec = reg.find("hesap.test.echo");
    REQUIRE(rec != nullptr);
    REQUIRE(rec->impl != nullptr);

    CommandArgs args{&alloc};
    const CommandResult r = rec->impl(args);
    REQUIRE(r.ok);
    const auto* txt = std::get_if<ResultText>(&r.value);
    REQUIRE(txt != nullptr);
    REQUIRE(crd::containers::StringView{txt->text.c_str(), txt->text.size()} ==
            crd::containers::StringView{"pong"});
}

TEST_CASE("CommandRegistry::find returns nullptr on miss", "[hesap][cli][registry]")
{
    CommandRegistry reg;
    REQUIRE(reg.find("does.not.exist") == nullptr);
}

TEST_CASE("CommandRegistry::all reflects insertion order", "[hesap][cli][registry]")
{
    crd::memory::TlsfAllocator alloc(128 * 1024);
    CommandRegistry reg;

    auto make = [&](const char* name)
    {
        CommandSchema s{&alloc};
        s.name = crd::containers::String{name, &alloc};
        s.output.kind = OutputKind::Void;
        return s;
    };

    REQUIRE(reg.register_command(make("hesap.a"), nullptr));
    REQUIRE(reg.register_command(make("hesap.b"), nullptr));
    REQUIRE(reg.register_command(make("hesap.c"), nullptr));

    const auto all = reg.all();
    REQUIRE(all.size() == 3);
    REQUIRE(crd::containers::StringView{all[0]->schema.name.c_str(), all[0]->schema.name.size()} ==
            crd::containers::StringView{"hesap.a"});
    REQUIRE(crd::containers::StringView{all[1]->schema.name.c_str(), all[1]->schema.name.size()} ==
            crd::containers::StringView{"hesap.b"});
    REQUIRE(crd::containers::StringView{all[2]->schema.name.c_str(), all[2]->schema.name.size()} ==
            crd::containers::StringView{"hesap.c"});
}
