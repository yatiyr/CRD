// v14-m NN inference pack gates.
// - f32 forward parity vs the frozen torch outputs (<= 1e-6, both models, all rows)
// - per-op unit gates: conv2d/linear BIT-EXACT vs the k-ordered fma-chain reference,
//   softmax row sums + exact-uniform adversary, layernorm statistics, pooling
// - Q8_0: quantize-on-load byte-exact vs the frozen .q8/.q8s refs; both weight-load
//   paths bit-exact against each other; run-twice determinism; bounded error vs f32
// - the {1,2,4,8,16} worker moat (bit-identity) and the allocation-free infer gate
#include <crd/hesap/stats/philox.hpp>
#include <crd/hesap/tensor/io.hpp>
#include <crd/hesap/tensor/nn.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using crd::hesap::tensor::BlockQ8_0;
using crd::hesap::tensor::build_cnn_from_safetensors;
using crd::hesap::tensor::build_mlp_from_safetensors;
using crd::hesap::tensor::kQuantBlock;
using crd::hesap::tensor::NnQuantTier;
using crd::hesap::tensor::NnSequential;
using crd::hesap::tensor::SafetensorsFile;
using crd::hesap::tensor::Tensor;
using crd::hesap::tensor::TensorStatus;
using crd::hesap::tensor::TensorView;

namespace
{

constexpr crd::usize kPool = 1U << 26;

crd::containers::String corpus_path(crd::memory::IAllocator* alloc, const char* file)
{
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996) // getenv — corpus-path dev knob only (the dense_lu_kernels precedent)
#endif
    const char* root = std::getenv("CRD_NN_CORPUS");
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
    if (root == nullptr)
    {
        root = "tests/hesap-tensor/nn_corpus";
    }
    crd::containers::String s(alloc);
    s.append(root);
    s.append("/");
    s.append(file);
    return s;
}

TensorStatus read_corpus_file(crd::memory::IAllocator* alloc, const char* file,
                              crd::containers::Array<crd::u8>& out)
{
    const crd::containers::String p = corpus_path(alloc, file);
    return crd::hesap::tensor::io_read_file(crd::containers::StringView{p.data(), p.size()}, out);
}

// counting allocator: forwards to a parent, counts every allocation-side call
// (the allocation-free infer gate — the test_tt.cpp pattern).
class CountingAllocator final : public crd::memory::IAllocator
{
public:
    explicit CountingAllocator(crd::memory::IAllocator* parent) noexcept : m_parent(parent)
    {
        m_name = "CountingAllocator";
    }
    void* allocate(crd::usize size, crd::usize alignment) override
    {
        ++m_allocs;
        return m_parent->allocate(size, alignment);
    }
    void deallocate(void* p) noexcept override { m_parent->deallocate(p); }
    bool owns(const void* p) const noexcept override { return m_parent->owns(p); }
    void* reallocate(void* p, crd::usize old_size, crd::usize new_size, crd::usize alignment) override
    {
        ++m_allocs;
        return m_parent->reallocate(p, old_size, new_size, alignment);
    }
    [[nodiscard]] void* try_allocate(crd::usize size, crd::usize alignment) override
    {
        ++m_allocs;
        return m_parent->try_allocate(size, alignment);
    }
    [[nodiscard]] crd::u64 alloc_calls() const noexcept { return m_allocs; }

private:
    crd::memory::IAllocator* m_parent;
    crd::u64 m_allocs = 0;
};

void fill_rand(crd::f32* p, crd::u64 n, crd::u64 seed, crd::u64 stream)
{
    crd::hesap::stats::PhiloxRng rng(seed, stream);
    for (crd::u64 i = 0; i < n; ++i)
    {
        p[i] = static_cast<crd::f32>(2.0 * rng.next_f64() - 1.0);
    }
}

struct LoadedModel
{
    explicit LoadedModel(crd::memory::IAllocator* alloc)
        : bytes(alloc), st(alloc), x(alloc), y(alloc)
    {
    }
    crd::containers::Array<crd::u8> bytes;
    SafetensorsFile st;
    Tensor<crd::f32> x;
    Tensor<crd::f32> y;
};

// load <name>.safetensors + <name>_x.npy + <name>_y.npy
bool load_model(crd::memory::IAllocator* alloc, const char* name, LoadedModel& m)
{
    crd::containers::String sf(alloc);
    sf.append(name);
    sf.append(".safetensors");
    if (read_corpus_file(alloc, sf.c_str(), m.bytes) != TensorStatus::Ok)
    {
        return false;
    }
    if (m.st.parse(crd::containers::as_const_span(m.bytes)) != TensorStatus::Ok)
    {
        return false;
    }
    crd::containers::String xf(alloc);
    xf.append(name);
    xf.append("_x.npy");
    crd::containers::Array<crd::u8> xb(alloc);
    if (read_corpus_file(alloc, xf.c_str(), xb) != TensorStatus::Ok ||
        crd::hesap::tensor::npy_read<crd::f32>(alloc, crd::containers::as_const_span(xb), m.x) !=
            TensorStatus::Ok)
    {
        return false;
    }
    crd::containers::String yf(alloc);
    yf.append(name);
    yf.append("_y.npy");
    crd::containers::Array<crd::u8> yb(alloc);
    return read_corpus_file(alloc, yf.c_str(), yb) == TensorStatus::Ok &&
           crd::hesap::tensor::npy_read<crd::f32>(alloc, crd::containers::as_const_span(yb), m.y) ==
               TensorStatus::Ok;
}

crd::f32 run_and_maxdiff(const NnSequential& net, const Tensor<crd::f32>& x, const Tensor<crd::f32>& yref,
                         Tensor<crd::f32>& yout, Tensor<crd::u8>& ws, crd::u32 nw = 1U)
{
    REQUIRE(net.infer(x.view(), yout.view(), {ws.data(), static_cast<crd::usize>(ws.size())}, nw) ==
            TensorStatus::Ok);
    crd::f32 maxd = 0.0F;
    for (crd::u64 i = 0; i < yref.size(); ++i)
    {
        const crd::f32 dd = std::fabs(yout.data()[i] - yref.data()[i]);
        maxd = dd > maxd ? dd : maxd;
    }
    return maxd;
}

} // namespace

TEST_CASE("nn: mlp f32 forward matches the frozen torch outputs within 1e-6", "[v14m][nn]")
{
    crd::memory::TlsfAllocator alloc(kPool);
    LoadedModel m(&alloc);
    REQUIRE(load_model(&alloc, "mlp", m));
    NnSequential net(&alloc);
    REQUIRE(build_mlp_from_safetensors(&alloc, m.st, m.x.shape(0), false, net) == TensorStatus::Ok);
    const crd::u64 wsb[1] = {net.workspace_bytes()};
    Tensor<crd::u8> ws(&alloc, {wsb, 1});
    Tensor<crd::f32> y(&alloc, m.y.shape());
    const crd::f32 maxd = run_and_maxdiff(net, m.x, m.y, y, ws);
    std::printf("[v14m] mlp f32 parity: max abs diff vs torch = %.3e (16 rows x 10)\n",
                static_cast<double>(maxd));
    REQUIRE(maxd <= 1e-6F);
}

TEST_CASE("nn: cnn f32 forward matches the frozen torch outputs within 1e-6", "[v14m][nn]")
{
    crd::memory::TlsfAllocator alloc(kPool);
    LoadedModel m(&alloc);
    REQUIRE(load_model(&alloc, "cnn", m));
    NnSequential net(&alloc);
    REQUIRE(build_cnn_from_safetensors(&alloc, m.st, m.x.shape(0), false, net) == TensorStatus::Ok);
    const crd::u64 wsb[1] = {net.workspace_bytes()};
    Tensor<crd::u8> ws(&alloc, {wsb, 1});
    Tensor<crd::f32> y(&alloc, m.y.shape());
    const crd::f32 maxd = run_and_maxdiff(net, m.x, m.y, y, ws);
    std::printf("[v14m] cnn f32 parity: max abs diff vs torch = %.3e (8 rows x 10)\n",
                static_cast<double>(maxd));
    REQUIRE(maxd <= 1e-6F);
}

TEST_CASE("nn: conv2d is bit-exact vs the k-ordered fma-chain direct convolution", "[v14m][nn]")
{
    crd::memory::TlsfAllocator alloc(kPool);
    // odd spatial sizes + tails, multiple pad/stride combos
    const struct
    {
        crd::u64 n, c, h, w, oc, k, pad, stride;
    } cases[] = {
        {2U, 3U, 7U, 9U, 5U, 3U, 1U, 1U},
        {1U, 1U, 16U, 16U, 8U, 3U, 1U, 1U},
        {2U, 4U, 8U, 8U, 6U, 3U, 0U, 1U},
        {2U, 2U, 9U, 9U, 4U, 3U, 1U, 2U},
    };
    for (const auto& cs : cases)
    {
        const crd::u64 xshp[4] = {cs.n, cs.c, cs.h, cs.w};
        const crd::u64 wshp[4] = {cs.oc, cs.c, cs.k, cs.k};
        Tensor<crd::f32> x(&alloc, {xshp, 4});
        Tensor<crd::f32> w(&alloc, {wshp, 4});
        Tensor<crd::f32> bias(&alloc);
        const crd::u64 bshp[1] = {cs.oc};
        REQUIRE(bias.resize({bshp, 1}) == TensorStatus::Ok);
        fill_rand(x.data(), x.size(), 5U, 0U);
        fill_rand(w.data(), w.size(), 5U, 1U);
        fill_rand(bias.data(), bias.size(), 5U, 2U);
        const crd::u64 oh = (cs.h + 2U * cs.pad - cs.k) / cs.stride + 1U;
        const crd::u64 ow = (cs.w + 2U * cs.pad - cs.k) / cs.stride + 1U;
        const crd::u64 yshp[4] = {cs.n, cs.oc, oh, ow};
        Tensor<crd::f32> y(&alloc, {yshp, 4});
        const crd::u64 sshp[1] = {crd::hesap::tensor::nn_conv2d_scratch_floats(cs.c, cs.k, cs.k, oh, ow)};
        Tensor<crd::f32> scratch(&alloc, {sshp, 1});
        REQUIRE(crd::hesap::tensor::nn_conv2d_f32(x.view(), w.view(),
                                                  {bias.data(), static_cast<crd::usize>(cs.oc)}, cs.pad,
                                                  cs.stride, y.view(),
                                                  {scratch.data(), static_cast<crd::usize>(scratch.size())}) ==
                TensorStatus::Ok);
        // reference: the same k-ordered single-rounded fma chain (the direct
        // kernel's bit contract) computed scalar
        crd::u64 mism = 0;
        for (crd::u64 img = 0; img < cs.n; ++img)
        {
            for (crd::u64 o = 0; o < cs.oc; ++o)
            {
                for (crd::u64 oi = 0; oi < oh; ++oi)
                {
                    for (crd::u64 oj = 0; oj < ow; ++oj)
                    {
                        crd::f32 acc = 0.0F;
                        for (crd::u64 ci = 0; ci < cs.c; ++ci)
                        {
                            for (crd::u64 ki = 0; ki < cs.k; ++ki)
                            {
                                for (crd::u64 kj = 0; kj < cs.k; ++kj)
                                {
                                    const crd::i64 ih = static_cast<crd::i64>(oi * cs.stride + ki) -
                                                        static_cast<crd::i64>(cs.pad);
                                    const crd::i64 iw = static_cast<crd::i64>(oj * cs.stride + kj) -
                                                        static_cast<crd::i64>(cs.pad);
                                    crd::f32 xv = 0.0F;
                                    if (ih >= 0 && ih < static_cast<crd::i64>(cs.h) && iw >= 0 &&
                                        iw < static_cast<crd::i64>(cs.w))
                                    {
                                        xv = x.data()[((img * cs.c + ci) * cs.h +
                                                       static_cast<crd::u64>(ih)) *
                                                          cs.w +
                                                      static_cast<crd::u64>(iw)];
                                    }
                                    const crd::f32 wv =
                                        w.data()[((o * cs.c + ci) * cs.k + ki) * cs.k + kj];
                                    acc = std::fma(wv, xv, acc);
                                }
                            }
                        }
                        const crd::f32 ref = bias.data()[o] + acc;
                        const crd::f32 got = y.data()[((img * cs.oc + o) * oh + oi) * ow + oj];
                        if (std::bit_cast<crd::u32>(ref) != std::bit_cast<crd::u32>(got))
                        {
                            ++mism;
                        }
                    }
                }
            }
        }
        INFO("conv case n=" << cs.n << " c=" << cs.c << " pad=" << cs.pad << " stride=" << cs.stride);
        REQUIRE(mism == 0U);
    }
}

TEST_CASE("nn: linear f32 is bit-exact vs the k-ordered fma-chain reference", "[v14m][nn]")
{
    crd::memory::TlsfAllocator alloc(kPool);
    const crd::u64 m = 7U;
    const crd::u64 k = 33U; // non-multiple-of-8 tails
    const crd::u64 n = 13U;
    const crd::u64 xshp[2] = {m, k};
    Tensor<crd::f32> x(&alloc, {xshp, 2});
    const crd::u64 wtshp[2] = {k, n};
    Tensor<crd::f32> wt(&alloc, {wtshp, 2});
    Tensor<crd::f32> bias(&alloc);
    const crd::u64 bshp[1] = {n};
    REQUIRE(bias.resize({bshp, 1}) == TensorStatus::Ok);
    fill_rand(x.data(), x.size(), 9U, 0U);
    fill_rand(wt.data(), wt.size(), 9U, 1U);
    fill_rand(bias.data(), bias.size(), 9U, 2U);
    const crd::u64 yshp[2] = {m, n};
    Tensor<crd::f32> y(&alloc, {yshp, 2});
    REQUIRE(crd::hesap::tensor::nn_linear_f32(x.view(), {wt.data(), static_cast<crd::usize>(wt.size())},
                                              {bias.data(), static_cast<crd::usize>(n)},
                                              y.view()) == TensorStatus::Ok);
    crd::u64 mism = 0;
    for (crd::u64 r = 0; r < m; ++r)
    {
        for (crd::u64 o = 0; o < n; ++o)
        {
            crd::f32 acc = 0.0F;
            for (crd::u64 p = 0; p < k; ++p)
            {
                acc = std::fma(x.data()[r * k + p], wt.data()[p * n + o], acc);
            }
            const crd::f32 ref = bias.data()[o] + acc;
            if (std::bit_cast<crd::u32>(ref) != std::bit_cast<crd::u32>(y.data()[r * n + o]))
            {
                ++mism;
            }
        }
    }
    REQUIRE(mism == 0U);
}

TEST_CASE("nn: softmax rows sum to one and the all-equal-logits row is exactly uniform", "[v14m][nn]")
{
    crd::memory::TlsfAllocator alloc(kPool);
    const crd::u64 shp[2] = {17U, 10U};
    Tensor<crd::f32> x(&alloc, {shp, 2});
    fill_rand(x.data(), x.size(), 13U, 0U);
    for (crd::u64 j = 0; j < 10U; ++j) // adversary: an all-equal-logits row
    {
        x.data()[3U * 10U + j] = 2.5F;
    }
    for (crd::u64 j = 0; j < 10U; ++j) // adversary: a huge-magnitude row (stability)
    {
        x.data()[5U * 10U + j] = j == 4U ? 1000.0F : -1000.0F;
    }
    Tensor<crd::f32> y(&alloc, {shp, 2});
    REQUIRE(crd::hesap::tensor::nn_softmax(x.view(), y.view()) == TensorStatus::Ok);
    for (crd::u64 r = 0; r < 17U; ++r)
    {
        crd::f64 s = 0.0;
        for (crd::u64 j = 0; j < 10U; ++j)
        {
            const crd::f32 v = y.data()[r * 10U + j];
            REQUIRE(std::isfinite(v));
            REQUIRE(v >= 0.0F);
            s += static_cast<crd::f64>(v);
        }
        INFO("row " << r);
        REQUIRE(std::fabs(s - 1.0) <= 1e-6);
    }
    for (crd::u64 j = 0; j < 10U; ++j) // exp(0)=1 exactly -> 1/10 in every lane, bit-equal
    {
        REQUIRE(std::bit_cast<crd::u32>(y.data()[3U * 10U + j]) == std::bit_cast<crd::u32>(0.1F));
    }
    REQUIRE(y.data()[5U * 10U + 4U] >= 0.999F); // the stable max-subtract path
}

TEST_CASE("nn: layernorm normalizes to zero mean unit variance and applies the affine", "[v14m][nn]")
{
    crd::memory::TlsfAllocator alloc(kPool);
    const crd::u64 rows = 9U;
    const crd::u64 d = 32U;
    const crd::u64 shp[2] = {rows, d};
    Tensor<crd::f32> x(&alloc, {shp, 2});
    fill_rand(x.data(), x.size(), 17U, 0U);
    Tensor<crd::f32> ones(&alloc);
    Tensor<crd::f32> zeros(&alloc);
    const crd::u64 dshp[1] = {d};
    REQUIRE(ones.resize({dshp, 1}) == TensorStatus::Ok);
    REQUIRE(zeros.resize({dshp, 1}) == TensorStatus::Ok);
    for (crd::u64 j = 0; j < d; ++j)
    {
        ones.data()[j] = 1.0F;
        zeros.data()[j] = 0.0F;
    }
    Tensor<crd::f32> y(&alloc, {shp, 2});
    REQUIRE(crd::hesap::tensor::nn_layernorm(x.view(), {ones.data(), static_cast<crd::usize>(d)},
                                             {zeros.data(), static_cast<crd::usize>(d)}, 1e-5F,
                                             y.view()) == TensorStatus::Ok);
    for (crd::u64 r = 0; r < rows; ++r)
    {
        crd::f64 mean = 0.0;
        crd::f64 var = 0.0;
        for (crd::u64 j = 0; j < d; ++j)
        {
            mean += static_cast<crd::f64>(y.data()[r * d + j]);
        }
        mean /= static_cast<crd::f64>(d);
        for (crd::u64 j = 0; j < d; ++j)
        {
            const crd::f64 t = static_cast<crd::f64>(y.data()[r * d + j]) - mean;
            var += t * t;
        }
        var /= static_cast<crd::f64>(d);
        INFO("row " << r);
        REQUIRE(std::fabs(mean) <= 1e-6);
        REQUIRE(std::fabs(var - 1.0) <= 1e-3); // 1 - eps/sigma^2 correction band
    }
    // affine: gamma=2, beta=-1 must equal 2*normalized - 1 within rounding
    Tensor<crd::f32> g2(&alloc);
    Tensor<crd::f32> bm1(&alloc);
    REQUIRE(g2.resize({dshp, 1}) == TensorStatus::Ok);
    REQUIRE(bm1.resize({dshp, 1}) == TensorStatus::Ok);
    for (crd::u64 j = 0; j < d; ++j)
    {
        g2.data()[j] = 2.0F;
        bm1.data()[j] = -1.0F;
    }
    Tensor<crd::f32> y2(&alloc, {shp, 2});
    REQUIRE(crd::hesap::tensor::nn_layernorm(x.view(), {g2.data(), static_cast<crd::usize>(d)},
                                             {bm1.data(), static_cast<crd::usize>(d)}, 1e-5F,
                                             y2.view()) == TensorStatus::Ok);
    for (crd::u64 i = 0; i < rows * d; ++i)
    {
        REQUIRE(std::fabs(y2.data()[i] - (2.0F * y.data()[i] - 1.0F)) <= 2e-6F);
    }
}

TEST_CASE("nn: max and avg pooling match the window references", "[v14m][nn]")
{
    crd::memory::TlsfAllocator alloc(kPool);
    const crd::u64 shp[4] = {2U, 3U, 16U, 16U};
    Tensor<crd::f32> x(&alloc, {shp, 4});
    fill_rand(x.data(), x.size(), 19U, 0U);
    const crd::u64 oshp[4] = {2U, 3U, 8U, 8U};
    Tensor<crd::f32> ym(&alloc, {oshp, 4});
    Tensor<crd::f32> ya(&alloc, {oshp, 4});
    REQUIRE(crd::hesap::tensor::nn_maxpool2d(x.view(), 2U, 2U, ym.view()) == TensorStatus::Ok);
    REQUIRE(crd::hesap::tensor::nn_avgpool2d(x.view(), 2U, 2U, ya.view()) == TensorStatus::Ok);
    crd::u64 mism = 0;
    for (crd::u64 p = 0; p < 6U; ++p)
    {
        const crd::f32* xp = x.data() + p * 256U;
        for (crd::u64 oi = 0; oi < 8U; ++oi)
        {
            for (crd::u64 oj = 0; oj < 8U; ++oj)
            {
                const crd::f32 a = xp[(2U * oi) * 16U + 2U * oj];
                const crd::f32 b = xp[(2U * oi) * 16U + 2U * oj + 1U];
                const crd::f32 c = xp[(2U * oi + 1U) * 16U + 2U * oj];
                const crd::f32 dd = xp[(2U * oi + 1U) * 16U + 2U * oj + 1U];
                crd::f32 mx = a;
                mx = b > mx ? b : mx;
                mx = c > mx ? c : mx;
                mx = dd > mx ? dd : mx;
                // avg reference mirrors the kernel's row-major accumulation order
                const crd::f32 av = (((a + b) + c) + dd) * 0.25F;
                if (std::bit_cast<crd::u32>(mx) !=
                    std::bit_cast<crd::u32>(ym.data()[p * 64U + oi * 8U + oj]))
                {
                    ++mism;
                }
                if (std::bit_cast<crd::u32>(av) !=
                    std::bit_cast<crd::u32>(ya.data()[p * 64U + oi * 8U + oj]))
                {
                    ++mism;
                }
            }
        }
    }
    REQUIRE(mism == 0U);
}

TEST_CASE("nn: activations match their reference formulas", "[v14m][nn]")
{
    crd::memory::TlsfAllocator alloc(kPool);
    const crd::u64 n = 1003U; // vector body + scalar tail
    const crd::u64 shp[1] = {n};
    Tensor<crd::f32> x(&alloc, {shp, 1});
    fill_rand(x.data(), n, 23U, 0U);
    for (crd::u64 i = 0; i < n; ++i)
    {
        x.data()[i] *= 6.0F; // cover the saturating bands
    }
    x.data()[0] = 0.0F;
    x.data()[1] = -0.0F;
    Tensor<crd::f32> y(&alloc, {shp, 1});
    const crd::containers::ConstSpan<crd::f32> xs{x.data(), static_cast<crd::usize>(n)};
    const crd::containers::Span<crd::f32> ys{y.data(), static_cast<crd::usize>(n)};

    crd::hesap::tensor::nn_relu(xs, ys);
    for (crd::u64 i = 0; i < n; ++i)
    {
        const crd::f32 ref = x.data()[i] > 0.0F ? x.data()[i] : 0.0F;
        REQUIRE(std::bit_cast<crd::u32>(y.data()[i]) == std::bit_cast<crd::u32>(ref));
    }

    crd::hesap::tensor::nn_gelu(xs, ys);
    for (crd::u64 i = 0; i < n; ++i)
    {
        const crd::f64 xd = static_cast<crd::f64>(x.data()[i]);
        const crd::f64 ref = 0.5 * xd * (1.0 + std::erf(xd * 0.70710678118654752440));
        REQUIRE(std::fabs(static_cast<crd::f64>(y.data()[i]) - ref) <= 1e-6);
    }
    REQUIRE(y.data()[0] == 0.0F); // gelu(0) = 0 exactly

    crd::hesap::tensor::nn_tanh(xs, ys);
    for (crd::u64 i = 0; i < n; ++i)
    {
        REQUIRE(std::fabs(y.data()[i] - std::tanh(x.data()[i])) <= 1e-6F);
    }

    crd::hesap::tensor::nn_sigmoid(xs, ys);
    for (crd::u64 i = 0; i < n; ++i)
    {
        const crd::f64 ref = 1.0 / (1.0 + std::exp(-static_cast<crd::f64>(x.data()[i])));
        REQUIRE(std::fabs(static_cast<crd::f64>(y.data()[i]) - ref) <= 1e-6);
    }
}

namespace
{

struct Q8Ref
{
    explicit Q8Ref(crd::memory::IAllocator* alloc) : q8(alloc), q8s(alloc) {}
    crd::containers::Array<crd::u8> q8;
    crd::containers::Array<crd::u8> q8s;

    [[nodiscard]] crd::containers::ConstSpan<crd::i8> qs() const noexcept
    {
        return {reinterpret_cast<const crd::i8*>(q8.data()), q8.size()};
    }
    [[nodiscard]] crd::containers::ConstSpan<crd::u16> scales() const noexcept
    {
        return {reinterpret_cast<const crd::u16*>(q8s.data()), q8s.size() / 2U};
    }
};

bool load_q8_ref(crd::memory::IAllocator* alloc, const char* model, const char* key, Q8Ref& out)
{
    crd::containers::String a(alloc);
    a.append(model);
    a.append("_");
    a.append(key);
    a.append(".q8");
    crd::containers::String b(alloc);
    b.append(model);
    b.append("_");
    b.append(key);
    b.append(".q8s");
    return read_corpus_file(alloc, a.c_str(), out.q8) == TensorStatus::Ok &&
           read_corpus_file(alloc, b.c_str(), out.q8s) == TensorStatus::Ok;
}

} // namespace

TEST_CASE("nn: the vectorized activation quantizer is bit-identical to dtypes quantize_q8_0",
          "[v14m][nn][q8]")
{
    crd::memory::TlsfAllocator alloc(kPool);
    const crd::u64 nblocks = 257U; // odd count, vector body exercised block by block
    const crd::u64 n = nblocks * kQuantBlock;
    const crd::u64 shp[1] = {n};
    Tensor<crd::f32> x(&alloc, {shp, 1});
    fill_rand(x.data(), n, 41U, 0U);
    for (crd::u64 i = 0; i < n; ++i)
    {
        x.data()[i] *= 100.0F; // wide dynamic range
    }
    // adversaries: an all-zero block, a +-0 block, a huge block, a denormal-ish block
    for (crd::u64 j = 0; j < kQuantBlock; ++j)
    {
        x.data()[0U * kQuantBlock + j] = 0.0F;
        x.data()[1U * kQuantBlock + j] = (j % 2U == 0U) ? 0.0F : -0.0F;
        x.data()[2U * kQuantBlock + j] = (j % 2U == 0U) ? 3.4e37F : -3.4e37F;
        x.data()[3U * kQuantBlock + j] = static_cast<crd::f32>(j) * 1e-30F; // tiny-scale block
    }
    Tensor<BlockQ8_0> qa(&alloc);
    Tensor<BlockQ8_0> qb(&alloc);
    const crd::u64 qshp[1] = {nblocks};
    REQUIRE(qa.resize({qshp, 1}) == TensorStatus::Ok);
    REQUIRE(qb.resize({qshp, 1}) == TensorStatus::Ok);
    crd::hesap::tensor::nndetail::quantize_q8_0_fast(x.data(), qa.data(), nblocks);
    crd::hesap::tensor::quantize_q8_0({x.data(), static_cast<crd::usize>(n)},
                                      {qb.data(), static_cast<crd::usize>(nblocks)});
    REQUIRE(std::memcmp(qa.data(), qb.data(), static_cast<crd::usize>(nblocks) * sizeof(BlockQ8_0)) == 0);
}

TEST_CASE("nn: q8 quantize-on-load is byte-exact vs the frozen q8 refs for every weight", "[v14m][nn][q8]")
{
    crd::memory::TlsfAllocator alloc(kPool);
    const struct
    {
        const char* model;
        const char* key;
    } weights[] = {
        {"mlp", "fc1.weight"}, {"mlp", "fc2.weight"}, {"mlp", "fc3.weight"},
        {"cnn", "c1.weight"},  {"cnn", "c2.weight"},  {"cnn", "fc.weight"},
    };
    for (const auto& wk : weights)
    {
        INFO("weight " << wk.model << "." << wk.key);
        LoadedModel m(&alloc);
        crd::containers::String sf(&alloc);
        sf.append(wk.model);
        sf.append(".safetensors");
        REQUIRE(read_corpus_file(&alloc, sf.c_str(), m.bytes) == TensorStatus::Ok);
        REQUIRE(m.st.parse(crd::containers::as_const_span(m.bytes)) == TensorStatus::Ok);
        Tensor<crd::f32> w(&alloc);
        REQUIRE(crd::hesap::tensor::nndetail::st_read_f32(m.st, &alloc, wk.key, w) == TensorStatus::Ok);
        const crd::u64 nblocks = (w.size() + kQuantBlock - 1U) / kQuantBlock;
        Tensor<BlockQ8_0> q(&alloc);
        const crd::u64 qshp[1] = {nblocks};
        REQUIRE(q.resize({qshp, 1}) == TensorStatus::Ok);
        crd::hesap::tensor::nn_quantize_q8_0_rint({w.data(), static_cast<crd::usize>(w.size())},
                                                  {q.data(), static_cast<crd::usize>(nblocks)});
        Q8Ref ref(&alloc);
        REQUIRE(load_q8_ref(&alloc, wk.model, wk.key, ref));
        REQUIRE(ref.qs().size() == nblocks * kQuantBlock);
        REQUIRE(ref.scales().size() == nblocks);
        crd::u64 qmism = 0;
        crd::u64 smism = 0;
        for (crd::u64 i = 0; i < nblocks; ++i)
        {
            if (std::memcmp(q.data()[i].qs, ref.qs().data() + i * kQuantBlock, kQuantBlock) != 0)
            {
                ++qmism;
            }
            if (q.data()[i].d != ref.scales()[i])
            {
                ++smism;
            }
        }
        REQUIRE(qmism == 0U);
        REQUIRE(smism == 0U);
    }
}

TEST_CASE("nn: q8 quantize-on-load and raw-ref weight loading are bit-exact against each other",
          "[v14m][nn][q8]")
{
    crd::memory::TlsfAllocator alloc(kPool);
    // MLP: quantize-on-load net vs a hand-assembled raw-ref net
    LoadedModel m(&alloc);
    REQUIRE(load_model(&alloc, "mlp", m));
    NnSequential qa(&alloc);
    REQUIRE(build_mlp_from_safetensors(&alloc, m.st, m.x.shape(0), true, qa) == TensorStatus::Ok);

    Tensor<crd::f32> b1(&alloc);
    Tensor<crd::f32> b2(&alloc);
    Tensor<crd::f32> b3(&alloc);
    Tensor<crd::f32> lng(&alloc);
    Tensor<crd::f32> lnb(&alloc);
    using crd::hesap::tensor::nndetail::st_read_f32;
    REQUIRE(st_read_f32(m.st, &alloc, "fc1.bias", b1) == TensorStatus::Ok);
    REQUIRE(st_read_f32(m.st, &alloc, "fc2.bias", b2) == TensorStatus::Ok);
    REQUIRE(st_read_f32(m.st, &alloc, "fc3.bias", b3) == TensorStatus::Ok);
    REQUIRE(st_read_f32(m.st, &alloc, "ln.weight", lng) == TensorStatus::Ok);
    REQUIRE(st_read_f32(m.st, &alloc, "ln.bias", lnb) == TensorStatus::Ok);
    Q8Ref r1(&alloc);
    Q8Ref r2(&alloc);
    Q8Ref r3(&alloc);
    REQUIRE(load_q8_ref(&alloc, "mlp", "fc1.weight", r1));
    REQUIRE(load_q8_ref(&alloc, "mlp", "fc2.weight", r2));
    REQUIRE(load_q8_ref(&alloc, "mlp", "fc3.weight", r3));
    NnSequential qb(&alloc);
    REQUIRE(qb.add_linear_q8_raw(r1.qs(), r1.scales(), 128U, 64U, b1.view()) == TensorStatus::Ok);
    REQUIRE(qb.add_relu() == TensorStatus::Ok);
    REQUIRE(qb.add_linear_q8_raw(r2.qs(), r2.scales(), 32U, 128U, b2.view()) == TensorStatus::Ok);
    REQUIRE(qb.add_relu() == TensorStatus::Ok);
    REQUIRE(qb.add_layernorm(lng.view(), lnb.view()) == TensorStatus::Ok);
    REQUIRE(qb.add_linear_q8_raw(r3.qs(), r3.scales(), 10U, 32U, b3.view()) == TensorStatus::Ok);
    REQUIRE(qb.add_softmax() == TensorStatus::Ok);
    const crd::u64 in_shp[2] = {m.x.shape(0), 64U};
    REQUIRE(qb.finalize({in_shp, 2}) == TensorStatus::Ok);

    for (const crd::u32 opi : {0U, 2U, 5U})
    {
        const auto qav = qa.q8_blocks(opi);
        const auto qbv = qb.q8_blocks(opi);
        REQUIRE(qav.size() == qbv.size());
        REQUIRE(std::memcmp(qav.data(), qbv.data(), qav.size() * sizeof(BlockQ8_0)) == 0);
    }
    const crd::u64 wsa[1] = {qa.workspace_bytes()};
    Tensor<crd::u8> ws(&alloc, {wsa, 1});
    Tensor<crd::f32> ya(&alloc, m.y.shape());
    Tensor<crd::f32> yb(&alloc, m.y.shape());
    REQUIRE(qa.infer(m.x.view(), ya.view(), {ws.data(), static_cast<crd::usize>(ws.size())}, 1U) ==
            TensorStatus::Ok);
    REQUIRE(qb.infer(m.x.view(), yb.view(), {ws.data(), static_cast<crd::usize>(ws.size())}, 1U) ==
            TensorStatus::Ok);
    REQUIRE(std::memcmp(ya.data(), yb.data(), ya.size() * sizeof(crd::f32)) == 0);

    // CNN: same gate through the conv-dequant path
    LoadedModel mc(&alloc);
    REQUIRE(load_model(&alloc, "cnn", mc));
    NnSequential ca(&alloc);
    REQUIRE(build_cnn_from_safetensors(&alloc, mc.st, mc.x.shape(0), true, ca) == TensorStatus::Ok);
    Tensor<crd::f32> c1b(&alloc);
    Tensor<crd::f32> c2b(&alloc);
    Tensor<crd::f32> fcb(&alloc);
    REQUIRE(st_read_f32(mc.st, &alloc, "c1.bias", c1b) == TensorStatus::Ok);
    REQUIRE(st_read_f32(mc.st, &alloc, "c2.bias", c2b) == TensorStatus::Ok);
    REQUIRE(st_read_f32(mc.st, &alloc, "fc.bias", fcb) == TensorStatus::Ok);
    Q8Ref rc1(&alloc);
    Q8Ref rc2(&alloc);
    Q8Ref rfc(&alloc);
    REQUIRE(load_q8_ref(&alloc, "cnn", "c1.weight", rc1));
    REQUIRE(load_q8_ref(&alloc, "cnn", "c2.weight", rc2));
    REQUIRE(load_q8_ref(&alloc, "cnn", "fc.weight", rfc));
    NnSequential cb(&alloc);
    REQUIRE(cb.add_conv2d_q8_raw(rc1.qs(), rc1.scales(), 8U, 1U, 3U, 3U, c1b.view(), 1U) == TensorStatus::Ok);
    REQUIRE(cb.add_relu() == TensorStatus::Ok);
    REQUIRE(cb.add_maxpool2d(2U, 2U) == TensorStatus::Ok);
    REQUIRE(cb.add_conv2d_q8_raw(rc2.qs(), rc2.scales(), 16U, 8U, 3U, 3U, c2b.view(), 1U) == TensorStatus::Ok);
    REQUIRE(cb.add_relu() == TensorStatus::Ok);
    REQUIRE(cb.add_maxpool2d(2U, 2U) == TensorStatus::Ok);
    REQUIRE(cb.add_flatten() == TensorStatus::Ok);
    REQUIRE(cb.add_linear_q8_raw(rfc.qs(), rfc.scales(), 10U, 256U, fcb.view()) == TensorStatus::Ok);
    REQUIRE(cb.add_softmax() == TensorStatus::Ok);
    const crd::u64 cshp[4] = {mc.x.shape(0), 1U, 16U, 16U};
    REQUIRE(cb.finalize({cshp, 4}) == TensorStatus::Ok);
    for (const crd::u32 opi : {0U, 3U, 7U})
    {
        const auto qav = ca.q8_blocks(opi);
        const auto qbv = cb.q8_blocks(opi);
        REQUIRE(qav.size() == qbv.size());
        REQUIRE(qav.size() > 0U);
        REQUIRE(std::memcmp(qav.data(), qbv.data(), qav.size() * sizeof(BlockQ8_0)) == 0);
        // conv ops: the dequantized f32 weights must also be bit-identical
        const auto wav = ca.weight_f32(opi);
        const auto wbv = cb.weight_f32(opi);
        REQUIRE(wav.size() == wbv.size());
        REQUIRE(std::memcmp(wav.data(), wbv.data(), wav.size() * sizeof(crd::f32)) == 0);
    }
    const crd::u64 wsc[1] = {ca.workspace_bytes()};
    Tensor<crd::u8> ws2(&alloc, {wsc, 1});
    Tensor<crd::f32> yca(&alloc, mc.y.shape());
    Tensor<crd::f32> ycb(&alloc, mc.y.shape());
    REQUIRE(ca.infer(mc.x.view(), yca.view(), {ws2.data(), static_cast<crd::usize>(ws2.size())}, 1U) ==
            TensorStatus::Ok);
    REQUIRE(cb.infer(mc.x.view(), ycb.view(), {ws2.data(), static_cast<crd::usize>(ws2.size())}, 1U) ==
            TensorStatus::Ok);
    REQUIRE(std::memcmp(yca.data(), ycb.data(), yca.size() * sizeof(crd::f32)) == 0);
}

TEST_CASE("nn: quantized inference is run-twice deterministic with bounded error vs f32", "[v14m][nn][q8]")
{
    crd::memory::TlsfAllocator alloc(kPool);
    const char* models[] = {"mlp", "cnn"};
    for (const char* name : models)
    {
        INFO("model " << name);
        LoadedModel m(&alloc);
        REQUIRE(load_model(&alloc, name, m));
        const bool is_mlp = std::strcmp(name, "mlp") == 0;
        NnSequential nf(&alloc);
        NnSequential nq(&alloc);
        if (is_mlp)
        {
            REQUIRE(build_mlp_from_safetensors(&alloc, m.st, m.x.shape(0), false, nf) == TensorStatus::Ok);
            REQUIRE(build_mlp_from_safetensors(&alloc, m.st, m.x.shape(0), true, nq) == TensorStatus::Ok);
        }
        else
        {
            REQUIRE(build_cnn_from_safetensors(&alloc, m.st, m.x.shape(0), false, nf) == TensorStatus::Ok);
            REQUIRE(build_cnn_from_safetensors(&alloc, m.st, m.x.shape(0), true, nq) == TensorStatus::Ok);
        }
        const crd::u64 wsb[1] = {nq.workspace_bytes() > nf.workspace_bytes() ? nq.workspace_bytes()
                                                                             : nf.workspace_bytes()};
        Tensor<crd::u8> ws(&alloc, {wsb, 1});
        Tensor<crd::f32> yf(&alloc, m.y.shape());
        Tensor<crd::f32> yq1(&alloc, m.y.shape());
        Tensor<crd::f32> yq2(&alloc, m.y.shape());
        const crd::containers::Span<crd::u8> wss{ws.data(), static_cast<crd::usize>(ws.size())};
        REQUIRE(nf.infer(m.x.view(), yf.view(), wss, 1U) == TensorStatus::Ok);
        REQUIRE(nq.infer(m.x.view(), yq1.view(), wss, 1U) == TensorStatus::Ok);
        REQUIRE(nq.infer(m.x.view(), yq2.view(), wss, 1U) == TensorStatus::Ok);
        REQUIRE(std::memcmp(yq1.data(), yq2.data(), yq1.size() * sizeof(crd::f32)) == 0); // run-twice
        crd::f32 maxd = 0.0F;
        crd::u64 argmax_mism = 0;
        const crd::u64 rows = m.y.shape(0);
        const crd::u64 d = m.y.shape(1);
        for (crd::u64 r = 0; r < rows; ++r)
        {
            crd::u64 af = 0;
            crd::u64 aq = 0;
            for (crd::u64 j = 0; j < d; ++j)
            {
                const crd::f32 dv = std::fabs(yq1.data()[r * d + j] - yf.data()[r * d + j]);
                maxd = dv > maxd ? dv : maxd;
                if (yf.data()[r * d + j] > yf.data()[r * d + af])
                {
                    af = j;
                }
                if (yq1.data()[r * d + j] > yq1.data()[r * d + aq])
                {
                    aq = j;
                }
            }
            argmax_mism += af == aq ? 0U : 1U;
        }
        std::printf("[v14m] %s q8 vs f32: max abs prob diff = %.3e, argmax mismatches = %llu/%llu\n", name,
                    static_cast<double>(maxd), static_cast<unsigned long long>(argmax_mism),
                    static_cast<unsigned long long>(rows));
        REQUIRE(maxd <= 2e-2F); // bounded quantization error on the probability simplex
        REQUIRE(argmax_mism == 0U); // classification preserved on the frozen corpus
    }
}

TEST_CASE("nn: inference is bit-identical at 1 2 4 8 16 workers for f32 and q8", "[v14m][nn][moat]")
{
    crd::memory::TlsfAllocator alloc(kPool);
    const char* models[] = {"mlp", "cnn"};
    for (const char* name : models)
    {
        INFO("model " << name);
        LoadedModel m(&alloc);
        REQUIRE(load_model(&alloc, name, m));
        const bool is_mlp = std::strcmp(name, "mlp") == 0;
        for (const bool quantized : {false, true})
        {
            NnSequential net(&alloc);
            if (is_mlp)
            {
                REQUIRE(build_mlp_from_safetensors(&alloc, m.st, m.x.shape(0), quantized, net) ==
                        TensorStatus::Ok);
            }
            else
            {
                REQUIRE(build_cnn_from_safetensors(&alloc, m.st, m.x.shape(0), quantized, net) ==
                        TensorStatus::Ok);
            }
            const crd::u64 wsb[1] = {net.workspace_bytes()};
            Tensor<crd::u8> ws(&alloc, {wsb, 1});
            const crd::containers::Span<crd::u8> wss{ws.data(), static_cast<crd::usize>(ws.size())};
            Tensor<crd::f32> yserial(&alloc, m.y.shape());
            REQUIRE(net.infer(m.x.view(), yserial.view(), wss, 1U) == TensorStatus::Ok);
            for (const crd::u32 nw : {1U, 2U, 4U, 8U, 16U})
            {
                crd::jobs::Config cfg;
                cfg.num_threads = nw;
                crd::jobs::init(cfg);
                Tensor<crd::f32> y(&alloc, m.y.shape());
                const TensorStatus st = net.infer(m.x.view(), y.view(), wss, 0U);
                crd::jobs::shutdown();
                REQUIRE(st == TensorStatus::Ok);
                crd::u64 mism = 0;
                for (crd::u64 i = 0; i < y.size(); ++i)
                {
                    if (std::bit_cast<crd::u32>(y.data()[i]) != std::bit_cast<crd::u32>(yserial.data()[i]))
                    {
                        ++mism;
                    }
                }
                INFO("quantized " << quantized << " workers " << nw);
                REQUIRE(mism == 0U);
            }
        }
    }
}

TEST_CASE("nn: infer performs ZERO allocations - counting-allocator gate", "[v14m][nn]")
{
    crd::memory::TlsfAllocator tlsf(kPool);
    CountingAllocator alloc(&tlsf);
    const char* models[] = {"mlp", "cnn"};
    for (const char* name : models)
    {
        INFO("model " << name);
        LoadedModel m(&alloc);
        REQUIRE(load_model(&alloc, name, m));
        const bool is_mlp = std::strcmp(name, "mlp") == 0;
        for (const NnQuantTier tier : {NnQuantTier::F32, NnQuantTier::Q8Block32, NnQuantTier::I8PerTensor})
        {
            NnSequential net(&alloc);
            if (is_mlp)
            {
                REQUIRE(build_mlp_from_safetensors(&alloc, m.st, m.x.shape(0), tier, net) ==
                        TensorStatus::Ok);
            }
            else
            {
                REQUIRE(build_cnn_from_safetensors(&alloc, m.st, m.x.shape(0), tier, net) ==
                        TensorStatus::Ok);
            }
            const crd::u64 wsb[1] = {net.workspace_bytes()};
            Tensor<crd::u8> ws(&alloc, {wsb, 1});
            Tensor<crd::f32> y(&alloc, m.y.shape());
            const crd::u64 before = alloc.alloc_calls();
            for (int rep = 0; rep < 3; ++rep)
            {
                REQUIRE(net.infer(m.x.view(), y.view(),
                                  {ws.data(), static_cast<crd::usize>(ws.size())}, 1U) == TensorStatus::Ok);
            }
            INFO("tier " << static_cast<int>(tier));
            REQUIRE(alloc.alloc_calls() == before); // allocation-free inference
        }
    }
}

TEST_CASE("nn: per-tensor int8 linear matches the pinned scalar reference bit for bit", "[v14m][nn][i8]")
{
    crd::memory::TlsfAllocator alloc(kPool);
    const crd::u64 m = 7U;
    const crd::u64 k = 64U;
    const crd::u64 n = 13U; // odd -> exercises the tail tile (t < 4)
    const crd::u64 xshp[2] = {m, k};
    Tensor<crd::f32> x(&alloc, {xshp, 2});
    fill_rand(x.data(), x.size(), 51U, 0U);
    for (crd::u64 i = 0; i < x.size(); ++i)
    {
        x.data()[i] *= 3.0F;
    }
    const crd::u64 wshp[2] = {n, k};
    Tensor<crd::f32> w(&alloc, {wshp, 2});
    fill_rand(w.data(), w.size(), 51U, 1U);
    Tensor<crd::f32> bias(&alloc);
    const crd::u64 bshp[1] = {n};
    REQUIRE(bias.resize({bshp, 1}) == TensorStatus::Ok);
    fill_rand(bias.data(), n, 51U, 2U);
    // weight quantization (the load-time semantics) + column sums
    Tensor<crd::i8> wq(&alloc, {wshp, 2});
    crd::f32 sw = 0.0F;
    crd::hesap::tensor::nn_quantize_i8_per_tensor({w.data(), static_cast<crd::usize>(w.size())},
                                                  {wq.data(), static_cast<crd::usize>(w.size())}, sw);
    REQUIRE(sw > 0.0F);
    Tensor<crd::i32> colsum(&alloc);
    const crd::u64 bshp2[1] = {n};
    REQUIRE(colsum.resize({bshp2, 1}) == TensorStatus::Ok);
    for (crd::u64 o = 0; o < n; ++o)
    {
        crd::i32 cs = 0;
        for (crd::u64 j = 0; j < k; ++j)
        {
            cs += static_cast<crd::i32>(wq.data()[o * k + j]);
        }
        colsum.data()[o] = cs;
    }
    const crd::u64 yshp[2] = {m, n};
    Tensor<crd::f32> y(&alloc, {yshp, 2});
    Tensor<crd::u8> xq(&alloc, {xshp, 2});
    Tensor<crd::f32> xsc(&alloc);
    Tensor<crd::i32> xzp(&alloc);
    const crd::u64 mshp[1] = {m};
    REQUIRE(xsc.resize({mshp, 1}) == TensorStatus::Ok);
    REQUIRE(xzp.resize({mshp, 1}) == TensorStatus::Ok);
    REQUIRE(crd::hesap::tensor::nn_linear_i8(x.view(), {wq.data(), static_cast<crd::usize>(wq.size())}, sw,
                                             {colsum.data(), static_cast<crd::usize>(n)},
                                             {bias.data(), static_cast<crd::usize>(n)}, y.view(),
                                             {xq.data(), static_cast<crd::usize>(xq.size())},
                                             {xsc.data(), static_cast<crd::usize>(m)},
                                             {xzp.data(), static_cast<crd::usize>(m)}) == TensorStatus::Ok);
    for (crd::u64 r = 0; r < m; ++r)
    {
        // the scalar reference: same asymmetric-u8 quantizer, exact integer dot,
        // exact zero-point correction, one fma
        crd::u8 xr[64];
        crd::f32 sx = 0.0F;
        crd::i32 zp = 0;
        crd::hesap::tensor::nn_quantize_u8_asym({x.data() + r * k, static_cast<crd::usize>(k)},
                                                {xr, static_cast<crd::usize>(k)}, sx, zp);
        REQUIRE(std::memcmp(xr, xq.data() + r * k, static_cast<crd::usize>(k)) == 0); // quantizer identity
        REQUIRE(std::bit_cast<crd::u32>(sx) == std::bit_cast<crd::u32>(xsc.data()[r]));
        REQUIRE(zp == xzp.data()[r]);
        REQUIRE(zp >= 0);
        REQUIRE(zp <= 255);
        for (crd::u64 o = 0; o < n; ++o)
        {
            crd::i32 idot = 0;
            for (crd::u64 j = 0; j < k; ++j)
            {
                idot += static_cast<crd::i32>(xr[j]) * static_cast<crd::i32>(wq.data()[o * k + j]);
            }
            idot -= zp * colsum.data()[o];
            const crd::f32 ref = std::fma(sx * sw, static_cast<crd::f32>(idot), bias.data()[o]);
            INFO("row " << r << " out " << o);
            REQUIRE(std::bit_cast<crd::u32>(ref) == std::bit_cast<crd::u32>(y.data()[r * n + o]));
        }
    }
    // the runner's PACKED kernel (a 1-layer net) must match the row-major
    // reference path bit for bit — including the masked tail block (n = 13)
    NnSequential net1(&alloc);
    REQUIRE(net1.add_linear_i8(w.view(), bias.view()) == TensorStatus::Ok);
    const crd::u64 in1[2] = {m, k};
    REQUIRE(net1.finalize({in1, 2}) == TensorStatus::Ok);
    const crd::u64 ws1b[1] = {net1.workspace_bytes()};
    Tensor<crd::u8> ws1(&alloc, {ws1b, 1});
    Tensor<crd::f32> ynet(&alloc, {yshp, 2});
    REQUIRE(net1.infer(x.view(), ynet.view(), {ws1.data(), static_cast<crd::usize>(ws1.size())}, 1U) ==
            TensorStatus::Ok);
    REQUIRE(std::memcmp(ynet.data(), y.data(), static_cast<crd::usize>(m * n) * sizeof(crd::f32)) == 0);

    // zero-row adversary: rmin == rmax == 0 -> scale 0, zp 0 -> y == bias exactly
    Tensor<crd::f32> xz = Tensor<crd::f32>::zeros(&alloc, {xshp, 2});
    Tensor<crd::f32> yz(&alloc, {yshp, 2});
    REQUIRE(crd::hesap::tensor::nn_linear_i8(xz.view(), {wq.data(), static_cast<crd::usize>(wq.size())}, sw,
                                             {colsum.data(), static_cast<crd::usize>(n)},
                                             {bias.data(), static_cast<crd::usize>(n)}, yz.view(),
                                             {xq.data(), static_cast<crd::usize>(xq.size())},
                                             {xsc.data(), static_cast<crd::usize>(m)},
                                             {xzp.data(), static_cast<crd::usize>(m)}) == TensorStatus::Ok);
    for (crd::u64 o = 0; o < n; ++o)
    {
        REQUIRE(std::bit_cast<crd::u32>(yz.data()[o]) == std::bit_cast<crd::u32>(bias.data()[o]));
    }
    // all-positive rows (the post-relu shape): the u8 range must cover them
    Tensor<crd::f32> xp(&alloc, {xshp, 2});
    for (crd::u64 i = 0; i < xp.size(); ++i)
    {
        const crd::f32 v = x.data()[i];
        xp.data()[i] = v > 0.0F ? v : -v;
    }
    Tensor<crd::f32> yp(&alloc, {yshp, 2});
    REQUIRE(crd::hesap::tensor::nn_linear_i8(xp.view(), {wq.data(), static_cast<crd::usize>(wq.size())}, sw,
                                             {colsum.data(), static_cast<crd::usize>(n)},
                                             {bias.data(), static_cast<crd::usize>(n)}, yp.view(),
                                             {xq.data(), static_cast<crd::usize>(xq.size())},
                                             {xsc.data(), static_cast<crd::usize>(m)},
                                             {xzp.data(), static_cast<crd::usize>(m)}) == TensorStatus::Ok);
    for (crd::u64 r = 0; r < m; ++r)
    {
        REQUIRE(xzp.data()[r] == 0); // all-positive rows pin the zero point at 0
    }
}

TEST_CASE("nn: per-tensor int8 tier - run-twice, worker moat, bounded error vs f32", "[v14m][nn][i8]")
{
    crd::memory::TlsfAllocator alloc(kPool);
    const char* models[] = {"mlp", "cnn"};
    for (const char* name : models)
    {
        INFO("model " << name);
        LoadedModel m(&alloc);
        REQUIRE(load_model(&alloc, name, m));
        const bool is_mlp = std::strcmp(name, "mlp") == 0;
        NnSequential nf(&alloc);
        NnSequential ni(&alloc);
        if (is_mlp)
        {
            REQUIRE(build_mlp_from_safetensors(&alloc, m.st, m.x.shape(0), NnQuantTier::F32, nf) ==
                    TensorStatus::Ok);
            REQUIRE(build_mlp_from_safetensors(&alloc, m.st, m.x.shape(0), NnQuantTier::I8PerTensor, ni) ==
                    TensorStatus::Ok);
        }
        else
        {
            REQUIRE(build_cnn_from_safetensors(&alloc, m.st, m.x.shape(0), NnQuantTier::F32, nf) ==
                    TensorStatus::Ok);
            REQUIRE(build_cnn_from_safetensors(&alloc, m.st, m.x.shape(0), NnQuantTier::I8PerTensor, ni) ==
                    TensorStatus::Ok);
        }
        const crd::u64 wsb[1] = {ni.workspace_bytes() > nf.workspace_bytes() ? ni.workspace_bytes()
                                                                             : nf.workspace_bytes()};
        Tensor<crd::u8> ws(&alloc, {wsb, 1});
        const crd::containers::Span<crd::u8> wss{ws.data(), static_cast<crd::usize>(ws.size())};
        Tensor<crd::f32> yf(&alloc, m.y.shape());
        Tensor<crd::f32> y1(&alloc, m.y.shape());
        Tensor<crd::f32> y2(&alloc, m.y.shape());
        REQUIRE(nf.infer(m.x.view(), yf.view(), wss, 1U) == TensorStatus::Ok);
        REQUIRE(ni.infer(m.x.view(), y1.view(), wss, 1U) == TensorStatus::Ok);
        REQUIRE(ni.infer(m.x.view(), y2.view(), wss, 1U) == TensorStatus::Ok);
        REQUIRE(std::memcmp(y1.data(), y2.data(), y1.size() * sizeof(crd::f32)) == 0); // run-twice
        crd::f32 maxd = 0.0F;
        crd::u64 argmax_mism = 0;
        const crd::u64 rows = m.y.shape(0);
        const crd::u64 d = m.y.shape(1);
        for (crd::u64 r = 0; r < rows; ++r)
        {
            crd::u64 af = 0;
            crd::u64 ai = 0;
            for (crd::u64 j = 0; j < d; ++j)
            {
                const crd::f32 dv = std::fabs(y1.data()[r * d + j] - yf.data()[r * d + j]);
                maxd = dv > maxd ? dv : maxd;
                if (yf.data()[r * d + j] > yf.data()[r * d + af])
                {
                    af = j;
                }
                if (y1.data()[r * d + j] > y1.data()[r * d + ai])
                {
                    ai = j;
                }
            }
            argmax_mism += af == ai ? 0U : 1U;
        }
        std::printf("[v14m] %s i8-per-tensor vs f32: max abs prob diff = %.3e, argmax mismatches = %llu/%llu\n",
                    name, static_cast<double>(maxd), static_cast<unsigned long long>(argmax_mism),
                    static_cast<unsigned long long>(rows));
        REQUIRE(maxd <= 2e-2F);
        REQUIRE(argmax_mism == 0U);
        // the worker moat
        for (const crd::u32 nw : {1U, 2U, 4U, 8U, 16U})
        {
            crd::jobs::Config cfg;
            cfg.num_threads = nw;
            crd::jobs::init(cfg);
            Tensor<crd::f32> y(&alloc, m.y.shape());
            const TensorStatus st = ni.infer(m.x.view(), y.view(), wss, 0U);
            crd::jobs::shutdown();
            REQUIRE(st == TensorStatus::Ok);
            crd::u64 mism = 0;
            for (crd::u64 i = 0; i < y.size(); ++i)
            {
                if (std::bit_cast<crd::u32>(y.data()[i]) != std::bit_cast<crd::u32>(y1.data()[i]))
                {
                    ++mism;
                }
            }
            INFO("workers " << nw);
            REQUIRE(mism == 0U);
        }
    }
}

TEST_CASE("nn: boundary adversaries - batch one, zero input, bad calls rejected", "[v14m][nn]")
{
    crd::memory::TlsfAllocator alloc(kPool);
    LoadedModel m(&alloc);
    REQUIRE(load_model(&alloc, "mlp", m));
    NnSequential net(&alloc);
    REQUIRE(build_mlp_from_safetensors(&alloc, m.st, m.x.shape(0), false, net) == TensorStatus::Ok);
    const crd::u64 wsb[1] = {net.workspace_bytes()};
    Tensor<crd::u8> ws(&alloc, {wsb, 1});
    const crd::containers::Span<crd::u8> wss{ws.data(), static_cast<crd::usize>(ws.size())};

    // batch 1 == row 0 of the full batch, bit-for-bit
    Tensor<crd::f32> yfull(&alloc, m.y.shape());
    REQUIRE(net.infer(m.x.view(), yfull.view(), wss, 1U) == TensorStatus::Ok);
    const TensorView<const crd::f32> x1 = TensorView<const crd::f32>(m.x.view()).slice(0U, 0U, 1U);
    const crd::u64 y1shp[2] = {1U, 10U};
    Tensor<crd::f32> y1(&alloc, {y1shp, 2});
    REQUIRE(net.infer(x1, y1.view(), wss, 1U) == TensorStatus::Ok);
    for (crd::u64 j = 0; j < 10U; ++j)
    {
        REQUIRE(std::bit_cast<crd::u32>(y1.data()[j]) == std::bit_cast<crd::u32>(yfull.data()[j]));
    }

    // zero input: finite probabilities, each row sums to 1
    const crd::u64 xzshp[2] = {2U, 64U};
    Tensor<crd::f32> xz = Tensor<crd::f32>::zeros(&alloc, {xzshp, 2});
    const crd::u64 yzshp[2] = {2U, 10U};
    Tensor<crd::f32> yz(&alloc, {yzshp, 2});
    REQUIRE(net.infer(xz.view(), yz.view(), wss, 1U) == TensorStatus::Ok);
    for (crd::u64 r = 0; r < 2U; ++r)
    {
        crd::f64 s = 0.0;
        for (crd::u64 j = 0; j < 10U; ++j)
        {
            REQUIRE(std::isfinite(yz.data()[r * 10U + j]));
            s += static_cast<crd::f64>(yz.data()[r * 10U + j]);
        }
        REQUIRE(std::fabs(s - 1.0) <= 1e-6);
    }

    // rejected calls: short workspace / wrong shapes / non-contiguous input
    Tensor<crd::f32> ybad(&alloc, m.y.shape());
    REQUIRE(net.infer(m.x.view(), ybad.view(), {ws.data(), 64U}, 1U) == TensorStatus::BadInput);
    const crd::u64 wrong[2] = {16U, 9U};
    Tensor<crd::f32> ywrong(&alloc, {wrong, 2});
    REQUIRE(net.infer(m.x.view(), ywrong.view(), wss, 1U) == TensorStatus::ShapeMismatch);
    const crd::u64 wide[2] = {16U, 128U};
    Tensor<crd::f32> xwide(&alloc, {wide, 2});
    fill_rand(xwide.data(), xwide.size(), 29U, 0U);
    const TensorView<const crd::f32> xstrided =
        TensorView<const crd::f32>(xwide.view()).slice(1U, 0U, 128U, 2U); // [16, 64], stride 2
    REQUIRE(net.infer(xstrided, ybad.view(), wss, 1U) == TensorStatus::NotContiguous);

    // finalize misuse: an empty net has no shape to close over
    NnSequential empty(&alloc);
    const crd::u64 eshp[2] = {1U, 8U};
    REQUIRE(empty.finalize({eshp, 2}) == TensorStatus::BadInput);
}

TEST_CASE("nn: linear f32 dense tier matches the direct tier and holds the moat", "[v14m][nn]")
{
    // batch 8192 pushes fc1 (2*8192*64*128 = 134 Mflop) over kNnDenseLinearFlops
    // onto the hesap-dense GEMM tier: gate tier consistency (<= 1e-6 of the
    // direct tier) + the worker moat + run-twice determinism on the dense path.
    crd::memory::TlsfAllocator alloc(1U << 28);
    LoadedModel m(&alloc);
    REQUIRE(load_model(&alloc, "mlp", m));
    const crd::u64 big = 8192U;
    REQUIRE(2U * big * 64U * 128U >= crd::hesap::tensor::kNnDenseLinearFlops); // the tier is exercised
    NnSequential net(&alloc);
    REQUIRE(build_mlp_from_safetensors(&alloc, m.st, big, false, net) == TensorStatus::Ok);
    const crd::u64 xshp[2] = {big, 64U};
    Tensor<crd::f32> x(&alloc, {xshp, 2});
    fill_rand(x.data(), x.size(), 31U, 0U);
    const crd::u64 wsb[1] = {net.workspace_bytes()};
    Tensor<crd::u8> ws(&alloc, {wsb, 1});
    const crd::containers::Span<crd::u8> wss{ws.data(), static_cast<crd::usize>(ws.size())};
    const crd::u64 yshp[2] = {big, 10U};
    Tensor<crd::f32> y1(&alloc, {yshp, 2});
    Tensor<crd::f32> y2(&alloc, {yshp, 2});
    REQUIRE(net.infer(x.view(), y1.view(), wss, 1U) == TensorStatus::Ok);
    REQUIRE(net.infer(x.view(), y2.view(), wss, 1U) == TensorStatus::Ok);
    REQUIRE(std::memcmp(y1.data(), y2.data(), y1.size() * sizeof(crd::f32)) == 0); // run-twice

    // tier consistency: a small-batch net (direct tier) on the same rows
    NnSequential small(&alloc);
    REQUIRE(build_mlp_from_safetensors(&alloc, m.st, 16U, false, small) == TensorStatus::Ok);
    const crd::u64 sshp[2] = {16U, 64U};
    Tensor<crd::f32> xs(&alloc, {sshp, 2});
    std::memcpy(xs.data(), x.data(), 16U * 64U * sizeof(crd::f32));
    const crd::u64 swsb[1] = {small.workspace_bytes()};
    Tensor<crd::u8> sws(&alloc, {swsb, 1});
    const crd::u64 syshp[2] = {16U, 10U};
    Tensor<crd::f32> ysd(&alloc, {syshp, 2});
    REQUIRE(small.infer(xs.view(), ysd.view(), {sws.data(), static_cast<crd::usize>(sws.size())}, 1U) ==
            TensorStatus::Ok);
    crd::f32 maxd = 0.0F;
    for (crd::u64 i = 0; i < 160U; ++i)
    {
        const crd::f32 dd = std::fabs(ysd.data()[i] - y1.data()[i]);
        maxd = dd > maxd ? dd : maxd;
    }
    std::printf("[v14m] dense-vs-direct linear tier: max abs prob diff = %.3e\n", static_cast<double>(maxd));
    REQUIRE(maxd <= 1e-6F);

    // moat on the dense tier
    for (const crd::u32 nw : {2U, 4U, 8U, 16U})
    {
        crd::jobs::Config cfg;
        cfg.num_threads = nw;
        crd::jobs::init(cfg);
        Tensor<crd::f32> y(&alloc, {yshp, 2});
        const TensorStatus st = net.infer(x.view(), y.view(), wss, 0U);
        crd::jobs::shutdown();
        REQUIRE(st == TensorStatus::Ok);
        crd::u64 mism = 0;
        for (crd::u64 i = 0; i < y.size(); ++i)
        {
            if (std::bit_cast<crd::u32>(y.data()[i]) != std::bit_cast<crd::u32>(y1.data()[i]))
            {
                ++mism;
            }
        }
        INFO("workers " << nw);
        REQUIRE(mism == 0U);
    }
}
