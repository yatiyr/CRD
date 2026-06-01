// v5a-2 — CLI registration for the sparse DIRECT supernodal Cholesky.
//
// hesap.direct.chol.{f32,f64,c32,c64}: factor + solve A·x = b for a sparse
// SPD (real) / HPD (complex Hermitian) matrix given as COO triplets (full
// symmetric/Hermitian; the lower triangle is used) plus an RHS b. One-shot
// factor+solve (serial, num_workers=1 → deterministic, no jobs::init needed).
// Returns [info, x...] where info=0 on success (k+1 = non-positive pivot at
// column k); x is flattened {re,im,...} for complex.
//
// Complex values/RHS travel as interleaved {re,im} pairs in an F64Array
// (the hesap CLI convention, mirroring hesap.iterative.cg.c32/c64).
//
// Anchor symbol: `register_direct_cli_anchor()` in `cli_anchor.hpp`.

#include <crd/hesap/cli/arg_value.hpp>
#include <crd/hesap/cli/command_registry.hpp>
#include <crd/hesap/complex.hpp>
#include <crd/hesap/dense/real_type.hpp>
#include <crd/hesap/direct/multifrontal_lu.hpp>
#include <crd/hesap/direct/sparse_lu.hpp>
#include <crd/hesap/direct/supernodal_cholesky.hpp>
#include <crd/hesap/sparse/convert.hpp>
#include <crd/hesap/sparse/triplet_builder.hpp>

#include <utility>

namespace
{
using namespace crd::hesap;
using namespace crd::hesap::cli;
using crd::hesap::dense::is_complex_v;
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

template <typename T> CommandResult impl_chol(const CommandArgs& args)
{
    using R = RealType<T>;
    const auto rows = args.get_u64("rows");
    const auto cols = args.get_u64("cols");
    if (!rows || !cols || *rows != *cols)
    {
        return error_result(args.alloc, "chol: rows and cols are required and must be equal (square SPD/HPD)");
    }
    const crd::u32 n = static_cast<crd::u32>(*rows);

    const auto tr = args.get_i64_array("triplet_rows");
    const auto tc = args.get_i64_array("triplet_cols");
    const auto vals = args.get_f64_array("values");
    const crd::usize stride = is_complex_v<T> ? 2U : 1U;
    if (tr.size() != tc.size() || vals.size() != stride * tr.size())
    {
        return error_result(args.alloc,
                            "chol: triplet_rows/cols length mismatch, or values != (1|2)*nnz (complex flattened)");
    }
    const auto bin = args.get_f64_array("b");
    if (bin.size() != stride * static_cast<crd::usize>(n))
    {
        return error_result(args.alloc, "chol: b has wrong length (n real, or 2n flattened complex)");
    }

    crd::hesap::sparse::TripletBuilder<T> tb(args.alloc, n, n);
    tb.reserve(tr.size());
    for (crd::usize k = 0; k < tr.size(); ++k)
    {
        if constexpr (is_complex_v<T>)
        {
            tb.add(static_cast<crd::u32>(tr[k]), static_cast<crd::u32>(tc[k]),
                   T{static_cast<R>(vals[2 * k]), static_cast<R>(vals[2 * k + 1])});
        }
        else
        {
            tb.add(static_cast<crd::u32>(tr[k]), static_cast<crd::u32>(tc[k]), static_cast<T>(vals[k]));
        }
    }
    auto a = tb.compress();
    const auto& av = a.values().values;
    auto f = direct::factor_supernodal_cholesky<T>(a.pattern(), {av.data(), av.size()}, args.alloc,
                                                   direct::kSupernodeRelax, 1);
    if (f.info() != 0)
    {
        return error_result(args.alloc, "chol: matrix is not positive-definite (non-positive pivot)");
    }

    crd::containers::Array<T> x(args.alloc);
    x.resize(n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        if constexpr (is_complex_v<T>)
        {
            x[i] = T{static_cast<R>(bin[2 * i]), static_cast<R>(bin[2 * i + 1])};
        }
        else
        {
            x[i] = static_cast<T>(bin[i]);
        }
    }
    if (!f.solve({x.data(), n}))
    {
        return error_result(args.alloc, "chol: solve failed");
    }

    crd::containers::Array<crd::f64> out(args.alloc);
    out.reserve(static_cast<crd::usize>(n) * stride + 1);
    out.push_back(static_cast<crd::f64>(f.info())); // 0 = success
    for (crd::u32 i = 0; i < n; ++i)
    {
        if constexpr (is_complex_v<T>)
        {
            out.push_back(static_cast<crd::f64>(x[i].re));
            out.push_back(static_cast<crd::f64>(x[i].im));
        }
        else
        {
            out.push_back(static_cast<crd::f64>(x[i]));
        }
    }
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

CommandSchema make_chol_schema(crd::memory::IAllocator* alloc, const char* name, const char* desc)
{
    CommandSchema s{alloc};
    s.name = crd::containers::String{name, alloc};
    s.description = crd::containers::String{desc, alloc};
    s.output.kind = OutputKind::BinaryBlob;
    s.required_caps.bits = Capability::kHesapCompute;
    s.idempotent = true;
    add_param(s, alloc, "rows", "Matrix rows (== cols; square SPD/HPD)", ParamKind::U64, true);
    add_param(s, alloc, "cols", "Matrix columns (== rows)", ParamKind::U64, true);
    add_param(s, alloc, "triplet_rows", "COO row indices (I64Array; full symmetric/Hermitian)", ParamKind::I64, true);
    add_param(s, alloc, "triplet_cols", "COO column indices (I64Array)", ParamKind::I64, true);
    add_param(s, alloc, "values", "COO values (F64Array; complex flattened {re,im,...})", ParamKind::F64, true);
    add_param(s, alloc, "b", "RHS vector (F64Array; n real or 2n flattened complex)", ParamKind::F64, true);
    return s;
}

// hesap.direct.lu_gp.{f32,f64,c32,c64}: Gilbert-Peierls sparse LU factor+solve A·x = b for a GENERAL
// (unsymmetric) sparse matrix given as COO triplets (the full matrix) + RHS b. Serial reference oracle
// (v5b-1); dynamic partial pivot. Returns [info, x...]; info=0 on success, k+1 = singular at column k.
template <typename T> CommandResult impl_lu(const CommandArgs& args)
{
    using R = RealType<T>;
    const auto rows = args.get_u64("rows");
    const auto cols = args.get_u64("cols");
    if (!rows || !cols || *rows != *cols)
    {
        return error_result(args.alloc, "lu_gp: rows and cols are required and must be equal (square)");
    }
    const crd::u32 n = static_cast<crd::u32>(*rows);

    const auto tr = args.get_i64_array("triplet_rows");
    const auto tc = args.get_i64_array("triplet_cols");
    const auto vals = args.get_f64_array("values");
    const crd::usize stride = is_complex_v<T> ? 2U : 1U;
    if (tr.size() != tc.size() || vals.size() != stride * tr.size())
    {
        return error_result(args.alloc,
                            "lu_gp: triplet_rows/cols length mismatch, or values != (1|2)*nnz (complex flattened)");
    }
    const auto bin = args.get_f64_array("b");
    if (bin.size() != stride * static_cast<crd::usize>(n))
    {
        return error_result(args.alloc, "lu_gp: b has wrong length (n real, or 2n flattened complex)");
    }

    crd::hesap::sparse::TripletBuilder<T> tb(args.alloc, n, n);
    tb.reserve(tr.size());
    for (crd::usize k = 0; k < tr.size(); ++k)
    {
        if constexpr (is_complex_v<T>)
        {
            tb.add(static_cast<crd::u32>(tr[k]), static_cast<crd::u32>(tc[k]),
                   T{static_cast<R>(vals[2 * k]), static_cast<R>(vals[2 * k + 1])});
        }
        else
        {
            tb.add(static_cast<crd::u32>(tr[k]), static_cast<crd::u32>(tc[k]), static_cast<T>(vals[k]));
        }
    }
    auto a = tb.compress();                                            // CSR
    auto acsc = crd::hesap::sparse::to_csc<T>(a, args.alloc);          // LU is column-oriented
    auto f = direct::factor_gp_lu<T>(acsc, args.alloc);
    if (f.info() != 0)
    {
        return error_result(args.alloc, "lu_gp: matrix is singular (zero pivot)");
    }

    crd::containers::Array<T> x(args.alloc);
    x.resize(n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        if constexpr (is_complex_v<T>)
        {
            x[i] = T{static_cast<R>(bin[2 * i]), static_cast<R>(bin[2 * i + 1])};
        }
        else
        {
            x[i] = static_cast<T>(bin[i]);
        }
    }
    if (!f.solve({x.data(), n}))
    {
        return error_result(args.alloc, "lu_gp: solve failed");
    }

    crd::containers::Array<crd::f64> out(args.alloc);
    out.reserve(static_cast<crd::usize>(n) * stride + 1);
    out.push_back(static_cast<crd::f64>(f.info()));
    for (crd::u32 i = 0; i < n; ++i)
    {
        if constexpr (is_complex_v<T>)
        {
            out.push_back(static_cast<crd::f64>(x[i].re));
            out.push_back(static_cast<crd::f64>(x[i].im));
        }
        else
        {
            out.push_back(static_cast<crd::f64>(x[i]));
        }
    }
    return blob_f64_result(args.alloc, crd::containers::ConstSpan<crd::f64>{out.data(), out.size()});
}

CommandSchema make_lu_schema(crd::memory::IAllocator* alloc, const char* name, const char* desc)
{
    CommandSchema s{alloc};
    s.name = crd::containers::String{name, alloc};
    s.description = crd::containers::String{desc, alloc};
    s.output.kind = OutputKind::BinaryBlob;
    s.required_caps.bits = Capability::kHesapCompute;
    s.idempotent = true;
    add_param(s, alloc, "rows", "Matrix rows (== cols; square)", ParamKind::U64, true);
    add_param(s, alloc, "cols", "Matrix columns (== rows)", ParamKind::U64, true);
    add_param(s, alloc, "triplet_rows", "COO row indices (I64Array; full general/unsymmetric)", ParamKind::I64, true);
    add_param(s, alloc, "triplet_cols", "COO column indices (I64Array)", ParamKind::I64, true);
    add_param(s, alloc, "values", "COO values (F64Array; complex flattened {re,im,...})", ParamKind::F64, true);
    add_param(s, alloc, "b", "RHS vector (F64Array; n real or 2n flattened complex)", ParamKind::F64, true);
    return s;
}

// hesap.direct.lu.{f32,f64,c32,c64}: the PRODUCTION sparse LU — factor+solve A·x = b for a GENERAL sparse
// matrix given as COO triplets + RHS b. DISPATCHER (per-matrix): the multifrontal LU (adaptive-MC64 static
// pivot — the crush kernel for structured/CFD, also handling circuit + saddle-point) is the PRIMARY; the
// Gilbert-Peierls dynamic-pivot oracle is the robustness FALLBACK if static pivoting cannot factor/solve.
// Serial (num_workers=1 → deterministic, no jobs::init needed). Returns [info, x...]; info=0 on success.
template <typename T> CommandResult impl_lu_direct(const CommandArgs& args)
{
    using R = RealType<T>;
    const auto rows = args.get_u64("rows");
    const auto cols = args.get_u64("cols");
    if (!rows || !cols || *rows != *cols)
    {
        return error_result(args.alloc, "lu: rows and cols are required and must be equal (square)");
    }
    const crd::u32 n = static_cast<crd::u32>(*rows);

    const auto tr = args.get_i64_array("triplet_rows");
    const auto tc = args.get_i64_array("triplet_cols");
    const auto vals = args.get_f64_array("values");
    const crd::usize stride = is_complex_v<T> ? 2U : 1U;
    if (tr.size() != tc.size() || vals.size() != stride * tr.size())
    {
        return error_result(args.alloc,
                            "lu: triplet_rows/cols length mismatch, or values != (1|2)*nnz (complex flattened)");
    }
    const auto bin = args.get_f64_array("b");
    if (bin.size() != stride * static_cast<crd::usize>(n))
    {
        return error_result(args.alloc, "lu: b has wrong length (n real, or 2n flattened complex)");
    }

    crd::hesap::sparse::TripletBuilder<T> tb(args.alloc, n, n);
    tb.reserve(tr.size());
    for (crd::usize k = 0; k < tr.size(); ++k)
    {
        if constexpr (is_complex_v<T>)
        {
            tb.add(static_cast<crd::u32>(tr[k]), static_cast<crd::u32>(tc[k]),
                   T{static_cast<R>(vals[2 * k]), static_cast<R>(vals[2 * k + 1])});
        }
        else
        {
            tb.add(static_cast<crd::u32>(tr[k]), static_cast<crd::u32>(tc[k]), static_cast<T>(vals[k]));
        }
    }
    auto a = tb.compress(); // CSR

    // Re-seed x = b before each kernel attempt (a failed solve leaves x partially overwritten).
    auto seed_x = [&](crd::containers::Array<T>& x)
    {
        x.resize(n);
        for (crd::u32 i = 0; i < n; ++i)
        {
            if constexpr (is_complex_v<T>)
            {
                x[i] = T{static_cast<R>(bin[2 * i]), static_cast<R>(bin[2 * i + 1])};
            }
            else
            {
                x[i] = static_cast<T>(bin[i]);
            }
        }
    };
    auto emit = [&](const crd::containers::Array<T>& x) -> CommandResult
    {
        crd::containers::Array<crd::f64> out(args.alloc);
        out.reserve(static_cast<crd::usize>(n) * stride + 1);
        out.push_back(0.0); // info = 0 (success)
        for (crd::u32 i = 0; i < n; ++i)
        {
            if constexpr (is_complex_v<T>)
            {
                out.push_back(static_cast<crd::f64>(x[i].re));
                out.push_back(static_cast<crd::f64>(x[i].im));
            }
            else
            {
                out.push_back(static_cast<crd::f64>(x[i]));
            }
        }
        return blob_f64_result(args.alloc, crd::containers::ConstSpan<crd::f64>{out.data(), out.size()});
    };

    // PRIMARY: multifrontal LU (serial, deterministic; internal adaptive MC64 static pivot + IR solve).
    crd::containers::Array<T> x(args.alloc);
    seed_x(x);
    auto mf = direct::factor_multifrontal_lu<T>(a, args.alloc, 1);
    if (mf.info() == 0 && mf.solve({x.data(), n}))
    {
        return emit(x);
    }

    // FALLBACK: Gilbert-Peierls dynamic-pivot oracle (robust where static pivoting cannot go).
    seed_x(x);
    auto acsc = crd::hesap::sparse::to_csc<T>(a, args.alloc);
    auto gp = direct::factor_gp_lu<T>(acsc, args.alloc);
    if (gp.info() != 0 || !gp.solve({x.data(), n}))
    {
        return error_result(args.alloc, "lu: matrix is singular (multifrontal and GP-LU both failed)");
    }
    return emit(x);
}
} // namespace

namespace crd::hesap::direct
{
void register_direct_cli_anchor() noexcept {}
} // namespace crd::hesap::direct

// Registration uses crd allocators (abort on OOM, never throw); the std bad_alloc path the check
// traces is unreachable, and the registrar ctor is noexcept (would terminate, not escape) regardless.
// NOLINTNEXTLINE(bugprone-throwing-static-initialization)
CRD_HESAP_CLI_REGISTER_MODULE(
    [](CommandRegistry& reg)
    {
        auto* alloc = crd::memory::default_allocator();
        reg.register_command(
            make_chol_schema(alloc, "hesap.direct.chol.f32",
                             "Supernodal Cholesky factor+solve A x = b (f32 SPD). Returns [info, x]."),
            &impl_chol<crd::f32>);
        reg.register_command(
            make_chol_schema(alloc, "hesap.direct.chol.f64",
                             "Supernodal Cholesky factor+solve A x = b (f64 SPD). Returns [info, x]."),
            &impl_chol<crd::f64>);
        reg.register_command(
            make_chol_schema(alloc, "hesap.direct.chol.c32",
                             "Supernodal Cholesky LLᴴ factor+solve A x = b (Complex<f32> HPD). Returns [info, x]."),
            &impl_chol<crd::hesap::Complex<crd::f32>>);
        reg.register_command(
            make_chol_schema(alloc, "hesap.direct.chol.c64",
                             "Supernodal Cholesky LLᴴ factor+solve A x = b (Complex<f64> HPD). Returns [info, x]."),
            &impl_chol<crd::hesap::Complex<crd::f64>>);
        reg.register_command(
            make_lu_schema(alloc, "hesap.direct.lu_gp.f32",
                           "Gilbert-Peierls sparse LU factor+solve A x = b (f32 general). Returns [info, x]."),
            &impl_lu<crd::f32>);
        reg.register_command(
            make_lu_schema(alloc, "hesap.direct.lu_gp.f64",
                           "Gilbert-Peierls sparse LU factor+solve A x = b (f64 general). Returns [info, x]."),
            &impl_lu<crd::f64>);
        reg.register_command(
            make_lu_schema(alloc, "hesap.direct.lu_gp.c32",
                           "Gilbert-Peierls sparse LU factor+solve A x = b (Complex<f32> general). Returns [info, x]."),
            &impl_lu<crd::hesap::Complex<crd::f32>>);
        reg.register_command(
            make_lu_schema(alloc, "hesap.direct.lu_gp.c64",
                           "Gilbert-Peierls sparse LU factor+solve A x = b (Complex<f64> general). Returns [info, x]."),
            &impl_lu<crd::hesap::Complex<crd::f64>>);
        reg.register_command(
            make_lu_schema(alloc, "hesap.direct.lu.f32",
                           "Sparse LU factor+solve A x = b (f32 general; multifrontal adaptive-MC64 + GP-LU "
                           "fallback). Returns [info, x]."),
            &impl_lu_direct<crd::f32>);
        reg.register_command(
            make_lu_schema(alloc, "hesap.direct.lu.f64",
                           "Sparse LU factor+solve A x = b (f64 general; multifrontal adaptive-MC64 + GP-LU "
                           "fallback). Returns [info, x]."),
            &impl_lu_direct<crd::f64>);
        reg.register_command(
            make_lu_schema(alloc, "hesap.direct.lu.c32",
                           "Sparse LU factor+solve A x = b (Complex<f32> general; multifrontal adaptive-MC64 + "
                           "GP-LU fallback). Returns [info, x]."),
            &impl_lu_direct<crd::hesap::Complex<crd::f32>>);
        reg.register_command(
            make_lu_schema(alloc, "hesap.direct.lu.c64",
                           "Sparse LU factor+solve A x = b (Complex<f64> general; multifrontal adaptive-MC64 + "
                           "GP-LU fallback). Returns [info, x]."),
            &impl_lu_direct<crd::hesap::Complex<crd::f64>>);
    });
