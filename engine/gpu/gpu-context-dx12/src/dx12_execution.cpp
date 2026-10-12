#include "dx12_execution.hpp"
#include "dx12_dred.hpp" // DIAG.7b(d): record DRED on every stopped device

#include <crd/core/assert.hpp>
#include <crd/core/fault_seam.hpp> // DIAG.9b: an authorized test can lose the device at a submission
#include <crd/gpu/driver_witness.hpp> // DIAG.10c: ExecuteCommandLists runs under the mode witness
#include <crd/gpu/queue_pressure.hpp> // DIAG.10c: every fence step is reported to the queue-depth tracker

#include <atomic>
#include <limits>
#include <mutex>
#include <new>

#include <wrl/client.h>

namespace crd::gpu::detail
{
namespace
{
// DIAG.10c: the queue-depth tracker keys a device by its address, and a later device may be created at the address of
// one that is gone. D3D12 releases an object's private-data interfaces when the object is destroyed, so a watch
// attached to the device the first time a seam reports a step on it tells the tracker exactly when that device ends.
// {6A1E3C5D-0F27-4B8E-9C41-2D7B5E0A9F13}
constexpr GUID kQueueDepthWatchGuid = {0x6a1e3c5dU, 0x0f27U, 0x4b8eU,
                                       {0x9cU, 0x41U, 0x2dU, 0x7bU, 0x5eU, 0x0aU, 0x9fU, 0x13U}};

class DeviceEndWatch final : public IUnknown
{
public:
    explicit DeviceEndWatch(const void* device) noexcept : m_device(device) {}
    DeviceEndWatch(const DeviceEndWatch&)            = delete;
    DeviceEndWatch& operator=(const DeviceEndWatch&) = delete;
    DeviceEndWatch(DeviceEndWatch&&)                 = delete;
    DeviceEndWatch& operator=(DeviceEndWatch&&)      = delete;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) noexcept override
    {
        if (out == nullptr)
        {
            return E_POINTER;
        }
        if (riid == __uuidof(IUnknown))
        {
            *out = static_cast<IUnknown*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() noexcept override
    {
        return m_refs.fetch_add(1U, std::memory_order_relaxed) + 1U;
    }
    ULONG STDMETHODCALLTYPE Release() noexcept override
    {
        const ULONG left = m_refs.fetch_sub(1U, std::memory_order_acq_rel) - 1U;
        if (left == 0U)
        {
            queue_forget_device(m_device); // the device is being destroyed: its counts end with it
            delete this;
        }
        return left;
    }

private:
    ~DeviceEndWatch() = default;

    std::atomic<ULONG> m_refs{1U};
    const void*        m_device = nullptr;
};

// Attach the watch once per device. Serialized, so two first submissions on one device cannot replace each other's
// watch (a replaced watch would end the device's counts while it lives).
void watch_device_end(ID3D12Device* device) noexcept
{
    UINT size = 0U;
    if (device->GetPrivateData(kQueueDepthWatchGuid, &size, nullptr) == S_OK)
    {
        return;
    }
    static std::mutex     attach_mutex;
    const std::lock_guard lock(attach_mutex);
    size = 0U;
    if (device->GetPrivateData(kQueueDepthWatchGuid, &size, nullptr) == S_OK)
    {
        return;
    }
    auto* const watch = new (std::nothrow) DeviceEndWatch(device);
    if (watch == nullptr)
    {
        return; // the device's counts then end with its last forgotten queue only
    }
    (void)device->SetPrivateDataInterface(kQueueDepthWatchGuid, watch); // holds its own reference on success
    (void)watch->Release();
}

void stop_failed_device(ID3D12Device* device) noexcept
{
    watch_device_end(device);
    queue_device_lost(device); // DIAG.10c: its pending fence steps end abandoned, whichever way it is removed
    if (FAILED(device->GetDeviceRemovedReason())) // the runtime/driver already removed it: an actual device loss
    {
        dx12_record_removal(device, Dx12RemovalOrigin::Observed);
        return;
    }
    Microsoft::WRL::ComPtr<ID3D12Device5> control;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&control))))
    {
        // Without completion or removal, returning would free memory still referenced by queued GPU work.
        CRD_FATAL("DX12 completion failed and the runtime cannot safely remove the device");
    }
    control->RemoveDevice();
    dx12_record_removal(device, Dx12RemovalOrigin::EngineForced);
}

// DIAG.9b: an injected device loss at a submission. Nothing of the submission is queued; the device is removed so that
// every later call meets a removed device, as after a real loss, and the removal is recorded as Simulated. A runtime
// without ID3D12Device5 cannot remove the device: the submission still fails and the context latches invalid.
void simulate_device_loss(ID3D12Device* device) noexcept
{
    watch_device_end(device);
    queue_device_lost(device); // DIAG.10c
    Microsoft::WRL::ComPtr<ID3D12Device5> control;
    if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&control))))
    {
        control->RemoveDevice();
    }
    dx12_record_removal(device, Dx12RemovalOrigin::Simulated);
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
    if (SUCCEEDED(result))
    {
        result = list->Reset(allocator, nullptr);
    }
    if (FAILED(result))
    {
        dx12_execution_failure(result, "Reset command allocator/list");
    }
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
    if (SUCCEEDED(result))
    {
        result = list->Close();
    }
    if (SUCCEEDED(result) && value >= std::numeric_limits<UINT64>::max() - 1U)
    {
        result = E_UNEXPECTED;
    }
    // DIAG.9b: the GPU fault seam, consulted only for a device that is not already removed, before anything is queued.
    if (SUCCEEDED(result)
        && crd::consult_gpu_fault(crd::GpuFaultOp::Submit, "Close/execute/signal command list")
               == crd::GpuFaultAction::LoseDevice)
    {
        simulate_device_loss(device);
        result = DXGI_ERROR_DEVICE_REMOVED;
    }
    if (SUCCEEDED(result))
    {
        ID3D12CommandList* lists[] = {list};
        witness_driver_call(DriverCall::Submit, "ExecuteCommandLists",
                            [&]() { queue->ExecuteCommandLists(1U, lists); });
        submitted = true;
        result = device->GetDeviceRemovedReason();
        if (SUCCEEDED(result))
        {
            result = queue->Signal(fence, value + 1U);
        }
        if (SUCCEEDED(result))
        {
            ++value;
            watch_device_end(device);
            queue_step_submitted(device, queue, fence, value); // DIAG.10c: in flight until a wait observes `value`
        }
    }
    if (FAILED(result))
    {
        dx12_execution_failure(result, "Close/execute/signal command list");
        if (submitted)
        {
            stop_failed_device(device);
        }
    }
    return result;
}

HRESULT dx12_signal(ID3D12Device* device, ID3D12CommandQueue* queue, ID3D12Fence* fence, UINT64& value) noexcept
{
    if (device == nullptr || queue == nullptr || fence == nullptr)
    {
        dx12_execution_failure(E_POINTER, "Signal missing device/queue/fence");
        if (device != nullptr)
        {
            stop_failed_device(device);
        }
        return E_POINTER;
    }
    HRESULT result = device->GetDeviceRemovedReason();
    if (SUCCEEDED(result) && value >= std::numeric_limits<UINT64>::max() - 1U)
    {
        result = E_UNEXPECTED;
    }
    if (SUCCEEDED(result))
    {
        result = queue->Signal(fence, value + 1U);
    }
    if (SUCCEEDED(result))
    {
        ++value;
        watch_device_end(device);
        queue_step_submitted(device, queue, fence, value); // DIAG.10c: a bare signal is a step too
    }
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
        if (device != nullptr)
        {
            stop_failed_device(device);
        }
        return result;
    }
    HRESULT result = device->GetDeviceRemovedReason();
    UINT64 completed = fence->GetCompletedValue();
    if (SUCCEEDED(result) && completed == std::numeric_limits<UINT64>::max())
    {
        result = DXGI_ERROR_DEVICE_REMOVED;
    }
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
                if (wait == WAIT_FAILED)
                {
                    error = GetLastError();
                }
                else if (wait == WAIT_TIMEOUT)
                {
                    error = ERROR_TIMEOUT;
                }
                if (error == ERROR_SUCCESS)
                {
                    error = ERROR_GEN_FAILURE;
                }
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
            else if (SUCCEEDED(result) && completed < value)
            {
                result = E_UNEXPECTED;
            }
        }
    }
    if (FAILED(result))
    {
        dx12_execution_failure(result, "Wait for command completion");
        stop_failed_device(device);
    }
    else
    {
        queue_timeline_completed(device, fence, value); // DIAG.10c: retires `value` and everything before it
    }
    return result;
}
} // namespace crd::gpu::detail
