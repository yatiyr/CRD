// crd-core -- the process-wide fault seams (file reads, GPU submissions, kernel self-checks, the hardware check, the
// GPU device checks). See crd/core/fault_seam.hpp.

#include <crd/core/fault_seam.hpp>

#include <atomic>
#include <mutex>

// CRD_FAULT_SEAMS comes from engine/foundation/core/CMakeLists.txt: 0 for a shipping build, 1 otherwise.
#if !defined(CRD_FAULT_SEAMS)
#error "CRD_FAULT_SEAMS must be defined by the crd-core target"
#endif

namespace crd
{

#if CRD_FAULT_SEAMS

namespace
{
// One seam's hook. The installed flag is the hot path's only cost: a consult with no hook never takes the mutex.
template <typename Hook> struct HookSlot
{
    std::atomic<bool> installed{false};
    std::mutex mutex;
    Hook hook = nullptr;
    void* user = nullptr;

    bool install(Hook h, void* u) noexcept
    {
        if (h == nullptr)
        {
            return false;
        }
        const std::lock_guard<std::mutex> guard(mutex);
        if (hook != nullptr)
        {
            return false;
        }
        hook = h;
        user = u;
        installed.store(true, std::memory_order_release);
        return true;
    }

    void uninstall() noexcept
    {
        const std::lock_guard<std::mutex> guard(mutex);
        installed.store(false, std::memory_order_release);
        hook = nullptr;
        user = nullptr;
    }

    // The hook's answer to `args`, or `none` when no hook is installed.
    template <typename Result, typename... Args> Result consult(Result none, Args... args) noexcept
    {
        if (!installed.load(std::memory_order_acquire))
        {
            return none;
        }
        const std::lock_guard<std::mutex> guard(mutex);
        return hook != nullptr ? hook(user, args...) : none;
    }
};

HookSlot<IoFaultHook> g_io;
HookSlot<GpuFaultHook> g_gpu;
HookSlot<KernelCheckFaultHook> g_kernel;
HookSlot<HardwareCheckFaultHook> g_hardware;
HookSlot<DeviceCheckFaultHook> g_device;
} // namespace

bool fault_seams_compiled() noexcept
{
    return true;
}

bool install_io_fault_hook(IoFaultHook hook, void* user) noexcept
{
    return g_io.install(hook, user);
}

void uninstall_io_fault_hook() noexcept
{
    g_io.uninstall();
}

IoFault consult_io_fault(IoFaultOp op, const char* path, usize path_bytes, u64 offset, u64 size) noexcept
{
    return g_io.consult(IoFault{}, op, path, path_bytes, offset, size);
}

bool install_gpu_fault_hook(GpuFaultHook hook, void* user) noexcept
{
    return g_gpu.install(hook, user);
}

void uninstall_gpu_fault_hook() noexcept
{
    g_gpu.uninstall();
}

GpuFaultAction consult_gpu_fault(GpuFaultOp op, const char* operation) noexcept
{
    return g_gpu.consult(GpuFaultAction::Proceed, op, operation);
}

bool install_kernel_check_fault_hook(KernelCheckFaultHook hook, void* user) noexcept
{
    return g_kernel.install(hook, user);
}

void uninstall_kernel_check_fault_hook() noexcept
{
    g_kernel.uninstall();
}

bool consult_kernel_check_fault(const char* family, CpuLevel level, CpuFeatureSet features) noexcept
{
    return g_kernel.consult(false, family, level, features);
}

bool install_hardware_check_fault_hook(HardwareCheckFaultHook hook, void* user) noexcept
{
    return g_hardware.install(hook, user);
}

void uninstall_hardware_check_fault_hook() noexcept
{
    g_hardware.uninstall();
}

bool consult_hardware_check_fault(u32 cpu, const char* arm) noexcept
{
    return g_hardware.consult(false, cpu, arm);
}

bool install_device_check_fault_hook(DeviceCheckFaultHook hook, void* user) noexcept
{
    return g_device.install(hook, user);
}

void uninstall_device_check_fault_hook() noexcept
{
    g_device.uninstall();
}

bool consult_device_check_fault(const char* backend, const char* adapter) noexcept
{
    return g_device.consult(false, backend, adapter);
}

#else

bool fault_seams_compiled() noexcept
{
    return false;
}

bool install_io_fault_hook(IoFaultHook /*hook*/, void* /*user*/) noexcept
{
    return false;
}

void uninstall_io_fault_hook() noexcept {}

IoFault consult_io_fault(IoFaultOp /*op*/, const char* /*path*/, usize /*path_bytes*/, u64 /*offset*/,
                         u64 /*size*/) noexcept
{
    return IoFault{};
}

bool install_gpu_fault_hook(GpuFaultHook /*hook*/, void* /*user*/) noexcept
{
    return false;
}

void uninstall_gpu_fault_hook() noexcept {}

GpuFaultAction consult_gpu_fault(GpuFaultOp /*op*/, const char* /*operation*/) noexcept
{
    return GpuFaultAction::Proceed;
}

bool install_kernel_check_fault_hook(KernelCheckFaultHook /*hook*/, void* /*user*/) noexcept
{
    return false;
}

void uninstall_kernel_check_fault_hook() noexcept {}

bool consult_kernel_check_fault(const char* /*family*/, CpuLevel /*level*/, CpuFeatureSet /*features*/) noexcept
{
    return false;
}

bool install_hardware_check_fault_hook(HardwareCheckFaultHook /*hook*/, void* /*user*/) noexcept
{
    return false;
}

void uninstall_hardware_check_fault_hook() noexcept {}

bool consult_hardware_check_fault(u32 /*cpu*/, const char* /*arm*/) noexcept
{
    return false;
}

bool install_device_check_fault_hook(DeviceCheckFaultHook /*hook*/, void* /*user*/) noexcept
{
    return false;
}

void uninstall_device_check_fault_hook() noexcept {}

bool consult_device_check_fault(const char* /*backend*/, const char* /*adapter*/) noexcept
{
    return false;
}

#endif

} // namespace crd
