// v3a-1 — CLI registration for the symmetric eigensolver eig_sym. Each
// command takes (A symmetric lower-half, n) and returns the n ascending
// eigenvalues as a binary blob. (Eigenvectors are available via the engine
// API; the CLI returns the spectrum, the primary agent-facing query.)
//
// Anchor symbol: `register_eig_cli_anchor()` in `cli_anchor.hpp`.

#include <crd/hesap/cli/arg_value.hpp>
#include <crd/hesap/cli/command_registry.hpp>
#include <crd/hesap/complex.hpp>
#include <crd/hesap/dense/cli_anchor.hpp>
#include <crd/hesap/dense/eig_nonsym.hpp>
#include <crd/hesap/dense/eig_sym.hpp>
#include <crd/hesap/dense/matrix.hpp>
#include <crd/hesap/dense/matrix_types.hpp>

#include <utility>

namespace
{
using crd::hesap::Complex;
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
using crd::hesap::dense::eig;
using crd::hesap::dense::eig_herm;
using crd::hesap::dense::eig_sym;
using crd::hesap::dense::eig_sym_mrrr;
using crd::hesap::dense::eigvals_sym;
using crd::hesap::dense::Hermitian;
using crd::hesap::dense::Matrix;
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

template <typename T> CommandResult impl_eig_sym(const CommandArgs& args)
{
    const auto a_flat = args.get_f64_array("A");
    const auto n = args.get_u64("n").value_or(crd::u64{0});
    if (n == 0 || a_flat.size() != n * n)
    {
        return error_result(args.alloc, "eig.sym: A=n*n (symmetric, lower-half), n required");
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
    const auto eig = eig_sym<T>(args.alloc, a_sym);
    crd::containers::Array<crd::f64> out(args.alloc);
    out.reserve(nn);
    for (crd::usize i = 0; i < nn; ++i)
    {
        out.push_back(static_cast<crd::f64>(eig.values.data()[i]));
    }
    return binary_result(args.alloc, crd::containers::ConstSpan<crd::f64>{out.data(), out.size()});
}

// Full eigendecomposition via MRRR (v3a-3): same input shape as eig.sym; returns
// the n ascending eigenvalues as an f64 blob (vectors available via the engine).
template <typename T> CommandResult impl_eig_sym_mrrr(const CommandArgs& args)
{
    const auto a_flat = args.get_f64_array("A");
    const auto n = args.get_u64("n").value_or(crd::u64{0});
    if (n == 0 || a_flat.size() != n * n)
    {
        return error_result(args.alloc, "eig.sym.mrrr: A=n*n (symmetric, lower-half), n required");
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
    const auto eig = eig_sym_mrrr<T>(args.alloc, a_sym);
    crd::containers::Array<crd::f64> out(args.alloc);
    out.reserve(nn);
    for (crd::usize i = 0; i < nn; ++i)
    {
        out.push_back(static_cast<crd::f64>(eig.values.data()[i]));
    }
    return binary_result(args.alloc, crd::containers::ConstSpan<crd::f64>{out.data(), out.size()});
}

// Eigenvalues-only via the fast MRRR dqds path (v3a-3.1). Same input shape as
// eig.sym; returns the n ascending eigenvalues as an f64 binary blob.
template <typename T> CommandResult impl_eigvals_sym(const CommandArgs& args)
{
    const auto a_flat = args.get_f64_array("A");
    const auto n = args.get_u64("n").value_or(crd::u64{0});
    if (n == 0 || a_flat.size() != n * n)
    {
        return error_result(args.alloc, "eigvals.sym: A=n*n (symmetric, lower-half), n required");
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
    const auto vals = eigvals_sym<T>(args.alloc, a_sym);
    crd::containers::Array<crd::f64> out(args.alloc);
    out.reserve(nn);
    for (crd::usize i = 0; i < nn; ++i)
    {
        out.push_back(static_cast<crd::f64>(vals.data()[i]));
    }
    return binary_result(args.alloc, crd::containers::ConstSpan<crd::f64>{out.data(), out.size()});
}

// Hermitian eigenvalues. A travels as an interleaved complex array
// [re0,im0, re1,im1, ...] of n*n entries (lower triangle used); the n real
// ascending eigenvalues are returned as an f64 binary blob.
template <typename U> CommandResult impl_eig_herm(const CommandArgs& args)
{
    using C = Complex<U>;
    const auto a_flat = args.get_f64_array("A");
    const auto n = args.get_u64("n").value_or(crd::u64{0});
    if (n == 0 || a_flat.size() != n * n * 2)
    {
        return error_result(args.alloc, "eig.herm: A=2*n*n interleaved [re,im] (lower-half), n required");
    }
    const crd::usize nn = static_cast<crd::usize>(n);
    Hermitian<C> a_herm(args.alloc, nn);
    for (crd::usize i = 0; i < nn; ++i)
    {
        for (crd::usize j = 0; j <= i; ++j)
        {
            const crd::usize k = (i * nn + j) * 2;
            a_herm.at_lower(i, j) = C{static_cast<U>(a_flat[k]), static_cast<U>(a_flat[k + 1])};
        }
    }
    const auto eig = eig_herm<C>(args.alloc, a_herm);
    crd::containers::Array<crd::f64> out(args.alloc);
    out.reserve(nn);
    for (crd::usize i = 0; i < nn; ++i)
    {
        out.push_back(static_cast<crd::f64>(eig.values.data()[i]));
    }
    return binary_result(args.alloc, crd::containers::ConstSpan<crd::f64>{out.data(), out.size()});
}

// Non-symmetric eigenvalues. A travels as a FULL n*n flattened real matrix
// (row-major); the n complex eigenvalues are returned interleaved
// [re0,im0, re1,im1, ...] (2*n f64) in Schur order. (Eigenvectors are
// available via the engine API; the CLI returns the spectrum.)
template <typename T> CommandResult impl_eig_nonsym(const CommandArgs& args)
{
    const auto a_flat = args.get_f64_array("A");
    const auto n = args.get_u64("n").value_or(crd::u64{0});
    if (n == 0 || a_flat.size() != n * n)
    {
        return error_result(args.alloc, "eig.nonsym: A=n*n (full, row-major), n required");
    }
    const crd::usize nn = static_cast<crd::usize>(n);
    Matrix<T> a(args.alloc, nn, nn);
    for (crd::usize i = 0; i < nn; ++i)
    {
        for (crd::usize j = 0; j < nn; ++j)
        {
            a.at(i, j) = static_cast<T>(a_flat[i * nn + j]);
        }
    }
    const auto e = eig<T>(args.alloc, a);
    crd::containers::Array<crd::f64> out(args.alloc);
    out.reserve(nn * 2);
    for (crd::usize i = 0; i < nn; ++i)
    {
        out.push_back(static_cast<crd::f64>(e.values.data()[i].re));
        out.push_back(static_cast<crd::f64>(e.values.data()[i].im));
    }
    return binary_result(args.alloc, crd::containers::ConstSpan<crd::f64>{out.data(), out.size()});
}

// Complex non-symmetric eig. A travels as an interleaved complex array
// [re,im, ...] of n*n entries (FULL matrix, row-major); the n complex
// eigenvalues (Schur order) are returned as interleaved [re,im] f64 (values-only,
// matching the real eig.nonsym CLI; vectors via the engine API).
template <typename U> CommandResult impl_eig_nonsym_complex(const CommandArgs& args)
{
    using C = Complex<U>;
    const auto a_flat = args.get_f64_array("A");
    const auto n = args.get_u64("n").value_or(crd::u64{0});
    if (n == 0 || a_flat.size() != n * n * 2)
    {
        return error_result(args.alloc, "eig.nonsym(complex): A=2*n*n interleaved [re,im] (full), n required");
    }
    const crd::usize nn = static_cast<crd::usize>(n);
    Matrix<C> a(args.alloc, nn, nn);
    for (crd::usize i = 0; i < nn; ++i)
    {
        for (crd::usize j = 0; j < nn; ++j)
        {
            const crd::usize k = (i * nn + j) * 2;
            a.at(i, j) = C{static_cast<U>(a_flat[k]), static_cast<U>(a_flat[k + 1])};
        }
    }
    const auto e = eig<C>(args.alloc, a);
    crd::containers::Array<crd::f64> out(args.alloc);
    out.reserve(nn * 2);
    for (crd::usize i = 0; i < nn; ++i)
    {
        out.push_back(static_cast<crd::f64>(e.values.data()[i].re));
        out.push_back(static_cast<crd::f64>(e.values.data()[i].im));
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
void register_eig_cli_anchor() noexcept {}
} // namespace crd::hesap::dense

// Registration uses crd allocators (abort on OOM, never throw); the std bad_alloc path the check
// traces is unreachable, and the registrar ctor is noexcept (would terminate, not escape) regardless.
// NOLINTNEXTLINE(bugprone-throwing-static-initialization)
CRD_HESAP_CLI_REGISTER_MODULE(
    [](CommandRegistry& reg)
    {
        auto* alloc = crd::memory::default_allocator();
        {
            auto s = make_schema(alloc, "hesap.dense.eig.sym.f32",
                                 "Symmetric eigenvalues (ascending) of A (f32; lower triangle used).");
            add_param(s, alloc, "n", "Order of A", ParamKind::U64, true);
            add_param(s, alloc, "A", "Symmetric A flattened (n*n); lower triangle used", ParamKind::F64, true);
            reg.register_command(std::move(s), &impl_eig_sym<crd::f32>);
        }
        {
            auto s = make_schema(alloc, "hesap.dense.eig.sym.f64",
                                 "Symmetric eigenvalues (ascending) of A (f64; lower triangle used).");
            add_param(s, alloc, "n", "Order of A", ParamKind::U64, true);
            add_param(s, alloc, "A", "Symmetric A flattened (n*n); lower triangle used", ParamKind::F64, true);
            reg.register_command(std::move(s), &impl_eig_sym<crd::f64>);
        }
        {
            auto s = make_schema(alloc, "hesap.dense.eigvals.sym.f32",
                                 "Symmetric eigenvalues only (ascending) via fast MRRR dqds (f32; lower triangle).");
            add_param(s, alloc, "n", "Order of A", ParamKind::U64, true);
            add_param(s, alloc, "A", "Symmetric A flattened (n*n); lower triangle used", ParamKind::F64, true);
            reg.register_command(std::move(s), &impl_eigvals_sym<crd::f32>);
        }
        {
            auto s = make_schema(alloc, "hesap.dense.eigvals.sym.f64",
                                 "Symmetric eigenvalues only (ascending) via fast MRRR dqds (f64; lower triangle).");
            add_param(s, alloc, "n", "Order of A", ParamKind::U64, true);
            add_param(s, alloc, "A", "Symmetric A flattened (n*n); lower triangle used", ParamKind::F64, true);
            reg.register_command(std::move(s), &impl_eigvals_sym<crd::f64>);
        }
        {
            auto s = make_schema(alloc, "hesap.dense.eig.sym.mrrr.f32",
                                 "Symmetric eigenvalues (ascending) via MRRR (f32; lower triangle).");
            add_param(s, alloc, "n", "Order of A", ParamKind::U64, true);
            add_param(s, alloc, "A", "Symmetric A flattened (n*n); lower triangle used", ParamKind::F64, true);
            reg.register_command(std::move(s), &impl_eig_sym_mrrr<crd::f32>);
        }
        {
            auto s = make_schema(alloc, "hesap.dense.eig.sym.mrrr.f64",
                                 "Symmetric eigenvalues (ascending) via MRRR (f64; lower triangle).");
            add_param(s, alloc, "n", "Order of A", ParamKind::U64, true);
            add_param(s, alloc, "A", "Symmetric A flattened (n*n); lower triangle used", ParamKind::F64, true);
            reg.register_command(std::move(s), &impl_eig_sym_mrrr<crd::f64>);
        }
        {
            auto s = make_schema(alloc, "hesap.dense.eig.herm.c32",
                                 "Hermitian eigenvalues (ascending) of A (Complex<f32>; lower triangle used).");
            add_param(s, alloc, "n", "Order of A", ParamKind::U64, true);
            add_param(s, alloc, "A", "Hermitian A as interleaved [re,im] (2*n*n); lower triangle used", ParamKind::F64,
                      true);
            reg.register_command(std::move(s), &impl_eig_herm<crd::f32>);
        }
        {
            auto s = make_schema(alloc, "hesap.dense.eig.herm.c64",
                                 "Hermitian eigenvalues (ascending) of A (Complex<f64>; lower triangle used).");
            add_param(s, alloc, "n", "Order of A", ParamKind::U64, true);
            add_param(s, alloc, "A", "Hermitian A as interleaved [re,im] (2*n*n); lower triangle used", ParamKind::F64,
                      true);
            reg.register_command(std::move(s), &impl_eig_herm<crd::f64>);
        }
        {
            auto s = make_schema(alloc, "hesap.dense.eig.nonsym.f32",
                                 "Non-symmetric eigenvalues (interleaved [re,im], Schur order) of full A (f32).");
            add_param(s, alloc, "n", "Order of A", ParamKind::U64, true);
            add_param(s, alloc, "A", "Full A flattened row-major (n*n)", ParamKind::F64, true);
            reg.register_command(std::move(s), &impl_eig_nonsym<crd::f32>);
        }
        {
            auto s = make_schema(alloc, "hesap.dense.eig.nonsym.f64",
                                 "Non-symmetric eigenvalues (interleaved [re,im], Schur order) of full A (f64).");
            add_param(s, alloc, "n", "Order of A", ParamKind::U64, true);
            add_param(s, alloc, "A", "Full A flattened row-major (n*n)", ParamKind::F64, true);
            reg.register_command(std::move(s), &impl_eig_nonsym<crd::f64>);
        }
        {
            auto s =
                make_schema(alloc, "hesap.dense.eig.nonsym.c32",
                            "Non-symmetric eigenvalues (interleaved [re,im], Schur order) of complex full A (c32).");
            add_param(s, alloc, "n", "Order of A", ParamKind::U64, true);
            add_param(s, alloc, "A", "Full A interleaved [re,im] row-major (2*n*n)", ParamKind::F64, true);
            reg.register_command(std::move(s), &impl_eig_nonsym_complex<crd::f32>);
        }
        {
            auto s =
                make_schema(alloc, "hesap.dense.eig.nonsym.c64",
                            "Non-symmetric eigenvalues (interleaved [re,im], Schur order) of complex full A (c64).");
            add_param(s, alloc, "n", "Order of A", ParamKind::U64, true);
            add_param(s, alloc, "A", "Full A interleaved [re,im] row-major (2*n*n)", ParamKind::F64, true);
            reg.register_command(std::move(s), &impl_eig_nonsym_complex<crd::f64>);
        }
    });
