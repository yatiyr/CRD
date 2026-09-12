#include "dx12_execution.hpp"

#include <crd/core/assert.hpp>

#include <limits>

#include <wrl/client.h>

namespace crd::gpu::detail
{
namespace
{
void stop_failed_device(ID3D12Device* device) noexcept
{
    if (FAILED(device->GetDeviceRemovedReason())) { return; }
    Microsoft::WRL::ComPtr<ID3D12Device5> control;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&control))))
    {
        // Without completion or removal, returning would free memory still referenced by queued GPU work.
        CRD_FATAL("DX12 completion failed and the runtime cannot safely remove the device");
    }
    control->RemoveDevice();
}
} // namespace

HRESULT dx12_reset(ID3D12CommandAllocator* allocator, ID3D12GraphicsCommandList* list) noexcept
{
    if (allocator == nullptr || list == nullptr)
    {
        dx12_execution_failure(E_POINTER, "Reset missing command allocator/list");
        return E_POINTER;
    }
    HRESULT result = allocator->Reset();
    if (SUCCEEDED(result)) { result = list->Reset(allocator, nullptr); }
    if (FAILED(result)) { dx12_execution_failure(result, "Reset command allocator/list"); }
    return result;
}

HRESULT dx12_submit(ID3D12Device* device, ID3D12CommandQueue* queue, ID3D12GraphicsCommandList* list,
                    ID3D12Fence* fence, UINT64& value, bool& submitted) noexcept
{
    submitted = false;
    if (device == nullptr || queue == nullptr || list == nullptr || fence == nullptr)
    {
        dx12_execution_failure(E_POINTER, "Submit missing device/queue/list/fence");
        return E_POINTER;
    }
    HRESULT result = device->GetDeviceRemovedReason();
    if (SUCCEEDED(result)) { result = list->Close(); }
    if (SUCCEEDED(result) && value >= std::numeric_limits<UINT64>::max() - 1U) { result = E_UNEXPECTED; }
    if (SUCCEEDED(result))
    {
        ID3D12CommandList* lists[] = {list};
        queue->ExecuteCommandLists(1U, lists);
        submitted = true;
        result = device->GetDeviceRemovedReason();
        if (SUCCEEDED(result)) { result = queue->Signal(fence, value + 1U); }
        if (SUCCEEDED(result)) { ++value; }
    }
    if (FAILED(result))
    {
        dx12_execution_failure(result, "Close/execute/signal command list");
        if (submitted) { stop_failed_device(device); }
    }
    return result;
}

HRESULT dx12_signal(ID3D12Device* device, ID3D12CommandQueue* queue, ID3D12Fence* fence, UINT64& value) noexcept
{
    if (device == nullptr || queue == nullptr || fence == nullptr)
    {
        dx12_execution_failure(E_POINTER, "Signal missing device/queue/fence");
        if (device != nullptr) { stop_failed_device(device); }
        return E_POINTER;
    }
    HRESULT result = device->GetDeviceRemovedReason();
    if (SUCCEEDED(result) && value >= std::numeric_limits<UINT64>::max() - 1U) { result = E_UNEXPECTED; }
    if (SUCCEEDED(result)) { result = queue->Signal(fence, value + 1U); }
    if (SUCCEEDED(result)) { ++value; }
    else
    {
        dx12_execution_failure(result, "Signal queue completion");
        stop_failed_device(device);
    }
    return result;
}

HRESULT dx12_wait(ID3D12Device* device, ID3D12Fence* fence, UINT64 value, HANDLE event, DWORD timeout_ms) noexcept
{
    if (device == nullptr || fence == nullptr || event == nullptr || timeout_ms == INFINITE)
    {
        const HRESULT result = timeout_ms == INFINITE ? E_INVALIDARG : E_POINTER;
        dx12_execution_failure(result, "Wait missing device/fence/event or unbounded timeout");
        if (device != nullptr) { stop_failed_device(device); }
        return result;
    }
    HRESULT result = device->GetDeviceRemovedReason();
    UINT64 completed = fence->GetCompletedValue();
    if (SUCCEEDED(result) && completed == std::numeric_limits<UINT64>::max()) { result = DXGI_ERROR_DEVICE_REMOVED; }
    if (SUCCEEDED(result) && completed < value)
    {
        result = fence->SetEventOnCompletion(value, event);
        if (SUCCEEDED(result))
        {
            // Bound the provider wait as well as the external test process. A timeout is failure, never completion.
            const DWORD wait = WaitForSingleObject(event, timeout_ms);
            if (wait != WAIT_OBJECT_0)
            {
                DWORD error = ERROR_GEN_FAILURE;
                if (wait == WAIT_FAILED) { error = GetLastError(); }
                else if (wait == WAIT_TIMEOUT) { error = ERROR_TIMEOUT; }
                if (error == ERROR_SUCCESS) { error = ERROR_GEN_FAILURE; }
                result = HRESULT_FROM_WIN32(error);
            }
        }
        if (SUCCEEDED(result))
        {
            result = device->GetDeviceRemovedReason();
            completed = fence->GetCompletedValue();
            if (SUCCEEDED(result) && completed == std::numeric_limits<UINT64>::max())
            {
                result = DXGI_ERROR_DEVICE_REMOVED;
            }
            else if (SUCCEEDED(result) && completed < value) { result = E_UNEXPECTED; }
        }
    }
    if (FAILED(result))
    {
        dx12_execution_failure(result, "Wait for command completion");
        stop_failed_device(device);
    }
    return result;
}
} // namespace crd::gpu::detail
