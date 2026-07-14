// test_ckir_kernel_emit.cpp — B-cmp Phase 0: the CUDA / MSL / WGSL emitters for the imperative compute-kernel IR (shared
// memory + barriers). CUDA/Metal/WebGPU have no compiler on this host, so — exactly like `test_ckir_msl.cpp` for the
// elementwise path — these are STRUCTURAL gates: the emitter must PRODUCE well-formed source (right kernel signature,
// storage buffers, shared arrays, barrier, the reverse index expression, and WGSL's reversed `select` operand order). The
// compile + bit-exact run on real hardware is ADR-0098 Part C; Vulkan + DX12 already run bit-exact (tests/gpu-context-*).

#include <crd/kir/ckir.hpp>
#include <crd/kir/ckir_cuda.hpp>
#include <crd/kir/ckir_fft.hpp>  // the [.emit-fft-cuda] ncu-harness generator
#include <crd/kir/ckir_glsl.hpp> // GlslKernel carrier
#include <crd/kir/ckir_msl.hpp>
#include <crd/kir/ckir_wgsl.hpp>

#include <crd/memory/allocators/tlsf_allocator.hpp>

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
} // namespace

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
