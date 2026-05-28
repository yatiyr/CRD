// v0e-g — CLI registration for dense direct solvers: LU / Cholesky /
// LDLT / QR. Each command takes (A, b) and returns x — a single-shot
// factor+solve. Factor objects (LU/Cholesky/LDLT/QR) are not directly
// serializable across the CLI boundary (Permutation + block_kinds +
// taus are intricate), so we ship factor+solve as one atomic command.
// Future v0e2 may expose factor-only / solve-only when consumers need
// to reuse a factor across many RHS vectors (e.g., iterative refinement
// via CLI).
//
// Anchor symbol: `register_solvers_cli_anchor()` in `cli_anchor.hpp`.

#include <crd/hesap/cli/arg_value.hpp>
#include <crd/hesap/cli/command_registry.hpp>
#include <crd/hesap/dense/cholesky.hpp>
#include <crd/hesap/dense/cli_anchor.hpp>
#include <crd/hesap/dense/ldlt.hpp>
#include <crd/hesap/dense/lu.hpp>
#include <crd/hesap/dense/matrix.hpp>
#include <crd/hesap/dense/matrix_types.hpp>
#include <crd/hesap/dense/qr.hpp>

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
using crd::hesap::dense::Cholesky;
using crd::hesap::dense::factor_cholesky;
using crd::hesap::dense::factor_ldlt;
using crd::hesap::dense::factor_lu;
using crd::hesap::dense::factor_qr;
using crd::hesap::dense::Layout;
using crd::hesap::dense::LDLT;
using crd::hesap::dense::LU;
using crd::hesap::dense::Matrix;
using crd::hesap::dense::QR;
using crd::hesap::dense::solve_cholesky;
using crd::hesap::dense::solve_ldlt;
using crd::hesap::dense::solve_lu;
using crd::hesap::dense::solve_qr;
using crd::hesap::dense::Symmetric;

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

// ---- LU solver ------------------------------------------------------

template <typename T> CommandResult impl_lu_solve(const CommandArgs& args)
{
    const auto a_flat = args.get_f64_array("A");
    const auto b_flat = args.get_f64_array("b");
    const auto n = args.get_u64("n").value_or(crd::u64{0});
    if (n == 0 || a_flat.size() != n * n || b_flat.size() != n)
    {
        return error_result(args.alloc, "lu.solve: A=n*n, b=n required");
    }
    const crd::usize nn = static_cast<crd::usize>(n);
    Matrix<T, Layout::RowMajor> a_mat(args.alloc, nn, nn);
    for (crd::usize i = 0; i < nn; ++i)
    {
        for (crd::usize j = 0; j < nn; ++j)
        {
            a_mat(i, j) = static_cast<T>(a_flat[i * nn + j]);
        }
    }
    LU<T, Layout::RowMajor> lu(args.alloc, nn);
    factor_lu(lu, a_mat);
    if (lu.is_singular())
    {
        return error_result(args.alloc, "lu.solve: matrix is singular");
    }
    crd::containers::Array<T> x(args.alloc);
    x.resize(nn);
    for (crd::usize i = 0; i < nn; ++i)
    {
        x[i] = static_cast<T>(b_flat[i]);
    }
    solve_lu(lu, crd::containers::Span<T>(x.data(), nn));
    crd::containers::Array<crd::f64> out(args.alloc);
    out.reserve(nn);
    for (crd::usize i = 0; i < nn; ++i)
    {
        out.push_back(static_cast<crd::f64>(x[i]));
    }
    return binary_result(args.alloc, crd::containers::ConstSpan<crd::f64>{out.data(), out.size()});
}

// ---- Cholesky solver ------------------------------------------------

template <typename T> CommandResult impl_cholesky_solve(const CommandArgs& args)
{
    const auto a_flat = args.get_f64_array("A");
    const auto b_flat = args.get_f64_array("b");
    const auto n = args.get_u64("n").value_or(crd::u64{0});
    if (n == 0 || a_flat.size() != n * n || b_flat.size() != n)
    {
        return error_result(args.alloc, "cholesky.solve: A=n*n (symmetric, lower-half), b=n required");
    }
    const crd::usize nn = static_cast<crd::usize>(n);
    Symmetric<T> a_sym(args.alloc, nn);
    for (crd::usize i = 0; i < nn; ++i)
    {
        for (crd::usize j = 0; j <= i; ++j)
        {
            a_sym.at(i, j) = static_cast<T>(a_flat[i * nn + j]);
        }
    }
    Cholesky<T, Layout::RowMajor> chol(args.alloc, nn);
    factor_cholesky(chol, a_sym);
    if (chol.is_singular())
    {
        return error_result(args.alloc, "cholesky.solve: matrix is not positive-definite");
    }
    crd::containers::Array<T> x(args.alloc);
    x.resize(nn);
    for (crd::usize i = 0; i < nn; ++i)
    {
        x[i] = static_cast<T>(b_flat[i]);
    }
    solve_cholesky(chol, crd::containers::Span<T>(x.data(), nn));
    crd::containers::Array<crd::f64> out(args.alloc);
    out.reserve(nn);
    for (crd::usize i = 0; i < nn; ++i)
    {
        out.push_back(static_cast<crd::f64>(x[i]));
    }
    return binary_result(args.alloc, crd::containers::ConstSpan<crd::f64>{out.data(), out.size()});
}

// ---- LDLT solver ----------------------------------------------------

template <typename T> CommandResult impl_ldlt_solve(const CommandArgs& args)
{
    const auto a_flat = args.get_f64_array("A");
    const auto b_flat = args.get_f64_array("b");
    const auto n = args.get_u64("n").value_or(crd::u64{0});
    if (n == 0 || a_flat.size() != n * n || b_flat.size() != n)
    {
        return error_result(args.alloc, "ldlt.solve: A=n*n (symmetric, lower-half), b=n required");
    }
    const crd::usize nn = static_cast<crd::usize>(n);
    Symmetric<T> a_sym(args.alloc, nn);
    for (crd::usize i = 0; i < nn; ++i)
    {
        for (crd::usize j = 0; j <= i; ++j)
        {
            a_sym.at(i, j) = static_cast<T>(a_flat[i * nn + j]);
        }
    }
    LDLT<T, Layout::RowMajor> ldlt(args.alloc, nn);
    factor_ldlt(ldlt, a_sym);
    if (ldlt.is_singular())
    {
        return error_result(args.alloc, "ldlt.solve: matrix is singular");
    }
    crd::containers::Array<T> x(args.alloc);
    x.resize(nn);
    for (crd::usize i = 0; i < nn; ++i)
    {
        x[i] = static_cast<T>(b_flat[i]);
    }
    solve_ldlt(ldlt, crd::containers::Span<T>(x.data(), nn));
    crd::containers::Array<crd::f64> out(args.alloc);
    out.reserve(nn);
    for (crd::usize i = 0; i < nn; ++i)
    {
        out.push_back(static_cast<crd::f64>(x[i]));
    }
    return binary_result(args.alloc, crd::containers::ConstSpan<crd::f64>{out.data(), out.size()});
}

// ---- QR solver (square or least-squares; m >= n) ---------------------

template <typename T> CommandResult impl_qr_solve(const CommandArgs& args)
{
    const auto a_flat = args.get_f64_array("A");
    const auto b_flat = args.get_f64_array("b");
    const auto m = args.get_u64("m").value_or(crd::u64{0});
    const auto n = args.get_u64("n").value_or(crd::u64{0});
    if (m == 0 || n == 0 || m < n || a_flat.size() != m * n || b_flat.size() != m)
    {
        return error_result(args.alloc, "qr.solve: A=m*n, b=m required (m >= n)");
    }
    const crd::usize mm = static_cast<crd::usize>(m);
    const crd::usize nn = static_cast<crd::usize>(n);
    Matrix<T, Layout::RowMajor> a_mat(args.alloc, mm, nn);
    for (crd::usize i = 0; i < mm; ++i)
    {
        for (crd::usize j = 0; j < nn; ++j)
        {
            a_mat(i, j) = static_cast<T>(a_flat[i * nn + j]);
        }
    }
    QR<T, Layout::RowMajor> qr(args.alloc, mm, nn);
    factor_qr(qr, a_mat);
    crd::containers::Array<T> b(args.alloc);
    b.resize(mm);
    for (crd::usize i = 0; i < mm; ++i)
    {
        b[i] = static_cast<T>(b_flat[i]);
    }
    solve_qr(qr, crd::containers::Span<T>(b.data(), mm));
    crd::containers::Array<crd::f64> out(args.alloc);
    out.reserve(nn);
    for (crd::usize i = 0; i < nn; ++i)
    {
        out.push_back(static_cast<crd::f64>(b[i]));
    }
    return binary_result(args.alloc, crd::containers::ConstSpan<crd::f64>{out.data(), out.size()});
}

CommandSchema make_schema(crd::memory::IAllocator* alloc, const char* name, const char* desc, OutputKind output_kind)
{
    CommandSchema s{alloc};
    s.name = crd::containers::String{name, alloc};
    s.description = crd::containers::String{desc, alloc};
    s.output.kind = output_kind;
    s.required_caps.bits = Capability::kHesapCompute;
    s.idempotent = true;
    return s;
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

} // namespace

namespace crd::hesap::dense
{
void register_solvers_cli_anchor() noexcept {}
} // namespace crd::hesap::dense

// Registration uses crd allocators (abort on OOM, never throw); the std bad_alloc path the check
// traces is unreachable, and the registrar ctor is noexcept (would terminate, not escape) regardless.
// NOLINTNEXTLINE(bugprone-throwing-static-initialization)
CRD_HESAP_CLI_REGISTER_MODULE(
    [](CommandRegistry& reg)
    {
        auto* alloc = crd::memory::default_allocator();

        // ---- LU --------------------------------------------------------
        {
            auto s = make_schema(alloc, "hesap.dense.solver.lu.f32",
                                 "Solve A * x = b via LU partial-pivoting (f32 square).", OutputKind::BinaryBlob);
            add_param(s, alloc, "n", "Order of A", ParamKind::U64, true);
            add_param(s, alloc, "A", "Matrix A flattened row-major (n*n)", ParamKind::F64, true);
            add_param(s, alloc, "b", "RHS b (n)", ParamKind::F64, true);
            reg.register_command(std::move(s), &impl_lu_solve<crd::f32>);
        }
        {
            auto s = make_schema(alloc, "hesap.dense.solver.lu.f64",
                                 "Solve A * x = b via LU partial-pivoting (f64 square).", OutputKind::BinaryBlob);
            add_param(s, alloc, "n", "Order of A", ParamKind::U64, true);
            add_param(s, alloc, "A", "Matrix A flattened row-major (n*n)", ParamKind::F64, true);
            add_param(s, alloc, "b", "RHS b (n)", ParamKind::F64, true);
            reg.register_command(std::move(s), &impl_lu_solve<crd::f64>);
        }

        // ---- Cholesky --------------------------------------------------
        {
            auto s = make_schema(alloc, "hesap.dense.solver.cholesky.f32",
                                 "Solve A * x = b via Cholesky (f32 SPD; A lower triangle authoritative).",
                                 OutputKind::BinaryBlob);
            add_param(s, alloc, "n", "Order of A", ParamKind::U64, true);
            add_param(s, alloc, "A", "Symmetric A flattened (n*n); lower triangle used", ParamKind::F64, true);
            add_param(s, alloc, "b", "RHS b (n)", ParamKind::F64, true);
            reg.register_command(std::move(s), &impl_cholesky_solve<crd::f32>);
        }
        {
            auto s = make_schema(alloc, "hesap.dense.solver.cholesky.f64",
                                 "Solve A * x = b via Cholesky (f64 SPD; A lower triangle authoritative).",
                                 OutputKind::BinaryBlob);
            add_param(s, alloc, "n", "Order of A", ParamKind::U64, true);
            add_param(s, alloc, "A", "Symmetric A flattened (n*n); lower triangle used", ParamKind::F64, true);
            add_param(s, alloc, "b", "RHS b (n)", ParamKind::F64, true);
            reg.register_command(std::move(s), &impl_cholesky_solve<crd::f64>);
        }

        // ---- LDLT ------------------------------------------------------
        {
            auto s = make_schema(alloc, "hesap.dense.solver.ldlt.f32",
                                 "Solve A * x = b via Bunch-Kaufman LDLT (f32 symmetric indefinite).",
                                 OutputKind::BinaryBlob);
            add_param(s, alloc, "n", "Order of A", ParamKind::U64, true);
            add_param(s, alloc, "A", "Symmetric A flattened (n*n); lower triangle used", ParamKind::F64, true);
            add_param(s, alloc, "b", "RHS b (n)", ParamKind::F64, true);
            reg.register_command(std::move(s), &impl_ldlt_solve<crd::f32>);
        }
        {
            auto s = make_schema(alloc, "hesap.dense.solver.ldlt.f64",
                                 "Solve A * x = b via Bunch-Kaufman LDLT (f64 symmetric indefinite).",
                                 OutputKind::BinaryBlob);
            add_param(s, alloc, "n", "Order of A", ParamKind::U64, true);
            add_param(s, alloc, "A", "Symmetric A flattened (n*n); lower triangle used", ParamKind::F64, true);
            add_param(s, alloc, "b", "RHS b (n)", ParamKind::F64, true);
            reg.register_command(std::move(s), &impl_ldlt_solve<crd::f64>);
        }

        // ---- QR (square + least-squares) -------------------------------
        {
            auto s =
                make_schema(alloc, "hesap.dense.solver.qr.f32",
                            "Solve A * x = b via Householder QR (f32; m >= n; LS for m > n).", OutputKind::BinaryBlob);
            add_param(s, alloc, "m", "Rows of A", ParamKind::U64, true);
            add_param(s, alloc, "n", "Cols of A", ParamKind::U64, true);
            add_param(s, alloc, "A", "Matrix A flattened row-major (m*n)", ParamKind::F64, true);
            add_param(s, alloc, "b", "RHS b (m)", ParamKind::F64, true);
            reg.register_command(std::move(s), &impl_qr_solve<crd::f32>);
        }
        {
            auto s =
                make_schema(alloc, "hesap.dense.solver.qr.f64",
                            "Solve A * x = b via Householder QR (f64; m >= n; LS for m > n).", OutputKind::BinaryBlob);
            add_param(s, alloc, "m", "Rows of A", ParamKind::U64, true);
            add_param(s, alloc, "n", "Cols of A", ParamKind::U64, true);
            add_param(s, alloc, "A", "Matrix A flattened row-major (m*n)", ParamKind::F64, true);
            add_param(s, alloc, "b", "RHS b (m)", ParamKind::F64, true);
            reg.register_command(std::move(s), &impl_qr_solve<crd::f64>);
        }
    });
