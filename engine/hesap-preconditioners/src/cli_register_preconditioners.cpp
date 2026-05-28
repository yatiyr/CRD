// Preconditioner + PCG CLI registration. Phase 3.1.6 v4a-1.
//
// Registers hesap.precond.jacobi.{f32,f64,c32,c64} (apply z = M⁻¹ r) and
// hesap.iterative.pcg.{f32,f64,c32,c64} (Jacobi-preconditioned CG) -- 8
// commands. PCG lives here (not in crd-hesap-iterative) because it needs BOTH
// the solver header (cg.hpp, header-only) and a concrete preconditioner; the
// dependency is one-way (preconditioners -> iterative).

#include <crd/hesap/cli/arg_value.hpp>
#include <crd/hesap/cli/command_registry.hpp>
#include <crd/hesap/complex.hpp>
#include <crd/hesap/dense/real_type.hpp>
#include <crd/hesap/iterative/bicgstab.hpp>
#include <crd/hesap/iterative/block_cg.hpp>
#include <crd/hesap/iterative/cg.hpp>
#include <crd/hesap/iterative/gcr.hpp>
#include <crd/hesap/iterative/gcrot.hpp>
#include <crd/hesap/iterative/gmres.hpp>
#include <crd/hesap/iterative/idrs.hpp>
#include <crd/hesap/iterative/lsmr.hpp>
#include <crd/hesap/iterative/lsqr.hpp>
#include <crd/hesap/iterative/minres.hpp>
#include <crd/hesap/iterative/qmr.hpp>
#include <crd/hesap/iterative/rminres.hpp>
#include <crd/hesap/iterative/symmlq.hpp>
#include <crd/hesap/preconditioners/block_jacobi.hpp>
#include <crd/hesap/preconditioners/block_preconditioner.hpp>
#include <crd/hesap/preconditioners/chebyshev.hpp>
#include <crd/hesap/preconditioners/cli_anchor.hpp>
#include <crd/hesap/preconditioners/column_jacobi.hpp>
#include <crd/hesap/preconditioners/fspai.hpp>
#include <crd/hesap/preconditioners/ic0.hpp>
#include <crd/hesap/preconditioners/ilu0.hpp>
#include <crd/hesap/preconditioners/ilup.hpp>
#include <crd/hesap/preconditioners/ilut.hpp>
#include <crd/hesap/preconditioners/inverse_based_ilu.hpp>
#include <crd/hesap/preconditioners/jacobi.hpp>
#include <crd/hesap/preconditioners/multilevel_ilu.hpp>
#include <crd/hesap/preconditioners/schwarz.hpp>
#include <crd/hesap/preconditioners/spai.hpp>
#include <crd/hesap/preconditioners/ssor.hpp>
#include <crd/hesap/sparse/block_linear_op.hpp>
#include <crd/hesap/sparse/sparse_linear_op.hpp>
#include <crd/hesap/sparse/triplet_builder.hpp>

#include <utility>

namespace crd::hesap::preconditioners
{
void register_preconditioners_cli_anchor() noexcept {}
} // namespace crd::hesap::preconditioners

namespace
{
using namespace crd::hesap;
using namespace crd::hesap::cli;
using namespace crd::hesap::preconditioners;
using crd::hesap::dense::is_complex_v;
using crd::hesap::dense::RealType;
using crd::hesap::iterative::IterativeOptions;
using crd::hesap::iterative::KrylovWorkspace;
using crd::hesap::sparse::SparseFormat;
using crd::hesap::sparse::SparseLinearOp;
using crd::hesap::sparse::SparseMatrix;
using crd::hesap::sparse::TripletBuilder;

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

template <typename T> SparseMatrix<T, SparseFormat::Csr> build_csr(const CommandArgs& args, crd::u32 n)
{
    const auto tr = args.get_i64_array("triplet_rows");
    const auto tc = args.get_i64_array("triplet_cols");
    const auto vals = args.get_f64_array("values");
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

// Rectangular (m×n) COO -> CSR for the least-squares solvers.
template <typename T> SparseMatrix<T, SparseFormat::Csr> build_csr_rect(const CommandArgs& args, crd::u32 m, crd::u32 n)
{
    const auto tr = args.get_i64_array("triplet_rows");
    const auto tc = args.get_i64_array("triplet_cols");
    const auto vals = args.get_f64_array("values");
    TripletBuilder<T> b(args.alloc, m, n);
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
void read_vec(const CommandArgs& args, const char* key, crd::hesap::dense::Vector<T>& out, crd::u32 n)
{
    using R = RealType<T>;
    const auto data = args.get_f64_array(key);
    for (crd::u32 i = 0; i < n; ++i)
    {
        if constexpr (is_complex_v<T>)
        {
            out(i) = T{static_cast<R>(data[2 * i]), static_cast<R>(data[2 * i + 1])};
        }
        else
        {
            out(i) = static_cast<T>(data[i]);
        }
    }
}

template <typename T>
void push_vec(crd::containers::Array<crd::f64>& out, const crd::hesap::dense::Vector<T>& v, crd::u32 n)
{
    for (crd::u32 i = 0; i < n; ++i)
    {
        if constexpr (is_complex_v<T>)
        {
            out.push_back(static_cast<crd::f64>(v(i).re));
            out.push_back(static_cast<crd::f64>(v(i).im));
        }
        else
        {
            out.push_back(static_cast<crd::f64>(v(i)));
        }
    }
}

// hesap.precond.block_jacobi.<T> : z = M⁻¹ r, M = block-diag(A).
template <typename T> CommandResult impl_block_jacobi(const CommandArgs& args)
{
    const auto rows = args.get_u64("rows");
    if (!rows)
    {
        return error_result(args.alloc, "block_jacobi: rows (== cols) is required");
    }
    const crd::u32 n = static_cast<crd::u32>(*rows);
    const crd::u32 bs = static_cast<crd::u32>(args.get_u64("block_size").value_or(4U));
    const auto rin = args.get_f64_array("r");
    const crd::usize expect = is_complex_v<T> ? static_cast<crd::usize>(n) * 2 : static_cast<crd::usize>(n);
    if (rin.size() != expect)
    {
        return error_result(args.alloc, "block_jacobi: r has wrong length");
    }
    auto a = build_csr<T>(args, n);
    BlockJacobiPreconditioner<T> m(a, bs, args.alloc);
    crd::hesap::dense::Vector<T> r(args.alloc, n);
    crd::hesap::dense::Vector<T> z(args.alloc, n);
    read_vec<T>(args, "r", r, n);
    (void)m.apply(r.span(), z.span());
    crd::containers::Array<crd::f64> out(args.alloc);
    push_vec<T>(out, z, n);
    return blob_f64_result(args.alloc, crd::containers::ConstSpan<crd::f64>{out.data(), out.size()});
}

// hesap.precond.ssor.<T> : z = M_SSOR⁻¹ r.
template <typename T> CommandResult impl_ssor(const CommandArgs& args)
{
    using R = RealType<T>;
    const auto rows = args.get_u64("rows");
    if (!rows)
    {
        return error_result(args.alloc, "ssor: rows (== cols) is required");
    }
    const crd::u32 n = static_cast<crd::u32>(*rows);
    const R omega = static_cast<R>(args.get_f64("omega").value_or(1.0));
    const auto rin = args.get_f64_array("r");
    const crd::usize expect = is_complex_v<T> ? static_cast<crd::usize>(n) * 2 : static_cast<crd::usize>(n);
    if (rin.size() != expect)
    {
        return error_result(args.alloc, "ssor: r has wrong length");
    }
    auto a = build_csr<T>(args, n);
    SsorPreconditioner<T> m(a, omega, args.alloc);
    crd::hesap::dense::Vector<T> r(args.alloc, n);
    crd::hesap::dense::Vector<T> z(args.alloc, n);
    read_vec<T>(args, "r", r, n);
    (void)m.apply(r.span(), z.span());
    crd::containers::Array<crd::f64> out(args.alloc);
    push_vec<T>(out, z, n);
    return blob_f64_result(args.alloc, crd::containers::ConstSpan<crd::f64>{out.data(), out.size()});
}

// hesap.precond.jacobi.<T> : z = M⁻¹ r, M = diag(A).
template <typename T> CommandResult impl_jacobi(const CommandArgs& args)
{
    const auto rows = args.get_u64("rows");
    if (!rows)
    {
        return error_result(args.alloc, "jacobi: rows (== cols) is required");
    }
    const crd::u32 n = static_cast<crd::u32>(*rows);
    const auto rin = args.get_f64_array("r");
    const crd::usize expect = is_complex_v<T> ? static_cast<crd::usize>(n) * 2 : static_cast<crd::usize>(n);
    if (rin.size() != expect)
    {
        return error_result(args.alloc, "jacobi: r has wrong length (n real or 2n flattened complex)");
    }
    auto a = build_csr<T>(args, n);
    JacobiPreconditioner<T> m(a, args.alloc);
    crd::hesap::dense::Vector<T> r(args.alloc, n);
    crd::hesap::dense::Vector<T> z(args.alloc, n);
    read_vec<T>(args, "r", r, n);
    (void)m.apply(r.span(), z.span());
    crd::containers::Array<crd::f64> out(args.alloc);
    push_vec<T>(out, z, n);
    return blob_f64_result(args.alloc, crd::containers::ConstSpan<crd::f64>{out.data(), out.size()});
}

// hesap.precond.ic0.<T> : z = M⁻¹ r, M = L·Lᴴ (incomplete Cholesky level 0; SPD/HPD A).
template <typename T> CommandResult impl_ic0(const CommandArgs& args)
{
    const auto rows = args.get_u64("rows");
    if (!rows)
    {
        return error_result(args.alloc, "ic0: rows (== cols) is required");
    }
    const crd::u32 n = static_cast<crd::u32>(*rows);
    const auto rin = args.get_f64_array("r");
    const crd::usize expect = is_complex_v<T> ? static_cast<crd::usize>(n) * 2 : static_cast<crd::usize>(n);
    if (rin.size() != expect)
    {
        return error_result(args.alloc, "ic0: r has wrong length");
    }
    auto a = build_csr<T>(args, n);
    Ic0Preconditioner<T> m(a, args.alloc);
    crd::hesap::dense::Vector<T> r(args.alloc, n);
    crd::hesap::dense::Vector<T> z(args.alloc, n);
    read_vec<T>(args, "r", r, n);
    (void)m.apply(r.span(), z.span());
    crd::containers::Array<crd::f64> out(args.alloc);
    push_vec<T>(out, z, n);
    return blob_f64_result(args.alloc, crd::containers::ConstSpan<crd::f64>{out.data(), out.size()});
}

// hesap.precond.ilu0.<T> : z = M⁻¹ r, M = L·U (incomplete LU level 0; general A).
template <typename T> CommandResult impl_ilu0(const CommandArgs& args)
{
    const auto rows = args.get_u64("rows");
    if (!rows)
    {
        return error_result(args.alloc, "ilu0: rows (== cols) is required");
    }
    const crd::u32 n = static_cast<crd::u32>(*rows);
    const auto rin = args.get_f64_array("r");
    const crd::usize expect = is_complex_v<T> ? static_cast<crd::usize>(n) * 2 : static_cast<crd::usize>(n);
    if (rin.size() != expect)
    {
        return error_result(args.alloc, "ilu0: r has wrong length");
    }
    auto a = build_csr<T>(args, n);
    Ilu0Preconditioner<T> m(a, args.alloc);
    crd::hesap::dense::Vector<T> r(args.alloc, n);
    crd::hesap::dense::Vector<T> z(args.alloc, n);
    read_vec<T>(args, "r", r, n);
    (void)m.apply(r.span(), z.span());
    crd::containers::Array<crd::f64> out(args.alloc);
    push_vec<T>(out, z, n);
    return blob_f64_result(args.alloc, crd::containers::ConstSpan<crd::f64>{out.data(), out.size()});
}

// hesap.precond.ilup.<T> : z = M⁻¹ r, M = L·U (level-of-fill ILU(p); `level` param).
template <typename T> CommandResult impl_ilup(const CommandArgs& args)
{
    const auto rows = args.get_u64("rows");
    if (!rows)
    {
        return error_result(args.alloc, "ilup: rows (== cols) is required");
    }
    const crd::u32 n = static_cast<crd::u32>(*rows);
    const auto rin = args.get_f64_array("r");
    const crd::usize expect = is_complex_v<T> ? static_cast<crd::usize>(n) * 2 : static_cast<crd::usize>(n);
    if (rin.size() != expect)
    {
        return error_result(args.alloc, "ilup: r has wrong length");
    }
    const crd::u32 level = static_cast<crd::u32>(args.get_u64("level").value_or(0U));
    auto a = build_csr<T>(args, n);
    IlupPreconditioner<T> m(a, args.alloc, level);
    crd::hesap::dense::Vector<T> r(args.alloc, n);
    crd::hesap::dense::Vector<T> z(args.alloc, n);
    read_vec<T>(args, "r", r, n);
    (void)m.apply(r.span(), z.span());
    crd::containers::Array<crd::f64> out(args.alloc);
    push_vec<T>(out, z, n);
    return blob_f64_result(args.alloc, crd::containers::ConstSpan<crd::f64>{out.data(), out.size()});
}

// hesap.precond.ilut.<T> : z = M⁻¹ r, M = L·U (dual-threshold ILUT; lfil/droptol params).
template <typename T> CommandResult impl_ilut(const CommandArgs& args)
{
    using R = RealType<T>;
    const auto rows = args.get_u64("rows");
    if (!rows)
    {
        return error_result(args.alloc, "ilut: rows (== cols) is required");
    }
    const crd::u32 n = static_cast<crd::u32>(*rows);
    const auto rin = args.get_f64_array("r");
    const crd::usize expect = is_complex_v<T> ? static_cast<crd::usize>(n) * 2 : static_cast<crd::usize>(n);
    if (rin.size() != expect)
    {
        return error_result(args.alloc, "ilut: r has wrong length");
    }
    const crd::u32 lfil = static_cast<crd::u32>(args.get_u64("lfil").value_or(0U));
    const R droptol = static_cast<R>(args.get_f64("droptol").value_or(-1.0));
    auto a = build_csr<T>(args, n);
    IlutPreconditioner<T> m(a, args.alloc, lfil, droptol);
    crd::hesap::dense::Vector<T> r(args.alloc, n);
    crd::hesap::dense::Vector<T> z(args.alloc, n);
    read_vec<T>(args, "r", r, n);
    (void)m.apply(r.span(), z.span());
    crd::containers::Array<crd::f64> out(args.alloc);
    push_vec<T>(out, z, n);
    return blob_f64_result(args.alloc, crd::containers::ConstSpan<crd::f64>{out.data(), out.size()});
}

// Read the SPAI/FSPAI pattern knobs (shared by the standalone commands + the selectors).
template <typename T> SpaiPattern read_spai_pattern(const CommandArgs& args)
{
    return args.get_string("spai_pattern") == crd::containers::StringView{"adaptive"} ? SpaiPattern::Adaptive
                                                                                      : SpaiPattern::Static;
}

// hesap.precond.spai.<T> : z = M⁻¹ r, M ≈ A⁻¹ (classical right-SPAI; general A).
template <typename T> CommandResult impl_spai(const CommandArgs& args)
{
    using R = RealType<T>;
    const auto rows = args.get_u64("rows");
    if (!rows)
    {
        return error_result(args.alloc, "spai: rows (== cols) is required");
    }
    const crd::u32 n = static_cast<crd::u32>(*rows);
    const auto rin = args.get_f64_array("r");
    const crd::usize expect = is_complex_v<T> ? static_cast<crd::usize>(n) * 2 : static_cast<crd::usize>(n);
    if (rin.size() != expect)
    {
        return error_result(args.alloc, "spai: r has wrong length");
    }
    const R eps = static_cast<R>(args.get_f64("spai_eps").value_or(0.4));
    const crd::u32 fill = static_cast<crd::u32>(args.get_u64("spai_fill").value_or(0U));
    auto a = build_csr<T>(args, n);
    SpaiPreconditioner<T> m(a, args.alloc, read_spai_pattern<T>(args), eps, fill);
    crd::hesap::dense::Vector<T> r(args.alloc, n);
    crd::hesap::dense::Vector<T> z(args.alloc, n);
    read_vec<T>(args, "r", r, n);
    (void)m.apply(r.span(), z.span());
    crd::containers::Array<crd::f64> out(args.alloc);
    push_vec<T>(out, z, n);
    return blob_f64_result(args.alloc, crd::containers::ConstSpan<crd::f64>{out.data(), out.size()});
}

// hesap.precond.fspai.<T> : z = M⁻¹ r, M = L·Lᴴ ≈ A⁻¹ (factored SPAI; SPD/HPD A).
template <typename T> CommandResult impl_fspai(const CommandArgs& args)
{
    using R = RealType<T>;
    const auto rows = args.get_u64("rows");
    if (!rows)
    {
        return error_result(args.alloc, "fspai: rows (== cols) is required");
    }
    const crd::u32 n = static_cast<crd::u32>(*rows);
    const auto rin = args.get_f64_array("r");
    const crd::usize expect = is_complex_v<T> ? static_cast<crd::usize>(n) * 2 : static_cast<crd::usize>(n);
    if (rin.size() != expect)
    {
        return error_result(args.alloc, "fspai: r has wrong length");
    }
    const R eps = static_cast<R>(args.get_f64("spai_eps").value_or(0.1));
    const crd::u32 fill = static_cast<crd::u32>(args.get_u64("spai_fill").value_or(0U));
    auto a = build_csr<T>(args, n);
    FspaiPreconditioner<T> m(a, args.alloc, read_spai_pattern<T>(args), eps, fill);
    crd::hesap::dense::Vector<T> r(args.alloc, n);
    crd::hesap::dense::Vector<T> z(args.alloc, n);
    read_vec<T>(args, "r", r, n);
    (void)m.apply(r.span(), z.span());
    crd::containers::Array<crd::f64> out(args.alloc);
    push_vec<T>(out, z, n);
    return blob_f64_result(args.alloc, crd::containers::ConstSpan<crd::f64>{out.data(), out.size()});
}

// hesap.precond.chebyshev.<T> : z = p_deg(A)·r ≈ A⁻¹r (matrix-free polynomial; SPD/HPD A).
template <typename T> CommandResult impl_chebyshev(const CommandArgs& args)
{
    using R = RealType<T>;
    const auto rows = args.get_u64("rows");
    if (!rows)
    {
        return error_result(args.alloc, "chebyshev: rows (== cols) is required");
    }
    const crd::u32 n = static_cast<crd::u32>(*rows);
    const auto rin = args.get_f64_array("r");
    const crd::usize expect = is_complex_v<T> ? static_cast<crd::usize>(n) * 2 : static_cast<crd::usize>(n);
    if (rin.size() != expect)
    {
        return error_result(args.alloc, "chebyshev: r has wrong length");
    }
    const crd::u32 deg = static_cast<crd::u32>(args.get_u64("degree").value_or(4U));
    const R lo_ratio = static_cast<R>(args.get_f64("cheb_lo_ratio").value_or(1.0 / 30.0));
    auto a = build_csr<T>(args, n);
    ChebyshevPreconditioner<T> m(a, args.alloc, deg, lo_ratio);
    crd::hesap::dense::Vector<T> r(args.alloc, n);
    crd::hesap::dense::Vector<T> z(args.alloc, n);
    read_vec<T>(args, "r", r, n);
    (void)m.apply(r.span(), z.span());
    crd::containers::Array<crd::f64> out(args.alloc);
    push_vec<T>(out, z, n);
    return blob_f64_result(args.alloc, crd::containers::ConstSpan<crd::f64>{out.data(), out.size()});
}

// Read the Schwarz knobs (shared by the standalone command + the selectors). `force` overrides
// the type (selectors pin AS for SPD solvers, RAS for nonsym).
template <typename T>
SchwarzPreconditioner<T> make_schwarz(const CommandArgs& args, const SparseMatrix<T, SparseFormat::Csr>& a,
                                      SchwarzType forced)
{
    const crd::u32 bs = static_cast<crd::u32>(args.get_u64("schwarz_block").value_or(64U));
    const crd::u32 ov = static_cast<crd::u32>(args.get_u64("schwarz_overlap").value_or(1U));
    const SchwarzPartition part = (args.get_string("schwarz_partition") == crd::containers::StringView{"nd"})
                                      ? SchwarzPartition::NestedDissection
                                      : SchwarzPartition::Contiguous;
    return SchwarzPreconditioner<T>(a, args.alloc, bs, ov, forced, part);
}

// hesap.precond.mlilu.<T> : MC64-preprocessed multilevel ILU (ILUPACK-class; general A).
template <typename T> CommandResult impl_mlilu(const CommandArgs& args)
{
    using R = RealType<T>;
    const auto rows = args.get_u64("rows");
    if (!rows)
    {
        return error_result(args.alloc, "mlilu: rows (== cols) is required");
    }
    const crd::u32 n = static_cast<crd::u32>(*rows);
    const auto rin = args.get_f64_array("r");
    const crd::usize expect = is_complex_v<T> ? static_cast<crd::usize>(n) * 2 : static_cast<crd::usize>(n);
    if (rin.size() != expect)
    {
        return error_result(args.alloc, "mlilu: r has wrong length");
    }
    const crd::u32 lfil = static_cast<crd::u32>(args.get_u64("lfil").value_or(0U));
    const R droptol = static_cast<R>(args.get_f64("droptol").value_or(-1.0));
    auto a = build_csr<T>(args, n);
    MultilevelIlu<T> m(a, args.alloc, lfil, droptol);
    crd::hesap::dense::Vector<T> r(args.alloc, n);
    crd::hesap::dense::Vector<T> z(args.alloc, n);
    read_vec<T>(args, "r", r, n);
    (void)m.apply(r.span(), z.span());
    crd::containers::Array<crd::f64> out(args.alloc);
    push_vec<T>(out, z, n);
    return blob_f64_result(args.alloc, crd::containers::ConstSpan<crd::f64>{out.data(), out.size()});
}

// hesap.precond.mlilu_ib.<T> : inverse-based multilevel ILU (Bollhöfer-Saad; ILUPACK core). z = M⁻¹ r.
template <typename T> CommandResult impl_mlilu_ib(const CommandArgs& args)
{
    using R = RealType<T>;
    const auto rows = args.get_u64("rows");
    if (!rows)
    {
        return error_result(args.alloc, "mlilu_ib: rows (== cols) is required");
    }
    const crd::u32 n = static_cast<crd::u32>(*rows);
    const auto rin = args.get_f64_array("r");
    const crd::usize expect = is_complex_v<T> ? static_cast<crd::usize>(n) * 2 : static_cast<crd::usize>(n);
    if (rin.size() != expect)
    {
        return error_result(args.alloc, "mlilu_ib: r has wrong length");
    }
    const R condest = static_cast<R>(args.get_f64("condest").value_or(-1.0)); // κ inverse-factor bound
    const R droptol = static_cast<R>(args.get_f64("droptol").value_or(-1.0));
    const bool reorder = args.get_bool("reorder").value_or(true); // v4z: default-ON (matches the ctor + ILUPACK)
    auto a = build_csr<T>(args, n);
    InverseBasedIlu<T> m(a, args.alloc, condest, droptol, 0U, 50U, 64U, Mc64Mode::None, R(0), reorder);
    crd::hesap::dense::Vector<T> r(args.alloc, n);
    crd::hesap::dense::Vector<T> z(args.alloc, n);
    read_vec<T>(args, "r", r, n);
    (void)m.apply(r.span(), z.span());
    crd::containers::Array<crd::f64> out(args.alloc);
    push_vec<T>(out, z, n);
    return blob_f64_result(args.alloc, crd::containers::ConstSpan<crd::f64>{out.data(), out.size()});
}

// hesap.precond.schwarz.<T> : z = Σ R̃ᵀ Aᵢᵢ⁻¹ R r (overlapping domain decomposition).
template <typename T> CommandResult impl_schwarz(const CommandArgs& args)
{
    const auto rows = args.get_u64("rows");
    if (!rows)
    {
        return error_result(args.alloc, "schwarz: rows (== cols) is required");
    }
    const crd::u32 n = static_cast<crd::u32>(*rows);
    const auto rin = args.get_f64_array("r");
    const crd::usize expect = is_complex_v<T> ? static_cast<crd::usize>(n) * 2 : static_cast<crd::usize>(n);
    if (rin.size() != expect)
    {
        return error_result(args.alloc, "schwarz: r has wrong length");
    }
    const SchwarzType type = (args.get_string("schwarz_type") == crd::containers::StringView{"additive"})
                                 ? SchwarzType::Additive
                                 : SchwarzType::Restricted;
    auto a = build_csr<T>(args, n);
    SchwarzPreconditioner<T> m = make_schwarz<T>(args, a, type);
    crd::hesap::dense::Vector<T> r(args.alloc, n);
    crd::hesap::dense::Vector<T> z(args.alloc, n);
    read_vec<T>(args, "r", r, n);
    (void)m.apply(r.span(), z.span());
    crd::containers::Array<crd::f64> out(args.alloc);
    push_vec<T>(out, z, n);
    return blob_f64_result(args.alloc, crd::containers::ConstSpan<crd::f64>{out.data(), out.size()});
}

// hesap.iterative.pcg.<T> : Jacobi-preconditioned CG.
template <typename T> CommandResult impl_pcg(const CommandArgs& args)
{
    using R = RealType<T>;
    const auto rows = args.get_u64("rows");
    const auto cols = args.get_u64("cols");
    if (!rows || !cols || *rows != *cols)
    {
        return error_result(args.alloc, "pcg: rows and cols are required and must be equal");
    }
    const crd::u32 n = static_cast<crd::u32>(*rows);
    const auto bin = args.get_f64_array("b");
    const crd::usize expect = is_complex_v<T> ? static_cast<crd::usize>(n) * 2 : static_cast<crd::usize>(n);
    if (bin.size() != expect)
    {
        return error_result(args.alloc, "pcg: b has wrong length (n real or 2n flattened complex)");
    }

    auto a = build_csr<T>(args, n);
    SparseLinearOp<T> op(a);
    crd::hesap::dense::Vector<T> bvec(args.alloc, n);
    crd::hesap::dense::Vector<T> xvec(args.alloc, n); // x0 = 0
    read_vec<T>(args, "b", bvec, n);

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
    auto run = [&](const crd::hesap::LinearOp<T>& m)
    {
        return crd::hesap::iterative::pcg<T>(op, m, bvec.span(), xvec.span(), opts, ws, args.alloc);
    };

    // precond selector: jacobi (default) | block_jacobi | ssor.
    const auto precond = args.get_string("precond");
    crd::hesap::iterative::IterativeResult<R> res(args.alloc);
    if (precond == crd::containers::StringView{"block_jacobi"})
    {
        const crd::u32 bs = static_cast<crd::u32>(args.get_u64("block_size").value_or(4U));
        BlockJacobiPreconditioner<T> m(a, bs, args.alloc);
        res = run(m);
    }
    else if (precond == crd::containers::StringView{"ssor"})
    {
        const R omega = static_cast<R>(args.get_f64("omega").value_or(1.0));
        SsorPreconditioner<T> m(a, omega, args.alloc);
        res = run(m);
    }
    else if (precond == crd::containers::StringView{"ic0"}) // incomplete Cholesky level 0 (SPD/HPD A)
    {
        Ic0Preconditioner<T> m(a, args.alloc);
        res = run(m);
    }
    else if (precond == crd::containers::StringView{"ilu0"}) // incomplete LU level 0 (general A)
    {
        Ilu0Preconditioner<T> m(a, args.alloc);
        res = run(m);
    }
    else if (precond == crd::containers::StringView{"ilut"}) // dual-threshold ILUT (general A)
    {
        const crd::u32 lfil = static_cast<crd::u32>(args.get_u64("lfil").value_or(0U));
        const R droptol = static_cast<R>(args.get_f64("droptol").value_or(-1.0));
        IlutPreconditioner<T> m(a, args.alloc, lfil, droptol);
        res = run(m);
    }
    else if (precond == crd::containers::StringView{"ilup"}) // level-of-fill ILU(p) (general A)
    {
        const crd::u32 level = static_cast<crd::u32>(args.get_u64("level").value_or(0U));
        IlupPreconditioner<T> m(a, args.alloc, level);
        res = run(m);
    }
    else if (precond == crd::containers::StringView{"fspai"}) // factored SPAI M = L·Lᴴ (SPD/HPD A)
    {
        const R eps = static_cast<R>(args.get_f64("spai_eps").value_or(0.1));
        const crd::u32 fill = static_cast<crd::u32>(args.get_u64("spai_fill").value_or(0U));
        FspaiPreconditioner<T> m(a, args.alloc, read_spai_pattern<T>(args), eps, fill);
        res = run(m);
    }
    else if (precond == crd::containers::StringView{"chebyshev"}) // matrix-free polynomial (SPD/HPD A)
    {
        const crd::u32 deg = static_cast<crd::u32>(args.get_u64("degree").value_or(4U));
        const R lr = static_cast<R>(args.get_f64("cheb_lo_ratio").value_or(1.0 / 30.0));
        ChebyshevPreconditioner<T> m(a, args.alloc, deg, lr);
        res = run(m);
    }
    else if (precond == crd::containers::StringView{"schwarz"}) // additive Schwarz (symmetric for SPD A)
    {
        SchwarzPreconditioner<T> m = make_schwarz<T>(args, a, SchwarzType::Additive);
        res = run(m);
    }
    else
    {
        JacobiPreconditioner<T> m(a, args.alloc);
        res = run(m);
    }

    crd::containers::Array<crd::f64> out(args.alloc);
    out.push_back(static_cast<crd::f64>(res.iterations));
    out.push_back(static_cast<crd::f64>(res.final_residual_norm));
    out.push_back(res.converged ? 1.0 : 0.0);
    push_vec<T>(out, xvec, n);
    return blob_f64_result(args.alloc, crd::containers::ConstSpan<crd::f64>{out.data(), out.size()});
}

// hesap.iterative.block_pcg.<T> : multi-RHS preconditioned block-CG (diagonal/Jacobi
// block preconditioner). A SPD/HPD (rows==cols); B is n×s ROW-MAJOR flattened
// (b[k*s+j]; complex {re,im} interleaved). Returns [iters, resid, converged, X...]
// (X n×s row-major). The CLI is the deterministic oracle ⇒ serial block spmm.
template <typename T> CommandResult impl_block_pcg(const CommandArgs& args)
{
    using R = RealType<T>;
    const auto rows = args.get_u64("rows");
    const auto cols = args.get_u64("cols");
    const auto srhs = args.get_u64("s");
    if (!rows || !cols || *rows != *cols || !srhs || *srhs < 1)
    {
        return error_result(args.alloc, "block_pcg: rows==cols and s>=1 are required");
    }
    const crd::u32 n = static_cast<crd::u32>(*rows);
    const crd::u32 s = static_cast<crd::u32>(*srhs);
    const auto bin = args.get_f64_array("b");
    const crd::usize expect = (is_complex_v<T> ? 2U : 1U) * static_cast<crd::usize>(n) * s;
    if (bin.size() != expect)
    {
        return error_result(args.alloc, "block_pcg: b must be n*s (real) or 2*n*s (complex) flattened row-major");
    }

    auto a = build_csr<T>(args, n);
    crd::hesap::sparse::ParallelSpmmLinearOp<T> op(a, ~crd::usize{0}); // serial spmm (CLI = oracle)
    crd::hesap::preconditioners::JacobiBlockPreconditioner<T> m(a, args.alloc);
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
    if (const auto rt = args.get_f64("rel_tol"))
    {
        opts.rel_tol = static_cast<R>(*rt);
    }
    if (const auto mi = args.get_u64("max_iter"))
    {
        opts.max_iter = static_cast<crd::usize>(*mi);
    }

    crd::hesap::iterative::BlockCgWorkspace<T> ws(args.alloc, n, s);
    auto res = crd::hesap::iterative::block_pcg<T>(op, m, bvec.span(), xvec.span(), opts, ws, args.alloc);

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

// hesap.iterative.fgmres.<T> : (flexible) restarted GMRES with a precond selector.
template <typename T> CommandResult impl_fgmres(const CommandArgs& args)
{
    using R = RealType<T>;
    const auto rows = args.get_u64("rows");
    const auto cols = args.get_u64("cols");
    if (!rows || !cols || *rows != *cols)
    {
        return error_result(args.alloc, "fgmres: rows and cols are required and must be equal");
    }
    const crd::u32 n = static_cast<crd::u32>(*rows);
    const auto bin = args.get_f64_array("b");
    const crd::usize expect = is_complex_v<T> ? static_cast<crd::usize>(n) * 2 : static_cast<crd::usize>(n);
    if (bin.size() != expect)
    {
        return error_result(args.alloc, "fgmres: b has wrong length (n real or 2n flattened complex)");
    }

    auto a = build_csr<T>(args, n);
    SparseLinearOp<T> op(a);
    crd::hesap::dense::Vector<T> bvec(args.alloc, n);
    crd::hesap::dense::Vector<T> xvec(args.alloc, n);
    read_vec<T>(args, "b", bvec, n);

    crd::hesap::iterative::IterativeOptions<R> opts;
    if (const auto rt = args.get_f64("rel_tol"))
    {
        opts.rel_tol = static_cast<R>(*rt);
    }
    if (const auto mi = args.get_u64("max_iter"))
    {
        opts.max_iter = static_cast<crd::usize>(*mi);
    }
    const crd::usize restart = static_cast<crd::usize>(args.get_u64("restart").value_or(30U));
    crd::hesap::iterative::GmresWorkspace<T> ws(args.alloc, n, restart);

    auto run = [&](const crd::hesap::LinearOp<T>* m)
    {
        return crd::hesap::iterative::fgmres<T>(op, m, bvec.span(), xvec.span(), opts, ws, args.alloc);
    };

    const auto precond = args.get_string("precond");
    crd::hesap::iterative::IterativeResult<R> res(args.alloc);
    if (precond == crd::containers::StringView{"jacobi"})
    {
        JacobiPreconditioner<T> m(a, args.alloc);
        res = run(&m);
    }
    else if (precond == crd::containers::StringView{"block_jacobi"})
    {
        const crd::u32 bs = static_cast<crd::u32>(args.get_u64("block_size").value_or(4U));
        BlockJacobiPreconditioner<T> m(a, bs, args.alloc);
        res = run(&m);
    }
    else if (precond == crd::containers::StringView{"ssor"})
    {
        const R omega = static_cast<R>(args.get_f64("omega").value_or(1.0));
        SsorPreconditioner<T> m(a, omega, args.alloc);
        res = run(&m);
    }
    else if (precond == crd::containers::StringView{"spai"}) // classical SPAI M ≈ A⁻¹ (general A)
    {
        const R eps = static_cast<R>(args.get_f64("spai_eps").value_or(0.4));
        const crd::u32 fill = static_cast<crd::u32>(args.get_u64("spai_fill").value_or(0U));
        SpaiPreconditioner<T> m(a, args.alloc, read_spai_pattern<T>(args), eps, fill);
        res = run(&m);
    }
    else if (precond == crd::containers::StringView{"schwarz"}) // restricted additive Schwarz (general A)
    {
        SchwarzPreconditioner<T> m = make_schwarz<T>(args, a, SchwarzType::Restricted);
        res = run(&m);
    }
    else if (precond == crd::containers::StringView{"mlilu"}) // MC64-preprocessed multilevel ILU (general A)
    {
        const crd::u32 lfil = static_cast<crd::u32>(args.get_u64("lfil").value_or(0U));
        const R droptol = static_cast<R>(args.get_f64("droptol").value_or(-1.0));
        MultilevelIlu<T> m(a, args.alloc, lfil, droptol);
        res = run(&m);
    }
    else if (precond == crd::containers::StringView{"mlilu_ib"}) // inverse-based multilevel ILU (ILUPACK core)
    {
        const R condest = static_cast<R>(args.get_f64("condest").value_or(-1.0));
        const R droptol = static_cast<R>(args.get_f64("droptol").value_or(-1.0));
        const bool reorder = args.get_bool("reorder").value_or(true); // v4z: per-level AMD reorder, default-ON
        InverseBasedIlu<T> m(a, args.alloc, condest, droptol, 0U, 50U, 64U, Mc64Mode::None, R(0), reorder);
        res = run(&m);
    }
    else
    {
        res = run(nullptr); // plain GMRES
    }

    crd::containers::Array<crd::f64> out(args.alloc);
    out.push_back(static_cast<crd::f64>(res.iterations));
    out.push_back(static_cast<crd::f64>(res.final_residual_norm));
    out.push_back(res.converged ? 1.0 : 0.0);
    push_vec<T>(out, xvec, n);
    return blob_f64_result(args.alloc, crd::containers::ConstSpan<crd::f64>{out.data(), out.size()});
}

// hesap.iterative.bicgstab.<T> : preconditioned BiCGSTAB with a precond selector.
template <typename T> CommandResult impl_bicgstab(const CommandArgs& args)
{
    using R = RealType<T>;
    const auto rows = args.get_u64("rows");
    const auto cols = args.get_u64("cols");
    if (!rows || !cols || *rows != *cols)
    {
        return error_result(args.alloc, "bicgstab: rows and cols are required and must be equal");
    }
    const crd::u32 n = static_cast<crd::u32>(*rows);
    const auto bin = args.get_f64_array("b");
    const crd::usize expect = is_complex_v<T> ? static_cast<crd::usize>(n) * 2 : static_cast<crd::usize>(n);
    if (bin.size() != expect)
    {
        return error_result(args.alloc, "bicgstab: b has wrong length (n real or 2n flattened complex)");
    }

    auto a = build_csr<T>(args, n);
    SparseLinearOp<T> op(a);
    crd::hesap::dense::Vector<T> bvec(args.alloc, n);
    crd::hesap::dense::Vector<T> xvec(args.alloc, n);
    read_vec<T>(args, "b", bvec, n);

    crd::hesap::iterative::IterativeOptions<R> opts;
    if (const auto rt = args.get_f64("rel_tol"))
    {
        opts.rel_tol = static_cast<R>(*rt);
    }
    if (const auto mi = args.get_u64("max_iter"))
    {
        opts.max_iter = static_cast<crd::usize>(*mi);
    }
    crd::hesap::iterative::BicgstabWorkspace<T> ws(args.alloc, n);

    auto run = [&](const crd::hesap::LinearOp<T>* m)
    {
        return crd::hesap::iterative::bicgstab<T>(op, m, bvec.span(), xvec.span(), opts, ws, args.alloc);
    };

    const auto precond = args.get_string("precond");
    crd::hesap::iterative::IterativeResult<R> res(args.alloc);
    if (precond == crd::containers::StringView{"jacobi"})
    {
        JacobiPreconditioner<T> m(a, args.alloc);
        res = run(&m);
    }
    else if (precond == crd::containers::StringView{"block_jacobi"})
    {
        const crd::u32 bs = static_cast<crd::u32>(args.get_u64("block_size").value_or(4U));
        BlockJacobiPreconditioner<T> m(a, bs, args.alloc);
        res = run(&m);
    }
    else if (precond == crd::containers::StringView{"ssor"})
    {
        const R omega = static_cast<R>(args.get_f64("omega").value_or(1.0));
        SsorPreconditioner<T> m(a, omega, args.alloc);
        res = run(&m);
    }
    else if (precond == crd::containers::StringView{"spai"}) // classical SPAI M ≈ A⁻¹ (general A)
    {
        const R eps = static_cast<R>(args.get_f64("spai_eps").value_or(0.4));
        const crd::u32 fill = static_cast<crd::u32>(args.get_u64("spai_fill").value_or(0U));
        SpaiPreconditioner<T> m(a, args.alloc, read_spai_pattern<T>(args), eps, fill);
        res = run(&m);
    }
    else if (precond == crd::containers::StringView{"schwarz"}) // restricted additive Schwarz (general A)
    {
        SchwarzPreconditioner<T> m = make_schwarz<T>(args, a, SchwarzType::Restricted);
        res = run(&m);
    }
    else if (precond == crd::containers::StringView{"mlilu"}) // MC64-preprocessed multilevel ILU (general A)
    {
        const crd::u32 lfil = static_cast<crd::u32>(args.get_u64("lfil").value_or(0U));
        const R droptol = static_cast<R>(args.get_f64("droptol").value_or(-1.0));
        MultilevelIlu<T> m(a, args.alloc, lfil, droptol);
        res = run(&m);
    }
    else if (precond == crd::containers::StringView{"mlilu_ib"}) // inverse-based multilevel ILU (ILUPACK core)
    {
        const R condest = static_cast<R>(args.get_f64("condest").value_or(-1.0));
        const R droptol = static_cast<R>(args.get_f64("droptol").value_or(-1.0));
        const bool reorder = args.get_bool("reorder").value_or(true); // v4z: per-level AMD reorder, default-ON
        InverseBasedIlu<T> m(a, args.alloc, condest, droptol, 0U, 50U, 64U, Mc64Mode::None, R(0), reorder);
        res = run(&m);
    }
    else
    {
        res = run(nullptr);
    }

    crd::containers::Array<crd::f64> out(args.alloc);
    out.push_back(static_cast<crd::f64>(res.iterations));
    out.push_back(static_cast<crd::f64>(res.final_residual_norm));
    out.push_back(res.converged ? 1.0 : 0.0);
    push_vec<T>(out, xvec, n);
    return blob_f64_result(args.alloc, crd::containers::ConstSpan<crd::f64>{out.data(), out.size()});
}

// hesap.iterative.minres.<T> : (preconditioned) MINRES. precond MUST be SPD/HPD
// (jacobi/block_jacobi/ssor on an SPD A; use none for indefinite A).
template <typename T> CommandResult impl_minres(const CommandArgs& args)
{
    using R = RealType<T>;
    const auto rows = args.get_u64("rows");
    const auto cols = args.get_u64("cols");
    if (!rows || !cols || *rows != *cols)
    {
        return error_result(args.alloc, "minres: rows and cols are required and must be equal (symmetric/Hermitian)");
    }
    const crd::u32 n = static_cast<crd::u32>(*rows);
    const auto bin = args.get_f64_array("b");
    const crd::usize expect = is_complex_v<T> ? static_cast<crd::usize>(n) * 2 : static_cast<crd::usize>(n);
    if (bin.size() != expect)
    {
        return error_result(args.alloc, "minres: b has wrong length (n real or 2n flattened complex)");
    }

    auto a = build_csr<T>(args, n);
    SparseLinearOp<T> op(a);
    crd::hesap::dense::Vector<T> bvec(args.alloc, n);
    crd::hesap::dense::Vector<T> xvec(args.alloc, n);
    read_vec<T>(args, "b", bvec, n);

    crd::hesap::iterative::IterativeOptions<R> opts;
    if (const auto rt = args.get_f64("rel_tol"))
    {
        opts.rel_tol = static_cast<R>(*rt);
    }
    if (const auto mi = args.get_u64("max_iter"))
    {
        opts.max_iter = static_cast<crd::usize>(*mi);
    }
    crd::hesap::iterative::MinresWorkspace<T> ws(args.alloc, n);

    auto run = [&](const crd::hesap::LinearOp<T>* m)
    {
        return crd::hesap::iterative::minres<T>(op, m, bvec.span(), xvec.span(), opts, ws, args.alloc);
    };

    const auto precond = args.get_string("precond");
    crd::hesap::iterative::IterativeResult<R> res(args.alloc);
    if (precond == crd::containers::StringView{"jacobi"})
    {
        JacobiPreconditioner<T> m(a, args.alloc);
        res = run(&m);
    }
    else if (precond == crd::containers::StringView{"block_jacobi"})
    {
        const crd::u32 bs = static_cast<crd::u32>(args.get_u64("block_size").value_or(4U));
        BlockJacobiPreconditioner<T> m(a, bs, args.alloc);
        res = run(&m);
    }
    else if (precond == crd::containers::StringView{"ssor"})
    {
        const R omega = static_cast<R>(args.get_f64("omega").value_or(1.0));
        SsorPreconditioner<T> m(a, omega, args.alloc);
        res = run(&m);
    }
    else if (precond == crd::containers::StringView{"fspai"}) // factored SPAI M = L·Lᴴ (SPD/HPD A)
    {
        const R eps = static_cast<R>(args.get_f64("spai_eps").value_or(0.1));
        const crd::u32 fill = static_cast<crd::u32>(args.get_u64("spai_fill").value_or(0U));
        FspaiPreconditioner<T> m(a, args.alloc, read_spai_pattern<T>(args), eps, fill);
        res = run(&m);
    }
    else if (precond == crd::containers::StringView{"chebyshev"}) // matrix-free polynomial (SPD/HPD A)
    {
        const crd::u32 deg = static_cast<crd::u32>(args.get_u64("degree").value_or(4U));
        const R lr = static_cast<R>(args.get_f64("cheb_lo_ratio").value_or(1.0 / 30.0));
        ChebyshevPreconditioner<T> m(a, args.alloc, deg, lr);
        res = run(&m);
    }
    else if (precond == crd::containers::StringView{"schwarz"}) // additive Schwarz (symmetric for SPD/HPD A)
    {
        SchwarzPreconditioner<T> m = make_schwarz<T>(args, a, SchwarzType::Additive);
        res = run(&m);
    }
    else
    {
        res = run(nullptr);
    }

    crd::containers::Array<crd::f64> out(args.alloc);
    out.push_back(static_cast<crd::f64>(res.iterations));
    out.push_back(static_cast<crd::f64>(res.final_residual_norm));
    out.push_back(res.converged ? 1.0 : 0.0);
    push_vec<T>(out, xvec, n);
    return blob_f64_result(args.alloc, crd::containers::ConstSpan<crd::f64>{out.data(), out.size()});
}

// hesap.iterative.symmlq.<T> : (preconditioned) SYMMLQ. precond MUST be SPD/HPD.
template <typename T> CommandResult impl_symmlq(const CommandArgs& args)
{
    using R = RealType<T>;
    const auto rows = args.get_u64("rows");
    const auto cols = args.get_u64("cols");
    if (!rows || !cols || *rows != *cols)
    {
        return error_result(args.alloc, "symmlq: rows and cols are required and must be equal (symmetric/Hermitian)");
    }
    const crd::u32 n = static_cast<crd::u32>(*rows);
    const auto bin = args.get_f64_array("b");
    const crd::usize expect = is_complex_v<T> ? static_cast<crd::usize>(n) * 2 : static_cast<crd::usize>(n);
    if (bin.size() != expect)
    {
        return error_result(args.alloc, "symmlq: b has wrong length (n real or 2n flattened complex)");
    }

    auto a = build_csr<T>(args, n);
    SparseLinearOp<T> op(a);
    crd::hesap::dense::Vector<T> bvec(args.alloc, n);
    crd::hesap::dense::Vector<T> xvec(args.alloc, n);
    read_vec<T>(args, "b", bvec, n);

    crd::hesap::iterative::IterativeOptions<R> opts;
    if (const auto rt = args.get_f64("rel_tol"))
    {
        opts.rel_tol = static_cast<R>(*rt);
    }
    if (const auto mi = args.get_u64("max_iter"))
    {
        opts.max_iter = static_cast<crd::usize>(*mi);
    }
    crd::hesap::iterative::SymmlqWorkspace<T> ws(args.alloc, n);

    auto run = [&](const crd::hesap::LinearOp<T>* m)
    {
        return crd::hesap::iterative::symmlq<T>(op, m, bvec.span(), xvec.span(), opts, ws, args.alloc);
    };

    const auto precond = args.get_string("precond");
    crd::hesap::iterative::IterativeResult<R> res(args.alloc);
    if (precond == crd::containers::StringView{"jacobi"})
    {
        JacobiPreconditioner<T> m(a, args.alloc);
        res = run(&m);
    }
    else if (precond == crd::containers::StringView{"block_jacobi"})
    {
        const crd::u32 bs = static_cast<crd::u32>(args.get_u64("block_size").value_or(4U));
        BlockJacobiPreconditioner<T> m(a, bs, args.alloc);
        res = run(&m);
    }
    else if (precond == crd::containers::StringView{"ssor"})
    {
        const R omega = static_cast<R>(args.get_f64("omega").value_or(1.0));
        SsorPreconditioner<T> m(a, omega, args.alloc);
        res = run(&m);
    }
    else if (precond == crd::containers::StringView{"fspai"}) // factored SPAI M = L·Lᴴ (SPD/HPD A)
    {
        const R eps = static_cast<R>(args.get_f64("spai_eps").value_or(0.1));
        const crd::u32 fill = static_cast<crd::u32>(args.get_u64("spai_fill").value_or(0U));
        FspaiPreconditioner<T> m(a, args.alloc, read_spai_pattern<T>(args), eps, fill);
        res = run(&m);
    }
    else if (precond == crd::containers::StringView{"chebyshev"}) // matrix-free polynomial (SPD/HPD A)
    {
        const crd::u32 deg = static_cast<crd::u32>(args.get_u64("degree").value_or(4U));
        const R lr = static_cast<R>(args.get_f64("cheb_lo_ratio").value_or(1.0 / 30.0));
        ChebyshevPreconditioner<T> m(a, args.alloc, deg, lr);
        res = run(&m);
    }
    else if (precond == crd::containers::StringView{"schwarz"}) // additive Schwarz (symmetric for SPD/HPD A)
    {
        SchwarzPreconditioner<T> m = make_schwarz<T>(args, a, SchwarzType::Additive);
        res = run(&m);
    }
    else
    {
        res = run(nullptr);
    }

    crd::containers::Array<crd::f64> out(args.alloc);
    out.push_back(static_cast<crd::f64>(res.iterations));
    out.push_back(static_cast<crd::f64>(res.final_residual_norm));
    out.push_back(res.converged ? 1.0 : 0.0);
    push_vec<T>(out, xvec, n);
    return blob_f64_result(args.alloc, crd::containers::ConstSpan<crd::f64>{out.data(), out.size()});
}

// hesap.iterative.qmr.<T> : (right-preconditioned) QMR for a GENERAL square A.
// precond none|jacobi|block_jacobi|ssor (each exposes apply_adjoint, required by
// QMR's two-sided bi-Lanczos for the N⁻ᴴ action).
template <typename T> CommandResult impl_qmr(const CommandArgs& args)
{
    using R = RealType<T>;
    const auto rows = args.get_u64("rows");
    const auto cols = args.get_u64("cols");
    if (!rows || !cols || *rows != *cols)
    {
        return error_result(args.alloc, "qmr: rows and cols are required and must be equal");
    }
    const crd::u32 n = static_cast<crd::u32>(*rows);
    const auto bin = args.get_f64_array("b");
    const crd::usize expect = is_complex_v<T> ? static_cast<crd::usize>(n) * 2 : static_cast<crd::usize>(n);
    if (bin.size() != expect)
    {
        return error_result(args.alloc, "qmr: b has wrong length (n real or 2n flattened complex)");
    }

    auto a = build_csr<T>(args, n);
    SparseLinearOp<T> op(a); // serial CSR; has apply + apply_adjoint
    crd::hesap::dense::Vector<T> bvec(args.alloc, n);
    crd::hesap::dense::Vector<T> xvec(args.alloc, n);
    read_vec<T>(args, "b", bvec, n);

    crd::hesap::iterative::IterativeOptions<R> opts;
    if (const auto rt = args.get_f64("rel_tol"))
    {
        opts.rel_tol = static_cast<R>(*rt);
    }
    if (const auto mi = args.get_u64("max_iter"))
    {
        opts.max_iter = static_cast<crd::usize>(*mi);
    }
    crd::hesap::iterative::QmrWorkspace<T> ws(args.alloc, n);

    auto run = [&](const crd::hesap::LinearOp<T>* m)
    {
        return crd::hesap::iterative::qmr<T>(op, m, bvec.span(), xvec.span(), opts, ws, args.alloc);
    };

    const auto precond = args.get_string("precond");
    crd::hesap::iterative::IterativeResult<R> res(args.alloc);
    if (precond == crd::containers::StringView{"jacobi"})
    {
        JacobiPreconditioner<T> m(a, args.alloc);
        res = run(&m);
    }
    else if (precond == crd::containers::StringView{"block_jacobi"})
    {
        const crd::u32 bs = static_cast<crd::u32>(args.get_u64("block_size").value_or(4U));
        BlockJacobiPreconditioner<T> m(a, bs, args.alloc);
        res = run(&m);
    }
    else if (precond == crd::containers::StringView{"ssor"})
    {
        const R omega = static_cast<R>(args.get_f64("omega").value_or(1.0));
        SsorPreconditioner<T> m(a, omega, args.alloc);
        res = run(&m);
    }
    else if (precond == crd::containers::StringView{"spai"}) // classical SPAI M ≈ A⁻¹ (general A)
    {
        const R eps = static_cast<R>(args.get_f64("spai_eps").value_or(0.4));
        const crd::u32 fill = static_cast<crd::u32>(args.get_u64("spai_fill").value_or(0U));
        SpaiPreconditioner<T> m(a, args.alloc, read_spai_pattern<T>(args), eps, fill);
        res = run(&m);
    }
    else if (precond == crd::containers::StringView{"schwarz"}) // restricted additive Schwarz (general A)
    {
        SchwarzPreconditioner<T> m = make_schwarz<T>(args, a, SchwarzType::Restricted);
        res = run(&m);
    }
    else if (precond == crd::containers::StringView{"mlilu"}) // MC64-preprocessed multilevel ILU (general A)
    {
        const crd::u32 lfil = static_cast<crd::u32>(args.get_u64("lfil").value_or(0U));
        const R droptol = static_cast<R>(args.get_f64("droptol").value_or(-1.0));
        MultilevelIlu<T> m(a, args.alloc, lfil, droptol);
        res = run(&m);
    }
    else if (precond == crd::containers::StringView{"mlilu_ib"}) // inverse-based multilevel ILU (ILUPACK core)
    {
        const R condest = static_cast<R>(args.get_f64("condest").value_or(-1.0));
        const R droptol = static_cast<R>(args.get_f64("droptol").value_or(-1.0));
        const bool reorder = args.get_bool("reorder").value_or(true); // v4z: per-level AMD reorder, default-ON
        InverseBasedIlu<T> m(a, args.alloc, condest, droptol, 0U, 50U, 64U, Mc64Mode::None, R(0), reorder);
        res = run(&m);
    }
    else
    {
        res = run(nullptr);
    }

    crd::containers::Array<crd::f64> out(args.alloc);
    out.push_back(static_cast<crd::f64>(res.iterations));
    out.push_back(static_cast<crd::f64>(res.final_residual_norm));
    out.push_back(res.converged ? 1.0 : 0.0);
    push_vec<T>(out, xvec, n);
    return blob_f64_result(args.alloc, crd::containers::ConstSpan<crd::f64>{out.data(), out.size()});
}

// hesap.iterative.gcr.<T> : (right-preconditioned) GCR(m) for a GENERAL square A.
// `restart` = max stored directions. precond none|jacobi|block_jacobi|ssor.
template <typename T> CommandResult impl_gcr(const CommandArgs& args)
{
    using R = RealType<T>;
    const auto rows = args.get_u64("rows");
    const auto cols = args.get_u64("cols");
    if (!rows || !cols || *rows != *cols)
    {
        return error_result(args.alloc, "gcr: rows and cols are required and must be equal");
    }
    const crd::u32 n = static_cast<crd::u32>(*rows);
    const auto bin = args.get_f64_array("b");
    const crd::usize expect = is_complex_v<T> ? static_cast<crd::usize>(n) * 2 : static_cast<crd::usize>(n);
    if (bin.size() != expect)
    {
        return error_result(args.alloc, "gcr: b has wrong length (n real or 2n flattened complex)");
    }

    auto a = build_csr<T>(args, n);
    SparseLinearOp<T> op(a);
    crd::hesap::dense::Vector<T> bvec(args.alloc, n);
    crd::hesap::dense::Vector<T> xvec(args.alloc, n);
    read_vec<T>(args, "b", bvec, n);

    crd::hesap::iterative::IterativeOptions<R> opts;
    if (const auto rt = args.get_f64("rel_tol"))
    {
        opts.rel_tol = static_cast<R>(*rt);
    }
    if (const auto mi = args.get_u64("max_iter"))
    {
        opts.max_iter = static_cast<crd::usize>(*mi);
    }
    const crd::usize restart = static_cast<crd::usize>(args.get_u64("restart").value_or(30U));
    crd::hesap::iterative::GcrWorkspace<T> ws(args.alloc, n, restart);

    auto run = [&](const crd::hesap::LinearOp<T>* m)
    {
        return crd::hesap::iterative::gcr<T>(op, m, bvec.span(), xvec.span(), opts, ws, args.alloc);
    };

    const auto precond = args.get_string("precond");
    crd::hesap::iterative::IterativeResult<R> res(args.alloc);
    if (precond == crd::containers::StringView{"jacobi"})
    {
        JacobiPreconditioner<T> m(a, args.alloc);
        res = run(&m);
    }
    else if (precond == crd::containers::StringView{"block_jacobi"})
    {
        const crd::u32 bs = static_cast<crd::u32>(args.get_u64("block_size").value_or(4U));
        BlockJacobiPreconditioner<T> m(a, bs, args.alloc);
        res = run(&m);
    }
    else if (precond == crd::containers::StringView{"ssor"})
    {
        const R omega = static_cast<R>(args.get_f64("omega").value_or(1.0));
        SsorPreconditioner<T> m(a, omega, args.alloc);
        res = run(&m);
    }
    else if (precond == crd::containers::StringView{"spai"}) // classical SPAI M ≈ A⁻¹ (general A)
    {
        const R eps = static_cast<R>(args.get_f64("spai_eps").value_or(0.4));
        const crd::u32 fill = static_cast<crd::u32>(args.get_u64("spai_fill").value_or(0U));
        SpaiPreconditioner<T> m(a, args.alloc, read_spai_pattern<T>(args), eps, fill);
        res = run(&m);
    }
    else if (precond == crd::containers::StringView{"schwarz"}) // restricted additive Schwarz (general A)
    {
        SchwarzPreconditioner<T> m = make_schwarz<T>(args, a, SchwarzType::Restricted);
        res = run(&m);
    }
    else if (precond == crd::containers::StringView{"mlilu"}) // MC64-preprocessed multilevel ILU (general A)
    {
        const crd::u32 lfil = static_cast<crd::u32>(args.get_u64("lfil").value_or(0U));
        const R droptol = static_cast<R>(args.get_f64("droptol").value_or(-1.0));
        MultilevelIlu<T> m(a, args.alloc, lfil, droptol);
        res = run(&m);
    }
    else if (precond == crd::containers::StringView{"mlilu_ib"}) // inverse-based multilevel ILU (ILUPACK core)
    {
        const R condest = static_cast<R>(args.get_f64("condest").value_or(-1.0));
        const R droptol = static_cast<R>(args.get_f64("droptol").value_or(-1.0));
        const bool reorder = args.get_bool("reorder").value_or(true); // v4z: per-level AMD reorder, default-ON
        InverseBasedIlu<T> m(a, args.alloc, condest, droptol, 0U, 50U, 64U, Mc64Mode::None, R(0), reorder);
        res = run(&m);
    }
    else
    {
        res = run(nullptr);
    }

    crd::containers::Array<crd::f64> out(args.alloc);
    out.push_back(static_cast<crd::f64>(res.iterations));
    out.push_back(static_cast<crd::f64>(res.final_residual_norm));
    out.push_back(res.converged ? 1.0 : 0.0);
    push_vec<T>(out, xvec, n);
    return blob_f64_result(args.alloc, crd::containers::ConstSpan<crd::f64>{out.data(), out.size()});
}

// hesap.iterative.rminres.<T> : RMINRES (recycling MINRES) for symmetric/Hermitian
// (indefinite) A. `restart` = inner Lanczos dim m; `recycle` = max recycle pairs k.
// No preconditioner option (the recycle space is the deflation).
template <typename T> CommandResult impl_rminres(const CommandArgs& args)
{
    using R = RealType<T>;
    const auto rows = args.get_u64("rows");
    const auto cols = args.get_u64("cols");
    if (!rows || !cols || *rows != *cols)
    {
        return error_result(args.alloc, "rminres: rows and cols are required and must be equal (symmetric/Hermitian)");
    }
    const crd::u32 n = static_cast<crd::u32>(*rows);
    const auto bin = args.get_f64_array("b");
    const crd::usize expect = is_complex_v<T> ? static_cast<crd::usize>(n) * 2 : static_cast<crd::usize>(n);
    if (bin.size() != expect)
    {
        return error_result(args.alloc, "rminres: b has wrong length (n real or 2n flattened complex)");
    }

    auto a = build_csr<T>(args, n);
    SparseLinearOp<T> op(a);
    crd::hesap::dense::Vector<T> bvec(args.alloc, n);
    crd::hesap::dense::Vector<T> xvec(args.alloc, n);
    read_vec<T>(args, "b", bvec, n);

    crd::hesap::iterative::IterativeOptions<R> opts;
    if (const auto rt = args.get_f64("rel_tol"))
    {
        opts.rel_tol = static_cast<R>(*rt);
    }
    if (const auto mi = args.get_u64("max_iter"))
    {
        opts.max_iter = static_cast<crd::usize>(*mi);
    }
    const crd::usize inner = static_cast<crd::usize>(args.get_u64("restart").value_or(20U));
    const crd::usize recycle = static_cast<crd::usize>(args.get_u64("recycle").value_or(10U));
    crd::hesap::iterative::RminresWorkspace<T> ws(args.alloc, n, inner, recycle);
    auto res = crd::hesap::iterative::rminres<T>(op, bvec.span(), xvec.span(), opts, ws, args.alloc);

    crd::containers::Array<crd::f64> out(args.alloc);
    out.push_back(static_cast<crd::f64>(res.iterations));
    out.push_back(static_cast<crd::f64>(res.final_residual_norm));
    out.push_back(res.converged ? 1.0 : 0.0);
    push_vec<T>(out, xvec, n);
    return blob_f64_result(args.alloc, crd::containers::ConstSpan<crd::f64>{out.data(), out.size()});
}

// hesap.iterative.gcrot.<T> : (right-preconditioned) GCROT(m,k) recycling GMRES.
// `restart` = inner Krylov dim m; `recycle` = max recycle pairs k.
template <typename T> CommandResult impl_gcrot(const CommandArgs& args)
{
    using R = RealType<T>;
    const auto rows = args.get_u64("rows");
    const auto cols = args.get_u64("cols");
    if (!rows || !cols || *rows != *cols)
    {
        return error_result(args.alloc, "gcrot: rows and cols are required and must be equal");
    }
    const crd::u32 n = static_cast<crd::u32>(*rows);
    const auto bin = args.get_f64_array("b");
    const crd::usize expect = is_complex_v<T> ? static_cast<crd::usize>(n) * 2 : static_cast<crd::usize>(n);
    if (bin.size() != expect)
    {
        return error_result(args.alloc, "gcrot: b has wrong length (n real or 2n flattened complex)");
    }

    auto a = build_csr<T>(args, n);
    SparseLinearOp<T> op(a);
    crd::hesap::dense::Vector<T> bvec(args.alloc, n);
    crd::hesap::dense::Vector<T> xvec(args.alloc, n);
    read_vec<T>(args, "b", bvec, n);

    crd::hesap::iterative::IterativeOptions<R> opts;
    if (const auto rt = args.get_f64("rel_tol"))
    {
        opts.rel_tol = static_cast<R>(*rt);
    }
    if (const auto mi = args.get_u64("max_iter"))
    {
        opts.max_iter = static_cast<crd::usize>(*mi);
    }
    const crd::usize inner = static_cast<crd::usize>(args.get_u64("restart").value_or(20U));
    const crd::usize recycle = static_cast<crd::usize>(args.get_u64("recycle").value_or(10U));
    crd::hesap::iterative::GcrotWorkspace<T> ws(args.alloc, n, inner, recycle);

    auto run = [&](const crd::hesap::LinearOp<T>* m)
    {
        return crd::hesap::iterative::gcrot<T>(op, m, bvec.span(), xvec.span(), opts, ws, args.alloc);
    };

    const auto precond = args.get_string("precond");
    crd::hesap::iterative::IterativeResult<R> res(args.alloc);
    if (precond == crd::containers::StringView{"jacobi"})
    {
        JacobiPreconditioner<T> m(a, args.alloc);
        res = run(&m);
    }
    else if (precond == crd::containers::StringView{"block_jacobi"})
    {
        const crd::u32 bs = static_cast<crd::u32>(args.get_u64("block_size").value_or(4U));
        BlockJacobiPreconditioner<T> m(a, bs, args.alloc);
        res = run(&m);
    }
    else if (precond == crd::containers::StringView{"ssor"})
    {
        const R omega = static_cast<R>(args.get_f64("omega").value_or(1.0));
        SsorPreconditioner<T> m(a, omega, args.alloc);
        res = run(&m);
    }
    else if (precond == crd::containers::StringView{"spai"}) // classical SPAI M ≈ A⁻¹ (general A)
    {
        const R eps = static_cast<R>(args.get_f64("spai_eps").value_or(0.4));
        const crd::u32 fill = static_cast<crd::u32>(args.get_u64("spai_fill").value_or(0U));
        SpaiPreconditioner<T> m(a, args.alloc, read_spai_pattern<T>(args), eps, fill);
        res = run(&m);
    }
    else if (precond == crd::containers::StringView{"schwarz"}) // restricted additive Schwarz (general A)
    {
        SchwarzPreconditioner<T> m = make_schwarz<T>(args, a, SchwarzType::Restricted);
        res = run(&m);
    }
    else if (precond == crd::containers::StringView{"mlilu"}) // MC64-preprocessed multilevel ILU (general A)
    {
        const crd::u32 lfil = static_cast<crd::u32>(args.get_u64("lfil").value_or(0U));
        const R droptol = static_cast<R>(args.get_f64("droptol").value_or(-1.0));
        MultilevelIlu<T> m(a, args.alloc, lfil, droptol);
        res = run(&m);
    }
    else if (precond == crd::containers::StringView{"mlilu_ib"}) // inverse-based multilevel ILU (ILUPACK core)
    {
        const R condest = static_cast<R>(args.get_f64("condest").value_or(-1.0));
        const R droptol = static_cast<R>(args.get_f64("droptol").value_or(-1.0));
        const bool reorder = args.get_bool("reorder").value_or(true); // v4z: per-level AMD reorder, default-ON
        InverseBasedIlu<T> m(a, args.alloc, condest, droptol, 0U, 50U, 64U, Mc64Mode::None, R(0), reorder);
        res = run(&m);
    }
    else
    {
        res = run(nullptr);
    }

    crd::containers::Array<crd::f64> out(args.alloc);
    out.push_back(static_cast<crd::f64>(res.iterations));
    out.push_back(static_cast<crd::f64>(res.final_residual_norm));
    out.push_back(res.converged ? 1.0 : 0.0);
    push_vec<T>(out, xvec, n);
    return blob_f64_result(args.alloc, crd::containers::ConstSpan<crd::f64>{out.data(), out.size()});
}

// hesap.iterative.idrs.<T> : (left-preconditioned) IDR(s) for a GENERAL square A.
// `s` = shadow-space dimension (default 4). precond none|jacobi|block_jacobi|ssor.
template <typename T> CommandResult impl_idrs(const CommandArgs& args)
{
    using R = RealType<T>;
    const auto rows = args.get_u64("rows");
    const auto cols = args.get_u64("cols");
    if (!rows || !cols || *rows != *cols)
    {
        return error_result(args.alloc, "idrs: rows and cols are required and must be equal");
    }
    const crd::u32 n = static_cast<crd::u32>(*rows);
    const auto bin = args.get_f64_array("b");
    const crd::usize expect = is_complex_v<T> ? static_cast<crd::usize>(n) * 2 : static_cast<crd::usize>(n);
    if (bin.size() != expect)
    {
        return error_result(args.alloc, "idrs: b has wrong length (n real or 2n flattened complex)");
    }

    auto a = build_csr<T>(args, n);
    SparseLinearOp<T> op(a);
    crd::hesap::dense::Vector<T> bvec(args.alloc, n);
    crd::hesap::dense::Vector<T> xvec(args.alloc, n);
    read_vec<T>(args, "b", bvec, n);

    crd::hesap::iterative::IterativeOptions<R> opts;
    if (const auto rt = args.get_f64("rel_tol"))
    {
        opts.rel_tol = static_cast<R>(*rt);
    }
    if (const auto mi = args.get_u64("max_iter"))
    {
        opts.max_iter = static_cast<crd::usize>(*mi);
    }
    const crd::usize sdim = static_cast<crd::usize>(args.get_u64("s").value_or(4U));
    crd::hesap::iterative::IdrsWorkspace<T> ws(args.alloc, n, sdim);

    auto run = [&](const crd::hesap::LinearOp<T>* m)
    {
        return crd::hesap::iterative::idrs<T>(op, m, bvec.span(), xvec.span(), opts, ws, args.alloc);
    };

    const auto precond = args.get_string("precond");
    crd::hesap::iterative::IterativeResult<R> res(args.alloc);
    if (precond == crd::containers::StringView{"jacobi"})
    {
        JacobiPreconditioner<T> m(a, args.alloc);
        res = run(&m);
    }
    else if (precond == crd::containers::StringView{"block_jacobi"})
    {
        const crd::u32 bs = static_cast<crd::u32>(args.get_u64("block_size").value_or(4U));
        BlockJacobiPreconditioner<T> m(a, bs, args.alloc);
        res = run(&m);
    }
    else if (precond == crd::containers::StringView{"ssor"})
    {
        const R omega = static_cast<R>(args.get_f64("omega").value_or(1.0));
        SsorPreconditioner<T> m(a, omega, args.alloc);
        res = run(&m);
    }
    else if (precond == crd::containers::StringView{"spai"}) // classical SPAI M ≈ A⁻¹ (general A)
    {
        const R eps = static_cast<R>(args.get_f64("spai_eps").value_or(0.4));
        const crd::u32 fill = static_cast<crd::u32>(args.get_u64("spai_fill").value_or(0U));
        SpaiPreconditioner<T> m(a, args.alloc, read_spai_pattern<T>(args), eps, fill);
        res = run(&m);
    }
    else if (precond == crd::containers::StringView{"schwarz"}) // restricted additive Schwarz (general A)
    {
        SchwarzPreconditioner<T> m = make_schwarz<T>(args, a, SchwarzType::Restricted);
        res = run(&m);
    }
    else if (precond == crd::containers::StringView{"mlilu"}) // MC64-preprocessed multilevel ILU (general A)
    {
        const crd::u32 lfil = static_cast<crd::u32>(args.get_u64("lfil").value_or(0U));
        const R droptol = static_cast<R>(args.get_f64("droptol").value_or(-1.0));
        MultilevelIlu<T> m(a, args.alloc, lfil, droptol);
        res = run(&m);
    }
    else if (precond == crd::containers::StringView{"mlilu_ib"}) // inverse-based multilevel ILU (ILUPACK core)
    {
        const R condest = static_cast<R>(args.get_f64("condest").value_or(-1.0));
        const R droptol = static_cast<R>(args.get_f64("droptol").value_or(-1.0));
        const bool reorder = args.get_bool("reorder").value_or(true); // v4z: per-level AMD reorder, default-ON
        InverseBasedIlu<T> m(a, args.alloc, condest, droptol, 0U, 50U, 64U, Mc64Mode::None, R(0), reorder);
        res = run(&m);
    }
    else
    {
        res = run(nullptr);
    }

    crd::containers::Array<crd::f64> out(args.alloc);
    out.push_back(static_cast<crd::f64>(res.iterations));
    out.push_back(static_cast<crd::f64>(res.final_residual_norm));
    out.push_back(res.converged ? 1.0 : 0.0);
    push_vec<T>(out, xvec, n);
    return blob_f64_result(args.alloc, crd::containers::ConstSpan<crd::f64>{out.data(), out.size()});
}

// hesap.iterative.{lsqr,lsmr}.<T> : least-squares for a RECTANGULAR (rows×cols) A.
// b has rows() entries; x (output) has cols(). IsLsmr: false=LSQR, true=LSMR.
// Optional column preconditioner: precond none (default) | jacobi (column-Jacobi
// M = diag(AᴴA)⁻¹). Square Jacobi / block-Jacobi / SSOR are intentionally NOT
// offered -- they need a square operator a least-squares A does not have.
template <typename T, bool IsLsmr> CommandResult impl_least_squares(const CommandArgs& args)
{
    using R = RealType<T>;
    const auto rows = args.get_u64("rows");
    const auto cols = args.get_u64("cols");
    if (!rows || !cols)
    {
        return error_result(args.alloc, "least-squares: rows and cols are required");
    }
    const crd::u32 m = static_cast<crd::u32>(*rows);
    const crd::u32 n = static_cast<crd::u32>(*cols);
    const auto bin = args.get_f64_array("b");
    const crd::usize expect = is_complex_v<T> ? static_cast<crd::usize>(m) * 2 : static_cast<crd::usize>(m);
    if (bin.size() != expect)
    {
        return error_result(args.alloc, "least-squares: b must have rows() entries (2*rows flattened complex)");
    }

    auto a = build_csr_rect<T>(args, m, n);
    SparseLinearOp<T> op(a);
    crd::hesap::dense::Vector<T> bvec(args.alloc, m);
    crd::hesap::dense::Vector<T> xvec(args.alloc, n); // x0 = 0
    read_vec<T>(args, "b", bvec, m);

    crd::hesap::iterative::IterativeOptions<R> opts;
    if (const auto rt = args.get_f64("rel_tol"))
    {
        opts.rel_tol = static_cast<R>(*rt);
    }
    if (const auto mi = args.get_u64("max_iter"))
    {
        opts.max_iter = static_cast<crd::usize>(*mi);
    }

    auto run = [&](const crd::hesap::LinearOp<T>* nprec)
    {
        crd::hesap::iterative::IterativeResult<R> r(args.alloc);
        if constexpr (IsLsmr)
        {
            crd::hesap::iterative::LsmrWorkspace<T> ws(args.alloc, m, n);
            r = crd::hesap::iterative::lsmr<T>(op, nprec, bvec.span(), xvec.span(), opts, ws, args.alloc);
        }
        else
        {
            crd::hesap::iterative::LsqrWorkspace<T> ws(args.alloc, m, n);
            r = crd::hesap::iterative::lsqr<T>(op, nprec, bvec.span(), xvec.span(), opts, ws, args.alloc);
        }
        return r;
    };

    const auto precond = args.get_string("precond");
    crd::hesap::iterative::IterativeResult<R> res(args.alloc);
    if (precond == crd::containers::StringView{"jacobi"})
    {
        LeastSquaresColumnJacobi<T> nprec(a, args.alloc);
        res = run(&nprec);
    }
    else
    {
        res = run(nullptr); // plain (unpreconditioned) LSQR/LSMR
    }

    crd::containers::Array<crd::f64> out(args.alloc);
    out.push_back(static_cast<crd::f64>(res.iterations));
    out.push_back(static_cast<crd::f64>(res.final_residual_norm));
    out.push_back(res.converged ? 1.0 : 0.0);
    push_vec<T>(out, xvec, n);
    return blob_f64_result(args.alloc, crd::containers::ConstSpan<crd::f64>{out.data(), out.size()});
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

void add_matrix_params(CommandSchema& s, crd::memory::IAllocator* alloc)
{
    add_param(s, alloc, "rows", "Matrix rows (== cols)", ParamKind::U64, true);
    add_param(s, alloc, "triplet_rows", "COO row indices (I64Array)", ParamKind::I64, true);
    add_param(s, alloc, "triplet_cols", "COO column indices (I64Array)", ParamKind::I64, true);
    add_param(s, alloc, "values", "COO values (F64Array; complex flattened)", ParamKind::F64, true);
}

CommandSchema make_jacobi_schema(crd::memory::IAllocator* alloc, const char* name, const char* desc)
{
    CommandSchema s{alloc};
    s.name = crd::containers::String{name, alloc};
    s.description = crd::containers::String{desc, alloc};
    s.output.kind = OutputKind::BinaryBlob;
    s.required_caps.bits = Capability::kHesapCompute;
    s.idempotent = true;
    add_matrix_params(s, alloc);
    add_param(s, alloc, "r", "Vector to precondition (F64Array; n real or 2n complex)", ParamKind::F64, true);
    return s;
}

CommandSchema make_ilut_schema(crd::memory::IAllocator* alloc, const char* name, const char* desc)
{
    CommandSchema s = make_jacobi_schema(alloc, name, desc);
    add_param(s, alloc, "lfil", "ILUT max fill per L/U row (default nnz/n+5)", ParamKind::U64, false);
    add_param(s, alloc, "droptol", "ILUT relative drop tolerance (default 1e-4)", ParamKind::F64, false);
    return s;
}

CommandSchema make_ilup_schema(crd::memory::IAllocator* alloc, const char* name, const char* desc)
{
    CommandSchema s = make_jacobi_schema(alloc, name, desc);
    add_param(s, alloc, "level", "ILU(p) level of fill (default 0 = ILU(0))", ParamKind::U64, false);
    return s;
}

CommandSchema make_mlilu_ib_schema(crd::memory::IAllocator* alloc, const char* name, const char* desc)
{
    CommandSchema s = make_jacobi_schema(alloc, name, desc);
    add_param(s, alloc, "condest", "Inverse-factor bound κ (default 5; tighter ⇒ more deferral)", ParamKind::F64,
              false);
    add_param(s, alloc, "droptol", "Inverse-based drop tolerance (default 1e-2)", ParamKind::F64, false);
    add_param(s, alloc, "reorder",
              "Per-level AMD fill-reducing reorder (default true; cuts iters ~2-4x, matches ILUPACK)", ParamKind::Bool,
              false);
    return s;
}

CommandSchema make_spai_schema(crd::memory::IAllocator* alloc, const char* name, const char* desc)
{
    CommandSchema s = make_jacobi_schema(alloc, name, desc);
    add_param(s, alloc, "spai_pattern", "Pattern: static (default) | adaptive", ParamKind::String, false);
    add_param(s, alloc, "spai_eps", "Adaptive residual tolerance", ParamKind::F64, false);
    add_param(s, alloc, "spai_fill", "Max fill per column (0 = auto)", ParamKind::U64, false);
    return s;
}

CommandSchema make_chebyshev_schema(crd::memory::IAllocator* alloc, const char* name, const char* desc)
{
    CommandSchema s = make_jacobi_schema(alloc, name, desc);
    add_param(s, alloc, "degree", "Polynomial degree (default 4)", ParamKind::U64, false);
    add_param(s, alloc, "cheb_lo_ratio", "λmin/λmax bracket ratio (default 1/30)", ParamKind::F64, false);
    return s;
}

CommandSchema make_schwarz_schema(crd::memory::IAllocator* alloc, const char* name, const char* desc)
{
    CommandSchema s = make_jacobi_schema(alloc, name, desc);
    add_param(s, alloc, "schwarz_block", "Base subdomain size (default 64)", ParamKind::U64, false);
    add_param(s, alloc, "schwarz_overlap", "Overlap layers (default 1)", ParamKind::U64, false);
    add_param(s, alloc, "schwarz_type", "additive (SPD) | restricted (default; general)", ParamKind::String, false);
    add_param(s, alloc, "schwarz_partition", "contiguous (default) | nd", ParamKind::String, false);
    return s;
}

CommandSchema make_pcg_schema(crd::memory::IAllocator* alloc, const char* name, const char* desc)
{
    CommandSchema s{alloc};
    s.name = crd::containers::String{name, alloc};
    s.description = crd::containers::String{desc, alloc};
    s.output.kind = OutputKind::BinaryBlob;
    s.required_caps.bits = Capability::kHesapCompute;
    s.idempotent = true;
    add_matrix_params(s, alloc);
    add_param(s, alloc, "cols", "Matrix columns (== rows)", ParamKind::U64, true);
    add_param(s, alloc, "b", "RHS vector (F64Array; n real or 2n complex)", ParamKind::F64, true);
    add_param(s, alloc, "rel_tol", "Relative residual tolerance (default per type)", ParamKind::F64, false);
    add_param(s, alloc, "max_iter", "Maximum iterations (default 1000)", ParamKind::U64, false);
    add_param(s, alloc, "precond",
              "Preconditioner: jacobi (default) | block_jacobi | ssor | ic0/fspai (SPD) | ilu0/ilut/ilup/spai",
              ParamKind::String, false);
    add_param(s, alloc, "block_size", "Block size for block_jacobi (default 4)", ParamKind::U64, false);
    add_param(s, alloc, "omega", "SSOR relaxation in (0,2) (default 1)", ParamKind::F64, false);
    add_param(s, alloc, "spai_pattern", "SPAI/FSPAI pattern: static (default) | adaptive", ParamKind::String, false);
    add_param(s, alloc, "spai_eps", "SPAI/FSPAI adaptive residual tolerance", ParamKind::F64, false);
    add_param(s, alloc, "spai_fill", "SPAI/FSPAI max fill per column (0 = auto)", ParamKind::U64, false);
    add_param(s, alloc, "degree", "Chebyshev polynomial degree (default 4)", ParamKind::U64, false);
    add_param(s, alloc, "cheb_lo_ratio", "Chebyshev λmin/λmax bracket ratio (default 1/30)", ParamKind::F64, false);
    add_param(s, alloc, "schwarz_block", "Schwarz base subdomain size (default 64)", ParamKind::U64, false);
    add_param(s, alloc, "schwarz_overlap", "Schwarz overlap layers (default 1)", ParamKind::U64, false);
    add_param(s, alloc, "schwarz_partition", "Schwarz partition: contiguous (default) | nd", ParamKind::String, false);
    return s;
}

CommandSchema make_fgmres_schema(crd::memory::IAllocator* alloc, const char* name, const char* desc)
{
    CommandSchema s = make_pcg_schema(alloc, name, desc); // matrix + cols + b + rel_tol + max_iter + precond/bs/omega
    add_param(s, alloc, "restart", "GMRES restart length m (default 30)", ParamKind::U64, false);
    return s;
}

CommandSchema make_block_pcg_schema(crd::memory::IAllocator* alloc, const char* name, const char* desc)
{
    CommandSchema s{alloc};
    s.name = crd::containers::String{name, alloc};
    s.description = crd::containers::String{desc, alloc};
    s.output.kind = OutputKind::BinaryBlob;
    s.required_caps.bits = Capability::kHesapCompute;
    s.idempotent = true;
    add_matrix_params(s, alloc);
    add_param(s, alloc, "cols", "Matrix columns (== rows)", ParamKind::U64, true);
    add_param(s, alloc, "s", "Number of right-hand sides (block width)", ParamKind::U64, true);
    add_param(s, alloc, "b", "RHS block n×s row-major (F64Array; n*s real or 2*n*s complex)", ParamKind::F64, true);
    add_param(s, alloc, "rel_tol", "Relative residual tolerance (default per type)", ParamKind::F64, false);
    add_param(s, alloc, "max_iter", "Maximum iterations (default 1000)", ParamKind::U64, false);
    return s;
}

CommandSchema make_block_jacobi_schema(crd::memory::IAllocator* alloc, const char* name, const char* desc)
{
    CommandSchema s = make_jacobi_schema(alloc, name, desc);
    add_param(s, alloc, "block_size", "Block size (default 4)", ParamKind::U64, false);
    return s;
}

CommandSchema make_ssor_schema(crd::memory::IAllocator* alloc, const char* name, const char* desc)
{
    CommandSchema s = make_jacobi_schema(alloc, name, desc);
    add_param(s, alloc, "omega", "Relaxation in (0,2) (default 1)", ParamKind::F64, false);
    return s;
}

CommandSchema make_gcrot_schema(crd::memory::IAllocator* alloc, const char* name, const char* desc)
{
    CommandSchema s = make_fgmres_schema(alloc, name, desc); // matrix + cols + b + tol + max_iter + precond + restart
    add_param(s, alloc, "recycle", "GCROT recycle-space dimension k (default 10)", ParamKind::U64, false);
    return s;
}

CommandSchema make_rminres_schema(crd::memory::IAllocator* alloc, const char* name, const char* desc)
{
    CommandSchema s{alloc};
    s.name = crd::containers::String{name, alloc};
    s.description = crd::containers::String{desc, alloc};
    s.output.kind = OutputKind::BinaryBlob;
    s.required_caps.bits = Capability::kHesapCompute;
    s.idempotent = true;
    add_matrix_params(s, alloc);
    add_param(s, alloc, "cols", "Matrix columns (== rows)", ParamKind::U64, true);
    add_param(s, alloc, "b", "RHS vector (F64Array; n real or 2n complex)", ParamKind::F64, true);
    add_param(s, alloc, "rel_tol", "Relative residual tolerance (default per type)", ParamKind::F64, false);
    add_param(s, alloc, "max_iter", "Maximum iterations (default 1000)", ParamKind::U64, false);
    add_param(s, alloc, "restart", "Inner Lanczos dimension m (default 20)", ParamKind::U64, false);
    add_param(s, alloc, "recycle", "Recycle-space dimension k (default 10)", ParamKind::U64, false);
    return s;
}

CommandSchema make_idrs_schema(crd::memory::IAllocator* alloc, const char* name, const char* desc)
{
    CommandSchema s = make_pcg_schema(alloc, name, desc); // matrix + cols + b + rel_tol + max_iter + precond/bs/omega
    add_param(s, alloc, "s", "IDR shadow-space dimension (default 4)", ParamKind::U64, false);
    return s;
}

CommandSchema make_least_squares_schema(crd::memory::IAllocator* alloc, const char* name, const char* desc)
{
    CommandSchema s{alloc};
    s.name = crd::containers::String{name, alloc};
    s.description = crd::containers::String{desc, alloc};
    s.output.kind = OutputKind::BinaryBlob;
    s.required_caps.bits = Capability::kHesapCompute;
    s.idempotent = true;
    add_param(s, alloc, "rows", "Matrix rows (m)", ParamKind::U64, true);
    add_param(s, alloc, "cols", "Matrix columns (n)", ParamKind::U64, true);
    add_param(s, alloc, "triplet_rows", "COO row indices (I64Array)", ParamKind::I64, true);
    add_param(s, alloc, "triplet_cols", "COO column indices (I64Array)", ParamKind::I64, true);
    add_param(s, alloc, "values", "COO values (F64Array; complex flattened)", ParamKind::F64, true);
    add_param(s, alloc, "b", "RHS vector (F64Array; rows real or 2*rows flattened complex)", ParamKind::F64, true);
    add_param(s, alloc, "rel_tol", "Relative tolerance (default per type)", ParamKind::F64, false);
    add_param(s, alloc, "max_iter", "Maximum iterations (default 1000)", ParamKind::U64, false);
    add_param(s, alloc, "precond", "Column preconditioner: none (default) | jacobi (diag(AᴴA)⁻¹)", ParamKind::String,
              false);
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

        reg.register_command(make_jacobi_schema(alloc, "hesap.precond.jacobi.f32", "Apply Jacobi M⁻¹r (f32)."),
                             &impl_jacobi<crd::f32>);
        reg.register_command(make_jacobi_schema(alloc, "hesap.precond.jacobi.f64", "Apply Jacobi M⁻¹r (f64)."),
                             &impl_jacobi<crd::f64>);
        reg.register_command(make_jacobi_schema(alloc, "hesap.precond.jacobi.c32", "Apply Jacobi M⁻¹r (Complex<f32>)."),
                             &impl_jacobi<crd::hesap::Complex<crd::f32>>);
        reg.register_command(make_jacobi_schema(alloc, "hesap.precond.jacobi.c64", "Apply Jacobi M⁻¹r (Complex<f64>)."),
                             &impl_jacobi<crd::hesap::Complex<crd::f64>>);

        reg.register_command(
            make_block_jacobi_schema(alloc, "hesap.precond.block_jacobi.f32", "Apply block-Jacobi M⁻¹r (f32)."),
            &impl_block_jacobi<crd::f32>);
        reg.register_command(
            make_block_jacobi_schema(alloc, "hesap.precond.block_jacobi.f64", "Apply block-Jacobi M⁻¹r (f64)."),
            &impl_block_jacobi<crd::f64>);
        reg.register_command(make_block_jacobi_schema(alloc, "hesap.precond.block_jacobi.c32",
                                                      "Apply block-Jacobi M⁻¹r (Complex<f32>)."),
                             &impl_block_jacobi<crd::hesap::Complex<crd::f32>>);
        reg.register_command(make_block_jacobi_schema(alloc, "hesap.precond.block_jacobi.c64",
                                                      "Apply block-Jacobi M⁻¹r (Complex<f64>)."),
                             &impl_block_jacobi<crd::hesap::Complex<crd::f64>>);

        reg.register_command(make_ssor_schema(alloc, "hesap.precond.ssor.f32", "Apply SSOR M⁻¹r (f32)."),
                             &impl_ssor<crd::f32>);
        reg.register_command(make_ssor_schema(alloc, "hesap.precond.ssor.f64", "Apply SSOR M⁻¹r (f64)."),
                             &impl_ssor<crd::f64>);
        reg.register_command(make_ssor_schema(alloc, "hesap.precond.ssor.c32", "Apply SSOR M⁻¹r (Complex<f32>)."),
                             &impl_ssor<crd::hesap::Complex<crd::f32>>);
        reg.register_command(make_ssor_schema(alloc, "hesap.precond.ssor.c64", "Apply SSOR M⁻¹r (Complex<f64>)."),
                             &impl_ssor<crd::hesap::Complex<crd::f64>>);

        reg.register_command(make_jacobi_schema(alloc, "hesap.precond.ic0.f32", "Apply IC(0) M⁻¹r (f32; SPD)."),
                             &impl_ic0<crd::f32>);
        reg.register_command(make_jacobi_schema(alloc, "hesap.precond.ic0.f64", "Apply IC(0) M⁻¹r (f64; SPD)."),
                             &impl_ic0<crd::f64>);
        reg.register_command(
            make_jacobi_schema(alloc, "hesap.precond.ic0.c32", "Apply IC(0) M⁻¹r (Complex<f32>; HPD)."),
            &impl_ic0<crd::hesap::Complex<crd::f32>>);
        reg.register_command(
            make_jacobi_schema(alloc, "hesap.precond.ic0.c64", "Apply IC(0) M⁻¹r (Complex<f64>; HPD)."),
            &impl_ic0<crd::hesap::Complex<crd::f64>>);
        reg.register_command(make_jacobi_schema(alloc, "hesap.precond.ilu0.f32", "Apply ILU(0) M⁻¹r (f32; general)."),
                             &impl_ilu0<crd::f32>);
        reg.register_command(make_jacobi_schema(alloc, "hesap.precond.ilu0.f64", "Apply ILU(0) M⁻¹r (f64; general)."),
                             &impl_ilu0<crd::f64>);
        reg.register_command(
            make_jacobi_schema(alloc, "hesap.precond.ilu0.c32", "Apply ILU(0) M⁻¹r (Complex<f32>; general)."),
            &impl_ilu0<crd::hesap::Complex<crd::f32>>);
        reg.register_command(
            make_jacobi_schema(alloc, "hesap.precond.ilu0.c64", "Apply ILU(0) M⁻¹r (Complex<f64>; general)."),
            &impl_ilu0<crd::hesap::Complex<crd::f64>>);
        reg.register_command(make_ilut_schema(alloc, "hesap.precond.ilut.f32", "Apply ILUT M⁻¹r (f32; lfil/droptol)."),
                             &impl_ilut<crd::f32>);
        reg.register_command(make_ilut_schema(alloc, "hesap.precond.ilut.f64", "Apply ILUT M⁻¹r (f64; lfil/droptol)."),
                             &impl_ilut<crd::f64>);
        reg.register_command(
            make_ilut_schema(alloc, "hesap.precond.ilut.c32", "Apply ILUT M⁻¹r (Complex<f32>; lfil/droptol)."),
            &impl_ilut<crd::hesap::Complex<crd::f32>>);
        reg.register_command(
            make_ilut_schema(alloc, "hesap.precond.ilut.c64", "Apply ILUT M⁻¹r (Complex<f64>; lfil/droptol)."),
            &impl_ilut<crd::hesap::Complex<crd::f64>>);
        reg.register_command(
            make_ilup_schema(alloc, "hesap.precond.ilup.f32", "Apply ILU(p) M⁻¹r (f32; level-of-fill)."),
            &impl_ilup<crd::f32>);
        reg.register_command(
            make_ilup_schema(alloc, "hesap.precond.ilup.f64", "Apply ILU(p) M⁻¹r (f64; level-of-fill)."),
            &impl_ilup<crd::f64>);
        reg.register_command(
            make_ilup_schema(alloc, "hesap.precond.ilup.c32", "Apply ILU(p) M⁻¹r (Complex<f32>; level-of-fill)."),
            &impl_ilup<crd::hesap::Complex<crd::f32>>);
        reg.register_command(
            make_ilup_schema(alloc, "hesap.precond.ilup.c64", "Apply ILU(p) M⁻¹r (Complex<f64>; level-of-fill)."),
            &impl_ilup<crd::hesap::Complex<crd::f64>>);

        reg.register_command(
            make_spai_schema(alloc, "hesap.precond.spai.f32", "Apply SPAI M≈A⁻¹ z=M·r (f32; general)."),
            &impl_spai<crd::f32>);
        reg.register_command(
            make_spai_schema(alloc, "hesap.precond.spai.f64", "Apply SPAI M≈A⁻¹ z=M·r (f64; general)."),
            &impl_spai<crd::f64>);
        reg.register_command(
            make_spai_schema(alloc, "hesap.precond.spai.c32", "Apply SPAI M≈A⁻¹ z=M·r (Complex<f32>; general)."),
            &impl_spai<crd::hesap::Complex<crd::f32>>);
        reg.register_command(
            make_spai_schema(alloc, "hesap.precond.spai.c64", "Apply SPAI M≈A⁻¹ z=M·r (Complex<f64>; general)."),
            &impl_spai<crd::hesap::Complex<crd::f64>>);
        reg.register_command(make_spai_schema(alloc, "hesap.precond.fspai.f32", "Apply FSPAI M=L·Lᴴ z=M·r (f32; SPD)."),
                             &impl_fspai<crd::f32>);
        reg.register_command(make_spai_schema(alloc, "hesap.precond.fspai.f64", "Apply FSPAI M=L·Lᴴ z=M·r (f64; SPD)."),
                             &impl_fspai<crd::f64>);
        reg.register_command(
            make_spai_schema(alloc, "hesap.precond.fspai.c32", "Apply FSPAI M=L·Lᴴ z=M·r (Complex<f32>; HPD)."),
            &impl_fspai<crd::hesap::Complex<crd::f32>>);
        reg.register_command(
            make_spai_schema(alloc, "hesap.precond.fspai.c64", "Apply FSPAI M=L·Lᴴ z=M·r (Complex<f64>; HPD)."),
            &impl_fspai<crd::hesap::Complex<crd::f64>>);

        reg.register_command(
            make_chebyshev_schema(alloc, "hesap.precond.chebyshev.f32", "Apply Chebyshev polynomial M⁻¹r (f32; SPD)."),
            &impl_chebyshev<crd::f32>);
        reg.register_command(
            make_chebyshev_schema(alloc, "hesap.precond.chebyshev.f64", "Apply Chebyshev polynomial M⁻¹r (f64; SPD)."),
            &impl_chebyshev<crd::f64>);
        reg.register_command(make_chebyshev_schema(alloc, "hesap.precond.chebyshev.c32",
                                                   "Apply Chebyshev polynomial M⁻¹r (Complex<f32>; HPD)."),
                             &impl_chebyshev<crd::hesap::Complex<crd::f32>>);
        reg.register_command(make_chebyshev_schema(alloc, "hesap.precond.chebyshev.c64",
                                                   "Apply Chebyshev polynomial M⁻¹r (Complex<f64>; HPD)."),
                             &impl_chebyshev<crd::hesap::Complex<crd::f64>>);

        reg.register_command(
            make_schwarz_schema(alloc, "hesap.precond.schwarz.f32", "Apply Schwarz domain-decomposition M⁻¹r (f32)."),
            &impl_schwarz<crd::f32>);
        reg.register_command(
            make_schwarz_schema(alloc, "hesap.precond.schwarz.f64", "Apply Schwarz domain-decomposition M⁻¹r (f64)."),
            &impl_schwarz<crd::f64>);
        reg.register_command(make_schwarz_schema(alloc, "hesap.precond.schwarz.c32",
                                                 "Apply Schwarz domain-decomposition M⁻¹r (Complex<f32>)."),
                             &impl_schwarz<crd::hesap::Complex<crd::f32>>);
        reg.register_command(make_schwarz_schema(alloc, "hesap.precond.schwarz.c64",
                                                 "Apply Schwarz domain-decomposition M⁻¹r (Complex<f64>)."),
                             &impl_schwarz<crd::hesap::Complex<crd::f64>>);

        reg.register_command(
            make_ilut_schema(alloc, "hesap.precond.mlilu.f32", "Apply multilevel ILU (MC64+ILUT) M⁻¹r (f32; general)."),
            &impl_mlilu<crd::f32>);
        reg.register_command(
            make_ilut_schema(alloc, "hesap.precond.mlilu.f64", "Apply multilevel ILU (MC64+ILUT) M⁻¹r (f64; general)."),
            &impl_mlilu<crd::f64>);
        reg.register_command(
            make_ilut_schema(alloc, "hesap.precond.mlilu.c32", "Apply multilevel ILU (MC64+ILUT) M⁻¹r (Complex<f32>)."),
            &impl_mlilu<crd::hesap::Complex<crd::f32>>);
        reg.register_command(
            make_ilut_schema(alloc, "hesap.precond.mlilu.c64", "Apply multilevel ILU (MC64+ILUT) M⁻¹r (Complex<f64>)."),
            &impl_mlilu<crd::hesap::Complex<crd::f64>>);

        reg.register_command(make_mlilu_ib_schema(alloc, "hesap.precond.mlilu_ib.f32",
                                                  "Apply inverse-based multilevel ILU M⁻¹r (f32; κ/droptol)."),
                             &impl_mlilu_ib<crd::f32>);
        reg.register_command(make_mlilu_ib_schema(alloc, "hesap.precond.mlilu_ib.f64",
                                                  "Apply inverse-based multilevel ILU M⁻¹r (f64; κ/droptol)."),
                             &impl_mlilu_ib<crd::f64>);
        reg.register_command(make_mlilu_ib_schema(alloc, "hesap.precond.mlilu_ib.c32",
                                                  "Apply inverse-based multilevel ILU M⁻¹r (Complex<f32>)."),
                             &impl_mlilu_ib<crd::hesap::Complex<crd::f32>>);
        reg.register_command(make_mlilu_ib_schema(alloc, "hesap.precond.mlilu_ib.c64",
                                                  "Apply inverse-based multilevel ILU M⁻¹r (Complex<f64>)."),
                             &impl_mlilu_ib<crd::hesap::Complex<crd::f64>>);

        reg.register_command(make_pcg_schema(alloc, "hesap.iterative.pcg.f32",
                                             "Jacobi-preconditioned CG (f32). [iters,resid,converged,x]."),
                             &impl_pcg<crd::f32>);
        reg.register_command(make_pcg_schema(alloc, "hesap.iterative.pcg.f64",
                                             "Jacobi-preconditioned CG (f64). [iters,resid,converged,x]."),
                             &impl_pcg<crd::f64>);
        reg.register_command(make_pcg_schema(alloc, "hesap.iterative.pcg.c32",
                                             "Jacobi-preconditioned CG (Complex<f32>). [iters,resid,converged,x]."),
                             &impl_pcg<crd::hesap::Complex<crd::f32>>);
        reg.register_command(make_pcg_schema(alloc, "hesap.iterative.pcg.c64",
                                             "Jacobi-preconditioned CG (Complex<f64>). [iters,resid,converged,x]."),
                             &impl_pcg<crd::hesap::Complex<crd::f64>>);

        reg.register_command(
            make_block_pcg_schema(alloc, "hesap.iterative.block_pcg.f32",
                                  "Jacobi-preconditioned block-CG multi-RHS (f32). [iters,resid,converged,X(n×s)]."),
            &impl_block_pcg<crd::f32>);
        reg.register_command(
            make_block_pcg_schema(alloc, "hesap.iterative.block_pcg.f64",
                                  "Jacobi-preconditioned block-CG multi-RHS (f64). [iters,resid,converged,X(n×s)]."),
            &impl_block_pcg<crd::f64>);
        reg.register_command(make_block_pcg_schema(alloc, "hesap.iterative.block_pcg.c32",
                                                   "Jacobi-preconditioned block-CG multi-RHS (Complex<f32>)."),
                             &impl_block_pcg<crd::hesap::Complex<crd::f32>>);
        reg.register_command(make_block_pcg_schema(alloc, "hesap.iterative.block_pcg.c64",
                                                   "Jacobi-preconditioned block-CG multi-RHS (Complex<f64>)."),
                             &impl_block_pcg<crd::hesap::Complex<crd::f64>>);

        reg.register_command(
            make_fgmres_schema(alloc, "hesap.iterative.fgmres.f32",
                               "(Flexible) restarted GMRES (f32; precond none|jacobi|block_jacobi|ssor)."),
            &impl_fgmres<crd::f32>);
        reg.register_command(
            make_fgmres_schema(alloc, "hesap.iterative.fgmres.f64",
                               "(Flexible) restarted GMRES (f64; precond none|jacobi|block_jacobi|ssor)."),
            &impl_fgmres<crd::f64>);
        reg.register_command(
            make_fgmres_schema(alloc, "hesap.iterative.fgmres.c32", "(Flexible) restarted GMRES (Complex<f32>)."),
            &impl_fgmres<crd::hesap::Complex<crd::f32>>);
        reg.register_command(
            make_fgmres_schema(alloc, "hesap.iterative.fgmres.c64", "(Flexible) restarted GMRES (Complex<f64>)."),
            &impl_fgmres<crd::hesap::Complex<crd::f64>>);

        reg.register_command(make_pcg_schema(alloc, "hesap.iterative.bicgstab.f32",
                                             "BiCGSTAB (f32; precond none|jacobi|block_jacobi|ssor)."),
                             &impl_bicgstab<crd::f32>);
        reg.register_command(make_pcg_schema(alloc, "hesap.iterative.bicgstab.f64",
                                             "BiCGSTAB (f64; precond none|jacobi|block_jacobi|ssor)."),
                             &impl_bicgstab<crd::f64>);
        reg.register_command(make_pcg_schema(alloc, "hesap.iterative.bicgstab.c32", "BiCGSTAB (Complex<f32>)."),
                             &impl_bicgstab<crd::hesap::Complex<crd::f32>>);
        reg.register_command(make_pcg_schema(alloc, "hesap.iterative.bicgstab.c64", "BiCGSTAB (Complex<f64>)."),
                             &impl_bicgstab<crd::hesap::Complex<crd::f64>>);

        reg.register_command(
            make_pcg_schema(alloc, "hesap.iterative.minres.f32",
                            "MINRES (f32; symmetric/indefinite-OK; precond none|jacobi|block_jacobi|ssor, SPD only)."),
            &impl_minres<crd::f32>);
        reg.register_command(
            make_pcg_schema(alloc, "hesap.iterative.minres.f64",
                            "MINRES (f64; symmetric/indefinite-OK; precond none|jacobi|block_jacobi|ssor, SPD only)."),
            &impl_minres<crd::f64>);
        reg.register_command(
            make_pcg_schema(alloc, "hesap.iterative.minres.c32", "MINRES (Complex<f32>; Hermitian/indefinite-OK)."),
            &impl_minres<crd::hesap::Complex<crd::f32>>);
        reg.register_command(
            make_pcg_schema(alloc, "hesap.iterative.minres.c64", "MINRES (Complex<f64>; Hermitian/indefinite-OK)."),
            &impl_minres<crd::hesap::Complex<crd::f64>>);

        reg.register_command(
            make_pcg_schema(alloc, "hesap.iterative.symmlq.f32",
                            "SYMMLQ (f32; symmetric/indefinite-OK; precond none|jacobi|block_jacobi|ssor, SPD only)."),
            &impl_symmlq<crd::f32>);
        reg.register_command(
            make_pcg_schema(alloc, "hesap.iterative.symmlq.f64",
                            "SYMMLQ (f64; symmetric/indefinite-OK; precond none|jacobi|block_jacobi|ssor, SPD only)."),
            &impl_symmlq<crd::f64>);
        reg.register_command(
            make_pcg_schema(alloc, "hesap.iterative.symmlq.c32", "SYMMLQ (Complex<f32>; Hermitian/indefinite-OK)."),
            &impl_symmlq<crd::hesap::Complex<crd::f32>>);
        reg.register_command(
            make_pcg_schema(alloc, "hesap.iterative.symmlq.c64", "SYMMLQ (Complex<f64>; Hermitian/indefinite-OK)."),
            &impl_symmlq<crd::hesap::Complex<crd::f64>>);

        reg.register_command(
            make_pcg_schema(alloc, "hesap.iterative.qmr.f32",
                            "QMR (f32; general nonsymmetric; right precond none|jacobi|block_jacobi|ssor)."),
            &impl_qmr<crd::f32>);
        reg.register_command(
            make_pcg_schema(alloc, "hesap.iterative.qmr.f64",
                            "QMR (f64; general nonsymmetric; right precond none|jacobi|block_jacobi|ssor)."),
            &impl_qmr<crd::f64>);
        reg.register_command(
            make_pcg_schema(alloc, "hesap.iterative.qmr.c32", "QMR (Complex<f32>; general non-Hermitian)."),
            &impl_qmr<crd::hesap::Complex<crd::f32>>);
        reg.register_command(
            make_pcg_schema(alloc, "hesap.iterative.qmr.c64", "QMR (Complex<f64>; general non-Hermitian)."),
            &impl_qmr<crd::hesap::Complex<crd::f64>>);

        reg.register_command(
            make_fgmres_schema(alloc, "hesap.iterative.gcr.f32",
                               "GCR(m) (f32; general nonsymmetric; restart + precond none|jacobi|block_jacobi|ssor)."),
            &impl_gcr<crd::f32>);
        reg.register_command(
            make_fgmres_schema(alloc, "hesap.iterative.gcr.f64",
                               "GCR(m) (f64; general nonsymmetric; restart + precond none|jacobi|block_jacobi|ssor)."),
            &impl_gcr<crd::f64>);
        reg.register_command(
            make_fgmres_schema(alloc, "hesap.iterative.gcr.c32", "GCR(m) (Complex<f32>; general non-Hermitian)."),
            &impl_gcr<crd::hesap::Complex<crd::f32>>);
        reg.register_command(
            make_fgmres_schema(alloc, "hesap.iterative.gcr.c64", "GCR(m) (Complex<f64>; general non-Hermitian)."),
            &impl_gcr<crd::hesap::Complex<crd::f64>>);

        reg.register_command(
            make_gcrot_schema(
                alloc, "hesap.iterative.gcrot.f32",
                "GCROT(m,k) recycling GMRES (f32; restart=m, recycle=k; precond none|jacobi|block_jacobi|ssor)."),
            &impl_gcrot<crd::f32>);
        reg.register_command(
            make_gcrot_schema(
                alloc, "hesap.iterative.gcrot.f64",
                "GCROT(m,k) recycling GMRES (f64; restart=m, recycle=k; precond none|jacobi|block_jacobi|ssor)."),
            &impl_gcrot<crd::f64>);
        reg.register_command(
            make_gcrot_schema(alloc, "hesap.iterative.gcrot.c32", "GCROT(m,k) recycling GMRES (Complex<f32>)."),
            &impl_gcrot<crd::hesap::Complex<crd::f32>>);
        reg.register_command(
            make_gcrot_schema(alloc, "hesap.iterative.gcrot.c64", "GCROT(m,k) recycling GMRES (Complex<f64>)."),
            &impl_gcrot<crd::hesap::Complex<crd::f64>>);

        reg.register_command(
            make_rminres_schema(alloc, "hesap.iterative.rminres.f32",
                                "RMINRES recycling MINRES (f32; symmetric/indefinite; restart=m, recycle=k)."),
            &impl_rminres<crd::f32>);
        reg.register_command(
            make_rminres_schema(alloc, "hesap.iterative.rminres.f64",
                                "RMINRES recycling MINRES (f64; symmetric/indefinite; restart=m, recycle=k)."),
            &impl_rminres<crd::f64>);
        reg.register_command(make_rminres_schema(alloc, "hesap.iterative.rminres.c32",
                                                 "RMINRES recycling MINRES (Complex<f32>; Hermitian/indefinite)."),
                             &impl_rminres<crd::hesap::Complex<crd::f32>>);
        reg.register_command(make_rminres_schema(alloc, "hesap.iterative.rminres.c64",
                                                 "RMINRES recycling MINRES (Complex<f64>; Hermitian/indefinite)."),
                             &impl_rminres<crd::hesap::Complex<crd::f64>>);

        reg.register_command(
            make_idrs_schema(alloc, "hesap.iterative.idrs.f32",
                             "IDR(s) (f32; general nonsymmetric; s + precond none|jacobi|block_jacobi|ssor)."),
            &impl_idrs<crd::f32>);
        reg.register_command(
            make_idrs_schema(alloc, "hesap.iterative.idrs.f64",
                             "IDR(s) (f64; general nonsymmetric; s + precond none|jacobi|block_jacobi|ssor)."),
            &impl_idrs<crd::f64>);
        reg.register_command(
            make_idrs_schema(alloc, "hesap.iterative.idrs.c32", "IDR(s) (Complex<f32>; general non-Hermitian)."),
            &impl_idrs<crd::hesap::Complex<crd::f32>>);
        reg.register_command(
            make_idrs_schema(alloc, "hesap.iterative.idrs.c64", "IDR(s) (Complex<f64>; general non-Hermitian)."),
            &impl_idrs<crd::hesap::Complex<crd::f64>>);

        reg.register_command(
            make_least_squares_schema(
                alloc, "hesap.iterative.lsqr.f32",
                "LSQR least-squares for rectangular A (f32; precond none|jacobi). [iters,resid,converged,x]."),
            &impl_least_squares<crd::f32, false>);
        reg.register_command(
            make_least_squares_schema(
                alloc, "hesap.iterative.lsqr.f64",
                "LSQR least-squares for rectangular A (f64; precond none|jacobi). [iters,resid,converged,x]."),
            &impl_least_squares<crd::f64, false>);
        reg.register_command(make_least_squares_schema(alloc, "hesap.iterative.lsqr.c32",
                                                       "LSQR least-squares (Complex<f32>; precond none|jacobi)."),
                             &impl_least_squares<crd::hesap::Complex<crd::f32>, false>);
        reg.register_command(make_least_squares_schema(alloc, "hesap.iterative.lsqr.c64",
                                                       "LSQR least-squares (Complex<f64>; precond none|jacobi)."),
                             &impl_least_squares<crd::hesap::Complex<crd::f64>, false>);

        reg.register_command(
            make_least_squares_schema(
                alloc, "hesap.iterative.lsmr.f32",
                "LSMR least-squares for rectangular A (f32; precond none|jacobi). [iters,ArNorm,converged,x]."),
            &impl_least_squares<crd::f32, true>);
        reg.register_command(
            make_least_squares_schema(
                alloc, "hesap.iterative.lsmr.f64",
                "LSMR least-squares for rectangular A (f64; precond none|jacobi). [iters,ArNorm,converged,x]."),
            &impl_least_squares<crd::f64, true>);
        reg.register_command(make_least_squares_schema(alloc, "hesap.iterative.lsmr.c32",
                                                       "LSMR least-squares (Complex<f32>; precond none|jacobi)."),
                             &impl_least_squares<crd::hesap::Complex<crd::f32>, true>);
        reg.register_command(make_least_squares_schema(alloc, "hesap.iterative.lsmr.c64",
                                                       "LSMR least-squares (Complex<f64>; precond none|jacobi)."),
                             &impl_least_squares<crd::hesap::Complex<crd::f64>, true>);
    });
