// v6-z — CLI registration for the sparse eigensolvers (hesap.eigen.*). Matrix in as COO triplets (full
// symmetric matrix; the operator uses it as A x), eigenpairs out as a flat f64 blob.
//
//   hesap.eigen.sym.{f32,f64}        : extreme symmetric eigenvalues (thick-restart Lanczos). Params + nev +
//                                      which (0=smallest, 1=largest). Out [nconv, values..., residuals...].
//   hesap.eigen.svds.{f32,f64}       : largest singular values of an m x n matrix (IRLBA). Params + nsv.
//                                      Out [nconv, sigma..., residuals...].
//   hesap.eigen.shift_invert.f64     : interior eigenvalues nearest a shift sigma (v5 LU factor + Lanczos).
//                                      Params + sigma + nev. Out [nconv, values..., residuals...].
//   hesap.eigen.feast.f64            : all eigenvalues in an interval [lo, hi] (FEAST contour integration).
//                                      Params + lo + hi + m0. Out [nconv, values..., residuals...].
//
// Output blob layout: out[0] = nconv; then k = (len-1)/2 eigenvalues; then k residuals (k = #returned, >= nconv).
// Eigenvalues are real (these are the symmetric / SVD families). Anchor: register_eigen_cli_anchor().

#include <crd/hesap/cli/arg_value.hpp>
#include <crd/hesap/cli/command_registry.hpp>
#include <crd/hesap/complex.hpp>
#include <crd/hesap/dense/real_type.hpp>
#include <crd/hesap/eigen/eigen.hpp>
#include <crd/hesap/sparse/sparse_linear_op.hpp>
#include <crd/hesap/sparse/triplet_builder.hpp>

#include <utility>

namespace
{
using namespace crd::hesap;
using namespace crd::hesap::cli;
using crd::hesap::dense::RealType;

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

// Add the shared matrix params (rows/cols/triplets/values, f64-real only — these families are real).
void add_matrix_params(CommandSchema& s, crd::memory::IAllocator* alloc)
{
    add_param(s, alloc, "rows", "Matrix rows", ParamKind::U64, true);
    add_param(s, alloc, "cols", "Matrix columns", ParamKind::U64, true);
    add_param(s, alloc, "triplet_rows", "COO row indices (I64Array)", ParamKind::I64, true);
    add_param(s, alloc, "triplet_cols", "COO column indices (I64Array)", ParamKind::I64, true);
    add_param(s, alloc, "values", "COO values (F64Array)", ParamKind::F64, true);
}

// Validate the matrix params (rows/cols + triplet/value sizes). `square` requires rows==cols. Returns false
// (+ fills `err`) on a malformed request; otherwise sets m/n.
bool validate_matrix(const CommandArgs& args, bool square, crd::u32& m, crd::u32& n, const char*& err)
{
    const auto rows = args.get_u64("rows");
    const auto cols = args.get_u64("cols");
    if (!rows || !cols || (square && *rows != *cols))
    {
        err = square ? "rows and cols are required and must be equal (square)" : "rows and cols are required";
        return false;
    }
    const auto tr = args.get_i64_array("triplet_rows");
    const auto tc = args.get_i64_array("triplet_cols");
    const auto vals = args.get_f64_array("values");
    if (tr.size() != tc.size() || vals.size() != tr.size())
    {
        err = "triplet_rows/cols length mismatch, or values != nnz (real f64 only)";
        return false;
    }
    m = static_cast<crd::u32>(*rows);
    n = static_cast<crd::u32>(*cols);
    return true;
}

// Build the CSR<T> from the (already-validated) triplet/value params (real values only). Returned by value.
template <typename T>
crd::hesap::sparse::SparseMatrix<T, crd::hesap::sparse::SparseFormat::Csr>
build_csr(const CommandArgs& args, crd::u32 m, crd::u32 n)
{
    const auto tr = args.get_i64_array("triplet_rows");
    const auto tc = args.get_i64_array("triplet_cols");
    const auto vals = args.get_f64_array("values");
    crd::hesap::sparse::TripletBuilder<T> tb(args.alloc, m, n);
    tb.reserve(tr.size());
    for (crd::usize k = 0; k < tr.size(); ++k)
    {
        tb.add(static_cast<crd::u32>(tr[k]), static_cast<crd::u32>(tc[k]), static_cast<T>(vals[k]));
    }
    return tb.compress();
}

// Emit [nconv, values..., residuals...] from real eigenvalues + residuals.
template <typename R>
CommandResult emit_eig(crd::memory::IAllocator* alloc, crd::u32 nconv, crd::containers::ConstSpan<R> values,
                       crd::containers::ConstSpan<R> residuals)
{
    crd::containers::Array<crd::f64> out(alloc);
    out.reserve(static_cast<crd::usize>(values.size()) * 2 + 1);
    out.push_back(static_cast<crd::f64>(nconv));
    for (crd::usize i = 0; i < values.size(); ++i)
    {
        out.push_back(static_cast<crd::f64>(values[i]));
    }
    for (crd::usize i = 0; i < residuals.size(); ++i)
    {
        out.push_back(static_cast<crd::f64>(residuals[i]));
    }
    return blob_f64_result(alloc, crd::containers::ConstSpan<crd::f64>{out.data(), out.size()});
}

template <typename T> CommandResult impl_sym(const CommandArgs& args)
{
    using R = RealType<T>;
    crd::u32 m = 0;
    crd::u32 n = 0;
    const char* err = nullptr;
    if (!validate_matrix(args, /*square=*/true, m, n, err))
    {
        return error_result(args.alloc, err);
    }
    auto a = build_csr<T>(args, m, n);
    eigen::EigenOptions<T> opts;
    opts.nev = static_cast<crd::u32>(args.get_u64("nev").value_or(1));
    opts.which =
        args.get_u64("which").value_or(0) == 1 ? eigen::Which::LargestAlgebraic : eigen::Which::SmallestAlgebraic;
    crd::hesap::sparse::SparseLinearOp<T> op(a);
    auto r = eigen::eigs_sym_tr<T>(op, opts, args.alloc);

    crd::containers::Array<R> vals(args.alloc);
    crd::containers::Array<R> res(args.alloc);
    for (crd::u32 i = 0; i < r.values.size(); ++i)
    {
        vals.push_back(r.values[i].re); // symmetric -> real
    }
    for (crd::u32 i = 0; i < r.residuals.size(); ++i)
    {
        res.push_back(r.residuals[i]);
    }
    return emit_eig<R>(args.alloc, r.nconv, {vals.data(), vals.size()}, {res.data(), res.size()});
}

template <typename T> CommandResult impl_svds(const CommandArgs& args)
{
    using R = RealType<T>;
    crd::u32 m = 0;
    crd::u32 n = 0;
    const char* err = nullptr;
    if (!validate_matrix(args, /*square=*/false, m, n, err))
    {
        return error_result(args.alloc, err);
    }
    auto a = build_csr<T>(args, m, n);
    eigen::EigenOptions<T> opts;
    opts.nev = static_cast<crd::u32>(args.get_u64("nsv").value_or(1));
    crd::hesap::sparse::SparseLinearOp<T> op(a);
    auto r = eigen::svds<T>(op, opts, args.alloc);
    return emit_eig<R>(args.alloc, r.nconv, {r.values.data(), r.values.size()},
                       {r.residuals.data(), r.residuals.size()});
}

CommandResult impl_shift_invert(const CommandArgs& args)
{
    crd::u32 m = 0;
    crd::u32 n = 0;
    const char* err = nullptr;
    if (!validate_matrix(args, /*square=*/true, m, n, err))
    {
        return error_result(args.alloc, err);
    }
    auto a = build_csr<crd::f64>(args, m, n);
    const auto sigma = args.get_f64("sigma");
    if (!sigma)
    {
        return error_result(args.alloc, "shift_invert: sigma (the shift) is required");
    }
    eigen::EigenOptions<crd::f64> opts;
    opts.nev = static_cast<crd::u32>(args.get_u64("nev").value_or(1));
    auto r = eigen::eigs_sym_shift_invert<crd::f64>(a, *sigma, opts, args.alloc);
    crd::containers::Array<crd::f64> vals(args.alloc);
    for (crd::u32 i = 0; i < r.values.size(); ++i)
    {
        vals.push_back(r.values[i].re);
    }
    return emit_eig<crd::f64>(args.alloc, r.nconv, {vals.data(), vals.size()},
                              {r.residuals.data(), r.residuals.size()});
}

CommandResult impl_feast(const CommandArgs& args)
{
    crd::u32 m = 0;
    crd::u32 n = 0;
    const char* err = nullptr;
    if (!validate_matrix(args, /*square=*/true, m, n, err))
    {
        return error_result(args.alloc, err);
    }
    auto a = build_csr<crd::f64>(args, m, n);
    const auto lo = args.get_f64("lo");
    const auto hi = args.get_f64("hi");
    if (!lo || !hi || !(*hi > *lo))
    {
        return error_result(args.alloc, "feast: lo and hi are required with hi > lo (the interval)");
    }
    const crd::u32 m0 = static_cast<crd::u32>(args.get_u64("m0").value_or(8));
    eigen::EigenOptions<crd::f64> opts;
    auto r = eigen::eigs_sym_feast<crd::f64>(a, *lo, *hi, m0, opts, args.alloc);
    crd::containers::Array<crd::f64> vals(args.alloc);
    for (crd::u32 i = 0; i < r.values.size(); ++i)
    {
        vals.push_back(r.values[i].re);
    }
    return emit_eig<crd::f64>(args.alloc, r.nconv, {vals.data(), vals.size()},
                              {r.residuals.data(), r.residuals.size()});
}

CommandSchema make_sym_schema(crd::memory::IAllocator* alloc, const char* name, const char* desc)
{
    CommandSchema s{alloc};
    s.name = crd::containers::String{name, alloc};
    s.description = crd::containers::String{desc, alloc};
    s.output.kind = OutputKind::BinaryBlob;
    s.required_caps.bits = Capability::kHesapCompute;
    s.idempotent = true;
    add_matrix_params(s, alloc);
    add_param(s, alloc, "nev", "Number of eigenpairs (default 1)", ParamKind::U64, false);
    add_param(s, alloc, "which", "0 = smallest, 1 = largest (default 0)", ParamKind::U64, false);
    return s;
}

CommandSchema make_svds_schema(crd::memory::IAllocator* alloc, const char* name, const char* desc)
{
    CommandSchema s{alloc};
    s.name = crd::containers::String{name, alloc};
    s.description = crd::containers::String{desc, alloc};
    s.output.kind = OutputKind::BinaryBlob;
    s.required_caps.bits = Capability::kHesapCompute;
    s.idempotent = true;
    add_matrix_params(s, alloc);
    add_param(s, alloc, "nsv", "Number of singular triplets (default 1)", ParamKind::U64, false);
    return s;
}

CommandSchema make_shift_invert_schema(crd::memory::IAllocator* alloc)
{
    CommandSchema s{alloc};
    s.name = crd::containers::String{"hesap.eigen.shift_invert.f64", alloc};
    s.description = crd::containers::String{
        "Interior symmetric eigenvalues nearest a shift sigma (f64; v5 LU factor + thick-restart Lanczos). "
        "Returns [nconv, values..., residuals...].",
        alloc};
    s.output.kind = OutputKind::BinaryBlob;
    s.required_caps.bits = Capability::kHesapCompute;
    s.idempotent = true;
    add_matrix_params(s, alloc);
    add_param(s, alloc, "sigma", "The shift (eigenvalues nearest sigma are returned)", ParamKind::F64, true);
    add_param(s, alloc, "nev", "Number of eigenpairs (default 1)", ParamKind::U64, false);
    return s;
}

CommandSchema make_feast_schema(crd::memory::IAllocator* alloc)
{
    CommandSchema s{alloc};
    s.name = crd::containers::String{"hesap.eigen.feast.f64", alloc};
    s.description = crd::containers::String{
        "All symmetric eigenvalues in an interval [lo, hi] (f64; FEAST contour integration). m0 = subspace size "
        ">= count in [lo,hi]. Returns [nconv, values..., residuals...].",
        alloc};
    s.output.kind = OutputKind::BinaryBlob;
    s.required_caps.bits = Capability::kHesapCompute;
    s.idempotent = true;
    add_matrix_params(s, alloc);
    add_param(s, alloc, "lo", "Interval lower bound", ParamKind::F64, true);
    add_param(s, alloc, "hi", "Interval upper bound (> lo)", ParamKind::F64, true);
    add_param(s, alloc, "m0", "Subspace size, >= the count in [lo,hi] (default 8)", ParamKind::U64, false);
    return s;
}
} // namespace

namespace crd::hesap::eigen
{
void register_eigen_cli_anchor() noexcept {}
} // namespace crd::hesap::eigen

// NOLINTNEXTLINE(bugprone-throwing-static-initialization)
CRD_HESAP_CLI_REGISTER_MODULE(
    [](CommandRegistry& reg)
    {
        auto* alloc = crd::memory::default_allocator();
        reg.register_command(
            make_sym_schema(alloc, "hesap.eigen.sym.f32",
                            "Extreme symmetric eigenvalues A x = lambda x (f32; thick-restart Lanczos). "
                            "Returns [nconv, values..., residuals...]."),
            &impl_sym<crd::f32>);
        reg.register_command(
            make_sym_schema(alloc, "hesap.eigen.sym.f64",
                            "Extreme symmetric eigenvalues A x = lambda x (f64; thick-restart Lanczos). "
                            "Returns [nconv, values..., residuals...]."),
            &impl_sym<crd::f64>);
        reg.register_command(
            make_svds_schema(alloc, "hesap.eigen.svds.f32",
                             "Largest singular values of an m x n matrix (f32; IRLBA bidiagonalization). "
                             "Returns [nconv, sigma..., residuals...]."),
            &impl_svds<crd::f32>);
        reg.register_command(
            make_svds_schema(alloc, "hesap.eigen.svds.f64",
                             "Largest singular values of an m x n matrix (f64; IRLBA bidiagonalization). "
                             "Returns [nconv, sigma..., residuals...]."),
            &impl_svds<crd::f64>);
        reg.register_command(make_shift_invert_schema(alloc), &impl_shift_invert);
        reg.register_command(make_feast_schema(alloc), &impl_feast);
    });
