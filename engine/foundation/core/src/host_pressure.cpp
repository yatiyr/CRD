#include <crd/core/host_pressure.hpp>

#include <crd/core/platform.hpp>

#include <cstdint>

#if CRD_OS_WINDOWS
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
// windows.h first: psapi.h uses its types. PSAPI_VERSION 2 (the default for Windows 7 and later) resolves both calls
// to their kernel32 entries (K32GetProcessMemoryInfo, K32GetPerformanceInfo), so no psapi import library is needed.
#include <psapi.h>
#elif CRD_OS_LINUX
#include <fcntl.h>
#include <sys/sysinfo.h>
#include <unistd.h>
#endif

namespace crd
{
namespace
{
BudgetReading bounded_reading(crd::u64 capacity, crd::u64 in_use) noexcept
{
    BudgetReading r;
    r.read     = true;
    r.bounded  = true;
    r.capacity = capacity;
    r.in_use   = in_use;
    return r;
}

#if CRD_OS_LINUX
// A /proc file read whole into `buffer` (NUL-terminated). False when it cannot be opened, read or does not fit.
bool read_proc_file(const char* path, char* buffer, crd::usize capacity) noexcept
{
    const int fd = ::open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
    {
        return false;
    }
    crd::usize length = 0U;
    bool       ok     = true;
    while (true)
    {
        if (length + 1U >= capacity)
        {
            ok = false; // longer than the buffer: never judge a truncated file
            break;
        }
        const ssize_t got = ::read(fd, buffer + length, capacity - 1U - length);
        if (got < 0)
        {
            ok = false;
            break;
        }
        if (got == 0)
        {
            break;
        }
        length += static_cast<crd::usize>(got);
    }
    (void)::close(fd);
    buffer[length] = '\0';
    return ok;
}

// The value of the line "<key>:   <digits> kB" (or without a unit) in `text`, in bytes when `kilobytes`.
bool field_value(const char* text, const char* key, bool kilobytes, crd::u64& out) noexcept
{
    const char* line = text;
    while (*line != '\0')
    {
        crd::usize k = 0U;
        while (key[k] != '\0' && line[k] == key[k])
        {
            ++k;
        }
        if (key[k] == '\0' && line[k] == ':')
        {
            const char* p = line + k + 1;
            while (*p == ' ' || *p == '\t')
            {
                ++p;
            }
            if (*p < '0' || *p > '9')
            {
                return false;
            }
            crd::u64 value = 0U;
            while (*p >= '0' && *p <= '9')
            {
                const auto digit = static_cast<crd::u64>(*p - '0');
                if (value > (UINT64_MAX - digit) / 10U)
                {
                    return false;
                }
                value = value * 10U + digit;
                ++p;
            }
            if (kilobytes)
            {
                if (value > UINT64_MAX / 1024U)
                {
                    return false;
                }
                value *= 1024U;
            }
            out = value;
            return true;
        }
        while (*line != '\0' && *line != '\n')
        {
            ++line;
        }
        if (*line == '\n')
        {
            ++line;
        }
    }
    return false;
}
#endif
} // namespace

HostMemoryReading read_host_memory() noexcept
{
    HostMemoryReading out;
#if CRD_OS_WINDOWS
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    if (GlobalMemoryStatusEx(&status) != FALSE && status.ullAvailPhys <= status.ullTotalPhys
        && status.ullAvailPageFile <= status.ullTotalPageFile)
    {
        out.physical = bounded_reading(status.ullTotalPhys, status.ullTotalPhys - status.ullAvailPhys);
        out.commit   = bounded_reading(status.ullTotalPageFile, status.ullTotalPageFile - status.ullAvailPageFile);
        out.commit_enforced = true;
        PERFORMANCE_INFORMATION performance{};
        performance.cb = sizeof(performance);
        if (GetPerformanceInfo(&performance, sizeof(performance)) != FALSE)
        {
            const crd::u64 peak = static_cast<crd::u64>(performance.CommitPeak) * performance.PageSize;
            // The peak is the system's since boot; one taken a moment before the charge read above may trail it.
            out.commit.high_water         = peak > out.commit.in_use ? peak : out.commit.in_use;
            out.commit.high_water_tracked = true;
        }
    }
    else
    {
        out.reason = "GlobalMemoryStatusEx failed";
    }
    PROCESS_MEMORY_COUNTERS counters{};
    counters.cb = sizeof(counters);
    if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)) != FALSE)
    {
        out.resident.read               = true;
        out.resident.in_use             = counters.WorkingSetSize;
        out.resident.high_water         = counters.PeakWorkingSetSize;
        out.resident.high_water_tracked = true;
    }
    else if (out.reason[0] == '\0')
    {
        out.reason = "GetProcessMemoryInfo failed";
    }
#elif CRD_OS_LINUX
    char     text[8192];
    crd::u64 total     = 0U;
    crd::u64 available = 0U;
    crd::u64 limit     = 0U;
    crd::u64 committed = 0U;
    if (read_proc_file("/proc/meminfo", text, sizeof(text)) && field_value(text, "MemTotal", true, total)
        && field_value(text, "MemAvailable", true, available) && available <= total)
    {
        out.physical = bounded_reading(total, total - available);
        if (field_value(text, "CommitLimit", true, limit) && field_value(text, "Committed_AS", true, committed))
        {
            char mode[32];
            out.commit_enforced =
                read_proc_file("/proc/sys/vm/overcommit_memory", mode, sizeof(mode)) && mode[0] == '2';
            out.commit.read     = true;
            out.commit.in_use   = committed;
            // Outside strict overcommit the kernel lets the charge pass the limit: report it, judge nothing.
            out.commit.bounded  = out.commit_enforced;
            out.commit.capacity = out.commit_enforced ? limit : 0U;
        }
        else
        {
            out.reason = "/proc/meminfo has no CommitLimit or Committed_AS";
        }
    }
    else
    {
        out.reason = "cannot read MemTotal and MemAvailable from /proc/meminfo";
    }
    crd::u64 rss = 0U;
    crd::u64 hwm = 0U;
    if (read_proc_file("/proc/self/status", text, sizeof(text)) && field_value(text, "VmRSS", true, rss)
        && field_value(text, "VmHWM", true, hwm))
    {
        out.resident.read               = true;
        out.resident.in_use             = rss;
        out.resident.high_water         = hwm > rss ? hwm : rss;
        out.resident.high_water_tracked = true;
    }
    else if (out.reason[0] == '\0')
    {
        out.reason = "cannot read VmRSS and VmHWM from /proc/self/status";
    }
#else
    out.reason = "no host memory probe on this OS";
#endif
    return out;
}

crd::u64 host_physical_total_cross_check() noexcept
{
#if CRD_OS_WINDOWS
    PERFORMANCE_INFORMATION performance{};
    performance.cb = sizeof(performance);
    if (GetPerformanceInfo(&performance, sizeof(performance)) == FALSE)
    {
        return 0U;
    }
    return static_cast<crd::u64>(performance.PhysicalTotal) * performance.PageSize;
#elif CRD_OS_LINUX
    struct sysinfo info{};
    if (::sysinfo(&info) != 0)
    {
        return 0U;
    }
    return static_cast<crd::u64>(info.totalram) * info.mem_unit;
#else
    return 0U;
#endif
}

} // namespace crd
