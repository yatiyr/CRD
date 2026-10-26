// crd-diag-overhead -- the eight workloads (DIAG.11b). See overhead.hpp for the modes.
//
// Each is a production path an acceptance workload of ADR-0133 ID-5 names, driven the way an application drives it:
//
//   alloc    allocation-heavy: 4096 allocate/free operations per frame on a TLSF heap over a static arena (sizes
//            16..2048 bytes from a fixed generator, 64 blocks live at a time), handed to its owner through the host's
//            allocator watch in basic and full. One site every 256 operations. With --memory the full mode counts
//            the heap's statistics (registered with the profiler before its first allocation, snapshotted at every
//            frame mark) and the tracy mode reports each allocation and free to Tracy (TracyAllocN/TracyFreeN).
//   jobs     tiny jobs: 2048 jobs per frame on the crd-jobs pool, each summing 512 squares (the scheduler
//            workload), the pool configured by the host (its watchdog window) and observed by the profiler's jobs
//            adapter in full. One site per job, recorded from whichever worker ran it.
//   offline  an offline CEIR pipeline per frame: the authored program text is cooked (parse, verify, serialize),
//            loaded back from the blob, compiled and run (the step loop of ceridc's run path); in full the compile and
//            run go through the profiling bridge (crd/ceir/host/plan_perf.hpp). One site per program run.
//   io       mixed I/O: per frame 50 reads (whole files, ranges and AsyncFile completions on a two-thread crd-jobs
//            pool) of files setup() writes into --scratch, every byte hashed. One site per read.
//   render   authored rendering: the scene renderer draws 4096 instances (a quad mesh setup() packs into --scratch,
//            1024 of them behind the camera) through the authored engine://frame/forward_csm graph, its shadow
//            cascades on, into an offscreen target on a headless device (D3D12 on Windows, Vulkan elsewhere), 64
//            instances moved every frame; the renderer, the world and the resources allocate from one heap, watched
//            by the host in basic and full. One site per frame. With --validation the device runs that validation
//            under a message capture, and a mode the device does not run refuses the run.
//   events   the site itself: 4096 sites per frame on one thread, the per-event cost of each mode.
//   zones    the profiler zone itself: 2048 CRD_PERF_SCOPE zones per frame on one thread (Tracy: ZoneScopedN), the
//            per-zone cost; a frame's zones fit the profiler's per-thread ring, which the full mode drains each frame.
//   marks    the frame mark itself: 64 CRD_PERF_FRAME_MARK per frame besides the harness's own (Tracy: FrameMark),
//            the per-mark cost.
//
// In the tracy mode (crd-diag-overhead-tracy) every site, zone and mark is Tracy's counterpart, the jobs pool's jobs
// are zoned through crd-jobs' observer as the jobs adapter's are, and the CEIR plan's ops through its RunHooks as the
// profiling bridge's are (tracy_peer.cpp).
//
// Every workload knows its answer: an iteration that computes anything else fails the run.

#include "overhead.hpp"
#include "quad_scene_fixture.hpp"

#include <crd/ceir/ceir.hpp>
#include <crd/ceir/cook/program_cook.hpp>
#include <crd/ceir/func.hpp>
#include <crd/ceir/gen/arith_ops.hpp>
#include <crd/ceir/gen/core_ops.hpp>
#include <crd/ceir/host/plan_perf.hpp>
#include <crd/ceir/plan.hpp>
#include <crd/containers/span.hpp>
#include <crd/gpu/raster_context.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/math/mat.hpp>
#include <crd/memory/allocators/growable_tlsf_allocator.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>
#include <crd/perf/config.hpp>
#include <crd/perf/memory.hpp>
#include <crd/perf/profiler.hpp>
#include <crd/perf/scope.hpp>
#include <crd/platform/async_file.hpp>
#include <crd/platform/filesystem.hpp>
#include <crd/resources/resource_manager.hpp>
#include <crd/scene/render_components.hpp>
#include <crd/scene/transform.hpp>
#include <crd/scene/world.hpp>
#include <crd/scenerender/scene_renderer.hpp>

#if defined(_WIN32)
#include <crd/gpu/dx12_context.hpp>
#include <crd/gpu/dx12_raster_context.hpp>
#include <crd/gpu/dx12_validation_capture.hpp>
#else
#include <crd/gpu/vulkan_context.hpp>
#include <crd/gpu/vulkan_raster_context.hpp>
#include <crd/gpu/vulkan_shader_compile.hpp>
#include <crd/gpu/vulkan_validation_capture.hpp>
#endif

#if CRD_PERF_ENABLED
#include <crd/perf/jobs_adapter.hpp>
#endif

#include <cstdio>
#include <cstring>
#include <memory>
#include <optional>
#include <span>

namespace diag_overhead
{

namespace
{

using crd::f32;
using crd::u32;
using crd::u64;
using crd::usize;
namespace fs = crd::platform::fs;

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
#if CRD_PERF_ENABLED
        if (setup.memory && setup.mode == Mode::Full)
        {
            m_registered = cp::register_allocator("diag-overhead", &*m_tlsf); // before the heap's first allocation
            if (m_registered == cp::kInvalidAllocatorIdx)
            {
                reason.append("the profiler did not register the heap");
                return false;
            }
        }
#endif
#if CRD_DIAG_OVERHEAD_TRACY
        m_tracy_memory = setup.memory && setup.mode == Mode::Tracy;
#endif
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
#if CRD_DIAG_OVERHEAD_TRACY
        if (m_tracy_memory)
        {
            return frame<true>(signature);
        }
#endif
        return frame<false>(signature);
    }

    void teardown() override
    {
#if CRD_PERF_ENABLED
        if (m_registered != cp::kInvalidAllocatorIdx)
        {
            cp::unregister_allocator(m_registered);
            m_registered = cp::kInvalidAllocatorIdx;
        }
#endif
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
    // One frame; with TracyMemory every allocation and free is reported to Tracy as well, next to the operation (one
    // thread, so the reports keep the operations' order, as Tracy requires).
    template <bool TracyMemory>
    bool frame(u64& signature)
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
#if CRD_DIAG_OVERHEAD_TRACY
                if constexpr (TracyMemory)
                {
                    TracyFreeN(live[slot], kTracyPool);
                }
#endif
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
#if CRD_DIAG_OVERHEAD_TRACY
            if constexpr (TracyMemory)
            {
                TracyAllocN(p, static_cast<usize>(size), kTracyPool);
            }
#endif
            static_cast<crd::u8*>(p)[0] = static_cast<crd::u8>(op);
            live[slot]                  = p;
            sizes[slot]                 = size;
            live_bytes += size;
            sum += size;
            if (((op + 1U) % kAllocBatch) == 0U)
            {
                DOV_SITE(*m_sites, kCodeAllocBatch, cp::Severity::Info,
                         payload(*m_sites, buffer, kPayloadBytes, "alloc batch", op + 1U, live_bytes), op + 1U,
                         live_bytes);
            }
        }
        for (void* p : live)
        {
            if (p != nullptr)
            {
#if CRD_DIAG_OVERHEAD_TRACY
                if constexpr (TracyMemory)
                {
                    TracyFreeN(p, kTracyPool);
                }
#endif
                m_alloc->deallocate(p);
            }
        }
        signature = sum;
        return ok;
    }

#if CRD_DIAG_OVERHEAD_TRACY
    static constexpr const char* kTracyPool = "diag-overhead"; // Tracy names a pool by the pointer
    bool                         m_tracy_memory = false;
#endif
    Sites*                                    m_sites = nullptr;
    std::optional<crd::memory::TlsfAllocator> m_tlsf;
    crd::memory::IAllocator*                  m_alloc      = nullptr;
    u64                                       m_expected   = 0U;
#if CRD_PERF_ENABLED
    crd::u32 m_registered = 0xFFFFFFFFU; // the profiler's slot (--memory, full)
#endif
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
    DOV_SITE(*slice->sites, kCodeJobDone, cp::Severity::Info,
             payload(*slice->sites, buffer, kPayloadBytes, "job", slice->index, sum), slice->index, sum);
}

// The crd-jobs pool the jobs and io workloads run on (the calling thread is worker 0, the pool starts the others),
// configured by the host (its watchdog window, which adds the watchdog thread) and observed by the profiler's jobs
// adapter in full.
class PooledWorkload : public Workload
{
public:
    [[nodiscard]] u64 own_threads(u32 threads) const noexcept override { return pool_threads(threads) - 1U; }
    [[nodiscard]] u64 diagnostic_threads(Mode mode) const noexcept override
    {
        return mode == Mode::Basic || mode == Mode::Full ? 1U : 0U; // the host's jobs watchdog
    }

protected:
    // The pool's threads, the calling thread included, for --threads `requested`.
    [[nodiscard]] virtual u32 pool_threads(u32 requested) const noexcept { return requested; }

    void start_pool(const WorkloadSetup& setup)
    {
        crd::jobs::Config config;
        config.num_threads = pool_threads(setup.threads);
        if (setup.host != nullptr)
        {
            setup.host->configure_jobs(config); // the profile's watchdog window, before the pool exists
        }
        m_adapter = setup.mode == Mode::Full && set_jobs_adapter(true); // the profiler observes every job
#if CRD_DIAG_OVERHEAD_TRACY
        m_tracy_observer = setup.mode == Mode::Tracy; // Tracy zones every job at the same seam
        if (m_tracy_observer)
        {
            tracy_peer::install_jobs_observer();
        }
#endif
        crd::jobs::init(config);
    }

    void stop_pool()
    {
        crd::jobs::shutdown(); // joins the workers and the watchdog before the host they report into stops
        if (m_adapter)
        {
            (void)set_jobs_adapter(false);
            m_adapter = false;
        }
#if CRD_DIAG_OVERHEAD_TRACY
        if (m_tracy_observer)
        {
            tracy_peer::uninstall_jobs_observer();
            m_tracy_observer = false;
        }
#endif
    }

public:
    [[nodiscard]] u64 tracy_zones() const noexcept override
    {
#if CRD_DIAG_OVERHEAD_TRACY
        return tracy_peer::jobs_zoned();
#else
        return 0U;
#endif
    }

private:
    bool m_adapter = false;
#if CRD_DIAG_OVERHEAD_TRACY
    bool m_tracy_observer = false;
#endif
};

class JobsWorkload final : public PooledWorkload
{
public:
    [[nodiscard]] const char* name() const noexcept override { return "jobs"; }

    bool setup(const WorkloadSetup& setup, cont::String& reason) override
    {
        (void)reason;
        start_pool(setup);
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

    void teardown() override { stop_pool(); }

    [[nodiscard]] u64 sites_per_iteration() const noexcept override { return kJobs; }

    [[nodiscard]] u64 expected() const noexcept override
    {
        const u64 n = static_cast<u64>(kJobs) * kJobItems; // the sum of i * i for i in [0, n)
        return ((n - 1U) * n * ((2U * n) - 1U)) / 6U;
    }
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
#if CRD_DIAG_OVERHEAD_TRACY
        m_tracy = setup.mode == Mode::Tracy;
#endif
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
#if CRD_DIAG_OVERHEAD_TRACY
        else if (m_tracy)
        {
            // The bridge's counterpart: a zone around the compile and the run (its CRD_PERF_SCOPE regions), the plan's
            // shape and the run's dispatch total as plots (its counters), and a zone per dispatched op.
            if (!tracy_run(run_ctx, *loaded.module, arguments, arena, result))
            {
                return false;
            }
        }
#endif
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
        DOV_SITE(*m_sites, kCodeProgramRan, cp::Severity::Info,
                 payload(*m_sites, buffer, kPayloadBytes, "program ran", static_cast<u64>(result),
                         static_cast<u64>(kProgramSteps)),
                 ++m_runs, static_cast<u64>(result));
        signature = static_cast<u64>(result);
        return true;
    }

    void teardown() override {}

    [[nodiscard]] u64 sites_per_iteration() const noexcept override { return 1U; }
    [[nodiscard]] u64 expected() const noexcept override { return static_cast<u64>(kProgramSteps) + 1U; }
    [[nodiscard]] u64 tracy_zones() const noexcept override
    {
#if CRD_DIAG_OVERHEAD_TRACY
        return m_tracy_regions + tracy_peer::ops_zoned();
#else
        return 0U;
#endif
    }

private:
#if CRD_DIAG_OVERHEAD_TRACY
    bool tracy_run(crd::ceir::Context& ctx, const crd::ceir::Module& module, cont::ConstSpan<crd::i64> arguments,
                   crd::memory::IAllocator& arena, crd::i64& result)
    {
        const bool regions = TracyIsConnected;
        const crd::ceir::plan::CompileResult compiled = [&]
        {
            ZoneScopedN("ceir.plan.compile");
            return crd::ceir::plan::compile(ctx, module, "main", &arena);
        }();
        if (!compiled.ok())
        {
            return false;
        }
        TracyPlot("ceir.plan.compile.instrs", static_cast<int64_t>(compiled.stats.num_instrs));
        TracyPlot("ceir.plan.compile.seqs", static_cast<int64_t>(compiled.stats.num_seqs));
        TracyPlot("ceir.plan.compile.funcs", static_cast<int64_t>(compiled.stats.num_funcs));
        TracyPlot("ceir.plan.compile.cells", static_cast<int64_t>(compiled.stats.num_cells));
        TracyPlot("ceir.plan.compile.maps", static_cast<int64_t>(compiled.stats.num_maps));
        const u64 zoned_before = tracy_peer::ops_zoned();
        {
            ZoneScopedN("ceir.plan.run");
            const crd::ceir::plan::RunResult ran =
                crd::ceir::plan::run(compiled.plan, arguments, &arena, tracy_peer::op_zone_hooks());
            if (!ran.ok() || ran.values.size() != 1U)
            {
                return false;
            }
            result = ran.values[0];
        }
        TracyPlot("ceir.plan.dispatch_total", static_cast<int64_t>(tracy_peer::ops_zoned() - zoned_before));
        m_tracy_regions += regions ? 2U : 0U;
        return true;
    }

    bool         m_tracy         = false;
    u64          m_tracy_regions = 0U; // the compile and run zones opened while connected
#endif
    Sites*       m_sites = nullptr;
    bool         m_full  = false;
    u64          m_runs  = 0U;
    cont::String m_path;
    cont::String m_text;
};

// ---- io ---------------------------------------------------------------------------------------------------------

constexpr u32 kIoFiles     = 8U;
constexpr u64 kIoFileBytes = u64{256} << 10U;
constexpr u64 kIoChunk     = u64{16} << 10U;
constexpr u64 kIoChunks    = kIoFileBytes / kIoChunk;
constexpr u32 kIoWhole     = 2U;  // read_file_binary, files 0 and 1
constexpr u32 kIoRanges    = 16U; // read_file_range, one chunk each
constexpr u32 kIoAsync     = 32U; // AsyncFile completions on the pool, one chunk each
constexpr u64 kFnvOffset   = 14695981039346656037ULL;
constexpr u64 kFnvPrime    = 1099511628211ULL;

// The byte at `offset` of file `file`: the top byte of a Fibonacci hash, so a read of the wrong file or offset
// hashes differently.
crd::u8 io_byte(u32 file, u64 offset) noexcept
{
    return static_cast<crd::u8>(((offset + (static_cast<u64>(file) * kIoFileBytes) + 1U) * kAllocSeed) >> 56U);
}

// FNV-1a over 8-byte little-endian words (every read is a multiple of 8 bytes), from memory or from io_byte().
u64 io_hash(const crd::u8* bytes, u64 size) noexcept
{
    u64 h = kFnvOffset;
    for (u64 i = 0U; i < size; i += 8U)
    {
        u64 word = 0U;
        std::memcpy(&word, bytes + i, 8U);
        h = (h ^ word) * kFnvPrime;
    }
    return h;
}

u64 io_hash_generated(u32 file, u64 offset, u64 size) noexcept
{
    u64 h = kFnvOffset;
    for (u64 i = 0U; i < size; i += 8U)
    {
        u64 word = 0U;
        for (u64 k = 0U; k < 8U; ++k)
        {
            word |= static_cast<u64>(io_byte(file, offset + i + k)) << (8U * k);
        }
        h = (h ^ word) * kFnvPrime;
    }
    return h;
}

// Read i's file and offset: ranges and completions visit every file and differing chunks of it.
constexpr u32 io_range_file(u32 i) noexcept
{
    return i % kIoFiles;
}

constexpr u64 io_range_offset(u32 i) noexcept
{
    return ((static_cast<u64>(i) * 5U) % kIoChunks) * kIoChunk;
}

constexpr u32 io_async_file(u32 i) noexcept
{
    return (i + 3U) % kIoFiles;
}

constexpr u64 io_async_offset(u32 i) noexcept
{
    return (((static_cast<u64>(i) * 3U) + 1U) % kIoChunks) * kIoChunk;
}

alignas(64) crd::u8 g_io_async[kIoAsync][kIoChunk]; // NOLINT(modernize-avoid-c-arrays): the completions' targets

// Mixed I/O: the three reads the I/O fault seam consults, on the files setup() wrote. Each frame submits 32 AsyncFile
// chunk reads to the pool, reads two files whole and 16 chunks with read_file_range on this thread while they
// complete, then waits for every completion: 1.25 MiB, 50 reads, each hashed and one site each. The pool is this
// thread and one completion worker whatever --threads says: with more workers the Linux reference host's frames
// alternate between two durations (DIAG.11b), wider than the budgets.
class IoWorkload final : public PooledWorkload
{
public:
    [[nodiscard]] const char* name() const noexcept override { return "io"; }

    bool setup(const WorkloadSetup& setup, cont::String& reason) override
    {
        m_sites = setup.sites;
        if (setup.scratch == nullptr)
        {
            reason.append("io needs --scratch (the directory its files are written to)");
            return false;
        }
        cont::Array<crd::u8> content;
        content.resize(static_cast<usize>(kIoFileBytes));
        for (u32 f = 0U; f < kIoFiles; ++f)
        {
            char name[48];
            (void)std::snprintf(name, sizeof(name), "diag-overhead-io-%u.bin", f);
            m_paths[f] = fs::Path(setup.scratch) / cont::StringView{name};
            for (u64 i = 0U; i < kIoFileBytes; ++i)
            {
                content[static_cast<usize>(i)] = io_byte(f, i);
            }
            const bool written =
                fs::write_file_binary(m_paths[f], cont::ConstSpan<crd::u8>{content.data(), content.size()});
            m_files[f] = written ? crd::platform::AsyncFile::open(m_paths[f].generic()) : crd::platform::AsyncFile{};
            if (!m_files[f].is_open())
            {
                reason.append("an io file could not be written and opened in --scratch");
                remove_files();
                return false;
            }
        }
        m_expected = 0U;
        for (u32 i = 0U; i < kIoWhole; ++i)
        {
            m_expected += io_hash_generated(i, 0U, kIoFileBytes);
        }
        for (u32 i = 0U; i < kIoRanges; ++i)
        {
            m_expected += io_hash_generated(io_range_file(i), io_range_offset(i), kIoChunk);
        }
        for (u32 i = 0U; i < kIoAsync; ++i)
        {
            m_expected += io_hash_generated(io_async_file(i), io_async_offset(i), kIoChunk);
        }
        start_pool(setup);
        return true;
    }

    bool iterate(u64& signature) override
    {
        crd::jobs::Counter*            counters[kIoAsync] = {}; // NOLINT(modernize-avoid-c-arrays)
        crd::platform::AsyncReadStatus status[kIoAsync]   = {}; // NOLINT(modernize-avoid-c-arrays)
        for (u32 i = 0U; i < kIoAsync; ++i)
        {
            counters[i] = m_files[io_async_file(i)].read_async(
                io_async_offset(i), cont::Span<crd::u8>{g_io_async[i], static_cast<usize>(kIoChunk)}, status[i]);
        }
        u64  sum  = 0U;
        u32  read = 0U;
        bool ok   = true;
        for (u32 i = 0U; i < kIoWhole; ++i)
        {
            const bool whole = fs::read_file_binary(m_paths[i], m_bytes) && m_bytes.size() == kIoFileBytes;
            ok               = ok && whole;
            sum += whole ? io_hash(m_bytes.data(), kIoFileBytes) : 0U;
            site(read++, kIoFileBytes);
        }
        for (u32 i = 0U; i < kIoRanges; ++i)
        {
            const bool range = fs::read_file_range(m_paths[io_range_file(i)], io_range_offset(i), kIoChunk, m_bytes) &&
                               m_bytes.size() == kIoChunk;
            ok = ok && range;
            sum += range ? io_hash(m_bytes.data(), kIoChunk) : 0U;
            site(read++, kIoChunk);
        }
        for (u32 i = 0U; i < kIoAsync; ++i)
        {
            if (counters[i] != nullptr)
            {
                crd::jobs::wait(counters[i]);
            }
            const bool done = counters[i] != nullptr && status[i] == crd::platform::AsyncReadStatus::Complete;
            ok              = ok && done;
            sum += done ? io_hash(g_io_async[i], kIoChunk) : 0U;
            site(read++, kIoChunk);
        }
        signature = sum;
        return ok;
    }

    void teardown() override
    {
        stop_pool();
        remove_files();
    }

    [[nodiscard]] u64 sites_per_iteration() const noexcept override { return kIoWhole + kIoRanges + kIoAsync; }
    [[nodiscard]] u64 expected() const noexcept override { return m_expected; }

protected:
    [[nodiscard]] u32 pool_threads(u32 requested) const noexcept override
    {
        (void)requested;
        return 2U;
    }

private:
    void site(u32 read, u64 bytes)
    {
        char buffer[kPayloadBytes];
        DOV_SITE(*m_sites, kCodeRead, cp::Severity::Info,
                 payload(*m_sites, buffer, kPayloadBytes, "read", read, bytes), read, bytes);
    }

    void remove_files()
    {
        for (u32 f = 0U; f < kIoFiles; ++f)
        {
            m_files[f] = crd::platform::AsyncFile{};
            if (!m_paths[f].empty())
            {
                (void)fs::remove_file(m_paths[f]);
            }
        }
    }

    Sites*                   m_sites = nullptr;
    fs::Path                 m_paths[kIoFiles]; // NOLINT(modernize-avoid-c-arrays)
    crd::platform::AsyncFile m_files[kIoFiles]; // NOLINT(modernize-avoid-c-arrays)
    cont::Array<crd::u8>     m_bytes;
    u64                      m_expected = 0U;
};

// ---- render -----------------------------------------------------------------------------------------------------

constexpr u32   kRenderColumns = 64U;    // the visible grid, kRenderColumns x kRenderRows quads on z = 0
constexpr u32   kRenderRows    = 48U;
constexpr u32   kRenderBehind  = 1024U;  // instances behind the camera: every frame culls them
constexpr u32   kRenderMoved   = 64U;    // instances moved every frame (the first grid row)
constexpr u32   kRenderTarget  = 512U;   // the offscreen target's side, in pixels: a quad is about 6 pixels tall
constexpr u32   kRenderCovered = 50000U; // pixels unlike the background the first frame must draw (it draws 124,032)
constexpr usize kRenderHeap    = usize{32} << 20U;

class RenderWorkload final : public Workload
{
public:
    [[nodiscard]] const char* name() const noexcept override { return "render"; }

    bool warm(cont::String& reason) override
    {
#if defined(_WIN32)
        (void)reason; // the D3D12 context owns its compiler and releases it
#else
        // The Vulkan backend loads shaderc once and keeps it for the process: one compile loads it.
        const crd::gpu::ShaderCompileResult compiled = crd::gpu::compile_glsl_to_spirv(
            crd::gpu::ShaderStage::Compute, cont::StringView{"#version 450\nvoid main() {}\n"},
            cont::StringView{"diag-overhead-warm"});
        if (!compiled.ok)
        {
            reason.append("shaderc did not compile: ");
            reason.append(compiled.error_message.c_str());
            return false;
        }
#endif
        return true;
    }

    bool setup(const WorkloadSetup& setup, cont::String& reason) override
    {
        if (!set_up(setup, reason))
        {
            teardown(); // before the host it allocates through stops
            return false;
        }
        return true;
    }

    bool iterate(u64& signature) override
    {
        ++m_frame;
        const f32 shift = (m_frame & 1U) != 0U ? 0.1F : 0.0F;
        for (u32 i = 0U; i < kRenderMoved; ++i)
        {
            place(m_movers[i], i, shift);
        }
        const crd::scenerender::SyncStats   synced = m_renderer->sync(*m_world);
        const crd::scenerender::RenderStats st     = draw();
        char                                buffer[kPayloadBytes];
        DOV_SITE(*m_sites, kCodeFrame, cp::Severity::Info,
                 payload(*m_sites, buffer, kPayloadBytes, "frame drew", st.drawn_instances, st.draws),
                 st.drawn_instances, st.draws);
        signature = signature_of(st);
        return synced.dirty_runs > 0U && st.timed_passes > 0U;
    }

    void teardown() override
    {
        m_renderer.reset();
        m_target.reset();
        m_world.reset();
        m_rm.reset();
        m_raster.reset();
#if defined(_WIN32)
        m_gpu.reset(); // the DX12 capture's lifecycle counts close with the last device
        read_validation();
#else
        read_validation(); // the Vulkan capture's messenger lives on the context's instance
        m_gpu.reset();
#endif
        if (!m_pack.empty())
        {
            (void)fs::remove_file(m_pack);
        }
        m_alloc = nullptr;
        m_heap.reset();
    }

    [[nodiscard]] u64 sites_per_iteration() const noexcept override { return 1U; }
    [[nodiscard]] u64 expected() const noexcept override { return m_expected; }
    [[nodiscard]] const ValidationOutcome* validation() const noexcept override
    {
        return m_outcome.has_value() ? &*m_outcome : nullptr;
    }

private:
    bool set_up(const WorkloadSetup& setup, cont::String& reason)
    {
        m_sites = setup.sites;
        m_validation = setup.validation;
        m_outcome.reset();
        if (setup.scratch == nullptr || setup.assets == nullptr)
        {
            reason.append("render needs --scratch (its mesh pack) and the engine asset root");
            return false;
        }
        m_heap.emplace(kRenderHeap, nullptr, "diag-overhead-render");
        m_alloc = &*m_heap;
        if (setup.host != nullptr)
        {
            m_alloc = setup.host->watch_allocator(&*m_heap, "diag-overhead-render");
            if (m_alloc == nullptr)
            {
                reason.append("the host refused to watch the allocator");
                return false;
            }
        }
        if (!open_device(reason))
        {
            return false;
        }
        const crd::resources::ResourceId mesh = crd::resources::ResourceId::mint_random();
        m_pack = fs::Path(setup.scratch) / cont::StringView{"diag-overhead-render.crdr"};
        m_rm.emplace(m_alloc);
        crd::resources::register_mesh_loader(&*m_rm, nullptr);
        if (!crd::tests::write_one_pack(m_alloc, m_pack, mesh, crd::resources::kFourCC_MESH,
                                        crd::tests::build_quad_mesh_crdr(m_alloc, mesh), "quad") ||
            !m_rm->mount_manifest(m_pack.generic()).is_valid())
        {
            reason.append("the mesh pack could not be written to --scratch and mounted");
            return false;
        }
        spawn(mesh);
        m_renderer.emplace(m_alloc);
        if (!m_renderer->set_asset_root(setup.assets) || !m_renderer->init(*m_raster, *m_rm) ||
            !m_renderer->init_programs(*m_gpu) || !m_renderer->set_shadows_enabled(true) ||
            m_renderer->sync(*m_world).total_instances != kRenderColumns * kRenderRows + kRenderBehind ||
            !m_renderer->set_frame_graph("engine://frame/forward_csm"))
        {
            reason.append("the renderer did not install engine://frame/forward_csm from ");
            reason.append(setup.assets);
            return false;
        }
        m_target = m_raster->create_color_depth_target_for(kRenderTarget, kRenderTarget, m_renderer->output_clear());
        if (m_target == nullptr)
        {
            reason.append("the offscreen target could not be created");
            return false;
        }
        // The first frame's counts are every frame's answer: it must draw the whole grid, cull every instance behind
        // the camera, run its passes on the device and cover the grid's pixels.
        const crd::scenerender::RenderStats first = draw();
        const u32                           lit   = covered();
        if (first.draws == 0U || first.timed_passes == 0U || first.drawn_instances != kRenderColumns * kRenderRows ||
            first.culled_instances != kRenderBehind || lit < kRenderCovered)
        {
            char text[160];
            (void)std::snprintf(text, sizeof(text),
                                "the first frame fell short: %u draws, %u device passes, %u drawn, %u culled, %u of %u "
                                "pixels covered",
                                first.draws, first.timed_passes, first.drawn_instances, first.culled_instances, lit,
                                kRenderCovered);
            reason.append(text);
            return false;
        }
        m_expected = signature_of(first);
        return true;
    }

    bool open_device(cont::String& reason)
    {
        crd::gpu::GpuContextConfig config;
        config.enable_validation = m_validation != Validation::None;
        config.enable_sync_validation = m_validation == Validation::Sync;
        config.enable_gpu_assisted_validation = m_validation == Validation::Gpu;
#if defined(_WIN32)
        if (config.enable_validation)
        {
            // Before any device, or it misses the run; GPU-based validation's infos outgrow the default 256 records.
            m_capture.emplace(crd::memory::default_allocator(), 4096U);
        }
        config.backend = crd::gpu::GpuBackend::Dx12;
        m_gpu          = crd::gpu::create_dx12_gpu_context(crd::memory::default_allocator(), config);
        m_raster       = m_gpu != nullptr && m_gpu->valid()
                             ? crd::gpu::create_dx12_raster_context(crd::memory::default_allocator(), config)
                             : nullptr;
#else
        config.backend  = crd::gpu::GpuBackend::Vulkan;
        config.headless = true;
        m_gpu           = crd::gpu::create_vulkan_gpu_context(config);
        auto* const vk  = m_gpu != nullptr ? static_cast<crd::gpu::VulkanGpuContext*>(m_gpu.get()) : nullptr;
        if (vk != nullptr && config.enable_validation)
        {
            m_capture.emplace(*vk); // on the context's instance, before the raster context is made
        }
        m_raster = vk != nullptr && vk->graphics_capable() && vk->shader_object()
                       ? crd::gpu::create_vulkan_raster_context(*vk)
                       : nullptr;
#endif
        if (m_raster == nullptr)
        {
            reason.append("no graphics device: render needs a D3D12 adapter (Windows) or a Vulkan device with shader "
                          "objects (elsewhere)");
            return false;
        }
        // A requested mode the device does not run refuses the run: its frames would measure nothing.
        m_activation = m_raster->validation_activation();
        // NOLINTNEXTLINE(modernize-avoid-c-arrays)
        const bool asked[] = {config.enable_validation, config.enable_sync_validation,
                              config.enable_gpu_assisted_validation};
        for (usize m = 0U; m < crd::gpu::kValidationModeCount; ++m)
        {
            if (asked[m] && !m_activation.active[m])
            {
                reason.append(validation_name(static_cast<Validation>(m + 1U)));
                reason.append(" validation is not active: ");
                reason.append(crd::gpu::to_string(m_activation.reason[m]));
                return false;
            }
        }
        return true;
    }

    // What the capture saw over the whole run, a capture that missed part of it counted as an instrumentation
    // failure; the capture then goes, before the context it observes.
    void read_validation()
    {
        if (!m_capture.has_value())
        {
            return;
        }
        ValidationOutcome outcome;
        outcome.activation = m_activation;
#if defined(_WIN32)
        const crd::gpu::Dx12ValidationReport native = m_capture->report();
        outcome.report = crd::gpu::to_common(native);
        if (native.contexts_started == 0U || native.contexts_started != native.contexts_finished ||
            native.active_contexts != 0U)
        {
            ++outcome.report.instrumentation_failures;
        }
        for (u32 i = 0U; i < native.messages; ++i)
        {
            crd::gpu::Dx12ValidationMessage message;
            if (m_capture->message(i, message) && message.severity != crd::gpu::Dx12ValidationSeverity::Info)
            {
                std::fprintf(stderr, "crd-diag-overhead: validation %u: %s\n", message.id, message.text);
            }
        }
#else
        outcome.report = m_capture->report();
        if (crd::gpu::validation_layer_spec_version() == 0U)
        {
            ++outcome.report.instrumentation_failures;
        }
        for (const crd::gpu::ValidationMessage& message : m_capture->messages())
        {
            if (message.severity != crd::gpu::ValidationSeverity::Info && !message.cost_notice)
            {
                std::fprintf(stderr, "crd-diag-overhead: validation %s: %s\n", message.message_id_name.c_str(),
                             message.message_text.c_str());
            }
        }
#endif
        m_capture.reset();
        m_outcome = outcome;
    }

    // The grid on z = 0 in front of the camera, then the instances behind it; the first row is kept to move.
    void spawn(const crd::resources::ResourceId& mesh)
    {
        m_world.emplace(m_alloc);
        m_world->register_component<crd::scene::Transform>(crd::scene::transform_serialize_trait(),
                                                           crd::scene::SpatialBVH{});
        crd::scene::register_render_components(*m_world);
        for (u32 i = 0U; i < kRenderColumns * kRenderRows + kRenderBehind; ++i)
        {
            const crd::scene::EntityId e = m_world->spawn();
            place(e, i, 0.0F);
            m_world->add_component(e, crd::scene::MeshRenderer{mesh, {}});
            if (i < kRenderMoved)
            {
                m_movers[i] = e;
            }
        }
    }

    void place(crd::scene::EntityId e, u32 i, f32 shift)
    {
        const bool behind = i >= kRenderColumns * kRenderRows;
        const u32  cell   = behind ? i - (kRenderColumns * kRenderRows) : i;
        const u32  row    = cell / kRenderColumns;
        const crd::math::Vec3f at{(static_cast<f32>(cell % kRenderColumns) * 0.8F) - 25.6F + shift,
                                  (static_cast<f32>(row) * 0.8F) - 19.2F, behind ? 70.0F : 0.0F};
        crd::scene::Transform t;
        t.translation = crd::math::from_raw_vec<crd::units::dim::Length>(at);
        t.world       = crd::math::from_trs(at, crd::math::Quatf::identity(), crd::math::Vec3f{0.4F, 0.4F, 1.0F});
        m_world->add_component(e, t);
    }

    crd::scenerender::RenderStats draw()
    {
        const crd::math::Mat4f view =
            crd::math::look_at(crd::math::Vec3f{0.0F, 0.0F, 50.0F}, crd::math::Vec3f{0.0F, 0.0F, 0.0F},
                               crd::math::Vec3f{0.0F, 1.0F, 0.0F});
        const crd::math::Mat4f proj = crd::math::perspective_reverse_z(1.0472F, 1.0F, 0.1F);
        return m_renderer->render(*m_target, proj * view, crd::math::Vec3f{0.3F, 1.0F, 0.2F},
                                  crd::gpu::ClearColor{0.0F, 0.0F, 0.0F, 1.0F}, nullptr);
    }

    // The pixels unlike the background (the corner, outside the grid).
    [[nodiscard]] u32 covered() const
    {
        const u32 background = m_target->read_pixel(0U, 0U);
        u32       lit        = 0U;
        for (u32 y = 0U; y < kRenderTarget; ++y)
        {
            for (u32 x = 0U; x < kRenderTarget; ++x)
            {
                lit += m_target->read_pixel(x, y) != background ? 1U : 0U;
            }
        }
        return lit;
    }

    [[nodiscard]] static u64 signature_of(const crd::scenerender::RenderStats& st) noexcept
    {
        return static_cast<u64>(st.draws) + (static_cast<u64>(st.timed_passes) << 12U) +
               (static_cast<u64>(st.drawn_instances) << 20U) + (static_cast<u64>(st.culled_instances) << 42U);
    }

    Sites*                                            m_sites = nullptr;
    std::optional<crd::memory::GrowableTlsfAllocator> m_heap;
    crd::memory::IAllocator*                          m_alloc = nullptr;
    std::unique_ptr<crd::gpu::IGpuContext>            m_gpu;
    std::unique_ptr<crd::gpu::IRasterContext>         m_raster;
    std::unique_ptr<crd::gpu::IRasterTarget>          m_target;
    std::optional<crd::resources::ResourceManager>    m_rm;
    std::optional<crd::scene::World>                  m_world;
    std::optional<crd::scenerender::SceneRenderer>    m_renderer;
    crd::scene::EntityId                              m_movers[kRenderMoved] = {}; // NOLINT(modernize-avoid-c-arrays)
    fs::Path                                          m_pack;
    u32                                               m_frame      = 0U;
    u64                                               m_expected   = 0U;
    Validation                                        m_validation = Validation::None;
    crd::gpu::ValidationActivation                    m_activation{};
    std::optional<ValidationOutcome>                  m_outcome;
#if defined(_WIN32)
    std::optional<crd::gpu::Dx12ValidationCapture> m_capture;
#else
    std::optional<crd::gpu::ValidationCapture> m_capture;
#endif
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
            DOV_SITE(*m_sites, kCodeEvent, cp::Severity::Info,
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

// ---- zones ------------------------------------------------------------------------------------------------------

constexpr u32 kZones = 2048U;
static_assert(kZones < cp::kPerThreadRingSlots, "a frame's zones must fit the ring the full mode drains each frame");

class ZonesWorkload final : public Workload
{
public:
    [[nodiscard]] const char* name() const noexcept override { return "zones"; }

    bool setup(const WorkloadSetup& setup, cont::String& reason) override
    {
        (void)reason;
        m_full     = setup.mode == Mode::Full;
        m_tracy    = setup.mode == Mode::Tracy;
        m_recorded = 0U;
        return true;
    }

    bool iterate(u64& signature) override
    {
#if CRD_DIAG_OVERHEAD_TRACY
        signature = m_tracy ? tracy_frame() : cerid_frame();
#else
        signature = cerid_frame();
#endif
#if CRD_PERF_ENABLED
        if (m_full) // this frame's zones, before the full mode's consumer drains the ring
        {
            m_recorded += cp::thread_samples(cp::current_thread_index()).size;
        }
#endif
        return true;
    }

    void teardown() override {}

    [[nodiscard]] u64 sites_per_iteration() const noexcept override { return 0U; }
    [[nodiscard]] u64 expected() const noexcept override { return (static_cast<u64>(kZones) * (kZones - 1U)) / 2U; }
    [[nodiscard]] u64 zones_recorded() const noexcept override { return m_recorded; }
    [[nodiscard]] u64 zones_per_iteration() const noexcept override { return kZones; }
    [[nodiscard]] u64 tracy_zones() const noexcept override { return m_tracy_frames * kZones; }

private:
    static u64 cerid_frame() noexcept
    {
        u64 sum = 0U;
        for (u32 i = 0U; i < kZones; ++i)
        {
            CRD_PERF_SCOPE("diag-overhead.zone");
            sum += i;
        }
        return sum;
    }

#if CRD_DIAG_OVERHEAD_TRACY
    u64 tracy_frame() noexcept
    {
        if (TracyIsConnected) // the frames whose zones a connected client receives
        {
            ++m_tracy_frames;
        }
        u64 sum = 0U;
        for (u32 i = 0U; i < kZones; ++i)
        {
            ZoneScopedN("diag-overhead.zone");
            sum += i;
        }
        return sum;
    }
#endif

    bool m_full         = false;
    bool m_tracy        = false;
    u64  m_recorded     = 0U;
    u64  m_tracy_frames = 0U;
};

// ---- marks ------------------------------------------------------------------------------------------------------

constexpr u32 kMarks = 64U;

class MarksWorkload final : public Workload
{
public:
    [[nodiscard]] const char* name() const noexcept override { return "marks"; }

    bool setup(const WorkloadSetup& setup, cont::String& reason) override
    {
        (void)reason;
        m_tracy = setup.mode == Mode::Tracy;
        return true;
    }

    bool iterate(u64& signature) override
    {
        u64 sum = 0U;
#if CRD_DIAG_OVERHEAD_TRACY
        if (m_tracy)
        {
            for (u32 i = 0U; i < kMarks; ++i)
            {
                FrameMark;
                sum += i;
            }
            signature = sum;
            return true;
        }
#endif
        for (u32 i = 0U; i < kMarks; ++i)
        {
            CRD_PERF_FRAME_MARK();
            sum += i;
        }
        signature = sum;
        return true;
    }

    void teardown() override {}

    [[nodiscard]] u64 sites_per_iteration() const noexcept override { return 0U; }
    [[nodiscard]] u64 expected() const noexcept override { return (static_cast<u64>(kMarks) * (kMarks - 1U)) / 2U; }
    [[nodiscard]] u64 marks_per_iteration() const noexcept override { return kMarks; }

private:
    bool m_tracy = false;
};

AllocWorkload   g_alloc;
JobsWorkload    g_jobs;
OfflineWorkload g_offline;
IoWorkload      g_io;
RenderWorkload  g_render;
EventsWorkload  g_events;
ZonesWorkload   g_zones;
MarksWorkload   g_marks;

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
    case Mode::Tracy:
        return "tracy";
    }
    return "?";
}

const char* validation_name(Validation v) noexcept
{
    switch (v)
    {
    case Validation::None:
        return "none";
    case Validation::Core:
        return "core";
    case Validation::Sync:
        return "sync";
    case Validation::Gpu:
        return "gpu";
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
    Workload* const all[] = {&g_alloc, &g_jobs,   &g_offline, &g_io, // NOLINT(modernize-avoid-c-arrays)
                             &g_render, &g_events, &g_zones,   &g_marks};
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
