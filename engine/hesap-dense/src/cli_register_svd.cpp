// v3b-1b — CLI registration for the singular value decomposition. Commands
// take (A general m x n flattened RowMajor, m, n) and return the min(m,n)
// singular values (descending) as an f64 binary blob. (Singular vectors are
// available via the engine API; the CLI returns the spectrum, the primary
// agent-facing query — mirrors hesap.dense.eig.sym.)
//
// Anchor symbol: `register_svd_cli_anchor()` in `cli_anchor.hpp`.

#include <crd/hesap/cli/arg_value.hpp>
#include <crd/hesap/cli/command_registry.hpp>
#include <crd/hesap/dense/cli_anchor.hpp>
#include <crd/hesap/dense/matrix.hpp>
#include <crd/hesap/dense/svd.hpp>

#include <utility>

namespace
{
using crd::hesap::cli::Capability;
using crd::hesap::cli::CommandArgs;
using crd::hesap::cli::CommandRegistry;
using crd::hesap::cli::CommandResult;
using crd::hesap::cli::CommandSchema;
using crd::hesap::cli::OutputKind;
using crd::hesap::cli::ParamKind;
using crd::hesap::cli::ParamSchema;
using crd::hesap::cli::ResultBinaryBlob;
using crd::hesap::cli::ResultError;
using crd::hesap::dense::Matrix;
using crd::hesap::dense::svd;
using crd::hesap::dense::svdvals;

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

CommandResult binary_result(crd::memory::IAllocator* alloc, crd::containers::ConstSpan<crd::f64> values)
{
    CommandResult r{alloc};
    r.ok = true;
    ResultBinaryBlob blob{alloc};
    const crd::u8* raw = reinterpret_cast<const crd::u8*>(values.data());
    blob.bytes.reserve(values.size() * sizeof(crd::f64));
    for (crd::usize i = 0; i < values.size() * sizeof(crd::f64); ++i)
    {
        blob.bytes.push_back(raw[i]);
    }
    r.value = std::move(blob);
    return r;
}

// Read the (m, n, A) arguments into a RowMajor Matrix<T>. Returns false (with
// an error CommandResult in `err`) on a shape mismatch.
template <typename T>
bool read_matrix(const CommandArgs& args, Matrix<T>& a, const char* who, CommandResult& err)
{
    const auto a_flat = args.get_f64_array("A");
    const auto m = args.get_u64("m").value_or(crd::u64{0});
    const auto n = args.get_u64("n").value_or(crd::u64{0});
    if (m == 0 || n == 0 || a_flat.size() != m * n)
    {
        err = error_result(args.alloc, who);
        return false;
    }
    const crd::usize mm = static_cast<crd::usize>(m);
    const crd::usize nn = static_cast<crd::usize>(n);
    a = Matrix<T>(args.alloc, mm, nn);
    for (crd::usize i = 0; i < mm; ++i)
    {
        for (crd::usize j = 0; j < nn; ++j)
        {
            a.at(i, j) = static_cast<T>(a_flat[i * nn + j]);
        }
    }
    return true;
}

template <typename T>
CommandResult impl_svd(const CommandArgs& args)
{
    Matrix<T> a(args.alloc);
    CommandResult err{args.alloc};
    if (!read_matrix<T>(args, a, "svd: A=m*n (RowMajor), m+n required", err))
    {
        return err;
    }
    const auto s = svd<T>(args.alloc, a);
    crd::containers::Array<crd::f64> out(args.alloc);
    out.reserve(s.s.size());
    for (crd::usize i = 0; i < s.s.size(); ++i)
    {
        out.push_back(static_cast<crd::f64>(s.s.data()[i]));
    }
    return binary_result(args.alloc, crd::containers::ConstSpan<crd::f64>{out.data(), out.size()});
}

template <typename T>
CommandResult impl_svdvals(const CommandArgs& args)
{
    Matrix<T> a(args.alloc);
    CommandResult err{args.alloc};
    if (!read_matrix<T>(args, a, "svdvals: A=m*n (RowMajor), m+n required", err))
    {
        return err;
    }
    const auto vals = svdvals<T>(args.alloc, a);
    crd::containers::Array<crd::f64> out(args.alloc);
    out.reserve(vals.size());
    for (crd::usize i = 0; i < vals.size(); ++i)
    {
        out.push_back(static_cast<crd::f64>(vals.data()[i]));
    }
    return binary_result(args.alloc, crd::containers::ConstSpan<crd::f64>{out.data(), out.size()});
}

CommandSchema make_schema(crd::memory::IAllocator* alloc, const char* name, const char* desc)
{
    CommandSchema s{alloc};
    s.name = crd::containers::String{name, alloc};
    s.description = crd::containers::String{desc, alloc};
    s.output.kind = OutputKind::BinaryBlob;
    s.required_caps.bits = Capability::kHesapCompute;
    s.idempotent = true;
    return s;
}

void add_param(CommandSchema& s, crd::memory::IAllocator* alloc, const char* name, const char* desc,
               ParamKind kind, bool required)
{
    ParamSchema p{alloc};
    p.name = crd::containers::String{name, alloc};
    p.description = crd::containers::String{desc, alloc};
    p.kind = kind;
    p.required = required;
    s.params.push_back(std::move(p));
}

void add_matrix_params(CommandSchema& s, crd::memory::IAllocator* alloc)
{
    add_param(s, alloc, "m", "Rows of A", ParamKind::U64, true);
    add_param(s, alloc, "n", "Columns of A", ParamKind::U64, true);
    add_param(s, alloc, "A", "General A flattened RowMajor (m*n)", ParamKind::F64, true);
}

} // namespace

namespace crd::hesap::dense
{
void register_svd_cli_anchor() noexcept
{
}
} // namespace crd::hesap::dense

CRD_HESAP_CLI_REGISTER_MODULE([](CommandRegistry& reg) {
    auto* alloc = crd::memory::default_allocator();
    {
        auto s = make_schema(alloc, "hesap.dense.svd.f32",
                             "Singular values (descending) of general A via Golub-Kahan + dbdsqr (f32).");
        add_matrix_params(s, alloc);
        reg.register_command(std::move(s), &impl_svd<crd::f32>);
    }
    {
        auto s = make_schema(alloc, "hesap.dense.svd.f64",
                             "Singular values (descending) of general A via Golub-Kahan + dbdsqr (f64).");
        add_matrix_params(s, alloc);
        reg.register_command(std::move(s), &impl_svd<crd::f64>);
    }
    {
        auto s = make_schema(alloc, "hesap.dense.svdvals.f32",
                             "Singular values only (descending) of general A via dqds (f32).");
        add_matrix_params(s, alloc);
        reg.register_command(std::move(s), &impl_svdvals<crd::f32>);
    }
    {
        auto s = make_schema(alloc, "hesap.dense.svdvals.f64",
                             "Singular values only (descending) of general A via dqds (f64).");
        add_matrix_params(s, alloc);
        reg.register_command(std::move(s), &impl_svdvals<crd::f64>);
    }
});
