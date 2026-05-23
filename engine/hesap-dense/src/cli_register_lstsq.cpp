// v3c-1b — CLI registration for the least-squares family.
//   hesap.dense.lstsq.{f32,f64,c32,c64} — min-norm solve of min‖A·x − b‖,
//       params (m, n, A=m*n RowMajor, b=m); returns the solution x (length n).
//   hesap.dense.pinv.{f32,f64,c32,c64} — Moore-Penrose pseudoinverse A⁺ (n×m),
//       params (m, n, A=m*n RowMajor); returns A⁺ flattened RowMajor (n*m).
// Complex matrices/vectors travel interleaved [re0,im0,re1,im1,...]; complex
// results are returned the same way.
//
// Anchor symbol: `register_lstsq_cli_anchor()` in `cli_anchor.hpp`.

#include <crd/hesap/cli/arg_value.hpp>
#include <crd/hesap/cli/command_registry.hpp>
#include <crd/hesap/complex.hpp>
#include <crd/hesap/dense/cli_anchor.hpp>
#include <crd/hesap/dense/lstsq.hpp>
#include <crd/hesap/dense/matrix.hpp>
#include <crd/hesap/dense/nnls.hpp>
#include <crd/hesap/dense/tls.hpp>
#include <crd/hesap/dense/vector.hpp>

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
using crd::hesap::Complex;
using crd::hesap::dense::lstsq;
using crd::hesap::dense::LstSqMethod;
using crd::hesap::dense::Matrix;
using crd::hesap::dense::nnls;
using crd::hesap::dense::pinv;
using crd::hesap::dense::tls;
using crd::hesap::dense::Vector;

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

// ---- real lstsq / pinv ------------------------------------------------

template <typename T>
CommandResult impl_lstsq(const CommandArgs& args)
{
    const auto a_flat = args.get_f64_array("A");
    const auto b_flat = args.get_f64_array("b");
    const auto m = args.get_u64("m").value_or(crd::u64{0});
    const auto n = args.get_u64("n").value_or(crd::u64{0});
    if (m == 0 || n == 0 || a_flat.size() != m * n || b_flat.size() != m)
    {
        return error_result(args.alloc, "lstsq: A=m*n (RowMajor) + b=m, m+n required");
    }
    const crd::usize mm = static_cast<crd::usize>(m);
    const crd::usize nn = static_cast<crd::usize>(n);
    Matrix<T> a(args.alloc, mm, nn);
    for (crd::usize i = 0; i < mm; ++i)
    {
        for (crd::usize j = 0; j < nn; ++j)
        {
            a.at(i, j) = static_cast<T>(a_flat[i * nn + j]);
        }
    }
    Vector<T> b(args.alloc, mm);
    for (crd::usize i = 0; i < mm; ++i)
    {
        b(i) = static_cast<T>(b_flat[i]);
    }
    const auto sol = lstsq<T>(args.alloc, a, b, LstSqMethod::Auto);
    crd::containers::Array<crd::f64> out(args.alloc);
    out.reserve(nn);
    for (crd::usize j = 0; j < nn; ++j)
    {
        out.push_back(static_cast<crd::f64>(sol.x.at(j, 0)));
    }
    return binary_result(args.alloc, crd::containers::ConstSpan<crd::f64>{out.data(), out.size()});
}

template <typename T>
CommandResult impl_pinv(const CommandArgs& args)
{
    const auto a_flat = args.get_f64_array("A");
    const auto m = args.get_u64("m").value_or(crd::u64{0});
    const auto n = args.get_u64("n").value_or(crd::u64{0});
    if (m == 0 || n == 0 || a_flat.size() != m * n)
    {
        return error_result(args.alloc, "pinv: A=m*n (RowMajor), m+n required");
    }
    const crd::usize mm = static_cast<crd::usize>(m);
    const crd::usize nn = static_cast<crd::usize>(n);
    Matrix<T> a(args.alloc, mm, nn);
    for (crd::usize i = 0; i < mm; ++i)
    {
        for (crd::usize j = 0; j < nn; ++j)
        {
            a.at(i, j) = static_cast<T>(a_flat[i * nn + j]);
        }
    }
    const auto p = pinv<T>(args.alloc, a);  // n x m
    crd::containers::Array<crd::f64> out(args.alloc);
    out.reserve(nn * mm);
    for (crd::usize j = 0; j < nn; ++j)
    {
        for (crd::usize i = 0; i < mm; ++i)
        {
            out.push_back(static_cast<crd::f64>(p.at(j, i)));
        }
    }
    return binary_result(args.alloc, crd::containers::ConstSpan<crd::f64>{out.data(), out.size()});
}

// ---- complex lstsq / pinv (interleaved [re,im]) -----------------------

template <typename U>
CommandResult impl_lstsq_complex(const CommandArgs& args)
{
    using Cx = Complex<U>;
    const auto a_flat = args.get_f64_array("A");
    const auto b_flat = args.get_f64_array("b");
    const auto m = args.get_u64("m").value_or(crd::u64{0});
    const auto n = args.get_u64("n").value_or(crd::u64{0});
    if (m == 0 || n == 0 || a_flat.size() != 2 * m * n || b_flat.size() != 2 * m)
    {
        return error_result(args.alloc, "lstsq (complex): A=2*m*n + b=2*m interleaved, m+n required");
    }
    const crd::usize mm = static_cast<crd::usize>(m);
    const crd::usize nn = static_cast<crd::usize>(n);
    Matrix<Cx> a(args.alloc, mm, nn);
    for (crd::usize i = 0; i < mm; ++i)
    {
        for (crd::usize j = 0; j < nn; ++j)
        {
            const crd::usize k = 2 * (i * nn + j);
            a.at(i, j) = Cx{static_cast<U>(a_flat[k]), static_cast<U>(a_flat[k + 1])};
        }
    }
    Vector<Cx> b(args.alloc, mm);
    for (crd::usize i = 0; i < mm; ++i)
    {
        b(i) = Cx{static_cast<U>(b_flat[2 * i]), static_cast<U>(b_flat[2 * i + 1])};
    }
    const auto sol = lstsq<Cx>(args.alloc, a, b, LstSqMethod::Auto);
    crd::containers::Array<crd::f64> out(args.alloc);
    out.reserve(2 * nn);
    for (crd::usize j = 0; j < nn; ++j)
    {
        out.push_back(static_cast<crd::f64>(sol.x.at(j, 0).re));
        out.push_back(static_cast<crd::f64>(sol.x.at(j, 0).im));
    }
    return binary_result(args.alloc, crd::containers::ConstSpan<crd::f64>{out.data(), out.size()});
}

template <typename U>
CommandResult impl_pinv_complex(const CommandArgs& args)
{
    using Cx = Complex<U>;
    const auto a_flat = args.get_f64_array("A");
    const auto m = args.get_u64("m").value_or(crd::u64{0});
    const auto n = args.get_u64("n").value_or(crd::u64{0});
    if (m == 0 || n == 0 || a_flat.size() != 2 * m * n)
    {
        return error_result(args.alloc, "pinv (complex): A=2*m*n interleaved, m+n required");
    }
    const crd::usize mm = static_cast<crd::usize>(m);
    const crd::usize nn = static_cast<crd::usize>(n);
    Matrix<Cx> a(args.alloc, mm, nn);
    for (crd::usize i = 0; i < mm; ++i)
    {
        for (crd::usize j = 0; j < nn; ++j)
        {
            const crd::usize k = 2 * (i * nn + j);
            a.at(i, j) = Cx{static_cast<U>(a_flat[k]), static_cast<U>(a_flat[k + 1])};
        }
    }
    const auto p = pinv<Cx>(args.alloc, a);  // n x m
    crd::containers::Array<crd::f64> out(args.alloc);
    out.reserve(2 * nn * mm);
    for (crd::usize j = 0; j < nn; ++j)
    {
        for (crd::usize i = 0; i < mm; ++i)
        {
            out.push_back(static_cast<crd::f64>(p.at(j, i).re));
            out.push_back(static_cast<crd::f64>(p.at(j, i).im));
        }
    }
    return binary_result(args.alloc, crd::containers::ConstSpan<crd::f64>{out.data(), out.size()});
}

// ---- NNLS (real) ------------------------------------------------------

template <typename T>
CommandResult impl_nnls(const CommandArgs& args)
{
    const auto a_flat = args.get_f64_array("A");
    const auto b_flat = args.get_f64_array("b");
    const auto m = args.get_u64("m").value_or(crd::u64{0});
    const auto n = args.get_u64("n").value_or(crd::u64{0});
    if (m == 0 || n == 0 || a_flat.size() != m * n || b_flat.size() != m)
    {
        return error_result(args.alloc, "nnls: A=m*n (RowMajor) + b=m, m+n required");
    }
    const crd::usize mm = static_cast<crd::usize>(m);
    const crd::usize nn = static_cast<crd::usize>(n);
    Matrix<T> a(args.alloc, mm, nn);
    for (crd::usize i = 0; i < mm; ++i)
    {
        for (crd::usize j = 0; j < nn; ++j)
        {
            a.at(i, j) = static_cast<T>(a_flat[i * nn + j]);
        }
    }
    Vector<T> b(args.alloc, mm);
    for (crd::usize i = 0; i < mm; ++i)
    {
        b(i) = static_cast<T>(b_flat[i]);
    }
    const auto r = nnls<T>(args.alloc, a, b);
    if (!r.converged)
    {
        return error_result(args.alloc, "nnls: did not converge within iteration cap");
    }
    crd::containers::Array<crd::f64> out(args.alloc);
    out.reserve(nn);
    for (crd::usize j = 0; j < nn; ++j)
    {
        out.push_back(static_cast<crd::f64>(r.x(j)));
    }
    return binary_result(args.alloc, crd::containers::ConstSpan<crd::f64>{out.data(), out.size()});
}

// ---- TLS (real + complex; single RHS) ---------------------------------

template <typename T>
CommandResult impl_tls(const CommandArgs& args)
{
    const auto a_flat = args.get_f64_array("A");
    const auto b_flat = args.get_f64_array("b");
    const auto m = args.get_u64("m").value_or(crd::u64{0});
    const auto n = args.get_u64("n").value_or(crd::u64{0});
    if (m == 0 || n == 0 || a_flat.size() != m * n || b_flat.size() != m)
    {
        return error_result(args.alloc, "tls: A=m*n (RowMajor) + b=m, m+n required");
    }
    const crd::usize mm = static_cast<crd::usize>(m);
    const crd::usize nn = static_cast<crd::usize>(n);
    Matrix<T> a(args.alloc, mm, nn);
    for (crd::usize i = 0; i < mm; ++i)
    {
        for (crd::usize j = 0; j < nn; ++j)
        {
            a.at(i, j) = static_cast<T>(a_flat[i * nn + j]);
        }
    }
    Vector<T> b(args.alloc, mm);
    for (crd::usize i = 0; i < mm; ++i)
    {
        b(i) = static_cast<T>(b_flat[i]);
    }
    const auto r = tls<T>(args.alloc, a, b);
    if (!r.exists)
    {
        return error_result(args.alloc, "tls: solution does not exist/unique (V22 singular)");
    }
    crd::containers::Array<crd::f64> out(args.alloc);
    out.reserve(nn);
    for (crd::usize j = 0; j < nn; ++j)
    {
        out.push_back(static_cast<crd::f64>(r.x.at(j, 0)));
    }
    return binary_result(args.alloc, crd::containers::ConstSpan<crd::f64>{out.data(), out.size()});
}

template <typename U>
CommandResult impl_tls_complex(const CommandArgs& args)
{
    using Cx = Complex<U>;
    const auto a_flat = args.get_f64_array("A");
    const auto b_flat = args.get_f64_array("b");
    const auto m = args.get_u64("m").value_or(crd::u64{0});
    const auto n = args.get_u64("n").value_or(crd::u64{0});
    if (m == 0 || n == 0 || a_flat.size() != 2 * m * n || b_flat.size() != 2 * m)
    {
        return error_result(args.alloc, "tls (complex): A=2*m*n + b=2*m interleaved, m+n required");
    }
    const crd::usize mm = static_cast<crd::usize>(m);
    const crd::usize nn = static_cast<crd::usize>(n);
    Matrix<Cx> a(args.alloc, mm, nn);
    for (crd::usize i = 0; i < mm; ++i)
    {
        for (crd::usize j = 0; j < nn; ++j)
        {
            const crd::usize k = 2 * (i * nn + j);
            a.at(i, j) = Cx{static_cast<U>(a_flat[k]), static_cast<U>(a_flat[k + 1])};
        }
    }
    Vector<Cx> b(args.alloc, mm);
    for (crd::usize i = 0; i < mm; ++i)
    {
        b(i) = Cx{static_cast<U>(b_flat[2 * i]), static_cast<U>(b_flat[2 * i + 1])};
    }
    const auto r = tls<Cx>(args.alloc, a, b);
    if (!r.exists)
    {
        return error_result(args.alloc, "tls (complex): solution does not exist/unique");
    }
    crd::containers::Array<crd::f64> out(args.alloc);
    out.reserve(2 * nn);
    for (crd::usize j = 0; j < nn; ++j)
    {
        out.push_back(static_cast<crd::f64>(r.x.at(j, 0).re));
        out.push_back(static_cast<crd::f64>(r.x.at(j, 0).im));
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

} // namespace

namespace crd::hesap::dense
{
void register_lstsq_cli_anchor() noexcept
{
}
} // namespace crd::hesap::dense

CRD_HESAP_CLI_REGISTER_MODULE([](CommandRegistry& reg) {
    auto* alloc = crd::memory::default_allocator();
    {
        auto s = make_schema(alloc, "hesap.dense.lstsq.f32",
                             "Least-squares min-norm solve of min||A*x - b|| (f32).");
        add_param(s, alloc, "m", "Rows of A", ParamKind::U64, true);
        add_param(s, alloc, "n", "Columns of A", ParamKind::U64, true);
        add_param(s, alloc, "A", "A flattened RowMajor (m*n)", ParamKind::F64, true);
        add_param(s, alloc, "b", "RHS vector (m)", ParamKind::F64, true);
        reg.register_command(std::move(s), &impl_lstsq<crd::f32>);
    }
    {
        auto s = make_schema(alloc, "hesap.dense.lstsq.f64",
                             "Least-squares min-norm solve of min||A*x - b|| (f64).");
        add_param(s, alloc, "m", "Rows of A", ParamKind::U64, true);
        add_param(s, alloc, "n", "Columns of A", ParamKind::U64, true);
        add_param(s, alloc, "A", "A flattened RowMajor (m*n)", ParamKind::F64, true);
        add_param(s, alloc, "b", "RHS vector (m)", ParamKind::F64, true);
        reg.register_command(std::move(s), &impl_lstsq<crd::f64>);
    }
    {
        auto s = make_schema(alloc, "hesap.dense.pinv.f32",
                             "Moore-Penrose pseudoinverse A+ (n x m) via SVD (f32).");
        add_param(s, alloc, "m", "Rows of A", ParamKind::U64, true);
        add_param(s, alloc, "n", "Columns of A", ParamKind::U64, true);
        add_param(s, alloc, "A", "A flattened RowMajor (m*n)", ParamKind::F64, true);
        reg.register_command(std::move(s), &impl_pinv<crd::f32>);
    }
    {
        auto s = make_schema(alloc, "hesap.dense.pinv.f64",
                             "Moore-Penrose pseudoinverse A+ (n x m) via SVD (f64).");
        add_param(s, alloc, "m", "Rows of A", ParamKind::U64, true);
        add_param(s, alloc, "n", "Columns of A", ParamKind::U64, true);
        add_param(s, alloc, "A", "A flattened RowMajor (m*n)", ParamKind::F64, true);
        reg.register_command(std::move(s), &impl_pinv<crd::f64>);
    }
    {
        auto s = make_schema(alloc, "hesap.dense.lstsq.c32",
                             "Least-squares min-norm solve (Complex<f32>, interleaved).");
        add_param(s, alloc, "m", "Rows of A", ParamKind::U64, true);
        add_param(s, alloc, "n", "Columns of A", ParamKind::U64, true);
        add_param(s, alloc, "A", "Complex A interleaved [re,im] (2*m*n) RowMajor", ParamKind::F64, true);
        add_param(s, alloc, "b", "Complex RHS interleaved [re,im] (2*m)", ParamKind::F64, true);
        reg.register_command(std::move(s), &impl_lstsq_complex<crd::f32>);
    }
    {
        auto s = make_schema(alloc, "hesap.dense.lstsq.c64",
                             "Least-squares min-norm solve (Complex<f64>, interleaved).");
        add_param(s, alloc, "m", "Rows of A", ParamKind::U64, true);
        add_param(s, alloc, "n", "Columns of A", ParamKind::U64, true);
        add_param(s, alloc, "A", "Complex A interleaved [re,im] (2*m*n) RowMajor", ParamKind::F64, true);
        add_param(s, alloc, "b", "Complex RHS interleaved [re,im] (2*m)", ParamKind::F64, true);
        reg.register_command(std::move(s), &impl_lstsq_complex<crd::f64>);
    }
    {
        auto s = make_schema(alloc, "hesap.dense.pinv.c32",
                             "Moore-Penrose pseudoinverse A+ (Complex<f32>, interleaved).");
        add_param(s, alloc, "m", "Rows of A", ParamKind::U64, true);
        add_param(s, alloc, "n", "Columns of A", ParamKind::U64, true);
        add_param(s, alloc, "A", "Complex A interleaved [re,im] (2*m*n) RowMajor", ParamKind::F64, true);
        reg.register_command(std::move(s), &impl_pinv_complex<crd::f32>);
    }
    {
        auto s = make_schema(alloc, "hesap.dense.pinv.c64",
                             "Moore-Penrose pseudoinverse A+ (Complex<f64>, interleaved).");
        add_param(s, alloc, "m", "Rows of A", ParamKind::U64, true);
        add_param(s, alloc, "n", "Columns of A", ParamKind::U64, true);
        add_param(s, alloc, "A", "Complex A interleaved [re,im] (2*m*n) RowMajor", ParamKind::F64, true);
        reg.register_command(std::move(s), &impl_pinv_complex<crd::f64>);
    }
    {
        auto s = make_schema(alloc, "hesap.dense.nnls.f32",
                             "Non-negative least squares min||A*x-b|| s.t. x>=0 (Lawson-Hanson, f32).");
        add_param(s, alloc, "m", "Rows of A", ParamKind::U64, true);
        add_param(s, alloc, "n", "Columns of A", ParamKind::U64, true);
        add_param(s, alloc, "A", "A flattened RowMajor (m*n)", ParamKind::F64, true);
        add_param(s, alloc, "b", "RHS vector (m)", ParamKind::F64, true);
        reg.register_command(std::move(s), &impl_nnls<crd::f32>);
    }
    {
        auto s = make_schema(alloc, "hesap.dense.nnls.f64",
                             "Non-negative least squares min||A*x-b|| s.t. x>=0 (Lawson-Hanson, f64).");
        add_param(s, alloc, "m", "Rows of A", ParamKind::U64, true);
        add_param(s, alloc, "n", "Columns of A", ParamKind::U64, true);
        add_param(s, alloc, "A", "A flattened RowMajor (m*n)", ParamKind::F64, true);
        add_param(s, alloc, "b", "RHS vector (m)", ParamKind::F64, true);
        reg.register_command(std::move(s), &impl_nnls<crd::f64>);
    }
    {
        auto s = make_schema(alloc, "hesap.dense.tls.f32",
                             "Total least squares via SVD of [A|b] (errors in A and b, f32).");
        add_param(s, alloc, "m", "Rows of A", ParamKind::U64, true);
        add_param(s, alloc, "n", "Columns of A", ParamKind::U64, true);
        add_param(s, alloc, "A", "A flattened RowMajor (m*n)", ParamKind::F64, true);
        add_param(s, alloc, "b", "RHS vector (m)", ParamKind::F64, true);
        reg.register_command(std::move(s), &impl_tls<crd::f32>);
    }
    {
        auto s = make_schema(alloc, "hesap.dense.tls.f64",
                             "Total least squares via SVD of [A|b] (errors in A and b, f64).");
        add_param(s, alloc, "m", "Rows of A", ParamKind::U64, true);
        add_param(s, alloc, "n", "Columns of A", ParamKind::U64, true);
        add_param(s, alloc, "A", "A flattened RowMajor (m*n)", ParamKind::F64, true);
        add_param(s, alloc, "b", "RHS vector (m)", ParamKind::F64, true);
        reg.register_command(std::move(s), &impl_tls<crd::f64>);
    }
    {
        auto s = make_schema(alloc, "hesap.dense.tls.c32",
                             "Total least squares (Complex<f32>, interleaved).");
        add_param(s, alloc, "m", "Rows of A", ParamKind::U64, true);
        add_param(s, alloc, "n", "Columns of A", ParamKind::U64, true);
        add_param(s, alloc, "A", "Complex A interleaved [re,im] (2*m*n) RowMajor", ParamKind::F64, true);
        add_param(s, alloc, "b", "Complex RHS interleaved [re,im] (2*m)", ParamKind::F64, true);
        reg.register_command(std::move(s), &impl_tls_complex<crd::f32>);
    }
    {
        auto s = make_schema(alloc, "hesap.dense.tls.c64",
                             "Total least squares (Complex<f64>, interleaved).");
        add_param(s, alloc, "m", "Rows of A", ParamKind::U64, true);
        add_param(s, alloc, "n", "Columns of A", ParamKind::U64, true);
        add_param(s, alloc, "A", "Complex A interleaved [re,im] (2*m*n) RowMajor", ParamKind::F64, true);
        add_param(s, alloc, "b", "Complex RHS interleaved [re,im] (2*m)", ParamKind::F64, true);
        reg.register_command(std::move(s), &impl_tls_complex<crd::f64>);
    }
});
