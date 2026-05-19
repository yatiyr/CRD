// smoke_hesap_substrate — Phase 3.1.6 v0a end-to-end smoke.
//
// Exercises every public substrate surface:
//   - Complex<f64> arithmetic
//   - LinearOp<f64> via a concrete identity-matrix subclass
//   - MatrixId / VectorId construction + null sentinel
//   - 'HDV0' CRDR FourCC pin
//   - CommandRegistry static-init macro registers a fake echo command
//   - meta.export-mcp-tools-shaped JSON dumped to stdout
//
// Exit 0 on success; non-zero with a printed diagnostic on first failure.

#include <crd/hesap/hesap.hpp>

#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <cstdio>
#include <cstdlib>
#include <utility>

namespace
{
// A trivial echo command — registered via the static-init macro so the
// smoke proves the registration path end-to-end (matches the v0b+ pattern).
crd::hesap::cli::CommandResult echo_impl(const crd::hesap::cli::CommandArgs& args)
{
    crd::hesap::cli::CommandResult r{args.alloc};
    r.ok = true;
    crd::hesap::cli::ResultText t{args.alloc};
    t.text = crd::containers::String{"pong", args.alloc};
    r.value = std::move(t);
    return r;
}

class IdentityOp final : public crd::hesap::LinearOp<double>
{
public:
    IdentityOp() : crd::hesap::LinearOp<double>(true, true) {}

    [[nodiscard]] bool apply(crd::containers::ConstSpan<double> x, crd::containers::Span<double> y) const override
    {
        if (x.size() != 3 || y.size() != 3)
        {
            return false;
        }
        for (crd::usize i = 0; i < 3; ++i)
        {
            y[i] = x[i];
        }
        return true;
    }

    [[nodiscard]] crd::usize n_rows() const noexcept override { return 3; }
    [[nodiscard]] crd::usize n_cols() const noexcept override { return 3; }
};

int fail(const char* msg)
{
    std::fprintf(stderr, "[smoke_hesap_substrate] FAIL: %s\n", msg);
    return 1;
}
} // namespace

CRD_HESAP_CLI_REGISTER_MODULE([](crd::hesap::cli::CommandRegistry& reg) {
    crd::hesap::cli::CommandSchema s{};
    s.name = crd::containers::String{"hesap.smoke.echo"};
    s.description = crd::containers::String{"Smoke echo for the v0a substrate."};
    s.output.kind = crd::hesap::cli::OutputKind::Text;
    s.idempotent = true;
    reg.register_command(std::move(s), &echo_impl);
})

int main()
{
    using namespace crd::hesap;

    // 1. Complex<f64> arithmetic sanity.
    Complex64 a{1.0, 2.0};
    Complex64 b{3.0, 4.0};
    Complex64 prod = a * b;
    if (prod.re != -5.0 || prod.im != 10.0)
    {
        return fail("Complex64 multiplication mismatch");
    }

    // 2. LinearOp identity round-trip.
    IdentityOp op;
    const double x[3] = {7.0, 8.0, 9.0};
    double y[3] = {0.0, 0.0, 0.0};
    if (!op.apply(crd::containers::ConstSpan<double>{x, 3}, crd::containers::Span<double>{y, 3}))
    {
        return fail("LinearOp apply failed");
    }
    for (crd::usize i = 0; i < 3; ++i)
    {
        if (y[i] != x[i])
        {
            return fail("LinearOp identity output mismatch");
        }
    }

    // 3. Handle round-trip.
    const MatrixId mid = MatrixId::make(42, 7);
    const VectorId vid = VectorId::make(99, 1);
    if (mid.index() != 42 || mid.generation() != 7 || vid.index() != 99 || vid.generation() != 1)
    {
        return fail("Handle make/decompose mismatch");
    }
    if (!MatrixId::null().is_null() || !VectorId::null().is_null())
    {
        return fail("Handle null sentinel mismatch");
    }

    // 4. CRDR FourCC.
    if (kHesapDenseFourCC != 0x30564448U)
    {
        return fail("kHesapDenseFourCC pin drift");
    }

    // 5. Registry — the static-init block above registered hesap.smoke.echo;
    //    confirm it shows up.
    auto& reg = cli::CommandRegistry::global();
    const auto* rec = reg.find("hesap.smoke.echo");
    if (rec == nullptr || rec->impl == nullptr)
    {
        return fail("CommandRegistry static-init missed hesap.smoke.echo");
    }

    crd::memory::TlsfAllocator alloc(128 * 1024);
    cli::CommandArgs args{&alloc};
    const cli::CommandResult r = rec->impl(args);
    if (!r.ok)
    {
        return fail("hesap.smoke.echo impl returned ok=false");
    }

    // 6. MCP descriptor JSON.
    const auto all = reg.all();
    crd::containers::Array<const cli::CommandSchema*> schemas(&alloc);
    for (const auto* p : all)
    {
        schemas.push_back(&p->schema);
    }
    const crd::containers::String mcp = cli::emit_mcp_tool_array_to_string(
        crd::containers::ConstSpan<const cli::CommandSchema*>{schemas.data(), schemas.size()}, &alloc);

    std::fprintf(stdout, "[smoke_hesap_substrate] MCP tool catalog (%zu tools):\n%s\n",
                 static_cast<std::size_t>(schemas.size()), mcp.c_str());

    std::fprintf(stdout, "[smoke_hesap_substrate] OK\n");
    return 0;
}
