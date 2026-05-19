// BLAS L1 CLI command registration. Phase 3.1.6 v0b.
//
// Registers 36 CommandSchemas via the CRD_HESAP_CLI_REGISTER_MODULE
// static-init hook (per ADR-0081 §7 + §10): every BLAS L1 op × 4 type
// variants is reachable from the CLI / RPC / MCP surface.
//
// v0b inline-JSON-array shape (D14): vectors travel as F64Array. Complex
// vectors travel as flattened {re, im, re, im, ...} F64Array (length 2N
// for an N-vector). When the VectorRegistry lands (v0c follow-on) the
// schemas bump major version; v1.0 stays Deprecated for ≥2 minor versions
// per ADR-0081 §2.
//
// Each command's impl reads its args, runs the op via the templated
// engine surface (`crd::hesap::dense::axpy<T>` etc.), and returns either
// a typed scalar / array / handle / void via CommandResult.

#include <crd/hesap/cli/arg_value.hpp>
#include <crd/hesap/cli/command_registry.hpp>
#include <crd/hesap/dense/blas1.hpp>
#include <crd/hesap/dense/cli_anchor.hpp>
#include <crd/hesap/dense/real_type.hpp>
#include <crd/hesap/dense/vector.hpp>

#include <utility>

namespace crd::hesap::dense
{
// Anchor definition — exported so downstream consumers can reference it
// and force the linker to pull this TU in (along with the static-init below).
void register_blas1_cli_anchor() noexcept
{
}
} // namespace crd::hesap::dense

namespace
{

using namespace crd::hesap;
using namespace crd::hesap::cli;
using namespace crd::hesap::dense;

// -----------------------------------------------------------------------
// Helpers — pack/unpack flattened complex {re, im, ...} arrays.
// -----------------------------------------------------------------------

template <typename T>
Vector<Complex<T>> unpack_complex_array(
    crd::containers::ConstSpan<crd::f64> flat,
    crd::memory::IAllocator* alloc)
{
    const crd::usize n = flat.size() / 2;
    Vector<Complex<T>> v(alloc, n);
    for (crd::usize i = 0; i < n; ++i)
    {
        v(i) = Complex<T>{static_cast<T>(flat[2 * i]), static_cast<T>(flat[2 * i + 1])};
    }
    return v;
}

template <typename T>
crd::containers::Array<crd::f64> pack_complex_array(
    crd::containers::ConstSpan<Complex<T>> v,
    crd::memory::IAllocator* alloc)
{
    crd::containers::Array<crd::f64> out(alloc);
    out.reserve(v.size() * 2);
    for (crd::usize i = 0; i < v.size(); ++i)
    {
        out.push_back(static_cast<crd::f64>(v[i].re));
        out.push_back(static_cast<crd::f64>(v[i].im));
    }
    return out;
}

template <typename T>
Vector<T> unpack_real_array(
    crd::containers::ConstSpan<crd::f64> flat,
    crd::memory::IAllocator* alloc)
{
    Vector<T> v(alloc, flat.size());
    for (crd::usize i = 0; i < flat.size(); ++i)
    {
        v(i) = static_cast<T>(flat[i]);
    }
    return v;
}

template <typename T>
crd::containers::Array<crd::f64> pack_real_array(
    crd::containers::ConstSpan<T> v,
    crd::memory::IAllocator* alloc)
{
    crd::containers::Array<crd::f64> out(alloc);
    out.reserve(v.size());
    for (crd::usize i = 0; i < v.size(); ++i)
    {
        out.push_back(static_cast<crd::f64>(v[i]));
    }
    return out;
}

// -----------------------------------------------------------------------
// CommandResult builders.
// -----------------------------------------------------------------------

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

CommandResult void_result(crd::memory::IAllocator* alloc)
{
    CommandResult r{alloc};
    r.ok = true;
    r.value = ResultVoid{};
    return r;
}

CommandResult scalar_result(crd::memory::IAllocator* alloc, crd::f64 value)
{
    CommandResult r{alloc};
    r.ok = true;
    r.value = ResultScalarF64{value};
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

// -----------------------------------------------------------------------
// Real BLAS L1 impls (templated over T ∈ {f32, f64}).
// Param shape:
//   alpha (F64 scalar; optional, default 1.0)
//   x     (F64Array required)
//   y     (F64Array, only for ops that need it)
// Result:
//   axpy / scal / copy / swap: BinaryBlob containing the updated array(s)
//   dot / nrm2 / asum: ScalarF64
//   iamax: ScalarF64 (returned as f64 since u64 not in v0a output types)
// -----------------------------------------------------------------------

template <typename T>
CommandResult impl_axpy_real(const CommandArgs& args)
{
    const auto alpha = args.get_f64("alpha").value_or(crd::f64{1.0});
    const auto x = args.get_f64_array("x");
    const auto y_in = args.get_f64_array("y");
    if (x.empty() || x.size() != y_in.size())
    {
        return error_result(args.alloc, "axpy: x and y must be non-empty arrays of equal length");
    }
    Vector<T> xv = unpack_real_array<T>(x, args.alloc);
    Vector<T> yv = unpack_real_array<T>(y_in, args.alloc);
    axpy<T>(static_cast<T>(alpha), xv, yv);
    auto out = pack_real_array<T>(yv.span(), args.alloc);
    return binary_result(args.alloc, crd::containers::ConstSpan<crd::f64>{out.data(), out.size()});
}

template <typename T>
CommandResult impl_dot_real(const CommandArgs& args)
{
    const auto x = args.get_f64_array("x");
    const auto y = args.get_f64_array("y");
    if (x.empty() || x.size() != y.size())
    {
        return error_result(args.alloc, "dot: x and y must be non-empty arrays of equal length");
    }
    Vector<T> xv = unpack_real_array<T>(x, args.alloc);
    Vector<T> yv = unpack_real_array<T>(y, args.alloc);
    const T v = dot<T>(xv, yv);
    return scalar_result(args.alloc, static_cast<crd::f64>(v));
}

template <typename T>
CommandResult impl_nrm2_real(const CommandArgs& args)
{
    const auto x = args.get_f64_array("x");
    if (x.empty())
    {
        return error_result(args.alloc, "nrm2: x must be a non-empty array");
    }
    Vector<T> xv = unpack_real_array<T>(x, args.alloc);
    const T v = nrm2<T>(xv);
    return scalar_result(args.alloc, static_cast<crd::f64>(v));
}

template <typename T>
CommandResult impl_scal_real(const CommandArgs& args)
{
    const auto alpha = args.get_f64("alpha").value_or(crd::f64{1.0});
    const auto x = args.get_f64_array("x");
    if (x.empty())
    {
        return error_result(args.alloc, "scal: x must be a non-empty array");
    }
    Vector<T> xv = unpack_real_array<T>(x, args.alloc);
    scal<T>(static_cast<T>(alpha), xv);
    auto out = pack_real_array<T>(xv.span(), args.alloc);
    return binary_result(args.alloc, crd::containers::ConstSpan<crd::f64>{out.data(), out.size()});
}

template <typename T>
CommandResult impl_copy_real(const CommandArgs& args)
{
    const auto src = args.get_f64_array("src");
    if (src.empty())
    {
        return error_result(args.alloc, "copy: src must be a non-empty array");
    }
    Vector<T> v = unpack_real_array<T>(src, args.alloc);
    auto out = pack_real_array<T>(v.span(), args.alloc);
    return binary_result(args.alloc, crd::containers::ConstSpan<crd::f64>{out.data(), out.size()});
}

template <typename T>
CommandResult impl_asum_real(const CommandArgs& args)
{
    const auto x = args.get_f64_array("x");
    if (x.empty())
    {
        return error_result(args.alloc, "asum: x must be a non-empty array");
    }
    Vector<T> xv = unpack_real_array<T>(x, args.alloc);
    const T v = asum<T>(xv);
    return scalar_result(args.alloc, static_cast<crd::f64>(v));
}

template <typename T>
CommandResult impl_iamax_real(const CommandArgs& args)
{
    const auto x = args.get_f64_array("x");
    if (x.empty())
    {
        return error_result(args.alloc, "iamax: x must be a non-empty array");
    }
    Vector<T> xv = unpack_real_array<T>(x, args.alloc);
    const crd::usize idx = iamax<T>(xv);
    return scalar_result(args.alloc, static_cast<crd::f64>(idx));
}

// -----------------------------------------------------------------------
// Complex BLAS L1 impls (T = Complex<U>, U ∈ {f32, f64}).
// Param shape:
//   alpha (F64Array of size 2 = [re, im], optional)
//   x     (F64Array of size 2N; flattened [re0, im0, re1, im1, ...])
//   y     (F64Array of size 2N for ops that need it)
// -----------------------------------------------------------------------

template <typename U>
CommandResult impl_axpy_complex(const CommandArgs& args)
{
    const auto alpha_flat = args.get_f64_array("alpha");
    Complex<U> alpha{U(1), U(0)};
    if (alpha_flat.size() >= 2)
    {
        alpha = Complex<U>{static_cast<U>(alpha_flat[0]), static_cast<U>(alpha_flat[1])};
    }
    const auto x = args.get_f64_array("x");
    const auto y_in = args.get_f64_array("y");
    if (x.empty() || x.size() != y_in.size() || (x.size() % 2) != 0)
    {
        return error_result(args.alloc, "axpy: complex x/y must be even-length flattened {re,im,...}");
    }
    Vector<Complex<U>> xv = unpack_complex_array<U>(x, args.alloc);
    Vector<Complex<U>> yv = unpack_complex_array<U>(y_in, args.alloc);
    axpy<Complex<U>>(alpha, xv, yv);
    auto out = pack_complex_array<U>(yv.span(), args.alloc);
    return binary_result(args.alloc, crd::containers::ConstSpan<crd::f64>{out.data(), out.size()});
}

template <typename U>
CommandResult impl_dotu_complex(const CommandArgs& args)
{
    const auto x = args.get_f64_array("x");
    const auto y = args.get_f64_array("y");
    if (x.empty() || x.size() != y.size() || (x.size() % 2) != 0)
    {
        return error_result(args.alloc, "dotu: complex x/y must be even-length flattened {re,im,...}");
    }
    Vector<Complex<U>> xv = unpack_complex_array<U>(x, args.alloc);
    Vector<Complex<U>> yv = unpack_complex_array<U>(y, args.alloc);
    const Complex<U> v = dotu<U>(xv, yv);
    crd::f64 pair[2] = {static_cast<crd::f64>(v.re), static_cast<crd::f64>(v.im)};
    return binary_result(args.alloc, crd::containers::ConstSpan<crd::f64>{pair, 2});
}

template <typename U>
CommandResult impl_dotc_complex(const CommandArgs& args)
{
    const auto x = args.get_f64_array("x");
    const auto y = args.get_f64_array("y");
    if (x.empty() || x.size() != y.size() || (x.size() % 2) != 0)
    {
        return error_result(args.alloc, "dotc: complex x/y must be even-length flattened {re,im,...}");
    }
    Vector<Complex<U>> xv = unpack_complex_array<U>(x, args.alloc);
    Vector<Complex<U>> yv = unpack_complex_array<U>(y, args.alloc);
    const Complex<U> v = dotc<U>(xv, yv);
    crd::f64 pair[2] = {static_cast<crd::f64>(v.re), static_cast<crd::f64>(v.im)};
    return binary_result(args.alloc, crd::containers::ConstSpan<crd::f64>{pair, 2});
}

template <typename U>
CommandResult impl_nrm2_complex(const CommandArgs& args)
{
    const auto x = args.get_f64_array("x");
    if (x.empty() || (x.size() % 2) != 0)
    {
        return error_result(args.alloc, "nrm2: complex x must be even-length flattened {re,im,...}");
    }
    Vector<Complex<U>> xv = unpack_complex_array<U>(x, args.alloc);
    const U v = nrm2<Complex<U>>(xv);
    return scalar_result(args.alloc, static_cast<crd::f64>(v));
}

template <typename U>
CommandResult impl_scal_complex(const CommandArgs& args)
{
    const auto alpha_flat = args.get_f64_array("alpha");
    Complex<U> alpha{U(1), U(0)};
    if (alpha_flat.size() >= 2)
    {
        alpha = Complex<U>{static_cast<U>(alpha_flat[0]), static_cast<U>(alpha_flat[1])};
    }
    const auto x = args.get_f64_array("x");
    if (x.empty() || (x.size() % 2) != 0)
    {
        return error_result(args.alloc, "scal: complex x must be even-length flattened {re,im,...}");
    }
    Vector<Complex<U>> xv = unpack_complex_array<U>(x, args.alloc);
    scal<Complex<U>>(alpha, xv);
    auto out = pack_complex_array<U>(xv.span(), args.alloc);
    return binary_result(args.alloc, crd::containers::ConstSpan<crd::f64>{out.data(), out.size()});
}

template <typename U>
CommandResult impl_asum_complex(const CommandArgs& args)
{
    const auto x = args.get_f64_array("x");
    if (x.empty() || (x.size() % 2) != 0)
    {
        return error_result(args.alloc, "asum: complex x must be even-length flattened {re,im,...}");
    }
    Vector<Complex<U>> xv = unpack_complex_array<U>(x, args.alloc);
    const U v = asum<Complex<U>>(xv);
    return scalar_result(args.alloc, static_cast<crd::f64>(v));
}

template <typename U>
CommandResult impl_iamax_complex(const CommandArgs& args)
{
    const auto x = args.get_f64_array("x");
    if (x.empty() || (x.size() % 2) != 0)
    {
        return error_result(args.alloc, "iamax: complex x must be even-length flattened {re,im,...}");
    }
    Vector<Complex<U>> xv = unpack_complex_array<U>(x, args.alloc);
    const crd::usize idx = iamax<Complex<U>>(xv);
    return scalar_result(args.alloc, static_cast<crd::f64>(idx));
}

// -----------------------------------------------------------------------
// Helper to build a CommandSchema with the common BLAS L1 param shape.
// -----------------------------------------------------------------------

CommandSchema make_schema(
    crd::memory::IAllocator* alloc,
    const char* name,
    const char* desc,
    OutputKind output_kind)
{
    CommandSchema s{alloc};
    s.name = crd::containers::String{name, alloc};
    s.description = crd::containers::String{desc, alloc};
    s.output.kind = output_kind;
    s.required_caps.bits = Capability::kHesapCompute;
    s.idempotent = true;  // BLAS L1 ops are stateless wrt registry; running twice on same args gives same result.
    return s;
}

void add_param(
    CommandSchema& s,
    crd::memory::IAllocator* alloc,
    const char* name,
    const char* desc,
    ParamKind kind,
    bool required)
{
    ParamSchema p{alloc};
    p.name = crd::containers::String{name, alloc};
    p.description = crd::containers::String{desc, alloc};
    p.kind = kind;
    p.required = required;
    s.params.push_back(std::move(p));
}

// Real-type variants share param shape; complex-type variants share another.
void add_real_axpy_like_params(CommandSchema& s, crd::memory::IAllocator* alloc)
{
    add_param(s, alloc, "alpha", "Scalar multiplier (default 1.0)", ParamKind::F64, /*required=*/false);
    add_param(s, alloc, "x", "Input vector x", ParamKind::F64, /*required=*/true);  // F64Array used at runtime
    add_param(s, alloc, "y", "Input vector y", ParamKind::F64, /*required=*/true);
}

void add_real_dot_like_params(CommandSchema& s, crd::memory::IAllocator* alloc)
{
    add_param(s, alloc, "x", "Input vector x (real)", ParamKind::F64, /*required=*/true);
    add_param(s, alloc, "y", "Input vector y (real)", ParamKind::F64, /*required=*/true);
}

void add_real_unary_params(CommandSchema& s, crd::memory::IAllocator* alloc)
{
    add_param(s, alloc, "x", "Input vector x (real)", ParamKind::F64, /*required=*/true);
}

void add_complex_axpy_like_params(CommandSchema& s, crd::memory::IAllocator* alloc)
{
    add_param(s, alloc, "alpha", "Complex scalar [re, im] (default [1,0])", ParamKind::F64, /*required=*/false);
    add_param(s, alloc, "x", "Complex vector x as flattened [re0,im0,...]", ParamKind::F64, /*required=*/true);
    add_param(s, alloc, "y", "Complex vector y as flattened [re0,im0,...]", ParamKind::F64, /*required=*/true);
}

void add_complex_dot_like_params(CommandSchema& s, crd::memory::IAllocator* alloc)
{
    add_param(s, alloc, "x", "Complex vector x as flattened [re0,im0,...]", ParamKind::F64, /*required=*/true);
    add_param(s, alloc, "y", "Complex vector y as flattened [re0,im0,...]", ParamKind::F64, /*required=*/true);
}

void add_complex_unary_params(CommandSchema& s, crd::memory::IAllocator* alloc)
{
    add_param(s, alloc, "x", "Complex vector x as flattened [re0,im0,...]", ParamKind::F64, /*required=*/true);
}

} // namespace

CRD_HESAP_CLI_REGISTER_MODULE([](CommandRegistry& reg) {
    auto* alloc = crd::memory::default_allocator();

    // ----- Real BLAS L1: f32 ------------------------------------------
    {
        auto s = make_schema(alloc, "hesap.dense.blas1.axpy.f32",
            "y += alpha * x (f32). alpha scalar, x/y arrays.", OutputKind::BinaryBlob);
        add_real_axpy_like_params(s, alloc);
        reg.register_command(std::move(s), &impl_axpy_real<crd::f32>);
    }
    {
        auto s = make_schema(alloc, "hesap.dense.blas1.dot.f32",
            "Real dot product sum x_i * y_i (f32).", OutputKind::Scalar);
        add_real_dot_like_params(s, alloc);
        reg.register_command(std::move(s), &impl_dot_real<crd::f32>);
    }
    {
        auto s = make_schema(alloc, "hesap.dense.blas1.nrm2.f32",
            "Euclidean norm sqrt(sum x_i^2) (f32).", OutputKind::Scalar);
        add_real_unary_params(s, alloc);
        reg.register_command(std::move(s), &impl_nrm2_real<crd::f32>);
    }
    {
        auto s = make_schema(alloc, "hesap.dense.blas1.scal.f32",
            "x *= alpha (f32).", OutputKind::BinaryBlob);
        add_param(s, alloc, "alpha", "Scalar multiplier", ParamKind::F64, true);
        add_param(s, alloc, "x", "Input vector x", ParamKind::F64, true);
        reg.register_command(std::move(s), &impl_scal_real<crd::f32>);
    }
    {
        auto s = make_schema(alloc, "hesap.dense.blas1.copy.f32",
            "Return a copy of x (f32).", OutputKind::BinaryBlob);
        add_param(s, alloc, "src", "Source vector", ParamKind::F64, true);
        reg.register_command(std::move(s), &impl_copy_real<crd::f32>);
    }
    {
        auto s = make_schema(alloc, "hesap.dense.blas1.asum.f32",
            "Sum of absolute values (f32).", OutputKind::Scalar);
        add_real_unary_params(s, alloc);
        reg.register_command(std::move(s), &impl_asum_real<crd::f32>);
    }
    {
        auto s = make_schema(alloc, "hesap.dense.blas1.iamax.f32",
            "Argmax |x_i|; ties broken by first index (f32).", OutputKind::Scalar);
        add_real_unary_params(s, alloc);
        reg.register_command(std::move(s), &impl_iamax_real<crd::f32>);
    }

    // ----- Real BLAS L1: f64 ------------------------------------------
    {
        auto s = make_schema(alloc, "hesap.dense.blas1.axpy.f64",
            "y += alpha * x (f64).", OutputKind::BinaryBlob);
        add_real_axpy_like_params(s, alloc);
        reg.register_command(std::move(s), &impl_axpy_real<crd::f64>);
    }
    {
        auto s = make_schema(alloc, "hesap.dense.blas1.dot.f64",
            "Real dot product sum x_i * y_i (f64).", OutputKind::Scalar);
        add_real_dot_like_params(s, alloc);
        reg.register_command(std::move(s), &impl_dot_real<crd::f64>);
    }
    {
        auto s = make_schema(alloc, "hesap.dense.blas1.nrm2.f64",
            "Euclidean norm sqrt(sum x_i^2) (f64).", OutputKind::Scalar);
        add_real_unary_params(s, alloc);
        reg.register_command(std::move(s), &impl_nrm2_real<crd::f64>);
    }
    {
        auto s = make_schema(alloc, "hesap.dense.blas1.scal.f64",
            "x *= alpha (f64).", OutputKind::BinaryBlob);
        add_param(s, alloc, "alpha", "Scalar multiplier", ParamKind::F64, true);
        add_param(s, alloc, "x", "Input vector x", ParamKind::F64, true);
        reg.register_command(std::move(s), &impl_scal_real<crd::f64>);
    }
    {
        auto s = make_schema(alloc, "hesap.dense.blas1.copy.f64",
            "Return a copy of x (f64).", OutputKind::BinaryBlob);
        add_param(s, alloc, "src", "Source vector", ParamKind::F64, true);
        reg.register_command(std::move(s), &impl_copy_real<crd::f64>);
    }
    {
        auto s = make_schema(alloc, "hesap.dense.blas1.asum.f64",
            "Sum of absolute values (f64).", OutputKind::Scalar);
        add_real_unary_params(s, alloc);
        reg.register_command(std::move(s), &impl_asum_real<crd::f64>);
    }
    {
        auto s = make_schema(alloc, "hesap.dense.blas1.iamax.f64",
            "Argmax |x_i|; ties broken by first index (f64).", OutputKind::Scalar);
        add_real_unary_params(s, alloc);
        reg.register_command(std::move(s), &impl_iamax_real<crd::f64>);
    }

    // ----- Complex BLAS L1: c32 (Complex<f32>) ------------------------
    {
        auto s = make_schema(alloc, "hesap.dense.blas1.axpy.c32",
            "y += alpha * x (Complex<f32>).", OutputKind::BinaryBlob);
        add_complex_axpy_like_params(s, alloc);
        reg.register_command(std::move(s), &impl_axpy_complex<crd::f32>);
    }
    {
        auto s = make_schema(alloc, "hesap.dense.blas1.dotu.c32",
            "Unconjugated dot sum x_i * y_i (Complex<f32>).", OutputKind::BinaryBlob);
        add_complex_dot_like_params(s, alloc);
        reg.register_command(std::move(s), &impl_dotu_complex<crd::f32>);
    }
    {
        auto s = make_schema(alloc, "hesap.dense.blas1.dotc.c32",
            "Hermitian dot sum conj(x_i) * y_i (Complex<f32>).", OutputKind::BinaryBlob);
        add_complex_dot_like_params(s, alloc);
        reg.register_command(std::move(s), &impl_dotc_complex<crd::f32>);
    }
    {
        auto s = make_schema(alloc, "hesap.dense.blas1.nrm2.c32",
            "Euclidean norm sqrt(sum |x_i|^2) (Complex<f32>).", OutputKind::Scalar);
        add_complex_unary_params(s, alloc);
        reg.register_command(std::move(s), &impl_nrm2_complex<crd::f32>);
    }
    {
        auto s = make_schema(alloc, "hesap.dense.blas1.scal.c32",
            "x *= alpha (Complex<f32>).", OutputKind::BinaryBlob);
        add_param(s, alloc, "alpha", "Complex scalar [re, im]", ParamKind::F64, true);
        add_param(s, alloc, "x", "Complex vector x flattened", ParamKind::F64, true);
        reg.register_command(std::move(s), &impl_scal_complex<crd::f32>);
    }
    {
        auto s = make_schema(alloc, "hesap.dense.blas1.asum.c32",
            "Sum of |re_i| + |im_i| (Complex<f32>).", OutputKind::Scalar);
        add_complex_unary_params(s, alloc);
        reg.register_command(std::move(s), &impl_asum_complex<crd::f32>);
    }
    {
        auto s = make_schema(alloc, "hesap.dense.blas1.iamax.c32",
            "Argmax (|re_i|+|im_i|); ties = first index (Complex<f32>).", OutputKind::Scalar);
        add_complex_unary_params(s, alloc);
        reg.register_command(std::move(s), &impl_iamax_complex<crd::f32>);
    }

    // ----- Complex BLAS L1: c64 (Complex<f64>) ------------------------
    {
        auto s = make_schema(alloc, "hesap.dense.blas1.axpy.c64",
            "y += alpha * x (Complex<f64>).", OutputKind::BinaryBlob);
        add_complex_axpy_like_params(s, alloc);
        reg.register_command(std::move(s), &impl_axpy_complex<crd::f64>);
    }
    {
        auto s = make_schema(alloc, "hesap.dense.blas1.dotu.c64",
            "Unconjugated dot sum x_i * y_i (Complex<f64>).", OutputKind::BinaryBlob);
        add_complex_dot_like_params(s, alloc);
        reg.register_command(std::move(s), &impl_dotu_complex<crd::f64>);
    }
    {
        auto s = make_schema(alloc, "hesap.dense.blas1.dotc.c64",
            "Hermitian dot sum conj(x_i) * y_i (Complex<f64>).", OutputKind::BinaryBlob);
        add_complex_dot_like_params(s, alloc);
        reg.register_command(std::move(s), &impl_dotc_complex<crd::f64>);
    }
    {
        auto s = make_schema(alloc, "hesap.dense.blas1.nrm2.c64",
            "Euclidean norm sqrt(sum |x_i|^2) (Complex<f64>).", OutputKind::Scalar);
        add_complex_unary_params(s, alloc);
        reg.register_command(std::move(s), &impl_nrm2_complex<crd::f64>);
    }
    {
        auto s = make_schema(alloc, "hesap.dense.blas1.scal.c64",
            "x *= alpha (Complex<f64>).", OutputKind::BinaryBlob);
        add_param(s, alloc, "alpha", "Complex scalar [re, im]", ParamKind::F64, true);
        add_param(s, alloc, "x", "Complex vector x flattened", ParamKind::F64, true);
        reg.register_command(std::move(s), &impl_scal_complex<crd::f64>);
    }
    {
        auto s = make_schema(alloc, "hesap.dense.blas1.asum.c64",
            "Sum of |re_i| + |im_i| (Complex<f64>).", OutputKind::Scalar);
        add_complex_unary_params(s, alloc);
        reg.register_command(std::move(s), &impl_asum_complex<crd::f64>);
    }
    {
        auto s = make_schema(alloc, "hesap.dense.blas1.iamax.c64",
            "Argmax (|re_i|+|im_i|); ties = first index (Complex<f64>).", OutputKind::Scalar);
        add_complex_unary_params(s, alloc);
        reg.register_command(std::move(s), &impl_iamax_complex<crd::f64>);
    }
})
