// v14-m NN inference bench: ours f32 + ours Q8_0 + ours i8 on the frozen tiny models
// (MLP batch 16 / CNN batch 8) + a scaled batch (4096). Pinned single-thread
// protocol (taskset -c 4, no jobs pool); best-of-50 whole-batch inferences.
// Peers (torch-CPU / onnxruntime-CPU, matched 1T) ride scripts/v14m_nn_peers.py.
// The nets run the hesap-tensor kernel level the process selected (CRD_CPU_LEVEL forces one, scripts/run_bench_nn.sh
// runs each). The input is the corpus's frozen rows (<model>_x.npy), repeated to fill a batch, so every timed output
// row has a reference: the f32 net at the corpus batch, itself within 1e-6 of torch's frozen outputs (<model>_y.npy).
// Each tier's rows must stay within its test's bound of that reference (f32 1e-6, Q8_0 and i8 2e-2: test_nn.cpp), or
// the run exits 1. The act rows time nn_gelu, nn_tanh and nn_sigmoid over 4096 x 128 values spread over [-12, 12]
// (the frozen MLP's hidden width at the scaled batch), best-of-50; each output must carry the bits of the scalar
// formula in general code (nn.hpp: f64 erf-form gelu and sigmoid, f32 tanh), or the run exits 1.
#include <crd/core/cpu_features.hpp>
#include <crd/core/environment.hpp>
#include <crd/hesap/tensor/io.hpp>
#include <crd/hesap/tensor/nn.hpp>
#include <crd/hesap/tensor/tensor_dispatch.hpp>
#include <crd/math/cmath.hpp>
#include <crd/math/deterministic.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using crd::hesap::tensor::build_cnn_from_safetensors;
using crd::hesap::tensor::build_mlp_from_safetensors;
using crd::hesap::tensor::NnQuantTier;
using crd::hesap::tensor::NnSequential;
using crd::hesap::tensor::SafetensorsFile;
using crd::hesap::tensor::Tensor;
using crd::hesap::tensor::TensorStatus;

namespace
{

// CRD_NN_CORPUS when it is set, else the committed corpus.
const char* corpus_root()
{
    static const crd::EnvironmentValue<512> kRoot{"CRD_NN_CORPUS"};
    return kRoot.c_str() != nullptr ? kRoot.c_str() : "tests/numerics/hesap-tensor/nn_corpus";
}

// <name><suffix> of the corpus, or a fatal exit.
void read_corpus(crd::memory::IAllocator* alloc, const char* name, const char* suffix,
                 crd::containers::Array<crd::u8>& out)
{
    crd::containers::String p(alloc);
    p.append(corpus_root());
    p.append("/");
    p.append(name);
    p.append(suffix);
    if (crd::hesap::tensor::io_read_file(crd::containers::StringView{p.data(), p.size()}, out) != TensorStatus::Ok)
    {
        std::printf("FATAL: missing %s\n", p.c_str());
        std::exit(1);
    }
}

// The corpus's frozen tensor <name><suffix> (.npy f32), or a fatal exit.
void read_corpus_npy(crd::memory::IAllocator* alloc, const char* name, const char* suffix, Tensor<crd::f32>& out)
{
    crd::containers::Array<crd::u8> bytes(alloc);
    read_corpus(alloc, name, suffix, bytes);
    if (crd::hesap::tensor::npy_read<crd::f32>(alloc, crd::containers::as_const_span(bytes), out) != TensorStatus::Ok)
    {
        std::printf("FATAL: bad npy %s%s\n", name, suffix);
        std::exit(1);
    }
}

// Repeated calls use the same lvalue; never consume an rvalue callable on each iteration.
// NOLINTNEXTLINE(cppcoreguidelines-missing-std-forward)
template <typename F> double best_of_ns(F&& f, int reps, int warm)
{
    for (int i = 0; i < warm; ++i)
    {
        f();
    }
    double best = 1e300;
    for (int r = 0; r < reps; ++r)
    {
        const auto t0 = std::chrono::steady_clock::now();
        f();
        const auto t1 = std::chrono::steady_clock::now();
        const double ns = std::chrono::duration<double, std::nano>(t1 - t0).count();
        best = ns < best ? ns : best;
    }
    return best;
}

// The largest |y[r] - ref[r mod ref rows]| over y's rows (row-major, the same width), NaN when any difference is.
crd::f32 max_row_diff(const Tensor<crd::f32>& y, const Tensor<crd::f32>& ref)
{
    crd::f32 worst = 0.0F;
    for (crd::u64 i = 0; i < y.size(); ++i)
    {
        const crd::f32 d = std::fabs(y.data()[i] - ref.data()[i % ref.size()]);
        worst = d > worst || std::isnan(d) ? d : worst;
    }
    return worst;
}

bool bench_model(crd::memory::IAllocator* alloc, const char* name, crd::u64 big_batch)
{
    crd::containers::Array<crd::u8> bytes(alloc);
    read_corpus(alloc, name, ".safetensors", bytes);
    SafetensorsFile st(alloc);
    if (st.parse(crd::containers::as_const_span(bytes)) != TensorStatus::Ok)
    {
        std::printf("FATAL: bad safetensors %s\n", name);
        std::exit(1);
    }
    Tensor<crd::f32> corpus_x(alloc);
    Tensor<crd::f32> torch_y(alloc);
    read_corpus_npy(alloc, name, "_x.npy", corpus_x);
    read_corpus_npy(alloc, name, "_y.npy", torch_y);
    const bool is_mlp = std::strcmp(name, "mlp") == 0;
    const crd::u64 corpus_batch = corpus_x.shape(0);
    const crd::u32 in_rank = corpus_x.rank();
    Tensor<crd::f32> reference(alloc, torch_y.shape()); // the f32 net's outputs at the corpus batch
    bool ok = true;
    for (const crd::u64 batch : {corpus_batch, big_batch})
    {
        crd::u64 shp[4];
        shp[0] = batch;
        for (crd::u32 d = 1; d < in_rank; ++d)
        {
            shp[d] = corpus_x.shape(d);
        }
        Tensor<crd::f32> x(alloc, {shp, in_rank});
        for (crd::u64 i = 0; i < x.size(); ++i)
        {
            x.data()[i] = corpus_x.data()[i % corpus_x.size()];
        }
        const struct
        {
            NnQuantTier tier;
            const char* label;
            crd::f32 bound;
        } tiers[] = {
            {NnQuantTier::F32, "f32", 1e-6F},
            {NnQuantTier::Q8Block32, "q8", 2e-2F},
            {NnQuantTier::I8PerTensor, "i8", 2e-2F},
        };
        for (const auto& tr : tiers)
        {
            NnSequential net(alloc);
            const TensorStatus bst = is_mlp ? build_mlp_from_safetensors(alloc, st, batch, tr.tier, net)
                                            : build_cnn_from_safetensors(alloc, st, batch, tr.tier, net);
            if (bst != TensorStatus::Ok)
            {
                std::printf("FATAL: build %s failed\n", name);
                std::exit(1);
            }
            const crd::u64 wsb[1] = {net.workspace_bytes()};
            Tensor<crd::u8> ws(alloc, {wsb, 1});
            const crd::u64 yshp[2] = {batch, torch_y.shape(1)};
            Tensor<crd::f32> y(alloc, {yshp, 2});
            const crd::containers::Span<crd::u8> wss{ws.data(), static_cast<crd::usize>(ws.size())};
            const auto run = [&]()
            {
                if (net.infer(x.view(), y.view(), wss, 1U) != TensorStatus::Ok)
                {
                    std::printf("FATAL: infer failed\n");
                    std::exit(1);
                }
            };
            const double ns = best_of_ns(run, 50, 10);
            if (tr.tier == NnQuantTier::F32 && batch == corpus_batch)
            {
                std::memcpy(reference.data(), y.data(), y.size() * sizeof(crd::f32));
                const crd::f32 torch_diff = max_row_diff(y, torch_y);
                if (!(torch_diff <= 1e-6F))
                {
                    std::printf("!MISMATCH! %s f32: %.3e from torch's frozen outputs (bound 1e-6)\n", name,
                                static_cast<double>(torch_diff));
                    ok = false;
                }
            }
            const crd::f32 diff = max_row_diff(y, reference);
            const bool within = diff <= tr.bound;
            ok = ok && within;
            std::printf("%-4s %-4s batch %5llu : %12.0f ns/batch  %9.1f ns/sample  max diff %.2e%s\n", name, tr.label,
                        static_cast<unsigned long long>(batch), ns, ns / static_cast<double>(batch),
                        static_cast<double>(diff), within ? "" : "  !MISMATCH!");
        }
    }
    return ok;
}

// The activation spans against their scalar formulas, bit for bit.
bool bench_activations(crd::memory::IAllocator* alloc)
{
    constexpr crd::u64 n = 4096U * 128U;
    constexpr crd::u64 period = 4093U;
    const crd::u64 shp[1] = {n};
    Tensor<crd::f32> x(alloc, {shp, 1});
    Tensor<crd::f32> y(alloc, {shp, 1});
    for (crd::u64 i = 0; i < n; ++i)
    {
        x.data()[i] = -12.0F + 24.0F * static_cast<crd::f32>(i % period) / static_cast<crd::f32>(period - 1U);
    }
    const auto gelu = [](crd::f32 v)
    {
        const crd::f64 vd = static_cast<crd::f64>(v);
        return static_cast<crd::f32>(0.5 * vd * (1.0 + crd::math::deterministic::erf(vd * 0.70710678118654752440)));
    };
    const auto tanh = [](crd::f32 v)
    {
        return crd::math::tanh(v);
    };
    const auto sigmoid = [](crd::f32 v)
    {
        return static_cast<crd::f32>(1.0 / (1.0 + crd::math::exp(-static_cast<crd::f64>(v))));
    };
    const crd::containers::ConstSpan<crd::f32> xs{x.data(), static_cast<crd::usize>(n)};
    const crd::containers::Span<crd::f32> ys{y.data(), static_cast<crd::usize>(n)};
    bool ok = true;
    const auto row = [&](const char* label, auto&& span, auto&& one)
    {
        const double ns = best_of_ns([&]() { span(xs, ys); }, 50, 5);
        crd::u64 mismatches = 0;
        for (crd::u64 i = 0; i < n; ++i)
        {
            mismatches += std::bit_cast<crd::u32>(y.data()[i]) != std::bit_cast<crd::u32>(one(x.data()[i])) ? 1U : 0U;
        }
        ok = ok && mismatches == 0U;
        std::printf("act  %-7s n %7llu : %12.0f ns/span  %9.3f ns/element  %llu bit mismatches%s\n", label,
                    static_cast<unsigned long long>(n), ns, ns / static_cast<double>(n),
                    static_cast<unsigned long long>(mismatches), mismatches == 0U ? "" : "  !MISMATCH!");
    };
    row("gelu", crd::hesap::tensor::nn_gelu, gelu);
    row("tanh", crd::hesap::tensor::nn_tanh, tanh);
    row("sigmoid", crd::hesap::tensor::nn_sigmoid, sigmoid);
    return ok;
}

} // namespace

int main()
{
    crd::memory::TlsfAllocator alloc(1U << 28);
    std::printf("v14-m NN inference bench (ours, 1T, best-of-50), hesap-tensor kernel level %s (selected %s)\n",
                crd::cpu_level_name(crd::hesap::tensor::tensor_kernels().level),
                crd::cpu_level_name(crd::cpu_selection().selected));
    const bool mlp = bench_model(&alloc, "mlp", 4096U);
    const bool cnn = bench_model(&alloc, "cnn", 4096U);
    const bool act = bench_activations(&alloc);
    return mlp && cnn && act ? 0 : 1;
}
