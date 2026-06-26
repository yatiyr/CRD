// crd-jobs — CPU topology detection + the worker-dispatch policy resolver (ADR-0094).
//
// performance_core_count(): the number of PHYSICAL performance cores. On Intel-hybrid parts these are the cores at
// the top EfficiencyClass (Windows) / top cpufreq tier (Linux). Bandwidth-bound elementwise batches want to limit
// concurrency to this count so they don't oversubscribe the E-cores/HT. Conservative by construction: if the
// topology can't be read (non-hybrid, or a sandbox like WSL that hides /sys), it returns 0 and the policy degrades
// to Default (one job per worker) — it NEVER assumes a hybrid layout.

#include <crd/jobs/jobs.hpp>

#include <crd/core/platform.hpp>
#include <crd/core/types.hpp>

#include <cstdlib> // std::getenv, std::atoi

#if CRD_OS_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#elif CRD_OS_LINUX
#include <cstdio>
#endif

namespace crd::jobs
{
namespace
{
crd::u32 detect_performance_cores() noexcept
{
#if CRD_OS_WINDOWS
    DWORD len = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &len);
    if (len == 0)
    {
        return 0U;
    }
    constexpr DWORD cap = 1U << 14U; // 16 KB stack buffer — holds ~hundreds of core records
    if (len > cap)
    {
        len = cap; // best-effort: count what fits (only matters on >300-core hosts)
    }
    alignas(16) unsigned char buf[cap];
    if (GetLogicalProcessorInformationEx(RelationProcessorCore,
                                         reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buf), &len) == 0)
    {
        return 0U;
    }
    BYTE max_eff = 0;
    for (DWORD off = 0; off < len;)
    {
        auto* const p = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buf + off);
        if (p->Relationship == RelationProcessorCore && p->Processor.EfficiencyClass > max_eff)
        {
            max_eff = p->Processor.EfficiencyClass;
        }
        off += p->Size;
    }
    crd::u32 count = 0;
    for (DWORD off = 0; off < len;)
    {
        auto* const p = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buf + off);
        if (p->Relationship == RelationProcessorCore && p->Processor.EfficiencyClass == max_eff)
        {
            ++count; // one record per physical core ⇒ physical P-core count
        }
        off += p->Size;
    }
    return count;
#elif CRD_OS_LINUX
    // Find the top cpufreq tier, then count UNIQUE physical cores (dedup SMT siblings by core_id) at that tier.
    long max_freq = 0;
    bool any = false;
    for (int cpu = 0; cpu < 512; ++cpu)
    {
        char path[160];
        std::snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", cpu);
        std::FILE* const f = std::fopen(path, "re");
        if (f == nullptr)
        {
            if (cpu == 0)
            {
                return 0U; // /sys/cpufreq unavailable (e.g. WSL) → unknown
            }
            break;
        }
        long v = 0;
        if (std::fscanf(f, "%ld", &v) == 1)
        {
            any = true;
            if (v > max_freq)
            {
                max_freq = v;
            }
        }
        std::fclose(f);
    }
    if (!any || max_freq == 0)
    {
        return 0U;
    }
    bool seen[512] = {false};
    crd::u32 count = 0;
    for (int cpu = 0; cpu < 512; ++cpu)
    {
        char path[160];
        std::snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", cpu);
        std::FILE* f = std::fopen(path, "re");
        if (f == nullptr)
        {
            break;
        }
        long v = 0;
        if (std::fscanf(f, "%ld", &v) != 1)
        {
            v = 0;
        }
        std::fclose(f);
        if (v != max_freq)
        {
            continue; // not a top-tier (P) core
        }
        std::snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/topology/core_id", cpu);
        int core_id = -1;
        f = std::fopen(path, "re");
        if (f != nullptr)
        {
            if (std::fscanf(f, "%d", &core_id) != 1)
            {
                core_id = -1;
            }
            std::fclose(f);
        }
        if (core_id >= 0 && core_id < 512)
        {
            if (!seen[core_id])
            {
                seen[core_id] = true;
                ++count;
            }
        }
        else
        {
            ++count; // no topology info → count the logical CPU
        }
    }
    return count;
#else
    return 0U;
#endif
}

// Fill out[0..return) with the logical-CPU indices of the performance cores (for affinity). Mirrors
// detect_performance_cores but emits indices, not a count. Returns 0 on unknown topology.
crd::u32 detect_performance_cpu_ids(crd::u32* out, crd::u32 max) noexcept
{
    if (out == nullptr || max == 0U)
    {
        return 0U;
    }
#if CRD_OS_WINDOWS
    DWORD len = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &len);
    if (len == 0)
    {
        return 0U;
    }
    constexpr DWORD cap = 1U << 14U;
    if (len > cap)
    {
        len = cap;
    }
    alignas(16) unsigned char buf[cap];
    if (GetLogicalProcessorInformationEx(RelationProcessorCore,
                                         reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buf), &len) == 0)
    {
        return 0U;
    }
    BYTE max_eff = 0;
    for (DWORD off = 0; off < len;)
    {
        auto* const p = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buf + off);
        if (p->Relationship == RelationProcessorCore && p->Processor.EfficiencyClass > max_eff)
        {
            max_eff = p->Processor.EfficiencyClass;
        }
        off += p->Size;
    }
    crd::u32 n = 0;
    for (DWORD off = 0; off < len && n < max;)
    {
        auto* const p = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buf + off);
        if (p->Relationship == RelationProcessorCore && p->Processor.EfficiencyClass == max_eff)
        {
            const KAFFINITY mask = p->Processor.GroupMask[0].Mask; // group 0 (≤64 logical CPUs)
            for (crd::u32 b = 0; b < 64U && n < max; ++b)
            {
                if ((mask >> b) & KAFFINITY{1})
                {
                    out[n++] = b;
                }
            }
        }
        off += p->Size;
    }
    return n;
#elif CRD_OS_LINUX
    long max_freq = 0;
    bool any = false;
    for (int cpu = 0; cpu < 512; ++cpu)
    {
        char path[160];
        std::snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", cpu);
        std::FILE* const f = std::fopen(path, "re");
        if (f == nullptr)
        {
            if (cpu == 0)
            {
                return 0U;
            }
            break;
        }
        long v = 0;
        if (std::fscanf(f, "%ld", &v) == 1)
        {
            any = true;
            if (v > max_freq)
            {
                max_freq = v;
            }
        }
        std::fclose(f);
    }
    if (!any || max_freq == 0)
    {
        return 0U;
    }
    crd::u32 n = 0;
    for (int cpu = 0; cpu < 512 && n < max; ++cpu)
    {
        char path[160];
        std::snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", cpu);
        std::FILE* const f = std::fopen(path, "re");
        if (f == nullptr)
        {
            break;
        }
        long v = 0;
        if (std::fscanf(f, "%ld", &v) != 1)
        {
            v = 0;
        }
        std::fclose(f);
        if (v == max_freq)
        {
            out[n++] = static_cast<crd::u32>(cpu); // the logical-CPU index
        }
    }
    return n;
#else
    (void)out;
    (void)max;
    return 0U;
#endif
}

// getenv wrapper — the override is a single dev/config knob read once; suppress MSVC's C4996 at this one site
// (mirrors crd-hesap-direct's mf_getenv).
[[nodiscard]] const char* policy_getenv(const char* name) noexcept
{
#if CRD_OS_WINDOWS
#pragma warning(push)
#pragma warning(disable : 4996)
#endif
    return std::getenv(name); // NOLINT(concurrency-mt-unsafe,cert-env33-c) — read once into a static
#if CRD_OS_WINDOWS
#pragma warning(pop)
#endif
}

// Cached MemoryBoundElementwise target.
//
// IMPORTANT (measured + root-caused 2026-06-22): merely dispatching FEWER jobs than workers does NOT realise the
// memory-bound win and in fact REGRESSES — with no CPU affinity the OS lands the few job-threads on E-cores, which
// is slower than spreading every core (where the P-cores carry the bandwidth). The clean win needs worker→P-core
// AFFINITY (pin the worker threads + route memory-bound jobs to the P-core subset via the scheduler's pinned lane)
// — a follow-up jobs feature; performance_core_count() is its prerequisite, shipped here. Until affinity lands,
// auto-reduction is OFF (returns 0 ⇒ caller uses num_workers, no regression). The env knob CRD_JOBS_MEMBOUND_WORKERS
// forces a count for experiments / for an app that has externally pinned its pool.
crd::u32 membound_target() noexcept
{
    static const crd::u32 kCached = []() noexcept -> crd::u32
    {
        const char* const env = policy_getenv("CRD_JOBS_MEMBOUND_WORKERS");
        if (env != nullptr)
        {
            const int v = std::atoi(env); // NOLINT(cert-err34-c) — best-effort config value
            if (v > 0)
            {
                return static_cast<crd::u32>(v);
            }
        }
        return 0U; // no auto-reduction until worker affinity exists (see note above)
    }();
    return kCached;
}
} // namespace

crd::u32 performance_core_count() noexcept
{
    static const crd::u32 kCached = detect_performance_cores();
    return kCached;
}

crd::u32 performance_core_cpu_ids(crd::u32* out_ids, crd::u32 max_ids) noexcept
{
    return detect_performance_cpu_ids(out_ids, max_ids);
}

// Worker count for a P-core-routed (Config::pcore_routing) batch: env override > performance_core_count() >
// num_workers(), clamped to the pool. Unlike recommended_jobs(MemoryBound) — which stays a no-op on the default
// path — this assumes the pool's workers are affinity-pinned, so reducing the count IS the win.
crd::u32 pcore_worker_count() noexcept
{
    const crd::u32 nw = num_workers();
    crd::u32 k = membound_target();      // env override (CRD_JOBS_MEMBOUND_WORKERS), else 0
    if (k == 0U)
    {
        k = performance_core_count();    // real topology (Windows / native Linux)
    }
    if (k == 0U)
    {
        k = nw;                          // unknown topology ⇒ use the whole pool
    }
    const crd::u32 m = k < nw ? k : nw;
    return m == 0U ? 1U : m;
}

crd::u32 recommended_jobs(WorkerPreference pref, crd::u32 count) noexcept
{
    const crd::u32 nw = num_workers();
    crd::u32 cap = nw;
    if (pref == WorkerPreference::MemoryBoundElementwise)
    {
        const crd::u32 mb = membound_target();
        cap = (mb == 0U) ? nw : mb; // unknown topology ⇒ safe default (no oversubscription beyond the pool)
    }
    if (cap > nw)
    {
        cap = nw;
    }
    if (count > 0U && cap > count)
    {
        cap = count;
    }
    return cap == 0U ? 1U : cap;
}

} // namespace crd::jobs
