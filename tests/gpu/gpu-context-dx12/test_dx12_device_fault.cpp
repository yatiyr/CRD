// DIAG.7b(h): a real device fault, captured by DRED and written as a removal bundle; DIAG.7a: the pass it happened in.
//
// The workload stores through a root UAV whose buffer was released after recording and before submission: a GPU
// use-after-free. Root descriptors carry no bounds and neither the runtime nor the core debug layer checks the address,
// so the store reaches execution. On hardware that is a page fault. It runs in a bounded child process (this executable
// re-entered on a hidden case): a faulting device can take its process down, and the child leaves with _Exit instead
// of tearing a removed device down.
//
// The list holds two passes, each bracketed by the production Dx12PassEventScope marker; the first completes and the
// second faults. DRED records the markers as breadcrumb contexts, and the removal report names the pass the GPU was
// inside when it stopped (Dx12DredBreadcrumbNode::in_flight_pass) -- the DIAG.7a pass-to-fault correlation.
//
// (h1) WARP, measured 2026-10-07: the software adapter produces no real fault. It discards stores to an unmapped
// address (a released buffer, 1 GiB past a live one, a wild low address) and completes the workload with the device
// live; a store loop that never ends runs past a 55 s bound with no watchdog, and the engine's own removal after the
// bounded wait leaves DRED's breadcrumb list empty, as a forced removal does on hardware (DIAG.7b(e)). The WARP leg
// below stays as the CI-capable run of the whole path: it accepts exactly "completed" or "recorded", and if a WARP
// update ever faults it checks the full bundle instead.
//
// (h2) The same child on the hardware adapter is the real-fault evidence. It runs only when the user starts it, on an
// idle machine, behind an explicit opt-in; it is never part of the default suite or CI:
//   set CRD_DX12_HARDWARE_FAULT_OPT_IN=1
//   build\win-debug\tests\gpu-context-dx12\crd-gpu-context-dx12-tests.exe "[.dx12-hardware-device-fault]" -s

#include "dx12_device_scope.hpp"
#include "dx12_dred.hpp"
#include "dx12_execution.hpp"
#include "dx12_identity_naming.hpp"

#include <crd/core/crash.hpp>
#include <crd/gpu/dx12_context.hpp>
#include <crd/gpu/identity_registry.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <dxgi1_4.h>
#include <filesystem>
#include <string_view>

namespace
{
namespace g = crd::gpu;
using Microsoft::WRL::ComPtr;

// Child exit codes. Only kFaultRecorded means a real removal reached the bundle.
constexpr DWORD kFaultRecorded = 0U;
constexpr DWORD kFaultNoDred = 20U;        // DRED settings absent or contexts not recorded: a capability result
constexpr DWORD kFaultNoCompiler = 21U;    // dxc/DXIL unavailable
constexpr DWORD kFaultNoAdapter = 22U;     // the requested adapter could not be opened
constexpr DWORD kFaultNotHardware = 23U;   // (h2) the default adapter is not positively classified as hardware
constexpr DWORD kFaultCompleted = 30U;     // the workload completed with the device live: no fault happened
constexpr DWORD kFaultEngineForced = 31U;  // only the engine's own removal (a timeout), no device fault
constexpr DWORD kFaultBundleMissing = 32U; // the removal was recorded but no bundle was written
constexpr DWORD kFaultSpawnFailed = 97U;
constexpr DWORD kFaultChildTimedOut = 98U;

constexpr const char* kFaultHlsl =
    "RWByteAddressBuffer target : register(u0);\n"
    "[numthreads(64, 1, 1)] void cs_main(uint3 id : SV_DispatchThreadID) { target.Store(id.x * 4u, id.x + 1u); }\n";

constexpr UINT64 kBufferBytes = 65536U;
constexpr UINT kFaultGroups = 64U; // 64 x 64 stores of 4 bytes: 16 KiB through the released buffer's address

[[nodiscard]] std::string_view env_or(const char* name, std::string_view fallback, char (&storage)[256])
{
    const DWORD n = GetEnvironmentVariableA(name, storage, static_cast<DWORD>(sizeof(storage)));
    if (n == 0U || n >= sizeof(storage))
    {
        return fallback;
    }
    return {storage, n};
}

[[nodiscard]] ComPtr<ID3D12Resource> make_uav_buffer(ID3D12Device* device)
{
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC buf{};
    buf.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buf.Width = kBufferBytes;
    buf.Height = 1U;
    buf.DepthOrArraySize = 1U;
    buf.MipLevels = 1U;
    buf.SampleDesc.Count = 1U;
    buf.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    buf.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ComPtr<ID3D12Resource> resource;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buf, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                               nullptr, IID_PPV_ARGS(&resource))))
    {
        return nullptr;
    }
    return resource;
}

[[nodiscard]] ComPtr<ID3D12RootSignature> make_root_uav_signature(ID3D12Device* device)
{
    D3D12_ROOT_PARAMETER parameter{};
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    parameter.Descriptor.ShaderRegister = 0U;
    D3D12_ROOT_SIGNATURE_DESC rsd{};
    rsd.NumParameters = 1U;
    rsd.pParameters = &parameter;
    ComPtr<ID3DBlob> sig;
    ComPtr<ID3DBlob> err;
    ComPtr<ID3D12RootSignature> root;
    if (SUCCEEDED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err)))
    {
        (void)device->CreateRootSignature(0U, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&root));
    }
    return root;
}

[[nodiscard]] std::filesystem::path first_gpu_dump(const std::filesystem::path& dir)
{
    std::error_code ec;
    for (std::filesystem::directory_iterator it{dir, ec}, end; it != end; it.increment(ec))
    {
        if (it->path().extension() == ".dmp" && it->path().filename().string().starts_with("gpu_"))
        {
            return it->path();
        }
    }
    return {};
}

// Re-enter this executable on the hidden child case with the given environment; bounded at 60 s.
[[nodiscard]] DWORD run_fault_child(const std::filesystem::path& dump_dir, const wchar_t* adapter)
{
    wchar_t self[MAX_PATH] = {};
    const DWORD length = GetModuleFileNameW(nullptr, self, MAX_PATH);
    if (length == 0U || length >= MAX_PATH)
    {
        return kFaultSpawnFailed;
    }
    wchar_t command[MAX_PATH + 64] = {}; // CreateProcessW needs a writable command line
    if (swprintf_s(command, L"\"%ls\" \"[.dx12-device-fault-child]\"", self) < 0)
    {
        return kFaultSpawnFailed;
    }
    (void)SetEnvironmentVariableW(L"CRD_DX12_FAULT_DIR", dump_dir.c_str());
    (void)SetEnvironmentVariableW(L"CRD_DX12_FAULT_ADAPTER", adapter);
    STARTUPINFOW startup{};
    PROCESS_INFORMATION process{};
    startup.cb = sizeof(startup);
    const BOOL made =
        CreateProcessW(nullptr, command, nullptr, nullptr, FALSE, 0U, nullptr, nullptr, &startup, &process);
    (void)SetEnvironmentVariableW(L"CRD_DX12_FAULT_DIR", nullptr);
    (void)SetEnvironmentVariableW(L"CRD_DX12_FAULT_ADAPTER", nullptr);
    if (made == 0)
    {
        return kFaultSpawnFailed;
    }
    DWORD code = kFaultChildTimedOut;
    if (WaitForSingleObject(process.hProcess, 60000U) == WAIT_OBJECT_0)
    {
        (void)GetExitCodeProcess(process.hProcess, &code);
    }
    else
    {
        (void)TerminateProcess(process.hProcess, kFaultChildTimedOut);
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return code;
}

void print_record(const g::detail::Dx12RemovalRecord& record)
{
    const g::detail::Dx12DredReport& d = record.dred;
    std::printf("record: origin=%u sequence=%llu reason=%08X state=%u breadcrumbs=%u (%08X) nodes=%u/%u "
                "page_fault=%u (%08X) va=%llx existing=%u freed=%u\n",
                static_cast<unsigned>(record.origin), static_cast<unsigned long long>(record.sequence),
                static_cast<unsigned>(d.removal_reason), static_cast<unsigned>(d.device_state),
                static_cast<unsigned>(d.breadcrumbs), static_cast<unsigned>(d.breadcrumbs_result), d.nodes_stored,
                d.node_count, static_cast<unsigned>(d.page_fault), static_cast<unsigned>(d.page_fault_result),
                static_cast<unsigned long long>(d.page_fault_va), d.existing_count, d.freed_count);
    for (crd::u32 n = 0; n < d.nodes_stored; ++n)
    {
        const g::detail::Dx12DredBreadcrumbNode& node = d.nodes[n];
        std::printf("  node %u list='%s' queue='%s' ops=%u last=%u(%d) contexts=%u in_flight=%d '%s'\n", n,
                    node.command_list, node.command_queue, node.op_count, node.last_completed,
                    static_cast<int>(node.has_last_completed), node.context_count,
                    static_cast<int>(node.has_in_flight_pass), node.in_flight_pass.text);
        for (crd::u32 i = 0; i < node.ops_stored; ++i)
        {
            std::printf("    op %u = %u\n", i, static_cast<unsigned>(node.ops[i]));
        }
        for (crd::u32 c = 0; c < node.contexts_stored; ++c)
        {
            std::printf("    context @%u '%s'\n", node.contexts[c].op_index, node.contexts[c].text);
        }
    }
    for (crd::u32 a = 0; a < d.allocations_stored; ++a)
    {
        std::printf("  allocation %u '%s' type=%u\n", a, d.allocations[a].name, d.allocations[a].type);
    }
    (void)std::fflush(stdout);
}

// What a real fault must leave in the bundle: an observed removal (never the engine's own), DRED's fault state, the
// faulting pass resolved from the markers (the second pass, not the completed first one), and the released buffer
// among the page fault's recently freed allocations under its retired Cerid identity.
void check_fault_bundle(const std::filesystem::path& dir)
{
    const std::filesystem::path bundle = first_gpu_dump(dir);
    REQUIRE_FALSE(bundle.empty());
    g::detail::Dx12RemovalRecord record{};
    REQUIRE(g::detail::dx12_read_removal_bundle(bundle.c_str(), record) == g::detail::Dx12BundleRead::Ok);
    print_record(record);
    const g::detail::Dx12DredReport& d = record.dred;
    CHECK(record.origin == g::detail::Dx12RemovalOrigin::Observed);
    CHECK(d.removal_reason < 0); // a failure HRESULT
    CHECK((d.device_state == g::detail::Dx12DredDeviceState::PageFault ||
           d.device_state == g::detail::Dx12DredDeviceState::Fault));
    CHECK(d.breadcrumbs == g::detail::Dx12DredQuery::Ok);
    bool pass_resolved = false;
    for (crd::u32 n = 0; n < d.nodes_stored; ++n)
    {
        const g::detail::Dx12DredBreadcrumbNode& node = d.nodes[n];
        if (node.has_in_flight_pass && std::strstr(node.in_flight_pass.text, "] fault pass") != nullptr)
        {
            pass_resolved = node.in_flight_pass.identity.kind == g::ObjectKind::Pass;
        }
    }
    CHECK(pass_resolved);
    CHECK(d.page_fault == g::detail::Dx12DredQuery::Ok);
    bool victim_freed = false;
    for (crd::u32 a = d.existing_count; a < d.allocations_stored; ++a)
    {
        if (std::strstr(d.allocations[a].name, "] fault-victim") != nullptr)
        {
            const g::ObjectIdentity& id = d.allocations[a].identity;
            victim_freed = id.valid() && id.kind == g::ObjectKind::Resource;
        }
    }
    CHECK(victim_freed);
}

[[nodiscard]] std::filesystem::path fresh_dump_dir(const char* tag)
{
    char leaf[96];
    (void)std::snprintf(leaf, sizeof(leaf), "crd_dx12_fault_%s_%lu", tag,
                        static_cast<unsigned long>(GetCurrentProcessId()));
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / leaf;
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    return dir;
}
} // namespace

TEST_CASE("DIAG.7b(h) device fault child (spawned by the fault tests, never run alone)", "[.dx12-device-fault-child]")
{
    char dir_storage[256];
    char adapter_storage[256];
    const auto dir = env_or("CRD_DX12_FAULT_DIR", {}, dir_storage);
    const bool hardware = env_or("CRD_DX12_FAULT_ADAPTER", "warp", adapter_storage) == "hardware";
    REQUIRE_FALSE(dir.empty());
    REQUIRE(crd::crash::InstallResult::Ok == crd::crash::install(dir_storage)); // NUL-terminated by the read

    crd::memory::TlsfAllocator allocator(16U << 20U, nullptr, "DX12 device fault child");
    const g::DxilCompileResult cs = g::compile_hlsl_to_dxil(
        g::ShaderStage::Compute, crd::containers::StringView(kFaultHlsl), "fault_cs", &allocator);
    if (!cs.ok)
    {
        std::_Exit(static_cast<int>(kFaultNoCompiler));
    }

    // (h1) the explicit WARP adapter; (h2) the default device, which the production classifier must call hardware.
    ComPtr<IDXGIAdapter1> warp;
    if (hardware)
    {
        if (g::dx12_default_adapter_kind() != g::Dx12AdapterKind::Hardware)
        {
            std::_Exit(static_cast<int>(kFaultNotHardware));
        }
    }
    else
    {
        ComPtr<IDXGIFactory4> factory;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) || FAILED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp))))
        {
            std::_Exit(static_cast<int>(kFaultNoAdapter));
        }
    }
    std::printf("[dx12-fault-child] adapter=%s\n", hardware ? "hardware (default device)" : "WARP (software)");

    g::detail::Dx12DeviceScope scope; // no validation: GBV would patch the access, and the fault must be real
    scope.request_dred(true, true);
    ComPtr<ID3D12Device> device;
    if (FAILED(scope.create(device, warp.Get())))
    {
        std::_Exit(static_cast<int>(kFaultNoAdapter));
    }
    const g::detail::Dx12DredActivation dred = scope.dred();
    if (dred.breadcrumbs != g::detail::Dx12DredSetup::Set || dred.page_faults != g::detail::Dx12DredSetup::Set ||
        !dred.breadcrumb_contexts || !dred.readable)
    {
        std::_Exit(static_cast<int>(kFaultNoDred));
    }

    const ComPtr<ID3D12RootSignature> root = make_root_uav_signature(device.Get());
    REQUIRE(root != nullptr);
    D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc{};
    pso_desc.pRootSignature = root.Get();
    pso_desc.CS.pShaderBytecode = cs.dxil.data();
    pso_desc.CS.BytecodeLength = cs.dxil.size();
    ComPtr<ID3D12PipelineState> pso;
    REQUIRE(SUCCEEDED(device->CreateComputePipelineState(&pso_desc, IID_PPV_ARGS(&pso))));
    (void)g::detail::dx12_attach_identity(pso.Get(), g::ObjectKind::Program, "fault-cs");

    ComPtr<ID3D12Resource> clean_target = make_uav_buffer(device.Get());
    ComPtr<ID3D12Resource> victim = make_uav_buffer(device.Get());
    REQUIRE(clean_target != nullptr);
    REQUIRE(victim != nullptr);
    (void)g::detail::dx12_attach_identity(clean_target.Get(), g::ObjectKind::Resource, "fault-clean-target");
    const g::ObjectIdentity victim_id =
        g::detail::dx12_attach_identity(victim.Get(), g::ObjectKind::Resource, "fault-victim");

    D3D12_COMMAND_QUEUE_DESC queue_desc{};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> commands;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    REQUIRE(SUCCEEDED(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue))));
    REQUIRE(SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&commands))));
    REQUIRE(SUCCEEDED(
        device->CreateCommandList(0U, D3D12_COMMAND_LIST_TYPE_DIRECT, commands.Get(), nullptr, IID_PPV_ARGS(&list))));
    REQUIRE(SUCCEEDED(device->CreateFence(0U, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))));
    (void)queue->SetName(L"dx12-fault-queue");
    (void)list->SetName(L"dx12-fault-list"); // plain: the pass must come from the markers, not the list name

    g::IdentityRegistry& registry = g::identity_registry();
    const g::ObjectIdentity clean_pass = registry.mint(g::ObjectKind::Pass);
    const g::ObjectIdentity fault_pass = registry.mint(g::ObjectKind::Pass);
    list->SetComputeRootSignature(root.Get());
    list->SetPipelineState(pso.Get());
    {
        const g::detail::Dx12PassEventScope event(list.Get(), clean_pass, "clean pass");
        list->SetComputeRootUnorderedAccessView(0U, clean_target->GetGPUVirtualAddress());
        list->Dispatch(1U, 1U, 1U);
    }
    {
        const g::detail::Dx12PassEventScope event(list.Get(), fault_pass, "fault pass");
        list->SetComputeRootUnorderedAccessView(0U, victim->GetGPUVirtualAddress());
        list->Dispatch(kFaultGroups, 1U, 1U);
    }
    g::detail::dx12_detach_identity(victim_id); // the production retire-then-release order
    victim.Reset();                             // the hazard: the recorded store now targets released memory

    UINT64 value = 0U;
    bool submitted = false;
    const HRESULT submit = g::detail::dx12_submit(device.Get(), queue.Get(), list.Get(), fence.Get(), value, submitted);
    HRESULT wait = submit;
    if (SUCCEEDED(submit))
    {
        HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        REQUIRE(event != nullptr);
        wait = g::detail::dx12_wait(device.Get(), fence.Get(), value, event, 20000U);
        CloseHandle(event);
    }
    const g::detail::Dx12RemovalRecord record = g::detail::dx12_last_removal();
    std::printf("[dx12-fault-child] submit=%08X wait=%08X reason=%08X\n", static_cast<unsigned>(submit),
                static_cast<unsigned>(wait), static_cast<unsigned>(device->GetDeviceRemovedReason()));
    print_record(record);
    DWORD code = kFaultRecorded;
    if (SUCCEEDED(wait))
    {
        code = kFaultCompleted;
    }
    else if (record.origin != g::detail::Dx12RemovalOrigin::Observed)
    {
        code = kFaultEngineForced;
    }
    else if (record.bundle_result != static_cast<crd::u32>(crd::crash::WriteResult::Ok))
    {
        code = kFaultBundleMissing;
    }
    (void)std::fflush(stdout);
    // Leave without tearing down: the device may be removed with the faulted work still queued.
    std::_Exit(static_cast<int>(code));
}

TEST_CASE("DIAG.7b(h1): a GPU use-after-free on WARP runs the device-fault path end to end",
          "[dx12][validation][dred][device-fault]")
{
    const std::filesystem::path dir = fresh_dump_dir("warp");
    const DWORD child = run_fault_child(dir, L"warp");
    INFO("WARP fault child exit " << child
                                  << " (0 recorded, 30 completed, 20 no DRED, 21 no dxc, 22 no adapter, "
                                     "31 engine-forced, 32 bundle missing, 97 spawn, 98 timeout)");
    if (child == kFaultNoCompiler)
    {
        SKIP("dxc/DXIL unavailable; the device-fault workload cannot be built");
    }
    // Measured: WARP completes the workload with the device live (software-provider limit; real faults are (h2)).
    // A WARP that faults must leave the full evidence instead. Anything else is a broken path.
    REQUIRE((child == kFaultCompleted || child == kFaultRecorded));
    if (child == kFaultCompleted)
    {
        std::printf("WARP: no device fault (software-provider limit); the real fault is the hardware gate (h2)\n");
        CHECK(first_gpu_dump(dir).empty()); // nothing was removed, so nothing was written
    }
    else
    {
        check_fault_bundle(dir);
    }
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

TEST_CASE("DIAG.7b(h2): a GPU use-after-free on the hardware adapter is a real page fault in the bundle",
          "[.dx12-hardware-device-fault]")
{
    char opt_in[256];
    if (env_or("CRD_DX12_HARDWARE_FAULT_OPT_IN", {}, opt_in) != "1")
    {
        SKIP("hardware device fault: set CRD_DX12_HARDWARE_FAULT_OPT_IN=1 on an idle machine to run it");
    }
    const std::filesystem::path dir = fresh_dump_dir("hardware");
    const DWORD child = run_fault_child(dir, L"hardware");
    INFO("hardware fault child exit " << child << " (0 recorded, 23 not hardware, 30 completed, 31 engine-forced)");
    REQUIRE(child == kFaultRecorded);
    check_fault_bundle(dir);
    std::printf("hardware fault bundle kept in %ls\n", dir.c_str()); // evidence for this GPU; not removed
}

// DIAG.7b(h1) / DIAG.7a: the in-flight pass is resolved from DRED's lists. One history, read at several stopping
// points: pass A completes, pass B holds a tool's own marker around its first dispatch, and pass C has no context
// string. A stopped op resolves to the innermost open Cerid pass; a closed pass, a context-less one and a finished
// list resolve to none.
TEST_CASE("DIAG.7b(h1): DRED resolves the Cerid pass a stopped command list was inside", "[dx12][validation][dred]")
{
    g::IdentityRegistry& registry = g::identity_registry();
    const g::ObjectIdentity pass_a = registry.mint(g::ObjectKind::Pass);
    const g::ObjectIdentity pass_b = registry.mint(g::ObjectKind::Pass);
    const g::ObjectIdentity resource = registry.mint(g::ObjectKind::Resource);
    char name_a[128];
    char name_b[128];
    char name_r[128];
    REQUIRE(g::format_debug_name(pass_a, "clean pass", name_a, sizeof(name_a)) != 0U);
    REQUIRE(g::format_debug_name(pass_b, "fault pass", name_b, sizeof(name_b)) != 0U);
    REQUIRE(g::format_debug_name(resource, "not a pass", name_r, sizeof(name_r)) != 0U);
    wchar_t wide_a[128] = {};
    wchar_t wide_b[128] = {};
    wchar_t wide_r[128] = {};
    for (crd::usize i = 0; name_a[i] != '\0'; ++i)
    {
        wide_a[i] = static_cast<wchar_t>(name_a[i]);
    }
    for (crd::usize i = 0; name_b[i] != '\0'; ++i)
    {
        wide_b[i] = static_cast<wchar_t>(name_b[i]);
    }
    for (crd::usize i = 0; name_r[i] != '\0'; ++i)
    {
        wide_r[i] = static_cast<wchar_t>(name_r[i]);
    }

    constexpr D3D12_AUTO_BREADCRUMB_OP ob = D3D12_AUTO_BREADCRUMB_OP_BEGINEVENT;
    constexpr D3D12_AUTO_BREADCRUMB_OP oe = D3D12_AUTO_BREADCRUMB_OP_ENDEVENT;
    constexpr D3D12_AUTO_BREADCRUMB_OP od = D3D12_AUTO_BREADCRUMB_OP_DISPATCH;
    //                                           0   1   2   3   4   5   6   7   8   9  10  11
    const D3D12_AUTO_BREADCRUMB_OP history[] = {ob, od, oe, ob, ob, od, oe, od, oe, ob, od, oe};
    constexpr crd::u32 op_count = 12U;
    // Pass A at 0, pass B at 3, a tool marker carrying a Resource token (not a pass) at 4; pass C at 9 has no context.
    D3D12_DRED_BREADCRUMB_CONTEXT contexts[] = {{0U, wide_a}, {3U, wide_b}, {4U, wide_r}};

    struct Stop
    {
        UINT32 value;
        bool has_pass;
        bool is_b; // otherwise A
    };
    const Stop stops[] = {
        {1U, true, false},   // dispatch in A
        {2U, true, false},   // the EndEvent of A is where it stopped: still A
        {3U, true, true},    // stopped at B's own marker: B is open
        {5U, true, true},    // dispatch inside the tool marker inside B: B (the marker is not a pass)
        {7U, true, true},    // dispatch in B after the tool marker closed
        {10U, false, false}, // dispatch in C, which carries no context: A and B are closed, so none
        {12U, false, false}, // the list finished
    };
    constexpr crd::u32 stop_count = sizeof(stops) / sizeof(stops[0]);
    D3D12_AUTO_BREADCRUMB_NODE1 nodes[stop_count]{};
    for (crd::u32 i = 0; i < stop_count; ++i)
    {
        nodes[i].pCommandListDebugNameA = "plain list";
        nodes[i].BreadcrumbCount = op_count;
        nodes[i].pLastBreadcrumbValue = &stops[i].value;
        nodes[i].pCommandHistory = history;
        nodes[i].BreadcrumbContextsCount = 3U;
        nodes[i].pBreadcrumbContexts = contexts;
        nodes[i].pNext = (i + 1U < stop_count) ? &nodes[i + 1U] : nullptr;
    }
    g::detail::Dx12DredReport report{};
    g::detail::dx12_dred_fill_breadcrumbs(&nodes[0], report);
    REQUIRE(report.nodes_stored == stop_count);
    for (crd::u32 i = 0; i < stop_count; ++i)
    {
        const g::detail::Dx12DredBreadcrumbNode& node = report.nodes[i];
        INFO("stopped at op " << stops[i].value);
        CHECK(node.has_in_flight_pass == stops[i].has_pass);
        if (stops[i].has_pass)
        {
            CHECK(node.in_flight_pass.identity == (stops[i].is_b ? pass_b : pass_a));
            CHECK(node.in_flight_pass.op_index == (stops[i].is_b ? 3U : 0U));
        }
    }
    // The history and contexts themselves are kept, narrowed and mapped.
    const g::detail::Dx12DredBreadcrumbNode& first = report.nodes[0];
    CHECK(first.ops_stored == op_count);
    CHECK(first.ops[9] == static_cast<crd::u8>(ob));
    CHECK(first.context_count == 3U);
    REQUIRE(first.contexts_stored == 3U);
    CHECK(std::strcmp(first.contexts[1].text, name_b) == 0);
    CHECK(first.contexts[1].identity == pass_b);
    CHECK(first.contexts[2].identity == resource);

    // Bounds: more contexts than are stored still resolve (the walk reads DRED's own list), the stored copy is capped,
    // and a non-ASCII code unit is narrowed to '?'.
    wchar_t noisy[] = {static_cast<wchar_t>(0x00E9), L' ', L'm', L'a', L'r', L'k', L'e', L'r', L'\0'};
    D3D12_DRED_BREADCRUMB_CONTEXT many[] = {{0U, noisy}, {1U, noisy}, {2U, noisy},
                                            {4U, noisy}, {5U, noisy}, {3U, wide_b}};
    const UINT32 at_seven = 7U;
    D3D12_AUTO_BREADCRUMB_NODE1 crowded{};
    crowded.BreadcrumbCount = op_count;
    crowded.pLastBreadcrumbValue = &at_seven;
    crowded.pCommandHistory = history;
    crowded.BreadcrumbContextsCount = 6U;
    crowded.pBreadcrumbContexts = many;
    g::detail::Dx12DredReport bounded{};
    g::detail::dx12_dred_fill_breadcrumbs(&crowded, bounded);
    REQUIRE(bounded.nodes_stored == 1U);
    CHECK(bounded.nodes[0].context_count == 6U);
    CHECK(bounded.nodes[0].contexts_stored == g::detail::kDx12DredMaxContexts);
    CHECK(bounded.nodes[0].contexts[0].text[0] == '?');
    CHECK(bounded.nodes[0].has_in_flight_pass);
    CHECK(bounded.nodes[0].in_flight_pass.identity == pass_b);

    (void)registry.retire(pass_a);
    (void)registry.retire(pass_b);
    (void)registry.retire(resource);
}
