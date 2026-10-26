// crd-diag-overhead -- the DIAG.11b measurement harness (see overhead.hpp).
//
//   crd-diag-overhead --workload <alloc|jobs|offline|io|render|events|zones|marks> --mode <off|basic|full>
//                     [--iterations N] [--warmup N] [--captures K] [--threads T] [--affinity <hex mask>]
//                     [--scratch <existing dir>] [--policy-dir <dir>] [--program <file.ceir>] [--keep] [--check]
//                     [--validation <core|sync|gpu>] [--memory] [--clock <tick|system>]
//   crd-diag-overhead-tracy  the same, and --mode tracy [--tracy-connect <ms>] (CRD_DIAG_PEER_TRACY)
//
// One process measures one workload in one mode and prints one JSON record (schema cerid-diag-overhead/1) on stdout:
// the build, the host's modes, every timed iteration in nanoseconds, the CPU every thread used over the timed
// iterations, the sites and their payload evaluations, what the recorder kept and left out, the captures requested
// and delivered with the bytes written, the resident set, the lifetime ledger's windows and, with --check, the
// structural verdicts. Nothing here judges a timing: that is the driver's (scripts/diag-overhead.py), over
// interleaved processes.
//
// The capture phase follows the timed frames: K manual triggers, each waited for (bounded) until the host delivered
// its capture and, with --scratch, wrote its bundle; the bytes written are counted and the bundles removed unless
// --keep. The crash handler writes into --scratch too (only when a crash happens); the io and render workloads write
// their files there in every mode and remove them at teardown.
//
// --check holds what must hold whatever the timing (exit 1 when one fails):
//   every mode   every timed and warmup iteration ran and produced the known answer; the process CPU time over the
//                timed iterations was read; every class of the lifetime ledger is balanced from before the mode was
//                set up to after it was torn down; the per-event clock is the cycle counter wherever it may be mapped
//                and MonotonicClock with --clock system.
//   off          no payload evaluated; setting the mode up acquired nothing (no thread, callback, file or mapping);
//                no recorder and no profiler run.
//   basic        no payload evaluated (every application site is informational); every site and frame event counted
//                as filtered and none recorded; every requested capture delivered (and written, with --scratch).
//   full         every payload evaluated once per site; every site and frame event recorded and none filtered; the
//                profiler counted every frame (where profiling is compiled); every requested capture delivered.
//   --validation (render only) the device validation saw the whole run and reported no error, warning or lost
//                message; the layer's notices of its own cost are counted (cost_notices), not failed. Its cost is
//                reported, not budgeted (ADR-0133: GPU-assisted validation accounts separately).
//   zones, marks the full mode's profiler recorded every zone (none dropped) and counted every mark as a frame.
//   --memory     (alloc only) full: the heap's statistics are complete and balance where the build counts them.
//   tracy        no Cerid diagnostic runs; Tracy started; a client connected from before the first frame to after
//                the last with --tracy-connect, none at either end without it; every payload formatted once per site
//                while connected and none otherwise. What a connected client received is the driver's to hold
//                (scripts/diag-overhead.py reads the capture's zones, frames and messages).
//
// --clock system reads MonotonicClock (steady_clock) for every per-event timestamp and the frames' timing, as on a CPU
// whose counter is not invariant (crd-time's TickClock::prefer_system, before the first read); the record's
// build.tick_clock_mapped and build.clock say which clock ran. The driver's full-system-clock arm is full on it.
//
// Exit codes: 0 measured (and checked), 1 a check failed, 2 usage, 3 the mode or the workload could not be set up.

#include "overhead.hpp"

#include <crd/containers/array.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/build_config.hpp>
#include <crd/core/host_pressure.hpp>
#include <crd/core/lifetime_ledger.hpp>
#include <crd/memory/allocator.hpp>
#include <crd/perf/config.hpp>
#include <crd/perf/diagnostics.hpp>
#include <crd/perf/profiler.hpp>
#include <crd/time/clocks.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#if defined(_WIN32)
#include <crtdbg.h>
#include <windows.h>
#else
#include <sched.h>
#include <time.h>
#endif

#ifndef CRD_DIAG_OVERHEAD_ASSETS
#define CRD_DIAG_OVERHEAD_ASSETS ""
#endif
#ifndef CRD_DIAG_OVERHEAD_ENGINE_ASSETS
#define CRD_DIAG_OVERHEAD_ENGINE_ASSETS ""
#endif
#ifndef CRD_DIAG_OVERHEAD_SHIPPING
#define CRD_DIAG_OVERHEAD_SHIPPING false
#endif
#ifndef CRD_DIAG_OVERHEAD_CONFIG
#define CRD_DIAG_OVERHEAD_CONFIG ""
#endif

namespace
{

namespace dov  = diag_overhead;
namespace cont = crd::containers;
namespace cp   = crd::perf;
using crd::u32;
using crd::u64;
using crd::usize;

constexpr u32 kMaxIterations  = 100000U;
constexpr u32 kMaxCaptures    = 16U;
constexpr u32 kCaptureWaitMs  = 5000U;
constexpr u32 kDefaultWarmup  = 10U;
constexpr u32 kDefaultIters   = 100U;
constexpr u32 kDefaultCapture = 3U;

// What the measured allocator's statistics cover when nothing requests them (DIAG.11b): a CRD_DEBUG build counts
// every allocator from construction; a profiling build without it waits for a request, which no mode makes, so
// its disabled mode pays for none; a build without either compiles them out.
#if CRD_MEMORY_STATS_ON_REQUEST
constexpr crd::memory::StatisticsCoverage kUnaskedCoverage = crd::memory::StatisticsCoverage::NotRequested;
#elif CRD_MEMORY_STATS_TRACKING
constexpr crd::memory::StatisticsCoverage kUnaskedCoverage = crd::memory::StatisticsCoverage::Complete;
#else
constexpr crd::memory::StatisticsCoverage kUnaskedCoverage = crd::memory::StatisticsCoverage::CompiledOut;
#endif

struct Options
{
    const char*     workload   = nullptr;
    bool            memory     = false;
    u32             tracy_connect_ms = 0U; // the tracy mode: wait this long for a client before the frames
    dov::Mode       mode       = dov::Mode::Off;
    bool            mode_given = false;
    u32             iterations = kDefaultIters;
    u32             warmup     = kDefaultWarmup;
    u32             captures   = kDefaultCapture;
    u32             threads    = 8U;
    u64             affinity   = 0U;
    const char*     scratch    = nullptr;
    const char*     policy_dir = CRD_DIAG_OVERHEAD_ASSETS;
    const char*     program    = nullptr;
    bool            keep       = false;
    bool            check      = false;
    bool            system_clock = false; // --clock system: the per-event clock is MonotonicClock for the whole run
    dov::Validation validation = dov::Validation::None;
};

bool same(const char* a, const char* b)
{
    return std::strcmp(a, b) == 0;
}

bool parse_u32(const char* text, u32 limit, u32& out)
{
    char* end   = nullptr;
    const u64 v = std::strtoull(text, &end, 10);
    if (end == text || *end != '\0' || v > limit)
    {
        return false;
    }
    out = static_cast<u32>(v);
    return true;
}

int usage(const char* why)
{
    std::fprintf(stderr,
                 "crd-diag-overhead: %s\nusage: crd-diag-overhead --workload "
                 "<alloc|jobs|offline|io|render|events|zones|marks> --mode <off|basic|full%s> [--iterations N] "
                 "[--warmup N] [--captures K] [--threads T] [--affinity <hex>] [--scratch <dir>] [--policy-dir <dir>] "
                 "[--program <file>] [--keep] [--check] [--validation <core|sync|gpu>] [--memory] "
                 "[--clock <tick|system>]%s\n",
                 why, CRD_DIAG_OVERHEAD_TRACY ? "|tracy" : "",
                 CRD_DIAG_OVERHEAD_TRACY ? " [--tracy-connect <ms>]" : "");
    return 2;
}

bool parse(int argc, char** argv, Options& o, const char*& error)
{
    for (int i = 1; i < argc; ++i)
    {
        const char* a     = argv[i];
        const char* value = (i + 1 < argc) ? argv[i + 1] : nullptr;
        if (same(a, "--keep"))
        {
            o.keep = true;
            continue;
        }
        if (same(a, "--check"))
        {
            o.check = true;
            continue;
        }
        if (same(a, "--memory"))
        {
            o.memory = true;
            continue;
        }
        if (value == nullptr)
        {
            error = "an option is missing its value";
            return false;
        }
        ++i;
        bool ok = true;
        if (same(a, "--workload"))
        {
            o.workload = value;
        }
        else if (same(a, "--mode"))
        {
            o.mode_given = true;
            if (same(value, "off"))
            {
                o.mode = dov::Mode::Off;
            }
            else if (same(value, "basic"))
            {
                o.mode = dov::Mode::Basic;
            }
            else if (same(value, "full"))
            {
                o.mode = dov::Mode::Full;
            }
            else if (CRD_DIAG_OVERHEAD_TRACY && same(value, "tracy"))
            {
                o.mode = dov::Mode::Tracy;
            }
            else
            {
                ok = false;
            }
        }
        else if (same(a, "--iterations"))
        {
            ok = parse_u32(value, kMaxIterations, o.iterations) && o.iterations > 0U;
        }
        else if (same(a, "--warmup"))
        {
            ok = parse_u32(value, kMaxIterations, o.warmup);
        }
        else if (same(a, "--captures"))
        {
            ok = parse_u32(value, kMaxCaptures, o.captures);
        }
        else if (same(a, "--threads"))
        {
            ok = parse_u32(value, 256U, o.threads) && o.threads > 0U;
        }
        else if (same(a, "--affinity"))
        {
            char* end  = nullptr;
            o.affinity = std::strtoull(value, &end, 16);
            ok         = end != value && *end == '\0' && o.affinity != 0U;
        }
        else if (same(a, "--scratch"))
        {
            o.scratch = value;
        }
        else if (same(a, "--policy-dir"))
        {
            o.policy_dir = value;
        }
        else if (same(a, "--program"))
        {
            o.program = value;
        }
        else if (same(a, "--tracy-connect"))
        {
            ok = parse_u32(value, 600000U, o.tracy_connect_ms) && o.tracy_connect_ms > 0U;
        }
        else if (same(a, "--clock"))
        {
            ok             = value != nullptr && (same(value, "tick") || same(value, "system"));
            o.system_clock = ok && same(value, "system");
        }
        else if (same(a, "--validation"))
        {
            ok = false;
            for (const dov::Validation v : {dov::Validation::Core, dov::Validation::Sync, dov::Validation::Gpu})
            {
                if (same(value, dov::validation_name(v)))
                {
                    o.validation = v;
                    ok = true;
                }
            }
        }
        else
        {
            error = "unknown option";
            return false;
        }
        if (!ok)
        {
            error = "an option has a bad value";
            return false;
        }
    }
    if (o.workload == nullptr || !o.mode_given)
    {
        error = "--workload and --mode are required";
        return false;
    }
    return true;
}

void run_headless()
{
    crd::set_assert_headless(true);
#if defined(_WIN32)
    ::SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    (void)_set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    (void)_CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    (void)_CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    (void)_CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    (void)_CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
#endif
}

// Pin the process to `mask` (bit i = logical processor i). False when the OS refused.
bool pin(u64 mask)
{
#if defined(_WIN32)
    return ::SetProcessAffinityMask(::GetCurrentProcess(), static_cast<DWORD_PTR>(mask)) != 0;
#else
    cpu_set_t set;
    CPU_ZERO(&set);
    for (u32 i = 0U; i < 64U; ++i)
    {
        if ((mask >> i) & 1U)
        {
            CPU_SET(i, &set);
        }
    }
    return sched_setaffinity(0, sizeof(set), &set) == 0;
#endif
}

// The CPU every thread of the process has used so far. Windows counts cycles (`QueryProcessCycleTime`;
// `GetProcessTimes` advances by whole clock ticks, too coarse for a few seconds of frames), POSIX nanoseconds; the
// driver compares only ratios within one unit.
#if defined(_WIN32)
constexpr const char* kCpuUnit = "cycles";
#else
constexpr const char* kCpuUnit = "ns";
#endif

bool read_process_cpu(u64& out)
{
#if defined(_WIN32)
    ULONG64 cycles = 0U;
    if (::QueryProcessCycleTime(::GetCurrentProcess(), &cycles) == 0)
    {
        return false;
    }
    out = cycles;
#else
    timespec now{};
    if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &now) != 0)
    {
        return false;
    }
    out = (static_cast<u64>(now.tv_sec) * 1000000000ULL) + static_cast<u64>(now.tv_nsec);
#endif
    return true;
}

u64 file_bytes(const char* path)
{
    std::FILE* f = dov::open_for_reading(path);
    if (f == nullptr)
    {
        return 0U;
    }
    u64 bytes = 0U;
    if (std::fseek(f, 0, SEEK_END) == 0)
    {
        const long end = std::ftell(f);
        bytes          = end > 0 ? static_cast<u64>(end) : 0U;
    }
    (void)std::fclose(f);
    return bytes;
}

// ---- JSON -------------------------------------------------------------------------------------------------------

class Json
{
public:
    explicit Json(cont::String& out) : m_out(out) {}

    void open(const char* key, char bracket)
    {
        this->key(key);
        m_out.push_back(bracket);
        m_first = true;
    }
    void close(char bracket)
    {
        m_out.push_back(bracket);
        m_first = false;
    }
    void str(const char* key, const char* value)
    {
        this->key(key);
        m_out.push_back('"');
        for (const char* p = value; *p != '\0'; ++p)
        {
            const char c = *p;
            if (c == '"' || c == '\\')
            {
                m_out.push_back('\\');
                m_out.push_back(c);
            }
            else if (static_cast<unsigned char>(c) < 0x20U)
            {
                m_out.push_back(' ');
            }
            else
            {
                m_out.push_back(c);
            }
        }
        m_out.push_back('"');
    }
    void u(const char* key, u64 value)
    {
        this->key(key);
        char text[32];
        (void)std::snprintf(text, sizeof(text), "%llu", static_cast<unsigned long long>(value));
        m_out.append(text);
    }
    void i(const char* key, crd::i64 value)
    {
        this->key(key);
        char text[32];
        (void)std::snprintf(text, sizeof(text), "%lld", static_cast<long long>(value));
        m_out.append(text);
    }
    void b(const char* key, bool value)
    {
        this->key(key);
        m_out.append(value ? "true" : "false");
    }

private:
    void key(const char* key)
    {
        if (!m_first)
        {
            m_out.push_back(',');
        }
        m_first = false;
        if (key != nullptr)
        {
            m_out.push_back('"');
            m_out.append(key);
            m_out.append("\":");
        }
    }

    cont::String& m_out;
    bool          m_first = true;
};

void lifetime_window(Json& j, const char* key, const crd::LifetimeSnapshot& before, const crd::LifetimeSnapshot& after)
{
    j.open(key, '{');
    for (usize c = 0U; c < crd::kLifetimeClassCount; ++c)
    {
        const auto cls = static_cast<crd::LifetimeClass>(c);
        j.u(crd::lifetime_class_name(cls), after.of(cls).acquired - before.of(cls).acquired);
    }
    j.close('}');
}

u64 acquired_in(const crd::LifetimeSnapshot& before, const crd::LifetimeSnapshot& after)
{
    u64 total = 0U;
    for (usize c = 0U; c < crd::kLifetimeClassCount; ++c)
    {
        const auto cls = static_cast<crd::LifetimeClass>(c);
        total += after.of(cls).acquired - before.of(cls).acquired;
    }
    return total;
}

struct Checks
{
    u32  failed = 0U;
    char text[1024]{};
    usize used = 0U;

    void expect(bool ok, const char* what)
    {
        if (ok)
        {
            return;
        }
        ++failed;
        const int n = std::snprintf(text + used, sizeof(text) - used, "%s%s", used == 0U ? "" : "; ", what);
        if (n > 0 && used + static_cast<usize>(n) < sizeof(text))
        {
            used += static_cast<usize>(n);
        }
    }
};

} // namespace

int main(int argc, char** argv)
{
    run_headless();
    Options     o;
    const char* error = nullptr;
    if (!parse(argc, argv, o, error))
    {
        return usage(error);
    }
    // --clock system: MonotonicClock for good, decided before the first read (nothing above reads the clock).
    if (o.system_clock && !crd::time::TickClock::prefer_system())
    {
        std::fprintf(stderr, "crd-diag-overhead: the clock was read before --clock system could take effect\n");
        return 3;
    }
    // The frames are timed with the diagnostic clock (TickClock): calibrated before anything, so every arm's frames are
    // timed by the same mapped counter from the first (a mode's start would otherwise calibrate it in some arms only).
    crd::time::TickClock::warm();
    dov::Workload* const workload = dov::find_workload(o.workload);
    if (workload == nullptr)
    {
        return usage("unknown workload");
    }
    if (o.validation != dov::Validation::None && !same(workload->name(), "render"))
    {
        return usage("--validation applies to the render workload");
    }
    const bool cerid_host = o.mode == dov::Mode::Basic || o.mode == dov::Mode::Full;
    if (cerid_host && o.scratch == nullptr)
    {
        return usage("basic and full need --scratch <existing dir> (crash and capture directory)");
    }
    if (o.memory && !same(workload->name(), "alloc"))
    {
        return usage("--memory applies to the alloc workload");
    }
    if (o.tracy_connect_ms != 0U && o.mode != dov::Mode::Tracy)
    {
        return usage("--tracy-connect applies to the tracy mode");
    }
    char program_path[1024];
    if (o.program == nullptr)
    {
        (void)std::snprintf(program_path, sizeof(program_path), "%s/overhead_offline.ceir", o.policy_dir);
        o.program = program_path;
    }
    const bool pinned = o.affinity != 0U && pin(o.affinity);

    cont::String reason;
    dov::Sites   sites;
    cont::Array<u64> samples;
    samples.reserve(o.iterations);
    if (!workload->warm(reason))
    {
        std::fprintf(stderr, "crd-diag-overhead: the workload's process caches did not fill: %s\n", reason.c_str());
        return 3;
    }

    const crd::LifetimeSnapshot  before_mode   = crd::lifetime_ledger().snapshot();
    const crd::HostMemoryReading memory_before = crd::read_host_memory();

    // ---- the mode ------------------------------------------------------------------------------------------------
    cp::DiagnosticHost host;
    bool               host_started = false;
    bool               perf_started = false;
    char               policy_path[1024];
    if (cerid_host)
    {
        (void)std::snprintf(policy_path, sizeof(policy_path), "%s/overhead_%s.flight.toml", o.policy_dir,
                            dov::mode_name(o.mode));
        cp::DiagnosticHostProfile profile;
        profile.crash_dir     = cont::String{o.scratch};
        profile.capture_dir   = cont::String{o.scratch};
        profile.flight_policy = cont::String{policy_path};
        const cp::DiagHostStart start = host.start(profile, reason);
        if (start != cp::DiagHostStart::Started)
        {
            std::fprintf(stderr, "crd-diag-overhead: the host did not start (%s): %s\n",
                         cp::diag_host_start_name(start).data(), reason.c_str());
            return 3;
        }
        host_started = true;
        (void)cp::register_code_range("diag-overhead", dov::kCodeFirst, dov::kCodeLast);
        host.recorder().set_replay_recipe(cont::StringView{"crd-diag-overhead (a measurement run; replay = rerun)"});
        sites.recorder = &host.recorder();
    }
#if CRD_PERF_ENABLED
    if (o.mode == dov::Mode::Full)
    {
        cp::init({});
        perf_started = cp::is_active();
    }
#endif
    // The peer: Tracy started (its threads are its own, outside the lifetime ledger), every site its counterpart.
    bool tracy_started       = false;
    bool tracy_connected     = false;
    bool tracy_connected_end = false;
#if CRD_DIAG_OVERHEAD_TRACY
    if (o.mode == dov::Mode::Tracy)
    {
        dov::tracy_peer::start();
        tracy_started = true;
        sites.tracy   = true;
        if (o.tracy_connect_ms != 0U && !dov::tracy_peer::wait_connected(o.tracy_connect_ms))
        {
            std::fprintf(stderr, "crd-diag-overhead: no Tracy client connected within %u ms\n", o.tracy_connect_ms);
            dov::tracy_peer::stop();
            return 3;
        }
        tracy_connected = dov::tracy_peer::connected();
    }
#endif
    const crd::LifetimeSnapshot  after_mode   = crd::lifetime_ledger().snapshot();
    const crd::HostMemoryReading memory_mode  = crd::read_host_memory();

    // ---- the workload --------------------------------------------------------------------------------------------
    dov::WorkloadSetup setup;
    setup.mode       = o.mode;
    setup.host       = host_started ? &host : nullptr;
    setup.sites      = &sites;
    setup.threads    = o.threads;
    setup.program    = o.program;
    setup.scratch    = o.scratch;
    setup.assets     = CRD_DIAG_OVERHEAD_ENGINE_ASSETS;
    setup.validation = o.validation;
    setup.memory     = o.memory;
    if (!workload->setup(setup, reason))
    {
        std::fprintf(stderr, "crd-diag-overhead: the workload did not set up: %s\n", reason.c_str());
        host.stop();
#if CRD_DIAG_OVERHEAD_TRACY
        if (tracy_started)
        {
            dov::tracy_peer::stop();
        }
#endif
        return 3;
    }
    const crd::LifetimeSnapshot after_setup = crd::lifetime_ledger().snapshot();

    // ---- the frames ----------------------------------------------------------------------------------------------
    const u32 total     = o.warmup + o.iterations;
    u32       executed  = 0U;
    u32       wrong     = 0U;
    u64       signature = 0U;
    u64       cpu_begin = 0U;
    u64       cpu_end   = 0U;
    bool      cpu_read  = false;
    for (u32 frame = 0U; frame < total; ++frame)
    {
        if (frame == o.warmup)
        {
            cpu_read = read_process_cpu(cpu_begin);
        }
        const u64  begin = cp::diagnostic_now_ns();
        const bool ok    = workload->iterate(signature);
        const u64  work  = cp::diagnostic_now_ns();
        if (host_started)
        {
            (void)host.frame(begin, work);
        }
#if CRD_PERF_ENABLED
        if (perf_started)
        {
            cp::frame_mark();
            cp::clear_samples(); // the full mode's consumer drains every frame
        }
#endif
#if CRD_DIAG_OVERHEAD_TRACY
        if (tracy_started)
        {
            FrameMark; // Tracy's consumer is its own worker thread, which streams to a connected client
        }
#endif
        const u64 end = cp::diagnostic_now_ns();
        ++executed;
        if (!ok || signature != workload->expected())
        {
            ++wrong;
        }
        if (frame >= o.warmup)
        {
            samples.push_back(end - begin);
        }
    }
    cpu_read = cpu_read && read_process_cpu(cpu_end) && cpu_end > cpu_begin;
#if CRD_DIAG_OVERHEAD_TRACY
    tracy_connected_end = tracy_started && dov::tracy_peer::connected();
#endif
    const crd::HostMemoryReading memory_frames = crd::read_host_memory();
    const u64                    recorded      = host_started ? host.recorder().total_recorded() : 0U;
    const u64                    filtered      = host_started ? host.recorder().total_filtered() : 0U;
    u64                          profiler_drops = 0U;
    u64                          profiler_frames = 0U;
#if CRD_PERF_ENABLED
    if (perf_started)
    {
        profiler_frames = cp::frame_count();
        for (u32 t = 0U; t < cp::kMaxThreads; ++t)
        {
            profiler_drops += cp::thread_samples(static_cast<crd::u8>(t)).dropped;
        }
    }
#endif

    // ---- the captures --------------------------------------------------------------------------------------------
    u32 requested = 0U;
    u32 delivered = 0U;
    u64 io_bytes  = 0U;
    u64 longest   = 0U;
    if (host_started)
    {
        for (u32 k = 0U; k < o.captures; ++k)
        {
            const u64             written_before = host.bundles_written();
            const u64             delivered_before = host.recorder().captures_delivered();
            cp::FlightTriggerInput input;
            input.kind    = cp::FlightTrigger::Manual;
            input.code    = cp::kFlightCodeManual;
            input.message = cont::StringView{"overhead capture"};
            input.value_a = k;
            ++requested;
            const u64 asked = cp::diagnostic_now_ns();
            if (host.recorder().trigger(input) != cp::FlightTriggerResult::Started)
            {
                continue;
            }
            bool done = false;
            for (u32 waited = 0U; waited <= kCaptureWaitMs && !done; waited += 1U)
            {
                (void)host.poll();
                done = host.recorder().captures_delivered() > delivered_before &&
                       host.bundles_written() + host.bundle_failures() > written_before;
                if (!done)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            }
            if (!done)
            {
                continue;
            }
            const u64 took = cp::diagnostic_now_ns() - asked;
            longest        = took > longest ? took : longest;
            if (host.bundles_written() > written_before)
            {
                ++delivered;
                const cont::String path = host.last_bundle();
                io_bytes += file_bytes(path.c_str());
                if (!o.keep)
                {
                    (void)std::remove(path.c_str());
                }
            }
        }
    }
    const u64 failures   = host_started ? host.bundle_failures() : 0U;
    // The recorder's storage: every thread ring and the shared ring (DIAG.11b (q1)), whether a thread touched it or not.
    const u64 ring_bytes = host_started ? host.recorder().ring_stats().ring_bytes : 0U;
    const bool recorder_running = host_started && host.recorder().is_initialized();

    // ---- the measured allocator's statistics, read before teardown destroys it -----------------------------------
    const crd::memory::IAllocator* const measured = workload->measured_allocator();
    // --memory's full mode requests them before the heap's first allocation; what that yields is the build's.
    const bool requested_stats = o.memory && o.mode == dov::Mode::Full && CRD_ENABLE_PROFILING != 0;
    const crd::memory::StatisticsCoverage expected_coverage =
        requested_stats && CRD_MEMORY_STATS_TRACKING ? crd::memory::StatisticsCoverage::Complete : kUnaskedCoverage;
    const crd::memory::StatisticsCoverage stats_coverage =
        measured != nullptr ? measured->stats().coverage() : expected_coverage;
    const u64 zones_recorded = workload->zones_recorded();
    const u64 tracy_zones    = workload->tracy_zones();
#if CRD_DIAG_OVERHEAD_TRACY
    const u64 tracy_unmatched =
        tracy_started ? dov::tracy_peer::jobs_unmatched() + dov::tracy_peer::ops_unmatched() : 0U;
#else
    const u64 tracy_unmatched = 0U;
#endif
    const crd::memory::MemoryStats::Snapshot stats =
        measured != nullptr ? measured->stats().snapshot() : crd::memory::MemoryStats::Snapshot{};

    // ---- teardown ------------------------------------------------------------------------------------------------
    workload->teardown();
    const dov::ValidationOutcome* const validated = workload->validation();
#if CRD_DIAG_OVERHEAD_TRACY
    if (tracy_started) // after every instrumented thread has ended (the pool joined its workers in teardown)
    {
        dov::tracy_peer::stop();
    }
#endif
#if CRD_PERF_ENABLED
    if (perf_started)
    {
        cp::shutdown();
    }
#endif
    if (host_started)
    {
        host.stop();
    }
    const crd::LifetimeSnapshot after_all = crd::lifetime_ledger().snapshot();
    const crd::LifetimeReport   balance   = crd::lifetime_report(before_mode, after_all);

    // ---- checks --------------------------------------------------------------------------------------------------
    const u64 frames_total = total;
    const u64 sites_total  = workload->sites_per_iteration() * frames_total;
    const u64 marks_total  = frames_total * (1U + workload->marks_per_iteration());
    const u64 payloads     = sites.payloads.load(std::memory_order_relaxed);
    const u64 setup_threads =
        after_setup.of(crd::LifetimeClass::Thread).acquired - after_mode.of(crd::LifetimeClass::Thread).acquired;
    Checks checks;
    if (o.check)
    {
        checks.expect(executed == total && samples.size() == o.iterations, "not every frame ran");
        checks.expect(wrong == 0U, "a frame computed the wrong answer");
        checks.expect(cpu_read, "the process CPU time over the timed frames was not read");
        checks.expect(balance.verdict == crd::LifetimeVerdict::Balanced, "the lifetime ledger is not balanced");
        // The clock the run asked for: the cycle counter wherever it may be mapped, MonotonicClock with --clock system.
        checks.expect(crd::time::TickClock::counter_mapped() ==
                          (!o.system_clock && crd::time::TickClock::counter_usable()),
                      o.system_clock ? "--clock system: the per-event clock was the cycle counter"
                                     : "the per-event clock is not the cycle counter where it may be");
        checks.expect(setup_threads == workload->own_threads(o.threads) + workload->diagnostic_threads(o.mode),
                      "the workload's setup started another number of threads than the mode adds to its own");
        if (measured != nullptr)
        {
            checks.expect(stats_coverage == expected_coverage,
                          requested_stats ? "the requested statistics of the measured allocator are not complete"
                                          : "the measured allocator's statistics cover something other than this "
                                            "build counts unasked");
            if (expected_coverage == crd::memory::StatisticsCoverage::Complete)
            {
                checks.expect(stats.alloc_count >= frames_total && stats.alloc_count == stats.dealloc_count &&
                                  stats.bytes_in_use == 0U,
                              "the measured allocator's counted statistics do not balance");
            }
            else
            {
                checks.expect(stats.alloc_count == 0U && stats.dealloc_count == 0U && stats.total_bytes == 0U &&
                                  stats.peak_bytes == 0U,
                              "the measured allocator counted operations nothing requested");
            }
        }
        if (o.mode == dov::Mode::Off)
        {
            checks.expect(payloads == 0U, "off: a payload was evaluated");
            checks.expect(acquired_in(before_mode, after_mode) == 0U, "off: setting the mode up acquired something");
            checks.expect(!recorder_running, "off: a recorder runs");
            checks.expect(!perf_started, "off: the profiler runs");
            checks.expect(requested == 0U && io_bytes == 0U, "off: a capture was asked for or written");
        }
        else if (o.mode == dov::Mode::Tracy)
        {
            checks.expect(tracy_started, "tracy: Tracy did not start");
            checks.expect(!recorder_running && !perf_started && requested == 0U && io_bytes == 0U,
                          "tracy: a Cerid diagnostic ran");
            const bool expect_client = o.tracy_connect_ms != 0U;
            checks.expect(tracy_connected == expect_client && tracy_connected_end == expect_client,
                          expect_client ? "tracy: the client was not connected from the first frame to the last"
                                        : "tracy: a client connected although none was asked for");
            checks.expect(payloads == (expect_client ? sites_total : 0U),
                          "tracy: not every payload was formatted once while connected, or one was without a client");
            checks.expect(tracy_unmatched == 0U, "tracy: a job's or an op's zone did not close where it began");
            checks.expect(expect_client || tracy_zones == 0U, "tracy: a zone was emitted without a client");
        }
        else if (o.mode == dov::Mode::Basic)
        {
            checks.expect(payloads == 0U, "basic: an informational payload was evaluated");
            checks.expect(recorded == 0U, "basic: an informational event was recorded");
            checks.expect(filtered == sites_total + frames_total, "basic: not every site and frame was filtered");
            checks.expect(delivered == o.captures && failures == 0U, "basic: a capture was not delivered");
        }
        else
        {
            checks.expect(payloads == sites_total, "full: not every payload was evaluated once");
            checks.expect(recorded == sites_total + frames_total, "full: not every site and frame was recorded");
            checks.expect(filtered == 0U, "full: an event was filtered");
            checks.expect(delivered == o.captures && failures == 0U, "full: a capture was not delivered");
            checks.expect(!CRD_PERF_ENABLED || profiler_frames == marks_total,
                          "full: the profiler did not count every frame and mark");
            checks.expect(!CRD_PERF_ENABLED || zones_recorded == workload->zones_per_iteration() * frames_total,
                          "full: the profiler did not record every zone");
            checks.expect(profiler_drops == 0U, "full: the profiler dropped a sample");
        }
        if (o.mode != dov::Mode::Full)
        {
            checks.expect(zones_recorded == 0U, "a mode without the profiler recorded a zone");
        }
        if (o.validation != dov::Validation::None)
        {
            checks.expect(validated != nullptr && validated->report.clean(),
                          "validation: a message was reported or lost, or the capture missed part of the run");
        }
    }

    // ---- the record ----------------------------------------------------------------------------------------------
    cont::String out;
    Json         j(out);
    out.push_back('{');
    j.str("schema", "cerid-diag-overhead/1");
    j.str("workload", workload->name());
    j.str("mode", dov::mode_name(o.mode));
    j.open("build", '{');
    j.b("profiling", CRD_ENABLE_PROFILING != 0);
    j.b("asserts", CRD_ENABLE_ASSERTS != 0);
    j.b("shipping", CRD_DIAG_OVERHEAD_SHIPPING);
    j.b("tick_clock_mapped", crd::time::TickClock::counter_mapped()); // the per-event clock was the cycle counter
    j.str("clock", o.system_clock ? "system" : "tick");               // --clock
    j.str("config", CRD_DIAG_OVERHEAD_CONFIG);
#if defined(_MSC_VER) && !defined(__clang__)
    j.str("compiler", "msvc");
    j.u("compiler_version", static_cast<u64>(_MSC_FULL_VER));
#elif defined(__clang__)
    j.str("compiler", "clang");
    j.u("compiler_version",
        static_cast<u64>((__clang_major__ * 10000) + (__clang_minor__ * 100) + __clang_patchlevel__));
#elif defined(__GNUC__)
    j.str("compiler", "gcc");
    j.u("compiler_version", static_cast<u64>((__GNUC__ * 10000) + (__GNUC_MINOR__ * 100) + __GNUC_PATCHLEVEL__));
#endif
    j.close('}');
    j.open("host", '{');
    j.b("started", host_started);
    if (host_started)
    {
        const cp::DiagnosticHostModes& modes = host.modes();
        j.str("crash", cp::diag_mode_state_name(modes.crash).data());
        j.str("flight", cp::diag_mode_state_name(modes.flight).data());
        j.str("faults", cp::diag_mode_state_name(modes.faults).data());
        j.str("policy", policy_path);
    }
    j.b("profiler", perf_started);
    j.b("memory", o.memory);
    j.u("affinity", o.affinity);
    j.b("pinned", pinned);
    j.u("threads", o.threads);
    j.close('}');
    j.u("warmup", o.warmup);
    j.u("iterations", o.iterations);
    j.u("executed", executed);
    j.u("wrong", wrong);
    j.u("expected_signature", workload->expected());
    j.open("samples_ns", '[');
    for (const u64 s : samples)
    {
        j.u(nullptr, s);
    }
    j.close(']');
    j.open("cpu", '{');
    j.b("read", cpu_read);
    j.str("unit", kCpuUnit);
    j.u("timed", cpu_read ? cpu_end - cpu_begin : 0U);
    j.close('}');
    j.open("events", '{');
    j.u("sites", sites_total);
    j.u("frames", frames_total);
    j.u("payloads", payloads);
    j.u("recorded", recorded);
    j.u("filtered", filtered);
    j.u("profiler_frames", profiler_frames);
    j.u("profiler_drops", profiler_drops);
    j.u("zones_recorded", zones_recorded);
    j.u("marks", marks_total);
    j.close('}');
    j.open("tracy", '{');
    j.b("started", tracy_started);
    j.u("connect_wait_ms", o.tracy_connect_ms);
    j.b("connected", tracy_connected);
    j.b("connected_at_end", tracy_connected_end);
    j.u("zones", tracy_zones);
    j.u("messages", tracy_started ? payloads : 0U);
    j.u("frame_marks", tracy_started ? marks_total : 0U);
    j.u("unmatched", tracy_unmatched);
    j.close('}');
    j.open("captures", '{');
    j.u("requested", requested);
    j.u("delivered", delivered);
    j.u("failures", failures);
    j.u("bytes_written", io_bytes);
    j.u("longest_ns", longest);
    j.close('}');
    j.open("memory", '{');
    j.b("read", memory_before.resident.read && memory_frames.resident.read);
    j.u("resident_before", memory_before.resident.in_use);
    j.u("resident_mode", memory_mode.resident.in_use);
    j.u("resident_frames", memory_frames.resident.in_use);
    j.u("resident_peak", memory_frames.resident.high_water);
    j.u("recorder_ring_bytes", ring_bytes);
    j.close('}');
    j.open("statistics", '{');
    j.b("measured", measured != nullptr);
    j.str("coverage", crd::memory::statistics_coverage_name(stats_coverage));
    j.u("allocations", stats.alloc_count);
    j.u("deallocations", stats.dealloc_count);
    j.u("bytes_in_use", stats.bytes_in_use);
    j.close('}');
    j.open("validation", '{');
    j.str("requested", dov::validation_name(o.validation));
    if (validated != nullptr)
    {
        j.b("core", validated->activation.active[0]);
        j.b("sync", validated->activation.active[1]);
        j.b("gpu", validated->activation.active[2]);
        j.u("errors", validated->report.error);
        j.u("warnings", validated->report.warning);
        j.u("info", validated->report.info);
        j.u("lost", validated->report.dropped + validated->report.truncated);
        j.u("instrumentation_failures", validated->report.instrumentation_failures);
        j.u("cost_notices", validated->report.cost_notices);
    }
    j.close('}');
    j.open("lifetime", '{');
    lifetime_window(j, "mode_setup", before_mode, after_mode);
    lifetime_window(j, "workload_setup", after_mode, after_setup);
    j.str("balance", crd::lifetime_verdict_name(balance.verdict));
    cont::String unbalanced; // the classes that hold more or less than before the mode was set up
    for (usize c = 0U; c < crd::kLifetimeClassCount; ++c)
    {
        const auto cls = static_cast<crd::LifetimeClass>(c);
        if (((balance.outstanding | balance.drained | balance.underflowed) & crd::lifetime_class_bit(cls)) != 0U)
        {
            unbalanced.append(unbalanced.empty() ? "" : ",");
            unbalanced.append(crd::lifetime_class_name(cls));
        }
    }
    j.str("unbalanced", unbalanced.c_str());
    j.close('}');
    j.open("check", '{');
    j.b("ran", o.check);
    j.b("passed", checks.failed == 0U);
    j.str("failures", checks.text);
    j.close('}');
    out.push_back('}');
    std::printf("%s\n", out.c_str());
    (void)std::fflush(stdout);
    if (checks.failed != 0U)
    {
        std::fprintf(stderr, "crd-diag-overhead: %s\n", checks.text);
        return 1;
    }
    return 0;
}
