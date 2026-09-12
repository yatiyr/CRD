// Matrix-resource CLI command registration. Phase 3.1.6 v4-corpus.
//
// Registers 9 CommandSchemas via the CRD_HESAP_CLI_REGISTER_MODULE static-init
// hook (ADR-0081 §7 + §10):
//   info               : .mtx text -> structural metadata (Text; type-agnostic).
//   cook.<T>           : .mtx text -> cooked 'HMTX' CRDR bytes (BinaryBlob).
//   load.<T>           : .mtx text -> cook -> load -> CSR values (BinaryBlob;
//                        complex flattened {re,im,...}). Proves the cook->load
//                        pipeline round-trips through the binary format.
//
// Stateless wire shape (D14 precedent): commands operate on inline .mtx text,
// not a stateful ResourceManager session. The full mount + load_sync<
// SparseMatrixResource> path is exercised by smoke_hesap_matrix_resource.
// `fetch` is DEV-TIME (build-time file(DOWNLOAD)); Cerid has no HTTP client.

#include <crd/hesap/cli/arg_value.hpp>
#include <crd/hesap/cli/command_registry.hpp>
#include <crd/hesap/complex.hpp>
#include <crd/hesap/resources/matrix_artifact_builder.hpp>
#include <crd/hesap/resources/matrix_resource.hpp>
#include <crd/hesap/resources/matrix_resource_loader.hpp>
#include <crd/hesap/sparse/matrix_market.hpp>
#include <crd/resources/resource_id.hpp>

#include <charconv>
#include <type_traits>
#include <utility>

namespace crd::hesap::resources
{
void register_hesap_matrix_cli_anchor() noexcept {}
} // namespace crd::hesap::resources

namespace
{
using namespace crd::hesap;
using namespace crd::hesap::cli;
using namespace crd::hesap::resources;
using crd::hesap::sparse::MatrixMarketError;
using crd::hesap::sparse::read_matrix_market;

template <typename> struct CliIsComplex : std::false_type
{
};
template <typename U> struct CliIsComplex<crd::hesap::Complex<U>> : std::true_type
{
};

CommandResult error_result(crd::memory::IAllocator* alloc, const char* msg)
{
    CommandResult r{alloc};
    r.ok = false;
    ResultError e{alloc};
    e.error_kind = crd::containers::String{"InvalidArgument", alloc};
    e.error_message = crd::containers::String{msg, alloc};
    r.value = std::move(e);
    return r;
}

CommandResult text_result(crd::memory::IAllocator* alloc, crd::containers::String&& text)
{
    CommandResult r{alloc};
    r.ok = true;
    ResultText t{alloc};
    t.text = std::move(text);
    r.value = std::move(t);
    return r;
}

CommandResult blob_f64_result(crd::memory::IAllocator* alloc, crd::containers::ConstSpan<crd::f64> values)
{
    CommandResult r{alloc};
    r.ok = true;
    ResultBinaryBlob blob{alloc};
    const auto* raw = reinterpret_cast<const crd::u8*>(values.data());
    const crd::usize n_bytes = values.size() * sizeof(crd::f64);
    blob.bytes.reserve(n_bytes);
    for (crd::usize i = 0; i < n_bytes; ++i)
    {
        blob.bytes.push_back(raw[i]);
    }
    r.value = std::move(blob);
    return r;
}

CommandResult blob_u8_result(crd::memory::IAllocator* alloc, crd::containers::ConstSpan<crd::u8> bytes)
{
    CommandResult r{alloc};
    r.ok = true;
    ResultBinaryBlob blob{alloc};
    blob.bytes.reserve(bytes.size());
    for (crd::usize i = 0; i < bytes.size(); ++i)
    {
        blob.bytes.push_back(bytes[i]);
    }
    r.value = std::move(blob);
    return r;
}

void append_cstr(crd::containers::String& s, const char* p)
{
    for (const char* q = p; *q != '\0'; ++q)
    {
        s.push_back(*q);
    }
}

void append_u64(crd::containers::String& s, crd::u64 v)
{
    char buf[24];
    auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), v);
    (void)ec;
    for (char* q = buf; q < ptr; ++q)
    {
        s.push_back(*q);
    }
}

// ---- info : .mtx text -> structural metadata (type-agnostic) -----------

CommandResult impl_info(const CommandArgs& args)
{
    const auto text = args.get_string("text");
    if (text.empty())
    {
        return error_result(args.alloc, "info: text (.mtx) is required");
    }
    MatrixMarketError err{args.alloc};
    auto m = read_matrix_market<crd::f64>(text, args.alloc, err);
    if (!err.ok)
    {
        return error_result(args.alloc, err.message.c_str());
    }
    crd::containers::String s{args.alloc};
    append_cstr(s, "rows=");
    append_u64(s, m.rows());
    append_cstr(s, " cols=");
    append_u64(s, m.cols());
    append_cstr(s, " nnz=");
    append_u64(s, static_cast<crd::u64>(m.nnz()));
    append_cstr(s, " topology_hash=");
    append_u64(s, m.pattern().topology_hash);
    return text_result(args.alloc, std::move(s));
}

// ---- cook.<T> : .mtx text -> cooked 'HMTX' bytes -----------------------

template <typename T> CommandResult impl_cook(const CommandArgs& args)
{
    const auto text = args.get_string("text");
    if (text.empty())
    {
        return error_result(args.alloc, "cook: text (.mtx) is required");
    }
    const crd::u64 hi = args.get_u64("id_hi").value_or(0U);
    const crd::u64 lo = args.get_u64("id_lo").value_or(0U);
    MatrixMarketError err{args.alloc};
    auto bytes = cook_matrix_market<T>(args.alloc, crd::resources::ResourceId{hi, lo}, text, err);
    if (!err.ok)
    {
        return error_result(args.alloc, err.message.c_str());
    }
    return blob_u8_result(args.alloc, crd::containers::ConstSpan<crd::u8>{bytes.data(), bytes.size()});
}

// ---- load.<T> : .mtx text -> cook -> load -> CSR values ----------------

template <typename T> CommandResult impl_load(const CommandArgs& args)
{
    const auto text = args.get_string("text");
    if (text.empty())
    {
        return error_result(args.alloc, "load: text (.mtx) is required");
    }
    MatrixMarketError err{args.alloc};
    auto bytes = cook_matrix_market<T>(args.alloc, crd::resources::ResourceId{0U, 0U}, text, err);
    if (!err.ok)
    {
        return error_result(args.alloc, err.message.c_str());
    }
    SparseMatrixResource res{args.alloc};
    if (!read_matrix_resource(crd::containers::ConstSpan<crd::u8>{bytes.data(), bytes.size()}, res, args.alloc))
    {
        return error_result(args.alloc, "load: cooked blob failed to load (corruption)");
    }
    auto m = res.template build_csr<T>(args.alloc);

    crd::containers::Array<crd::f64> out(args.alloc);
    if constexpr (CliIsComplex<T>::value)
    {
        out.reserve(m.nnz() * 2);
        for (crd::usize i = 0; i < m.nnz(); ++i)
        {
            out.push_back(static_cast<crd::f64>(m.values().values[i].re));
            out.push_back(static_cast<crd::f64>(m.values().values[i].im));
        }
    }
    else
    {
        out.reserve(m.nnz());
        for (crd::usize i = 0; i < m.nnz(); ++i)
        {
            out.push_back(static_cast<crd::f64>(m.values().values[i]));
        }
    }
    return blob_f64_result(args.alloc, crd::containers::ConstSpan<crd::f64>{out.data(), out.size()});
}

// ---- schema helpers ----------------------------------------------------

void add_param(CommandSchema& s, crd::memory::IAllocator* alloc, const char* name, const char* desc, ParamKind kind,
               bool required)
{
    ParamSchema p{alloc};
    p.name = crd::containers::String{name, alloc};
    p.description = crd::containers::String{desc, alloc};
    p.kind = kind;
    p.required = required;
    s.params.push_back(std::move(p));
}

CommandSchema make_mtx_schema(crd::memory::IAllocator* alloc, const char* name, const char* desc, OutputKind out_kind,
                              bool with_id)
{
    CommandSchema s{alloc};
    s.name = crd::containers::String{name, alloc};
    s.description = crd::containers::String{desc, alloc};
    s.output.kind = out_kind;
    s.required_caps.bits = Capability::kHesapCompute;
    s.idempotent = true;
    add_param(s, alloc, "text", "Matrix Market (.mtx) coordinate text", ParamKind::String, true);
    if (with_id)
    {
        add_param(s, alloc, "id_hi", "ResourceId high 64 bits (default 0)", ParamKind::U64, false);
        add_param(s, alloc, "id_lo", "ResourceId low 64 bits (default 0)", ParamKind::U64, false);
    }
    return s;
}

} // namespace

// Registration uses crd allocators (abort on OOM, never throw); the std bad_alloc path the check
// traces is unreachable, and the registrar ctor is noexcept (would terminate, not escape) regardless.
// NOLINTNEXTLINE(bugprone-throwing-static-initialization)
CRD_HESAP_CLI_REGISTER_MODULE(
    [](CommandRegistry& reg)
    {
        auto* alloc = crd::memory::default_allocator();

        reg.register_command(make_mtx_schema(alloc, "hesap.matrix.info",
                                             "Inspect a .mtx matrix: returns rows/cols/nnz/topology_hash (Text).",
                                             OutputKind::Text, false),
                             &impl_info);

        reg.register_command(make_mtx_schema(alloc, "hesap.matrix.cook.f32",
                                             "Cook .mtx text into an 'HMTX' CRDR blob (f32).", OutputKind::BinaryBlob,
                                             true),
                             &impl_cook<crd::f32>);
        reg.register_command(make_mtx_schema(alloc, "hesap.matrix.cook.f64",
                                             "Cook .mtx text into an 'HMTX' CRDR blob (f64).", OutputKind::BinaryBlob,
                                             true),
                             &impl_cook<crd::f64>);
        reg.register_command(make_mtx_schema(alloc, "hesap.matrix.cook.c32",
                                             "Cook .mtx text into an 'HMTX' CRDR blob (Complex<f32>).",
                                             OutputKind::BinaryBlob, true),
                             &impl_cook<crd::hesap::Complex<crd::f32>>);
        reg.register_command(make_mtx_schema(alloc, "hesap.matrix.cook.c64",
                                             "Cook .mtx text into an 'HMTX' CRDR blob (Complex<f64>).",
                                             OutputKind::BinaryBlob, true),
                             &impl_cook<crd::hesap::Complex<crd::f64>>);

        reg.register_command(make_mtx_schema(alloc, "hesap.matrix.load.f32",
                                             "Cook+load .mtx through 'HMTX'; returns CSR values (f32 blob).",
                                             OutputKind::BinaryBlob, false),
                             &impl_load<crd::f32>);
        reg.register_command(make_mtx_schema(alloc, "hesap.matrix.load.f64",
                                             "Cook+load .mtx through 'HMTX'; returns CSR values (f64 blob).",
                                             OutputKind::BinaryBlob, false),
                             &impl_load<crd::f64>);
        reg.register_command(
            make_mtx_schema(alloc, "hesap.matrix.load.c32",
                            "Cook+load .mtx through 'HMTX'; returns CSR values (Complex<f32> flattened blob).",
                            OutputKind::BinaryBlob, false),
            &impl_load<crd::hesap::Complex<crd::f32>>);
        reg.register_command(
            make_mtx_schema(alloc, "hesap.matrix.load.c64",
                            "Cook+load .mtx through 'HMTX'; returns CSR values (Complex<f64> flattened blob).",
                            OutputKind::BinaryBlob, false),
            &impl_load<crd::hesap::Complex<crd::f64>>);
    });
