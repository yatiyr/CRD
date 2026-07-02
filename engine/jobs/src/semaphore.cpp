#include "semaphore.hpp"

#include <crd/core/platform.hpp>
#include <crd/core/types.hpp>

#if CRD_OS_WINDOWS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif CRD_OS_LINUX
#include <climits>
#include <ctime>
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>
#else
#error "crd::jobs::detail::Semaphore: unsupported platform (crd-jobs is Windows + Linux, like its fiber asm)"
#endif

namespace crd::jobs::detail
{

// The futex / WaitOnAddress word is the atomic count itself. Both kernels
// atomically re-check the word against the expected value (0) under their
// internal lock before queueing the sleeper, which is what closes the
// pre-sleep release window (see the class comment in semaphore.hpp).

#if CRD_OS_WINDOWS

void Semaphore::wait_on_zero() noexcept
{
    crd::u32 expected = 0U;
    (void)::WaitOnAddress(&m_count, &expected, sizeof(crd::u32), INFINITE);
}

void Semaphore::timed_wait_on_zero(crd::u32 ms) noexcept
{
    crd::u32 expected = 0U;
    (void)::WaitOnAddress(&m_count, &expected, sizeof(crd::u32), ms);
}

void Semaphore::wake_one() noexcept
{
    ::WakeByAddressSingle(&m_count);
}

void Semaphore::wake_all() noexcept
{
    ::WakeByAddressAll(&m_count);
}

#elif CRD_OS_LINUX

namespace
{
long futex_op(std::atomic<crd::u32>* word, int op, crd::u32 val, const timespec* timeout) noexcept
{
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg) — SYS_futex is the raw kernel interface
    return ::syscall(SYS_futex, reinterpret_cast<crd::u32*>(word), op, val, timeout, nullptr, 0);
}
} // namespace

void Semaphore::wait_on_zero() noexcept
{
    // EAGAIN (word != 0), EINTR and spurious returns are all absorbed by the
    // caller's retry loop.
    (void)futex_op(&m_count, FUTEX_WAIT_PRIVATE, 0U, nullptr);
}

void Semaphore::timed_wait_on_zero(crd::u32 ms) noexcept
{
    timespec ts{};
    ts.tv_sec = static_cast<time_t>(ms / 1000U);
    ts.tv_nsec = static_cast<long>(ms % 1000U) * 1000000L;
    (void)futex_op(&m_count, FUTEX_WAIT_PRIVATE, 0U, &ts);
}

void Semaphore::wake_one() noexcept
{
    (void)futex_op(&m_count, FUTEX_WAKE_PRIVATE, 1U, nullptr);
}

void Semaphore::wake_all() noexcept
{
    (void)futex_op(&m_count, FUTEX_WAKE_PRIVATE, static_cast<crd::u32>(INT_MAX), nullptr);
}

#endif

} // namespace crd::jobs::detail
