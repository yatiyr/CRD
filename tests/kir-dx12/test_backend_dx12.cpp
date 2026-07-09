// test_backend_dx12.cpp — Phase 3.1.6 v17-d: the CKIR DirectX 12 backend on the GPU. The THIRD bit-exact backend from
// one CKIR IR. Runs elementwise (arith), matmul, and reduce through KirBackendDx12 and gates them BIT-EXACT vs the
// CPU-reference oracle + deterministic. (Division is ULP on D3D12 — same GPU fast reciprocal as Vulkan.) ADR-0098.

#include <crd/kir/backend.hpp>
#include <crd/kir/ckir.hpp>
#include <crd/kir/dx12/backend_dx12.hpp>

#include <crd/containers/array.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstring>

namespace kir = crd::kir;

namespace
{
constexpr int kN = 1024;
void fill(float* v, int n, float base) { for (int i = 0; i < n; ++i) { v[i] = base + 0.013F * static_cast<float>(i) - 0.5F * static_cast<float>(i % 7); } }
} // namespace

TEST_CASE("v17-d: DX12 elementwise (arith) bit-matches the CPU oracle + deterministic", "[kir][dx12][gpu]")
{
    crd::memory::TlsfAllocator alloc(64 << 20);
    kir::KirBackendDx12        dx(&alloc);
    if (!dx.valid()) { WARN("no D3D12 device available; skipping"); return; }
    kir::KirBackendCpu cpu(&alloc);

    kir::KGraph      g(&alloc);
    const kir::Shape sh = kir::make_shape({kN});
    const int        x  = g.input(sh, kir::DType::F32);
    const int        y  = g.input(sh, kir::DType::F32);
    // out = (x + y) * x - y * y + x * x  (Add/Sub/Mul, precise ⇒ bit-exact)
    const int out = g.binary(kir::KOp::Add, g.binary(kir::KOp::Sub, g.binary(kir::KOp::Mul, g.binary(kir::KOp::Add, x, y), x), g.binary(kir::KOp::Mul, y, y)), g.binary(kir::KOp::Mul, x, x));

    float xv[kN];
    float yv[kN];
    fill(xv, kN, 1.0F);
    fill(yv, kN, -0.7F);
    const float* inputs[] = {xv, yv};
    float        gpu_out[kN];
    float        cpu_out[kN];
    float        d2[kN];
    REQUIRE(dx.run(g, out, inputs, 2, gpu_out));
    REQUIRE(cpu.run(g, out, inputs, 2, cpu_out));
    REQUIRE(dx.run(g, out, inputs, 2, d2));
    for (int i = 0; i < kN; ++i)
    {
        CHECK(gpu_out[i] == cpu_out[i]);
        CHECK(gpu_out[i] == d2[i]);
    }
}


TEST_CASE("v17-g: DX12 FUSES GEMM+bias+SiLU into one kernel, correct vs the oracle", "[kir][dx12][gpu]")
{
    crd::memory::TlsfAllocator alloc(64 << 20);
    kir::KirBackendDx12        be(&alloc);
    if (!be.valid()) { WARN("no DX12 device available; skipping"); return; }
    kir::KirBackendCpu cpu(&alloc);

    constexpr int mm = 128;
    constexpr int kk = 96;
    constexpr int nn = 128;
    kir::KGraph   g(&alloc);
    const int     a    = g.input(kir::make_shape({mm, kk}), kir::DType::F32);
    const int     b    = g.input(kir::make_shape({kk, nn}), kir::DType::F32);
    const int     bias = g.input(kir::make_shape({nn}), kir::DType::F32);
    const int     c    = g.contract(a, b);
    const int     bc   = g.broadcast(bias, kir::make_shape({mm, nn}));
    const int     z    = g.binary(kir::KOp::Add, c, bc);
    const int     nz   = g.unary(kir::KOp::Neg, z);
    const int     ez   = g.unary(kir::KOp::Exp, nz);
    const int     one  = g.constant(1.0, kir::make_shape({mm, nn}), kir::DType::F32);
    const int     den  = g.binary(kir::KOp::Add, one, ez);
    const int     sig  = g.unary(kir::KOp::Recip, den);
    const int     out  = g.binary(kir::KOp::Mul, z, sig); // SiLU(A@B + bias)

    crd::containers::Array<float> av(&alloc);
    crd::containers::Array<float> bv(&alloc);
    crd::containers::Array<float> biasv(&alloc);
    crd::containers::Array<float> gpu(&alloc);
    crd::containers::Array<float> cpuo(&alloc);
    av.resize(static_cast<crd::usize>(mm) * kk);
    bv.resize(static_cast<crd::usize>(kk) * nn);
    biasv.resize(static_cast<crd::usize>(nn));
    gpu.resize(static_cast<crd::usize>(mm) * nn);
    cpuo.resize(static_cast<crd::usize>(mm) * nn);
    fill(av.data(), mm * kk, 0.1F);
    fill(bv.data(), kk * nn, -0.05F);
    fill(biasv.data(), nn, 0.2F);
    const float* inputs[] = {av.data(), bv.data(), biasv.data()};
    REQUIRE(be.run(g, out, inputs, 3, gpu.data()));  // -> the fused kernel (ONE dispatch)
    REQUIRE(cpu.run(g, out, inputs, 3, cpuo.data()));
    float maxrel = 0.0F;
    for (int i = 0; i < mm * nn; ++i)
    {
        const float d  = (gpu[i] - cpuo[i]) / (1.0F + (cpuo[i] < 0.0F ? -cpuo[i] : cpuo[i]));
        const float ad = d < 0.0F ? -d : d;
        if (ad > maxrel) { maxrel = ad; }
    }
    CHECK(maxrel < 2e-3F);
}

TEST_CASE("v17-h: DX12 T2 FAST tiled GEMM (FMA, transposed-A) matches the oracle within tolerance + deterministic", "[kir][dx12][gpu]")
{
    crd::memory::TlsfAllocator alloc(64 << 20);
    kir::KirBackendDx12        dx(&alloc);
    if (!dx.valid()) { WARN("no D3D12 device available; skipping"); return; }
    kir::KirBackendCpu cpu(&alloc);

    constexpr int mm = 128; // 64x64x8-tileable => routes to emit_contract_fast_hlsl (the ported crush kernel)
    constexpr int kk = 64;
    constexpr int nn = 128;
    kir::KGraph   g(&alloc);
    const int     a = g.input(kir::make_shape({mm, kk}), kir::DType::F32);
    const int     b = g.input(kir::make_shape({kk, nn}), kir::DType::F32);
    const int     c = g.contract(a, b, kir::DetTier::Fast);
    float         av[mm * kk];
    float         bv[kk * nn];
    fill(av, mm * kk, 0.2F);
    fill(bv, kk * nn, -0.15F);
    const float* inputs[] = {av, bv};
    float        gpu_out[mm * nn];
    float        cpu_out[mm * nn];
    REQUIRE(dx.run(g, c, inputs, 2, gpu_out));
    REQUIRE(cpu.run(g, c, inputs, 2, cpu_out));
    float maxrel = 0.0F; // FMA tier ⇒ relative-tolerance, not bit-exact
    for (int i = 0; i < mm * nn; ++i) { float df = gpu_out[i] - cpu_out[i]; if (df < 0.0F) { df = -df; } float cv = cpu_out[i] < 0.0F ? -cpu_out[i] : cpu_out[i]; float rd = df / (cv + 1e-3F); if (rd > maxrel) { maxrel = rd; } }
    CHECK(maxrel < 1e-4F);
    float d2[mm * nn]; // T2 determinism: run-to-run bit-identical
    REQUIRE(dx.run(g, c, inputs, 2, d2));
    for (int i = 0; i < mm * nn; ++i) { CHECK(gpu_out[i] == d2[i]); }
}

TEST_CASE("v17-d: DX12 matmul + reduce bit-match the CPU oracle", "[kir][dx12][gpu]")
{
    crd::memory::TlsfAllocator alloc(64 << 20);
    kir::KirBackendDx12        dx(&alloc);
    if (!dx.valid()) { WARN("no D3D12 device available; skipping"); return; }
    kir::KirBackendCpu cpu(&alloc);

    SECTION("matmul 32x48 @ 48x24")
    {
        constexpr int mm = 32;
        constexpr int kk = 48;
        constexpr int nn = 24;
        kir::KGraph   g(&alloc);
        const int     a = g.input(kir::make_shape({mm, kk}), kir::DType::F32);
        const int     b = g.input(kir::make_shape({kk, nn}), kir::DType::F32);
        const int     c = g.contract(a, b);
        float         av[mm * kk];
        float         bv[kk * nn];
        fill(av, mm * kk, 0.2F);
        fill(bv, kk * nn, -0.15F);
        const float* inputs[] = {av, bv};
        float        gpu_out[mm * nn];
        float        cpu_out[mm * nn];
        REQUIRE(dx.run(g, c, inputs, 2, gpu_out));
        REQUIRE(cpu.run(g, c, inputs, 2, cpu_out));
        for (int i = 0; i < mm * nn; ++i) { CHECK(gpu_out[i] == cpu_out[i]); }
    }
    SECTION("reduce-sum over rows")
    {
        constexpr int rows = 40;
        constexpr int cols = 96;
        kir::KGraph   g(&alloc);
        const int     a   = g.input(kir::make_shape({rows, cols}), kir::DType::F32);
        const int     red = g.reduce(kir::KOp::ReduceSum, a, 0x2U);
        float         xv[rows * cols];
        fill(xv, rows * cols, 0.25F);
        const float* inputs[] = {xv};
        float        gpu_out[rows];
        float        cpu_out[rows];
        REQUIRE(dx.run(g, red, inputs, 1, gpu_out));
        REQUIRE(cpu.run(g, red, inputs, 1, cpu_out));
        for (int i = 0; i < rows; ++i) { CHECK(gpu_out[i] == cpu_out[i]); }
    }
    SECTION("reduce-min + reduce-prod over rows (bit-exact vs oracle)")
    {
        constexpr int  rows   = 40;
        constexpr int  cols   = 96;
        const kir::KOp ops[2] = {kir::KOp::ReduceMin, kir::KOp::ReduceProd};
        for (int oi = 0; oi < 2; ++oi)
        {
            kir::KGraph g(&alloc);
            const int   a   = g.input(kir::make_shape({rows, cols}), kir::DType::F32);
            const int   red = g.reduce(ops[oi], a, 0x2U);
            float       xv[rows * cols];
            for (int i = 0; i < rows * cols; ++i) { xv[i] = 0.98F + 0.0004F * static_cast<float>(i % 51); }
            const float* inputs[] = {xv};
            float        gpu_out[rows];
            float        cpu_out[rows];
            REQUIRE(dx.run(g, red, inputs, 1, gpu_out));
            REQUIRE(cpu.run(g, red, inputs, 1, cpu_out));
            for (int i = 0; i < rows; ++i) { CHECK(gpu_out[i] == cpu_out[i]); }
        }
    }
}

TEST_CASE("v17-breadth: DX12 floor/ceil/sign/cmpeq/cmple bit-match the CPU oracle", "[kir][dx12][gpu]")
{
    crd::memory::TlsfAllocator alloc(64 << 20);
    kir::KirBackendDx12        be(&alloc);
    if (!be.valid()) { WARN("no DX12 device available; skipping"); return; }
    kir::KirBackendCpu cpu(&alloc);

    constexpr int    nn = 1024;
    kir::KGraph      g(&alloc);
    const kir::Shape sh = kir::make_shape({nn});
    const int        x  = g.input(sh, kir::DType::F32);
    const int        y  = g.input(sh, kir::DType::F32);
    const int uf  = g.unary(kir::KOp::Floor, x);
    const int uc  = g.unary(kir::KOp::Ceil, x);
    const int us  = g.unary(kir::KOp::Sign, y);
    const int ut  = g.unary(kir::KOp::Trunc, y);
    const int bce = g.binary(kir::KOp::CmpEq, x, y);
    const int bcl = g.binary(kir::KOp::CmpLe, x, y);
    const int out = g.binary(kir::KOp::Add, g.binary(kir::KOp::Add, g.binary(kir::KOp::Add, g.binary(kir::KOp::Add, uf, uc), g.binary(kir::KOp::Add, us, bce)), bcl), ut);

    float xv[nn];
    float yv[nn];
    for (int i = 0; i < nn; ++i)
    {
        xv[i] = -3.5F + 0.017F * static_cast<float>(i % 401);
        yv[i] = (i % 5 == 0) ? xv[i] : (xv[i] + 0.5F * static_cast<float>((i % 3) - 1));
    }
    const float* inputs[] = {xv, yv};
    float        gpu_out[nn];
    float        cpu_out[nn];
    REQUIRE(be.run(g, out, inputs, 2, gpu_out));
    REQUIRE(cpu.run(g, out, inputs, 2, cpu_out));
    for (int i = 0; i < nn; ++i) { CHECK(gpu_out[i] == cpu_out[i]); } // floor/ceil/sign/cmp all exact => BIT-EXACT
}

TEST_CASE("v17-breadth: DX12 gather row index-select bit-matches the CPU oracle", "[kir][dx12][gpu]")
{
    crd::memory::TlsfAllocator alloc(64 << 20);
    kir::KirBackendDx12        be(&alloc);
    if (!be.valid()) { WARN("no DX12 device available; skipping"); return; }
    kir::KirBackendCpu cpu(&alloc);

    constexpr int rr = 50; // data rows
    constexpr int cc = 8;  // row width
    constexpr int mm = 20; // gather count
    kir::KGraph   g(&alloc);
    const int     data = g.input(kir::make_shape({rr, cc}), kir::DType::F32);
    const int     idx  = g.input(kir::make_shape({mm}), kir::DType::F32);
    const int     out  = g.gather(data, idx);
    float dv[rr * cc];
    for (int i = 0; i < rr * cc; ++i) { dv[i] = 0.1F * static_cast<float>(i) - 3.0F; }
    float iv[mm];
    for (int i = 0; i < mm; ++i) { iv[i] = static_cast<float>((i * 7 + 3) % rr); }
    const float* inputs[] = {dv, iv};
    float        gpu_out[mm * cc];
    float        cpu_out[mm * cc];
    REQUIRE(be.run(g, out, inputs, 2, gpu_out));
    REQUIRE(cpu.run(g, out, inputs, 2, cpu_out));
    for (int i = 0; i < mm * cc; ++i) { CHECK(gpu_out[i] == cpu_out[i]); } // gather = pure copy => BIT-EXACT
}

TEST_CASE("v17-breadth: DX12 argmax/argmin index bit-matches the CPU oracle", "[kir][dx12][gpu]")
{
    crd::memory::TlsfAllocator alloc(64 << 20);
    kir::KirBackendDx12        be(&alloc);
    if (!be.valid()) { WARN("no DX12 device available; skipping"); return; }
    kir::KirBackendCpu cpu(&alloc);

    constexpr int  rows   = 40;
    constexpr int  cols   = 96;
    const kir::KOp ops[2] = {kir::KOp::ArgMax, kir::KOp::ArgMin};
    for (int oi = 0; oi < 2; ++oi)
    {
        kir::KGraph g(&alloc);
        const int   a   = g.input(kir::make_shape({rows, cols}), kir::DType::F32);
        const int   red = g.reduce(ops[oi], a, 0x2U);
        float       xv[rows * cols];
        for (int i = 0; i < rows * cols; ++i) { xv[i] = static_cast<float>((i * 37) % 91) * 0.1F; } // varied; first-match ties
        const float* inputs[] = {xv};
        float        gpu_out[rows];
        float        cpu_out[rows];
        REQUIRE(be.run(g, red, inputs, 1, gpu_out));
        REQUIRE(cpu.run(g, red, inputs, 1, cpu_out));
        for (int i = 0; i < rows; ++i) { CHECK(gpu_out[i] == cpu_out[i]); } // the extremum INDEX, exact => BIT-EXACT
    }
}

TEST_CASE("v17-breadth: DX12 round ties-to-even bit-matches the CPU oracle", "[kir][dx12][gpu]")
{
    crd::memory::TlsfAllocator alloc(64 << 20);
    kir::KirBackendDx12        be(&alloc);
    if (!be.valid()) { WARN("no DX12 device available; skipping"); return; }
    kir::KirBackendCpu cpu(&alloc);

    constexpr int    nn = 256;
    kir::KGraph      g(&alloc);
    const kir::Shape sh  = kir::make_shape({nn});
    const int        x   = g.input(sh, kir::DType::F32);
    const int        out = g.unary(kir::KOp::Round, x);
    float            xv[nn];
    for (int i = 0; i < nn; ++i) { xv[i] = -8.0F + 0.5F * static_cast<float>(i); } // every value is .0 or .5 => exercises ties-to-even
    const float* inputs[] = {xv};
    float        gpu_out[nn];
    float        cpu_out[nn];
    REQUIRE(be.run(g, out, inputs, 1, gpu_out));
    REQUIRE(cpu.run(g, out, inputs, 1, cpu_out));
    for (int i = 0; i < nn; ++i) { CHECK(gpu_out[i] == cpu_out[i]); } // ties-even == nearbyint => BIT-EXACT
}

TEST_CASE("v17-breadth: DX12 scatter last-wins bit-matches the CPU oracle", "[kir][dx12][gpu]")
{
    crd::memory::TlsfAllocator alloc(64 << 20);
    kir::KirBackendDx12        be(&alloc);
    if (!be.valid()) { WARN("no DX12 device available; skipping"); return; }
    kir::KirBackendCpu cpu(&alloc);

    constexpr int rr = 30; // base rows
    constexpr int cc = 8;  // row width
    constexpr int mm = 20; // scatter count (idx has DUPLICATES -> exercises deterministic last-wins)
    kir::KGraph   g(&alloc);
    const int     base = g.input(kir::make_shape({rr, cc}), kir::DType::F32);
    const int     idx  = g.input(kir::make_shape({mm}), kir::DType::F32);
    const int     upd  = g.input(kir::make_shape({mm, cc}), kir::DType::F32);
    const int     out  = g.scatter(base, idx, upd);
    float bv[rr * cc];
    for (int i = 0; i < rr * cc; ++i) { bv[i] = -1.0F - 0.1F * static_cast<float>(i); }
    float iv[mm];
    for (int i = 0; i < mm; ++i) { iv[i] = static_cast<float>((i * 3) % rr); } // i=0 and i=10 both hit row 0
    float uv[mm * cc];
    for (int i = 0; i < mm * cc; ++i) { uv[i] = 5.0F + 0.25F * static_cast<float>(i); }
    const float* inputs[] = {bv, iv, uv};
    float        gpu_out[rr * cc];
    float        cpu_out[rr * cc];
    REQUIRE(be.run(g, out, inputs, 3, gpu_out));
    REQUIRE(cpu.run(g, out, inputs, 3, cpu_out));
    for (int i = 0; i < rr * cc; ++i) { CHECK(gpu_out[i] == cpu_out[i]); } // last-wins, fixed order => BIT-EXACT
}

TEST_CASE("v17-breadth: DX12 scan prefix-sum bit-matches the CPU oracle", "[kir][dx12][gpu]")
{
    crd::memory::TlsfAllocator alloc(64 << 20);
    kir::KirBackendDx12        be(&alloc);
    if (!be.valid()) { WARN("no DX12 device available; skipping"); return; }
    kir::KirBackendCpu cpu(&alloc);

    constexpr int rows = 32;
    constexpr int cols = 48;
    kir::KGraph   g(&alloc);
    const int     a   = g.input(kir::make_shape({rows, cols}), kir::DType::F32);
    const int     out = g.scan(a); // inclusive prefix-sum along cols
    float         xv[rows * cols];
    for (int i = 0; i < rows * cols; ++i) { xv[i] = static_cast<float>((i % 7) + 1); } // small ints => exact cumulative sums everywhere
    const float* inputs[] = {xv};
    float        gpu_out[rows * cols];
    float        cpu_out[rows * cols];
    REQUIRE(be.run(g, out, inputs, 1, gpu_out));
    REQUIRE(cpu.run(g, out, inputs, 1, cpu_out));
    for (int i = 0; i < rows * cols; ++i) { CHECK(gpu_out[i] == cpu_out[i]); } // exact integer prefix sums => BIT-EXACT
}

TEST_CASE("v17-perf: DX12 T2 fast reduce sum/prod/max/min matches T1 oracle + deterministic", "[kir][dx12][gpu]")
{
    crd::memory::TlsfAllocator alloc(64 << 20);
    kir::KirBackendDx12        be(&alloc);
    if (!be.valid()) { WARN("no DX12 device available; skipping"); return; }
    kir::KirBackendCpu cpu(&alloc);

    constexpr int  rows   = 16;
    constexpr int  cols   = 32; // small so prod stays exact (<= 2^32) under reassociation
    const kir::KOp ops[4] = {kir::KOp::ReduceSum, kir::KOp::ReduceProd, kir::KOp::ReduceMax, kir::KOp::ReduceMin};
    for (int oi = 0; oi < 4; ++oi)
    {
        kir::KGraph g(&alloc);
        const int   a  = g.input(kir::make_shape({rows, cols}), kir::DType::F32);
        const int   r2 = g.reduce(ops[oi], a, 0x2U, kir::DetTier::Fast); // T2 parallel tree-reduce
        float       xv[rows * cols];
        for (int i = 0; i < rows * cols; ++i) { xv[i] = (i % 3 == 0) ? 2.0F : 1.0F; } // {1,2}: sum/prod/max/min all reassociation-exact
        const float* inputs[] = {xv};
        float        g1[rows];
        float        g2[rows];
        float        co[rows];
        REQUIRE(be.run(g, r2, inputs, 1, g1));
        REQUIRE(cpu.run(g, r2, inputs, 1, co)); // T1 fixed-order oracle == exact for these inputs
        REQUIRE(be.run(g, r2, inputs, 1, g2));
        for (int i = 0; i < rows; ++i) { CHECK(g1[i] == co[i]); CHECK(g1[i] == g2[i]); } // T2 correct + run-to-run deterministic
    }
}

TEST_CASE("v17-perf: DX12 T2 fast parallel scan matches T1 oracle + deterministic", "[kir][dx12][gpu]")
{
    crd::memory::TlsfAllocator alloc(64 << 20);
    kir::KirBackendDx12        be(&alloc);
    if (!be.valid()) { WARN("no DX12 device available; skipping"); return; }
    kir::KirBackendCpu cpu(&alloc);

    constexpr int rows = 4;
    constexpr int nlen = 1000; // > 256 => exercises the chunked parallel scan
    kir::KGraph   g(&alloc);
    const int     a  = g.input(kir::make_shape({rows, nlen}), kir::DType::F32);
    const int     s2 = g.scan(a, kir::DetTier::Fast); // T2 parallel prefix-sum
    crd::containers::Array<float> xv(&alloc);
    xv.resize(static_cast<crd::usize>(rows) * nlen);
    for (int i = 0; i < rows * nlen; ++i) { xv[i] = static_cast<float>((i % 4) + 1); } // small ints => order-invariant exact prefix sums
    const float* inputs[] = {xv.data()};
    crd::containers::Array<float> g1(&alloc);
    crd::containers::Array<float> g2(&alloc);
    crd::containers::Array<float> co(&alloc);
    g1.resize(static_cast<crd::usize>(rows) * nlen);
    g2.resize(static_cast<crd::usize>(rows) * nlen);
    co.resize(static_cast<crd::usize>(rows) * nlen);
    REQUIRE(be.run(g, s2, inputs, 1, g1.data()));
    REQUIRE(cpu.run(g, s2, inputs, 1, co.data())); // T1 fixed-order oracle == exact for integer inputs
    REQUIRE(be.run(g, s2, inputs, 1, g2.data()));
    for (int i = 0; i < rows * nlen; ++i) { CHECK(g1[i] == co[i]); CHECK(g1[i] == g2[i]); } // T2 correct + run-to-run deterministic
}

// ── v17-i: MORTON authored in CKIR, running on DX12 (same graph as the Vulkan proof; the HLSL emitter is now dtype-aware)
namespace morton_ckir
{
constexpr int kN = 256;
inline crd::u32 expand_ref(crd::u32 v)
{
    v = (v | (v << 16)) & 0x030000FFU;
    v = (v | (v << 8)) & 0x0300F00FU;
    v = (v | (v << 4)) & 0x030C30C3U;
    v = (v | (v << 2)) & 0x09249249U;
    return v;
}
inline crd::u32 ref(crd::u32 x, crd::u32 y, crd::u32 z) { return (expand_ref(x) << 2) | (expand_ref(y) << 1) | expand_ref(z); }
inline int      expand(kir::KGraph& g, int v, const kir::Shape& sh)
{
    auto konst = [&](crd::i64 c) { return g.constant(static_cast<crd::f64>(c), sh, kir::DType::I32); };
    auto shl   = [&](int a, crd::i64 b) { return g.binary(kir::KOp::Shl, a, konst(b)); };
    auto bor   = [&](int a, int b) { return g.binary(kir::KOp::BitOr, a, b); };
    auto band  = [&](int a, crd::i64 m) { return g.binary(kir::KOp::BitAnd, a, konst(m)); };
    v = band(bor(v, shl(v, 16)), 0x030000FF);
    v = band(bor(v, shl(v, 8)), 0x0300F00F);
    v = band(bor(v, shl(v, 4)), 0x030C30C3);
    v = band(bor(v, shl(v, 2)), 0x09249249);
    return v;
}
inline int build(kir::KGraph& g, const kir::Shape& sh)
{
    const int x = g.input(sh, kir::DType::F32);
    const int y = g.input(sh, kir::DType::F32);
    const int z = g.input(sh, kir::DType::F32);
    auto      konstf = [&](crd::f64 c) { return g.constant(c, sh, kir::DType::F32); };
    auto      quant  = [&](int c) {
        int s = g.binary(kir::KOp::Mul, c, konstf(1024.0));
        s     = g.binary(kir::KOp::Max, s, konstf(0.0));
        s     = g.binary(kir::KOp::Min, s, konstf(1023.0));
        s     = g.unary(kir::KOp::Floor, s);
        return g.cast(s, kir::DType::I32);
    };
    const int ex     = expand(g, quant(x), sh);
    const int ey     = expand(g, quant(y), sh);
    const int ez     = expand(g, quant(z), sh);
    auto      konsti = [&](crd::i64 c) { return g.constant(static_cast<crd::f64>(c), sh, kir::DType::I32); };
    return g.binary(kir::KOp::BitOr, g.binary(kir::KOp::BitOr, g.binary(kir::KOp::Shl, ex, konsti(2)), g.binary(kir::KOp::Shl, ey, konsti(1))), ez);
}
} // namespace morton_ckir

TEST_CASE("v17-i: Morton authored in CKIR runs on DX12, bit-exact vs the reference", "[kir][dx12][gpu][morton]")
{
    namespace m = morton_ckir;
    crd::memory::TlsfAllocator alloc(16U << 20U);
    kir::KirBackendDx12        dx(&alloc);
    if (!dx.valid()) { WARN("no D3D12 device available; skipping"); return; }

    kir::KGraph      g(&alloc);
    const kir::Shape sh     = kir::make_shape({m::kN});
    const int        morton = m::build(g, sh);

    crd::u32 qx[m::kN];
    crd::u32 qy[m::kN];
    crd::u32 qz[m::kN];
    float    xv[m::kN];
    float    yv[m::kN];
    float    zv[m::kN];
    for (int i = 0; i < m::kN; ++i)
    {
        qx[i] = static_cast<crd::u32>((i * 7) % 1024);
        qy[i] = static_cast<crd::u32>((i * 13) % 1024);
        qz[i] = static_cast<crd::u32>((i * 29) % 1024);
        xv[i] = (static_cast<float>(qx[i]) + 0.5F) / 1024.0F;
        yv[i] = (static_cast<float>(qy[i]) + 0.5F) / 1024.0F;
        zv[i] = (static_cast<float>(qz[i]) + 0.5F) / 1024.0F;
    }
    const float* inputs[] = {xv, yv, zv};
    float        gpu_out[m::kN];
    REQUIRE(dx.run(g, morton, inputs, 3, gpu_out));

    int mism = 0;
    for (int i = 0; i < m::kN; ++i)
    {
        crd::u32 code = 0;
        std::memcpy(&code, &gpu_out[i], 4);
        if (code != m::ref(qx[i], qy[i], qz[i])) { ++mism; }
    }
    CHECK(mism == 0);
}

// ── v17 A3: the comps-aware VEC emitter fanned out to DX12 (the vec corpus runs on the 2nd backend too) ──────────────
TEST_CASE("v17 A3: CKIR vec3 ops (construct/cross/add/normalize) run on DX12 via the comps-aware emitter", "[kir][dx12][gpu][vec]")
{
    crd::memory::TlsfAllocator alloc(16U << 20U);
    kir::KirBackendDx12        dx(&alloc);
    if (!dx.valid()) { WARN("no D3D12 device available; skipping"); return; }

    constexpr int    vn = 128;
    const kir::Shape sh = kir::make_shape({vn});
    kir::KGraph      g(&alloc);
    const int        ax = g.input(sh, kir::DType::F32), ay = g.input(sh, kir::DType::F32), az = g.input(sh, kir::DType::F32);
    const int        bx = g.input(sh, kir::DType::F32), by = g.input(sh, kir::DType::F32), bz = g.input(sh, kir::DType::F32);
    const int        a  = g.vec3(ax, ay, az);
    const int        b  = g.vec3(bx, by, bz);
    const int        o  = g.normalize(g.binary(kir::KOp::Add, g.cross(a, b), a));

    float fin[6][vn];
    for (int i = 0; i < vn; ++i) { fin[0][i] = 0.5F + 0.03F * i; fin[1][i] = 1.0F - 0.02F * i; fin[2][i] = 0.2F + 0.01F * i; fin[3][i] = -0.4F + 0.02F * i; fin[4][i] = 0.7F + 0.015F * i; fin[5][i] = 0.9F - 0.01F * i; }
    const float* inp[] = {fin[0], fin[1], fin[2], fin[3], fin[4], fin[5]};
    float        gpu[vn * 3];
    REQUIRE(dx.run(g, o, inp, 6, gpu));

    int bad = 0;
    for (int i = 0; i < vn; ++i)
    {
        const float avx = fin[0][i], avy = fin[1][i], avz = fin[2][i], bvx = fin[3][i], bvy = fin[4][i], bvz = fin[5][i];
        const float cx = avy * bvz - avz * bvy, cy = avz * bvx - avx * bvz, cz = avx * bvy - avy * bvx;
        const float sx = cx + avx, sy = cy + avy, sz = cz + avz;
        const float len = std::sqrt(sx * sx + sy * sy + sz * sz);
        if (std::fabs(gpu[i * 3] - sx / len) > 1e-4F * std::fabs(sx / len) + 1e-5F) { ++bad; }
        if (std::fabs(gpu[i * 3 + 1] - sy / len) > 1e-4F * std::fabs(sy / len) + 1e-5F) { ++bad; }
        if (std::fabs(gpu[i * 3 + 2] - sz / len) > 1e-4F * std::fabs(sz / len) + 1e-5F) { ++bad; }
    }
    CHECK(bad == 0);
}

TEST_CASE("v17 A4: CKIR unroll_for (fixed-count loop) runs on DX12 (bit-exact, fan-out)", "[kir][dx12][gpu][controlflow]")
{
    crd::memory::TlsfAllocator alloc(16U << 20U);
    kir::KirBackendDx12        dx(&alloc);
    if (!dx.valid()) { WARN("no D3D12 device available; skipping"); return; }
    constexpr int    cn = 256;
    const kir::Shape sh = kir::make_shape({cn});
    kir::KGraph      g(&alloc);
    const int        x = g.input(sh, kir::DType::F32);
    const int        y = g.input(sh, kir::DType::F32);
    const int        r = g.unroll_for(8, x, [&](int i, int acc) { return g.binary(kir::KOp::Add, acc, g.binary(kir::KOp::Mul, g.constant(static_cast<crd::f64>(i), sh, kir::DType::F32), y)); });

    float xv[cn];
    float yv[cn];
    for (int i = 0; i < cn; ++i) { xv[i] = (0.05F * i) - 3.0F; yv[i] = 0.1F + (0.003F * i); }
    const float* inp[] = {xv, yv};
    float        gpu[cn];
    REQUIRE(dx.run(g, r, inp, 2, gpu));

    int mism = 0;
    for (int i = 0; i < cn; ++i) { float acc = xv[i]; for (int it = 0; it < 8; ++it) { acc = acc + (static_cast<float>(it) * yv[i]); } if (gpu[i] != acc) { ++mism; } }
    CHECK(mism == 0);
}

TEST_CASE("v17 A4 tier-2: CKIR dynamic for_loop (native GPU loop, index + divergent count) on DX12", "[kir][dx12][gpu][controlflow]")
{
    crd::memory::TlsfAllocator alloc(16U << 20U);
    kir::KirBackendDx12        dx(&alloc);
    if (!dx.valid()) { WARN("no D3D12 device available; skipping"); return; }
    constexpr int    cn  = 128;
    const kir::Shape sh  = kir::make_shape({cn});
    kir::KGraph      g(&alloc);
    const int        x   = g.input(sh, kir::DType::F32);
    const int        y   = g.input(sh, kir::DType::F32);
    const int        cnt = g.input(sh, kir::DType::F32);
    const int        r   = g.for_loop(cnt, x, [&](int idx, int acc) { return g.binary(kir::KOp::Add, acc, g.binary(kir::KOp::Mul, idx, y)); });

    float xv[cn];
    float yv[cn];
    float cv[cn];
    for (int i = 0; i < cn; ++i) { xv[i] = (0.05F * i) - 3.0F; yv[i] = 0.1F + (0.003F * i); cv[i] = static_cast<float>(i % 8); }
    const float* inp[] = {xv, yv, cv};
    float        gpu[cn];
    REQUIRE(dx.run(g, r, inp, 3, gpu));

    int mism = 0;
    for (int i = 0; i < cn; ++i)
    {
        float     acc = xv[i];
        const int c   = static_cast<int>(cv[i]);
        for (int it = 0; it < c; ++it) { acc = acc + (static_cast<float>(it) * yv[i]); }
        if (gpu[i] != acc) { ++mism; }
    }
    CHECK(mism == 0);
}

TEST_CASE("v17 A3: CKIR mat3 (MatFromCols + mat*vec + inverse) runs on DX12 (HLSL column-major convention)", "[kir][dx12][gpu][mat]")
{
    crd::memory::TlsfAllocator alloc(16U << 20U);
    kir::KirBackendDx12        dx(&alloc);
    if (!dx.valid()) { WARN("no D3D12 device available; skipping"); return; }

    constexpr int    mn = 96;
    const kir::Shape sh = kir::make_shape({mn});
    kir::KGraph      g(&alloc);
    const int        c0 = g.input_vec(sh, kir::DType::F32, 3), c1 = g.input_vec(sh, kir::DType::F32, 3), c2 = g.input_vec(sh, kir::DType::F32, 3);
    const int        vv = g.input_vec(sh, kir::DType::F32, 3);
    const int        M  = g.mat3(c0, c1, c2);
    const int        mv = g.mat_mul_vec(M, vv);
    const int        pv = g.mat_mul_vec(g.mat_mul(M, g.mat_inverse(M)), vv); // (M·M⁻¹)·v ≈ v

    float c0d[mn * 3];
    float c1d[mn * 3];
    float c2d[mn * 3];
    float vd[mn * 3];
    for (int i = 0; i < mn; ++i)
    {
        c0d[i * 3] = 3.0F + 0.01F * i; c0d[i * 3 + 1] = 0.2F; c0d[i * 3 + 2] = 0.1F;
        c1d[i * 3] = 0.1F; c1d[i * 3 + 1] = 4.0F - 0.01F * i; c1d[i * 3 + 2] = 0.2F;
        c2d[i * 3] = 0.2F; c2d[i * 3 + 1] = 0.1F; c2d[i * 3 + 2] = 5.0F + 0.005F * i;
        vd[i * 3] = 0.5F * i - 2.0F; vd[i * 3 + 1] = 1.0F + 0.1F * i; vd[i * 3 + 2] = -0.3F * i + 1.0F;
    }
    const float* inp[] = {c0d, c1d, c2d, vd};
    float        gmv[mn * 3];
    float        gpv[mn * 3];
    REQUIRE(dx.run(g, mv, inp, 4, gmv));
    REQUIRE(dx.run(g, pv, inp, 4, gpv));

    int bad = 0;
    for (int i = 0; i < mn; ++i)
    {
        for (int r = 0; r < 3; ++r)
        {
            const float ref = c0d[i * 3 + r] * vd[i * 3] + c1d[i * 3 + r] * vd[i * 3 + 1] + c2d[i * 3 + r] * vd[i * 3 + 2]; // column-major M·v
            if (std::fabs(gmv[i * 3 + r] - ref) > 1e-4F * std::fabs(ref) + 1e-4F) { ++bad; }
            if (std::fabs(gpv[i * 3 + r] - vd[i * 3 + r]) > 1e-3F * std::fabs(vd[i * 3 + r]) + 1e-3F) { ++bad; }
        }
    }
    CHECK(bad == 0);
}

// ── v17-i rung 2: ATOMIC scatter-add (radix histogram) on DX12 (InterlockedAdd; D3D12 zero-inits the committed output) ─
namespace histo_ckir
{
constexpr int   hn = 1024;
constexpr int   hm = 256;
inline float    asf(crd::i32 v) { float f = 0.0F; std::memcpy(&f, &v, 4); return f; }
inline crd::u32 asu(float f) { crd::u32 u = 0; std::memcpy(&u, &f, 4); return u; }
} // namespace histo_ckir

TEST_CASE("v17-i: CKIR scatter-add histogram runs on DX12 (integer atomics, deterministic)", "[kir][dx12][gpu][atomics]")
{
    namespace h = histo_ckir;
    crd::memory::TlsfAllocator alloc(16U << 20U);
    kir::KirBackendDx12        dx(&alloc);
    if (!dx.valid()) { WARN("no D3D12 device available; skipping"); return; }

    kir::KGraph      g(&alloc);
    const kir::Shape shn  = kir::make_shape({h::hn});
    const kir::Shape shm  = kir::make_shape({h::hm});
    const int        idx  = g.input(shn, kir::DType::I32);
    const int        upd  = g.input(shn, kir::DType::I32);
    const int        hist = g.scatter_add(idx, upd, shm);

    float    idxv[h::hn];
    float    updv[h::hn];
    crd::u32 ref[h::hm];
    for (int i = 0; i < h::hm; ++i) { ref[i] = 0; }
    for (int i = 0; i < h::hn; ++i)
    {
        const crd::i32 d = static_cast<crd::i32>((i * 7 + 13) % h::hm);
        idxv[i]          = h::asf(d);
        updv[i]          = h::asf(1);
        ref[d]++;
    }
    const float* inputs[] = {idxv, updv};
    float        out[h::hm];
    REQUIRE(dx.run(g, hist, inputs, 2, out));

    int mism = 0;
    for (int i = 0; i < h::hm; ++i) { if (h::asu(out[i]) != ref[i]) { ++mism; } }
    CHECK(mism == 0);
}
