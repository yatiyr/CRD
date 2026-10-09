// crd-sandbox -- DIAG.9a: calls a request thread hands to the frame-loop thread. Declared in frame_thread_calls.hpp.

#include "frame_thread_calls.hpp"

namespace crd::sandbox
{
bool FrameThreadCalls::call(CallFn fn, void* user)
{
    std::unique_lock<std::mutex> lock(m_mutex);
    m_changed.wait(lock, [this] { return m_closed || m_slot == Slot::Empty; });
    if (m_closed)
    {
        ++m_refused;
        return false;
    }
    m_fn   = fn;
    m_user = user;
    m_ran  = false;
    m_slot = Slot::Posted;
    m_changed.notify_all();
    m_changed.wait(lock, [this] { return m_slot == Slot::Done; });
    const bool ran = m_ran;
    m_fn           = nullptr;
    m_user         = nullptr;
    m_slot         = Slot::Empty;
    m_changed.notify_all(); // a caller waiting for the slot, or a refusal after close
    return ran;
}

bool FrameThreadCalls::pump()
{
    CallFn fn   = nullptr;
    void*  user = nullptr;
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        if (m_slot != Slot::Posted)
        {
            return false;
        }
        m_slot = Slot::Running;
        fn     = m_fn;
        user   = m_user;
    }
    fn(user); // outside the lock: another caller may wait for the slot meanwhile
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_ran  = true;
        m_slot = Slot::Done;
        ++m_calls_run;
    }
    m_changed.notify_all();
    return true;
}

void FrameThreadCalls::close()
{
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_closed = true;
        if (m_slot == Slot::Posted)
        {
            m_ran  = false;
            m_slot = Slot::Done;
            ++m_refused;
        }
    }
    m_changed.notify_all();
}

bool FrameThreadCalls::posted() const
{
    const std::lock_guard<std::mutex> lock(m_mutex);
    return m_slot == Slot::Posted;
}

bool FrameThreadCalls::closed() const
{
    const std::lock_guard<std::mutex> lock(m_mutex);
    return m_closed;
}

crd::u64 FrameThreadCalls::calls_run() const
{
    const std::lock_guard<std::mutex> lock(m_mutex);
    return m_calls_run;
}

crd::u64 FrameThreadCalls::refused() const
{
    const std::lock_guard<std::mutex> lock(m_mutex);
    return m_refused;
}

namespace
{
// One device run, as the request thread hands it over.
struct DeviceCall
{
    const ceir::cook::DeviceExecutor*                     inner  = nullptr;
    ceir::Context*                                        ctx    = nullptr;
    const ceir::Module*                                   module = nullptr;
    containers::ConstSpan<ceir::cook::DeviceKernelSource> kernels;
    containers::Span<ceir::cook::DeviceBufferView>        buffers;
    ceir::cook::DeviceRunOutcome*                         out    = nullptr;
    containers::String*                                   reason = nullptr;
    bool                                                  ran    = false;
};

void run_device_call(void* user)
{
    auto* const c = static_cast<DeviceCall*>(user);
    c->ran = c->inner->run(*c->ctx, *c->module, c->kernels, c->buffers, *c->out, *c->reason, c->inner->user);
}
} // namespace

FrameThreadDevice::FrameThreadDevice(FrameThreadCalls& calls, const ceir::cook::DeviceExecutor& inner)
    : m_calls(&calls), m_inner(inner), m_executor(inner)
{
    m_executor.run  = &FrameThreadDevice::run;
    m_executor.user = this;
}

bool FrameThreadDevice::run(ceir::Context& ctx, const ceir::Module& module,
                            containers::ConstSpan<ceir::cook::DeviceKernelSource> kernels,
                            containers::Span<ceir::cook::DeviceBufferView> buffers, ceir::cook::DeviceRunOutcome& out,
                            containers::String& reason, void* user)
{
    auto* const self = static_cast<FrameThreadDevice*>(user);
    DeviceCall  c;
    c.inner   = &self->m_inner;
    c.ctx     = &ctx;
    c.module  = &module;
    c.kernels = kernels;
    c.buffers = buffers;
    c.out     = &out;
    c.reason  = &reason;
    if (!self->m_calls->call(&run_device_call, &c))
    {
        reason.append("the frame loop has ended: the device run did not start");
        return false;
    }
    return c.ran;
}
} // namespace crd::sandbox
