// Smoothed-Aggregation AMG CLI registration. Phase 3.1.6 v4k-c.
//
// Registers hesap.amg.{f32,f64,c32,c64} -- AMG-as-SOLVER via a stationary
// multigrid-cycle iteration x += M⁻¹(b − A x) (V/W/F/K cycle; θ strength). Output
// [iters, resid, converged, x]. Self-contained: crd-hesap-amg does not depend on
// crd-hesap-iterative, so this is the textbook AMG standalone solver (no Krylov);
// AMG-as-preconditioner is exercised by the iterative solvers' tests/bench. The
// cycle iteration uses a SERIAL spmv ⇒ the CLI is a deterministic oracle.

#include <crd/hesap/amg/amg.hpp>
#include <crd/hesap/amg/cli_anchor.hpp>
#include <crd/hesap/cli/arg_value.hpp>
#include <crd/hesap/cli/command_registry.hpp>
#include <crd/hesap/complex.hpp>
#include <crd/hesap/dense/real_type.hpp>
#include <crd/hesap/dense/vector.hpp>
#include <crd/hesap/sparse/sparse_linear_op.hpp>
#include <crd/hesap/sparse/sparse_matrix.hpp>
#include <crd/hesap/sparse/triplet_builder.hpp>

#include <cmath>
#include <utility>

namespace crd::hesap::amg
{
void register_amg_cli_anchor() noexcept {}
} // namespace crd::hesap::amg

namespace
{
using namespace crd::hesap;
using namespace crd::hesap::cli;
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

template <typename T> typename crd::hesap::amg::SaAmg<T>::Cycle parse_cycle(const CommandArgs& args)
{
    using C = typename crd::hesap::amg::SaAmg<T>::Cycle;
    const auto cycle = args.get_string("cycle");
    if (cycle == crd::containers::StringView{"w"})
    {
        return C::W;
    }
    if (cycle == crd::containers::StringView{"f"})
    {
        return C::F;
    }
    if (cycle == crd::containers::StringView{"k"})
    {
        return C::K;
    }
    return C::V; // default
}

// hesap.amg.<T> : AMG-as-solver. Stationary cycle iteration x ← x + M⁻¹(b − A x),
// M⁻¹ = one V/W/F/K cycle of the SA-AMG hierarchy. Converges on the diffusion /
// mild-convection regime AMG handles standalone (strong convection needs an outer
// Krylov — use the iterative solvers with AMG as preconditioner). [iters,resid,conv,x].
template <typename T> CommandResult impl_amg(const CommandArgs& args)
{
    using R = RealType<T>;
    const auto rows = args.get_u64("rows");
    if (!rows)
    {
        return error_result(args.alloc, "amg: rows (== cols) is required");
    }
    const crd::u32 n = static_cast<crd::u32>(*rows);
    const auto bin = args.get_f64_array("b");
    const crd::usize expect = is_complex_v<T> ? static_cast<crd::usize>(n) * 2 : static_cast<crd::usize>(n);
    if (bin.size() != expect)
    {
        return error_result(args.alloc, "amg: b has wrong length (n real or 2n complex)");
    }

    auto a = build_csr<T>(args, n);
    if (!a.pattern().is_compressed() || a.rows() != n)
    {
        return error_result(args.alloc, "amg: failed to build matrix");
    }

    typename crd::hesap::amg::SaAmg<T>::Options opts;
    opts.cycle = parse_cycle<T>(args);
    if (args.get_string("coarsening") == crd::containers::StringView{"rs"})
    {
        opts.coarsening = crd::hesap::amg::SaAmg<T>::Coarsening::RugeStuben; // classical Ruge-Stüben
    }
    if (const auto th = args.get_f64("theta"))
    {
        opts.theta = static_cast<R>(*th);
    }
    crd::hesap::amg::SaAmg<T> amg(a, args.alloc, opts);

    SparseLinearOp<T> op(a);
    crd::hesap::dense::Vector<T> bvec(args.alloc, n);
    crd::hesap::dense::Vector<T> x(args.alloc, n);
    crd::hesap::dense::Vector<T> ax(args.alloc, n);
    crd::hesap::dense::Vector<T> z(args.alloc, n);
    read_vec<T>(args, "b", bvec, n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        x(i) = T{};
    }

    const R rel_tol = static_cast<R>(args.get_f64("rel_tol").value_or(1e-8));
    const crd::u32 max_iter = static_cast<crd::u32>(args.get_u64("max_iter").value_or(200U));

    auto norm2 = [&](const crd::hesap::dense::Vector<T>& v)
    {
        R s = R(0);
        for (crd::u32 i = 0; i < n; ++i)
        {
            if constexpr (is_complex_v<T>)
            {
                s += v(i).re * v(i).re + v(i).im * v(i).im;
            }
            else
            {
                s += v(i) * v(i);
            }
        }
        return std::sqrt(s);
    };

    const R bnorm = norm2(bvec);
    const R denom = (bnorm > R(0)) ? bnorm : R(1);
    crd::u32 iters = 0;
    R resid = R(1);
    bool converged = false;
    for (iters = 0; iters < max_iter; ++iters)
    {
        (void)op.apply(x.span(), ax.span()); // ax = A x
        for (crd::u32 i = 0; i < n; ++i)
        {
            ax(i) = bvec(i) - ax(i);
        } // ax = r = b − A x
        resid = norm2(ax) / denom;
        if (resid <= rel_tol)
        {
            converged = true;
            break;
        }
        (void)amg.apply(crd::containers::ConstSpan<T>{ax.data(), n}, z.span()); // z = M⁻¹ r
        for (crd::u32 i = 0; i < n; ++i)
        {
            x(i) = x(i) + z(i);
        } // x += z
    }

    crd::containers::Array<crd::f64> out(args.alloc);
    out.push_back(static_cast<crd::f64>(iters));
    out.push_back(static_cast<crd::f64>(resid));
    out.push_back(converged ? 1.0 : 0.0);
    push_vec<T>(out, x, n);
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

CommandSchema make_amg_schema(crd::memory::IAllocator* alloc, const char* name, const char* desc)
{
    CommandSchema s{alloc};
    s.name = crd::containers::String{name, alloc};
    s.description = crd::containers::String{desc, alloc};
    s.output.kind = OutputKind::BinaryBlob;
    s.required_caps.bits = Capability::kHesapCompute;
    s.idempotent = true;
    add_param(s, alloc, "rows", "Matrix rows (== cols)", ParamKind::U64, true);
    add_param(s, alloc, "triplet_rows", "COO row indices (I64Array)", ParamKind::I64, true);
    add_param(s, alloc, "triplet_cols", "COO column indices (I64Array)", ParamKind::I64, true);
    add_param(s, alloc, "values", "COO values (F64Array; complex flattened)", ParamKind::F64, true);
    add_param(s, alloc, "b", "RHS vector (F64Array; n real or 2n flattened complex)", ParamKind::F64, true);
    add_param(s, alloc, "cycle", "Multigrid cycle: v (default) | w | f | k", ParamKind::String, false);
    add_param(s, alloc, "coarsening", "Coarsening: sa (smoothed aggregation, default) | rs (Ruge-Stüben)",
              ParamKind::String, false);
    add_param(s, alloc, "theta", "Strength-of-connection threshold (default 0.08)", ParamKind::F64, false);
    add_param(s, alloc, "rel_tol", "Relative residual tolerance (default 1e-8)", ParamKind::F64, false);
    add_param(s, alloc, "max_iter", "Maximum cycle iterations (default 200)", ParamKind::U64, false);
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

        reg.register_command(
            make_amg_schema(alloc, "hesap.amg.f32", "SA-AMG solver (f32; cycle v/w/f/k). [iters,resid,converged,x]."),
            &impl_amg<crd::f32>);
        reg.register_command(
            make_amg_schema(alloc, "hesap.amg.f64", "SA-AMG solver (f64; cycle v/w/f/k). [iters,resid,converged,x]."),
            &impl_amg<crd::f64>);
        reg.register_command(make_amg_schema(alloc, "hesap.amg.c32", "SA-AMG solver (Complex<f32>; cycle v/w/f/k)."),
                             &impl_amg<crd::hesap::Complex<crd::f32>>);
        reg.register_command(make_amg_schema(alloc, "hesap.amg.c64", "SA-AMG solver (Complex<f64>; cycle v/w/f/k)."),
                             &impl_amg<crd::hesap::Complex<crd::f64>>);
    });
