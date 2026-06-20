// v10-z — CLI registration for the FFT cluster (hesap.fft.*). Signals are DATA (interleaved re/im f64), so the
// agent reaches the transforms directly via an F64Array argument (the v7-z data-vs-callable split):
//
//   hesap.fft.forward.f64 : forward/inverse complex FFT of ARBITRARY size (Bluestein over the pow-2 engine).
//     data    : interleaved [re0,im0,re1,im1,...] of length 2n (n = signal length, any n ≥ 1).
//     inverse : 0 = forward DFT (default) · 1 = inverse IDFT (1/n normalised, round-trip exact).
//     Out blob = the transformed signal, interleaved re/im, length 2n.
//
//   hesap.fft.sparse.f64 : Sparse FFT (HIKP) — recover the k dominant frequencies of a (power-of-two) signal.
//     data : interleaved re/im, length 2n (n a power of two).  k : sparsity (1 ≤ k ≤ n).
//     Out blob = [count, (freq, re, im) × count]  (freq as f64).
// Anchor: register_fft_cli_anchor().

#include <crd/hesap/cli/arg_value.hpp>
#include <crd/hesap/cli/command_registry.hpp>
#include <crd/hesap/fft/bluestein.hpp>
#include <crd/hesap/fft/sparse_fft.hpp>

#include <utility>

namespace
{
using namespace crd::hesap;
using namespace crd::hesap::cli;
namespace fft = crd::hesap::fft;
using Complexd = crd::hesap::Complex<crd::f64>;

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

CommandSchema make_forward_schema(crd::memory::IAllocator* alloc)
{
    CommandSchema s{alloc};
    s.name = crd::containers::String{"hesap.fft.forward.f64", alloc};
    s.description = crd::containers::String{
        "Forward/inverse complex FFT of arbitrary size (Bluestein). data = interleaved re/im (length 2n).", alloc};
    add_param(s, alloc, "data", "interleaved [re,im,...] signal as an f64 vector, length 2n", ParamKind::VectorId, true);
    add_param(s, alloc, "inverse", "0 = forward DFT (default), 1 = inverse IDFT (1/n)", ParamKind::I64, false);
    return s;
}

CommandResult impl_forward(const CommandArgs& args)
{
    const auto data = args.get_f64_array("data");
    if (data.size() < 2 || (data.size() & 1U) != 0)
    {
        return error_result(args.alloc, "fft.forward: data must be interleaved re/im (even length >= 2)");
    }
    const crd::usize n = data.size() / 2;
    const bool inv = args.get_i64("inverse").value_or(0) != 0;

    crd::containers::Array<Complexd> x(args.alloc);
    x.resize(n);
    for (crd::usize i = 0; i < n; ++i)
    {
        x[i] = Complexd{data[2 * i], data[2 * i + 1]};
    }
    const fft::BluesteinPlan<crd::f64> plan(args.alloc, n);
    plan.execute(crd::containers::Span<Complexd>(x.data(), n),
                 inv ? fft::FftDirection::Inverse : fft::FftDirection::Forward);

    crd::containers::Array<crd::f64> out(args.alloc);
    out.resize(2 * n);
    for (crd::usize i = 0; i < n; ++i)
    {
        out[2 * i] = x[i].re;
        out[2 * i + 1] = x[i].im;
    }
    return blob_f64_result(args.alloc, crd::containers::ConstSpan<crd::f64>(out.data(), 2 * n));
}

CommandSchema make_sparse_schema(crd::memory::IAllocator* alloc)
{
    CommandSchema s{alloc};
    s.name = crd::containers::String{"hesap.fft.sparse.f64", alloc};
    s.description = crd::containers::String{
        "Sparse FFT (HIKP): recover the k dominant frequencies of a power-of-two signal. Out = [count,(f,re,im)*].",
        alloc};
    add_param(s, alloc, "data", "interleaved [re,im,...] signal as an f64 vector, length 2n (n a power of two)",
              ParamKind::VectorId, true);
    add_param(s, alloc, "k", "sparsity (1 <= k <= n)", ParamKind::I64, true);
    return s;
}

CommandResult impl_sparse(const CommandArgs& args)
{
    const auto data = args.get_f64_array("data");
    const auto kopt = args.get_i64("k");
    if (data.size() < 2 || (data.size() & 1U) != 0 || !kopt)
    {
        return error_result(args.alloc, "fft.sparse: data (interleaved re/im, len 2n) and k are required");
    }
    const crd::usize n = data.size() / 2;
    if ((n & (n - 1)) != 0)
    {
        return error_result(args.alloc, "fft.sparse: n must be a power of two");
    }
    if (*kopt < 1 || static_cast<crd::usize>(*kopt) > n)
    {
        return error_result(args.alloc, "fft.sparse: k must satisfy 1 <= k <= n");
    }
    const crd::usize k = static_cast<crd::usize>(*kopt);

    crd::containers::Array<Complexd> x(args.alloc);
    x.resize(n);
    for (crd::usize i = 0; i < n; ++i)
    {
        x[i] = Complexd{data[2 * i], data[2 * i + 1]};
    }
    const fft::SparseFftPlan<crd::f64> plan(args.alloc, n, k, 20);
    crd::containers::Array<crd::usize> rf(args.alloc);
    crd::containers::Array<Complexd> rc(args.alloc);
    rf.resize(k);
    rc.resize(k);
    const crd::usize got = plan.recover(crd::containers::ConstSpan<Complexd>(x.data(), n),
                                        crd::containers::Span<crd::usize>(rf.data(), k),
                                        crd::containers::Span<Complexd>(rc.data(), k));

    crd::containers::Array<crd::f64> out(args.alloc);
    out.push_back(static_cast<crd::f64>(got));
    for (crd::usize j = 0; j < got; ++j)
    {
        out.push_back(static_cast<crd::f64>(rf[j]));
        out.push_back(rc[j].re);
        out.push_back(rc[j].im);
    }
    return blob_f64_result(args.alloc, crd::containers::ConstSpan<crd::f64>(out.data(), out.size()));
}
} // namespace

namespace crd::hesap::fft
{
void register_fft_cli_anchor() noexcept {}
} // namespace crd::hesap::fft

// NOLINTNEXTLINE(bugprone-throwing-static-initialization)
CRD_HESAP_CLI_REGISTER_MODULE(
    [](CommandRegistry& reg)
    {
        auto* alloc = crd::memory::default_allocator();
        reg.register_command(make_forward_schema(alloc), &impl_forward);
        reg.register_command(make_sparse_schema(alloc), &impl_sparse);
    });
