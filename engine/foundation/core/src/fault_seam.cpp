// crd-core -- the process-wide I/O fault seam. See crd/core/fault_seam.hpp.

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
// The installed flag is the hot path's only cost: a read with no hook never takes the mutex.
std::atomic<bool> g_io_hook_installed{false};
std::mutex        g_io_hook_mutex;
IoFaultHook       g_io_hook      = nullptr;
void*             g_io_hook_user = nullptr;
} // namespace

bool fault_seams_compiled() noexcept
{
    return true;
}

bool install_io_fault_hook(IoFaultHook hook, void* user) noexcept
{
    if (hook == nullptr)
    {
        return false;
    }
    const std::lock_guard<std::mutex> guard(g_io_hook_mutex);
    if (g_io_hook != nullptr)
    {
        return false;
    }
    g_io_hook      = hook;
    g_io_hook_user = user;
    g_io_hook_installed.store(true, std::memory_order_release);
    return true;
}

void uninstall_io_fault_hook() noexcept
{
    const std::lock_guard<std::mutex> guard(g_io_hook_mutex);
    g_io_hook_installed.store(false, std::memory_order_release);
    g_io_hook      = nullptr;
    g_io_hook_user = nullptr;
}

IoFault consult_io_fault(IoFaultOp op, const char* path, usize path_bytes, u64 offset, u64 size) noexcept
{
    if (!g_io_hook_installed.load(std::memory_order_acquire))
    {
        return IoFault{};
    }
    const std::lock_guard<std::mutex> guard(g_io_hook_mutex);
    if (g_io_hook == nullptr)
    {
        return IoFault{};
    }
    return g_io_hook(g_io_hook_user, op, path, path_bytes, offset, size);
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

#endif

} // namespace crd
