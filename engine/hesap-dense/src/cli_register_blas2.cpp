// BLAS L2 CLI command registration. Phase 3.1.6 v0c.

#include <crd/hesap/cli/arg_value.hpp>
#include <crd/hesap/cli/command_registry.hpp>
#include <crd/hesap/dense/blas2.hpp>
#include <crd/hesap/dense/cli_anchor.hpp>
#include <crd/hesap/dense/matrix.hpp>
#include <crd/hesap/dense/matrix_types.hpp>
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
using crd::hesap::dense::Layout;
using crd::hesap::dense::Trans;
using crd::hesap::dense::TriangularDiag;
using crd::hesap::dense::TriangularSide;

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

// ---- gemv impls ------------------------------------------------------

CommandResult impl_gemv_f32(const CommandArgs& args)
{
    using T = crd::f32;
    using Mat = crd::hesap::dense::Matrix<T>;
    using Vec = crd::hesap::dense::Vector<T>;
    const auto a_flat = args.get_f64_array("A");
    const auto x_arr = args.get_f64_array("x");
    const auto y_in = args.get_f64_array("y");
    const auto rows = args.get_u64("rows").value_or(crd::u64{0});
    const auto cols = args.get_u64("cols").value_or(crd::u64{0});
    const T alpha = static_cast<T>(args.get_f64("alpha").value_or(crd::f64{1.0}));
    const T beta = static_cast<T>(args.get_f64("beta").value_or(crd::f64{0.0}));
    if (rows == 0 || cols == 0 || a_flat.size() != rows * cols ||
        x_arr.size() != cols || y_in.size() != rows)
    {
        return error_result(args.alloc, "gemv: size mismatch");
    }
    const crd::usize rn = static_cast<crd::usize>(rows);
    const crd::usize cn = static_cast<crd::usize>(cols);
    Mat a_mat(args.alloc, rn, cn);
    for (crd::usize i = 0; i < rn; ++i)
    {
        for (crd::usize j = 0; j < cn; ++j)
        {
            a_mat(i, j) = static_cast<T>(a_flat[i * cn + j]);
        }
    }
    Vec x(args.alloc, cn);
    Vec y(args.alloc, rn);
    for (crd::usize i = 0; i < cn; ++i) x(i) = static_cast<T>(x_arr[i]);
    for (crd::usize i = 0; i < rn; ++i) y(i) = static_cast<T>(y_in[i]);
    crd::hesap::dense::gemv<T, Layout::RowMajor>(alpha, a_mat.cview(), x.span(), beta, y.span(), Trans::None);
    crd::containers::Array<crd::f64> out(args.alloc);
    out.reserve(rn);
    for (crd::usize i = 0; i < rn; ++i) out.push_back(static_cast<crd::f64>(y(i)));
    return binary_result(args.alloc, crd::containers::ConstSpan<crd::f64>{out.data(), out.size()});
}

CommandResult impl_gemv_f64(const CommandArgs& args)
{
    using T = crd::f64;
    using Mat = crd::hesap::dense::Matrix<T>;
    using Vec = crd::hesap::dense::Vector<T>;
    const auto a_flat = args.get_f64_array("A");
    const auto x_arr = args.get_f64_array("x");
    const auto y_in = args.get_f64_array("y");
    const auto rows = args.get_u64("rows").value_or(crd::u64{0});
    const auto cols = args.get_u64("cols").value_or(crd::u64{0});
    const T alpha = static_cast<T>(args.get_f64("alpha").value_or(crd::f64{1.0}));
    const T beta = static_cast<T>(args.get_f64("beta").value_or(crd::f64{0.0}));
    if (rows == 0 || cols == 0 || a_flat.size() != rows * cols ||
        x_arr.size() != cols || y_in.size() != rows)
    {
        return error_result(args.alloc, "gemv: size mismatch");
    }
    const crd::usize rn = static_cast<crd::usize>(rows);
    const crd::usize cn = static_cast<crd::usize>(cols);
    Mat a_mat(args.alloc, rn, cn);
    for (crd::usize i = 0; i < rn; ++i)
    {
        for (crd::usize j = 0; j < cn; ++j)
        {
            a_mat(i, j) = static_cast<T>(a_flat[i * cn + j]);
        }
    }
    Vec x(args.alloc, cn);
    Vec y(args.alloc, rn);
    for (crd::usize i = 0; i < cn; ++i) x(i) = static_cast<T>(x_arr[i]);
    for (crd::usize i = 0; i < rn; ++i) y(i) = static_cast<T>(y_in[i]);
    crd::hesap::dense::gemv<T, Layout::RowMajor>(alpha, a_mat.cview(), x.span(), beta, y.span(), Trans::None);
    crd::containers::Array<crd::f64> out(args.alloc);
    out.reserve(rn);
    for (crd::usize i = 0; i < rn; ++i) out.push_back(static_cast<crd::f64>(y(i)));
    return binary_result(args.alloc, crd::containers::ConstSpan<crd::f64>{out.data(), out.size()});
}

// ---- symv impls ------------------------------------------------------

CommandResult impl_symv_f32(const CommandArgs& args)
{
    using T = crd::f32;
    using Sym = crd::hesap::dense::Symmetric<T>;
    using Vec = crd::hesap::dense::Vector<T>;
    const auto a_flat = args.get_f64_array("A");
    const auto x_arr = args.get_f64_array("x");
    const auto y_in = args.get_f64_array("y");
    const auto n = args.get_u64("n").value_or(crd::u64{0});
    const T alpha = static_cast<T>(args.get_f64("alpha").value_or(crd::f64{1.0}));
    const T beta = static_cast<T>(args.get_f64("beta").value_or(crd::f64{0.0}));
    if (n == 0 || a_flat.size() != n * n || x_arr.size() != n || y_in.size() != n)
    {
        return error_result(args.alloc, "symv: size mismatch");
    }
    const crd::usize nn = static_cast<crd::usize>(n);
    Sym a_sym(args.alloc, nn);
    for (crd::usize i = 0; i < nn; ++i)
        for (crd::usize j = 0; j <= i; ++j)
            a_sym.at(i, j) = static_cast<T>(a_flat[i * nn + j]);
    Vec x(args.alloc, nn);
    Vec y(args.alloc, nn);
    for (crd::usize i = 0; i < nn; ++i) x(i) = static_cast<T>(x_arr[i]);
    for (crd::usize i = 0; i < nn; ++i) y(i) = static_cast<T>(y_in[i]);
    crd::hesap::dense::symv<T>(alpha, a_sym, x.span(), beta, y.span());
    crd::containers::Array<crd::f64> out(args.alloc);
    out.reserve(nn);
    for (crd::usize i = 0; i < nn; ++i) out.push_back(static_cast<crd::f64>(y(i)));
    return binary_result(args.alloc, crd::containers::ConstSpan<crd::f64>{out.data(), out.size()});
}

CommandResult impl_symv_f64(const CommandArgs& args)
{
    using T = crd::f64;
    using Sym = crd::hesap::dense::Symmetric<T>;
    using Vec = crd::hesap::dense::Vector<T>;
    const auto a_flat = args.get_f64_array("A");
    const auto x_arr = args.get_f64_array("x");
    const auto y_in = args.get_f64_array("y");
    const auto n = args.get_u64("n").value_or(crd::u64{0});
    const T alpha = static_cast<T>(args.get_f64("alpha").value_or(crd::f64{1.0}));
    const T beta = static_cast<T>(args.get_f64("beta").value_or(crd::f64{0.0}));
    if (n == 0 || a_flat.size() != n * n || x_arr.size() != n || y_in.size() != n)
    {
        return error_result(args.alloc, "symv: size mismatch");
    }
    const crd::usize nn = static_cast<crd::usize>(n);
    Sym a_sym(args.alloc, nn);
    for (crd::usize i = 0; i < nn; ++i)
        for (crd::usize j = 0; j <= i; ++j)
            a_sym.at(i, j) = static_cast<T>(a_flat[i * nn + j]);
    Vec x(args.alloc, nn);
    Vec y(args.alloc, nn);
    for (crd::usize i = 0; i < nn; ++i) x(i) = static_cast<T>(x_arr[i]);
    for (crd::usize i = 0; i < nn; ++i) y(i) = static_cast<T>(y_in[i]);
    crd::hesap::dense::symv<T>(alpha, a_sym, x.span(), beta, y.span());
    crd::containers::Array<crd::f64> out(args.alloc);
    out.reserve(nn);
    for (crd::usize i = 0; i < nn; ++i) out.push_back(static_cast<crd::f64>(y(i)));
    return binary_result(args.alloc, crd::containers::ConstSpan<crd::f64>{out.data(), out.size()});
}

// ---- trsv impls (Lower / Upper × f32 / f64) --------------------------

CommandResult impl_trsv_lower_f32(const CommandArgs& args)
{
    using T = crd::f32;
    using Tri = crd::hesap::dense::Triangular<T, TriangularSide::Lower, TriangularDiag::Explicit>;
    using Vec = crd::hesap::dense::Vector<T>;
    const auto a_flat = args.get_f64_array("A");
    const auto b_arr = args.get_f64_array("b");
    const auto n = args.get_u64("n").value_or(crd::u64{0});
    if (n == 0 || a_flat.size() != n * n || b_arr.size() != n)
    {
        return error_result(args.alloc, "trsv: size mismatch");
    }
    const crd::usize nn = static_cast<crd::usize>(n);
    Tri tri_l(args.alloc, nn);
    for (crd::usize i = 0; i < nn; ++i)
        for (crd::usize j = 0; j <= i; ++j)
            tri_l.at(i, j) = static_cast<T>(a_flat[i * nn + j]);
    Vec x(args.alloc, nn);
    for (crd::usize i = 0; i < nn; ++i) x(i) = static_cast<T>(b_arr[i]);
    crd::hesap::dense::trsv<T, TriangularSide::Lower, TriangularDiag::Explicit>(tri_l, x.span(), Trans::None);
    crd::containers::Array<crd::f64> out(args.alloc);
    out.reserve(nn);
    for (crd::usize i = 0; i < nn; ++i) out.push_back(static_cast<crd::f64>(x(i)));
    return binary_result(args.alloc, crd::containers::ConstSpan<crd::f64>{out.data(), out.size()});
}

CommandResult impl_trsv_lower_f64(const CommandArgs& args)
{
    using T = crd::f64;
    using Tri = crd::hesap::dense::Triangular<T, TriangularSide::Lower, TriangularDiag::Explicit>;
    using Vec = crd::hesap::dense::Vector<T>;
    const auto a_flat = args.get_f64_array("A");
    const auto b_arr = args.get_f64_array("b");
    const auto n = args.get_u64("n").value_or(crd::u64{0});
    if (n == 0 || a_flat.size() != n * n || b_arr.size() != n)
    {
        return error_result(args.alloc, "trsv: size mismatch");
    }
    const crd::usize nn = static_cast<crd::usize>(n);
    Tri tri_l(args.alloc, nn);
    for (crd::usize i = 0; i < nn; ++i)
        for (crd::usize j = 0; j <= i; ++j)
            tri_l.at(i, j) = static_cast<T>(a_flat[i * nn + j]);
    Vec x(args.alloc, nn);
    for (crd::usize i = 0; i < nn; ++i) x(i) = static_cast<T>(b_arr[i]);
    crd::hesap::dense::trsv<T, TriangularSide::Lower, TriangularDiag::Explicit>(tri_l, x.span(), Trans::None);
    crd::containers::Array<crd::f64> out(args.alloc);
    out.reserve(nn);
    for (crd::usize i = 0; i < nn; ++i) out.push_back(static_cast<crd::f64>(x(i)));
    return binary_result(args.alloc, crd::containers::ConstSpan<crd::f64>{out.data(), out.size()});
}

CommandResult impl_trsv_upper_f32(const CommandArgs& args)
{
    using T = crd::f32;
    using Tri = crd::hesap::dense::Triangular<T, TriangularSide::Upper, TriangularDiag::Explicit>;
    using Vec = crd::hesap::dense::Vector<T>;
    const auto a_flat = args.get_f64_array("A");
    const auto b_arr = args.get_f64_array("b");
    const auto n = args.get_u64("n").value_or(crd::u64{0});
    if (n == 0 || a_flat.size() != n * n || b_arr.size() != n)
    {
        return error_result(args.alloc, "trsv: size mismatch");
    }
    const crd::usize nn = static_cast<crd::usize>(n);
    Tri tri_u(args.alloc, nn);
    for (crd::usize i = 0; i < nn; ++i)
        for (crd::usize j = i; j < nn; ++j)
            tri_u.at(i, j) = static_cast<T>(a_flat[i * nn + j]);
    Vec x(args.alloc, nn);
    for (crd::usize i = 0; i < nn; ++i) x(i) = static_cast<T>(b_arr[i]);
    crd::hesap::dense::trsv<T, TriangularSide::Upper, TriangularDiag::Explicit>(tri_u, x.span(), Trans::None);
    crd::containers::Array<crd::f64> out(args.alloc);
    out.reserve(nn);
    for (crd::usize i = 0; i < nn; ++i) out.push_back(static_cast<crd::f64>(x(i)));
    return binary_result(args.alloc, crd::containers::ConstSpan<crd::f64>{out.data(), out.size()});
}

CommandResult impl_trsv_upper_f64(const CommandArgs& args)
{
    using T = crd::f64;
    using Tri = crd::hesap::dense::Triangular<T, TriangularSide::Upper, TriangularDiag::Explicit>;
    using Vec = crd::hesap::dense::Vector<T>;
    const auto a_flat = args.get_f64_array("A");
    const auto b_arr = args.get_f64_array("b");
    const auto n = args.get_u64("n").value_or(crd::u64{0});
    if (n == 0 || a_flat.size() != n * n || b_arr.size() != n)
    {
        return error_result(args.alloc, "trsv: size mismatch");
    }
    const crd::usize nn = static_cast<crd::usize>(n);
    Tri tri_u(args.alloc, nn);
    for (crd::usize i = 0; i < nn; ++i)
        for (crd::usize j = i; j < nn; ++j)
            tri_u.at(i, j) = static_cast<T>(a_flat[i * nn + j]);
    Vec x(args.alloc, nn);
    for (crd::usize i = 0; i < nn; ++i) x(i) = static_cast<T>(b_arr[i]);
    crd::hesap::dense::trsv<T, TriangularSide::Upper, TriangularDiag::Explicit>(tri_u, x.span(), Trans::None);
    crd::containers::Array<crd::f64> out(args.alloc);
    out.reserve(nn);
    for (crd::usize i = 0; i < nn; ++i) out.push_back(static_cast<crd::f64>(x(i)));
    return binary_result(args.alloc, crd::containers::ConstSpan<crd::f64>{out.data(), out.size()});
}

// ---- Schema builders --------------------------------------------------

CommandSchema make_schema(crd::memory::IAllocator* alloc, const char* name, const char* desc,
                          OutputKind output_kind)
{
    CommandSchema s{alloc};
    s.name = crd::containers::String{name, alloc};
    s.description = crd::containers::String{desc, alloc};
    s.output.kind = output_kind;
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

void add_gemv_params(CommandSchema& s, crd::memory::IAllocator* alloc)
{
    add_param(s, alloc, "alpha", "Scalar multiplier (default 1.0)", ParamKind::F64, false);
    add_param(s, alloc, "beta", "Scalar multiplier on y (default 0.0)", ParamKind::F64, false);
    add_param(s, alloc, "rows", "Number of rows of A", ParamKind::U64, true);
    add_param(s, alloc, "cols", "Number of cols of A", ParamKind::U64, true);
    add_param(s, alloc, "A", "Matrix A flattened row-major (length rows*cols)", ParamKind::F64, true);
    add_param(s, alloc, "x", "Input vector x (length cols)", ParamKind::F64, true);
    add_param(s, alloc, "y", "Input/output vector y (length rows)", ParamKind::F64, true);
}

void add_symv_params(CommandSchema& s, crd::memory::IAllocator* alloc)
{
    add_param(s, alloc, "alpha", "Scalar multiplier (default 1.0)", ParamKind::F64, false);
    add_param(s, alloc, "beta", "Scalar multiplier on y (default 0.0)", ParamKind::F64, false);
    add_param(s, alloc, "n", "Order of A", ParamKind::U64, true);
    add_param(s, alloc, "A", "Symmetric A flattened (lower triangle canonical)", ParamKind::F64, true);
    add_param(s, alloc, "x", "Input vector x", ParamKind::F64, true);
    add_param(s, alloc, "y", "Input/output vector y", ParamKind::F64, true);
}

void add_trsv_params(CommandSchema& s, crd::memory::IAllocator* alloc, const char* a_desc)
{
    add_param(s, alloc, "n", "Matrix order", ParamKind::U64, true);
    add_param(s, alloc, "A", a_desc, ParamKind::F64, true);
    add_param(s, alloc, "b", "Right-hand side", ParamKind::F64, true);
}

} // namespace

namespace crd::hesap::dense
{
void register_blas2_cli_anchor() noexcept
{
}
} // namespace crd::hesap::dense

CRD_HESAP_CLI_REGISTER_MODULE([](CommandRegistry& reg) {
    auto* alloc = crd::memory::default_allocator();

    {
        auto s = make_schema(alloc, "hesap.dense.blas2.gemv.f32",
            "y = alpha * A * x + beta * y (f32 general).", OutputKind::BinaryBlob);
        add_gemv_params(s, alloc);
        reg.register_command(std::move(s), &impl_gemv_f32);
    }
    {
        auto s = make_schema(alloc, "hesap.dense.blas2.gemv.f64",
            "y = alpha * A * x + beta * y (f64 general).", OutputKind::BinaryBlob);
        add_gemv_params(s, alloc);
        reg.register_command(std::move(s), &impl_gemv_f64);
    }
    {
        auto s = make_schema(alloc, "hesap.dense.blas2.symv.f32",
            "y = alpha * A * x + beta * y (f32 symmetric).", OutputKind::BinaryBlob);
        add_symv_params(s, alloc);
        reg.register_command(std::move(s), &impl_symv_f32);
    }
    {
        auto s = make_schema(alloc, "hesap.dense.blas2.symv.f64",
            "y = alpha * A * x + beta * y (f64 symmetric).", OutputKind::BinaryBlob);
        add_symv_params(s, alloc);
        reg.register_command(std::move(s), &impl_symv_f64);
    }
    {
        auto s = make_schema(alloc, "hesap.dense.blas2.trsv.lower.f32",
            "Solve L * x = b in-place (f32).", OutputKind::BinaryBlob);
        add_trsv_params(s, alloc, "Lower triangular L flattened (only lower half used)");
        reg.register_command(std::move(s), &impl_trsv_lower_f32);
    }
    {
        auto s = make_schema(alloc, "hesap.dense.blas2.trsv.lower.f64",
            "Solve L * x = b in-place (f64).", OutputKind::BinaryBlob);
        add_trsv_params(s, alloc, "Lower triangular L flattened (only lower half used)");
        reg.register_command(std::move(s), &impl_trsv_lower_f64);
    }
    {
        auto s = make_schema(alloc, "hesap.dense.blas2.trsv.upper.f32",
            "Solve U * x = b in-place (f32).", OutputKind::BinaryBlob);
        add_trsv_params(s, alloc, "Upper triangular U flattened (only upper half used)");
        reg.register_command(std::move(s), &impl_trsv_upper_f32);
    }
    {
        auto s = make_schema(alloc, "hesap.dense.blas2.trsv.upper.f64",
            "Solve U * x = b in-place (f64).", OutputKind::BinaryBlob);
        add_trsv_params(s, alloc, "Upper triangular U flattened (only upper half used)");
        reg.register_command(std::move(s), &impl_trsv_upper_f64);
    }
})
