// crd-diag-overhead -- the four workloads (DIAG.11b). See overhead.hpp for the modes.
//
// Each is a production path an acceptance workload of ADR-0133 ID-5 names, driven the way an application drives it:
//
//   alloc    allocation-heavy: 4096 allocate/free operations per frame on a TLSF heap over a static arena (sizes
//            16..2048 bytes from a fixed generator, 64 blocks live at a time), handed to its owner through the host's
//            allocator watch in basic and full. One site every 256 operations.
//   jobs     tiny jobs: 2048 jobs per frame on the crd-jobs pool, each summing 512 squares (the scheduler
//            workload), the pool configured by the host (its watchdog window) and observed by the profiler's jobs
//            adapter in full. One site per job, recorded from whichever worker ran it.
//   offline  an offline CEIR pipeline per frame: the authored program text is cooked (parse, verify, serialize),
//            loaded back from the blob, compiled and run (the step loop of ceridc's run path); in full the compile and
//            run go through the profiling bridge (crd/ceir/host/plan_perf.hpp). One site per program run.
//   events   the site itself: 4096 sites per frame on one thread, the per-event cost of each mode.
//
// Every workload knows its answer: an iteration that computes anything else fails the run.

#include "overhead.hpp"

#include <crd/ceir/ceir.hpp>
#include <crd/ceir/cook/program_cook.hpp>
#include <crd/ceir/func.hpp>
#include <crd/ceir/gen/arith_ops.hpp>
#include <crd/ceir/gen/core_ops.hpp>
#include <crd/ceir/host/plan_perf.hpp>
#include <crd/ceir/plan.hpp>
#include <crd/containers/span.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/memory/allocators/growable_tlsf_allocator.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>
#include <crd/perf/config.hpp>

#if CRD_PERF_ENABLED
#include <crd/perf/jobs_adapter.hpp>
#endif

#include <cstdio>
#include <optional>
#include <span>

namespace diag_overhead
{

namespace
{

using crd::u32;
using crd::u64;
using crd::usize;

// Build a site's message into `buffer`; called only when the recorder admits the event.
cont::StringView payload(Sites& sites, char* buffer, usize capacity, const char* what, u64 a, u64 b)
{
    sites.payloads.fetch_add(1U, std::memory_order_relaxed);
    const int written = std::snprintf(buffer, capacity, "%s %llu of %llu", what, static_cast<unsigned long long>(a),
                                      static_cast<unsigned long long>(b));
    const usize length = written < 0 ? 0U : static_cast<usize>(written);
    return cont::StringView{buffer, length < capacity ? length : capacity - 1U};
}

constexpr usize kPayloadBytes = 96U;

// ---- alloc ------------------------------------------------------------------------------------------------------

constexpr u32   kAllocOps   = 4096U;
constexpr u32   kAllocSlots = 64U;
constexpr u32   kAllocBatch = 256U;
constexpr usize kAllocArena = usize{8} << 20U;
constexpr u64   kAllocSeed  = 0x9E3779B97F4A7C15ULL;

alignas(64) crd::u8 g_alloc_arena[kAllocArena]; // NOLINT(modernize-avoid-c-arrays): static, so no allocator is hidden
                                                // underneath the one being measured

u64 next_size(u64& state) noexcept
{
    state = (state * 6364136223846793005ULL) + 1442695040888963407ULL;
    return 16U + ((state >> 33U) % 2033U); // 16 .. 2048 bytes
}

class AllocWorkload final : public Workload
{
public:
    [[nodiscard]] const char* name() const noexcept override { return "alloc"; }

    bool setup(const WorkloadSetup& setup, cont::String& reason) override
    {
        m_sites = setup.sites;
        m_tlsf.emplace(static_cast<void*>(g_alloc_arena), kAllocArena, "diag-overhead");
        m_alloc = &*m_tlsf;
        if (setup.host != nullptr)
        {
            m_alloc = setup.host->watch_allocator(&*m_tlsf, "diag-overhead");
            if (m_alloc == nullptr)
            {
                reason.append("the host refused to watch the allocator");
                return false;
            }
        }
        u64 state = kAllocSeed;
        m_expected = 0U;
        for (u32 op = 0U; op < kAllocOps; ++op)
        {
            m_expected += next_size(state);
        }
        return true;
    }

    bool iterate(u64& signature) override
    {
        void* live[kAllocSlots] = {}; // NOLINT(modernize-avoid-c-arrays)
        u64   live_bytes        = 0U;
        u64   sizes[kAllocSlots] = {}; // NOLINT(modernize-avoid-c-arrays)
        u64   state             = kAllocSeed;
        u64   sum               = 0U;
        bool  ok                = true;
        char  buffer[kPayloadBytes];
        for (u32 op = 0U; op < kAllocOps; ++op)
        {
            const u32 slot = op % kAllocSlots;
            if (live[slot] != nullptr)
            {
                m_alloc->deallocate(live[slot]);
                live_bytes -= sizes[slot];
                live[slot] = nullptr;
            }
            const u64 size = next_size(state);
            void*     p    = m_alloc->try_allocate(static_cast<usize>(size), 16U);
            if (p == nullptr)
            {
                ok = false;
                break;
            }
            static_cast<crd::u8*>(p)[0] = static_cast<crd::u8>(op);
            live[slot]                  = p;
            sizes[slot]                 = size;
            live_bytes += size;
            sum += size;
            if (((op + 1U) % kAllocBatch) == 0U)
            {
                CRD_FLIGHT_RECORD(m_sites->recorder, kCodeAllocBatch, cp::Severity::Info,
                                  payload(*m_sites, buffer, kPayloadBytes, "alloc batch", op + 1U, live_bytes), op + 1U,
                                  live_bytes);
            }
        }
        for (void* p : live)
        {
            if (p != nullptr)
            {
                m_alloc->deallocate(p);
            }
        }
        signature = sum;
        return ok;
    }

    void teardown() override
    {
        m_alloc = nullptr;
        m_tlsf.reset();
    }

    [[nodiscard]] u64 sites_per_iteration() const noexcept override { return kAllocOps / kAllocBatch; }
    [[nodiscard]] u64 expected() const noexcept override { return m_expected; }
    [[nodiscard]] const crd::memory::IAllocator* measured_allocator() const noexcept override
    {
        return m_tlsf.has_value() ? &*m_tlsf : nullptr;
    }

private:
    Sites*                                    m_sites = nullptr;
    std::optional<crd::memory::TlsfAllocator> m_tlsf;
    crd::memory::IAllocator*                  m_alloc    = nullptr;
    u64                                       m_expected = 0U;
};

// ---- jobs -------------------------------------------------------------------------------------------------------

constexpr u32 kJobs     = 2048U; // half the default injection queue: one frame's batch always fits
constexpr u32 kJobItems = 512U;

struct Slice
{
    u64    begin = 0U;
    u64    sum   = 0U;
    u32    index = 0U;
    Sites* sites = nullptr;
};

Slice              g_slices[kJobs]; // NOLINT(modernize-avoid-c-arrays)
crd::jobs::JobDecl g_decls[kJobs];  // NOLINT(modernize-avoid-c-arrays)

// Install or remove the profiler's jobs adapter; false where profiling is compiled out (nothing to install).
bool set_jobs_adapter(bool installed)
{
#if CRD_PERF_ENABLED
    if (installed)
    {
        crd::perf::install_jobs_adapter();
    }
    else
    {
        crd::perf::uninstall_jobs_adapter();
    }
    return true;
#else
    (void)installed;
    return false;
#endif
}

void sum_slice(void* data)
{
    auto* const slice = static_cast<Slice*>(data);
    u64         sum   = 0U;
    for (u64 i = slice->begin; i < slice->begin + kJobItems; ++i)
    {
        sum += i * i;
    }
    slice->sum = sum;
    char buffer[kPayloadBytes];
    CRD_FLIGHT_RECORD(slice->sites->recorder, kCodeJobDone, cp::Severity::Info,
                      payload(*slice->sites, buffer, kPayloadBytes, "job", slice->index, sum), slice->index, sum);
}

class JobsWorkload final : public Workload
{
public:
    [[nodiscard]] const char* name() const noexcept override { return "jobs"; }

    bool setup(const WorkloadSetup& setup, cont::String& reason) override
    {
        (void)reason;
        crd::jobs::Config config;
        config.num_threads = setup.threads;
        if (setup.host != nullptr)
        {
            setup.host->configure_jobs(config); // the profile's watchdog window, before the pool exists
        }
        m_adapter = setup.mode == Mode::Full && set_jobs_adapter(true); // the profiler observes every job
        crd::jobs::init(config);
        for (u32 i = 0U; i < kJobs; ++i)
        {
            g_slices[i].begin = static_cast<u64>(i) * kJobItems;
            g_slices[i].index = i;
            g_slices[i].sites = setup.sites;
            g_decls[i].fn     = &sum_slice;
            g_decls[i].data   = &g_slices[i];
        }
        return true;
    }

    bool iterate(u64& signature) override
    {
        crd::jobs::run_and_wait(std::span<const crd::jobs::JobDecl>{g_decls, kJobs});
        u64 total = 0U;
        for (const Slice& slice : g_slices)
        {
            total += slice.sum;
        }
        signature = total;
        return true;
    }

    void teardown() override
    {
        crd::jobs::shutdown(); // joins the workers and the watchdog before the host they report into stops
        if (m_adapter)
        {
            (void)set_jobs_adapter(false);
            m_adapter = false;
        }
    }

    [[nodiscard]] u64 sites_per_iteration() const noexcept override { return kJobs; }

    // The calling thread is worker 0; the pool starts the others. The host's window adds the watchdog thread.
    [[nodiscard]] u64 own_threads(u32 threads) const noexcept override { return threads - 1U; }
    [[nodiscard]] u64 diagnostic_threads(Mode mode) const noexcept override { return mode == Mode::Off ? 0U : 1U; }

    [[nodiscard]] u64 expected() const noexcept override
    {
        const u64 n = static_cast<u64>(kJobs) * kJobItems; // the sum of i * i for i in [0, n)
        return ((n - 1U) * n * ((2U * n) - 1U)) / 6U;
    }

private:
    bool m_adapter = false;
};

// ---- offline ----------------------------------------------------------------------------------------------------

constexpr crd::i64 kProgramSteps   = 20000; // main(n) runs its step loop n times and returns n + 1
constexpr u64      kProgramAssetId = 0x00A20011U;
constexpr usize    kProgramChunk   = usize{4} << 20U;
constexpr usize    kProgramMaxText = usize{64} << 10U;

void register_dialects(crd::ceir::Context& ctx)
{
    (void)crd::ceir::arith::register_arith_ops(ctx);
    (void)crd::ceir::core::register_core_ops(ctx);
    (void)crd::ceir::func::register_dialect(ctx);
}

class OfflineWorkload final : public Workload
{
public:
    [[nodiscard]] const char* name() const noexcept override { return "offline"; }

    bool setup(const WorkloadSetup& setup, cont::String& reason) override
    {
        m_sites = setup.sites;
        m_full  = setup.mode == Mode::Full;
        if (setup.program == nullptr)
        {
            reason.append("no program (--program)");
            return false;
        }
        m_path.clear();
        m_path.append(setup.program);
        std::FILE* file = open_for_reading(setup.program);
        if (file == nullptr)
        {
            reason.append("the program could not be opened: ");
            reason.append(setup.program);
            return false;
        }
        char  chunk[4096];
        usize read = 0U;
        m_text.clear();
        while ((read = std::fread(chunk, 1U, sizeof(chunk), file)) > 0U)
        {
            m_text.append(cont::StringView{chunk, read});
            if (m_text.size() > kProgramMaxText)
            {
                break;
            }
        }
        (void)std::fclose(file);
        if (m_text.empty() || m_text.size() > kProgramMaxText)
        {
            reason.append("the program is empty or larger than 64 KiB");
            return false;
        }
        return true;
    }

    bool iterate(u64& signature) override
    {
        crd::memory::GrowableTlsfAllocator arena(kProgramChunk, nullptr, "diag-overhead-offline");
        crd::ceir::Context                 cook_ctx(&arena);
        register_dialects(cook_ctx);
        const crd::ceir::cook::CookResult cooked = crd::ceir::cook::cook_program_text(
            cook_ctx, cont::StringView{m_text.c_str(), m_text.size()}, cont::StringView{m_path.c_str(), m_path.size()},
            kProgramAssetId, &arena, &arena);
        if (!cooked.ok())
        {
            return false;
        }
        crd::ceir::Context run_ctx(&arena);
        register_dialects(run_ctx);
        const crd::ceir::cook::ReadResult loaded = crd::ceir::cook::read_program(
            run_ctx, cont::ConstSpan<crd::u8>{cooked.blob.data(), cooked.blob.size()}, &arena);
        if (!loaded.ok() || loaded.module == nullptr)
        {
            return false;
        }
        const crd::i64 args[1] = {kProgramSteps}; // NOLINT(modernize-avoid-c-arrays)
        const cont::ConstSpan<crd::i64> arguments{args, 1U};
        crd::i64                        result = -1;
        if (m_full)
        {
            const crd::ceir::plan::CompileResult compiled =
                crd::ceir::host::profiled_compile(run_ctx, *loaded.module, "main", &arena);
            if (!compiled.ok())
            {
                return false;
            }
            crd::ceir::host::PlanProfile         profile;
            const crd::ceir::plan::RunResult ran =
                crd::ceir::host::profiled_run(compiled.plan, arguments, &arena, profile);
            if (!ran.ok() || ran.values.size() != 1U)
            {
                return false;
            }
            result = ran.values[0];
        }
        else
        {
            const crd::ceir::plan::CompileResult compiled =
                crd::ceir::plan::compile(run_ctx, *loaded.module, "main", &arena);
            if (!compiled.ok())
            {
                return false;
            }
            const crd::ceir::plan::RunResult ran = crd::ceir::plan::run(compiled.plan, arguments, &arena);
            if (!ran.ok() || ran.values.size() != 1U)
            {
                return false;
            }
            result = ran.values[0];
        }
        char buffer[kPayloadBytes];
        CRD_FLIGHT_RECORD(m_sites->recorder, kCodeProgramRan, cp::Severity::Info,
                          payload(*m_sites, buffer, kPayloadBytes, "program ran", static_cast<u64>(result),
                                  static_cast<u64>(kProgramSteps)),
                          ++m_runs, static_cast<u64>(result));
        signature = static_cast<u64>(result);
        return true;
    }

    void teardown() override {}

    [[nodiscard]] u64 sites_per_iteration() const noexcept override { return 1U; }
    [[nodiscard]] u64 expected() const noexcept override { return static_cast<u64>(kProgramSteps) + 1U; }

private:
    Sites*       m_sites = nullptr;
    bool         m_full  = false;
    u64          m_runs  = 0U;
    cont::String m_path;
    cont::String m_text;
};

// ---- events -----------------------------------------------------------------------------------------------------

constexpr u32 kEvents = 4096U;

class EventsWorkload final : public Workload
{
public:
    [[nodiscard]] const char* name() const noexcept override { return "events"; }

    bool setup(const WorkloadSetup& setup, cont::String& reason) override
    {
        (void)reason;
        m_sites = setup.sites;
        return true;
    }

    bool iterate(u64& signature) override
    {
        char buffer[kPayloadBytes];
        u64  sum = 0U;
        ++m_iteration;
        for (u32 i = 0U; i < kEvents; ++i)
        {
            CRD_FLIGHT_RECORD(m_sites->recorder, kCodeEvent, cp::Severity::Info,
                              payload(*m_sites, buffer, kPayloadBytes, "event", i, m_iteration), i, m_iteration);
            sum += i;
        }
        signature = sum;
        return true;
    }

    void teardown() override {}

    [[nodiscard]] u64 sites_per_iteration() const noexcept override { return kEvents; }
    [[nodiscard]] u64 expected() const noexcept override { return (static_cast<u64>(kEvents) * (kEvents - 1U)) / 2U; }

private:
    Sites* m_sites     = nullptr;
    u64    m_iteration = 0U;
};

AllocWorkload   g_alloc;
JobsWorkload    g_jobs;
OfflineWorkload g_offline;
EventsWorkload  g_events;

} // namespace

const char* mode_name(Mode m) noexcept
{
    switch (m)
    {
    case Mode::Off:
        return "off";
    case Mode::Basic:
        return "basic";
    case Mode::Full:
        return "full";
    }
    return "?";
}

std::FILE* open_for_reading(const char* path) noexcept
{
#if defined(_WIN32)
    std::FILE* file = nullptr;
    return fopen_s(&file, path, "rb") == 0 ? file : nullptr;
#else
    return std::fopen(path, "rb");
#endif
}

Workload* find_workload(const char* name) noexcept
{
    Workload* const all[] = {&g_alloc, &g_jobs, &g_offline, &g_events}; // NOLINT(modernize-avoid-c-arrays)
    for (Workload* w : all)
    {
        const char* a = w->name();
        const char* b = name;
        while (*a != '\0' && *a == *b)
        {
            ++a;
            ++b;
        }
        if (*a == '\0' && *b == '\0')
        {
            return w;
        }
    }
    return nullptr;
}

} // namespace diag_overhead
