#include "dx12_dred.hpp"

#include <dxgi.h> // DXGI_ERROR_NOT_CURRENTLY_AVAILABLE
#include <wrl/client.h>

#include <mutex>
#include <string_view>

namespace crd::gpu::detail
{
namespace
{
Dx12DredQuery classify(HRESULT result) noexcept
{
    if (SUCCEEDED(result)) { return Dx12DredQuery::Ok; }
    if (result == DXGI_ERROR_NOT_CURRENTLY_AVAILABLE || result == DXGI_ERROR_UNSUPPORTED || result == E_NOINTERFACE)
    {
        return Dx12DredQuery::NotAvailable;
    }
    return Dx12DredQuery::Failed;
}

// Bounded copy of a possibly-null narrow debug name; always NUL-terminated.
void copy_name(char (&out)[kDx12DredNameBytes], const char* name) noexcept
{
    u32 i = 0;
    if (name != nullptr)
    {
        for (; i + 1U < kDx12DredNameBytes && name[i] != '\0'; ++i) { out[i] = name[i]; }
    }
    out[i] = '\0';
}

Dx12DredDeviceState map_state(D3D12_DRED_DEVICE_STATE state) noexcept
{
    switch (state)
    {
    case D3D12_DRED_DEVICE_STATE_HUNG: return Dx12DredDeviceState::Hung;
    case D3D12_DRED_DEVICE_STATE_FAULT: return Dx12DredDeviceState::Fault;
    case D3D12_DRED_DEVICE_STATE_PAGEFAULT: return Dx12DredDeviceState::PageFault;
    case D3D12_DRED_DEVICE_STATE_UNKNOWN:
    default: return Dx12DredDeviceState::Unknown;
    }
}

// (e) The first valid Cerid identity token in a native debug name; invalid when there is none.
ObjectIdentity identity_in(const char* name) noexcept
{
    ObjectIdentity id{};
    if (name != nullptr) { (void)parse(std::string_view{name}, id); }
    return id;
}

void read_allocations(const D3D12_DRED_ALLOCATION_NODE1* node, u32& count, Dx12DredReport& report) noexcept
{
    for (; node != nullptr; node = node->pNext)
    {
        ++count;
        if (report.allocations_stored < kDx12DredMaxAllocs)
        {
            Dx12DredAllocation& out = report.allocations[report.allocations_stored++];
            copy_name(out.name, node->ObjectNameA);
            out.type     = static_cast<u32>(node->AllocationType);
            out.identity = identity_in(node->ObjectNameA);
        }
    }
}
} // namespace

void dx12_dred_fill_breadcrumbs(const D3D12_AUTO_BREADCRUMB_NODE1* head, Dx12DredReport& report) noexcept
{
    for (const D3D12_AUTO_BREADCRUMB_NODE1* node = head; node != nullptr; node = node->pNext)
    {
        ++report.node_count;
        if (report.nodes_stored >= kDx12DredMaxNodes) { continue; }
        Dx12DredBreadcrumbNode& out = report.nodes[report.nodes_stored++];
        copy_name(out.command_list, node->pCommandListDebugNameA);
        copy_name(out.command_queue, node->pCommandQueueDebugNameA);
        out.list_identity  = identity_in(node->pCommandListDebugNameA);
        out.queue_identity = identity_in(node->pCommandQueueDebugNameA);
        out.op_count       = node->BreadcrumbCount;
        if (node->pLastBreadcrumbValue != nullptr)
        {
            out.last_completed     = *node->pLastBreadcrumbValue;
            out.has_last_completed = true;
        }
    }
}

void dx12_dred_fill_page_fault(const D3D12_DRED_PAGE_FAULT_OUTPUT1& output, Dx12DredReport& report) noexcept
{
    report.page_fault_va = static_cast<u64>(output.PageFaultVA);
    read_allocations(output.pHeadExistingAllocationNode, report.existing_count, report);
    read_allocations(output.pHeadRecentFreedAllocationNode, report.freed_count, report);
}

Dx12DredReport dx12_read_dred(ID3D12Device* device) noexcept
{
    Dx12DredReport report{};
    if (device == nullptr) { return report; }
    report.removal_reason = static_cast<i32>(device->GetDeviceRemovedReason());

    Microsoft::WRL::ComPtr<ID3D12DeviceRemovedExtendedData1> dred;
    const HRESULT interface_result = device->QueryInterface(IID_PPV_ARGS(&dred));
    if (FAILED(interface_result))
    {
        report.breadcrumbs        = classify(interface_result);
        report.breadcrumbs_result = static_cast<i32>(interface_result);
        report.page_fault         = report.breadcrumbs;
        report.page_fault_result  = report.breadcrumbs_result;
        return report;
    }

    D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1 breadcrumbs{};
    const HRESULT breadcrumbs_result = dred->GetAutoBreadcrumbsOutput1(&breadcrumbs);
    report.breadcrumbs               = classify(breadcrumbs_result);
    report.breadcrumbs_result        = static_cast<i32>(breadcrumbs_result);
    if (SUCCEEDED(breadcrumbs_result)) { dx12_dred_fill_breadcrumbs(breadcrumbs.pHeadAutoBreadcrumbNode, report); }

    D3D12_DRED_PAGE_FAULT_OUTPUT1 page_fault{};
    const HRESULT page_fault_result = dred->GetPageFaultAllocationOutput1(&page_fault);
    report.page_fault               = classify(page_fault_result);
    report.page_fault_result        = static_cast<i32>(page_fault_result);
    if (SUCCEEDED(page_fault_result)) { dx12_dred_fill_page_fault(page_fault, report); }

    Microsoft::WRL::ComPtr<ID3D12DeviceRemovedExtendedData2> dred2;
    if (SUCCEEDED(dred.As(&dred2))) { report.device_state = map_state(dred2->GetDeviceState()); }
    return report;
}

namespace
{
struct RemovalStore
{
    std::mutex        mutex;
    Dx12RemovalRecord last{};
};

RemovalStore& removal_store() noexcept
{
    static RemovalStore store;
    return store;
}
} // namespace

void dx12_record_removal(ID3D12Device* device, Dx12RemovalOrigin origin) noexcept
{
    const Dx12DredReport report = dx12_read_dred(device); // read outside the lock: queries may be slow after a loss
    RemovalStore&        store  = removal_store();
    const std::lock_guard lock(store.mutex);
    store.last.origin = origin;
    store.last.dred   = report;
    ++store.last.sequence;
}

Dx12RemovalRecord dx12_last_removal() noexcept
{
    RemovalStore&         store = removal_store();
    const std::lock_guard lock(store.mutex);
    return store.last;
}
} // namespace crd::gpu::detail
