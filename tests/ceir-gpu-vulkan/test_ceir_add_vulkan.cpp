// CEIR-13z-1b (Vulkan) — a CEIR `add` compute ASSET, lowered by 13d and run on a real Vulkan device through
// `execute_lowered` (ADR-0126), proven BYTE-IDENTICAL to the direct CKIR dispatch of the SAME kernel + == the CPU oracle,
// validation-SILENT. ⛔ The device-free "always-runs" case (the asset lowers to one dispatch) guards the all-skip false-green
// for THIS target; the device case soft-skips with no adapter. The seam itself is unit-tested device-free in
// tests/ceir-gpu/test_execute.cpp; here it drives a real GPU.

#include <crd/ceir/ceir.hpp>
#include <crd/ceir/cook/hot_reload.hpp> // CEIR-13z-4 leg 2: the 10a ReloadSet (add_source/reload_source)
#include <crd/ceir/gpu/execute.hpp>
#include <crd/ceir/gpu/lower.hpp>
#include <crd/ceir/parse.hpp> // CEIR-13z-2 §121: parse a TEXT-authored asset
#include <crd/kir/ckir.hpp>
#include <crd/kir/ckir_fft.hpp>    // CEIR-13z-3: build_fft2d_c2c (the 6-dispatch 2D FFT plan)
#include <crd/kir/ckir_glsl.hpp>
#include <crd/kir/ckir_reduce.hpp> // CEIR-13z-2: build_reduce_block (the single-workgroup device reduction)
#include <crd/kir/ckir_scan.hpp>   // CEIR-13z-2: build_scan_block (the single-workgroup device prefix sum)

#include <crd/math/cmath.hpp> // CEIR-13z-3: cos/sin for the FFT twiddle tables

#include <crd/gpu/vulkan_compute_context.hpp>
#include <crd/gpu/vulkan_context.hpp>
#include <crd/gpu/vulkan_shader_compile.hpp>
#include <crd/gpu/vulkan_validation_capture.hpp>

#include <crd/memory/allocators/growable_tlsf_allocator.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include "../gpu-shared/ceir_execute_1wg.hpp"
#include "../gpu-shared/ckir_kernel_dispatch.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstring>

namespace ce  = crd::ceir;
namespace ceg = crd::ceir::gpu;
namespace cgt = crd::ceir_gpu_test;

namespace
{
// The ReloadSet Registrar for CEIR-13z-4: install exactly the dialects a compute+resource asset needs so the transient
// cook Context's verifiers are not vacuous (§121 text == builder). Same four the shared harness registers.
void ceir_gpu_registrar(ce::Context& c, void* /*user*/)
{
    (void)ce::arith::register_arith_ops(c);
    (void)ce::func::register_dialect(c);
    (void)ce::resource::register_resource_ops(c);
    (void)ce::compute::register_compute_ops(c);
}

// ⭐ CEIR-13z-4 hot-swap: a SYMBOL-KEYED kernel resolver. It reads the dispatch op's `kernel` symbol (@radd / @rmax) and
// returns the matching pipeline — so the RELOADED text alone selects add-reduce vs max-reduce, not the test. `ctx` is the
// CURRENT generation's Context (rebound after each reload; a reload mints a fresh Context that owns the dispatch op).
struct SwapResolve
{
    const ce::Context*         ctx   = nullptr;
    crd::gpu::ComputePipeline* p_add = nullptr;
    crd::gpu::ComputePipeline* p_max = nullptr;
};
crd::gpu::ComputePipeline* swap_resolve(const ce::Operation* disp, void* user)
{
    auto* const     s  = static_cast<SwapResolve*>(user);
    const ce::AttrValue kv = s->ctx->attr_value(disp->attr(crd::containers::StringView("kernel")));
    if (kv.kind != ce::AttrKind::SymbolRef) { return nullptr; }
    if (kv.s == crd::containers::StringView("radd")) { return s->p_add; }
    if (kv.s == crd::containers::StringView("rmax")) { return s->p_max; }
    return nullptr;
}

// The §121 reduce asset text, parameterized by the kernel symbol (@radd / @rmax). Same shape (2 buffers, r/w) — so the
// symbol swap changes content_hash but NOT interface_hash (func-less: empty exported-func projection) → a HotSwap.
const char* const kReduceRadd = "module {\n"
                                  "  ^bb0:\n"
                                  "    %0 = arith.const() {value = 1} : !index\n"
                                  "    %1 = resource.declare() : !buffer<plain,!f32>\n"
                                  "    %2 = resource.declare() : !buffer<plain,!f32>\n"
                                  "    compute.dispatch(%0, %0, %0, %1, %2) {access = \"r,w\", kernel = @radd}\n"
                                  "}\n";
const char* const kReduceRmax = "module {\n"
                                  "  ^bb0:\n"
                                  "    %0 = arith.const() {value = 1} : !index\n"
                                  "    %1 = resource.declare() : !buffer<plain,!f32>\n"
                                  "    %2 = resource.declare() : !buffer<plain,!f32>\n"
                                  "    compute.dispatch(%0, %0, %0, %1, %2) {access = \"r,w\", kernel = @rmax}\n"
                                  "}\n";
} // namespace

TEST_CASE("ceir 13z: the add CEIR asset lowers to one dispatch over 3 bindings (device-free always-runs)", "[ceir][ceir-gpu][vulkan]")
{
    crd::memory::GrowableTlsfAllocator root;
    ce::Context                  cctx(&root);
    const ce::Value*             binds[3];
    ce::Block* const             b = cgt::build_add_ceir_asset(cctx, binds);

    crd::containers::Array<ceg::LoweredCommand> cmds(&root);
    ceg::lower_region(cctx, *b, cmds);
    REQUIRE(cmds.size() == 1U);
    CHECK(cmds[0].kind == ceg::LoweredKind::Dispatch);
    CHECK_FALSE(cmds[0].dynamic_grid);
    CHECK(cmds[0].groups_x == 1U); // grid const = 1 workgroup (the ls threads live in local_size)
    CHECK(binds[0] != nullptr);
    CHECK(binds[2] != nullptr);
}

TEST_CASE("ceir 13z: add CEIR asset on Vulkan == direct CKIR (byte-identical) + oracle + validation-silent", "[ceir][ceir-gpu][vulkan][gpu]")
{
    namespace gpu = crd::gpu;
    gpu::GpuContextConfig gcfg{};
    gcfg.backend           = gpu::GpuBackend::Vulkan;
    gcfg.headless          = true;
    gcfg.enable_validation = true; // the dispatch path must be validation-SILENT, asserted by counters
    auto ctx               = gpu::create_vulkan_gpu_context(gcfg);
    if (ctx == nullptr) { WARN("no Vulkan device available; skipping"); return; }
    auto* vkctx = static_cast<gpu::VulkanGpuContext*>(ctx.get());

    gpu::ValidationCapture     capture(*vkctx);
    crd::memory::TlsfAllocator alloc(32U << 20U);
    gpu::VulkanComputeContext  compute(*vkctx, &alloc);
    REQUIRE(compute.valid());

    // build + emit + compile the add kernel to a pipeline (3 storage bindings, no push constant)
    constexpr int   n = 64;
    crd::kir::KGraph g(&alloc);
    const crd::kir::KEntry e = cgt::build_add_kernel(g, n);
    crd::kir::GlslKernel   kern(&alloc);
    REQUIRE(crd::kir::emit_compute_kernel_glsl(g, e, &alloc, kern));
    const auto cres = gpu::compile_glsl_to_spirv(gpu::ShaderStage::Compute, crd::containers::to_view(kern.source), "ceir_add", &alloc);
    REQUIRE(cres.ok);
    auto pipe = compute.create_pipeline_from_spirv(crd::containers::ConstSpan<crd::u8>(cres.spirv.data(), cres.spirv.size()), 3, 0U);
    REQUIRE(pipe != nullptr);

    // build + lower the CEIR asset
    crd::memory::GrowableTlsfAllocator croot;
    ce::Context                  cctx(&croot);
    const ce::Value*             binds[3];
    ce::Block* const             blk = cgt::build_add_ceir_asset(cctx, binds);
    crd::containers::Array<ceg::LoweredCommand> cmds(&croot);
    ceg::lower_region(cctx, *blk, cmds);
    REQUIRE(cmds.size() == 1U);

    // inputs (f32 add is one IEEE op -> bit-exact on every backend, llvmpipe included)
    float a0[n];
    float b0[n];
    for (int i = 0; i < n; ++i)
    {
        a0[i] = static_cast<float>(i) * 0.5F;
        b0[i] = static_cast<float>(n - i) * 0.25F;
    }
    const int lens[3] = {n, n, n};

    // DIRECT path: dispatch_kernel_1wg (rec.dispatch) — the reference.
    float ad[n];
    float bd[n];
    float cd[n];
    for (int i = 0; i < n; ++i) { ad[i] = a0[i]; bd[i] = b0[i]; cd[i] = 0.0F; }
    float* hd[3] = {ad, bd, cd};
    crd::kir_test::dispatch_kernel_1wg(compute, *pipe, hd, lens, 3, 1U);

    // CEIR path: dispatch_ceir_1wg (execute_lowered) — the SAME pipe, separate output.
    float ac[n];
    float bc[n];
    float cc[n];
    for (int i = 0; i < n; ++i) { ac[i] = a0[i]; bc[i] = b0[i]; cc[i] = 0.0F; }
    float* hc[3] = {ac, bc, cc};
    const ceg::ExecuteError err =
        cgt::dispatch_ceir_1wg(cctx, crd::containers::ConstSpan<ceg::LoweredCommand>(cmds.data(), cmds.size()), binds, *pipe,
                               compute, hc, lens, 3, 1U);

    CHECK(err == ceg::ExecuteError::None);
    CHECK(std::memcmp(cc, cd, sizeof(cc)) == 0);        // ⭐ CEIR path byte-identical to the direct CKIR path
    for (int i = 0; i < n; ++i) { CHECK(cc[i] == a0[i] + b0[i]); } // == the CPU oracle (f32 add exact)
    CHECK(capture.error_count() == 0U);                 // validation-SILENT
}

TEST_CASE("ceir 13z: reduce CEIR asset on Vulkan == direct CKIR (byte-identical) + oracle + validation-silent", "[ceir][ceir-gpu][vulkan][gpu]")
{
    namespace gpu = crd::gpu;
    gpu::GpuContextConfig gcfg{};
    gcfg.backend           = gpu::GpuBackend::Vulkan;
    gcfg.headless          = true;
    gcfg.enable_validation = true;
    auto ctx               = gpu::create_vulkan_gpu_context(gcfg);
    if (ctx == nullptr) { WARN("no Vulkan device available; skipping"); return; }
    auto* vkctx = static_cast<gpu::VulkanGpuContext*>(ctx.get());

    gpu::ValidationCapture     capture(*vkctx);
    crd::memory::TlsfAllocator alloc(32U << 20U);
    gpu::VulkanComputeContext  compute(*vkctx, &alloc);
    REQUIRE(compute.valid());

    // build_reduce_block: ONE workgroup of n threads sum-reduces its span of n elements -> out[0]. 2 buffers (in r, out w).
    constexpr int   n = 64;
    crd::kir::KGraph g(&alloc);
    const crd::kir::KEntry e = crd::kir::build_reduce_block(g, n, n, crd::kir::KOp::Add);
    crd::kir::GlslKernel   kern(&alloc);
    REQUIRE(crd::kir::emit_compute_kernel_glsl(g, e, &alloc, kern));
    const auto cres = gpu::compile_glsl_to_spirv(gpu::ShaderStage::Compute, crd::containers::to_view(kern.source), "ceir_reduce", &alloc);
    REQUIRE(cres.ok);
    auto pipe = compute.create_pipeline_from_spirv(crd::containers::ConstSpan<crd::u8>(cres.spirv.data(), cres.spirv.size()), 2, 0U);
    REQUIRE(pipe != nullptr);

    crd::memory::GrowableTlsfAllocator croot;
    ce::Context                  cctx(&croot);
    const cgt::CeirDispatchAsset asset = cgt::build_ceir_dispatch_asset(cctx, "reduce", "r,w", 2);
    crd::containers::Array<ceg::LoweredCommand> cmds(&croot);
    ceg::lower_region(cctx, *asset.block, cmds);
    REQUIRE(cmds.size() == 1U);

    // integer-valued f32 inputs (sum 0+1+..+63 = 2016 < 2^24 -> every association order is EXACT, so the naive oracle is `==`)
    float in0[n];
    float expect = 0.0F;
    for (int i = 0; i < n; ++i)
    {
        in0[i] = static_cast<float>(i);
        expect += in0[i];
    }
    const int lens[2] = {n, 1}; // out is a single reduced value (one workgroup -> out[0])

    float ind[n];
    float outd[1];
    for (int i = 0; i < n; ++i) { ind[i] = in0[i]; }
    outd[0]      = 0.0F;
    float* hd[2] = {ind, outd};
    crd::kir_test::dispatch_kernel_1wg(compute, *pipe, hd, lens, 2, 1U);

    float inc[n];
    float outc[1];
    for (int i = 0; i < n; ++i) { inc[i] = in0[i]; }
    outc[0]      = 0.0F;
    float* hc[2] = {inc, outc};
    const ceg::ExecuteError err =
        cgt::dispatch_ceir_1wg(cctx, crd::containers::ConstSpan<ceg::LoweredCommand>(cmds.data(), cmds.size()), asset.binds,
                               *pipe, compute, hc, lens, 2, 1U);

    CHECK(err == ceg::ExecuteError::None);
    CHECK(outc[0] == outd[0]);          // ⭐ CEIR path byte-identical to the direct CKIR path
    CHECK(outc[0] == expect);           // == the CPU oracle (integer-valued -> exact)
    CHECK(capture.error_count() == 0U); // validation-SILENT
}

TEST_CASE("ceir 13z: scan CEIR asset on Vulkan == direct CKIR (byte-identical) + oracle + validation-silent", "[ceir][ceir-gpu][vulkan][gpu]")
{
    namespace gpu = crd::gpu;
    gpu::GpuContextConfig gcfg{};
    gcfg.backend           = gpu::GpuBackend::Vulkan;
    gcfg.headless          = true;
    gcfg.enable_validation = true;
    auto ctx               = gpu::create_vulkan_gpu_context(gcfg);
    if (ctx == nullptr) { WARN("no Vulkan device available; skipping"); return; }
    auto* vkctx = static_cast<gpu::VulkanGpuContext*>(ctx.get());

    gpu::ValidationCapture     capture(*vkctx);
    crd::memory::TlsfAllocator alloc(32U << 20U);
    gpu::VulkanComputeContext  compute(*vkctx, &alloc);
    REQUIRE(compute.valid());

    // build_scan_block: ONE workgroup INCLUSIVE-scans its span in place; write_blocksum=TRUE publishes the span TOTAL to
    // bsum[0] (⛔ landmine 2: every declared buffer LIVE). 3 buffers: in(r), out(w), bsum(w).
    constexpr int   n = 64;
    crd::kir::KGraph g(&alloc);
    const crd::kir::KEntry e = crd::kir::build_scan_block(g, n, n, /*inclusive=*/true, /*write_blocksum=*/true);
    crd::kir::GlslKernel   kern(&alloc);
    REQUIRE(crd::kir::emit_compute_kernel_glsl(g, e, &alloc, kern));
    const auto cres = gpu::compile_glsl_to_spirv(gpu::ShaderStage::Compute, crd::containers::to_view(kern.source), "ceir_scan", &alloc);
    REQUIRE(cres.ok);
    auto pipe = compute.create_pipeline_from_spirv(crd::containers::ConstSpan<crd::u8>(cres.spirv.data(), cres.spirv.size()), 3, 0U);
    REQUIRE(pipe != nullptr);

    crd::memory::GrowableTlsfAllocator croot;
    ce::Context                  cctx(&croot);
    const cgt::CeirDispatchAsset asset = cgt::build_ceir_dispatch_asset(cctx, "scan", "r,w,w", 3);
    crd::containers::Array<ceg::LoweredCommand> cmds(&croot);
    ceg::lower_region(cctx, *asset.block, cmds);
    REQUIRE(cmds.size() == 1U);

    // integer-valued inputs -> the inclusive prefix sums k(k+1)/2 are all exact integers < 2^24.
    float in0[n];
    float pref[n];
    float total = 0.0F;
    for (int i = 0; i < n; ++i)
    {
        in0[i] = static_cast<float>(i);
        total += in0[i];
        pref[i] = total;
    }
    const int lens[3] = {n, n, 1}; // in, out (scanned), bsum (the span total)

    float ind[n];
    float outd[n];
    float bsd[1];
    for (int i = 0; i < n; ++i)
    {
        ind[i]  = in0[i];
        outd[i] = 0.0F;
    }
    bsd[0]       = 0.0F;
    float* hd[3] = {ind, outd, bsd};
    crd::kir_test::dispatch_kernel_1wg(compute, *pipe, hd, lens, 3, 1U);

    float inc[n];
    float outc[n];
    float bsc[1];
    for (int i = 0; i < n; ++i)
    {
        inc[i]  = in0[i];
        outc[i] = 0.0F;
    }
    bsc[0]       = 0.0F;
    float* hc[3] = {inc, outc, bsc};
    const ceg::ExecuteError err =
        cgt::dispatch_ceir_1wg(cctx, crd::containers::ConstSpan<ceg::LoweredCommand>(cmds.data(), cmds.size()), asset.binds,
                               *pipe, compute, hc, lens, 3, 1U);

    CHECK(err == ceg::ExecuteError::None);
    CHECK(std::memcmp(outc, outd, sizeof(outc)) == 0); // ⭐ CEIR path byte-identical to the direct CKIR path
    CHECK(bsc[0] == bsd[0]);
    for (int i = 0; i < n; ++i) { CHECK(outc[i] == pref[i]); } // == the inclusive-scan oracle
    CHECK(bsc[0] == total);                                    // bsum[0] == the span total (the free landmine-2 oracle)
    CHECK(capture.error_count() == 0U);                        // validation-SILENT
}

TEST_CASE("ceir 13z: a TEXT-authored reduce asset (parsed) executes on Vulkan == oracle (the text-authoring device leg)", "[ceir][ceir-gpu][vulkan][gpu]")
{
    namespace gpu = crd::gpu;
    gpu::GpuContextConfig gcfg{};
    gcfg.backend  = gpu::GpuBackend::Vulkan;
    gcfg.headless = true;
    auto ctx      = gpu::create_vulkan_gpu_context(gcfg);
    if (ctx == nullptr) { WARN("no Vulkan device available; skipping"); return; }
    auto* vkctx = static_cast<gpu::VulkanGpuContext*>(ctx.get());

    crd::memory::TlsfAllocator alloc(32U << 20U);
    gpu::VulkanComputeContext  compute(*vkctx, &alloc);
    REQUIRE(compute.valid());

    constexpr int   n = 64;
    crd::kir::KGraph g(&alloc);
    const crd::kir::KEntry e = crd::kir::build_reduce_block(g, n, n, crd::kir::KOp::Add);
    crd::kir::GlslKernel   kern(&alloc);
    REQUIRE(crd::kir::emit_compute_kernel_glsl(g, e, &alloc, kern));
    const auto cres = gpu::compile_glsl_to_spirv(gpu::ShaderStage::Compute, crd::containers::to_view(kern.source), "ceir_reduce_txt", &alloc);
    REQUIRE(cres.ok);
    auto pipe = compute.create_pipeline_from_spirv(crd::containers::ConstSpan<crd::u8>(cres.spirv.data(), cres.spirv.size()), 2, 0U);
    REQUIRE(pipe != nullptr);

    // ⭐ the asset is authored as TEXT and PARSED (not built) — the §121 authoring path reaching real GPU execution.
    const char* const k_reduce_text = "module {\n"
                                      "  ^bb0:\n"
                                      "    %0 = arith.const() {value = 1} : !index\n"
                                      "    %1 = resource.declare() : !buffer<plain,!f32>\n"
                                      "    %2 = resource.declare() : !buffer<plain,!f32>\n"
                                      "    compute.dispatch(%0, %0, %0, %1, %2) {access = \"r,w\", kernel = @reduce}\n"
                                      "}\n";
    crd::memory::GrowableTlsfAllocator croot;
    ce::Context                  cctx(&croot);
    const ce::ParseResult        pr = ce::parse(cctx, crd::containers::StringView(k_reduce_text));
    REQUIRE(pr.ok);
    ce::Block* const pb = pr.module->body()->first_block();
    REQUIRE(pb != nullptr);
    const ce::Value* binds[8];
    const int        nb = cgt::collect_dispatch_binds(cctx, *pb, binds);
    REQUIRE(nb == 2);
    crd::containers::Array<ceg::LoweredCommand> cmds(&croot);
    ceg::lower_region(cctx, *pb, cmds);
    REQUIRE(cmds.size() == 1U);

    float in0[n];
    float expect = 0.0F;
    for (int i = 0; i < n; ++i)
    {
        in0[i] = static_cast<float>(i);
        expect += in0[i];
    }
    const int lens[2] = {n, 1};
    float     inc[n];
    float     outc[1];
    for (int i = 0; i < n; ++i) { inc[i] = in0[i]; }
    outc[0]      = 0.0F;
    float* hc[2] = {inc, outc};
    const ceg::ExecuteError err =
        cgt::dispatch_ceir_1wg(cctx, crd::containers::ConstSpan<ceg::LoweredCommand>(cmds.data(), cmds.size()), binds, *pipe,
                               compute, hc, lens, 2, 1U);
    CHECK(err == ceg::ExecuteError::None);
    CHECK(outc[0] == expect); // the TEXT-authored reduce ran on the GPU and == the oracle
}

// ⭐ CEIR-13z-4 leg 2 (device-free): a compute+resource CEIR asset through the 10a hot-reload machinery end to end.
// (1) THE PROBE — add_source cooks the §121 text to a CRDR blob (cook_program_text) + load_program rebuilds the module in a
//     FRESH generation Context. Untested seam: prior 10a coverage cooked func/arith/state programs, never a top-level
//     compute.dispatch over resource.declare. If the cook rejected a func-less top-level-compute module, add_source would fail here.
// (2) THE HOTSWAP DECISION — reload_source(@rmax) changes the kernel SYMBOL (content_hash moves) but not the caller contract
//     (func-less ⇒ empty interface projection ⇒ interface_hash HELD) → the reload classifies HotSwap and installs. The
//     RELOADED module (new Context) is re-collected + re-lowered to prove it is a functional replacement, not a stale alias.
// (3) THE NoChange SEAL — reload_source with the IDENTICAL text is a no-op (content_hash unchanged) → cook DETERMINISM through
//     the text seam for this asset shape (same text → same blob → same content_hash).
TEST_CASE("ceir 13z-4: a compute+resource CEIR asset hot-reloads through the 10a ReloadSet (HotSwap, device-free)", "[ceir][ceir-gpu][vulkan]")
{
    namespace cook = crd::ceir::cook;
    crd::memory::GrowableTlsfAllocator root;
    cook::ReloadSet              rs(&root, &ceir_gpu_registrar, nullptr);
    const cook::AssetId          id{1U};

    // (1) THE PROBE — cook + load of compute+resource, end to end.
    const cook::AddResult ar = rs.add_source(id, crd::containers::StringView(kReduceRadd));
    INFO("add_error=" << static_cast<int>(ar.error) << " cook_error=" << static_cast<int>(ar.cook_error)
                      << " load_error=" << static_cast<int>(ar.load_error));
    REQUIRE(ar.ok());

    const auto lower_current = [&](const char* what) -> crd::usize {
        const cook::Generation* g = rs.generation(id);
        REQUIRE(g != nullptr);
        REQUIRE(g->ctx != nullptr);
        REQUIRE(g->program.module != nullptr);
        ce::Block* const pb = g->program.module->body()->first_block();
        REQUIRE(pb != nullptr);
        const ce::Value* binds[8];
        const int        nb = cgt::collect_dispatch_binds(*g->ctx, *pb, binds);
        INFO(what << ": binds=" << nb);
        REQUIRE(nb == 2); // the two resource.declare bindings survived the round-trip
        crd::containers::Array<ceg::LoweredCommand> cmds(&root);
        ceg::lower_region(*g->ctx, *pb, cmds);
        REQUIRE(cmds.size() == 1U); // one dispatch
        CHECK(cmds[0].kind == ceg::LoweredKind::Dispatch);
        return cmds.size();
    };
    REQUIRE(lower_current("initial") == 1U);
    const crd::u64 content0   = rs.program(id)->content_hash;
    const crd::u64 interface0 = rs.program(id)->interface_hash;

    // (2) THE HOTSWAP — the kernel symbol @radd → @rmax: content moves, contract holds.
    const cook::ReloadResult rr = rs.reload_source(id, crd::containers::StringView(kReduceRmax));
    INFO("decision=" << static_cast<int>(rr.decision) << " installed=" << rr.installed << " load_ok=" << rr.load_ok
                     << " cook_error=" << static_cast<int>(rr.cook_error));
    REQUIRE(rr.load_ok);
    CHECK(rr.decision == cook::ReloadDecision::HotSwap); // symbol feeds content, not interface (a func-less contract is empty)
    CHECK(rr.installed);
    CHECK(rs.program(id)->content_hash != content0);    // the reloaded content differs...
    CHECK(rs.program(id)->interface_hash == interface0); // ...but the caller contract is unchanged
    REQUIRE(lower_current("after-hotswap") == 1U);       // the RELOADED module is a functional replacement

    // (3) THE NoChange SEAL — identical text cooks to the identical blob (determinism), so reload is a no-op.
    const cook::ReloadResult rr2 = rs.reload_source(id, crd::containers::StringView(kReduceRmax));
    CHECK(rr2.decision == cook::ReloadDecision::NoChange);
    CHECK_FALSE(rr2.installed);
}

// ⭐ CEIR-13z-4 leg 2 (DEVICE): a hot-reloaded reduce asset re-executes on a real Vulkan device with a DIFFERENT result —
// and the reload ALONE is the cause. The SYMBOL-KEYED resolver maps the dispatch's kernel symbol to a pipeline (add→sum,
// max→max); both pipelines + the resolver are FIXED across both runs. Run 1 (@radd) sums [0..63] → 2016. reload_source(@rmax)
// HotSwaps the text; run 2, with the SAME resolver, now resolves to the max pipeline because the reloaded symbol changed →
// 63. Nothing in the test picks the pipeline — the reloaded text does. Soft-skips with no adapter (the device-free case above
// carries the decision coverage).
TEST_CASE("ceir 13z-4: a hot-reloaded reduce asset re-executes on Vulkan add to max 2016 to 63 (symbol-keyed resolver)", "[ceir][ceir-gpu][vulkan][gpu]")
{
    namespace gpu  = crd::gpu;
    namespace cook = crd::ceir::cook;
    gpu::GpuContextConfig gcfg{};
    gcfg.backend  = gpu::GpuBackend::Vulkan;
    gcfg.headless = true;
    auto ctx      = gpu::create_vulkan_gpu_context(gcfg);
    if (ctx == nullptr) { WARN("no Vulkan device available; skipping"); return; }
    auto* vkctx = static_cast<gpu::VulkanGpuContext*>(ctx.get());

    crd::memory::TlsfAllocator alloc(32U << 20U);
    gpu::VulkanComputeContext  compute(*vkctx, &alloc);
    REQUIRE(compute.valid());

    constexpr int n = 64;
    // Two FIXED reduce pipelines: add (sum) and max. The RELOADED symbol picks between them via swap_resolve.
    crd::kir::KGraph       ga(&alloc);
    crd::kir::KGraph       gm(&alloc);
    const crd::kir::KEntry ea = crd::kir::build_reduce_block(ga, n, n, crd::kir::KOp::Add);
    const crd::kir::KEntry em = crd::kir::build_reduce_block(gm, n, n, crd::kir::KOp::Max);
    crd::kir::GlslKernel   ka(&alloc);
    crd::kir::GlslKernel   km(&alloc);
    REQUIRE(crd::kir::emit_compute_kernel_glsl(ga, ea, &alloc, ka));
    REQUIRE(crd::kir::emit_compute_kernel_glsl(gm, em, &alloc, km));
    const auto ca = gpu::compile_glsl_to_spirv(gpu::ShaderStage::Compute, crd::containers::to_view(ka.source), "ceir_radd", &alloc);
    const auto cm = gpu::compile_glsl_to_spirv(gpu::ShaderStage::Compute, crd::containers::to_view(km.source), "ceir_rmax", &alloc);
    REQUIRE(ca.ok);
    REQUIRE(cm.ok);
    auto pa = compute.create_pipeline_from_spirv(crd::containers::ConstSpan<crd::u8>(ca.spirv.data(), ca.spirv.size()), 2, 0U);
    auto pm = compute.create_pipeline_from_spirv(crd::containers::ConstSpan<crd::u8>(cm.spirv.data(), cm.spirv.size()), 2, 0U);
    REQUIRE(pa != nullptr);
    REQUIRE(pm != nullptr);

    crd::memory::GrowableTlsfAllocator croot;
    cook::ReloadSet              rs(&croot, &ceir_gpu_registrar, nullptr);
    const cook::AssetId          id{7U};
    REQUIRE(rs.add_source(id, crd::containers::StringView(kReduceRadd)).ok());

    SwapResolve sctx;
    sctx.p_add = pa.get();
    sctx.p_max = pm.get();

    // input [0..63]: sum = 2016, max = 63. `run` re-fetches the CURRENT generation, re-lowers, executes with the fixed resolver.
    const auto run = [&]() -> float {
        const cook::Generation* g = rs.generation(id);
        REQUIRE(g != nullptr);
        sctx.ctx = g->ctx; // ⛔ rebind: a reload minted a fresh Context owning the dispatch op the resolver reads
        ce::Block* const pb = g->program.module->body()->first_block();
        REQUIRE(pb != nullptr);
        const ce::Value* binds[8];
        const int        nb = cgt::collect_dispatch_binds(*g->ctx, *pb, binds);
        REQUIRE(nb == 2);
        crd::containers::Array<ceg::LoweredCommand> cmds(&croot);
        ceg::lower_region(*g->ctx, *pb, cmds);
        REQUIRE(cmds.size() == 1U);
        float in[n];
        float out[1];
        for (int i = 0; i < n; ++i) { in[i] = static_cast<float>(i); }
        out[0]            = 0.0F;
        float*    hc[2]   = {in, out};
        const int lens[2] = {n, 1};
        const ceg::ExecuteError err =
            cgt::dispatch_ceir_1wg_resolved(*g->ctx, crd::containers::ConstSpan<ceg::LoweredCommand>(cmds.data(), cmds.size()),
                                            binds, swap_resolve, &sctx, compute, hc, lens, 2, 1U);
        CHECK(err == ceg::ExecuteError::None);
        return out[0];
    };

    CHECK(run() == 2016.0F); // @radd → sum over [0..63]

    // ⭐ HOT-SWAP: reload to @rmax; the SAME resolver now returns the max pipeline because the reloaded symbol changed.
    const cook::ReloadResult rr = rs.reload_source(id, crd::containers::StringView(kReduceRmax));
    REQUIRE(rr.load_ok);
    REQUIRE(rr.decision == cook::ReloadDecision::HotSwap);
    REQUIRE(rr.installed);

    CHECK(run() == 63.0F); // @rmax → max over [0..63]; ONLY the reloaded text changed the result
}

// ⭐ CEIR-13z-3 part 3: the 6-dispatch 2D FFT as a MULTI-DISPATCH CEIR asset — the on-device BARRIER STRESS. The CEIR
// lowering derives the per-resource inter-pass barriers (⭐ pass 3 gets TWO: RAW on b_tr_re from pass 1 AND b_tr_im from
// pass 2 — the part-1 completion, load-bearing here) and execute_lowered replays them. Bit-exact vs the CPU oracle
// (run_fft2d_cpu) — the SAME correctness gate the direct dispatch_fft2d test uses.
TEST_CASE("ceir 13z: a 6-dispatch 2D FFT CEIR asset on Vulkan == CPU oracle (multi-dispatch per-resource barriers)", "[ceir][ceir-gpu][vulkan][gpu][fft]")
{
    namespace gpu = crd::gpu;
    namespace kir = crd::kir;
    gpu::GpuContextConfig gcfg{};
    gcfg.backend           = gpu::GpuBackend::Vulkan;
    gcfg.headless          = true;
    gcfg.enable_validation = true;
    auto ctx               = gpu::create_vulkan_gpu_context(gcfg);
    if (ctx == nullptr) { WARN("no Vulkan device available; skipping"); return; }
    auto* vkctx = static_cast<gpu::VulkanGpuContext*>(ctx.get());

    gpu::ValidationCapture     capture(*vkctx);
    crd::memory::TlsfAllocator alloc(64U << 20U);
    gpu::VulkanComputeContext  compute(*vkctx, &alloc);
    REQUIRE(compute.valid());

    kir::KGraph          g0(&alloc);
    kir::KGraph          g1(&alloc);
    kir::KGraph          g2(&alloc);
    kir::KGraph          g3(&alloc);
    kir::KGraph*         graphs[4] = {&g0, &g1, &g2, &g3};
    constexpr int        rr   = 64;
    constexpr int        cc   = 64;
    constexpr int        tile = 16;
    const kir::Fft2dPlan plan = kir::build_fft2d_c2c(graphs, rr, cc, false, tile);

    int off[16];
    int total = 0;
    for (int b = 0; b < plan.nbuffers; ++b) { off[b] = total; total += plan.buffers[b].size; }
    crd::containers::Array<crd::f64> a64(&alloc);
    crd::containers::Array<float>    a32(&alloc);
    a64.resize(static_cast<crd::usize>(total), 0.0);
    a32.resize(static_cast<crd::usize>(total), 0.0F);
    crd::f64* h64[16];
    float*    h32[16];
    for (int b = 0; b < plan.nbuffers; ++b)
    {
        h64[b] = a64.data() + off[b];
        h32[b] = a32.data() + off[b];
    }

    constexpr crd::f64 two_pi = 6.28318530717958647693;
    const auto         f32d   = [](crd::f64 v) { return static_cast<crd::f64>(static_cast<float>(v)); };
    for (int i = 0; i < rr * cc; ++i)
    {
        h64[plan.in_re][i] = static_cast<crd::f64>((i * 7 + 3) % 11 - 5);
        h64[plan.in_im][i] = static_cast<crd::f64>((i * 5 + 1) % 7 - 3);
    }
    for (int k = 0; k < cc; ++k)
    {
        const crd::f64 a         = two_pi * static_cast<crd::f64>(k) / static_cast<crd::f64>(cc);
        h64[plan.tw_col_re][k]   = f32d(crd::math::cos(a));
        h64[plan.tw_col_im][k]   = f32d(-crd::math::sin(a));
    }
    for (int k = 0; k < rr; ++k)
    {
        const crd::f64 a         = two_pi * static_cast<crd::f64>(k) / static_cast<crd::f64>(rr);
        h64[plan.tw_row_re][k]   = f32d(crd::math::cos(a));
        h64[plan.tw_row_im][k]   = f32d(-crd::math::sin(a));
    }
    for (int i = 0; i < total; ++i) { a32[static_cast<crd::usize>(i)] = static_cast<float>(a64[static_cast<crd::usize>(i)]); }

    crd::kir_test::run_fft2d_cpu(plan, h64, &alloc); // the CPU oracle (mutates h64 -> the spectrum)

    // compile one pipeline per pass (emit GLSL -> SPIR-V -> pipeline; nbind per pass)
    std::unique_ptr<gpu::ComputePipeline> pipe_store[8];
    gpu::ComputePipeline*                 pipes[8] = {};
    for (int pi = 0; pi < plan.npasses; ++pi)
    {
        kir::GlslKernel kern(&alloc);
        REQUIRE(kir::emit_compute_kernel_glsl(*plan.passes[pi].graph, plan.passes[pi].entry, &alloc, kern));
        const auto spv = gpu::compile_glsl_to_spirv(gpu::ShaderStage::Compute, crd::containers::to_view(kern.source), "ceir_fft", &alloc);
        REQUIRE(spv.ok);
        pipe_store[pi] = compute.create_pipeline_from_spirv(crd::containers::ConstSpan<crd::u8>(spv.spirv.data(), spv.spirv.size()),
                                                            plan.passes[pi].nbind, 0U);
        REQUIRE(pipe_store[pi] != nullptr);
        pipes[pi] = pipe_store[pi].get();
    }

    // Build the CEIR asset MIRRORING the plan: one dispatch per pass, honest access (FFT pass = r,r,r,r,w,w; transpose = r,w).
    cgt::MultiPass mp[8];
    const char*    names[8] = {"p0", "p1", "p2", "p3", "p4", "p5", "p6", "p7"};
    for (int pi = 0; pi < plan.npasses; ++pi)
    {
        mp[pi].kernel = names[pi];
        mp[pi].nbind  = plan.passes[pi].nbind;
        mp[pi].access = (plan.passes[pi].nbind == 6) ? "r,r,r,r,w,w" : "r,w";
        mp[pi].grid   = static_cast<int>(plan.passes[pi].num_workgroups);
        for (int k = 0; k < plan.passes[pi].nbind; ++k) { mp[pi].bind[k] = plan.passes[pi].bind[k]; }
    }
    crd::memory::GrowableTlsfAllocator  croot;
    ce::Context                   cctx(&croot);
    const cgt::CeirMultiAsset     asset = cgt::build_ceir_multi_asset(cctx, plan.nbuffers, mp, plan.npasses);
    crd::containers::Array<ceg::LoweredCommand> cmds(&croot);
    ceg::lower_region(cctx, *asset.block, cmds);

    int sizes[16];
    for (int b = 0; b < plan.nbuffers; ++b) { sizes[b] = plan.buffers[b].size; }

    // run the CEIR path (execute_lowered derives + replays the inter-pass barriers) into h32.
    const ceg::ExecuteError err = cgt::dispatch_ceir_multi(
        cctx, asset, crd::containers::ConstSpan<ceg::LoweredCommand>(cmds.data(), cmds.size()), sizes, pipes, compute, h32);
    CHECK(err == ceg::ExecuteError::None);

    // bit-exact vs the CPU oracle (the same gate the direct dispatch_fft2d test uses).
    int badr = 0;
    int badi = 0;
    for (int i = 0; i < rr * cc; ++i)
    {
        if (h32[plan.res_re][i] != static_cast<float>(h64[plan.res_re][i])) { ++badr; }
        if (h32[plan.res_im][i] != static_cast<float>(h64[plan.res_im][i])) { ++badi; }
    }
    CHECK(badr == 0);
    CHECK(badi == 0);
    CHECK(capture.error_count() == 0U); // the multi-pass barriered execution is validation-SILENT
}

// ⭐ CEIR-13z-4 leg 1a: the CPU REFERENCE EXECUTOR — run the LOWERED FFT command list on the CPU (execute_lowered_cpu) and
// assert it is BYTE-EQUAL to the plan-driven run_fft2d_cpu. Both run the same (graph, entry) kernels in the same order over
// identical f64 data — one driven by the plan, one by the CEIR lowering — so ANY lowering error (dispatch sequence, binding
// mapping, or grid resolution via cmd.groups_x) diverges. ⛔ DEVICE-FREE (no GPU) — the §118 reference gates on all 4 configs.
TEST_CASE("ceir 13z-4: the CPU reference executor of the lowered FFT list == run_fft2d_cpu (device-free)", "[ceir][ceir-gpu][vulkan][fft]")
{
    namespace kir = crd::kir;
    crd::memory::TlsfAllocator alloc(64U << 20U);

    kir::KGraph          g0(&alloc);
    kir::KGraph          g1(&alloc);
    kir::KGraph          g2(&alloc);
    kir::KGraph          g3(&alloc);
    kir::KGraph*         graphs[4] = {&g0, &g1, &g2, &g3};
    constexpr int        rr   = 64;
    constexpr int        cc   = 64;
    constexpr int        tile = 16;
    const kir::Fft2dPlan plan = kir::build_fft2d_c2c(graphs, rr, cc, false, tile);

    int off[16];
    int total = 0;
    for (int b = 0; b < plan.nbuffers; ++b) { off[b] = total; total += plan.buffers[b].size; }
    crd::containers::Array<crd::f64> aref(&alloc);
    crd::containers::Array<crd::f64> acei(&alloc);
    aref.resize(static_cast<crd::usize>(total), 0.0);
    acei.resize(static_cast<crd::usize>(total), 0.0);
    crd::f64* href[16];
    crd::f64* hcei[16];
    for (int b = 0; b < plan.nbuffers; ++b)
    {
        href[b] = aref.data() + off[b];
        hcei[b] = acei.data() + off[b];
    }
    constexpr crd::f64 two_pi = 6.28318530717958647693;
    for (int i = 0; i < rr * cc; ++i)
    {
        href[plan.in_re][i] = static_cast<crd::f64>((i * 7 + 3) % 11 - 5);
        href[plan.in_im][i] = static_cast<crd::f64>((i * 5 + 1) % 7 - 3);
    }
    for (int k = 0; k < cc; ++k)
    {
        const crd::f64 a       = two_pi * static_cast<crd::f64>(k) / static_cast<crd::f64>(cc);
        href[plan.tw_col_re][k] = crd::math::cos(a);
        href[plan.tw_col_im][k] = -crd::math::sin(a);
    }
    for (int k = 0; k < rr; ++k)
    {
        const crd::f64 a       = two_pi * static_cast<crd::f64>(k) / static_cast<crd::f64>(rr);
        href[plan.tw_row_re][k] = crd::math::cos(a);
        href[plan.tw_row_im][k] = -crd::math::sin(a);
    }
    for (int i = 0; i < total; ++i) { acei[static_cast<crd::usize>(i)] = aref[static_cast<crd::usize>(i)]; } // identical start

    // PLAN driver (the independent reference).
    crd::kir_test::run_fft2d_cpu(plan, href, &alloc);

    // Build the CEIR asset + lower, then run the CPU reference executor over the LOWERED list.
    cgt::MultiPass mp[8];
    const char*    names[8] = {"p0", "p1", "p2", "p3", "p4", "p5", "p6", "p7"};
    for (int pi = 0; pi < plan.npasses; ++pi)
    {
        mp[pi].kernel = names[pi];
        mp[pi].nbind  = plan.passes[pi].nbind;
        mp[pi].access = (plan.passes[pi].nbind == 6) ? "r,r,r,r,w,w" : "r,w";
        mp[pi].grid   = static_cast<int>(plan.passes[pi].num_workgroups);
        for (int k = 0; k < plan.passes[pi].nbind; ++k) { mp[pi].bind[k] = plan.passes[pi].bind[k]; }
    }
    crd::memory::GrowableTlsfAllocator                croot;
    ce::Context                                 cctx(&croot);
    const cgt::CeirMultiAsset                   asset = cgt::build_ceir_multi_asset(cctx, plan.nbuffers, mp, plan.npasses);
    crd::containers::Array<ceg::LoweredCommand> cmds(&croot);
    ceg::lower_region(cctx, *asset.block, cmds);

    cgt::CpuKernelRef refs[8];
    for (int pi = 0; pi < plan.npasses; ++pi) { refs[pi] = {plan.passes[pi].graph, plan.passes[pi].entry}; }
    cgt::CpuMultiResolve cr{asset.dispatches, refs, plan.npasses};
    cgt::CpuBinding      cb[16];
    for (int b = 0; b < plan.nbuffers; ++b) { cb[b] = {asset.buffers[b], hcei[b], plan.buffers[b].size}; }

    const bool ok = cgt::execute_lowered_cpu(cctx, crd::containers::ConstSpan<ceg::LoweredCommand>(cmds.data(), cmds.size()),
                                             cgt::resolve_cpu_multi, &cr, cb, plan.nbuffers, &alloc);
    REQUIRE(ok);

    // ⭐ byte-equal: the lowered-list driver and the plan driver ran the SAME kernels in the SAME order.
    int bad = 0;
    for (int i = 0; i < total; ++i)
    {
        if (acei[static_cast<crd::usize>(i)] != aref[static_cast<crd::usize>(i)]) { ++bad; }
    }
    CHECK(bad == 0);
}
