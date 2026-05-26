// Iterative-solver CLI registration. Phase 3.1.6 v4a-1.
//
// Registers hesap.iterative.cg.{f32,f64,c32,c64} (4). Stateless wire shape
// (D14): the SPD/HPD matrix arrives as COO triplets, the RHS as a flattened
// f64 array `b`; the result blob is [iterations, final_residual, converged,
// x...] (x flattened {re,im,...} for complex). PCG and the least-squares
// solvers (LSQR/LSMR, which take an optional column preconditioner) live in the
// preconditioners module's CLI -- each needs both a solver header and a
// preconditioner.

#include <crd/hesap/cli/arg_value.hpp>
#include <crd/hesap/cli/command_registry.hpp>
#include <crd/hesap/complex.hpp>
#include <crd/hesap/dense/real_type.hpp>
#include <crd/hesap/iterative/block_bicgstab.hpp>
#include <crd/hesap/iterative/block_cg.hpp>
#include <crd/hesap/iterative/block_gmres.hpp>
#include <crd/hesap/iterative/cg.hpp>
#include <crd/hesap/iterative/cli_anchor.hpp>
#include <crd/hesap/sparse/block_linear_op.hpp>
#include <crd/hesap/sparse/sparse_linear_op.hpp>
#include <crd/hesap/sparse/triplet_builder.hpp>

#include <utility>

namespace crd::hesap::iterative
{
void register_iterative_cli_anchor() noexcept {}
} // namespace crd::hesap::iterative

namespace
{
using namespace crd::hesap;
using namespace crd::hesap::cli;
using namespace crd::hesap::iterative;
using crd::hesap::dense::is_complex_v;
using crd::hesap::dense::RealType;
using crd::hesap::sparse::SparseFormat;
using crd::hesap::sparse::SparseLinearOp;
using crd::hesap::sparse::SparseMatrix;
using crd::hesap::sparse::TripletBuilder;

CommandResult error_result(crd::memory::IAllocator* alloc, const char* msg)
{
    CommandResult r{alloc};
    r.ok = false;
    ResultError e{alloc};
    e.error_kind    = crd::containers::String{"InvalidArgument", alloc};
    e.error_message = crd::containers::String{msg, alloc};
    r.value         = std::move(e);
    return r;
}

CommandResult blob_f64_result(crd::memory::IAllocator* alloc, crd::containers::ConstSpan<crd::f64> values)
{
    CommandResult r{alloc};
    r.ok = true;
    ResultBinaryBlob blob{alloc};
    const auto*      raw     = reinterpret_cast<const crd::u8*>(values.data());
    const crd::usize n_bytes = values.size() * sizeof(crd::f64);
    blob.bytes.reserve(n_bytes);
    for (crd::usize i = 0; i < n_bytes; ++i)
    {
        blob.bytes.push_back(raw[i]);
    }
    r.value = std::move(blob);
    return r;
}

// Build a square SPD/HPD CSR matrix from COO triplets (real or complex flattened).
// Only the (square) CG path lives here now; the rectangular least-squares builder
// moved out with LSQR/LSMR to the preconditioners module.
template <typename T>
SparseMatrix<T, SparseFormat::Csr> build_csr(const CommandArgs& args, crd::u32 n)
{
    const auto   tr   = args.get_i64_array("triplet_rows");
    const auto   tc   = args.get_i64_array("triplet_cols");
    const auto   vals = args.get_f64_array("values");
    TripletBuilder<T> b(args.alloc, n, n);
    for (crd::usize k = 0; k < tr.size(); ++k)
    {
        if constexpr (is_complex_v<T>)
        {
            using U = RealType<T>;
            b.add(static_cast<crd::u32>(tr[k]), static_cast<crd::u32>(tc[k]),
                  T{static_cast<U>(vals[2 * k]), static_cast<U>(vals[2 * k + 1])});
        }
        else
        {
            b.add(static_cast<crd::u32>(tr[k]), static_cast<crd::u32>(tc[k]), static_cast<T>(vals[k]));
        }
    }
    return b.compress();
}

template <typename T>
CommandResult impl_cg(const CommandArgs& args)
{
    using R = RealType<T>;
    const auto rows = args.get_u64("rows");
    const auto cols = args.get_u64("cols");
    if (!rows || !cols || *rows != *cols)
    {
        return error_result(args.alloc, "cg: rows and cols are required and must be equal (square SPD/HPD)");
    }
    const crd::u32 n = static_cast<crd::u32>(*rows);

    const auto       bin    = args.get_f64_array("b");
    const crd::usize expect = is_complex_v<T> ? static_cast<crd::usize>(n) * 2 : static_cast<crd::usize>(n);
    if (bin.size() != expect)
    {
        return error_result(args.alloc, "cg: b has wrong length (n real, or 2n flattened complex)");
    }

    auto                a  = build_csr<T>(args, n);
    SparseLinearOp<T>   op(a);
    crd::hesap::dense::Vector<T> bvec(args.alloc, n);
    crd::hesap::dense::Vector<T> xvec(args.alloc, n); // x0 = 0
    for (crd::u32 i = 0; i < n; ++i)
    {
        if constexpr (is_complex_v<T>)
        {
            bvec(i) = T{static_cast<R>(bin[2 * i]), static_cast<R>(bin[2 * i + 1])};
        }
        else
        {
            bvec(i) = static_cast<T>(bin[i]);
        }
    }

    IterativeOptions<R> opts;
    if (const auto rt = args.get_f64("rel_tol"))
    {
        opts.rel_tol = static_cast<R>(*rt);
    }
    if (const auto mi = args.get_u64("max_iter"))
    {
        opts.max_iter = static_cast<crd::usize>(*mi);
    }

    KrylovWorkspace<T> ws(args.alloc, n);
    auto               res = cg<T>(op, bvec.span(), xvec.span(), opts, ws, args.alloc);

    crd::containers::Array<crd::f64> out(args.alloc);
    out.push_back(static_cast<crd::f64>(res.iterations));
    out.push_back(static_cast<crd::f64>(res.final_residual_norm));
    out.push_back(res.converged ? 1.0 : 0.0);
    for (crd::u32 i = 0; i < n; ++i)
    {
        if constexpr (is_complex_v<T>)
        {
            out.push_back(static_cast<crd::f64>(xvec(i).re));
            out.push_back(static_cast<crd::f64>(xvec(i).im));
        }
        else
        {
            out.push_back(static_cast<crd::f64>(xvec(i)));
        }
    }
    return blob_f64_result(args.alloc, crd::containers::ConstSpan<crd::f64>{out.data(), out.size()});
}

// hesap.iterative.block_cg.<T> : multi-RHS block-CG. A SPD/HPD (rows==cols); B is
// n×s ROW-MAJOR flattened (b[k*s+j]; complex {re,im} interleaved). Returns
// [iters, 0, converged, X...] (X n×s row-major flattened).
template <typename T>
CommandResult impl_block_cg(const CommandArgs& args)
{
    using R         = RealType<T>;
    const auto rows = args.get_u64("rows");
    const auto cols = args.get_u64("cols");
    const auto srhs = args.get_u64("s");
    if (!rows || !cols || *rows != *cols || !srhs || *srhs < 1)
    {
        return error_result(args.alloc, "block_cg: rows==cols and s>=1 are required");
    }
    const crd::u32   n      = static_cast<crd::u32>(*rows);
    const crd::u32   s      = static_cast<crd::u32>(*srhs);
    const auto       bin    = args.get_f64_array("b");
    const crd::usize expect = (is_complex_v<T> ? 2U : 1U) * static_cast<crd::usize>(n) * s;
    if (bin.size() != expect)
    {
        return error_result(args.alloc, "block_cg: b must be n*s (real) or 2*n*s (complex) flattened row-major");
    }

    auto a = build_csr<T>(args, n);
    crd::hesap::sparse::ParallelSpmmLinearOp<T> op(a, ~crd::usize{0}); // serial spmm (CLI = oracle)
    crd::hesap::dense::Vector<T> bvec(args.alloc, static_cast<crd::usize>(n) * s);
    crd::hesap::dense::Vector<T> xvec(args.alloc, static_cast<crd::usize>(n) * s);
    for (crd::usize idx = 0; idx < static_cast<crd::usize>(n) * s; ++idx)
    {
        if constexpr (is_complex_v<T>)
        {
            bvec(idx) = T{static_cast<R>(bin[2 * idx]), static_cast<R>(bin[2 * idx + 1])};
        }
        else
        {
            bvec(idx) = static_cast<T>(bin[idx]);
        }
    }

    IterativeOptions<R> opts;
    if (const auto rt = args.get_f64("rel_tol")) { opts.rel_tol = static_cast<R>(*rt); }
    if (const auto mi = args.get_u64("max_iter")) { opts.max_iter = static_cast<crd::usize>(*mi); }

    BlockCgWorkspace<T> ws(args.alloc, n, s);
    auto                res = block_cg<T>(op, bvec.span(), xvec.span(), opts, ws, args.alloc);

    crd::containers::Array<crd::f64> out(args.alloc);
    out.push_back(static_cast<crd::f64>(res.iterations));
    out.push_back(static_cast<crd::f64>(res.final_residual_norm));
    out.push_back(res.converged ? 1.0 : 0.0);
    for (crd::usize idx = 0; idx < static_cast<crd::usize>(n) * s; ++idx)
    {
        if constexpr (is_complex_v<T>)
        {
            out.push_back(static_cast<crd::f64>(xvec(idx).re));
            out.push_back(static_cast<crd::f64>(xvec(idx).im));
        }
        else
        {
            out.push_back(static_cast<crd::f64>(xvec(idx)));
        }
    }
    return blob_f64_result(args.alloc, crd::containers::ConstSpan<crd::f64>{out.data(), out.size()});
}

// hesap.iterative.block_gmres.<T> : multi-RHS block-GMRES(m) on a GENERAL square matrix.
// B is n×s ROW-MAJOR flattened. Returns [iters, resid, converged, X(n×s)].
template <typename T>
CommandResult impl_block_gmres(const CommandArgs& args)
{
    using R         = RealType<T>;
    const auto rows = args.get_u64("rows");
    const auto cols = args.get_u64("cols");
    const auto srhs = args.get_u64("s");
    if (!rows || !cols || *rows != *cols || !srhs || *srhs < 1)
    {
        return error_result(args.alloc, "block_gmres: rows==cols and s>=1 are required");
    }
    const crd::u32   n      = static_cast<crd::u32>(*rows);
    const crd::u32   s      = static_cast<crd::u32>(*srhs);
    const auto       bin    = args.get_f64_array("b");
    const crd::usize expect = (is_complex_v<T> ? 2U : 1U) * static_cast<crd::usize>(n) * s;
    if (bin.size() != expect)
    {
        return error_result(args.alloc, "block_gmres: b must be n*s (real) or 2*n*s (complex) flattened row-major");
    }
    crd::usize restart = static_cast<crd::usize>(args.get_u64("restart").value_or(30));
    if (restart < 1) { restart = 1; }
    if (restart > n) { restart = n; }

    auto a = build_csr<T>(args, n);
    crd::hesap::sparse::ParallelSpmmLinearOp<T> op(a, ~crd::usize{0}); // serial spmm (CLI = oracle)
    crd::hesap::dense::Vector<T> bvec(args.alloc, static_cast<crd::usize>(n) * s);
    crd::hesap::dense::Vector<T> xvec(args.alloc, static_cast<crd::usize>(n) * s);
    for (crd::usize idx = 0; idx < static_cast<crd::usize>(n) * s; ++idx)
    {
        if constexpr (is_complex_v<T>) { bvec(idx) = T{static_cast<R>(bin[2 * idx]), static_cast<R>(bin[2 * idx + 1])}; }
        else { bvec(idx) = static_cast<T>(bin[idx]); }
    }
    IterativeOptions<R> opts;
    if (const auto rt = args.get_f64("rel_tol")) { opts.rel_tol = static_cast<R>(*rt); }
    if (const auto mi = args.get_u64("max_iter")) { opts.max_iter = static_cast<crd::usize>(*mi); }

    BlockGmresWorkspace<T> ws(args.alloc, n, s, restart);
    auto                   res = block_gmres<T>(op, bvec.span(), xvec.span(), opts, ws, args.alloc);

    crd::containers::Array<crd::f64> out(args.alloc);
    out.push_back(static_cast<crd::f64>(res.iterations));
    out.push_back(static_cast<crd::f64>(res.final_residual_norm));
    out.push_back(res.converged ? 1.0 : 0.0);
    for (crd::usize idx = 0; idx < static_cast<crd::usize>(n) * s; ++idx)
    {
        if constexpr (is_complex_v<T>) { out.push_back(static_cast<crd::f64>(xvec(idx).re)); out.push_back(static_cast<crd::f64>(xvec(idx).im)); }
        else { out.push_back(static_cast<crd::f64>(xvec(idx))); }
    }
    return blob_f64_result(args.alloc, crd::containers::ConstSpan<crd::f64>{out.data(), out.size()});
}

// hesap.iterative.block_bicgstab.<T> : multi-RHS block-BiCGSTAB on a GENERAL square matrix.
template <typename T>
CommandResult impl_block_bicgstab(const CommandArgs& args)
{
    using R         = RealType<T>;
    const auto rows = args.get_u64("rows");
    const auto cols = args.get_u64("cols");
    const auto srhs = args.get_u64("s");
    if (!rows || !cols || *rows != *cols || !srhs || *srhs < 1)
    {
        return error_result(args.alloc, "block_bicgstab: rows==cols and s>=1 are required");
    }
    const crd::u32   n      = static_cast<crd::u32>(*rows);
    const crd::u32   s      = static_cast<crd::u32>(*srhs);
    const auto       bin    = args.get_f64_array("b");
    const crd::usize expect = (is_complex_v<T> ? 2U : 1U) * static_cast<crd::usize>(n) * s;
    if (bin.size() != expect)
    {
        return error_result(args.alloc, "block_bicgstab: b must be n*s (real) or 2*n*s (complex) flattened row-major");
    }

    auto a = build_csr<T>(args, n);
    crd::hesap::sparse::ParallelSpmmLinearOp<T> op(a, ~crd::usize{0});
    crd::hesap::dense::Vector<T> bvec(args.alloc, static_cast<crd::usize>(n) * s);
    crd::hesap::dense::Vector<T> xvec(args.alloc, static_cast<crd::usize>(n) * s);
    for (crd::usize idx = 0; idx < static_cast<crd::usize>(n) * s; ++idx)
    {
        if constexpr (is_complex_v<T>) { bvec(idx) = T{static_cast<R>(bin[2 * idx]), static_cast<R>(bin[2 * idx + 1])}; }
        else { bvec(idx) = static_cast<T>(bin[idx]); }
    }
    IterativeOptions<R> opts;
    if (const auto rt = args.get_f64("rel_tol")) { opts.rel_tol = static_cast<R>(*rt); }
    if (const auto mi = args.get_u64("max_iter")) { opts.max_iter = static_cast<crd::usize>(*mi); }

    BlockBicgstabWorkspace<T> ws(args.alloc, n, s);
    auto                      res = block_bicgstab<T>(op, bvec.span(), xvec.span(), opts, ws, args.alloc);

    crd::containers::Array<crd::f64> out(args.alloc);
    out.push_back(static_cast<crd::f64>(res.iterations));
    out.push_back(static_cast<crd::f64>(res.final_residual_norm));
    out.push_back(res.converged ? 1.0 : 0.0);
    for (crd::usize idx = 0; idx < static_cast<crd::usize>(n) * s; ++idx)
    {
        if constexpr (is_complex_v<T>) { out.push_back(static_cast<crd::f64>(xvec(idx).re)); out.push_back(static_cast<crd::f64>(xvec(idx).im)); }
        else { out.push_back(static_cast<crd::f64>(xvec(idx))); }
    }
    return blob_f64_result(args.alloc, crd::containers::ConstSpan<crd::f64>{out.data(), out.size()});
}

void add_param(CommandSchema& s, crd::memory::IAllocator* alloc, const char* name, const char* desc, ParamKind kind,
               bool required)
{
    ParamSchema p{alloc};
    p.name        = crd::containers::String{name, alloc};
    p.description = crd::containers::String{desc, alloc};
    p.kind        = kind;
    p.required    = required;
    s.params.push_back(std::move(p));
}

CommandSchema make_cg_schema(crd::memory::IAllocator* alloc, const char* name, const char* desc)
{
    CommandSchema s{alloc};
    s.name               = crd::containers::String{name, alloc};
    s.description        = crd::containers::String{desc, alloc};
    s.output.kind        = OutputKind::BinaryBlob;
    s.required_caps.bits = Capability::kHesapCompute;
    s.idempotent         = true;
    add_param(s, alloc, "rows", "Matrix rows (== cols; square SPD/HPD)", ParamKind::U64, true);
    add_param(s, alloc, "cols", "Matrix columns (== rows)", ParamKind::U64, true);
    add_param(s, alloc, "triplet_rows", "COO row indices (I64Array)", ParamKind::I64, true);
    add_param(s, alloc, "triplet_cols", "COO column indices (I64Array)", ParamKind::I64, true);
    add_param(s, alloc, "values", "COO values (F64Array; complex flattened {re,im,...})", ParamKind::F64, true);
    add_param(s, alloc, "b", "RHS vector (F64Array; n real or 2n flattened complex)", ParamKind::F64, true);
    add_param(s, alloc, "rel_tol", "Relative residual tolerance (default per type)", ParamKind::F64, false);
    add_param(s, alloc, "max_iter", "Maximum iterations (default 1000)", ParamKind::U64, false);
    return s;
}

CommandSchema make_block_cg_schema(crd::memory::IAllocator* alloc, const char* name, const char* desc)
{
    CommandSchema s{alloc};
    s.name               = crd::containers::String{name, alloc};
    s.description        = crd::containers::String{desc, alloc};
    s.output.kind        = OutputKind::BinaryBlob;
    s.required_caps.bits = Capability::kHesapCompute;
    s.idempotent         = true;
    add_param(s, alloc, "rows", "Matrix rows (== cols; square SPD/HPD)", ParamKind::U64, true);
    add_param(s, alloc, "cols", "Matrix columns (== rows)", ParamKind::U64, true);
    add_param(s, alloc, "s", "Number of right-hand sides (block width)", ParamKind::U64, true);
    add_param(s, alloc, "triplet_rows", "COO row indices (I64Array)", ParamKind::I64, true);
    add_param(s, alloc, "triplet_cols", "COO column indices (I64Array)", ParamKind::I64, true);
    add_param(s, alloc, "values", "COO values (F64Array; complex flattened)", ParamKind::F64, true);
    add_param(s, alloc, "b", "RHS block n×s row-major (F64Array; n*s real or 2*n*s complex)", ParamKind::F64, true);
    add_param(s, alloc, "rel_tol", "Relative residual tolerance (default per type)", ParamKind::F64, false);
    add_param(s, alloc, "max_iter", "Maximum iterations (default 1000)", ParamKind::U64, false);
    return s;
}

CommandSchema make_block_gmres_schema(crd::memory::IAllocator* alloc, const char* name, const char* desc)
{
    CommandSchema s = make_block_cg_schema(alloc, name, desc); // rows/cols/s/triplets/b/rel_tol/max_iter
    add_param(s, alloc, "restart", "Block-GMRES restart length m (default 30, capped to n)", ParamKind::U64, false);
    return s;
}

} // namespace

CRD_HESAP_CLI_REGISTER_MODULE([](CommandRegistry& reg) {
    auto* alloc = crd::memory::default_allocator();
    reg.register_command(make_cg_schema(alloc, "hesap.iterative.cg.f32",
                                        "Conjugate Gradient on an SPD matrix (f32). Returns [iters,resid,converged,x]."),
                         &impl_cg<crd::f32>);
    reg.register_command(make_cg_schema(alloc, "hesap.iterative.cg.f64",
                                        "Conjugate Gradient on an SPD matrix (f64). Returns [iters,resid,converged,x]."),
                         &impl_cg<crd::f64>);
    reg.register_command(
        make_cg_schema(alloc, "hesap.iterative.cg.c32",
                       "Conjugate Gradient on an HPD matrix (Complex<f32>). Returns [iters,resid,converged,x]."),
        &impl_cg<crd::hesap::Complex<crd::f32>>);
    reg.register_command(
        make_cg_schema(alloc, "hesap.iterative.cg.c64",
                       "Conjugate Gradient on an HPD matrix (Complex<f64>). Returns [iters,resid,converged,x]."),
        &impl_cg<crd::hesap::Complex<crd::f64>>);

    reg.register_command(make_block_cg_schema(alloc, "hesap.iterative.block_cg.f32",
                                              "Block-CG multi-RHS on an SPD matrix (f32). [iters,resid,converged,X(n×s)]."),
                         &impl_block_cg<crd::f32>);
    reg.register_command(make_block_cg_schema(alloc, "hesap.iterative.block_cg.f64",
                                              "Block-CG multi-RHS on an SPD matrix (f64). [iters,resid,converged,X(n×s)]."),
                         &impl_block_cg<crd::f64>);
    reg.register_command(make_block_cg_schema(alloc, "hesap.iterative.block_cg.c32", "Block-CG multi-RHS on an HPD matrix (Complex<f32>)."),
                         &impl_block_cg<crd::hesap::Complex<crd::f32>>);
    reg.register_command(make_block_cg_schema(alloc, "hesap.iterative.block_cg.c64", "Block-CG multi-RHS on an HPD matrix (Complex<f64>)."),
                         &impl_block_cg<crd::hesap::Complex<crd::f64>>);

    reg.register_command(make_block_gmres_schema(alloc, "hesap.iterative.block_gmres.f32",
                                                 "Block-GMRES(m) multi-RHS on a general matrix (f32). [iters,resid,converged,X(n×s)]."),
                         &impl_block_gmres<crd::f32>);
    reg.register_command(make_block_gmres_schema(alloc, "hesap.iterative.block_gmres.f64",
                                                 "Block-GMRES(m) multi-RHS on a general matrix (f64). [iters,resid,converged,X(n×s)]."),
                         &impl_block_gmres<crd::f64>);
    reg.register_command(make_block_gmres_schema(alloc, "hesap.iterative.block_gmres.c32", "Block-GMRES(m) multi-RHS on a general matrix (Complex<f32>)."),
                         &impl_block_gmres<crd::hesap::Complex<crd::f32>>);
    reg.register_command(make_block_gmres_schema(alloc, "hesap.iterative.block_gmres.c64", "Block-GMRES(m) multi-RHS on a general matrix (Complex<f64>)."),
                         &impl_block_gmres<crd::hesap::Complex<crd::f64>>);

    reg.register_command(make_block_cg_schema(alloc, "hesap.iterative.block_bicgstab.f32",
                                              "Block-BiCGSTAB multi-RHS on a general matrix (f32). [iters,resid,converged,X(n×s)]."),
                         &impl_block_bicgstab<crd::f32>);
    reg.register_command(make_block_cg_schema(alloc, "hesap.iterative.block_bicgstab.f64",
                                              "Block-BiCGSTAB multi-RHS on a general matrix (f64). [iters,resid,converged,X(n×s)]."),
                         &impl_block_bicgstab<crd::f64>);
    reg.register_command(make_block_cg_schema(alloc, "hesap.iterative.block_bicgstab.c32", "Block-BiCGSTAB multi-RHS on a general matrix (Complex<f32>)."),
                         &impl_block_bicgstab<crd::hesap::Complex<crd::f32>>);
    reg.register_command(make_block_cg_schema(alloc, "hesap.iterative.block_bicgstab.c64", "Block-BiCGSTAB multi-RHS on a general matrix (Complex<f64>)."),
                         &impl_block_bicgstab<crd::hesap::Complex<crd::f64>>);
});
