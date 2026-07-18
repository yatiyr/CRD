// test_ckir_kernel_emit.cpp — B-cmp Phase 0: the CUDA / MSL / WGSL emitters for the imperative compute-kernel IR (shared
// memory + barriers). CUDA/Metal/WebGPU have no compiler on this host, so — exactly like `test_ckir_msl.cpp` for the
// elementwise path — these are STRUCTURAL gates: the emitter must PRODUCE well-formed source (right kernel signature,
// storage buffers, shared arrays, barrier, the reverse index expression, and WGSL's reversed `select` operand order). The
// compile + bit-exact run on real hardware is ADR-0098 Part C; Vulkan + DX12 already run bit-exact (tests/gpu-context-*).

#include <crd/kir/ckir.hpp>
#include <crd/kir/ckir_clouds.hpp> // B15-b: the Perlin-Worley cloud density (worley3_loop) — cross-backend emit gate
#include <crd/kir/ckir_kernel_eval.hpp> // eval_cpu_kernel + KernelBuffer (the CPU oracle)
#include <crd/kir/ckir_cuda.hpp>
#include <crd/kir/ckir_fft.hpp>  // the [.emit-fft-cuda] ncu-harness generator
#include <crd/kir/ckir_glsl.hpp> // GlslKernel carrier + GLSL emitter
#include <crd/kir/ckir_hlsl.hpp>
#include <crd/kir/ckir_msl.hpp>
#include <crd/kir/ckir_ocean.hpp> // B16-a: the FFT-ocean kernels — cross-backend emit gate
#include <crd/kir/ckir_wgsl.hpp>

#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <cmath>
#include <cstring>

#include <catch2/catch_test_macros.hpp>

namespace kir = crd::kir;

namespace
{
bool has(const kir::GlslKernel& k, const char* needle) { return std::strstr(k.source.c_str(), needle) != nullptr; }

// The shared-memory REVERSE kernel (F32): shared[lid]=in[lid]; barrier; out[lid]=shared[ls-1-lid]. Same graph the Vulkan +
// DX12 dispatch tests run bit-exact; here it drives the three off-host emitters structurally.
kir::KEntry build_reverse(kir::KGraph& g, int ls)
{
    const kir::Shape sh1    = kir::make_shape({1});
    const int        inbuf  = g.buffer_decl(kir::DType::F32, 0, 0, false);
    const int        outbuf = g.buffer_decl(kir::DType::F32, 0, 1, true);
    const int        smem   = g.shared_decl(kir::DType::F32, ls);
    const int        lid    = g.builtin(kir::KBuiltin::LocalInvocationIndex);
    const int        mark   = g.kernel_stmt_mark();
    g.stmt_shared_store(smem, lid, g.buffer_load(inbuf, lid));
    g.stmt_barrier();
    const int revidx = g.binary(kir::KOp::Sub, g.constant(static_cast<crd::f64>(ls - 1), sh1, kir::DType::U32), lid);
    g.stmt_buffer_store(outbuf, lid, g.shared_load(smem, revidx));

    kir::KEntry e;
    e.stage             = kir::KStage::Compute;
    e.local_size[0]     = static_cast<crd::u32>(ls);
    e.kernel_body_begin = mark;
    e.kernel_body_count = g.stmt_count() - mark;
    return e;
}

// B16-a-0: a COMPUTE kernel exercising the transcendentals the ocean spectrum needs — `log`/`log2`/`tanh`/`atan2`/`atan`/`asin`/
// `acos`/`sinh`/`cosh` — which the compute-kernel emitters lacked (only sqrt/sin/cos/exp/pow/floor were wired; the full set lived
// only in the raster value emitter). One thread per element: out = Σ of the 9 ops applied to in∈[0,1] (domains kept legal —
// asin/acos take x·½). Same graph the oracle + Vulkan run bit-exact-to-ULP; here it drives all five emitters structurally.
kir::KEntry build_transcendental_probe(kir::KGraph& g, int ls)
{
    const kir::Shape sh1    = kir::make_shape({1});
    const int        inbuf  = g.buffer_decl(kir::DType::F32, 0, 0, false);
    const int        outbuf = g.buffer_decl(kir::DType::F32, 0, 1, true);
    const int        mark   = g.kernel_stmt_mark();
    const auto       ku     = [&](crd::u32 v) { return g.constant(static_cast<crd::f64>(v), sh1, kir::DType::U32); };
    const auto       kf     = [&](crd::f64 v) { return g.constant(v, sh1, kir::DType::F32); };
    const auto       un     = [&](kir::KOp op, int a) { return g.unary(op, a); };
    const auto       bi     = [&](kir::KOp op, int a, int b) { return g.binary(op, a, b); };
    const int        gid    = bi(kir::KOp::Add, bi(kir::KOp::Mul, g.builtin(kir::KBuiltin::WorkgroupIndex), ku(static_cast<crd::u32>(ls))), g.builtin(kir::KBuiltin::LocalInvocationIndex));
    const int        x      = g.buffer_load(inbuf, gid);
    const int        xh     = bi(kir::KOp::Mul, x, kf(0.5)); // ∈[0,0.5] ⊂ [-1,1] for asin/acos
    int              acc    = un(kir::KOp::Log, bi(kir::KOp::Add, x, kf(1.0)));
    acc                     = bi(kir::KOp::Add, acc, un(kir::KOp::Log2, bi(kir::KOp::Add, x, kf(2.0))));
    acc                     = bi(kir::KOp::Add, acc, un(kir::KOp::Tanh, x));
    acc                     = bi(kir::KOp::Add, acc, bi(kir::KOp::Atan2, x, kf(0.5)));
    acc                     = bi(kir::KOp::Add, acc, un(kir::KOp::Atan, x));
    acc                     = bi(kir::KOp::Add, acc, un(kir::KOp::Asin, xh));
    acc                     = bi(kir::KOp::Add, acc, un(kir::KOp::Acos, xh));
    acc                     = bi(kir::KOp::Add, acc, un(kir::KOp::Sinh, x));
    acc                     = bi(kir::KOp::Add, acc, un(kir::KOp::Cosh, x));
    g.stmt_buffer_store(outbuf, gid, acc);

    kir::KEntry e;
    e.stage             = kir::KStage::Compute;
    e.local_size[0]     = static_cast<crd::u32>(ls);
    e.kernel_body_begin = mark;
    e.kernel_body_count = g.stmt_count() - mark;
    return e;
}
} // namespace

// B16-a-0: the full transcendental set (log/log2/tanh/atan2/atan/asin/acos/sinh/cosh) must EMIT on ALL FIVE compute backends —
// they were only in the raster emitter. The oracle's apply_unary/apply_binary already handle them (verified in [ckir]); Vulkan
// runs it bit-exact-to-ULP (tests/gpu-context-vulkan). This is the structural cross-backend proof they lower everywhere.
TEST_CASE("B16-a-0: compute transcendentals emit on ALL backends", "[kir][kernel][ocean]")
{
    crd::memory::TlsfAllocator alloc(8U << 20U);
    kir::KGraph                g(&alloc);
    const kir::KEntry          e = build_transcendental_probe(g, 64);
    kir::GlslKernel            kg(&alloc);
    kir::GlslKernel            kh(&alloc);
    kir::GlslKernel            kc(&alloc);
    kir::GlslKernel            km(&alloc);
    kir::GlslKernel            kw(&alloc);
    REQUIRE(kir::emit_compute_kernel_glsl(g, e, &alloc, kg));
    REQUIRE(kir::emit_compute_kernel_hlsl(g, e, &alloc, kh));
    REQUIRE(kir::emit_compute_kernel_cuda(g, e, &alloc, kc));
    REQUIRE(kir::emit_compute_kernel_msl(g, e, &alloc, km));
    REQUIRE(kir::emit_compute_kernel_wgsl(g, e, &alloc, kw));
    CHECK(has(kg, "atan(")); CHECK(has(kg, "tanh(")); CHECK(has(kg, "log2(")); CHECK(has(kg, "asin(")); CHECK(has(kg, "sinh("));
    CHECK(has(kh, "atan2(")); CHECK(has(kc, "atan2f(")); CHECK(has(kc, "log2f(")); CHECK(has(kw, "atan2("));

    // CPU-oracle sanity: the kernel value at a known input matches a direct std computation (proves the oracle path).
    crd::containers::Array<crd::f64> in(&alloc);
    crd::containers::Array<crd::f64> out(&alloc);
    in.resize(64, 0.0);
    out.resize(64, 0.0);
    for (int i = 0; i < 64; ++i) { in[static_cast<crd::usize>(i)] = 0.25 + 0.01 * static_cast<double>(i); }
    kir::KernelBuffer bufs[2] = {{in.data(), 64, 0, 0}, {out.data(), 64, 0, 1}};
    kir::eval_cpu_kernel(g, e, bufs, 2, e.local_size[0], &alloc, 1U);
    const double xv  = in[3];
    const double ref = std::log(xv + 1.0) + std::log2(xv + 2.0) + std::tanh(xv) + std::atan2(xv, 0.5) + std::atan(xv)
                       + std::asin(xv * 0.5) + std::acos(xv * 0.5) + std::sinh(xv) + std::cosh(xv);
    CHECK(std::fabs(static_cast<double>(out[3]) - ref) < 1e-4);
}

// B15-b: the gold Perlin-Worley cloud DENSITY must EMIT on ALL FIVE backends. `worley3_loop` composes a runtime `stmt_for` over
// the 27 Worley cells + a per-thread shared min accumulator + the scalar Burtle-Jenkins hash — a NEW compute construct, and the
// GLSL emitter first rejected the vector form (the statement path is scalar-only ⇒ scalarize). This gate proves every backend
// emitter ACCEPTS it (returns true — an unhandled op returns false); Vulkan additionally runs it BIT-EXACT (tests/gpu-context-
// vulkan). It also guards the CSE that makes it possible: the density is a DEEP shared value DAG (a perlin FBM + the hash), and
// the CUDA/MSL/WGSL emitters used to INLINE-EXPAND values with no temp cache ⇒ exponential blow-up (OOM). They now carry the same
// node-id materialization pass as GLSL/HLSL (debt `ckir-offhost-emitter-cse`, RESOLVED 2026-07-15), so all five emit compactly.
TEST_CASE("B15-b: Perlin-Worley cloud density emits on ALL backends (worley3_loop portability)", "[kir][kernel][clouds]")
{
    crd::memory::TlsfAllocator alloc(64U << 20U);
    kir::KGraph                g(&alloc);
    const crd::kir::clouds::CloudConfig cfg;
    const kir::KEntry          e = crd::kir::clouds::build_cloud_density(g, cfg);
    kir::GlslKernel            kg(&alloc);
    kir::GlslKernel            kh(&alloc);
    kir::GlslKernel            kc(&alloc);
    kir::GlslKernel            km(&alloc);
    kir::GlslKernel            kw(&alloc);
    REQUIRE(kir::emit_compute_kernel_glsl(g, e, &alloc, kg)); // Vulkan — GPU-verified bit-exact in tests/gpu-context-vulkan
    REQUIRE(kir::emit_compute_kernel_hlsl(g, e, &alloc, kh)); // DX12
    REQUIRE(kir::emit_compute_kernel_cuda(g, e, &alloc, kc));
    REQUIRE(kir::emit_compute_kernel_msl(g, e, &alloc, km));
    REQUIRE(kir::emit_compute_kernel_wgsl(g, e, &alloc, kw));
    CHECK(has(kg, "for (uint lv"));      // the 27-cell runtime loop (worley3_loop), NOT a 27x unroll
    CHECK(has(kh, "for (uint lv"));
    CHECK(has(kc, "for (unsigned lv"));  // CUDA loop
    CHECK(has(km, "for (uint lv"));      // MSL loop
    CHECK(has(kw, "for (var lv"));       // WGSL loop
}

// B16-a-2: the FFT-ocean kernels must EMIT on ALL FIVE compute backends — the same "fully CKIR, fanned out everywhere" proof
// as B16-a-0 (transcendentals) and B15-b (clouds). Vulkan + DX12 additionally RUN them bit-exact / ULP (tests/gpu-context-*);
// CUDA/Metal/WebGPU have no compiler on this host, so these are STRUCTURAL gates (emit must return true — an unhandled op
// returns false). The FUSED evolve+row-IFFT (build_ocean_evolve_rowfft) is the load-bearing one: it composes shared arrays +
// barriers + the field `select` + the dispersion/pack transcendentals + the radix-4 stages, so its clean emit on all five is
// the real portability proof. n=64 (a power of 4, so the fused radix-4 row IFFT is well-formed).
TEST_CASE("B16-a-2: FFT-ocean kernels emit on ALL backends (spectrum/evolve/fused-rowfft/assemble)", "[kir][kernel][ocean]")
{
    crd::memory::TlsfAllocator alloc(64U << 20U);
    kir::ocean::OceanConfig    cfg;
    cfg.n            = 64;
    cfg.patch_length = crd::units::Length64{250.0};

    const auto emit5 = [&](const kir::KEntry& e, kir::KGraph& g, const char* what) {
        kir::GlslKernel kg(&alloc);
        kir::GlslKernel kh(&alloc);
        kir::GlslKernel kc(&alloc);
        kir::GlslKernel km(&alloc);
        kir::GlslKernel kw(&alloc);
        INFO(what);
        REQUIRE(kir::emit_compute_kernel_glsl(g, e, &alloc, kg)); // Vulkan — GPU-verified bit-exact
        REQUIRE(kir::emit_compute_kernel_hlsl(g, e, &alloc, kh)); // DX12 — GPU-verified bit-exact
        REQUIRE(kir::emit_compute_kernel_cuda(g, e, &alloc, kc));
        REQUIRE(kir::emit_compute_kernel_msl(g, e, &alloc, km));
        REQUIRE(kir::emit_compute_kernel_wgsl(g, e, &alloc, kw));
    };

    kir::KGraph       gspec(&alloc);
    const kir::KEntry espec = kir::ocean::build_ocean_spectrum(gspec, cfg);
    emit5(espec, gspec, "build_ocean_spectrum");

    kir::KGraph       gevo(&alloc);
    const kir::KEntry eevo = kir::ocean::build_ocean_evolve(gevo, cfg);
    emit5(eevo, gevo, "build_ocean_evolve");

    kir::KGraph       gasm(&alloc);
    const kir::KEntry easm = kir::ocean::build_ocean_assemble(gasm, cfg);
    emit5(easm, gasm, "build_ocean_assemble");

    // the fused evolve+row-IFFT — structural checks on all five prove the whole construct (shared + barrier + select +
    // transcendentals + radix-4) lowers, not just that emit returns true.
    kir::KGraph       gfus(&alloc);
    const kir::KEntry efus = kir::ocean::build_ocean_evolve_rowfft(gfus, cfg);
    kir::GlslKernel   fg(&alloc);
    kir::GlslKernel   fh(&alloc);
    kir::GlslKernel   fc(&alloc);
    kir::GlslKernel   fm(&alloc);
    kir::GlslKernel   fw(&alloc);
    REQUIRE(kir::emit_compute_kernel_glsl(gfus, efus, &alloc, fg));
    REQUIRE(kir::emit_compute_kernel_hlsl(gfus, efus, &alloc, fh));
    REQUIRE(kir::emit_compute_kernel_cuda(gfus, efus, &alloc, fc));
    REQUIRE(kir::emit_compute_kernel_msl(gfus, efus, &alloc, fm));
    REQUIRE(kir::emit_compute_kernel_wgsl(gfus, efus, &alloc, fw));
    CHECK(has(fg, "tanh("));  CHECK(has(fg, "barrier();"));            // dispersion + the shared exchange
    CHECK(has(fc, "tanhf(")); CHECK(has(fc, "__syncthreads();"));     // CUDA transcendental + barrier
    CHECK(has(fm, "tanh("));  CHECK(has(fm, "threadgroup_barrier(")); // MSL
    CHECK(has(fw, "select(")); CHECK(has(fw, "workgroupBarrier();")); // WGSL field-select + barrier
}

TEST_CASE("B-cmp: CUDA compute-kernel emitter -- well-formed shared-memory kernel", "[kir][cuda][kernel]")
{
    crd::memory::TlsfAllocator alloc(8U << 20U);
    kir::KGraph                g(&alloc);
    const kir::KEntry          e = build_reverse(g, 256);
    kir::GlslKernel            k(&alloc);
    REQUIRE(kir::emit_compute_kernel_cuda(g, e, &alloc, k));
    CHECK(has(k, "__global__ void ckir("));
    CHECK(has(k, "float* buf0"));                     // typed pointer params (not raw UAV)
    CHECK(has(k, "float* buf1"));
    CHECK(has(k, "__shared__ float sh"));
    CHECK(has(k, "__syncthreads();"));                // the workgroup barrier
    CHECK(has(k, "threadIdx.x"));                     // LocalInvocationIndex
    CHECK(has(k, "buf1[threadIdx.x] = sh"));          // the reverse store into the output buffer
}

TEST_CASE("B-cmp: MSL compute-kernel emitter -- well-formed shared-memory kernel", "[kir][msl][kernel]")
{
    crd::memory::TlsfAllocator alloc(8U << 20U);
    kir::KGraph                g(&alloc);
    const kir::KEntry          e = build_reverse(g, 256);
    kir::GlslKernel            k(&alloc);
    REQUIRE(kir::emit_compute_kernel_msl(g, e, &alloc, k));
    CHECK(has(k, "#include <metal_stdlib>"));
    CHECK(has(k, "kernel void ckir("));
    CHECK(has(k, "device const float* buf0 [[buffer(0)]]")); // readonly input
    CHECK(has(k, "device float* buf1 [[buffer(1)]]"));       // writable output
    CHECK(has(k, "threadgroup float sh"));                   // function-local shared array
    CHECK(has(k, "threadgroup_barrier(mem_flags::mem_threadgroup);"));
    CHECK(has(k, "thread_position_in_threadgroup"));         // LocalInvocationIndex
}

TEST_CASE("B-cmp: WGSL compute-kernel emitter -- well-formed shared-memory kernel", "[kir][wgsl][kernel]")
{
    crd::memory::TlsfAllocator alloc(8U << 20U);
    kir::KGraph                g(&alloc);
    const kir::KEntry          e = build_reverse(g, 256);
    kir::GlslKernel            k(&alloc);
    REQUIRE(kir::emit_compute_kernel_wgsl(g, e, &alloc, k));
    CHECK(has(k, "@group(0) @binding(0) var<storage, read> buf0 : array<f32>;"));       // readonly
    CHECK(has(k, "@group(0) @binding(1) var<storage, read_write> buf1 : array<f32>;")); // writable
    CHECK(has(k, "var<workgroup> sh"));
    CHECK(has(k, "@compute @workgroup_size(256, 1, 1)"));
    CHECK(has(k, "@builtin(local_invocation_index) lidx : u32"));
    CHECK(has(k, "workgroupBarrier();"));
}

// The one WGSL divergence a naive mirror gets wrong: `Select` has NO ternary — it is `select(false_val, true_val, cond)`,
// operand order reversed vs `?:`. Build `select(active, sh[lid], 0.0)` and prove WGSL puts the FALSE value (0.0) first
// while CUDA/MSL keep the condition first (`(cond) ? true : false`).
TEST_CASE("B-cmp: kernel Select -- WGSL reverses operand order vs CUDA/MSL ternary", "[kir][kernel][select]")
{
    crd::memory::TlsfAllocator alloc(8U << 20U);
    kir::KGraph                g(&alloc);
    const kir::Shape           sh1    = kir::make_shape({1});
    const int                  outbuf = g.buffer_decl(kir::DType::F32, 0, 0, true);
    const int                  smem   = g.shared_decl(kir::DType::F32, 64);
    const int                  lid    = g.builtin(kir::KBuiltin::LocalInvocationIndex);
    const int                  mark   = g.kernel_stmt_mark();
    g.stmt_shared_store(smem, lid, g.constant(1.0, sh1, kir::DType::F32));
    g.stmt_barrier();
    const int active = g.binary(kir::KOp::CmpLt, lid, g.constant(32.0, sh1, kir::DType::U32)); // Bool predicate
    const int sel    = g.select(active, g.shared_load(smem, lid), g.constant(0.0, sh1, kir::DType::F32));
    g.stmt_buffer_store(outbuf, lid, sel);

    kir::KEntry e;
    e.stage             = kir::KStage::Compute;
    e.local_size[0]     = 64;
    e.kernel_body_begin = mark;
    e.kernel_body_count = g.stmt_count() - mark;

    kir::GlslKernel kw(&alloc);
    REQUIRE(kir::emit_compute_kernel_wgsl(g, e, &alloc, kw));
    CHECK(has(kw, "select(0"));           // FALSE value (0.0) emitted FIRST — the reversed WGSL order
    CHECK(has(kw, "(lidx < 32u)"));       // Bool cond emitted directly (no `!= 0` wrapper needed)
    CHECK_FALSE(has(kw, ") ? "));         // WGSL has no ternary at all

    kir::GlslKernel kc(&alloc);
    REQUIRE(kir::emit_compute_kernel_cuda(g, e, &alloc, kc));
    CHECK(has(kc, "threadIdx.x < 32u"));  // CUDA ternary: condition FIRST ...
    CHECK(has(kc, ") ? sh"));             // ... then the TRUE value (shared load), not the false 0.0f
    CHECK_FALSE(has(kc, "select("));      // no WGSL-style select

    kir::GlslKernel km(&alloc);
    REQUIRE(kir::emit_compute_kernel_msl(g, e, &alloc, km));
    CHECK(has(km, "lidx < 32u"));         // MSL ternary: condition FIRST ...
    CHECK(has(km, ") ? sh"));             // ... then the TRUE value
}

// HIDDEN GENERATOR ([.emit-fft-cuda]): write the CUDA source of the Phase-1/2 FFT kernels to bench/gpu-fft/generated/ so
// the Nsight-Compute harness (bench/gpu-fft/ckir_fft_profilee.cu) can #include + launch OUR EXACT kernels on real CUDA HW
// and read registers/occupancy/stalls/L2 -- the apples-to-apples profile against cuFFT's vector_fft/regular_fft anatomy.
// Run explicitly: crd-kir-tests.exe "[.emit-fft-cuda]". Regenerate whenever ckir_fft.hpp changes.
TEST_CASE("B-cmp profile: emit the FFT kernels as CUDA for the ncu harness", "[.emit-fft-cuda]")
{
    crd::memory::TlsfAllocator alloc(64U << 20U);
    const auto                 dump = [](const char* path, const kir::GlslKernel& k) {
        FILE* f = nullptr;
        REQUIRE(fopen_s(&f, path, "wb") == 0);
        REQUIRE(f != nullptr);
        fwrite(k.source.c_str(), 1, k.source.size(), f);
        fclose(f);
    };

    { // the batched radix-4 row-FFT kernel, n=1024 (256 threads, grid = rows)
        kir::KGraph          g(&alloc);
        const kir::Fft1dPlan p = kir::build_fft1d_radix4(g, 1024, false, true);
        kir::GlslKernel      k(&alloc);
        REQUIRE(kir::emit_compute_kernel_cuda(g, p.entry, &alloc, k));
        dump("D:/Dev/cerid/bench/gpu-fft/generated/fft1024_r4_batched.cu", k);
    }
    { // the fused column conv kernel, n=1024, per-workgroup filter, 1/(1024*1024) scale (pass 3 of the 2-D conv)
        kir::KGraph          g(&alloc);
        const kir::Fft1dPlan p = kir::build_fft1d_convolution(g, 1024, true, 1.0 / (1024.0 * 1024.0), true);
        kir::GlslKernel      k(&alloc);
        REQUIRE(kir::emit_compute_kernel_cuda(g, p.entry, &alloc, k));
        dump("D:/Dev/cerid/bench/gpu-fft/generated/conv1024_fused.cu", k);
    }
    { // the REGISTER-BLOCKED radix-16 row-FFT kernel, n=1024 (64 threads, [16,16,4])
        kir::KGraph          g(&alloc);
        const kir::Fft1dPlan p = kir::build_fft1d_radix16(g, 1024, false, true);
        kir::GlslKernel      k(&alloc);
        REQUIRE(kir::emit_compute_kernel_cuda(g, p.entry, &alloc, k));
        dump("D:/Dev/cerid/bench/gpu-fft/generated/fft1024_r16_batched.cu", k);
    }
    { // the REGISTER-BLOCKED radix-16 fused conv kernel, n=1024
        kir::KGraph          g(&alloc);
        const kir::Fft1dPlan p = kir::build_fft1d_convolution16(g, 1024, true, 1.0 / (1024.0 * 1024.0), true);
        kir::GlslKernel      k(&alloc);
        REQUIRE(kir::emit_compute_kernel_cuda(g, p.entry, &alloc, k));
        dump("D:/Dev/cerid/bench/gpu-fft/generated/conv1024_r16_fused.cu", k);
    }
    { // the coalesced tile^2 transpose, 1024x1024 tile 32 (1024 threads, grid = 1024)
        kir::KGraph       g(&alloc);
        const kir::KEntry e = kir::build_transpose2d(g, 1024, 1024, 32);
        kir::GlslKernel   k(&alloc);
        REQUIRE(kir::emit_compute_kernel_cuda(g, e, &alloc, k));
        dump("D:/Dev/cerid/bench/gpu-fft/generated/transpose1024_t32.cu", k);
    }
}
