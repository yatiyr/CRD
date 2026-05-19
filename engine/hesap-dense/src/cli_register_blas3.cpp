// BLAS L3 CLI command registration. Phase 3.1.6 v0d.
//
// v0d-foundation surface: 4 working commands (gemm.f32/f64,
// trsm.lower.f32/f64). Remaining BLAS L3 schemas (syrk / herk / her2k /
// trmm + complex variants) are filed as `v0d-cli-extend` follow-on per
// `feedback_ship_at_consumer_template_from_day_one` — the engine surface
// is complete; CLI widens when v0e dense-direct / iterative consumers
// arrive.

#include <crd/hesap/cli/arg_value.hpp>
#include <crd/hesap/cli/command_registry.hpp>
#include <crd/hesap/dense/blas3.hpp>
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

// ---- gemm impls ------------------------------------------------------

CommandResult impl_gemm_f32(const CommandArgs& args)
{
    using T = crd::f32;
    using Mat = crd::hesap::dense::Matrix<T>;
    const auto a_flat = args.get_f64_array("A");
    const auto b_flat = args.get_f64_array("B");
    const auto c_in = args.get_f64_array("C");
    const auto m = args.get_u64("m").value_or(crd::u64{0});
    const auto k = args.get_u64("k").value_or(crd::u64{0});
    const auto n = args.get_u64("n").value_or(crd::u64{0});
    const T alpha = static_cast<T>(args.get_f64("alpha").value_or(crd::f64{1.0}));
    const T beta = static_cast<T>(args.get_f64("beta").value_or(crd::f64{0.0}));
    if (m == 0 || k == 0 || n == 0 || a_flat.size() != m * k || b_flat.size() != k * n ||
        c_in.size() != m * n)
    {
        return error_result(args.alloc, "gemm: A=m*k, B=k*n, C=m*n required");
    }
    const crd::usize mm = static_cast<crd::usize>(m);
    const crd::usize kk = static_cast<crd::usize>(k);
    const crd::usize nn = static_cast<crd::usize>(n);
    Mat a_mat(args.alloc, mm, kk);
    Mat b_mat(args.alloc, kk, nn);
    Mat c_mat(args.alloc, mm, nn);
    for (crd::usize i = 0; i < mm; ++i)
        for (crd::usize j = 0; j < kk; ++j)
            a_mat(i, j) = static_cast<T>(a_flat[i * kk + j]);
    for (crd::usize i = 0; i < kk; ++i)
        for (crd::usize j = 0; j < nn; ++j)
            b_mat(i, j) = static_cast<T>(b_flat[i * nn + j]);
    for (crd::usize i = 0; i < mm; ++i)
        for (crd::usize j = 0; j < nn; ++j)
            c_mat(i, j) = static_cast<T>(c_in[i * nn + j]);
    crd::hesap::dense::gemm<T, Layout::RowMajor>(alpha, a_mat.cview(), b_mat.cview(), beta, c_mat.view());
    crd::containers::Array<crd::f64> out(args.alloc);
    out.reserve(mm * nn);
    for (crd::usize i = 0; i < mm; ++i)
        for (crd::usize j = 0; j < nn; ++j)
            out.push_back(static_cast<crd::f64>(c_mat(i, j)));
    return binary_result(args.alloc, crd::containers::ConstSpan<crd::f64>{out.data(), out.size()});
}

CommandResult impl_gemm_f64(const CommandArgs& args)
{
    using T = crd::f64;
    using Mat = crd::hesap::dense::Matrix<T>;
    const auto a_flat = args.get_f64_array("A");
    const auto b_flat = args.get_f64_array("B");
    const auto c_in = args.get_f64_array("C");
    const auto m = args.get_u64("m").value_or(crd::u64{0});
    const auto k = args.get_u64("k").value_or(crd::u64{0});
    const auto n = args.get_u64("n").value_or(crd::u64{0});
    const T alpha = args.get_f64("alpha").value_or(crd::f64{1.0});
    const T beta = args.get_f64("beta").value_or(crd::f64{0.0});
    if (m == 0 || k == 0 || n == 0 || a_flat.size() != m * k || b_flat.size() != k * n ||
        c_in.size() != m * n)
    {
        return error_result(args.alloc, "gemm: A=m*k, B=k*n, C=m*n required");
    }
    const crd::usize mm = static_cast<crd::usize>(m);
    const crd::usize kk = static_cast<crd::usize>(k);
    const crd::usize nn = static_cast<crd::usize>(n);
    Mat a_mat(args.alloc, mm, kk);
    Mat b_mat(args.alloc, kk, nn);
    Mat c_mat(args.alloc, mm, nn);
    for (crd::usize i = 0; i < mm; ++i)
        for (crd::usize j = 0; j < kk; ++j)
            a_mat(i, j) = a_flat[i * kk + j];
    for (crd::usize i = 0; i < kk; ++i)
        for (crd::usize j = 0; j < nn; ++j)
            b_mat(i, j) = b_flat[i * nn + j];
    for (crd::usize i = 0; i < mm; ++i)
        for (crd::usize j = 0; j < nn; ++j)
            c_mat(i, j) = c_in[i * nn + j];
    crd::hesap::dense::gemm<T, Layout::RowMajor>(alpha, a_mat.cview(), b_mat.cview(), beta, c_mat.view());
    crd::containers::Array<crd::f64> out(args.alloc);
    out.reserve(mm * nn);
    for (crd::usize i = 0; i < mm; ++i)
        for (crd::usize j = 0; j < nn; ++j)
            out.push_back(c_mat(i, j));
    return binary_result(args.alloc, crd::containers::ConstSpan<crd::f64>{out.data(), out.size()});
}

// ---- trsm Lower impls ------------------------------------------------

CommandResult impl_trsm_lower_f32(const CommandArgs& args)
{
    using T = crd::f32;
    using Tri = crd::hesap::dense::Triangular<T, TriangularSide::Lower, TriangularDiag::Explicit>;
    using Mat = crd::hesap::dense::Matrix<T>;
    const auto a_flat = args.get_f64_array("A");
    const auto b_flat = args.get_f64_array("B");
    const auto n = args.get_u64("n").value_or(crd::u64{0});
    const auto cols = args.get_u64("cols").value_or(crd::u64{0});
    const T alpha = static_cast<T>(args.get_f64("alpha").value_or(crd::f64{1.0}));
    if (n == 0 || cols == 0 || a_flat.size() != n * n || b_flat.size() != n * cols)
    {
        return error_result(args.alloc, "trsm: A=n*n, B=n*cols required");
    }
    const crd::usize nn = static_cast<crd::usize>(n);
    const crd::usize cc = static_cast<crd::usize>(cols);
    Tri tri_l(args.alloc, nn);
    for (crd::usize i = 0; i < nn; ++i)
        for (crd::usize j = 0; j <= i; ++j)
            tri_l.at(i, j) = static_cast<T>(a_flat[i * nn + j]);
    Mat b_mat(args.alloc, nn, cc);
    for (crd::usize i = 0; i < nn; ++i)
        for (crd::usize j = 0; j < cc; ++j)
            b_mat(i, j) = static_cast<T>(b_flat[i * cc + j]);
    crd::hesap::dense::trsm<T, TriangularSide::Lower, TriangularDiag::Explicit>(
        alpha, tri_l, b_mat.view(), Trans::None);
    crd::containers::Array<crd::f64> out(args.alloc);
    out.reserve(nn * cc);
    for (crd::usize i = 0; i < nn; ++i)
        for (crd::usize j = 0; j < cc; ++j)
            out.push_back(static_cast<crd::f64>(b_mat(i, j)));
    return binary_result(args.alloc, crd::containers::ConstSpan<crd::f64>{out.data(), out.size()});
}

CommandResult impl_trsm_lower_f64(const CommandArgs& args)
{
    using T = crd::f64;
    using Tri = crd::hesap::dense::Triangular<T, TriangularSide::Lower, TriangularDiag::Explicit>;
    using Mat = crd::hesap::dense::Matrix<T>;
    const auto a_flat = args.get_f64_array("A");
    const auto b_flat = args.get_f64_array("B");
    const auto n = args.get_u64("n").value_or(crd::u64{0});
    const auto cols = args.get_u64("cols").value_or(crd::u64{0});
    const T alpha = args.get_f64("alpha").value_or(crd::f64{1.0});
    if (n == 0 || cols == 0 || a_flat.size() != n * n || b_flat.size() != n * cols)
    {
        return error_result(args.alloc, "trsm: A=n*n, B=n*cols required");
    }
    const crd::usize nn = static_cast<crd::usize>(n);
    const crd::usize cc = static_cast<crd::usize>(cols);
    Tri tri_l(args.alloc, nn);
    for (crd::usize i = 0; i < nn; ++i)
        for (crd::usize j = 0; j <= i; ++j)
            tri_l.at(i, j) = a_flat[i * nn + j];
    Mat b_mat(args.alloc, nn, cc);
    for (crd::usize i = 0; i < nn; ++i)
        for (crd::usize j = 0; j < cc; ++j)
            b_mat(i, j) = b_flat[i * cc + j];
    crd::hesap::dense::trsm<T, TriangularSide::Lower, TriangularDiag::Explicit>(
        alpha, tri_l, b_mat.view(), Trans::None);
    crd::containers::Array<crd::f64> out(args.alloc);
    out.reserve(nn * cc);
    for (crd::usize i = 0; i < nn; ++i)
        for (crd::usize j = 0; j < cc; ++j)
            out.push_back(b_mat(i, j));
    return binary_result(args.alloc, crd::containers::ConstSpan<crd::f64>{out.data(), out.size()});
}

// ---- syrk impls (v0d-cli-extend) -------------------------------------

template <typename T>
CommandResult impl_syrk(const CommandArgs& args)
{
    using Mat = crd::hesap::dense::Matrix<T>;
    using Sym = crd::hesap::dense::Symmetric<T>;
    const auto a_flat = args.get_f64_array("A");
    const auto c_in = args.get_f64_array("C");
    const auto m = args.get_u64("m").value_or(crd::u64{0});
    const auto k = args.get_u64("k").value_or(crd::u64{0});
    const T alpha = static_cast<T>(args.get_f64("alpha").value_or(crd::f64{1.0}));
    const T beta = static_cast<T>(args.get_f64("beta").value_or(crd::f64{0.0}));
    if (m == 0 || k == 0 || a_flat.size() != m * k || c_in.size() != m * m)
    {
        return error_result(args.alloc, "syrk: A=m*k, C=m*m required");
    }
    const crd::usize mm = static_cast<crd::usize>(m);
    const crd::usize kk = static_cast<crd::usize>(k);
    Mat a_mat(args.alloc, mm, kk);
    Sym c_sym(args.alloc, mm);
    for (crd::usize i = 0; i < mm; ++i)
        for (crd::usize j = 0; j < kk; ++j)
            a_mat(i, j) = static_cast<T>(a_flat[i * kk + j]);
    for (crd::usize i = 0; i < mm; ++i)
        for (crd::usize j = 0; j <= i; ++j)
            c_sym.at(i, j) = static_cast<T>(c_in[i * mm + j]);
    crd::hesap::dense::syrk<T>(alpha, a_mat.cview(), beta, c_sym);
    crd::containers::Array<crd::f64> out(args.alloc);
    out.reserve(mm * mm);
    for (crd::usize i = 0; i < mm; ++i)
        for (crd::usize j = 0; j < mm; ++j)
            out.push_back(static_cast<crd::f64>(c_sym.at(i, j)));
    return binary_result(args.alloc, crd::containers::ConstSpan<crd::f64>{out.data(), out.size()});
}

// ---- trmm impls (lower / upper, f32 / f64) ----------------------------

template <typename T, TriangularSide Side>
CommandResult impl_trmm(const CommandArgs& args)
{
    using Tri = crd::hesap::dense::Triangular<T, Side, TriangularDiag::Explicit>;
    using Mat = crd::hesap::dense::Matrix<T>;
    const auto a_flat = args.get_f64_array("A");
    const auto b_in = args.get_f64_array("B");
    const auto n = args.get_u64("n").value_or(crd::u64{0});
    const auto cols = args.get_u64("cols").value_or(crd::u64{0});
    const T alpha = static_cast<T>(args.get_f64("alpha").value_or(crd::f64{1.0}));
    if (n == 0 || cols == 0 || a_flat.size() != n * n || b_in.size() != n * cols)
    {
        return error_result(args.alloc, "trmm: A=n*n, B=n*cols required");
    }
    const crd::usize nn = static_cast<crd::usize>(n);
    const crd::usize cc = static_cast<crd::usize>(cols);
    Tri tri_a(args.alloc, nn);
    for (crd::usize i = 0; i < nn; ++i)
    {
        if constexpr (Side == TriangularSide::Lower)
        {
            for (crd::usize j = 0; j <= i; ++j)
                tri_a.at(i, j) = static_cast<T>(a_flat[i * nn + j]);
        }
        else
        {
            for (crd::usize j = i; j < nn; ++j)
                tri_a.at(i, j) = static_cast<T>(a_flat[i * nn + j]);
        }
    }
    Mat b_mat(args.alloc, nn, cc);
    for (crd::usize i = 0; i < nn; ++i)
        for (crd::usize j = 0; j < cc; ++j)
            b_mat(i, j) = static_cast<T>(b_in[i * cc + j]);
    crd::hesap::dense::trmm<T, Side, TriangularDiag::Explicit>(alpha, tri_a, b_mat.view());
    crd::containers::Array<crd::f64> out(args.alloc);
    out.reserve(nn * cc);
    for (crd::usize i = 0; i < nn; ++i)
        for (crd::usize j = 0; j < cc; ++j)
            out.push_back(static_cast<crd::f64>(b_mat(i, j)));
    return binary_result(args.alloc, crd::containers::ConstSpan<crd::f64>{out.data(), out.size()});
}

// ---- trsm Upper impls (f32, f64) — Lower already shipped --------------

template <typename T>
CommandResult impl_trsm_upper(const CommandArgs& args)
{
    using Tri = crd::hesap::dense::Triangular<T, TriangularSide::Upper, TriangularDiag::Explicit>;
    using Mat = crd::hesap::dense::Matrix<T>;
    const auto a_flat = args.get_f64_array("A");
    const auto b_flat = args.get_f64_array("B");
    const auto n = args.get_u64("n").value_or(crd::u64{0});
    const auto cols = args.get_u64("cols").value_or(crd::u64{0});
    const T alpha = static_cast<T>(args.get_f64("alpha").value_or(crd::f64{1.0}));
    if (n == 0 || cols == 0 || a_flat.size() != n * n || b_flat.size() != n * cols)
    {
        return error_result(args.alloc, "trsm: A=n*n, B=n*cols required");
    }
    const crd::usize nn = static_cast<crd::usize>(n);
    const crd::usize cc = static_cast<crd::usize>(cols);
    Tri tri_u(args.alloc, nn);
    for (crd::usize i = 0; i < nn; ++i)
        for (crd::usize j = i; j < nn; ++j)
            tri_u.at(i, j) = static_cast<T>(a_flat[i * nn + j]);
    Mat b_mat(args.alloc, nn, cc);
    for (crd::usize i = 0; i < nn; ++i)
        for (crd::usize j = 0; j < cc; ++j)
            b_mat(i, j) = static_cast<T>(b_flat[i * cc + j]);
    crd::hesap::dense::trsm<T, TriangularSide::Upper, TriangularDiag::Explicit>(
        alpha, tri_u, b_mat.view(), Trans::None);
    crd::containers::Array<crd::f64> out(args.alloc);
    out.reserve(nn * cc);
    for (crd::usize i = 0; i < nn; ++i)
        for (crd::usize j = 0; j < cc; ++j)
            out.push_back(static_cast<crd::f64>(b_mat(i, j)));
    return binary_result(args.alloc, crd::containers::ConstSpan<crd::f64>{out.data(), out.size()});
}

// ---- gemm_parallel_auto impls (heuristic worker picker, v0d-parallelism-
//      auto-dispatch) ---------------------------------------------------

template <typename T>
CommandResult impl_gemm_parallel_auto(const CommandArgs& args)
{
    using Mat = crd::hesap::dense::Matrix<T>;
    const auto a_flat = args.get_f64_array("A");
    const auto b_flat = args.get_f64_array("B");
    const auto c_in = args.get_f64_array("C");
    const auto m = args.get_u64("m").value_or(crd::u64{0});
    const auto k = args.get_u64("k").value_or(crd::u64{0});
    const auto n = args.get_u64("n").value_or(crd::u64{0});
    const T alpha = static_cast<T>(args.get_f64("alpha").value_or(crd::f64{1.0}));
    const T beta = static_cast<T>(args.get_f64("beta").value_or(crd::f64{0.0}));
    if (m == 0 || k == 0 || n == 0 || a_flat.size() != m * k || b_flat.size() != k * n ||
        c_in.size() != m * n)
    {
        return error_result(args.alloc, "gemm_parallel_auto: A=m*k, B=k*n, C=m*n required");
    }
    const crd::usize mm = static_cast<crd::usize>(m);
    const crd::usize kk = static_cast<crd::usize>(k);
    const crd::usize nn = static_cast<crd::usize>(n);
    Mat a_mat(args.alloc, mm, kk);
    Mat b_mat(args.alloc, kk, nn);
    Mat c_mat(args.alloc, mm, nn);
    for (crd::usize i = 0; i < mm; ++i)
        for (crd::usize j = 0; j < kk; ++j)
            a_mat(i, j) = static_cast<T>(a_flat[i * kk + j]);
    for (crd::usize i = 0; i < kk; ++i)
        for (crd::usize j = 0; j < nn; ++j)
            b_mat(i, j) = static_cast<T>(b_flat[i * nn + j]);
    for (crd::usize i = 0; i < mm; ++i)
        for (crd::usize j = 0; j < nn; ++j)
            c_mat(i, j) = static_cast<T>(c_in[i * nn + j]);
    crd::hesap::dense::gemm_parallel_auto<T, Layout::RowMajor>(alpha, a_mat.cview(), b_mat.cview(),
                                                               beta, c_mat.view());
    crd::containers::Array<crd::f64> out(args.alloc);
    out.reserve(mm * nn);
    for (crd::usize i = 0; i < mm; ++i)
        for (crd::usize j = 0; j < nn; ++j)
            out.push_back(static_cast<crd::f64>(c_mat(i, j)));
    return binary_result(args.alloc, crd::containers::ConstSpan<crd::f64>{out.data(), out.size()});
}

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

} // namespace

namespace crd::hesap::dense
{
void register_blas3_cli_anchor() noexcept
{
}
} // namespace crd::hesap::dense

CRD_HESAP_CLI_REGISTER_MODULE([](CommandRegistry& reg) {
    auto* alloc = crd::memory::default_allocator();

    {
        auto s = make_schema(alloc, "hesap.dense.blas3.gemm.f32",
            "C = alpha * A * B + beta * C (f32 general).", OutputKind::BinaryBlob);
        add_param(s, alloc, "alpha", "Scalar multiplier", ParamKind::F64, false);
        add_param(s, alloc, "beta", "Scalar multiplier on C", ParamKind::F64, false);
        add_param(s, alloc, "m", "Rows of A and C", ParamKind::U64, true);
        add_param(s, alloc, "k", "Cols of A / rows of B", ParamKind::U64, true);
        add_param(s, alloc, "n", "Cols of B and C", ParamKind::U64, true);
        add_param(s, alloc, "A", "Matrix A flattened row-major (m*k)", ParamKind::F64, true);
        add_param(s, alloc, "B", "Matrix B flattened row-major (k*n)", ParamKind::F64, true);
        add_param(s, alloc, "C", "Input/output C flattened row-major (m*n)", ParamKind::F64, true);
        reg.register_command(std::move(s), &impl_gemm_f32);
    }
    {
        auto s = make_schema(alloc, "hesap.dense.blas3.gemm.f64",
            "C = alpha * A * B + beta * C (f64 general).", OutputKind::BinaryBlob);
        add_param(s, alloc, "alpha", "Scalar multiplier", ParamKind::F64, false);
        add_param(s, alloc, "beta", "Scalar multiplier on C", ParamKind::F64, false);
        add_param(s, alloc, "m", "Rows of A and C", ParamKind::U64, true);
        add_param(s, alloc, "k", "Cols of A / rows of B", ParamKind::U64, true);
        add_param(s, alloc, "n", "Cols of B and C", ParamKind::U64, true);
        add_param(s, alloc, "A", "Matrix A flattened row-major (m*k)", ParamKind::F64, true);
        add_param(s, alloc, "B", "Matrix B flattened row-major (k*n)", ParamKind::F64, true);
        add_param(s, alloc, "C", "Input/output C flattened row-major (m*n)", ParamKind::F64, true);
        reg.register_command(std::move(s), &impl_gemm_f64);
    }
    {
        auto s = make_schema(alloc, "hesap.dense.blas3.trsm.lower.f32",
            "Solve alpha * L * X = B for X in-place (f32 lower).", OutputKind::BinaryBlob);
        add_param(s, alloc, "alpha", "Scalar multiplier", ParamKind::F64, false);
        add_param(s, alloc, "n", "Order of L", ParamKind::U64, true);
        add_param(s, alloc, "cols", "Number of columns in B", ParamKind::U64, true);
        add_param(s, alloc, "A", "Lower triangular L flattened (only lower half used)", ParamKind::F64, true);
        add_param(s, alloc, "B", "RHS B flattened row-major (n*cols)", ParamKind::F64, true);
        reg.register_command(std::move(s), &impl_trsm_lower_f32);
    }
    {
        auto s = make_schema(alloc, "hesap.dense.blas3.trsm.lower.f64",
            "Solve alpha * L * X = B for X in-place (f64 lower).", OutputKind::BinaryBlob);
        add_param(s, alloc, "alpha", "Scalar multiplier", ParamKind::F64, false);
        add_param(s, alloc, "n", "Order of L", ParamKind::U64, true);
        add_param(s, alloc, "cols", "Number of columns in B", ParamKind::U64, true);
        add_param(s, alloc, "A", "Lower triangular L flattened (only lower half used)", ParamKind::F64, true);
        add_param(s, alloc, "B", "RHS B flattened row-major (n*cols)", ParamKind::F64, true);
        reg.register_command(std::move(s), &impl_trsm_lower_f64);
    }
    // ---- v0d-cli-extend: syrk / trmm / trsm.upper / gemm_parallel_auto ----
    {
        auto s = make_schema(alloc, "hesap.dense.blas3.syrk.f32",
            "C = alpha * A * A^T + beta * C (f32 symmetric rank-k).", OutputKind::BinaryBlob);
        add_param(s, alloc, "alpha", "Scalar multiplier", ParamKind::F64, false);
        add_param(s, alloc, "beta", "Scalar multiplier on C", ParamKind::F64, false);
        add_param(s, alloc, "m", "Rows of A; order of C", ParamKind::U64, true);
        add_param(s, alloc, "k", "Cols of A", ParamKind::U64, true);
        add_param(s, alloc, "A", "Matrix A flattened (m*k)", ParamKind::F64, true);
        add_param(s, alloc, "C", "Symmetric C flattened (m*m); lower triangle authoritative", ParamKind::F64, true);
        reg.register_command(std::move(s), &impl_syrk<crd::f32>);
    }
    {
        auto s = make_schema(alloc, "hesap.dense.blas3.syrk.f64",
            "C = alpha * A * A^T + beta * C (f64 symmetric rank-k).", OutputKind::BinaryBlob);
        add_param(s, alloc, "alpha", "Scalar multiplier", ParamKind::F64, false);
        add_param(s, alloc, "beta", "Scalar multiplier on C", ParamKind::F64, false);
        add_param(s, alloc, "m", "Rows of A; order of C", ParamKind::U64, true);
        add_param(s, alloc, "k", "Cols of A", ParamKind::U64, true);
        add_param(s, alloc, "A", "Matrix A flattened (m*k)", ParamKind::F64, true);
        add_param(s, alloc, "C", "Symmetric C flattened (m*m); lower triangle authoritative", ParamKind::F64, true);
        reg.register_command(std::move(s), &impl_syrk<crd::f64>);
    }
    {
        auto s = make_schema(alloc, "hesap.dense.blas3.trmm.lower.f32",
            "B = alpha * L * B in-place (f32 lower triangular multiply).", OutputKind::BinaryBlob);
        add_param(s, alloc, "alpha", "Scalar multiplier", ParamKind::F64, false);
        add_param(s, alloc, "n", "Order of L", ParamKind::U64, true);
        add_param(s, alloc, "cols", "Cols of B", ParamKind::U64, true);
        add_param(s, alloc, "A", "Lower triangular L flattened (n*n)", ParamKind::F64, true);
        add_param(s, alloc, "B", "Input/output B flattened row-major (n*cols)", ParamKind::F64, true);
        reg.register_command(std::move(s), &impl_trmm<crd::f32, TriangularSide::Lower>);
    }
    {
        auto s = make_schema(alloc, "hesap.dense.blas3.trmm.lower.f64",
            "B = alpha * L * B in-place (f64 lower triangular multiply).", OutputKind::BinaryBlob);
        add_param(s, alloc, "alpha", "Scalar multiplier", ParamKind::F64, false);
        add_param(s, alloc, "n", "Order of L", ParamKind::U64, true);
        add_param(s, alloc, "cols", "Cols of B", ParamKind::U64, true);
        add_param(s, alloc, "A", "Lower triangular L flattened (n*n)", ParamKind::F64, true);
        add_param(s, alloc, "B", "Input/output B flattened row-major (n*cols)", ParamKind::F64, true);
        reg.register_command(std::move(s), &impl_trmm<crd::f64, TriangularSide::Lower>);
    }
    {
        auto s = make_schema(alloc, "hesap.dense.blas3.trmm.upper.f32",
            "B = alpha * U * B in-place (f32 upper triangular multiply).", OutputKind::BinaryBlob);
        add_param(s, alloc, "alpha", "Scalar multiplier", ParamKind::F64, false);
        add_param(s, alloc, "n", "Order of U", ParamKind::U64, true);
        add_param(s, alloc, "cols", "Cols of B", ParamKind::U64, true);
        add_param(s, alloc, "A", "Upper triangular U flattened (n*n)", ParamKind::F64, true);
        add_param(s, alloc, "B", "Input/output B flattened row-major (n*cols)", ParamKind::F64, true);
        reg.register_command(std::move(s), &impl_trmm<crd::f32, TriangularSide::Upper>);
    }
    {
        auto s = make_schema(alloc, "hesap.dense.blas3.trmm.upper.f64",
            "B = alpha * U * B in-place (f64 upper triangular multiply).", OutputKind::BinaryBlob);
        add_param(s, alloc, "alpha", "Scalar multiplier", ParamKind::F64, false);
        add_param(s, alloc, "n", "Order of U", ParamKind::U64, true);
        add_param(s, alloc, "cols", "Cols of B", ParamKind::U64, true);
        add_param(s, alloc, "A", "Upper triangular U flattened (n*n)", ParamKind::F64, true);
        add_param(s, alloc, "B", "Input/output B flattened row-major (n*cols)", ParamKind::F64, true);
        reg.register_command(std::move(s), &impl_trmm<crd::f64, TriangularSide::Upper>);
    }
    {
        auto s = make_schema(alloc, "hesap.dense.blas3.trsm.upper.f32",
            "Solve alpha * U * X = B for X in-place (f32 upper).", OutputKind::BinaryBlob);
        add_param(s, alloc, "alpha", "Scalar multiplier", ParamKind::F64, false);
        add_param(s, alloc, "n", "Order of U", ParamKind::U64, true);
        add_param(s, alloc, "cols", "Cols of B", ParamKind::U64, true);
        add_param(s, alloc, "A", "Upper triangular U flattened (n*n)", ParamKind::F64, true);
        add_param(s, alloc, "B", "RHS B flattened row-major (n*cols)", ParamKind::F64, true);
        reg.register_command(std::move(s), &impl_trsm_upper<crd::f32>);
    }
    {
        auto s = make_schema(alloc, "hesap.dense.blas3.trsm.upper.f64",
            "Solve alpha * U * X = B for X in-place (f64 upper).", OutputKind::BinaryBlob);
        add_param(s, alloc, "alpha", "Scalar multiplier", ParamKind::F64, false);
        add_param(s, alloc, "n", "Order of U", ParamKind::U64, true);
        add_param(s, alloc, "cols", "Cols of B", ParamKind::U64, true);
        add_param(s, alloc, "A", "Upper triangular U flattened (n*n)", ParamKind::F64, true);
        add_param(s, alloc, "B", "RHS B flattened row-major (n*cols)", ParamKind::F64, true);
        reg.register_command(std::move(s), &impl_trsm_upper<crd::f64>);
    }
    {
        auto s = make_schema(alloc, "hesap.dense.blas3.gemm_parallel_auto.f32",
            "C = alpha * A * B + beta * C with heuristic worker-count picker (f32).",
            OutputKind::BinaryBlob);
        add_param(s, alloc, "alpha", "Scalar multiplier", ParamKind::F64, false);
        add_param(s, alloc, "beta", "Scalar multiplier on C", ParamKind::F64, false);
        add_param(s, alloc, "m", "Rows of A and C", ParamKind::U64, true);
        add_param(s, alloc, "k", "Cols of A / rows of B", ParamKind::U64, true);
        add_param(s, alloc, "n", "Cols of B and C", ParamKind::U64, true);
        add_param(s, alloc, "A", "Matrix A flattened row-major (m*k)", ParamKind::F64, true);
        add_param(s, alloc, "B", "Matrix B flattened row-major (k*n)", ParamKind::F64, true);
        add_param(s, alloc, "C", "Input/output C flattened row-major (m*n)", ParamKind::F64, true);
        reg.register_command(std::move(s), &impl_gemm_parallel_auto<crd::f32>);
    }
    {
        auto s = make_schema(alloc, "hesap.dense.blas3.gemm_parallel_auto.f64",
            "C = alpha * A * B + beta * C with heuristic worker-count picker (f64).",
            OutputKind::BinaryBlob);
        add_param(s, alloc, "alpha", "Scalar multiplier", ParamKind::F64, false);
        add_param(s, alloc, "beta", "Scalar multiplier on C", ParamKind::F64, false);
        add_param(s, alloc, "m", "Rows of A and C", ParamKind::U64, true);
        add_param(s, alloc, "k", "Cols of A / rows of B", ParamKind::U64, true);
        add_param(s, alloc, "n", "Cols of B and C", ParamKind::U64, true);
        add_param(s, alloc, "A", "Matrix A flattened row-major (m*k)", ParamKind::F64, true);
        add_param(s, alloc, "B", "Matrix B flattened row-major (k*n)", ParamKind::F64, true);
        add_param(s, alloc, "C", "Input/output C flattened row-major (m*n)", ParamKind::F64, true);
        reg.register_command(std::move(s), &impl_gemm_parallel_auto<crd::f64>);
    }
})
