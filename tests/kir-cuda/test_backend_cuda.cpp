// test_backend_cuda.cpp — Phase 3.1.6 v17-c: the CKIR CUDA backend on the GPU. Runs elementwise (INCLUDING division —
// CUDA's `--prec-div=true` is correctly-rounded, unlike Vulkan's fast reciprocal), matmul, and reduce kernels through
// KirBackendCuda and gates them BIT-EXACT vs the CPU-reference oracle + deterministic. ADR-0098.

#include <crd/kir/backend.hpp>
#include <crd/kir/ckir.hpp>
#include <crd/kir/ckir_tile.hpp>
#include <crd/kir/cuda/backend_cuda.hpp>

#include <crd/containers/array.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

namespace kir = crd::kir;

namespace
{
constexpr int kN = 1024;
void fill(float* v, int n, float base) { for (int i = 0; i < n; ++i) { v[i] = base + 0.013F * static_cast<float>(i) - 0.5F * static_cast<float>(i % 7); } }
} // namespace

TEST_CASE("v17-c: CUDA elementwise (incl. division) bit-matches the CPU oracle", "[kir][cuda][gpu]")
{
    crd::memory::TlsfAllocator alloc(64 << 20);
    kir::KirBackendCuda        cu(&alloc);
    if (!cu.valid()) { WARN("no CUDA device available; skipping"); return; }
    kir::KirBackendCpu cpu(&alloc);

    kir::KGraph      g(&alloc);
    const kir::Shape sh = kir::make_shape({kN});
    const int        x  = g.input(sh, kir::DType::F32);
    const int        y  = g.input(sh, kir::DType::F32);
    // out = (x + y) * x - y / (1 + |y|)  — CUDA --fmad=false + --prec-div=true ⇒ BIT-EXACT incl. the division
    const int one = g.constant(1.0, sh, kir::DType::F32);
    const int den = g.binary(kir::KOp::Add, one, g.unary(kir::KOp::Abs, y));
    const int out = g.binary(kir::KOp::Sub, g.binary(kir::KOp::Mul, g.binary(kir::KOp::Add, x, y), x), g.binary(kir::KOp::Div, y, den));

    float xv[kN];
    float yv[kN];
    fill(xv, kN, 1.0F);
    fill(yv, kN, -0.7F);
    const float* inputs[] = {xv, yv};
    float        gpu_out[kN];
    float        cpu_out[kN];
    float        d2[kN];
    REQUIRE(cu.run(g, out, inputs, 2, gpu_out));
    REQUIRE(cpu.run(g, out, inputs, 2, cpu_out));
    REQUIRE(cu.run(g, out, inputs, 2, d2));
    for (int i = 0; i < kN; ++i)
    {
        CHECK(gpu_out[i] == cpu_out[i]); // BIT-EXACT incl. division
        CHECK(gpu_out[i] == d2[i]);      // deterministic
    }
}

TEST_CASE("v17-b: CUDA WarpTiled Contract schedule (256^3) matches the oracle (fast tier, FMA)", "[kir][cuda][gpu]")
{
    crd::memory::TlsfAllocator alloc(256 << 20);
    kir::KirBackendCuda        cu(&alloc);
    if (!cu.valid()) { WARN("no CUDA device available; skipping"); return; }
    kir::KirBackendCpu cpu(&alloc);

    constexpr int mm = 256;
    constexpr int kk = 256;
    constexpr int nn = 256;
    kir::KGraph   g(&alloc);
    const int     a = g.input(kir::make_shape({mm, kk}), kir::DType::F32);
    const int     b = g.input(kir::make_shape({kk, nn}), kir::DType::F32);
    const int     c = g.contract(a, b);

    // this shape lowers to the WarpTiled schedule (M,N %128==0, K %8==0, batch 1) — the v17-e crush kernel
    const kir::TileSchedule sch = kir::select_schedule(g, c);
    REQUIRE(sch.kind == kir::Sched::WarpTiled);

    crd::containers::Array<float> av(&alloc);
    crd::containers::Array<float> bv(&alloc);
    crd::containers::Array<float> gpu_out(&alloc);
    crd::containers::Array<float> cpu_out(&alloc);
    av.resize(static_cast<crd::usize>(mm) * kk);
    bv.resize(static_cast<crd::usize>(kk) * nn);
    gpu_out.resize(static_cast<crd::usize>(mm) * nn);
    cpu_out.resize(static_cast<crd::usize>(mm) * nn);
    fill(av.data(), mm * kk, 0.05F);
    fill(bv.data(), kk * nn, -0.03F);
    const float* inputs[] = {av.data(), bv.data()};

    REQUIRE(cu.run(g, c, inputs, 2, gpu_out.data()));  // → the emitted warp-tiled kernel on the GPU
    REQUIRE(cpu.run(g, c, inputs, 2, cpu_out.data())); // the no-FMA CPU oracle
    // fast tier uses FMA (fixed-order deterministic, T1) ⇒ within ULP of the no-FMA oracle over K=256, not bit-exact
    float maxrel = 0.0F;
    for (int i = 0; i < mm * nn; ++i)
    {
        const float d = (gpu_out[i] - cpu_out[i]) / (1.0F + (cpu_out[i] < 0.0F ? -cpu_out[i] : cpu_out[i]));
        const float ad = d < 0.0F ? -d : d;
        if (ad > maxrel) { maxrel = ad; }
    }
    CHECK(maxrel < 1e-3F);
    // determinism: the tiled kernel replays bit-identical run-to-run
    crd::containers::Array<float> d2(&alloc);
    d2.resize(static_cast<crd::usize>(mm) * nn);
    REQUIRE(cu.run(g, c, inputs, 2, d2.data()));
    for (int i = 0; i < mm * nn; ++i) { CHECK(gpu_out[i] == d2[i]); }
}

TEST_CASE("v17-g: CUDA FUSES GEMM+bias+SiLU into one kernel, correct vs the oracle", "[kir][cuda][gpu]")
{
    crd::memory::TlsfAllocator alloc(256 << 20);
    kir::KirBackendCuda        cu(&alloc);
    if (!cu.valid()) { WARN("no CUDA device available; skipping"); return; }
    kir::KirBackendCpu cpu(&alloc);

    constexpr int mm = 256;
    constexpr int kk = 256;
    constexpr int nn = 256;
    kir::KGraph   g(&alloc);
    const int     a    = g.input(kir::make_shape({mm, kk}), kir::DType::F32);
    const int     b    = g.input(kir::make_shape({kk, nn}), kir::DType::F32);
    const int     bias = g.input(kir::make_shape({nn}), kir::DType::F32);
    // out = SiLU(A@B + bias) = z * (1/(1+exp(-z))), z = A@B + bias-broadcast — the LLM-MLP op (SiLU off cublasLt's menu)
    const int c   = g.contract(a, b);
    const int bc  = g.broadcast(bias, kir::make_shape({mm, nn}));
    const int z   = g.binary(kir::KOp::Add, c, bc);
    const int nz  = g.unary(kir::KOp::Neg, z);
    const int ez  = g.unary(kir::KOp::Exp, nz);
    const int one = g.constant(1.0, kir::make_shape({mm, nn}), kir::DType::F32);
    const int den = g.binary(kir::KOp::Add, one, ez);
    const int sig = g.unary(kir::KOp::Recip, den);
    const int out = g.binary(kir::KOp::Mul, z, sig);

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
    fill(av.data(), mm * kk, 0.05F);
    fill(bv.data(), kk * nn, -0.03F);
    fill(biasv.data(), nn, 0.1F);
    const float* inputs[] = {av.data(), bv.data(), biasv.data()};

    REQUIRE(cu.run(g, out, inputs, 3, gpu.data()));   // → the FUSED warp-tiled GEMM+bias+SiLU kernel (ONE launch)
    REQUIRE(cpu.run(g, out, inputs, 3, cpuo.data()));  // the unfused CPU reference (same graph)
    float maxrel = 0.0F;
    for (int i = 0; i < mm * nn; ++i)
    {
        const float d  = (gpu[i] - cpuo[i]) / (1.0F + (cpuo[i] < 0.0F ? -cpuo[i] : cpuo[i]));
        const float ad = d < 0.0F ? -d : d;
        if (ad > maxrel) { maxrel = ad; }
    }
    CHECK(maxrel < 2e-3F);
}

TEST_CASE("v17-c: CUDA matmul + reduce bit-match the CPU oracle", "[kir][cuda][gpu]")
{
    crd::memory::TlsfAllocator alloc(64 << 20);
    kir::KirBackendCuda        cu(&alloc);
    if (!cu.valid()) { WARN("no CUDA device available; skipping"); return; }
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
        REQUIRE(cu.run(g, c, inputs, 2, gpu_out));
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
        REQUIRE(cu.run(g, red, inputs, 1, gpu_out));
        REQUIRE(cpu.run(g, red, inputs, 1, cpu_out));
        for (int i = 0; i < rows; ++i) { CHECK(gpu_out[i] == cpu_out[i]); }
    }
    SECTION("reduce-min + reduce-prod over rows (bit-exact vs oracle)")
    {
        constexpr int    rows   = 40;
        constexpr int    cols   = 96;
        const kir::KOp   ops[2] = {kir::KOp::ReduceMin, kir::KOp::ReduceProd};
        for (int oi = 0; oi < 2; ++oi)
        {
            kir::KGraph g(&alloc);
            const int   a   = g.input(kir::make_shape({rows, cols}), kir::DType::F32);
            const int   red = g.reduce(ops[oi], a, 0x2U);
            float       xv[rows * cols];
            for (int i = 0; i < rows * cols; ++i) { xv[i] = 0.98F + 0.0004F * static_cast<float>(i % 51); } // near 1.0: prod finite, min distinct
            const float* inputs[] = {xv};
            float        gpu_out[rows];
            float        cpu_out[rows];
            REQUIRE(cu.run(g, red, inputs, 1, gpu_out));
            REQUIRE(cpu.run(g, red, inputs, 1, cpu_out));
            for (int i = 0; i < rows; ++i) { CHECK(gpu_out[i] == cpu_out[i]); }
        }
    }
}

TEST_CASE("v17-breadth: CUDA floor/ceil/sign/cmpeq/cmple bit-match the CPU oracle", "[kir][cuda][gpu]")
{
    crd::memory::TlsfAllocator alloc(64 << 20);
    kir::KirBackendCuda        cu(&alloc);
    if (!cu.valid()) { WARN("no CUDA device available; skipping"); return; }
    kir::KirBackendCpu cpu(&alloc);

    kir::KGraph      g(&alloc);
    const kir::Shape sh = kir::make_shape({kN});
    const int        x  = g.input(sh, kir::DType::F32);
    const int        y  = g.input(sh, kir::DType::F32);
    // one fused elementwise cone exercising all five new ops: floor(x)+ceil(x)+sign(y)+(x==y)+(x<=y)
    const int uf  = g.unary(kir::KOp::Floor, x);
    const int uc  = g.unary(kir::KOp::Ceil, x);
    const int us  = g.unary(kir::KOp::Sign, y);
    const int ut  = g.unary(kir::KOp::Trunc, y);
    const int be  = g.cast(g.binary(kir::KOp::CmpEq, x, y), kir::DType::F32); // B0-3: bool -> float before arithmetic
    const int bl  = g.cast(g.binary(kir::KOp::CmpLe, x, y), kir::DType::F32);
    const int out = g.binary(kir::KOp::Add, g.binary(kir::KOp::Add, g.binary(kir::KOp::Add, g.binary(kir::KOp::Add, uf, uc), g.binary(kir::KOp::Add, us, be)), bl), ut);

    float xv[kN];
    float yv[kN];
    for (int i = 0; i < kN; ++i)
    {
        xv[i] = -3.5F + 0.017F * static_cast<float>(i % 401);              // spans negatives, non-integers
        yv[i] = (i % 5 == 0) ? xv[i] : (xv[i] + 0.5F * static_cast<float>((i % 3) - 1)); // some equal → exercises ==
    }
    const float* inputs[] = {xv, yv};
    float        gpu_out[kN];
    float        cpu_out[kN];
    REQUIRE(cu.run(g, out, inputs, 2, gpu_out));
    REQUIRE(cpu.run(g, out, inputs, 2, cpu_out));
    for (int i = 0; i < kN; ++i) { CHECK(gpu_out[i] == cpu_out[i]); } // floor/ceil/sign/cmp are all exact ⇒ BIT-EXACT
}

TEST_CASE("v17-breadth: CUDA gather row index-select bit-matches the CPU oracle", "[kir][cuda][gpu]")
{
    crd::memory::TlsfAllocator alloc(64 << 20);
    kir::KirBackendCuda        be(&alloc);
    if (!be.valid()) { WARN("no CUDA device available; skipping"); return; }
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

TEST_CASE("v17-breadth: CUDA argmax/argmin index bit-matches the CPU oracle", "[kir][cuda][gpu]")
{
    crd::memory::TlsfAllocator alloc(64 << 20);
    kir::KirBackendCuda        be(&alloc);
    if (!be.valid()) { WARN("no CUDA device available; skipping"); return; }
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

TEST_CASE("v17-breadth: CUDA round ties-to-even bit-matches the CPU oracle", "[kir][cuda][gpu]")
{
    crd::memory::TlsfAllocator alloc(64 << 20);
    kir::KirBackendCuda        be(&alloc);
    if (!be.valid()) { WARN("no CUDA device available; skipping"); return; }
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

TEST_CASE("v17-breadth: CUDA scatter last-wins bit-matches the CPU oracle", "[kir][cuda][gpu]")
{
    crd::memory::TlsfAllocator alloc(64 << 20);
    kir::KirBackendCuda        be(&alloc);
    if (!be.valid()) { WARN("no CUDA device available; skipping"); return; }
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

TEST_CASE("v17-breadth: CUDA scan prefix-sum bit-matches the CPU oracle", "[kir][cuda][gpu]")
{
    crd::memory::TlsfAllocator alloc(64 << 20);
    kir::KirBackendCuda        be(&alloc);
    if (!be.valid()) { WARN("no CUDA device available; skipping"); return; }
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

TEST_CASE("v17-perf: CUDA T2 fast reduce sum/prod/max/min matches T1 oracle + deterministic", "[kir][cuda][gpu]")
{
    crd::memory::TlsfAllocator alloc(64 << 20);
    kir::KirBackendCuda        be(&alloc);
    if (!be.valid()) { WARN("no CUDA device available; skipping"); return; }
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

TEST_CASE("v17-perf: CUDA T2 fast parallel scan matches T1 oracle + deterministic", "[kir][cuda][gpu]")
{
    crd::memory::TlsfAllocator alloc(64 << 20);
    kir::KirBackendCuda        be(&alloc);
    if (!be.valid()) { WARN("no CUDA device available; skipping"); return; }
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

// ── B0 fan-out (2026-07-10): the type layer on CUDA, by SCALARIZATION ────────────────────────────────────────────────
// CUDA has NO native vector arithmetic (`float3` carries no operators) and no matrix type, so a value of `comps`
// components becomes `comps` scalar temps and every op is emitted componentwise. The backend compiles with
// `--fmad=false --prec-div=true --prec-sqrt=true`, so no FMA contraction occurs.
//
// ★ These assert BIT-EXACTNESS (`==`) against the CPU oracle — the strongest gate any backend has. It is possible
// because (a) `ckir_eval` now rounds EVERY elementary IEEE op to the node dtype (the 2026-07-10 f32-faithfulness fix;
// before it, the oracle accumulated vec/mat ops in f64 and was ~1 ULP *more accurate* than any f32 kernel), and
// (b) the scalarized CUDA emitter writes those same elementary ops, in the same order, with `--fmad=false`.
// GLSL/HLSL/WGSL cannot yet be gated this way: their `dot()`/`normalize()`/`inverse()` builtins have
// implementation-defined internal order, so those suites stay ULP-tolerant until ADR-0098 §5's `float_controls` audit.
TEST_CASE("v17 B0 fan-out: CUDA vec3/mat3 value layer is BIT-EXACT vs the CPU oracle (scalarized)", "[kir][cuda][gpu][typelayer]")
{
    crd::memory::TlsfAllocator alloc(32U << 20U);
    kir::KirBackendCuda        be(&alloc);
    if (!be.valid()) { WARN("no CUDA device available; skipping"); return; }
    kir::KirBackendCpu cpu(&alloc);

    constexpr int    vn = 128;
    const kir::Shape sh = kir::make_shape({vn});

    { // vec3: cross + add + normalize -- normalize divides by the length, exactly as the oracle does
        kir::KGraph g(&alloc);
        const int   a = g.input_vec(sh, kir::DType::F32, 3);
        const int   b = g.input_vec(sh, kir::DType::F32, 3);
        const int   o = g.normalize(g.binary(kir::KOp::Add, g.cross(a, b), a));

        float av[vn * 3];
        float bv[vn * 3];
        for (int i = 0; i < vn; ++i)
        {
            const float fi = static_cast<float>(i);
            av[i * 3] = 0.5F + 0.03F * fi;  av[i * 3 + 1] = 1.0F - 0.02F * fi;  av[i * 3 + 2] = 0.2F + 0.01F * fi;
            bv[i * 3] = -0.4F + 0.02F * fi; bv[i * 3 + 1] = 0.7F + 0.015F * fi; bv[i * 3 + 2] = 0.9F - 0.01F * fi;
        }
        const float* inputs[] = {av, bv};
        float        gpu[vn * 3];
        float        ref[vn * 3];
        REQUIRE(be.run(g, o, inputs, 2, gpu));
        REQUIRE(cpu.run(g, o, inputs, 2, ref));
        // cross + add + normalize: sums of products, a sqrt and a divide — every one now rounded per step by the oracle
        for (int i = 0; i < vn * 3; ++i) { CHECK(gpu[i] == ref[i]); }
    }

    { // mat3 from columns + mat*vec: an accumulating op, and now bit-exact (oracle rounds each multiply-add)
        kir::KGraph g(&alloc);
        const int   c0  = g.input_vec(sh, kir::DType::F32, 3);
        const int   c1  = g.input_vec(sh, kir::DType::F32, 3);
        const int   c2  = g.input_vec(sh, kir::DType::F32, 3);
        const int   vv  = g.input_vec(sh, kir::DType::F32, 3);
        const int   mv  = g.mat_mul_vec(g.mat3(c0, c1, c2), vv);

        float c0d[vn * 3];
        float c1d[vn * 3];
        float c2d[vn * 3];
        float vd[vn * 3];
        for (int i = 0; i < vn; ++i)
        {
            const float fi = static_cast<float>(i);
            c0d[i * 3] = 3.0F + 0.01F * fi; c0d[i * 3 + 1] = 0.2F;              c0d[i * 3 + 2] = 0.1F;
            c1d[i * 3] = 0.1F;              c1d[i * 3 + 1] = 4.0F - 0.01F * fi; c1d[i * 3 + 2] = 0.2F;
            c2d[i * 3] = 0.2F;              c2d[i * 3 + 1] = 0.1F;              c2d[i * 3 + 2] = 5.0F + 0.005F * fi;
            vd[i * 3]  = 0.5F * fi - 2.0F;  vd[i * 3 + 1]  = 1.0F + 0.1F * fi;  vd[i * 3 + 2]  = -0.3F * fi + 1.0F;
        }
        const float* inputs[] = {c0d, c1d, c2d, vd};
        float        gpu[vn * 3];
        float        ref[vn * 3];
        REQUIRE(be.run(g, mv, inputs, 4, gpu));
        REQUIRE(cpu.run(g, mv, inputs, 4, ref));
        for (int i = 0; i < vn * 3; ++i) { CHECK(gpu[i] == ref[i]); } // ascending-order accumulation, rounded per step
    }

    { // (M * inverse(M)) * v ~= v  -- the emitted cofactor inverse mirrors ckir_eval's minor/sign ordering
        kir::KGraph g(&alloc);
        const int   c0  = g.input_vec(sh, kir::DType::F32, 3);
        const int   c1  = g.input_vec(sh, kir::DType::F32, 3);
        const int   c2  = g.input_vec(sh, kir::DType::F32, 3);
        const int   vv  = g.input_vec(sh, kir::DType::F32, 3);
        const int   mat = g.mat3(c0, c1, c2);
        const int   pv  = g.mat_mul_vec(g.mat_mul(mat, g.mat_inverse(mat)), vv);

        float c0d[vn * 3];
        float c1d[vn * 3];
        float c2d[vn * 3];
        float vd[vn * 3];
        for (int i = 0; i < vn; ++i)
        {
            const float fi = static_cast<float>(i);
            c0d[i * 3] = 3.0F + 0.01F * fi; c0d[i * 3 + 1] = 0.2F;              c0d[i * 3 + 2] = 0.1F;
            c1d[i * 3] = 0.1F;              c1d[i * 3 + 1] = 4.0F - 0.01F * fi; c1d[i * 3 + 2] = 0.2F;
            c2d[i * 3] = 0.2F;              c2d[i * 3 + 1] = 0.1F;              c2d[i * 3 + 2] = 5.0F + 0.005F * fi;
            vd[i * 3]  = 0.5F * fi - 2.0F;  vd[i * 3 + 1]  = 1.0F + 0.1F * fi;  vd[i * 3 + 2]  = -0.3F * fi + 1.0F;
        }
        const float* inputs[] = {c0d, c1d, c2d, vd};
        float        gpu[vn * 3];
        REQUIRE(be.run(g, pv, inputs, 4, gpu));
        int bad = 0;
        for (int i = 0; i < vn * 3; ++i) { const float d = gpu[i] - vd[i]; const float ad = d < 0.0F ? -d : d; const float av = vd[i] < 0.0F ? -vd[i] : vd[i]; if (ad > 1e-3F * av + 1e-3F) { ++bad; } }
        CHECK(bad == 0);
    }
}

// bvec + struct SROA on CUDA. Scalarization makes aggregates free: a FieldGet resolves a component index back to its
// producing scalar at emit time, so nothing is materialized and nothing is copied.
TEST_CASE("v17 B0 fan-out: CUDA bvec3 (any/all) + Light-struct SROA vs the CPU oracle", "[kir][cuda][gpu][typelayer]")
{
    crd::memory::TlsfAllocator alloc(32U << 20U);
    kir::KirBackendCuda        be(&alloc);
    if (!be.valid()) { WARN("no CUDA device available; skipping"); return; }
    kir::KirBackendCpu cpu(&alloc);

    constexpr int    bn = 128;
    const kir::Shape sh = kir::make_shape({bn});

    {
        kir::KGraph g(&alloc);
        const int   av = g.input_vec(sh, kir::DType::F32, 3);
        const int   bv = g.input_vec(sh, kir::DType::F32, 3);
        const int   lt = g.binary(kir::KOp::CmpLt, av, bv); // per-component bool temps
        const int   an = g.vany(lt);
        const int   al = g.vall(lt);
        REQUIRE(g.node(lt).type.scalar == kir::DType::Bool);

        float avd[bn * 3];
        float bvd[bn * 3];
        for (int i = 0; i < bn; ++i)
        {
            const float fi = static_cast<float>(i);
            avd[i * 3] = fi - 64.0F; avd[i * 3 + 1] = 1.0F; avd[i * 3 + 2] = (i % 2 == 0) ? -1.0F : 5.0F;
            bvd[i * 3] = 0.0F;       bvd[i * 3 + 1] = 2.0F; bvd[i * 3 + 2] = 0.0F;
        }
        const float* inputs[] = {avd, bvd};
        float        gan[bn];
        float        gal[bn];
        float        ran[bn];
        float        ral[bn];
        REQUIRE(be.run(g, an, inputs, 2, gan));
        REQUIRE(be.run(g, al, inputs, 2, gal));
        REQUIRE(cpu.run(g, an, inputs, 2, ran));
        REQUIRE(cpu.run(g, al, inputs, 2, ral));
        for (int i = 0; i < bn; ++i) { CHECK(gan[i] == ran[i]); CHECK(gal[i] == ral[i]); } // bool read-back is exact
    }

    { // struct Light { vec3 pos; float radius; vec3 color; } -> destructure -> color*radius + pos
        kir::KGraph      g(&alloc);
        const kir::KType fields[3] = {kir::KType::vec(kir::DType::F32, 3), kir::KType::make_scalar(kir::DType::F32),
                                      kir::KType::vec(kir::DType::F32, 3)};
        const int        light   = g.define_struct(fields, 3);
        const int        pos     = g.input_vec(sh, kir::DType::F32, 3);
        const int        rad     = g.input(sh, kir::DType::F32);
        const int        col     = g.input_vec(sh, kir::DType::F32, 3);
        const int        flds[3] = {pos, rad, col};
        const int        lite    = g.struct_make(light, flds, 3);
        const int        out = g.binary(kir::KOp::Add, g.binary(kir::KOp::Mul, g.field_get(lite, 2), g.splat(g.field_get(lite, 1), 3)), g.field_get(lite, 0));

        float posd[bn * 3];
        float radd[bn];
        float cold[bn * 3];
        for (int i = 0; i < bn; ++i)
        {
            const float fi = static_cast<float>(i);
            posd[i * 3] = fi * 0.5F - 8.0F; posd[i * 3 + 1] = 1.0F - 0.1F * fi; posd[i * 3 + 2] = 0.25F * fi;
            radd[i]     = 0.5F + 0.03F * fi;
            cold[i * 3] = 0.1F * fi;        cold[i * 3 + 1] = 2.0F;             cold[i * 3 + 2] = -0.4F * fi;
        }
        const float* inputs[] = {posd, radd, cold};
        float        gout[bn * 3];
        float        rout[bn * 3];
        REQUIRE(be.run(g, out, inputs, 3, gout));
        REQUIRE(cpu.run(g, out, inputs, 3, rout));
        for (int i = 0; i < bn * 3; ++i) { CHECK(gout[i] == rout[i]); } // bit-exact
    }
}
