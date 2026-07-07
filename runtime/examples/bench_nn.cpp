// v14-m NN inference bench: ours f32 + ours Q8_0 on the frozen tiny models
// (MLP batch 16 / CNN batch 8) + a scaled batch (4096). Pinned single-thread
// protocol (taskset -c 4, no jobs pool); best-of-50 whole-batch inferences.
// Peers (torch-CPU / onnxruntime-CPU, matched 1T) ride scripts/v14m_nn_peers.py.
#include <crd/hesap/stats/philox.hpp>
#include <crd/hesap/tensor/io.hpp>
#include <crd/hesap/tensor/nn.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using crd::hesap::tensor::build_cnn_from_safetensors;
using crd::hesap::tensor::build_mlp_from_safetensors;
using crd::hesap::tensor::NnSequential;
using crd::hesap::tensor::SafetensorsFile;
using crd::hesap::tensor::Tensor;
using crd::hesap::tensor::TensorStatus;

namespace
{

const char* corpus_root()
{
    const char* root = std::getenv("CRD_NN_CORPUS");
    return root != nullptr ? root : "tests/hesap-tensor/nn_corpus";
}

bool read_file(crd::memory::IAllocator* alloc, const char* file, crd::containers::Array<crd::u8>& out)
{
    crd::containers::String p(alloc);
    p.append(corpus_root());
    p.append("/");
    p.append(file);
    return crd::hesap::tensor::io_read_file(crd::containers::StringView{p.data(), p.size()}, out) ==
           TensorStatus::Ok;
}

void fill_rand(crd::f32* p, crd::u64 n, crd::u64 seed)
{
    crd::hesap::stats::PhiloxRng rng(seed, 0U);
    for (crd::u64 i = 0; i < n; ++i)
    {
        p[i] = static_cast<crd::f32>(2.0 * rng.next_f64() - 1.0);
    }
}

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

void bench_model(crd::memory::IAllocator* alloc, const char* name, const crd::u64* in_shape,
                 crd::u32 in_rank, crd::u64 tiny_batch, crd::u64 big_batch)
{
    crd::containers::Array<crd::u8> bytes(alloc);
    crd::containers::String sf(alloc);
    sf.append(name);
    sf.append(".safetensors");
    if (!read_file(alloc, sf.c_str(), bytes))
    {
        std::printf("FATAL: missing %s\n", sf.c_str());
        std::exit(1);
    }
    SafetensorsFile st(alloc);
    if (st.parse(crd::containers::as_const_span(bytes)) != TensorStatus::Ok)
    {
        std::printf("FATAL: bad safetensors %s\n", name);
        std::exit(1);
    }
    const bool is_mlp = std::strcmp(name, "mlp") == 0;
    for (const crd::u64 batch : {tiny_batch, big_batch})
    {
        crd::u64 shp[4];
        shp[0] = batch;
        crd::u64 per_sample = 1;
        for (crd::u32 d = 1; d < in_rank; ++d)
        {
            shp[d] = in_shape[d];
            per_sample *= in_shape[d];
        }
        Tensor<crd::f32> x(alloc, {shp, in_rank});
        fill_rand(x.data(), x.size(), 11U);
        using crd::hesap::tensor::NnQuantTier;
        const struct
        {
            NnQuantTier tier;
            const char* label;
        } tiers[] = {
            {NnQuantTier::F32, "f32"},
            {NnQuantTier::Q8Block32, "q8"},
            {NnQuantTier::I8PerTensor, "i8"},
        };
        for (const auto& tr : tiers)
        {
            NnSequential net(alloc);
            const TensorStatus bst = is_mlp
                                         ? build_mlp_from_safetensors(alloc, st, batch, tr.tier, net)
                                         : build_cnn_from_safetensors(alloc, st, batch, tr.tier, net);
            if (bst != TensorStatus::Ok)
            {
                std::printf("FATAL: build %s failed\n", name);
                std::exit(1);
            }
            const crd::u64 wsb[1] = {net.workspace_bytes()};
            Tensor<crd::u8> ws(alloc, {wsb, 1});
            const crd::u64 yshp[2] = {batch, 10U};
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
            std::printf("%-4s %-4s batch %5llu : %12.0f ns/batch  %9.1f ns/sample\n", name, tr.label,
                        static_cast<unsigned long long>(batch), ns, ns / static_cast<double>(batch));
            (void)per_sample;
        }
    }
}

} // namespace

int main()
{
    crd::memory::TlsfAllocator alloc(1U << 28);
    const crd::u64 mlp_shape[2] = {0U, 64U};
    const crd::u64 cnn_shape[4] = {0U, 1U, 16U, 16U};
    std::printf("v14-m NN inference bench (ours, 1T, best-of-50)\n");
    bench_model(&alloc, "mlp", mlp_shape, 2U, 16U, 4096U);
    bench_model(&alloc, "cnn", cnn_shape, 4U, 8U, 4096U);
    return 0;
}
