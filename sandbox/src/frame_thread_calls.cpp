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

namespace
{
// One host record or replay, as the request thread hands it over. Exactly one of the two requests is set.
struct HostCall
{
    const ceir::cook::ReplayHostExecutor* inner          = nullptr;
    const ceir::cook::HostRecordRequest*  record_request = nullptr;
    ceir::cook::ReplayRecord*             record_out     = nullptr;
    ceir::cook::OwnedReplaySite*          fault          = nullptr;
    containers::String*                   missing        = nullptr;
    const ceir::cook::HostReplayRequest*  replay_request = nullptr;
    ceir::cook::HostReplayAnswer*         replay_out     = nullptr;
    containers::String*                   reason         = nullptr;
    ceir::cook::HostExecutorStatus        status         = ceir::cook::HostExecutorStatus::Failed;
};

// On the frame-loop thread: the inner call.
void run_host_call(void* user)
{
    auto* const c = static_cast<HostCall*>(user);
    if (c->record_request != nullptr)
    {
        c->status =
            c->inner->record(*c->record_request, *c->record_out, *c->fault, *c->missing, *c->reason, c->inner->user);
    }
    else
    {
        c->status = c->inner->replay(*c->replay_request, *c->replay_out, *c->reason, c->inner->user);
    }
}

[[nodiscard]] ceir::cook::HostExecutorStatus hand_over(FrameThreadCalls& calls, HostCall& c, containers::String& reason)
{
    if (!calls.call(&run_host_call, &c))
    {
        reason.append("the frame loop has ended: the host run did not start");
        return ceir::cook::HostExecutorStatus::Failed;
    }
    return c.status;
}
} // namespace

FrameThreadHost::FrameThreadHost(FrameThreadCalls& calls, const ceir::cook::ReplayHostExecutor& inner)
    : m_calls(&calls), m_inner(inner), m_executor{&FrameThreadHost::record, &FrameThreadHost::replay, this}
{
}

ceir::cook::HostExecutorStatus FrameThreadHost::record(const ceir::cook::HostRecordRequest& request,
                                                       ceir::cook::ReplayRecord& out,
                                                       ceir::cook::OwnedReplaySite& fault, containers::String& missing,
                                                       containers::String& reason, void* user)
{
    auto* const self = static_cast<FrameThreadHost*>(user);
    HostCall    c;
    c.inner          = &self->m_inner;
    c.record_request = &request;
    c.record_out     = &out;
    c.fault          = &fault;
    c.missing        = &missing;
    c.reason         = &reason;
    return hand_over(*self->m_calls, c, reason);
}

ceir::cook::HostExecutorStatus FrameThreadHost::replay(const ceir::cook::HostReplayRequest& request,
                                                       ceir::cook::HostReplayAnswer& out, containers::String& reason,
                                                       void* user)
{
    auto* const self = static_cast<FrameThreadHost*>(user);
    HostCall    c;
    c.inner          = &self->m_inner;
    c.replay_request = &request;
    c.replay_out     = &out;
    c.reason         = &reason;
    return hand_over(*self->m_calls, c, reason);
}
} // namespace crd::sandbox
