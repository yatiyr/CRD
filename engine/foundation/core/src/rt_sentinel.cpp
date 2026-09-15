#include <crd/core/rt_sentinel.hpp>

#include <atomic>

namespace crd
{
namespace
{
// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables)
std::atomic<RtViolationHandler> g_rt_handler{nullptr};
std::atomic<void*>              g_rt_handler_user{nullptr};
#if CRD_ENABLE_ASSERTS
thread_local crd::u32 tl_rt_depth     = 0U;
thread_local bool     tl_rt_reporting = false; // re-entrancy guard: a reporting handler must not recurse
#endif
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables)
} // namespace

void set_rt_violation_handler(RtViolationHandler handler, void* user) noexcept
{
    g_rt_handler_user.store(user, std::memory_order_relaxed);
    g_rt_handler.store(handler, std::memory_order_release);
}

#if CRD_ENABLE_ASSERTS

bool in_rt_scope() noexcept { return tl_rt_depth != 0U; }

void report_rt_violation(RtViolationKind kind, crd::usize bytes) noexcept
{
    if (tl_rt_reporting)
        return; // the handler itself allocated/blocked -- report once, do not recurse

    RtViolationHandler handler = g_rt_handler.load(std::memory_order_acquire);
    if (handler == nullptr)
        return; // no default action: a sentinel reports, it never changes behaviour

    void* const         user = g_rt_handler_user.load(std::memory_order_relaxed);
    const RtViolation   v{kind, bytes};
    tl_rt_reporting = true;
    handler(v, user);
    tl_rt_reporting = false;
}

RtScope::RtScope() noexcept { ++tl_rt_depth; }

RtScope::~RtScope() noexcept
{
    if (tl_rt_depth != 0U)
        --tl_rt_depth; // guard underflow (a mismatched teardown must not wrap to a huge depth)
}

#endif // CRD_ENABLE_ASSERTS

} // namespace crd
