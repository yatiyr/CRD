#include <crd/gpu/dx12_context.hpp>
#include <crd/gpu/dx12_compute_context.hpp>
#include <crd/gpu/dx12_raster_context.hpp>
#include <crd/gpu/dx12_ray_tracing_context.hpp>
#include <crd/gpu/dx12_validation_capture.hpp>
#include <crd/gpu/dx12_work_graph_context.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>
#include <crd/gpu/frame_graph.hpp>

#include <ckir_vertex_pull.hpp>
#include <ckir_raster_triangle.hpp>
#include <crd/kir/ckir_rt.hpp>   // DIAG.7a(d2b-dx12-b) batch 3d: build_rt_pipeline_{raygen,miss,closesthit}
#include <crd/kir/ckir_hlsl.hpp> // DIAG.7a(d2b-dx12-b) batch 3d: emit_rt_stage_hlsl
#include <verb_packet_helpers.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstring>
#include <cstdio>
#include <thread>

#include "dx12_device_scope.hpp"
#include "dx12_execution.hpp"
#include "dx12_frame_descriptors.hpp"
#include "dx12_identity_naming.hpp" // DIAG.7a(d2b-dx12-a): the attach/detach helper under test
#include <crd/gpu/identity_registry.hpp> // DIAG.7a(d2b): identity_registry() + alive()/live_count() the tests assert on

#include <d3d12sdklayers.h>
#include <dxgi1_4.h>

namespace
{
namespace g = crd::gpu;
using Microsoft::WRL::ComPtr;

class RejectAllocator final : public crd::memory::IAllocator
{
public:
    void* allocate(crd::usize, crd::usize) override { return nullptr; }
    void* try_allocate(crd::usize, crd::usize) override { return nullptr; }
    void deallocate(void*) noexcept override {}
    [[nodiscard]] bool owns(const void*) const noexcept override { return false; }
};

class DescriptorAllocator final : public crd::memory::IAllocator
{
public:
    bool reject = false;
    crd::usize live = 0U;
    void* allocate(crd::usize bytes, crd::usize alignment) override
    {
        void* storage = m_backing.allocate(bytes, alignment);
        if (storage != nullptr) { ++live; }
        return storage;
    }
    void* try_allocate(crd::usize bytes, crd::usize alignment) override
    {
        if (reject) { return nullptr; }
        void* storage = m_backing.try_allocate(bytes, alignment);
        if (storage != nullptr) { ++live; }
        return storage;
    }
    void deallocate(void* storage) noexcept override
    {
        if (storage != nullptr) { --live; }
        m_backing.deallocate(storage);
    }
    [[nodiscard]] bool owns(const void* storage) const noexcept override { return m_backing.owns(storage); }
private:
    crd::memory::TlsfAllocator m_backing{crd::usize{4} << 20U, nullptr, "descriptor test pages"};
};

void build_increment_kernel(crd::kir::KGraph& graph, crd::kir::KEntry& entry)
{
    namespace k = crd::kir;
    const auto shape = k::make_shape({1});
    const int buffer = graph.buffer_decl(k::DType::U32, 0, 0, true);
    const int zero = graph.constant(0.0, shape, k::DType::U32);
    const int one = graph.constant(1.0, shape, k::DType::U32);
    graph.stmt_buffer_store(buffer, zero, graph.binary(k::KOp::Add, graph.buffer_load(buffer, zero), one));
    entry.stage = k::KStage::Compute;
    entry.local_size[0] = 1U;
    entry.kernel_body_begin = 0;
    entry.kernel_body_count = static_cast<int>(graph.serial_stmts().size());
}

void print_messages(const g::Dx12ValidationCapture& capture)
{
    const auto report = capture.report();
    std::printf("capture: info=%llu warnings=%llu errors=%llu instrumentation=%llu first=%u result=%08X detail=%llu\n",
                static_cast<unsigned long long>(report.info), static_cast<unsigned long long>(report.warnings),
                static_cast<unsigned long long>(report.errors), static_cast<unsigned long long>(report.instrumentation_failures),
                static_cast<unsigned>(report.first_issue), static_cast<unsigned>(report.first_issue_result),
                static_cast<unsigned long long>(report.first_issue_detail));
    for (crd::u32 index = 0; index < report.messages; ++index)
    {
        g::Dx12ValidationMessage message;
        REQUIRE(capture.message(index, message));
        if (message.severity != g::Dx12ValidationSeverity::Info)
        {
            std::printf("DX12 diagnostic %u: %s\n", message.id, message.text);
            UNSCOPED_INFO("DX12 diagnostic " << message.id << ": " << message.text);
        }
    }
    UNSCOPED_INFO("instrument failures=" << report.instrumentation_failures << ", dropped=" << report.dropped
                  << ", truncated=" << report.truncated << ", contexts=" << report.contexts_started
                  << "/" << report.contexts_finished);
}
} // namespace

TEST_CASE("DX12 validation rejects invalid capacity and allocation failure", "[dx12][validation]")
{
    RejectAllocator allocator;
    g::Dx12ValidationCapture invalid(&allocator, 0U);
    CHECK(invalid.report().readiness == g::Dx12ValidationReadiness::InvalidCapacity);
    g::Dx12ValidationCapture too_large(&allocator, 4097U);
    CHECK(too_large.report().readiness == g::Dx12ValidationReadiness::InvalidCapacity);
    g::Dx12ValidationCapture failed(&allocator);
    CHECK(failed.report().readiness == g::Dx12ValidationReadiness::AllocationFailed);
    CHECK_FALSE(failed.report().complete_and_silent());
    g::Dx12ValidationMessage message;
    CHECK_FALSE(failed.message(0U, message));
}

TEST_CASE("DX12 validation captures invalid command close without submitting it", "[dx12][validation]")
{
    crd::memory::TlsfAllocator allocator(64U << 20U, nullptr, "DX12 validation test");
    g::Dx12ValidationCapture capture(&allocator);
    REQUIRE(capture.report().readiness == g::Dx12ValidationReadiness::Ready);
    {
        g::detail::Dx12DeviceScope lifetime;
        ComPtr<ID3D12Device> device;
        REQUIRE(SUCCEEDED(lifetime.create(device)));
        ComPtr<ID3D12CommandQueue> queue;
        const D3D12_COMMAND_QUEUE_DESC queue_desc{};
        REQUIRE(SUCCEEDED(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue))));
        ComPtr<ID3D12CommandAllocator> commands;
        REQUIRE(SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&commands))));
        ComPtr<ID3D12GraphicsCommandList> list;
        REQUIRE(SUCCEEDED(device->CreateCommandList(0U, D3D12_COMMAND_LIST_TYPE_DIRECT, commands.Get(), nullptr,
                                                   IID_PPV_ARGS(&list))));
        ComPtr<ID3D12Fence> fence;
        REQUIRE(SUCCEEDED(device->CreateFence(0U, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))));
        REQUIRE(SUCCEEDED(list->Close()));
        UINT64 value = 0U;
        bool submitted = true;
        // Intentional bounded misuse: closing an already closed list fails before any GPU work is submitted.
        CHECK(FAILED(g::detail::dx12_submit(device.Get(), queue.Get(), list.Get(), fence.Get(), value, submitted)));
        CHECK_FALSE(submitted);
        CHECK(value == 0U);
        CHECK(fence->GetCompletedValue() == 0U);
        CHECK(SUCCEEDED(device->GetDeviceRemovedReason()));
    }
    CHECK(capture.report().errors >= 2U); // Native debug-layer error AND explicit command-lifecycle failure.
    CHECK(capture.report().execution_failures == 1U);
    CHECK(capture.report().contexts_started == 1U);
    CHECK(capture.report().contexts_finished == 1U);
    CHECK(capture.report().instrumentation_failures == 0U);
    CHECK_FALSE(capture.report().complete_and_silent());
}

TEST_CASE("DX12 validation refuses late enablement without removing a live device", "[dx12][validation]")
{
    crd::memory::TlsfAllocator allocator(64U << 20U, nullptr, "DX12 late validation test");
    g::detail::Dx12DeviceScope lifetime;
    ComPtr<ID3D12Device> device;
    REQUIRE(SUCCEEDED(lifetime.create(device)));
    g::Dx12ValidationCapture late(&allocator);
    CHECK(late.report().readiness == g::Dx12ValidationReadiness::StartedAfterDevice);
    CHECK_FALSE(late.report().complete_and_silent());
    CHECK(SUCCEEDED(device->GetDeviceRemovedReason()));
}

TEST_CASE("DX12 validation distinguishes removed device from fence completion", "[dx12][validation]")
{
    crd::memory::TlsfAllocator allocator(64U << 20U, nullptr, "DX12 lost device validation test");
    g::Dx12ValidationCapture capture(&allocator);
    REQUIRE(capture.report().readiness == g::Dx12ValidationReadiness::Ready);
    {
        ComPtr<IDXGIFactory4> factory;
        REQUIRE(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))));
        ComPtr<IDXGIAdapter1> warp;
        REQUIRE(SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp))));
        g::detail::Dx12DeviceScope lifetime;
        ComPtr<ID3D12Device> device;
        REQUIRE(SUCCEEDED(lifetime.create(device, warp.Get())));
        ComPtr<ID3D12Device5> control;
        REQUIRE(SUCCEEDED(device.As(&control)));
        ComPtr<ID3D12Fence> fence;
        REQUIRE(SUCCEEDED(device->CreateFence(0U, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))));
        const HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        REQUIRE(event != nullptr);
        // Only this process's logical WARP device is removed. No submitted workload or system adapter reset.
        control->RemoveDevice();
        const HRESULT result = g::detail::dx12_wait(device.Get(), fence.Get(), 0U, event);
        CHECK(CloseHandle(event) != FALSE);
        CHECK(FAILED(result));
        CHECK(FAILED(device->GetDeviceRemovedReason()));
    }
    CHECK(capture.report().execution_failures == 1U);
    CHECK(capture.report().contexts_finished == 1U);
    CHECK_FALSE(capture.report().complete_and_silent());
}

TEST_CASE("DX12 validation rejects timeout and premature fence wakeup", "[dx12][validation]")
{
    // A signalled event is insufficient proof of completion; a timeout also cannot release live GPU resources.
    bool premature = false;
    SECTION("unsignalled completion expires within the caller budget") {}
    SECTION("premature event cannot masquerade as completed GPU work") { premature = true; }
    crd::memory::TlsfAllocator allocator(64U << 20U, nullptr, "DX12 bounded wait test");
    g::Dx12ValidationCapture capture(&allocator);
    REQUIRE(capture.report().readiness == g::Dx12ValidationReadiness::Ready);
    {
        ComPtr<IDXGIFactory4> factory;
        REQUIRE(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))));
        ComPtr<IDXGIAdapter1> warp;
        REQUIRE(SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp))));
        g::detail::Dx12DeviceScope lifetime;
        ComPtr<ID3D12Device> device;
        REQUIRE(SUCCEEDED(lifetime.create(device, warp.Get())));
        ComPtr<ID3D12Fence> fence;
        REQUIRE(SUCCEEDED(device->CreateFence(0U, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))));
        const HANDLE event = CreateEventW(nullptr, FALSE, premature ? TRUE : FALSE, nullptr);
        REQUIRE(event != nullptr);
        const HRESULT result = g::detail::dx12_wait(device.Get(), fence.Get(), 1U, event, 5U);
        CHECK(CloseHandle(event) != FALSE);
        CHECK(result == (premature ? E_UNEXPECTED : HRESULT_FROM_WIN32(ERROR_TIMEOUT)));
        CHECK(FAILED(device->GetDeviceRemovedReason())); // Failure retires the logical device before caller cleanup.
    }
    CHECK(capture.report().execution_failures == 1U);
    CHECK(capture.report().contexts_finished == 1U);
}

TEST_CASE("DX12 validation bounds concurrent callbacks and observer lifetimes", "[dx12][validation]")
{
    crd::memory::TlsfAllocator allocator(64U << 20U, nullptr, "DX12 concurrent validation test");
    g::Dx12ValidationCapture bounded(&allocator, 2U);
    g::Dx12ValidationCapture complete(&allocator, 1024U);
    REQUIRE(bounded.report().readiness == g::Dx12ValidationReadiness::Ready);
    REQUIRE(complete.report().readiness == g::Dx12ValidationReadiness::Ready);
    {
        g::detail::Dx12DeviceScope first;
        ComPtr<ID3D12Device> device;
        REQUIRE(SUCCEEDED(first.create(device)));
        g::detail::Dx12DeviceScope second;
        ComPtr<ID3D12Device> shared;
        REQUIRE(SUCCEEDED(second.create(shared)));
        REQUIRE(device.Get() == shared.Get());
        ComPtr<ID3D12InfoQueue> queue;
        REQUIRE(SUCCEEDED(device.As(&queue)));
        const auto before = complete.report().warnings;
        std::atomic<crd::u32> failed{0U};
        {
            std::jthread workers[4];
            for (auto& worker : workers)
            {
                worker = std::jthread([&]
                {
                    for (crd::u32 index = 0; index < 100U; ++index)
                    {
                        if (FAILED(queue->AddApplicationMessage(D3D12_MESSAGE_SEVERITY_WARNING, "concurrent sentinel")))
                        {
                            failed.fetch_add(1U);
                        }
                        (void)complete.report();
                    }
                });
            }
        }
        CHECK(failed.load() == 0U);
        CHECK(complete.report().warnings - before == 400U); // Shared device has one callback, not duplicate observers.
        CHECK(bounded.report().messages == 2U);
        CHECK(bounded.report().dropped >= 398U);
        CHECK(complete.report().dropped == 0U);
        char long_text[2048];
        std::memset(long_text, 'x', sizeof(long_text));
        long_text[sizeof(long_text) - 1U] = '\0';
        REQUIRE(SUCCEEDED(queue->AddApplicationMessage(D3D12_MESSAGE_SEVERITY_WARNING, long_text)));
        CHECK(complete.report().truncated != 0U);
        {
            g::Dx12ValidationCapture late(&allocator);
            CHECK(late.report().readiness == g::Dx12ValidationReadiness::StartedAfterDevice);
            CHECK_FALSE(late.report().complete_and_silent());
        }
        // Late observer is destroyed while the device callback remains registered; subsequent delivery is safe.
        REQUIRE(SUCCEEDED(queue->AddApplicationMessage(D3D12_MESSAGE_SEVERITY_INFO, "after observer destruction")));
    }
    CHECK(complete.report().contexts_started == 2U);
    CHECK(complete.report().contexts_finished == 2U);
    CHECK(complete.report().active_contexts == 0U);
    CHECK(complete.report().instrumentation_failures == 0U);
}

TEST_CASE("DX12 validation covers separate default and WARP devices", "[dx12][validation]")
{
    crd::memory::TlsfAllocator allocator(64U << 20U, nullptr, "DX12 device validation test");
    g::Dx12ValidationCapture capture(&allocator);
    REQUIRE(capture.report().readiness == g::Dx12ValidationReadiness::Ready);
    {
        g::detail::Dx12DeviceScope first;
        ComPtr<ID3D12Device> device;
        REQUIRE(SUCCEEDED(first.create(device)));
        ComPtr<IDXGIFactory4> factory;
        REQUIRE(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))));
        ComPtr<IDXGIAdapter1> warp;
        REQUIRE(SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp))));
        g::detail::Dx12DeviceScope second;
        ComPtr<ID3D12Device> software;
        REQUIRE(SUCCEEDED(second.create(software, warp.Get())));
        for (auto* native : {device.Get(), software.Get()})
        {
            ComPtr<ID3D12InfoQueue> queue;
            REQUIRE(SUCCEEDED(native->QueryInterface(IID_PPV_ARGS(&queue))));
            const auto before = capture.report().errors;
            REQUIRE(SUCCEEDED(queue->AddApplicationMessage(D3D12_MESSAGE_SEVERITY_ERROR, "device sentinel")));
            CHECK(capture.report().errors > before);
        }
    }
    CHECK(capture.report().contexts_started == 2U);
    CHECK(capture.report().contexts_finished == 2U);
    CHECK(capture.report().instrumentation_failures == 0U);
}

TEST_CASE("DX12 validation qualifies production compute copy and context teardown", "[dx12][validation]")
{
    crd::memory::TlsfAllocator allocator(64U << 20U, nullptr, "DX12 production validation test");
    g::Dx12ValidationCapture capture(&allocator);
    REQUIRE(capture.report().readiness == g::Dx12ValidationReadiness::Ready);
    CHECK_FALSE(capture.report().complete_and_silent());
    {
        auto programs = g::create_dx12_gpu_context(&allocator);
        REQUIRE(programs != nullptr);
        auto raster = g::create_dx12_raster_context();
        REQUIRE(raster != nullptr);
        g::Dx12RayTracingContext rays;
        g::Dx12WorkGraphContext work_graphs; // Unsupported features may be absent; their device lifecycle is still covered.
        g::Dx12ComputeContext compute(&allocator);
        REQUIRE(compute.valid());
        auto source = compute.create_buffer(sizeof(crd::u32), 0U, g::ComputeMemory::CpuToGpu);
        auto destination = compute.create_buffer(sizeof(crd::u32), 0U, g::ComputeMemory::GpuToCpu);
        REQUIRE(source != nullptr);
        REQUIRE(destination != nullptr);
        auto* mapped = static_cast<crd::u32*>(source->map());
        REQUIRE(mapped != nullptr);
        *mapped = 0xC3D007U;
        source->unmap();
        compute.begin().copy(*source, *destination, 0U, 0U, sizeof(crd::u32));
        compute.submit_and_wait();
        REQUIRE(compute.valid());
        mapped = static_cast<crd::u32*>(destination->map());
        REQUIRE(mapped != nullptr);
        CHECK(*mapped == 0xC3D007U);
        destination->unmap();
        CHECK(capture.report().active_contexts == 5U);
        CHECK_FALSE(capture.report().complete_and_silent());
    }
    print_messages(capture);
    CHECK(capture.report().contexts_started == 5U);
    CHECK(capture.report().complete_and_silent());
}

TEST_CASE("DX12 resource states restore indexed indirect buffers before compute and repeated frames",
          "[dx12][validation][resource-states]")
{
    namespace k = crd::kir;
    crd::memory::TlsfAllocator allocator(64U << 20U, nullptr, "DX12 resource state test");
    g::Dx12ValidationCapture capture(&allocator, 4096U);
    REQUIRE(capture.report().readiness == g::Dx12ValidationReadiness::Ready);
    {
        auto gpu = g::create_dx12_gpu_context(&allocator);
        auto raster = g::create_dx12_raster_context();
        REQUIRE(gpu != nullptr);
        REQUIRE(raster != nullptr);
        k::KGraph vertex(&allocator);
        k::KGraph fragment(&allocator);
        k::KGraph compute(&allocator);
        k::KEntry vertex_entry;
        k::KEntry fragment_entry;
        k::KEntry compute_entry;
        crd::gputest::build_vertex_pull_vs(vertex, vertex_entry);
        crd::gputest::build_triangle_fs(fragment, fragment_entry);
        const auto shape = k::make_shape({1});
        const int buffer = compute.buffer_decl(k::DType::U32, 0, 0, true);
        const int word = compute.constant(448.0, shape, k::DType::U32);
        const int one = compute.constant(1.0, shape, k::DType::U32);
        const int previous = compute.buffer_load(buffer, word);
        compute.stmt_buffer_store(buffer, word, compute.binary(k::KOp::Add, previous, one));
        compute_entry.stage = k::KStage::Compute;
        compute_entry.local_size[0] = 1U;
        compute_entry.kernel_body_begin = 0;
        compute_entry.kernel_body_count = static_cast<int>(compute.serial_stmts().size());
        auto vs = gpu->create_program(vertex, vertex_entry);
        auto fs = gpu->create_program(fragment, fragment_entry);
        auto kernel = gpu->create_program(compute, compute_entry);
        REQUIRE(vs != nullptr);
        REQUIRE(fs != nullptr);
        REQUIRE(kernel != nullptr);
        auto program = raster->create_raster_program(*vs, *fs);
        REQUIRE(program != nullptr);

        k::KGraph depth_fragment(&allocator);

        k::KGraph sample_vertex(&allocator);

        k::KGraph sample_fragment(&allocator);
        k::KEntry de;
        k::KEntry sve;
        k::KEntry sfe;
        crd::gputest::build_depth_only_const_fs(depth_fragment, de, 0.75);
        crd::gputest::build_textured_vs(sample_vertex, sve);
        crd::gputest::build_shadow_fs(sample_fragment, sfe);
        auto df = gpu->create_program(depth_fragment, de);
        auto sv = gpu->create_program(sample_vertex, sve);
        auto sf = gpu->create_program(sample_fragment, sfe);
        REQUIRE(df != nullptr); REQUIRE(sv != nullptr); REQUIRE(sf != nullptr);
        auto depth_program = raster->create_raster_program(*vs, *df);
        auto sample_program = raster->create_raster_program(*sv, *sf);
        REQUIRE(depth_program != nullptr); REQUIRE(sample_program != nullptr);

        // Both color and depth: no count, distinct count, args=count, scene=count, scene=args, all shared.
        for (crd::u32 path = 0; path != 12U; ++path)
        {
            const crd::u32 mode = path % 6U;
            const bool depth_only = path >= 6U;
            CAPTURE(mode, depth_only);
            auto scene = raster->create_storage_buffer(2048U);
            auto arguments = raster->create_storage_buffer(2048U);
            auto counter = raster->create_storage_buffer(2048U);
            REQUIRE(scene != nullptr);
            REQUIRE(arguments != nullptr);
            REQUIRE(counter != nullptr);
            g::IStorageBuffer* args = mode >= 4U ? scene.get() : arguments.get();
            g::IStorageBuffer* count = counter.get();
            if (mode == 0U) { count = nullptr; }
            else if (mode == 2U) { count = args; }
            else if (mode == 3U || mode == 5U) { count = scene.get(); }
            float vertices[36]{};
            vertices[0] = -0.8F; vertices[1] = -0.8F; vertices[2] = 0.5F;
            vertices[12] = 0.8F; vertices[13] = -0.8F; vertices[14] = 0.5F;
            vertices[24] = 0.0F; vertices[25] = 0.8F; vertices[26] = 0.5F;
            const crd::u32 indices[3] = {0U, 1U, 2U};
            const crd::u32 command[6] = {0U, 3U, 1U, 0U, 0U, 0U};
            const crd::u32 count_value = 1U;
            REQUIRE(raster->indirect_command_stride() == sizeof(command));
            REQUIRE(raster->upload_storage(*scene, 0U, vertices, sizeof(vertices)));
            REQUIRE(raster->upload_storage(*scene, 768U, indices, sizeof(indices)));
            REQUIRE(raster->upload_storage(*args, 1024U, command, sizeof(command)));
            if (count != nullptr) { REQUIRE(raster->upload_storage(*count, 1536U, &count_value, sizeof(count_value))); }
            auto target = raster->create_color_target(32U, 32U);
            auto graph = raster->create_frame_graph();
            REQUIRE(target != nullptr);
            REQUIRE(graph != nullptr);
            const auto output = graph->import_target(*target);
            g::FgImageDesc depth_desc{};
            depth_desc.width = 32U; depth_desc.height = 32U;
            depth_desc.format = g::FgImageFormat::D32Float; depth_desc.sampled = true;
            const auto image = depth_only ? graph->create_transient_image(depth_desc) : output;
            REQUIRE(image.valid());
            struct DrawState
            {
                g::FgImage image;
                g::IRasterProgram* program;
                g::IGpuProgram* kernel;
                g::IStorageBuffer* buffers[3];
                bool depth_only;
            } state{image, depth_only ? depth_program.get() : program.get(), kernel.get(),
                    {scene.get(), args, count}, depth_only};
            graph->add_pass("repeat-indirect-then-compute").writes(state.image).execute(
                [](g::IFrameContext& context, void* user) {
                    auto& draw = *static_cast<DrawState*>(user);
                    for (crd::u32 repeat = 0; repeat != 2U; ++repeat)
                    {
                        if (draw.depth_only)
                        {
                            crd::gputest::enc_draw_storage_multi_indexed_depth_only_indirect(context.raster(),
                                *context.image(draw.image), *draw.program, 0.0F, g::DepthCompare::Always,
                                *draw.buffers[0], 768U, *draw.buffers[1], 1024U,
                                draw.buffers[2], 1536U, 1U, repeat != 0U);
                        }
                        else
                        {
                            crd::gputest::enc_draw_storage_multi_indexed_indirect(context.raster(),
                                *context.image(draw.image), *draw.program, {}, 0.0F, g::DepthCompare::Always,
                                *draw.buffers[0], 768U, nullptr, nullptr, *draw.buffers[1], 1024U,
                                draw.buffers[2], 1536U, 1U, repeat != 0U);
                        }
                        for (auto* storage : draw.buffers)
                        {
                            if (storage == nullptr) { continue; }
                            g::IStorageBuffer* bindings[1] = {storage};
                            crd::gputest::enc_dispatch(context.raster(), *draw.kernel, 1U, 1U, 1U, bindings, 1U);
                        }
                    }
                }, &state);
            struct Sample { g::FgImage src; g::FgImage dst; g::IRasterProgram* program; };
            Sample sample{image, output, sample_program.get()};
            if (depth_only)
            {
                graph->add_pass("observe-indirect-depth").reads(image).writes(output).execute(
                    [](g::IFrameContext& context, void* user) {
                        const auto& data = *static_cast<Sample*>(user);
                        crd::gputest::enc_draw_shadow(context.raster(), *context.image(data.dst), *data.program, {},
                                                     *context.texture(data.src), 3U);
                    }, &sample);
            }
            REQUIRE(graph->build());
            for (crd::u32 frame = 1; frame != 4U; ++frame)
            {
                CAPTURE(frame);
                graph->execute();
                if (capture.report().errors != 0U) { print_messages(capture); }
                REQUIRE(capture.report().errors == 0U);
                REQUIRE(raster->valid());
                CHECK(graph->last_submit_count() == 1U);
                CHECK((target->read_pixel(16U, 16U) & 0xffU) >= (depth_only ? 180U : 250U));
                for (auto* storage : state.buffers)
                {
                    if (storage == nullptr) { continue; }
                    crd::u32 occurrences = 0;
                    for (auto* other : state.buffers) { if (other == storage) { ++occurrences; } }
                    REQUIRE(raster->download_storage(*storage));
                    CHECK(storage->read_u32(448U) == frame * 2U * occurrences);
                }
            }
        }
    }
    print_messages(capture);
    CHECK(capture.report().complete_and_silent());
}

TEST_CASE("DX12 resource states preserve image state while reactivating aliased targets",
          "[dx12][validation][resource-states]")
{
    namespace k = crd::kir;
    crd::memory::TlsfAllocator allocator(64U << 20U, nullptr, "DX12 alias state test");
    g::Dx12ValidationCapture capture(&allocator, 4096U);
    REQUIRE(capture.report().readiness == g::Dx12ValidationReadiness::Ready);
    {
        auto gpu = g::create_dx12_gpu_context(&allocator);
        auto raster = g::create_dx12_raster_context();
        REQUIRE(gpu != nullptr);
        REQUIRE(raster != nullptr);
        k::KGraph vertex(&allocator);
        k::KGraph red(&allocator);
        k::KGraph green(&allocator);
        k::KEntry ve;
        k::KEntry re;
        k::KEntry ge;
        crd::gputest::build_fullscreen_vs(vertex, ve);
        crd::gputest::build_solid_fs(red, re, 1.0, 0.0, 0.0);
        crd::gputest::build_solid_fs(green, ge, 0.0, 1.0, 0.0);
        auto vs = gpu->create_program(vertex, ve);
        auto rf = gpu->create_program(red, re);
        auto gf = gpu->create_program(green, ge);
        REQUIRE(vs != nullptr);
        REQUIRE(rf != nullptr);
        REQUIRE(gf != nullptr);
        auto rp = raster->create_raster_program(*vs, *rf);
        auto gp = raster->create_raster_program(*vs, *gf);
        REQUIRE(rp != nullptr);
        REQUIRE(gp != nullptr);
        auto first = raster->create_color_target(32U, 32U);
        auto second = raster->create_color_target(32U, 32U);
        auto graph = raster->create_frame_graph();
        REQUIRE(first != nullptr);
        REQUIRE(second != nullptr);
        REQUIRE(graph != nullptr);
        g::FgImageDesc desc{};
        desc.width = 32U; desc.height = 32U;
        bool pinned = false;
        SECTION("disjoint targets share memory") {}
        SECTION("a pinned first target cannot be reused by a later target") { pinned = true; }
        desc.no_alias = pinned;
        const auto a = graph->create_transient_image(desc);
        desc.no_alias = false;
        const auto b = graph->create_transient_image(desc);
        const auto x = graph->import_target(*first);
        const auto y = graph->import_target(*second);
        REQUIRE(a.valid()); REQUIRE(b.valid());
        struct Paint { g::FgImage image; g::IRasterProgram* program; } paints[2] = {{a, rp.get()}, {b, gp.get()}};
        struct Copy { g::FgImage src; g::FgImage dst; } copies[2] = {{a, x}, {b, y}};
        const auto paint = [](g::IFrameContext& context, void* user) {
            const auto& data = *static_cast<Paint*>(user);
            crd::gputest::enc_draw(context.raster(), *context.image(data.image), *data.program, {}, 3U);
        };
        const auto copy = [](g::IFrameContext& context, void* user) {
            const auto& data = *static_cast<Copy*>(user);
            auto encoder = context.raster().create_command_encoder();
            g::TransferDesc transfer{};
            transfer.kind = g::TransferKind::Copy;
            transfer.src = context.image(data.src); transfer.dst = context.image(data.dst);
            encoder->transfer(transfer);
        };
        graph->add_pass("paint-a").writes(a).execute(paint, &paints[0]);
        graph->add_pass("preserve-a", g::FgPassKind::Transfer).reads(a).writes(x).execute(copy, &copies[0]);
        // Reading x makes activation B strictly follow preservation A, so the declared lifetimes are disjoint.
        graph->add_pass("paint-b").reads(x).writes(b).execute(paint, &paints[1]);
        graph->add_pass("preserve-b", g::FgPassKind::Transfer).reads(b).writes(y).execute(copy, &copies[1]);
        REQUIRE(graph->build());
        if (pinned) { CHECK(graph->transient_memory_bytes() == graph->transient_logical_bytes()); }
        else { CHECK(graph->transient_memory_bytes() < graph->transient_logical_bytes()); }
        for (crd::u32 frame = 0; frame != 3U; ++frame)
        {
            CAPTURE(frame);
            graph->execute();
            if (capture.report().errors != 0U) { print_messages(capture); }
            REQUIRE(capture.report().errors == 0U);
            REQUIRE(raster->valid());
            CHECK(graph->last_submit_count() == 1U);
            CHECK((first->read_pixel(16U, 16U) & 0xffffffU) == 0x0000ffU);
            CHECK((second->read_pixel(16U, 16U) & 0xffffffU) == 0x00ff00U);
        }
    }
    print_messages(capture);
    CHECK(capture.report().complete_and_silent());
}

TEST_CASE("DX12 resource states initialize and reuse sampled depth aliases",
          "[dx12][validation][resource-states]")
{
    namespace k = crd::kir;
    crd::memory::TlsfAllocator allocator(64U << 20U, nullptr, "DX12 depth state test");
    g::Dx12ValidationCapture capture(&allocator, 4096U);
    REQUIRE(capture.report().readiness == g::Dx12ValidationReadiness::Ready);
    {
        auto gpu = g::create_dx12_gpu_context(&allocator);
        auto raster = g::create_dx12_raster_context();
        REQUIRE(gpu != nullptr); REQUIRE(raster != nullptr);
        k::KGraph vertex(&allocator);
        k::KGraph depth_a(&allocator);
        k::KGraph depth_b(&allocator);
        k::KGraph sample_v(&allocator);
        k::KGraph sample_f(&allocator);
        k::KEntry ve;
        k::KEntry ae;
        k::KEntry be;
        k::KEntry sve;
        k::KEntry sfe;
        crd::gputest::build_fullscreen_vs(vertex, ve);
        crd::gputest::build_depth_only_const_fs(depth_a, ae, 0.25);
        crd::gputest::build_depth_only_const_fs(depth_b, be, 0.75);
        crd::gputest::build_textured_vs(sample_v, sve);
        crd::gputest::build_shadow_fs(sample_f, sfe);
        auto vs = gpu->create_program(vertex, ve);
        auto af = gpu->create_program(depth_a, ae);
        auto bf = gpu->create_program(depth_b, be);
        auto sv = gpu->create_program(sample_v, sve);
        auto sf = gpu->create_program(sample_f, sfe);
        REQUIRE(vs != nullptr); REQUIRE(af != nullptr); REQUIRE(bf != nullptr);
        REQUIRE(sv != nullptr); REQUIRE(sf != nullptr);
        auto ap = raster->create_raster_program(*vs, *af);
        auto bp = raster->create_raster_program(*vs, *bf);
        auto sample = raster->create_raster_program(*sv, *sf);
        REQUIRE(ap != nullptr); REQUIRE(bp != nullptr); REQUIRE(sample != nullptr);
        auto dummy = raster->create_storage_buffer(16U);
        auto first = raster->create_color_target(32U, 32U);
        auto second = raster->create_color_target(32U, 32U);
        auto graph = raster->create_frame_graph();
        REQUIRE(dummy != nullptr); REQUIRE(first != nullptr); REQUIRE(second != nullptr); REQUIRE(graph != nullptr);
        g::FgImageDesc desc{};
        desc.width = 32U; desc.height = 32U; desc.format = g::FgImageFormat::D32Float; desc.sampled = true;
        const auto a = graph->create_transient_image(desc);
        const auto b = graph->create_transient_image(desc);
        const auto x = graph->import_target(*first);
        const auto y = graph->import_target(*second);
        REQUIRE(a.valid()); REQUIRE(b.valid());
        struct Depth { g::FgImage image; g::IRasterProgram* program; g::IStorageBuffer* storage; };
        Depth depths[2] = {{a, ap.get(), dummy.get()}, {b, bp.get(), dummy.get()}};
        struct Sample { g::FgImage src; g::FgImage dst; g::IRasterProgram* program; };
        Sample samples[2] = {{a, x, sample.get()}, {b, y, sample.get()}};
        const auto depth = [](g::IFrameContext& context, void* user) {
            const auto& data = *static_cast<Depth*>(user);
            crd::gputest::enc_draw_storage_depth_only(context.raster(), *context.image(data.image), *data.program,
                                                     0.0F, g::DepthCompare::Always, *data.storage, 3U);
        };
        const auto shade = [](g::IFrameContext& context, void* user) {
            const auto& data = *static_cast<Sample*>(user);
            crd::gputest::enc_draw_shadow(context.raster(), *context.image(data.dst), *data.program, {},
                                         *context.texture(data.src), 3U);
        };
        graph->add_pass("depth-a").writes(a).execute(depth, &depths[0]);
        graph->add_pass("sample-a").reads(a).writes(x).execute(shade, &samples[0]);
        graph->add_pass("depth-b").reads(x).writes(b).execute(depth, &depths[1]);
        graph->add_pass("sample-b").reads(b).writes(y).execute(shade, &samples[1]);
        REQUIRE(graph->build());
        CHECK(graph->transient_memory_bytes() < graph->transient_logical_bytes());
        for (crd::u32 frame = 0; frame != 3U; ++frame)
        {
            CAPTURE(frame);
            graph->execute();
            if (capture.report().errors != 0U) { print_messages(capture); }
            REQUIRE(capture.report().errors == 0U);
            REQUIRE(raster->valid());
            CHECK(graph->last_submit_count() == 1U);
            CHECK((first->read_pixel(16U, 16U) & 0xffU) < 80U);
            CHECK((second->read_pixel(16U, 16U) & 0xffU) > 180U);
        }
    }
    print_messages(capture);
    CHECK(capture.report().complete_and_silent());
}

TEST_CASE("DX12 resource states activate aliased compute buffers before consuming their contents",
          "[dx12][validation][resource-states]")
{
    namespace k = crd::kir;
    crd::memory::TlsfAllocator allocator(64U << 20U, nullptr, "DX12 buffer alias test");
    g::Dx12ValidationCapture capture(&allocator, 4096U);
    REQUIRE(capture.report().readiness == g::Dx12ValidationReadiness::Ready);
    {
        auto gpu = g::create_dx12_gpu_context(&allocator);
        auto raster = g::create_dx12_raster_context();
        REQUIRE(gpu != nullptr); REQUIRE(raster != nullptr);
        const auto make_kernel = [&](crd::u32 value, bool copy) {
            k::KGraph code(&allocator);
            k::KEntry entry;
            const auto shape = k::make_shape({1});
            const int src = code.buffer_decl(k::DType::U32, 0, 0, true);
            const int zero = code.constant(0.0, shape, k::DType::U32);
            if (copy)
            {
                const int dst = code.buffer_decl(k::DType::U32, 0, 1, true);
                code.stmt_buffer_store(dst, zero, code.buffer_load(src, zero));
            }
            else { code.stmt_buffer_store(src, zero, code.constant(static_cast<double>(value), shape, k::DType::U32)); }
            entry.stage = k::KStage::Compute;
            entry.local_size[0] = 1U;
            entry.kernel_body_begin = 0;
            entry.kernel_body_count = static_cast<int>(code.serial_stmts().size());
            return gpu->create_program(code, entry);
        };
        auto a_kernel = make_kernel(17U, false);
        auto b_kernel = make_kernel(42U, false);
        auto copy_kernel = make_kernel(0U, true);
        REQUIRE(a_kernel != nullptr); REQUIRE(b_kernel != nullptr); REQUIRE(copy_kernel != nullptr);
        auto first = raster->create_storage_buffer(16U);
        auto second = raster->create_storage_buffer(16U);
        auto graph = raster->create_frame_graph();
        REQUIRE(first != nullptr); REQUIRE(second != nullptr); REQUIRE(graph != nullptr);
        const auto a = graph->create_transient_buffer(16U);
        const auto b = graph->create_transient_buffer(16U);
        const auto x = graph->import_storage(*first);
        const auto y = graph->import_storage(*second);
        REQUIRE(a.valid()); REQUIRE(b.valid());
        struct Dispatch { g::IGpuProgram* kernel; g::FgBuffer src; g::FgBuffer dst; };
        Dispatch dispatches[4] = {{a_kernel.get(), a, {}}, {copy_kernel.get(), a, x},
                                  {b_kernel.get(), b, {}}, {copy_kernel.get(), b, y}};
        const auto dispatch = [](g::IFrameContext& context, void* user) {
            const auto& data = *static_cast<Dispatch*>(user);
            g::IStorageBuffer* bindings[2] = {context.buffer(data.src), data.dst.valid() ? context.buffer(data.dst) : nullptr};
            crd::gputest::enc_dispatch(context.raster(), *data.kernel, 1U, 1U, 1U, bindings, data.dst.valid() ? 2U : 1U);
        };
        graph->add_pass("write-a", g::FgPassKind::Compute).writes(a).execute(dispatch, &dispatches[0]);
        graph->add_pass("preserve-a", g::FgPassKind::Compute).reads(a).writes(x).execute(dispatch, &dispatches[1]);
        graph->add_pass("write-b", g::FgPassKind::Compute).reads(x).writes(b).execute(dispatch, &dispatches[2]);
        graph->add_pass("preserve-b", g::FgPassKind::Compute).reads(b).writes(y).execute(dispatch, &dispatches[3]);
        REQUIRE(graph->build());
        CHECK(graph->transient_memory_bytes() < graph->transient_logical_bytes());
        for (crd::u32 frame = 0; frame != 3U; ++frame)
        {
            CAPTURE(frame);
            graph->execute();
            if (capture.report().errors != 0U) { print_messages(capture); }
            REQUIRE(capture.report().errors == 0U);
            REQUIRE(raster->valid());
            CHECK(graph->last_submit_count() == 1U);
            CHECK(graph->last_barrier_count() > 0U);
            REQUIRE(raster->download_storage(*first)); REQUIRE(raster->download_storage(*second));
            CHECK(first->read_u32(0U) == 17U);
            CHECK(second->read_u32(0U) == 42U);
        }
    }
    print_messages(capture);
    CHECK(capture.report().complete_and_silent());
}


TEST_CASE("DX12 descriptors bound contiguous ranges and preserve allocation failure state",
          "[dx12][validation][descriptors]")
{
    crd::memory::TlsfAllocator allocator(64U << 20U, nullptr, "descriptor capture");
    g::Dx12ValidationCapture capture(&allocator, 512U);
    REQUIRE(capture.report().readiness == g::Dx12ValidationReadiness::Ready);
    DescriptorAllocator pages;
    {
        g::detail::Dx12DeviceScope scope;
        ComPtr<ID3D12Device> device;
        REQUIRE(SUCCEEDED(scope.create(device)));
        g::detail::Dx12FrameDescriptors arena(device.Get(), &pages, {4U, 8U, 12U});
        g::detail::Dx12DescriptorRange first;
        g::detail::Dx12DescriptorRange next;
        REQUIRE(SUCCEEDED(arena.reserve(4U, first)));
        CHECK(arena.capacity() == 4U);
        CHECK(arena.used() == 4U);
        CHECK(first.count == 4U);
        pages.reject = true;
        CHECK(arena.reserve(1U, next) == E_OUTOFMEMORY);
        CHECK(next.heap == nullptr);
        CHECK(next.count == 0U);
        CHECK(arena.capacity() == 4U);
        CHECK(arena.used() == 4U);
        pages.reject = false;
        REQUIRE(SUCCEEDED(arena.reserve(1U, next)));
        CHECK(next.heap != first.heap);
        const auto next_gpu = next.gpu.ptr;
        REQUIRE(SUCCEEDED(arena.reserve(7U, next)));
        CHECK(next.gpu.ptr == next_gpu + arena.increment());
        CHECK(arena.capacity() == 12U);
        CHECK(arena.used() == 12U);
        CHECK(arena.pages() == 2U);
        CHECK(arena.reserve(1U, next) == E_OUTOFMEMORY);
        CHECK(arena.reserve(0xffffffffU, next) == E_OUTOFMEMORY);
        CHECK(arena.reserve(0U, next) == E_INVALIDARG);
        CHECK(next.heap == nullptr);
        CHECK(arena.used() == 12U);
        // No command referenced these ranges. A production graph resets only after its submitted fence retires.
        arena.reset_after_retirement();
        REQUIRE(SUCCEEDED(arena.reserve(8U, next)));
        CHECK(next.gpu.ptr == next_gpu);
        REQUIRE(SUCCEEDED(arena.reserve(4U, next)));
        CHECK(next.gpu.ptr == first.gpu.ptr);
        CHECK(arena.pages() == 2U);
        CHECK(pages.live == 2U);
        {
            // Growth left one slot in the first page: exhaust the newer page, then append to that old tail.
            g::detail::Dx12FrameDescriptors fragmented(device.Get(), &pages, {4U, 8U, 12U});
            REQUIRE(SUCCEEDED(fragmented.reserve(3U, first)));
            REQUIRE(SUCCEEDED(fragmented.reserve(8U, next)));
            REQUIRE(SUCCEEDED(fragmented.reserve(1U, next)));
            CHECK(next.heap == first.heap);
            CHECK(next.gpu.ptr == first.gpu.ptr + 3U * fragmented.increment());
            CHECK(fragmented.used() == 12U);
            CHECK(fragmented.capacity() == 12U);
        }
        CHECK(pages.live == 2U);
    }
    CHECK(pages.live == 0U);
    print_messages(capture);
    CHECK(capture.report().complete_and_silent());
}

TEST_CASE("DX12 descriptors preserve queued compute bindings beyond capacity and across frames",
          "[dx12][validation][descriptors]")
{
    namespace k = crd::kir;
    crd::memory::TlsfAllocator allocator(64U << 20U, nullptr, "descriptor compute capture");
    g::Dx12ValidationCapture capture(&allocator, 4096U);
    REQUIRE(capture.report().readiness == g::Dx12ValidationReadiness::Ready);
    DescriptorAllocator pages;
    {
        auto gpu = g::create_dx12_gpu_context(&allocator);
        auto raster = g::create_dx12_raster_context(&pages);
        REQUIRE(gpu != nullptr); REQUIRE(raster != nullptr);
        k::KGraph compute(&allocator);
        k::KEntry entry;
        build_increment_kernel(compute, entry);
        auto kernel = gpu->create_program(compute, entry);
        REQUIRE(kernel != nullptr);
        std::unique_ptr<g::IStorageBuffer> buffers[64];
        for (auto& storage : buffers)
        {
            storage = raster->create_storage_buffer(4U);
            REQUIRE(storage != nullptr);
        }
        auto graph = raster->create_frame_graph();
        REQUIRE(graph != nullptr);
        graph->set_readback_enabled(false);
        auto cleared = raster->create_storage_buffer(4U);
        REQUIRE(cleared != nullptr);
        crd::u32 expected[64]{};
        // Eight descriptors per dispatch: 32 exactly fills the old heap, 33 crosses it, 300 requires more pages.
        for (const crd::u32 count : {32U, 33U, 300U})
        {
            CAPTURE(count);
            struct State { g::IGpuProgram* kernel; std::unique_ptr<g::IStorageBuffer>* buffers; crd::u32 count; g::IStorageBuffer* cleared; };
            State state{kernel.get(), buffers, count, cleared.get()};
            graph->reset();
            auto& pass = graph->add_pass("queued-distinct-bindings", g::FgPassKind::Compute);
            for (auto& storage : buffers) { pass.writes(graph->import_storage(*storage)); }
            pass.writes(graph->import_storage(*cleared));
            pass.execute([](g::IFrameContext& context, void* user) {
                const auto& data = *static_cast<State*>(user);
                for (crd::u32 i = 0; i < data.count; ++i)
                {
                    g::IStorageBuffer* binding[1] = {data.buffers[i % 64U].get()};
                    crd::gputest::enc_dispatch(context.raster(), *data.kernel, 1U, 1U, 1U, binding, 1U);
                }
                context.raster().fill_buffer(*data.cleared, 0U, 4U, data.count);
                g::IStorageBuffer* binding[1] = {data.cleared};
                crd::gputest::enc_dispatch(context.raster(), *data.kernel, 1U, 1U, 1U, binding, 1U);
            }, &state);
            REQUIRE(graph->build());
            for (crd::u32 frame = 0; frame != 3U; ++frame)
            {
                graph->execute();
                REQUIRE(raster->valid());
                CHECK(graph->last_submit_count() == 1U);
            }
            graph->reset(); // Retire the final deferred submit before inspecting or reusing its descriptors.
            REQUIRE(raster->download_storage(*cleared));
            CHECK(cleared->read_u32(0U) == count + 1U);
            for (crd::u32 i = 0; i != 64U; ++i)
            {
                expected[i] += 3U * (count / 64U + (i < count % 64U ? 1U : 0U));
                REQUIRE(raster->download_storage(*buffers[i]));
                CHECK(buffers[i]->read_u32(0U) == expected[i]);
            }
        }
        CHECK(pages.live > 1U);
    }
    CHECK(pages.live == 0U);
    print_messages(capture);
    CHECK(capture.report().complete_and_silent());
}

TEST_CASE("DX12 descriptors keep storage and full bindless tables together through heap growth",
          "[dx12][validation][descriptors]")
{
    namespace k = crd::kir;
    crd::memory::TlsfAllocator allocator(64U << 20U, nullptr, "descriptor graphics capture");
    g::Dx12ValidationCapture capture(&allocator, 4096U);
    REQUIRE(capture.report().readiness == g::Dx12ValidationReadiness::Ready);
    DescriptorAllocator pages;
    {
        auto gpu = g::create_dx12_gpu_context(&allocator);
        auto raster = g::create_dx12_raster_context(&pages);
        REQUIRE(gpu != nullptr); REQUIRE(raster != nullptr);
        k::KGraph vertex(&allocator);
        k::KGraph fragment(&allocator);
        k::KEntry ve;
        k::KEntry fe;
        crd::gputest::build_textured_vs(vertex, ve);
        crd::gputest::build_bindless_fs(fragment, fe);
        const auto shape = k::make_shape({1});
        const int zero = fragment.constant(0.0, shape, k::DType::U32);
        const int gain = fragment.int_bits_to_float(fragment.cast(fragment.storage_load(zero), k::DType::I32));
        fe.out[0].node = fragment.binary(k::KOp::Mul, fe.out[0].node, fragment.vec4(gain, gain, gain, gain));
        auto vs = gpu->create_program(vertex, ve);
        auto fs = gpu->create_program(fragment, fe);
        REQUIRE(vs != nullptr); REQUIRE(fs != nullptr);
        auto program = raster->create_raster_program(*vs, *fs);
        REQUIRE(program != nullptr);
        const crd::u8 red[4] = {255U, 0U, 0U, 255U};
        const crd::u8 green[4] = {0U, 255U, 0U, 255U};
        auto red_texture = raster->create_texture(1U, 1U, red);
        auto green_texture = raster->create_texture(1U, 1U, green);
        REQUIRE(red_texture != nullptr); REQUIRE(green_texture != nullptr);
        g::ITexture* textures[2] = {red_texture.get(), green_texture.get()};
        std::unique_ptr<g::IRasterTarget> targets[4];
        std::unique_ptr<g::IRasterTarget> copies[4];
        std::unique_ptr<g::IStorageBuffer> buffers[4];
        auto graph = raster->create_frame_graph();
        REQUIRE(graph != nullptr);
        struct Draw { g::FgImage image; g::IRasterProgram* program; g::IStorageBuffer* constants; g::ITexture** textures; };
        Draw draws[4]{};
        struct Copy { g::FgImage src; g::FgImage dst; };
        Copy transfers[4]{};
        k::KGraph sampled_fragment(&allocator);
        k::KEntry sampled_entry;
        crd::gputest::build_sample_fs(sampled_fragment, sampled_entry);
        auto sampled_fs = gpu->create_program(sampled_fragment, sampled_entry);
        REQUIRE(sampled_fs != nullptr);
        auto sampled_program = raster->create_raster_program(*vs, *sampled_fs);
        auto sampled_target = raster->create_color_target(32U, 32U);
        REQUIRE(sampled_program != nullptr); REQUIRE(sampled_target != nullptr);
        struct Sample { g::FgImage image; g::IRasterProgram* program; g::ITexture* texture; };
        Sample sampled{graph->import_target(*sampled_target), sampled_program.get(), green_texture.get()};
        // t1 is used here, then a different shader uses u0/t16 on a larger page. Identical native root layouts
        // may be deduplicated by D3D12; no old table may survive a heap change merely because a new shader ignores it.
        graph->add_pass("sample-before-bindless-growth").writes(sampled.image).execute(
            [](g::IFrameContext& context, void* user) {
                const auto& data = *static_cast<Sample*>(user);
                crd::gputest::enc_draw_textured(context.raster(), *context.image(data.image), *data.program,
                                               {}, *data.texture, 3U);
            }, &sampled);
        for (crd::u32 i = 0; i != 4U; ++i)
        {
            targets[i] = raster->create_color_target(32U, 32U);
            buffers[i] = raster->create_storage_buffer(4U);
            REQUIRE(targets[i] != nullptr); REQUIRE(buffers[i] != nullptr);
            const float value = static_cast<float>(i + 1U) * 0.2F; // 51/102/153/204, away from UNORM rounding ties.
            REQUIRE(raster->upload_storage(*buffers[i], 0U, &value, sizeof(value)));
            draws[i] = {graph->import_target(*targets[i]), program.get(), buffers[i].get(), textures};
            graph->add_pass("storage-plus-bindless").writes(draws[i].image).execute(
                [](g::IFrameContext& context, void* user) {
                    const auto& data = *static_cast<Draw*>(user);
                    g::ResourceBinding storage{};
                    storage.kind = g::BindingKind::StorageBuffer;
                    storage.buffer = data.constants;
                    g::ResourceBinding array{};
                    array.kind = g::BindingKind::BindlessTextureArray;
                    array.texture_array = data.textures;
                    array.array_count = 2U;
                    g::ResourceBindingTable bindings{};
                    bindings.push_back(storage); bindings.push_back(array);
                    crd::gputest::enc_fullscreen(context.raster(), *context.image(data.image), *data.program, {}, bindings, 3U);
                }, &draws[i]);
        }
        for (crd::u32 i = 0; i != 4U; ++i)
        {
            copies[i] = raster->create_color_target(32U, 32U);
            REQUIRE(copies[i] != nullptr);
            transfers[i] = {draws[i].image, graph->import_target(*copies[i])};
            graph->add_pass("blit-shaded-output", g::FgPassKind::Transfer).reads(transfers[i].src).writes(transfers[i].dst).execute(
                [](g::IFrameContext& context, void* user) {
                    const auto& data = *static_cast<Copy*>(user);
                    auto encoder = context.raster().create_command_encoder();
                    g::TransferDesc transfer{};
                    transfer.kind = g::TransferKind::Blit;
                    transfer.src = context.image(data.src);
                    transfer.dst = context.image(data.dst);
                    transfer.filter = g::SamplerFilter::Nearest;
                    encoder->transfer(transfer);
                }, &transfers[i]);
        }
        REQUIRE(graph->build());
        for (crd::u32 frame = 0; frame != 3U; ++frame)
        {
            graph->execute();
            REQUIRE(raster->valid());
            CHECK(graph->last_submit_count() == 1U);
            CHECK(sampled_target->read_pixel(16U, 16U) == 0xff00ff00U);
            for (crd::u32 i = 0; i != 4U; ++i)
            {
                const crd::u32 expected = 51U * (i + 1U);
                const crd::u32 left = targets[i]->read_pixel(8U, 16U);
                const crd::u32 right = targets[i]->read_pixel(24U, 16U);
                CHECK((left & 0xffU) == expected);
                CHECK((left & 0xff00U) == 0U);
                CHECK(((right >> 8U) & 0xffU) == expected);
                CHECK((right & 0xffU) == 0U);
                CHECK(copies[i]->read_pixel(8U, 16U) == left);
                CHECK(copies[i]->read_pixel(24U, 16U) == right);
            }
        }
        CHECK(pages.live > 1U);
    }
    CHECK(pages.live == 0U);
    print_messages(capture);
    CHECK(capture.report().complete_and_silent());
}

TEST_CASE("DX12 stage interfaces link authored output locations independently of declaration order",
          "[dx12][validation][raster]")
{
    namespace k = crd::kir;
    crd::memory::TlsfAllocator allocator(64U << 20U, nullptr, "DX12 varying capture");
    g::Dx12ValidationCapture capture(&allocator);
    REQUIRE(capture.report().readiness == g::Dx12ValidationReadiness::Ready);
    {
        auto gpu = g::create_dx12_gpu_context(&allocator);
        auto raster = g::create_dx12_raster_context(&allocator);
        REQUIRE(gpu != nullptr); REQUIRE(raster != nullptr);
        k::KGraph vertex(&allocator);
        k::KGraph fragment(&allocator);
        k::KEntry ve;
        k::KEntry fe;
        crd::gputest::build_fullscreen_vs(vertex, ve);
        const auto shape = k::make_shape({1});
        const int red = vertex.constant(0.2, shape, k::DType::F32);
        const int green = vertex.constant(0.4, shape, k::DType::F32);
        const int blue = vertex.constant(0.6, shape, k::DType::F32);
        ve.n_out = 3;
        ve.out[0] = {vertex.vec4(red, red, red, red), 5, k::Interp::Smooth};
        ve.out[1] = {vertex.vec4(green, green, green, green), 6, k::Interp::Smooth};
        ve.out[2] = {blue, 4, k::Interp::Flat}; // The real velocity cook appends its dither fade this way.
        const int in_green = fragment.stage_in(k::KType::vec(k::DType::F32, 4), 6, k::Interp::Smooth);
        const int in_blue = fragment.stage_in(k::KType::make_scalar(k::DType::F32), 4, k::Interp::Flat);
        const int in_red = fragment.stage_in(k::KType::vec(k::DType::F32, 4), 5, k::Interp::Smooth);
        fe.stage = k::KStage::Fragment;
        fe.n_out = 1;
        fe.out[0] = {fragment.vec4(fragment.swizzle(in_red, 0), fragment.swizzle(in_green, 0), in_blue,
                                   fragment.constant(1.0, shape, k::DType::F32)), 0};
        auto vs = gpu->create_program(vertex, ve);
        auto fs = gpu->create_program(fragment, fe);
        REQUIRE(vs != nullptr); REQUIRE(fs != nullptr);
        auto program = raster->create_raster_program(*vs, *fs);
        auto target = raster->create_color_target(16U, 16U);
        REQUIRE(program != nullptr); REQUIRE(target != nullptr);
        crd::gputest::enc_draw(*raster, *target, *program, {}, 3U);
        REQUIRE(raster->valid());
        CHECK(target->read_pixel(8U, 8U) == 0xff996633U);
    }
    print_messages(capture);
    CHECK(capture.report().complete_and_silent());
}

TEST_CASE("DX12 descriptors cancel an unsubmitted frame on allocator failure",
          "[dx12][validation][descriptors]")
{
    namespace k = crd::kir;
    crd::memory::TlsfAllocator allocator(64U << 20U, nullptr, "descriptor failure capture");
    g::Dx12ValidationCapture capture(&allocator, 4096U);
    REQUIRE(capture.report().readiness == g::Dx12ValidationReadiness::Ready);
    DescriptorAllocator pages;
    {
        auto gpu = g::create_dx12_gpu_context(&allocator);
        auto raster = g::create_dx12_raster_context(&pages);
        REQUIRE(gpu != nullptr); REQUIRE(raster != nullptr);
        k::KGraph compute(&allocator);
        k::KEntry entry;
        build_increment_kernel(compute, entry);
        auto kernel = gpu->create_program(compute, entry);
        REQUIRE(kernel != nullptr);
        auto buffer_storage = raster->create_storage_buffer(4U);
        auto graph = raster->create_frame_graph();
        REQUIRE(buffer_storage != nullptr); REQUIRE(graph != nullptr);
        struct State { g::IGpuProgram* kernel; g::IStorageBuffer* buffer; crd::u32 count; };
        State state{kernel.get(), buffer_storage.get(), 32U};
        graph->add_pass("allocation-failure", g::FgPassKind::Compute).writes(graph->import_storage(*buffer_storage)).execute(
            [](g::IFrameContext& context, void* user) {
                const auto& data = *static_cast<State*>(user);
                g::IStorageBuffer* bindings[1] = {data.buffer};
                for (crd::u32 i = 0; i < data.count; ++i)
                {
                    crd::gputest::enc_dispatch(context.raster(), *data.kernel, 1U, 1U, 1U, bindings, 1U);
                }
            }, &state);
        REQUIRE(graph->build());
        graph->execute();
        REQUIRE(raster->valid());
        REQUIRE(graph->last_submit_count() == 1U);
        REQUIRE(raster->download_storage(*buffer_storage));
        CHECK(buffer_storage->read_u32(0U) == 32U);
        CHECK(pages.live == 1U);
        state.count = 33U;
        pages.reject = true;
        graph->execute();
        CHECK_FALSE(raster->valid());
        CHECK(graph->last_submit_count() == 0U);
        CHECK(buffer_storage->read_u32(0U) == 32U);
        CHECK(capture.report().execution_failures == 1U);
        CHECK(capture.report().errors == 1U);
        CHECK(capture.report().warnings == 0U);
        graph.reset();
        CHECK(pages.live == 0U);
    }
    print_messages(capture);
    CHECK(capture.report().active_contexts == 0U);
    CHECK(capture.report().execution_failures == 1U);
    CHECK(capture.report().instrumentation_failures == 0U);
    CHECK(capture.report().errors == 1U);
    CHECK(capture.report().warnings == 0U);
    crd::u32 expected_failures = 0U;
    for (crd::u32 i = 0; i < capture.report().messages; ++i)
    {
        g::Dx12ValidationMessage message;
        REQUIRE(capture.message(i, message));
        if (message.severity == g::Dx12ValidationSeverity::Error)
        {
            CHECK(message.id == 0U);
            CHECK(std::strcmp(message.text, "frame descriptor reservation failed: HRESULT 0x8007000E") == 0);
            ++expected_failures;
        }
    }
    CHECK(expected_failures == 1U);
    CHECK_FALSE(capture.report().complete_and_silent()); // The expected recording failure must remain visible.
}

// DIAG.7a(b): the DX12-native report/severity project onto the common vocabulary (device-free; pure mapping).
TEST_CASE("Dx12 validation maps onto the common ValidationReport (dropped != clean)", "[dx12][validation][vocab]")
{
    // A Ready capture whose ONLY defect is lost messages: dropped/truncated must make the common report non-clean.
    g::Dx12ValidationReport d{};
    d.readiness                = g::Dx12ValidationReadiness::Ready;
    d.info                     = 3;
    d.warnings                 = 0;
    d.errors                   = 0;
    d.dropped                  = 2;
    d.truncated                = 1;
    d.instrumentation_failures = 0;

    const g::ValidationReport c = g::to_common(d);
    REQUIRE(c.info == 3U);
    REQUIRE(c.error == 0U);
    REQUIRE(c.dropped == 2U);
    REQUIRE(c.truncated == 1U);
    REQUIRE_FALSE(c.clean()); // dropped/truncated -> not clean, the acceptance rule, now shared with Vulkan

    // a Ready capture with nothing lost and no failure maps to a clean common report
    g::Dx12ValidationReport ready{};
    ready.readiness = g::Dx12ValidationReadiness::Ready;
    REQUIRE(g::to_common(ready).clean());

    // the instrument itself failing is NOT a clean run: a capture that never reached Ready must map to non-clean
    // (the registered-default-empty-reads-as-provably-none scar -- zero message counters must not read as clean).
    g::Dx12ValidationReport not_ready{}; // default readiness == AllocationFailed
    REQUIRE_FALSE(g::to_common(not_ready).clean());
    REQUIRE(g::to_common(not_ready).instrumentation_failures == 1U);

    // device-creation / execution failures on an otherwise-Ready, message-silent capture also fold to non-clean
    g::Dx12ValidationReport failed{};
    failed.readiness                = g::Dx12ValidationReadiness::Ready;
    failed.device_creation_failures = 1;
    failed.execution_failures       = 2;
    const g::ValidationReport cf = g::to_common(failed);
    REQUIRE(cf.instrumentation_failures == 3U);
    REQUIRE_FALSE(cf.clean());

    // severity translation
    REQUIRE(g::to_common(g::Dx12ValidationSeverity::Error) == g::ValidationSeverity::Error);
    REQUIRE(g::to_common(g::Dx12ValidationSeverity::Warning) == g::ValidationSeverity::Warning);
    REQUIRE(g::to_common(g::Dx12ValidationSeverity::Info) == g::ValidationSeverity::Info);
}

// DIAG.7a(d1): the capture records the stable Cerid identity parsed from a message whose text embeds an encoded
// object name; a message with no token records a default-invalid identity. Device test on the real GPU here.
TEST_CASE("DX12 validation records the Cerid identity embedded in a message", "[dx12][validation][identity]")
{
    crd::memory::TlsfAllocator allocator(64U << 20U, nullptr, "DX12 identity capture");
    g::Dx12ValidationCapture capture(&allocator, 64U);
    REQUIRE(capture.report().readiness == g::Dx12ValidationReadiness::Ready);
    {
        g::detail::Dx12DeviceScope scope;
        ComPtr<ID3D12Device> device;
        REQUIRE(SUCCEEDED(scope.create(device)));
        ComPtr<ID3D12InfoQueue> queue;
        REQUIRE(SUCCEEDED(device.As(&queue)));

        const auto warnings_before = capture.report().warnings;
        // One message names a resource with an encoded Cerid token embedded in prose; one carries no token.
        REQUIRE(SUCCEEDED(queue->AddApplicationMessage(
            D3D12_MESSAGE_SEVERITY_WARNING, "identity-sentinel [crd:res:0000002a:g00000007] shadowmap 2048x2048")));
        REQUIRE(SUCCEEDED(queue->AddApplicationMessage(
            D3D12_MESSAGE_SEVERITY_WARNING, "plain-sentinel no cerid token here")));
        CHECK(capture.report().warnings - warnings_before == 2U); // exactly our two synthetic warnings reached it

        bool found_id = false;
        bool found_plain = false;
        const auto report = capture.report();
        for (crd::u32 i = 0; i < report.messages; ++i)
        {
            g::Dx12ValidationMessage m;
            REQUIRE(capture.message(i, m));
            if (std::strstr(m.text, "identity-sentinel") != nullptr)
            {
                found_id = true;
                CHECK(m.identity.valid());
                CHECK(m.identity == (g::ObjectIdentity{g::ObjectKind::Resource, 0x2AU, 0x7U}));
            }
            if (std::strstr(m.text, "plain-sentinel") != nullptr)
            {
                found_plain = true;
                CHECK_FALSE(m.identity.valid()); // no token -> the default invalid identity
            }
        }
        CHECK(found_id);
        CHECK(found_plain);
    }
    CHECK(capture.report().instrumentation_failures == 0U);
}

// DIAG.7a(d2b-dx12-a): the attach helper mints a Cerid identity and SetName's it onto a native ID3D12Object; the name
// reads back in-process (GetPrivateData) EXACTLY as format_debug_name built it and parses back to the identity.
TEST_CASE("DX12 identity attaches as a native debug name and round-trips", "[dx12][validation][identity][naming]")
{
    g::detail::Dx12DeviceScope scope;
    ComPtr<ID3D12Device> device;
    REQUIRE(SUCCEEDED(scope.create(device)));

    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width            = 256U;
    rd.Height           = 1U;
    rd.DepthOrArraySize = 1U;
    rd.MipLevels        = 1U;
    rd.SampleDesc.Count = 1U;
    rd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> res;
    REQUIRE(SUCCEEDED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                                      D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&res))));

    const g::ObjectIdentity id = g::detail::dx12_attach_identity(res.Get(), g::ObjectKind::Resource, "dx12-storage");
    REQUIRE(id.valid());
    CHECK(g::identity_registry().alive(id));

    // Read the native debug name back and compare EXACTLY to what format_debug_name would have built.
    wchar_t wname[256]{};
    UINT    wsize = sizeof(wname);
    REQUIRE(SUCCEEDED(res->GetPrivateData(WKPDID_D3DDebugObjectNameW, &wsize, wname)));
    char       got[256]{};
    const UINT wchars = wsize / static_cast<UINT>(sizeof(wchar_t)); // byte size includes the NUL terminator
    for (UINT i = 0; i + 1U < wchars && i + 1U < sizeof(got); ++i) { got[i] = static_cast<char>(wname[i]); }

    char            expected[256]{};
    const crd::usize elen = g::format_debug_name(id, "dx12-storage", expected, sizeof(expected));
    REQUIRE(elen != 0U);
    CHECK(std::string_view{got} == std::string_view{expected});

    g::ObjectIdentity parsed;
    REQUIRE(g::parse(std::string_view{got}, parsed));
    CHECK(parsed == id);

    g::detail::dx12_detach_identity(id);
    CHECK_FALSE(g::identity_registry().alive(id)); // retired -> the freed object's slot generation has bumped
}

// DIAG.7a(d2b-dx12-a): the wiring proof -- create_storage_buffer() mints one identity, its destruction retires it.
// Deltas, because the process-wide registry may hold other live objects.
TEST_CASE("DX12 create_storage_buffer mints and retires a Cerid identity", "[dx12][validation][identity][naming]")
{
    auto raster = g::create_dx12_raster_context();
    REQUIRE(raster != nullptr);
    const crd::usize before = g::identity_registry().live_count(g::ObjectKind::Resource);
    {
        auto sb = raster->create_storage_buffer(256U);
        REQUIRE(sb != nullptr);
        CHECK(g::identity_registry().live_count(g::ObjectKind::Resource) == before + 1U);
    }
    CHECK(g::identity_registry().live_count(g::ObjectKind::Resource) == before); // destroyed -> retired
}

// DIAG.7a(d2b-dx12-b): textures mint + retire a Cerid identity through the same helper as the storage buffer. Wiring
// proof by live_count delta (the raw-helper SetName->GetPrivateData round-trip in the (d2b-dx12-a) case above already
// proves the naming path itself; Dx12Texture exposes no test-reachable native handle, so no round-trip here).
TEST_CASE("DX12 textures mint and retire a Cerid identity", "[dx12][validation][identity][naming]")
{
    auto raster = g::create_dx12_raster_context();
    REQUIRE(raster != nullptr);
    const crd::u8 rgba[4] = {255U, 128U, 0U, 255U};
    const float   depth   = 0.5F;

    const crd::usize base = g::identity_registry().live_count(g::ObjectKind::Resource);
    {
        auto tex = raster->create_texture(1U, 1U, rgba); // -> Dx12Texture factory (site "dx12-texture")
        REQUIRE(tex != nullptr);
        CHECK(g::identity_registry().live_count(g::ObjectKind::Resource) == base + 1U);
        {
            auto dtex = raster->create_depth_texture(1U, 1U, &depth); // -> depth factory (site "dx12-depth-texture")
            REQUIRE(dtex != nullptr);
            CHECK(g::identity_registry().live_count(g::ObjectKind::Resource) == base + 2U);
        }
        CHECK(g::identity_registry().live_count(g::ObjectKind::Resource) == base + 1U); // depth texture retired
    }
    CHECK(g::identity_registry().live_count(g::ObjectKind::Resource) == base); // both retired
}

// DIAG.7a(d2b-dx12-b): raster & G-buffer targets carry ONE identity for the whole logical resource. Each factory owns
// several native objects (colour + resolve + depth + readback + rtv/dsv heaps; a G-buffer's N planes + N readbacks +
// heap), yet live_count(Resource) rises by exactly 1 per target -- the choice point's "one per logical resource, not
// per native object". The G-buffer(3) case is the discriminating one: a per-native-object scheme would show +7.
TEST_CASE("DX12 raster & G-buffer targets mint exactly one identity each", "[dx12][validation][identity][naming]")
{
    auto raster = g::create_dx12_raster_context();
    REQUIRE(raster != nullptr);
    const crd::usize base = g::identity_registry().live_count(g::ObjectKind::Resource);

    {
        auto color = raster->create_color_target(32U, 32U); // tex + rtv_heap + readback -> +1
        REQUIRE(color != nullptr);
        CHECK(g::identity_registry().live_count(g::ObjectKind::Resource) == base + 1U);
    }
    CHECK(g::identity_registry().live_count(g::ObjectKind::Resource) == base);

    {
        auto cd = raster->create_color_depth_target(32U, 32U); // tex + depth + readback + rtv_heap + dsv_heap -> still +1
        REQUIRE(cd != nullptr);
        CHECK(g::identity_registry().live_count(g::ObjectKind::Resource) == base + 1U);
    }
    CHECK(g::identity_registry().live_count(g::ObjectKind::Resource) == base);

    {
        auto gb = raster->create_gbuffer_target(32U, 32U, 3U); // 3 planes + 3 readbacks + rtv_heap -> +1, NOT +7
        REQUIRE(gb != nullptr);
        CHECK(g::identity_registry().live_count(g::ObjectKind::Resource) == base + 1U);
    }
    CHECK(g::identity_registry().live_count(g::ObjectKind::Resource) == base);
}

// DIAG.7a(d2b-dx12-b): the name-only mechanism itself. dx12_name_object() stamps an EXISTING identity onto a second
// native object without minting -- both objects' debug names read back to the SAME identity, and live_count rises by
// exactly 1 (the mint), not 2. This is how every sibling of a logical target resolves to its one identity.
TEST_CASE("DX12 dx12_name_object stamps an identity without minting", "[dx12][validation][identity][naming]")
{
    g::detail::Dx12DeviceScope scope;
    ComPtr<ID3D12Device> device;
    REQUIRE(SUCCEEDED(scope.create(device)));

    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width            = 256U;
    rd.Height           = 1U;
    rd.DepthOrArraySize = 1U;
    rd.MipLevels        = 1U;
    rd.SampleDesc.Count = 1U;
    rd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> primary;
    ComPtr<ID3D12Resource> sibling;
    REQUIRE(SUCCEEDED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_GENERIC_READ,
                                                      nullptr, IID_PPV_ARGS(&primary))));
    REQUIRE(SUCCEEDED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_GENERIC_READ,
                                                      nullptr, IID_PPV_ARGS(&sibling))));

    const crd::usize before = g::identity_registry().live_count(g::ObjectKind::Resource);
    const g::ObjectIdentity id = g::detail::dx12_attach_identity(primary.Get(), g::ObjectKind::Resource, "dx12-target-color");
    REQUIRE(id.valid());
    g::detail::dx12_name_object(sibling.Get(), id, "dx12-target-depth"); // SAME id, no new mint
    CHECK(g::identity_registry().live_count(g::ObjectKind::Resource) == before + 1U); // one mint only, not two

    // The sibling's native debug name is EXACTLY what format_debug_name builds for this id + its role site.
    wchar_t wname[256]{};
    UINT    wsize = sizeof(wname);
    REQUIRE(SUCCEEDED(sibling->GetPrivateData(WKPDID_D3DDebugObjectNameW, &wsize, wname)));
    char       got[256]{};
    const UINT wchars = wsize / static_cast<UINT>(sizeof(wchar_t));
    for (UINT i = 0; i + 1U < wchars && i + 1U < sizeof(got); ++i) { got[i] = static_cast<char>(wname[i]); }
    char             expected[256]{};
    const crd::usize elen = g::format_debug_name(id, "dx12-target-depth", expected, sizeof(expected));
    REQUIRE(elen != 0U);
    CHECK(std::string_view{got} == std::string_view{expected});

    // A message about EITHER object resolves to the one logical identity.
    wchar_t pname[256]{};
    UINT    psize = sizeof(pname);
    REQUIRE(SUCCEEDED(primary->GetPrivateData(WKPDID_D3DDebugObjectNameW, &psize, pname)));
    char       pgot[256]{};
    const UINT pchars = psize / static_cast<UINT>(sizeof(wchar_t));
    for (UINT i = 0; i + 1U < pchars && i + 1U < sizeof(pgot); ++i) { pgot[i] = static_cast<char>(pname[i]); }
    g::ObjectIdentity from_primary;
    g::ObjectIdentity from_sibling;
    REQUIRE(g::parse(std::string_view{pgot}, from_primary));
    REQUIRE(g::parse(std::string_view{got}, from_sibling));
    CHECK(from_primary == id);
    CHECK(from_sibling == id);

    g::detail::dx12_detach_identity(id);
    CHECK(g::identity_registry().live_count(g::ObjectKind::Resource) == before); // one retire drops the one identity
}

// DIAG.7a(d2b-dx12-b) batch 3c: the raster context's authored-kernel compute-PSO cache (kernel_pipeline) mints ONE
// ObjectKind::Program identity per UNIQUE cached kernel PSO -- on the cache-MISS build, never on a hit -- and retires
// them all in ~Dx12RasterContext. The cache keys by the kernel's DXIL POINTER, so the same IGpuProgram dispatched twice
// is +1 (a hit), while a second program built from identical source is a distinct pointer -> +2 (a new entry). The
// sibling caches m_rt_pso (rt_kernel_pipeline) and m_sampled_pso (sampled_kernel_pipeline) are byte-identical in shape
// and wiring (see the session doc); this proves the mechanism on kernel_pipeline, with teeth on its mint.
TEST_CASE("DX12 cached kernel PSOs mint one Program identity each, retired with the context",
          "[dx12][kernel][program][identity][naming]")
{
    namespace k = crd::kir;
    crd::memory::TlsfAllocator allocator(16U << 20U);
    auto gpu = g::create_dx12_gpu_context(&allocator);
    if (gpu == nullptr || !gpu->valid()) { WARN("no D3D12 device available; skipping"); return; }

    // A trivial compute kernel (buffer[448] += 1), mirroring this file's existing kernel authoring. Result is irrelevant
    // -- the test only asserts identity counts, so a valid PSO build (which triggers the mint) is all that matters.
    const auto make_kernel = [&](k::KGraph& cg, k::KEntry& ce) {
        const auto shape = k::make_shape({1});
        const int  buf   = cg.buffer_decl(k::DType::U32, 0, 0, true);
        const int  word  = cg.constant(448.0, shape, k::DType::U32);
        const int  one   = cg.constant(1.0, shape, k::DType::U32);
        const int  prev  = cg.buffer_load(buf, word);
        cg.stmt_buffer_store(buf, word, cg.binary(k::KOp::Add, prev, one));
        ce.stage            = k::KStage::Compute;
        ce.local_size[0]    = 1U;
        ce.kernel_body_begin = 0;
        ce.kernel_body_count = static_cast<int>(cg.serial_stmts().size());
    };

    const crd::usize prog_before = g::identity_registry().live_count(g::ObjectKind::Program);
    {
        auto raster = g::create_dx12_raster_context();
        REQUIRE(raster != nullptr);
        if (!raster->valid()) { WARN("no D3D12 raster device; skipping"); return; }

        k::KGraph cg1(&allocator);
        k::KEntry ce1;
        make_kernel(cg1, ce1);
        auto kernel = gpu->create_program(cg1, ce1);
        REQUIRE(kernel != nullptr);

        auto sbuf   = raster->create_storage_buffer(64U);
        auto target = raster->create_color_target(16U, 16U);
        REQUIRE(sbuf != nullptr);
        REQUIRE(target != nullptr);

        struct S { g::IGpuProgram* kernel; g::IStorageBuffer* buf; } st{kernel.get(), sbuf.get()};
        const auto dispatch_once = [&] {
            auto graph = raster->create_frame_graph();
            REQUIRE(graph != nullptr);
            const auto out = graph->import_target(*target);
            graph->add_pass("kernel-id").writes(out).execute(
                [](g::IFrameContext& context, void* user) {
                    auto&              s   = *static_cast<S*>(user);
                    g::IStorageBuffer* b[1] = {s.buf};
                    crd::gputest::enc_dispatch(context.raster(), *s.kernel, 1U, 1U, 1U, b, 1U);
                }, &st);
            REQUIRE(graph->build());
            graph->execute();
        };

        const crd::usize prog0 = g::identity_registry().live_count(g::ObjectKind::Program);
        const crd::usize res0  = g::identity_registry().live_count(g::ObjectKind::Resource);
        dispatch_once(); // cache MISS -> mint exactly one Program identity on the kernel PSO
        CHECK(g::identity_registry().live_count(g::ObjectKind::Program) == prog0 + 1U);
        CHECK(g::identity_registry().live_count(g::ObjectKind::Resource) == res0); // a PSO is Program-kind, not Resource
        dispatch_once(); // same kernel DXIL pointer -> cache HIT, no mint
        CHECK(g::identity_registry().live_count(g::ObjectKind::Program) == prog0 + 1U);

        // A SECOND program from identical source is a distinct IGpuProgram (distinct DXIL pointer) -> a new cache entry.
        k::KGraph cg2(&allocator);
        k::KEntry ce2;
        make_kernel(cg2, ce2);
        auto kernel2 = gpu->create_program(cg2, ce2);
        REQUIRE(kernel2 != nullptr);
        st.kernel = kernel2.get();
        dispatch_once();
        CHECK(g::identity_registry().live_count(g::ObjectKind::Program) == prog0 + 2U);
    }
    // The raster context (owner of the caches) is destroyed here -> every cached kernel PSO identity is retired.
    CHECK(g::identity_registry().live_count(g::ObjectKind::Program) == prog_before);
}

// DIAG.7a(d2b-dx12-b) batch 3d: the raster context's DXR state-object cache (m_dxr) mints ONE ObjectKind::Program
// identity per UNIQUE ray-tracing pipeline -- on the cache-MISS build, before the DxrPipe is copied into the array;
// primary the ID3D12StateObject, with the SBT resource NAMED (not minted, so Resource stays flat) -- and retires them
// in ~Dx12RasterContext. The cache keys by the CONTENT HASH of each stage's DXIL (DxrPipe::key SCAR), so a pipeline
// built from the SAME stages is a real content HIT -> still +1. DXR-hardware-gated: WARN-skips honestly without it.
TEST_CASE("DX12 cached DXR pipelines mint one Program identity each, retired with the context",
          "[dx12][rt][program][identity][naming]")
{
    namespace k = crd::kir;
    crd::memory::TlsfAllocator allocator(64U << 20U);
    auto gpu = g::create_dx12_gpu_context(&allocator);
    if (gpu == nullptr || !gpu->valid()) { WARN("no D3D12 device available; skipping"); return; }

    // The three required RT stages, authored in CKIR and cooked to DXIL libraries (the test_dx12_rt.cpp path).
    k::KGraph grg(&allocator), gch(&allocator), gms(&allocator);
    const k::KEntry erg = k::rt::build_rt_pipeline_raygen(grg, /*use_ser=*/false);
    const k::KEntry ech = k::rt::build_rt_pipeline_closesthit(gch);
    const k::KEntry ems = k::rt::build_rt_pipeline_miss(gms);
    k::GlslKernel krg(&allocator), kch(&allocator), kms(&allocator);
    REQUIRE(k::emit_rt_stage_hlsl(grg, erg, &allocator, krg, false));
    REQUIRE(k::emit_rt_stage_hlsl(gch, ech, &allocator, kch, false));
    REQUIRE(k::emit_rt_stage_hlsl(gms, ems, &allocator, kms, false));
    const auto drg = g::compile_hlsl_to_dxil(g::ShaderStage::RayGen, crd::containers::to_view(krg.source), "rg", &allocator);
    if (!drg.ok) { WARN("dxc unavailable / lib_6_3 unsupported; skipping"); return; }
    const auto dch = g::compile_hlsl_to_dxil(g::ShaderStage::ClosestHit, crd::containers::to_view(kch.source), "ch", &allocator);
    const auto dms = g::compile_hlsl_to_dxil(g::ShaderStage::Miss, crd::containers::to_view(kms.source), "ms", &allocator);
    REQUIRE(dch.ok);
    REQUIRE(dms.ok);
    auto rg = gpu->create_program(g::ShaderStage::RayGen, crd::containers::ConstSpan<crd::u8>(drg.dxil.data(), drg.dxil.size()));
    auto ch = gpu->create_program(g::ShaderStage::ClosestHit, crd::containers::ConstSpan<crd::u8>(dch.dxil.data(), dch.dxil.size()));
    auto ms = gpu->create_program(g::ShaderStage::Miss, crd::containers::ConstSpan<crd::u8>(dms.dxil.data(), dms.dxil.size()));
    REQUIRE(rg != nullptr);
    REQUIRE(ch != nullptr);
    REQUIRE(ms != nullptr);

    const crd::usize prog_before = g::identity_registry().live_count(g::ObjectKind::Program);
    {
        auto raster = g::create_dx12_raster_context();
        REQUIRE(raster != nullptr);
        if (!raster->valid() || !raster->supports_rt_pipeline()) { WARN("no DXR-capable raster device; skipping"); return; }

        g::Dx12RayTracingContext rt;
        if (!rt.valid()) { WARN("no D3D12 DXR-1.1 device; skipping"); return; }
        const float verts[9] = {0.0F, 0.0F, 2.0F, 1.0F, 0.0F, 2.0F, 0.0F, 1.0F, 2.0F}; // one triangle
        auto scene = rt.build_scene(verts, 1U);
        REQUIRE(scene != nullptr);

        auto sbuf = raster->create_storage_buffer(256U);
        auto target = raster->create_color_target(16U, 16U);
        REQUIRE(sbuf != nullptr);
        REQUIRE(target != nullptr);

        struct S { g::IGpuProgram* rg; g::IGpuProgram* ms; g::IGpuProgram* ch; g::IAccelerationStructure* as; g::IStorageBuffer* buf; }
            st{rg.get(), ms.get(), ch.get(), scene.get(), sbuf.get()};
        const auto trace_once = [&] {
            auto graph = raster->create_frame_graph();
            REQUIRE(graph != nullptr);
            const auto out = graph->import_target(*target);
            graph->add_pass("dxr-id").writes(out).execute(
                [](g::IFrameContext& context, void* user) {
                    auto&        s   = *static_cast<S*>(user);
                    auto         enc = context.raster().create_command_encoder();
                    g::TraceDesc td{};
                    td.raygen = s.rg; td.miss = s.ms; td.closest_hit = s.ch;
                    td.acceleration_structure = s.as;
                    td.width = 8U; td.height = 8U;
                    g::ResourceBinding b{};
                    b.kind   = g::BindingKind::StorageBuffer;
                    b.buffer = s.buf;
                    td.bindings.push_back(b);
                    enc->trace_rays(td);
                }, &st);
            REQUIRE(graph->build());
            graph->execute();
        };

        const crd::usize prog0 = g::identity_registry().live_count(g::ObjectKind::Program);
        const crd::usize res0  = g::identity_registry().live_count(g::ObjectKind::Resource);
        trace_once(); // cache MISS -> mint exactly one Program identity on the DXR state object
        CHECK(g::identity_registry().live_count(g::ObjectKind::Program) == prog0 + 1U);
        CHECK(g::identity_registry().live_count(g::ObjectKind::Resource) == res0); // the SBT is named, not minted
        trace_once(); // SAME stage DXIL content -> content-hash HIT, no mint
        CHECK(g::identity_registry().live_count(g::ObjectKind::Program) == prog0 + 1U);
    }
    // The raster context (owner of m_dxr) is destroyed here -> the DXR pipeline identity is retired.
    CHECK(g::identity_registry().live_count(g::ObjectKind::Program) == prog_before);
}

// DIAG.7a(d2b-dx12) batch 4b: an acceleration structure (Dx12RtScene) is ONE logical resource. build_scene mints
// exactly one ObjectKind::Resource identity -- on the TLAS (the bound handle); the BLAS is NAMED with the SAME id, not
// separately minted -- and ~SceneImpl retires it. So the count rises by ONE per scene despite two native buffers, the
// choice point's "one identity per logical resource" rule (as for Dx12RasterTarget). Distinct scenes are distinct
// objects -> +2 (no dedup). A scene mints no Program identity. DXR-gated: WARN-skips honestly without a device.
TEST_CASE("DX12 acceleration structure mints one Resource identity per scene",
          "[dx12][rt][resource][identity][naming]")
{
    g::Dx12RayTracingContext rt;
    if (!rt.valid()) { WARN("no D3D12 DXR-1.1 device available; skipping"); return; }

    const float      verts[9]    = {0.0F, 0.0F, 2.0F, 1.0F, 0.0F, 2.0F, 0.0F, 1.0F, 2.0F}; // one triangle
    const crd::usize res_before  = g::identity_registry().live_count(g::ObjectKind::Resource);
    const crd::usize prog_before = g::identity_registry().live_count(g::ObjectKind::Program);
    {
        auto a = rt.build_scene(verts, 1U);
        REQUIRE(a != nullptr);
        // +1 despite TWO native buffers (BLAS + TLAS): the BLAS is named onto the scene identity, not minted.
        CHECK(g::identity_registry().live_count(g::ObjectKind::Resource) == res_before + 1U);
        CHECK(g::identity_registry().live_count(g::ObjectKind::Program) == prog_before); // a scene is not a program
        {
            auto b = rt.build_scene(verts, 1U);
            REQUIRE(b != nullptr);
            CHECK(g::identity_registry().live_count(g::ObjectKind::Resource) == res_before + 2U); // distinct scene, no dedup
        }
        CHECK(g::identity_registry().live_count(g::ObjectKind::Resource) == res_before + 1U); // b retired
    }
    CHECK(g::identity_registry().live_count(g::ObjectKind::Resource) == res_before); // a retired -> baseline
}

// DIAG.7a(f-3): per-mode validation ACTIVATION on D3D12, the mirror of the Vulkan device test. Core = the debug layer is on
// for THIS device (ID3D12InfoQueue QI is the post-hoc oracle); GpuAssisted = SetEnableGPUBasedValidation ran pre-device on
// ID3D12Debug1; Synchronization has NO D3D12 equivalent (BackendHasNoEquivalent -- a correct report, not a failure).
TEST_CASE("DIAG.7a(f-3): DX12 reports per-mode validation activation", "[dx12][validation][gpu]")
{
    crd::memory::TlsfAllocator allocator(16U << 20U, nullptr, "DX12 f-3 validation activation test");

    g::GpuContextConfig cfg;
    cfg.backend                        = g::GpuBackend::Dx12;
    cfg.enable_validation              = true; // Core
    cfg.enable_sync_validation         = true; // Synchronization (no D3D12 equivalent)
    cfg.enable_gpu_assisted_validation = true; // GpuAssisted (GBV)
    auto ctx = g::create_dx12_gpu_context(&allocator, cfg);
    if (ctx == nullptr || !ctx->valid()) { SKIP("no D3D12 device / Graphics Tools available"); }

    const g::ValidationActivation va = ctx->validation_activation();
    REQUIRE(va.consistent());

    CHECK(va.is_requested(g::ValidationMode::Core));
    CHECK(va.is_requested(g::ValidationMode::Synchronization));
    CHECK(va.is_requested(g::ValidationMode::GpuAssisted));

    // This box has a real D3D12 device; requesting Core turns the debug layer on -> InfoQueue QI succeeds -> Core active.
    CHECK(va.is_active(g::ValidationMode::Core));
    CHECK(va.unsupported_reason(g::ValidationMode::Core) == g::ValidationUnsupportedReason::None);

    // GPU-assisted (GBV): Graphics Tools present here -> ID3D12Debug1 QI ok -> enabled pre-device -> active.
    CHECK(va.is_active(g::ValidationMode::GpuAssisted));

    // Synchronization: D3D12 has no synchronization-validation mode -> reported unsupported, never active.
    CHECK_FALSE(va.is_active(g::ValidationMode::Synchronization));
    CHECK(va.unsupported_reason(g::ValidationMode::Synchronization)
          == g::ValidationUnsupportedReason::BackendHasNoEquivalent);

    // A context that requests NOTHING reports every mode NotRequested / inactive. (The process-global GBV reset itself is
    // not observable from this report; the no-leak proof is [identity][naming] holding its baseline after this test runs.)
    g::GpuContextConfig plain;
    plain.backend = g::GpuBackend::Dx12;
    auto ctx2 = g::create_dx12_gpu_context(&allocator, plain);
    if (ctx2 != nullptr && ctx2->valid())
    {
        const g::ValidationActivation va2 = ctx2->validation_activation();
        REQUIRE(va2.consistent());
        for (crd::usize i = 0; i < g::kValidationModeCount; ++i)
        {
            CHECK_FALSE(va2.requested[i]);
            CHECK_FALSE(va2.active[i]);
            CHECK(va2.reason[i] == g::ValidationUnsupportedReason::NotRequested);
        }
    }
}

// DIAG.7a(g-5): the DX12 CORE resource route -- the 2nd-backend claimed route. DX12 has no object list; the debug layer
// interpolates the resource's SetName into the message text, and the capture pre-parses the Cerid token into
// Dx12ValidationMessage::identity (d1, both backends). Specimen: a Cerid-named buffer takes an invalid ResourceBarrier
// (two transitions of the SAME subresource in one call -> RESOURCE_BARRIER_DUPLICATE_SUBRESOURCE_TRANSITIONS, a record-time
// error naming the resource). Three legs: (a) named -> a record correlates to the id; (b) unnamed -> the error still fires
// but nothing parses (the name is load-bearing); (c) a valid barrier on the named buffer -> nothing correlates to the id.
// The debug layer is process-global (no in-process off-side), so "valid stays clean" is the discriminator (as f-3/g-1).
namespace
{
// Scan capture records [from, now) for one whose parsed identity == id. from = report().messages before the leg (the
// capture is process-global, so creation chatter precedes our hazard).
// The acceptance route is a correlated ERROR: a record whose parsed identity == id AND whose severity is Error (the
// debug layer also emits non-error records that name the resource -- those correlate but are not the hazard).
[[nodiscard]] bool dx12_new_error_correlates(const g::Dx12ValidationCapture& capture, crd::u32 from,
                                             const g::ObjectIdentity& id)
{
    const crd::u32 now = capture.report().messages;
    for (crd::u32 i = from; i < now; ++i)
    {
        g::Dx12ValidationMessage m;
        if (capture.message(i, m) && m.identity == id && m.severity == g::Dx12ValidationSeverity::Error) { return true; }
    }
    return false;
}

[[nodiscard]] bool dx12_new_record_has_any_identity(const g::Dx12ValidationCapture& capture, crd::u32 from)
{
    const crd::u32 now = capture.report().messages;
    for (crd::u32 i = from; i < now; ++i)
    {
        g::Dx12ValidationMessage m;
        if (capture.message(i, m) && m.identity.valid()) { return true; }
    }
    return false;
}

[[nodiscard]] Microsoft::WRL::ComPtr<ID3D12Resource> dx12_make_buffer(ID3D12Device* device)
{
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width            = 256U;
    desc.Height           = 1U;
    desc.DepthOrArraySize = 1U;
    desc.MipLevels        = 1U;
    desc.SampleDesc.Count = 1U;
    desc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    Microsoft::WRL::ComPtr<ID3D12Resource> buffer;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                               IID_PPV_ARGS(&buffer))))
    {
        return nullptr;
    }
    return buffer;
}

// Record two transitions of subresource 0 of the same buffer in ONE ResourceBarrier call -> a record-time duplicate error.
void dx12_record_duplicate_barrier(ID3D12GraphicsCommandList* list, ID3D12Resource* buffer)
{
    D3D12_RESOURCE_BARRIER two[2]{};
    for (auto& b : two)
    {
        b.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource   = buffer;
        b.Transition.Subresource = 0U;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        b.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_DEST;
    }
    list->ResourceBarrier(2U, two); // duplicate subresource 0 in one call
}
} // namespace

TEST_CASE("DIAG.7a(g-5): a DX12 Core hazard on a named resource correlates to its identity; unnamed and valid stay clean",
          "[dx12][validation][identity][hazard]")
{
    crd::memory::TlsfAllocator allocator(64U << 20U, nullptr, "DX12 g-5 correlated hazard test");
    g::Dx12ValidationCapture capture(&allocator, 4096U);
    REQUIRE(capture.report().readiness == g::Dx12ValidationReadiness::Ready);

    g::detail::Dx12DeviceScope lifetime;
    ComPtr<ID3D12Device> device;
    REQUIRE(SUCCEEDED(lifetime.create(device)));
    ComPtr<ID3D12CommandAllocator> commands;
    REQUIRE(SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&commands))));

    const auto make_list = [&]() {
        ComPtr<ID3D12GraphicsCommandList> list;
        REQUIRE(SUCCEEDED(device->CreateCommandList(0U, D3D12_COMMAND_LIST_TYPE_DIRECT, commands.Get(), nullptr,
                                                    IID_PPV_ARGS(&list))));
        return list;
    };

    // (a) HAZARD on a NAMED Cerid buffer -> a record correlates to its identity.
    {
        auto buffer = dx12_make_buffer(device.Get());
        REQUIRE(buffer != nullptr);
        const g::ObjectIdentity id =
            g::detail::dx12_attach_identity(buffer.Get(), g::ObjectKind::Resource, "dx12-hazard-buffer");
        REQUIRE(id.valid());
        REQUIRE(g::identity_registry().alive(id));

        const crd::u32 before = capture.report().messages;
        auto           list   = make_list();
        dx12_record_duplicate_barrier(list.Get(), buffer.Get());
        (void)list->Close();

        for (crd::u32 i = before; i < capture.report().messages; ++i)
        {
            g::Dx12ValidationMessage m;
            if (capture.message(i, m))
            {
                UNSCOPED_INFO("dx12 msg id=" << m.id << " ident.valid=" << m.identity.valid()
                              << " kind=" << (m.identity.valid() ? static_cast<int>(m.identity.kind) : -1)
                              << " text=" << m.text);
            }
        }
        CHECK(capture.report().dropped == 0U);
        REQUIRE(dx12_new_error_correlates(capture, before, id)); // a correlated ERROR names THIS resource
        CHECK(g::identity_registry().alive(id));
        g::detail::dx12_detach_identity(id);
    }

    // (b) TEETH: the same hazard on an UNNAMED buffer -> the error fires, but nothing parses (name load-bearing).
    {
        auto buffer = dx12_make_buffer(device.Get());
        REQUIRE(buffer != nullptr); // deliberately NOT named
        const crd::u32 before = capture.report().messages;
        const crd::u64 err0   = capture.report().errors;
        auto           list   = make_list();
        dx12_record_duplicate_barrier(list.Get(), buffer.Get());
        (void)list->Close();
        CHECK(capture.report().errors > err0);                          // the hazard still fired
        CHECK_FALSE(dx12_new_record_has_any_identity(capture, before)); // but no crd token -> no correlation
    }

    // (c) VALID consumer on a NAMED buffer -> a single legal transition -> nothing correlates to the id.
    {
        auto buffer = dx12_make_buffer(device.Get());
        REQUIRE(buffer != nullptr);
        const g::ObjectIdentity id =
            g::detail::dx12_attach_identity(buffer.Get(), g::ObjectKind::Resource, "dx12-valid-buffer");
        REQUIRE(id.valid());
        const crd::u32 before = capture.report().messages;
        auto           list   = make_list();
        D3D12_RESOURCE_BARRIER ok{};
        ok.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        ok.Transition.pResource   = buffer.Get();
        ok.Transition.Subresource = 0U;
        ok.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        ok.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_DEST;
        list->ResourceBarrier(1U, &ok);
        (void)list->Close();
        CHECK_FALSE(dx12_new_error_correlates(capture, before, id)); // valid consumer produces no correlated error
        g::detail::dx12_detach_identity(id);
    }
}
