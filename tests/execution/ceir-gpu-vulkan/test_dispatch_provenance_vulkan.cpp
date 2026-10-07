// DIAG.8a — a GPU validation error navigates back to the authored CEIR dispatch. The two-dispatch program of
// dispatch_provenance_fixture.hpp (authored as text under a named file, CSE, serialized, loaded into a fresh Context
// and lowered) runs on a real Vulkan device through execute_lowered with a DispatchSites table, so each dispatch is
// recorded inside its own debug label. In the error leg the second dispatch's authored grid asks for one more workgroup
// in Y than the device allows; Core validation reports it when the dispatch is recorded, the capture keeps the label
// identity, the site table maps it to the dispatch op, and that op resolves to its authored line. Such a recording is
// discarded and never submitted. The same holds for the generation a ReloadSet installed after a failed reload.
// Control: the program with its in-limit grid is clean when recorded and run, and every dispatch was labelled. Measured
// first: with this VVL (1.4.341), synchronization validation does not report a missing barrier between two compute
// dispatches over storage descriptors (shader accesses are only tracked with the syncval_shader_accesses_heuristic
// setting), so a dropped barrier cannot serve as the error. DIAG.8b (the last case): the same program recorded under a
// debugger session is non-pausable at the seam and still computes. Soft-skips without a Vulkan device. ASCII test
// names.

#include "../../gpu/gpu-shared/ceir_execute_1wg.hpp"
#include "../ceir-gpu/dispatch_provenance_fixture.hpp"

#include <crd/ceir/cook/hot_reload.hpp>
#include <crd/ceir/gpu/execute.hpp>
#include <crd/ceir/inspect.hpp>
#include <crd/gpu/identity_registry.hpp>
#include <crd/gpu/vulkan_compute_context.hpp>
#include <crd/gpu/vulkan_context.hpp>
#include <crd/gpu/vulkan_shader_compile.hpp>
#include <crd/gpu/vulkan_validation_capture.hpp>
#include <crd/kir/ckir.hpp>
#include <crd/kir/ckir_glsl.hpp>
#include <crd/memory/allocators/growable_tlsf_allocator.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string_view>
#include <vulkan/vulkan.h>

using namespace crd;                                     // NOLINT(google-build-using-namespace)
using namespace crd::ceir;                               // NOLINT(google-build-using-namespace)
using namespace crd::ceir::gpu;                          // NOLINT(google-build-using-namespace)
using namespace crd::ceir_gpu_test::dispatch_provenance; // NOLINT(google-build-using-namespace)
using crd::containers::Array;
using crd::containers::ConstSpan;
using crd::containers::String;
using crd::containers::StringView;

namespace
{
constexpr const char* kBrokenFile = "programs/diag/two_pass_broken.ceir";

// kTwoPass with the second dispatch's Y grid replaced by a constant of `groups_y`.
String over_limit_text(u64 groups_y, memory::IAllocator* alloc)
{
    const std::string_view base(kTwoPass);
    const std::string_view second("    compute.dispatch(%0, %0, %0, %3");
    const usize at = base.find(second);
    REQUIRE(at != std::string_view::npos);
    String text(alloc);
    text.append(base.substr(0U, at));
    text.append("    %5 = arith.const() {value = ");
    append_decimal(text, static_cast<u32>(groups_y));
    text.append("} : !index\n");
    text.append("    compute.dispatch(%0, %5, %0, %3");
    text.append(base.substr(at + second.size()));
    return text;
}

void reload_registrar(Context& ctx, void* /*user*/)
{
    register_dialects(ctx);
}

// The first error raised inside a Cerid debug label, or nullptr.
[[nodiscard]] const crd::gpu::ValidationMessage* first_labelled_error(const crd::gpu::ValidationCapture& capture)
{
    for (const auto& m : capture.messages())
    {
        if (m.severity == crd::gpu::ValidationSeverity::Error && m.label.valid())
        {
            return &m;
        }
    }
    return nullptr;
}

void log_messages(const crd::gpu::ValidationCapture& capture)
{
    for (const auto& m : capture.messages())
    {
        UNSCOPED_INFO("msg name=" << m.message_id_name.c_str() << " sev=" << static_cast<int>(m.severity)
                                  << " ident.valid=" << m.identity.valid()
                                  << " ident.kind=" << (m.identity.valid() ? static_cast<int>(m.identity.kind) : -1)
                                  << " label.valid=" << m.label.valid() << " text=" << m.message_text.c_str());
    }
}

// The device side of one leg: the compute context, the pipeline both dispatches use and four buffers.
struct Rig
{
    crd::gpu::VulkanGpuContext*               vk      = nullptr;
    crd::gpu::VulkanComputeContext*           compute = nullptr;
    crd::gpu::ComputePipeline*                pipe    = nullptr;
    std::unique_ptr<crd::gpu::ComputeBuffer>* buffers = nullptr; // [4]
};

// Record `commands` of `l` (lowered in `ctx`) with a site table, never submitted. The one labelled error must be the
// second dispatch's, and its op must resolve in `ctx` to `l.second_at` under `file`.
void expect_error_names_second_dispatch(Rig& rig, const Context& ctx, const Array<LoweredCommand>& commands,
                                        const Loaded& l, const char* file, memory::IAllocator* alloc)
{
    ResolvedBinding binds[4];
    for (usize i = 0; i < 4U; ++i)
    {
        binds[i] = ResolvedBinding{l.buffers[i], rig.buffers[i].get()};
    }
    crd::gpu::ValidationCapture capture(*rig.vk);
    DispatchSites sites(alloc);
    crd::gpu::ComputeRecorder& rec = rig.compute->begin();
    const ExecuteError err = execute_lowered(ctx, ConstSpan<LoweredCommand>(commands.data(), commands.size()), rec,
                                             &crd::ceir_gpu_test::resolve_single_pipeline, rig.pipe,
                                             ConstSpan<ResolvedBinding>(binds, 4U), &sites);
    log_messages(capture);
    REQUIRE(err == ExecuteError::None); // execute_lowered checks structure, not device limits
    REQUIRE(sites.sites().size() == 2U);
    CHECK(sites.sites()[0].labelled); // Vulkan opened both labels
    CHECK(sites.sites()[1].labelled);
    CHECK(capture.dropped_count() == 0U);

    const crd::gpu::ValidationMessage* const error = first_labelled_error(capture);
    REQUIRE(error != nullptr); // Core validation refused the dispatch inside a Cerid label
    const DispatchSite* const site = sites.find(error->label);
    REQUIRE(site != nullptr); // ... one this table minted
    CHECK(site->op == l.second);
    CHECK(site->command == 2U);
    const TextPos at = authored_at(ctx, site->op, alloc);
    CHECK(at.line == l.second_at.line);
    CHECK(at.col == l.second_at.col);
    const String rendered = render_op_site(ctx, site->op, alloc);
    String       want(alloc);
    want.append(file);
    want.append(":");
    append_decimal(want, l.second_at.line);
    want.append(":");
    append_decimal(want, l.second_at.col);
    UNSCOPED_INFO("site: " << rendered.c_str());
    CHECK(contains(rendered, StringView("compute.dispatch")));
    CHECK(contains(rendered, StringView(want.data(), want.size())));
    // The message's own identity still takes the object route (measured: the bound pipeline, a Program), and the
    // label survives beside it: that is what the capture's separate `label` keeps.
    UNSCOPED_INFO("object identity kind=" << (error->identity.valid() ? static_cast<int>(error->identity.kind) : -1));
    CHECK(error->identity.valid());
    CHECK_FALSE(error->identity == error->label);
    // Only the second dispatch was refused: no message names the first dispatch's label.
    for (const auto& m : capture.messages())
    {
        CHECK_FALSE(m.label == sites.sites()[0].label);
    }

    // Discard the recording: begin() resets the command buffer, so the over-limit dispatch never runs.
    (void)rig.compute->begin();
}
} // namespace

TEST_CASE("diag 8a: a validation error at a CEIR dispatch names the authored dispatch through its label",
          "[ceir][ceir-gpu][vulkan][gpu][diag][validation]")
{
    namespace gpu  = crd::gpu;
    namespace cook = crd::ceir::cook;
    gpu::GpuContextConfig cfg{};
    cfg.backend           = gpu::GpuBackend::Vulkan;
    cfg.headless          = true;
    cfg.enable_validation = true; // Core: reports the over-limit grid and enables debug-utils labels
    auto ctx              = gpu::create_vulkan_gpu_context(cfg);
    if (ctx == nullptr)
    {
        WARN("no Vulkan device available; skipping");
        return;
    }
    auto* vk = static_cast<gpu::VulkanGpuContext*>(ctx.get());
    REQUIRE(vk->valid());
    REQUIRE(gpu::validation_layer_spec_version() != 0U);

    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(vk->vk_physical_device(), &props);
    const u64 limit_y = props.limits.maxComputeWorkGroupCount[1];
    REQUIRE(limit_y < 0xFFFFFFFFULL); // one more must still be a u32 grid

    crd::memory::TlsfAllocator alloc(32U << 20U);
    {
        gpu::VulkanComputeContext compute(*vk, &alloc);
        REQUIRE(compute.valid());

        // One add kernel (3 storage bindings) serves both dispatches; the kernel symbols only name the CKIR kernel.
        constexpr int          n = 64;
        crd::kir::KGraph       g(&alloc);
        const crd::kir::KEntry e = crd::ceir_gpu_test::build_add_kernel(g, n);
        crd::kir::GlslKernel   kern(&alloc);
        REQUIRE(crd::kir::emit_compute_kernel_glsl(g, e, &alloc, kern));
        const auto cres = gpu::compile_glsl_to_spirv(gpu::ShaderStage::Compute, crd::containers::to_view(kern.source),
                                                     "diag_two_pass", &alloc);
        REQUIRE(cres.ok);
        auto pipe = compute.create_pipeline_from_spirv(ConstSpan<u8>(cres.spirv.data(), cres.spirv.size()), 3, 0U);
        REQUIRE(pipe != nullptr);

        std::unique_ptr<gpu::ComputeBuffer> buffers[4];
        for (auto& b : buffers)
        {
            b = compute.create_buffer(static_cast<u64>(n) * 4U, gpu::compute_usage::storage,
                                      gpu::ComputeMemory::GpuOnly);
            REQUIRE(b != nullptr);
        }
        Rig rig{vk, &compute, pipe.get(), buffers};

        crd::memory::GrowableTlsfAllocator croot;
        const usize live_before = gpu::identity_registry().live_count(gpu::ObjectKind::Pass);
        const String over       = over_limit_text(limit_y + 1U, &croot);
        const StringView over_src(over.data(), over.size());

        // (a) ERROR: the second dispatch's authored Y grid is one past the device limit. Recorded, never submitted.
        {
            Context               loaded(&croot);
            Array<LoweredCommand> commands(&croot);
            Loaded                l;
            author_and_load(loaded, commands, l, &croot, over_src);
            REQUIRE(commands.size() == 3U);
            REQUIRE(commands[2].groups_y == limit_y + 1U); // the authored grid reached the lowered command
            expect_error_names_second_dispatch(rig, loaded, commands, l, kFile, &croot);
        }

        // (b) AFTER A FAILED RELOAD: the same program is cooked and installed under kFile; a reload with an unparsable
        // source under another file fails and keeps that generation, whose dispatch still names its own authored line.
        {
            cook::ReloadSet     rs(&croot, &reload_registrar, nullptr);
            const cook::AssetId id{1U};
            REQUIRE(rs.add_source(id, over_src, StringView(kFile)).ok());
            const cook::ReloadResult rr =
                rs.reload_source(id, StringView("module {\n  ^bb0:\n    not an op\n}\n"), StringView(kBrokenFile));
            CHECK_FALSE(rr.installed);
            CHECK(rr.cook_error != cook::CookError::Ok);
            const cook::Generation* const gen = rs.generation(id);
            REQUIRE(gen != nullptr);
            REQUIRE(gen->ctx != nullptr);
            REQUIRE(gen->program.module != nullptr);
            Block* const block = gen->program.module->body()->first_block();
            REQUIRE(block != nullptr);
            Array<LoweredCommand> commands(&croot);
            Loaded                l;
            find_dispatches(over_src, l);
            collect_and_lower(*gen->ctx, *block, commands, l);
            REQUIRE(commands.size() == 3U);
            expect_error_names_second_dispatch(rig, *gen->ctx, commands, l, kFile, &croot);
        }

        // (c) CONTROL: the authored in-limit program records and runs clean, and both dispatches were labelled.
        {
            Context               loaded(&croot);
            Array<LoweredCommand> commands(&croot);
            Loaded                l;
            author_and_load(loaded, commands, l, &croot);
            ResolvedBinding binds[4];
            for (usize i = 0; i < 4U; ++i)
            {
                binds[i] = ResolvedBinding{l.buffers[i], buffers[i].get()};
            }

            gpu::ValidationCapture capture(*vk);
            DispatchSites          sites(&croot);
            gpu::ComputeRecorder&  rec = compute.begin();
            const ExecuteError     err =
                execute_lowered(loaded, ConstSpan<LoweredCommand>(commands.data(), commands.size()), rec,
                                &crd::ceir_gpu_test::resolve_single_pipeline, pipe.get(),
                                ConstSpan<ResolvedBinding>(binds, 4U), &sites);
            compute.submit_and_wait();
            log_messages(capture);
            REQUIRE(err == ExecuteError::None);
            CHECK(capture.error_count() == 0U);
            CHECK(first_labelled_error(capture) == nullptr);
            REQUIRE(sites.sites().size() == 2U);
            CHECK(sites.sites()[0].labelled);
            CHECK(sites.sites()[1].labelled);
        }

        // Each table retired the identities it minted.
        CHECK(gpu::identity_registry().live_count(gpu::ObjectKind::Pass) == live_before);

        // The layer retires submissions on its own queue thread; idle the device before the context and fence go.
        REQUIRE(vkDeviceWaitIdle(vk->vk_device()) == VK_SUCCESS);
    }
}

// DIAG.8b — the same authored two-dispatch program recorded on a real Vulkan device under a debugger session. The
// session's scope is Task and a breakpoint is bound to the second dispatch's authored line, yet GPU work is
// non-pausable at the execute_lowered seam: the hit is counted and refused NonPausable, the recording never stops, and
// the submitted work is validation-clean and computes the authored result (out = a + 2b: the first dispatch writes
// c = a + b, the second out = c + b). This thread is declared the controller, so a seam that tried to pause here would
// be refused SameThread instead of hanging, and the refusal kind tells the two apart.
TEST_CASE("diag 8b: a GPU dispatch recorded under a task session is non-pausable and still computes",
          "[ceir][ceir-gpu][vulkan][gpu][diag][validation]")
{
    namespace gpu  = crd::gpu;
    namespace insp = crd::ceir::inspect;
    using gpu::compute_usage::storage;
    using gpu::compute_usage::transfer_dst;
    using gpu::compute_usage::transfer_src;
    gpu::GpuContextConfig cfg{};
    cfg.backend           = gpu::GpuBackend::Vulkan;
    cfg.headless          = true;
    cfg.enable_validation = true;
    auto ctx              = gpu::create_vulkan_gpu_context(cfg);
    if (ctx == nullptr)
    {
        WARN("no Vulkan device available; skipping");
        return;
    }
    auto* vk = static_cast<gpu::VulkanGpuContext*>(ctx.get());
    REQUIRE(vk->valid());

    crd::memory::TlsfAllocator alloc(32U << 20U);
    {
        gpu::VulkanComputeContext compute(*vk, &alloc);
        REQUIRE(compute.valid());

        constexpr int          n = 64;
        crd::kir::KGraph       g(&alloc);
        const crd::kir::KEntry e = crd::ceir_gpu_test::build_add_kernel(g, n);
        crd::kir::GlslKernel   kern(&alloc);
        REQUIRE(crd::kir::emit_compute_kernel_glsl(g, e, &alloc, kern));
        const auto cres = gpu::compile_glsl_to_spirv(gpu::ShaderStage::Compute, crd::containers::to_view(kern.source),
                                                     "diag_two_pass_inspect", &alloc);
        REQUIRE(cres.ok);
        auto pipe = compute.create_pipeline_from_spirv(ConstSpan<u8>(cres.spirv.data(), cres.spirv.size()), 3, 0U);
        REQUIRE(pipe != nullptr);

        constexpr u64                       bytes = static_cast<u64>(n) * 4U;
        std::unique_ptr<gpu::ComputeBuffer> dev[4];
        for (auto& b : dev)
        {
            b = compute.create_buffer(bytes, storage | transfer_dst | transfer_src, gpu::ComputeMemory::GpuOnly);
            REQUIRE(b != nullptr);
        }
        std::unique_ptr<gpu::ComputeBuffer> up[2];
        for (int b = 0; b < 2; ++b)
        {
            up[b] = compute.create_buffer(bytes, transfer_src, gpu::ComputeMemory::CpuToGpu);
            REQUIRE(up[b] != nullptr);
            auto* p = static_cast<float*>(up[b]->map());
            REQUIRE(p != nullptr);
            for (int i = 0; i < n; ++i)
            {
                p[i] = (b == 0) ? static_cast<float>(i) : 1.5F; // a = i, b = 1.5
            }
            up[b]->unmap();
        }
        auto rb = compute.create_buffer(bytes, transfer_dst, gpu::ComputeMemory::GpuToCpu);
        REQUIRE(rb != nullptr);

        crd::memory::GrowableTlsfAllocator croot;
        Context                            loaded(&croot);
        Array<LoweredCommand>              commands(&croot);
        Loaded                             l;
        author_and_load(loaded, commands, l, &croot);
        REQUIRE(commands.size() == 3U);
        ResolvedBinding binds[4];
        for (usize i = 0; i < 4U; ++i)
        {
            binds[i] = ResolvedBinding{l.buffers[i], dev[i].get()};
        }

        constexpr u64 generation = 3U;
        insp::Session s(&croot, insp::PauseScope::Task);
        s.connect_controller();
        u32 bp = 0U;
        REQUIRE(s.add_line_breakpoint(StringView(kFile), l.second_at.line, bp) == insp::Refusal::None);
        Array<insp::BindReport> rep(&croot);
        REQUIRE(s.bind(*l.module, loaded, generation, rep) == insp::Refusal::None);
        REQUIRE(rep.size() == 1U);
        REQUIRE(rep[0].status == insp::BindStatus::Bound);
        CHECK(rep[0].first_op == l.second->stable_id());

        gpu::ValidationCapture capture(*vk);
        DispatchSites          sites(&croot);
        DeviceInspect          di{&s, generation, insp::Refusal::Busy};
        gpu::ComputeRecorder&  rec = compute.begin();
        for (int b = 0; b < 2; ++b)
        {
            rec.copy(*up[b], *dev[b], 0U, 0U, bytes);
            rec.barrier(*dev[b], gpu::ComputeAccess::TransferDst, gpu::ComputeAccess::ShaderRead);
        }
        const ExecuteError err =
            execute_lowered(loaded, ConstSpan<LoweredCommand>(commands.data(), commands.size()), rec,
                            &crd::ceir_gpu_test::resolve_single_pipeline, pipe.get(),
                            ConstSpan<ResolvedBinding>(binds, 4U), &sites, &di);
        rec.barrier(*dev[3], gpu::ComputeAccess::ShaderWrite, gpu::ComputeAccess::TransferSrc);
        rec.copy(*dev[3], *rb, 0U, 0U, bytes);
        compute.submit_and_wait();
        log_messages(capture);

        REQUIRE(err == ExecuteError::None);
        CHECK(di.refusal == insp::Refusal::None);
        CHECK(s.device_hits() == 1U); // the bound dispatch was a hit ...
        CHECK(s.refused_pauses() == 1U);
        CHECK(s.last_refusal() == insp::Refusal::NonPausable); // ... refused by the seam, not SameThread
        insp::StopRecord stop;
        CHECK(s.wait_for_stop(generation, 0U, stop) == insp::Refusal::Finished); // and nothing ever stopped
        CHECK(capture.error_count() == 0U);
        REQUIRE(sites.sites().size() == 2U);
        CHECK(sites.sites()[1].op == l.second);
        CHECK(sites.sites()[1].labelled);

        const auto* r = static_cast<const float*>(rb->map());
        REQUIRE(r != nullptr);
        int wrong = 0;
        for (int i = 0; i < n; ++i)
        {
            if (r[i] != static_cast<float>(i) + 3.0F)
            {
                ++wrong;
            }
        }
        rb->unmap();
        CHECK(wrong == 0);

        REQUIRE(vkDeviceWaitIdle(vk->vk_device()) == VK_SUCCESS);
    }
}
