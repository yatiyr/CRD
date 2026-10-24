// The DIAG.11a diagnostic-host specimen: a small application that composes the engine's DiagnosticHost from an
// authored profile (argv[1], a diagnostic-host asset) before anything else, records its own work into the flight
// recorder, then runs one mode (argv[2]) with its defect, or without it when argv[3] is "repaired". Nothing in it wires
// a crash handler, a recorder trigger or a bundle writer by hand: the profile's capture directory gets the bundle. The
// parent (crd-perf-tests) reads it: the trigger, the events before it, the replay recipe, the build and this
// executable's own identity. DIAG.12 runs every mode of the same application faulty, then repaired.
//
//   invariant       a refund larger than the payment fails a CRD_DIAG_INVARIANT (a fatal invariant)
//   assert          the same defect reported through crd's own assert path while headless (it aborts)
//   use-after-free  an order written after its release: the DiagnosticAllocator's quarantine names the write, its
//                   event carries the allocation and free sites, and a fatal invariant ends the run
//   wait            two jobs each wait on the other's counter: the watchdog window the host gives the pool reports the
//                   wait cycle as a hang, the bundle is written while the jobs stay parked, and the run exits 5
//   race            two jobs add to one plain counter: ThreadSanitizer (halt_on_error) aborts the run; no other build
//                   can catch it, so the mode announces sanitizer "none" there and the harness says InstrumentAbsent
//   crash           an order written through a lookup that found nothing: the crash handler the host installed
//                   writes one dump (Windows) or signal record (Linux) into the profile's crash directory, no bundle
//   program         the authored order program beside the profile (orders.ceir) is cooked, loaded, compiled and run on
//                   the engine's CEIR path: its zero loop step faults the run, the application reports the fault as a
//                   trigger at the authored op's file and line, stop() writes the bundle and the run exits 7
//   gpu             the order ledger goes up to a Vulkan buffer in two overlapping copies with no barrier between them:
//                   synchronization validation reports the write-after-write on that buffer inside the upload's
//                   labelled pass, the application reports it as a trigger naming both, stop() writes the bundle and
//                   the run exits 9; a host with no Vulkan device, no validation layer or no synchronization
//                   validation prints CRD_HOST_GPU_ABSENT with the reason and exits 10, never clean
// Every mode first records 40 orders; the last names its customer as a restricted event. A repaired run prints
// CRD_HOST_CLEAN with the bundles written and exits 0; an unknown mode exits 2.
#include "specimen_common.hpp"

#include <crd/ceir/ceir.hpp>
#include <crd/ceir/cook/program_cook.hpp>
#include <crd/ceir/func.hpp>
#include <crd/ceir/gen/arith_ops.hpp>
#include <crd/ceir/gen/core_ops.hpp>
#include <crd/ceir/plan.hpp>
#include <crd/ceir/provenance.hpp>
#include <crd/containers/string.hpp>
#include <crd/gpu/identity_registry.hpp>
#include <crd/gpu/vulkan_compute_context.hpp>
#include <crd/gpu/vulkan_context.hpp>
#include <crd/gpu/vulkan_validation_capture.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/memory/allocators/growable_tlsf_allocator.hpp>
#include <crd/memory/diagnostic_allocator.hpp>
#include <crd/perf/diag_assets.hpp>
#include <crd/perf/diagnostic_host.hpp>
#include <crd/perf/diagnostics.hpp>
#include <crd/perf/flight_triggers.hpp>
#include <crd/platform/filesystem.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <span>
#include <thread>

#if CRD_DIAG_HAS_TSAN
// crash: the host's crash handler must receive the fault. ThreadSanitizer otherwise handles SIGSEGV itself and exits
// with code 66 before that handler runs (as in crash_capture_specimen).
extern "C" const char* __tsan_default_options()
{
    return "handle_segv=0";
}
#endif

namespace
{
namespace cp   = crd::perf;
namespace cont = crd::containers; // CRD_DIAG_INVARIANT names cont::StringView

constexpr cp::EventCode kOrderEvent      = 0x0042U;
constexpr cp::EventCode kStaleWriteEvent = 0x0043U;

// use-after-free: the violation handler records the write with its sites as program counters, which the bundle's
// module list resolves offline.
struct StaleWrite
{
    crd::memory::DiagnosticAllocator* heap     = nullptr;
    cp::FlightRecorder*               recorder = nullptr;
};

void on_violation(const crd::memory::Violation& v, void* user)
{
    const auto* const watch    = static_cast<const StaleWrite*>(user);
    void*             alloc_pc = nullptr;
    void*             free_pc  = nullptr;
    (void)watch->heap->stack_frames(v.alloc_stack, &alloc_pc, 1U);
    (void)watch->heap->stack_frames(v.free_stack, &free_pc, 1U);
    char text[64];
    (void)std::snprintf(text, sizeof(text), "an order was written after free at byte %zu", v.byte_offset);
    watch->recorder->record(kStaleWriteEvent, cp::Severity::DeveloperAssertion, cont::StringView{text},
                            static_cast<crd::u64>(reinterpret_cast<std::uintptr_t>(alloc_pc)),
                            static_cast<crd::u64>(reinterpret_cast<std::uintptr_t>(free_pc)));
}

void use_after_free(cp::DiagnosticHost& host, bool repaired)
{
    crd::memory::GrowableTlsfAllocator orders{crd::usize{64} << 10U, nullptr, "orders"};
    crd::memory::DiagnosticConfig      cfg;
    cfg.quarantine_capacity_bytes = 4096U; // a released order stays quarantined, so a later write is still seen
    crd::memory::DiagnosticAllocator heap{&orders, cfg, nullptr, "orders-diagnostic"};
    StaleWrite                       watch{&heap, &host.recorder()};
    heap.set_violation_handler(&on_violation, &watch);

    auto* const order = static_cast<crd::u32*>(heap.allocate(4U * sizeof(crd::u32), alignof(crd::u32)));
    order[0]          = 7U;
    heap.deallocate(order);
    if (!repaired)
    {
        *static_cast<volatile crd::u32*>(order) = 8U; // the defect: the order is updated after its release
    }
    CRD_DIAG_INVARIANT(heap.check_all_redzones() == 0U, 0x0712U, "a released order was written after free");
}

// wait and race: the jobs see one of these through JobDecl::data.
struct Shared
{
    bool                             repaired = false;
    std::atomic<crd::jobs::Counter*> first{nullptr};
    std::atomic<crd::jobs::Counter*> second{nullptr};
    int                              plain = 0; // race: the defect adds to this with no ordering
    std::atomic<int>                 counted{0};
};

crd::jobs::Counter* published(const std::atomic<crd::jobs::Counter*>& c)
{
    crd::jobs::Counter* p = c.load(std::memory_order_acquire);
    while (p == nullptr)
    {
        std::this_thread::yield(); // keeps the worker executing until main publishes the counter
        p = c.load(std::memory_order_acquire);
    }
    return p;
}

void first_job(void* data) noexcept
{
    crd::jobs::wait(published(static_cast<Shared*>(data)->second));
}

void second_job(void* data) noexcept
{
    auto* const s = static_cast<Shared*>(data);
    if (!s->repaired)
    {
        crd::jobs::wait(published(s->first)); // the defect: each job waits for the other
    }
}

void race_job(void* data) noexcept
{
    auto* const s = static_cast<Shared*>(data);
    for (int i = 0; i < 200000; ++i)
    {
        if (s->repaired)
        {
            s->counted.fetch_add(1, std::memory_order_relaxed);
        }
        else
        {
            s->plain += 1;
        }
    }
}

void start_jobs(const cp::DiagnosticHost& host)
{
    crd::jobs::Config jobs;
    jobs.num_threads = 3U; // main plus two workers, so the two jobs run at once
    host.configure_jobs(jobs);
    crd::jobs::init(jobs);
}

void wait_cycle(cp::DiagnosticHost& host, Shared& s)
{
    start_jobs(host);
    crd::jobs::JobDecl a{};
    a.fn   = &first_job;
    a.data = &s;
    crd::jobs::JobDecl b{};
    b.fn   = &second_job;
    b.data = &s;
    s.first.store(crd::jobs::run(a), std::memory_order_release);
    s.second.store(crd::jobs::run(b), std::memory_order_release);
    if (s.repaired)
    {
        crd::jobs::wait(s.first.load(std::memory_order_acquire)); // the first job released the second's counter
        crd::jobs::shutdown();
        return;
    }
    // The parked jobs never finish, so the pool cannot shut down: once the hang bundle is written the run ends here.
    for (crd::u32 tick = 0U; tick < 2000U && host.bundles_written() == 0U; ++tick)
    {
        host.recorder().record(kOrderEvent, cp::Severity::Info, cont::StringView{"waiting"}, tick, 0U);
        (void)host.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    std::printf("CRD_HOST_HANG bundles=%llu\n", static_cast<unsigned long long>(host.bundles_written()));
    std::fflush(stdout);
    std::_Exit(host.bundles_written() > 0U ? 5 : 6);
}

void job_race(const cp::DiagnosticHost& host, Shared& s)
{
    start_jobs(host);
    crd::jobs::JobDecl jobs[2] = {};
    for (crd::jobs::JobDecl& j : jobs)
    {
        j.fn   = &race_job;
        j.data = &s;
    }
    crd::jobs::wait(crd::jobs::run(std::span<const crd::jobs::JobDecl>{jobs}));
    crd::jobs::shutdown();
    std::printf("CRD_HOST_TALLY %d\n", s.repaired ? s.counted.load() : s.plain);
}

// crash: the order lookup finds nothing and the defect writes through the null result. Both pointers are volatile, so
// the store reaches the hardware as written (see crash_capture_specimen's null_write).
void write_order(bool repaired)
{
    volatile crd::u32* volatile order = nullptr;
    if (!repaired)
    {
        *order = 8U;
    }
}

// program: the authored program is read from beside the profile and registered under its file name, so the trigger
// names the asset its author edits. Returns 0 when the program ran (its result printed), 7 once its fault's bundle is
// written and 8 when the program could not be read, cooked, loaded or compiled.
constexpr cp::EventCode kProgramFaultCode = 0x0713U;
constexpr const char*   kProgramFile      = "orders.ceir";

void register_dialects(crd::ceir::Context& ctx)
{
    (void)crd::ceir::arith::register_arith_ops(ctx);
    (void)crd::ceir::core::register_core_ops(ctx);
    (void)crd::ceir::func::register_dialect(ctx);
}

int run_program(cp::DiagnosticHost& host, const char* profile_path)
{
    namespace fs   = crd::platform::fs;
    namespace ceir = crd::ceir;
    const fs::Path         profile{profile_path};
    const cont::StringView given = profile.generic();
    crd::usize             dir   = given.size();
    while (dir > 0U && given[dir - 1U] != '/')
    {
        --dir;
    }
    const fs::Path source = dir == 0U ? fs::Path{kProgramFile} : fs::Path{given.substr(0U, dir)} / kProgramFile;
    crd::memory::GrowableTlsfAllocator arena{crd::usize{4} << 20U, nullptr, "orders-program"};
    cont::String                       text{&arena};
    if (!fs::read_file_text(source, text))
    {
        std::printf("CRD_HOST_PROGRAM_UNREADABLE\n");
        return 8;
    }
    ceir::Context cook_ctx{&arena};
    register_dialects(cook_ctx);
    const ceir::cook::CookResult cooked = ceir::cook::cook_program_text(
        cook_ctx, cont::StringView{text.data(), text.size()}, kProgramFile, 0x00A20012U, &arena, &arena);
    ceir::Context run_ctx{&arena};
    register_dialects(run_ctx);
    const ceir::cook::ReadResult loaded =
        cooked.ok() ? ceir::cook::read_program(run_ctx, {cooked.blob.data(), cooked.blob.size()}, &arena)
                    : ceir::cook::ReadResult{&arena};
    if (!loaded.ok() || loaded.module == nullptr)
    {
        std::printf("CRD_HOST_PROGRAM_REFUSED\n");
        return 8;
    }
    const ceir::plan::CompileResult compiled = ceir::plan::compile(run_ctx, *loaded.module, "main", &arena);
    if (!compiled.ok())
    {
        std::printf("CRD_HOST_PROGRAM_REFUSED\n");
        return 8;
    }
    const ceir::plan::RunResult ran = ceir::plan::run(compiled.plan, {}, &arena);
    if (ran.ok())
    {
        std::printf("CRD_HOST_PROGRAM result=%lld\n",
                    ran.values.size() == 1U ? static_cast<long long>(ran.values[0]) : -1LL);
        return 0;
    }
    // The defect: the plan names the faulting instruction, whose provenance rode the cooked blob to the authored op.
    const ceir::Provenance    where = ceir::plan::instr_provenance(compiled.plan, ran.fault);
    const ceir::Origin* const at    = where.primary();
    cont::String              message{&arena};
    message.append(ceir::plan::run_error_name(ran.error));
    message.append(" at ");
    message.append(ceir::render_provenance(run_ctx, where, &arena));
    cp::FlightTriggerInput trigger;
    trigger.kind        = cp::FlightTrigger::Manual;
    trigger.code        = kProgramFaultCode;
    trigger.message     = cont::StringView{message.data(), message.size()};
    trigger.value_a     = static_cast<crd::u64>(ran.error);
    trigger.value_b     = where.op.value;
    trigger.source.file = at != nullptr ? run_ctx.file_path(at->loc.file_id) : cont::StringView{};
    trigger.source.line = at != nullptr ? at->loc.line : 0U;
    (void)host.recorder().trigger(trigger);
    host.stop(); // delivers the open capture, which the host writes as a bundle
    std::printf("CRD_HOST_PROGRAM_FAULT bundles=%llu\n", static_cast<unsigned long long>(host.bundles_written()));
    return 7;
}

// gpu: the detector is the Vulkan validation layer's synchronization validation, read through the engine's capture;
// the application records its upload through the public compute recorder inside a debug label carrying its own Pass
// identity. Returns 0 when the upload validated clean, 9 once the hazard's bundle is written, 10 when this host
// cannot run the detector and 11 when the repaired upload is not clean.
constexpr cp::EventCode kGpuHazardCode = 0x0714U;
constexpr crd::u64      kLedgerBytes   = 256U;

crd::u64 packed(const crd::gpu::ObjectIdentity& id)
{
    return (static_cast<crd::u64>(id.index) << 32U) | id.generation;
}

int gpu_hazard(cp::DiagnosticHost& host, bool repaired)
{
    namespace gpu = crd::gpu;
    gpu::GpuContextConfig cfg;
    cfg.backend                = gpu::GpuBackend::Vulkan;
    cfg.headless               = true;
    cfg.enable_validation      = true;
    cfg.enable_sync_validation = true;
    const std::unique_ptr<gpu::IGpuContext> ctx = gpu::create_vulkan_gpu_context(cfg);
    const char*                             absent = nullptr;
    if (ctx == nullptr || !ctx->valid())
    {
        absent = "no-vulkan-device";
    }
    else if (gpu::validation_layer_spec_version() == 0U)
    {
        absent = "no-validation-layer";
    }
    else if (!ctx->validation_activation().is_active(gpu::ValidationMode::Synchronization))
    {
        absent = "no-synchronization-validation";
    }
    if (absent != nullptr)
    {
        std::printf("CRD_HOST_GPU_ABSENT %s\n", absent);
        return 10;
    }
    auto&                     vk = static_cast<gpu::VulkanGpuContext&>(*ctx);
    gpu::ValidationCapture    capture(vk);
    gpu::VulkanComputeContext compute(vk, nullptr);
    const auto                staging =
        compute.create_buffer(kLedgerBytes, gpu::compute_usage::transfer_src, gpu::ComputeMemory::CpuToGpu);
    const auto ledger = compute.create_buffer(
        kLedgerBytes, gpu::compute_usage::storage | gpu::compute_usage::transfer_dst, gpu::ComputeMemory::GpuOnly);
    const gpu::ObjectIdentity upload = gpu::identity_registry().mint(gpu::ObjectKind::Pass);
    char                      token[gpu::kObjectIdentityBufferSize];
    (void)gpu::encode(upload, token, sizeof(token));
    std::printf("CRD_HOST_GPU_PASS %s\n", token);
    std::memset(staging->map(), 0x5A, kLedgerBytes);
    staging->unmap();

    gpu::ComputeRecorder& rec      = compute.begin();
    const bool            labelled = rec.begin_label(upload, cont::StringView{"orders upload"});
    rec.copy(*staging, *ledger, 0U, 0U, kLedgerBytes / 2U);
    if (repaired)
    {
        rec.barrier(*ledger, gpu::ComputeAccess::TransferDst, gpu::ComputeAccess::TransferDst);
    }
    rec.copy(*staging, *ledger, kLedgerBytes / 4U, kLedgerBytes / 4U, kLedgerBytes / 2U); // overlaps the first copy
    if (labelled)
    {
        rec.end_label();
    }
    compute.submit_and_wait();

    const gpu::ValidationMessage* hazard = nullptr;
    for (const gpu::ValidationMessage& m : capture.messages())
    {
        const cont::StringView name{m.message_id_name.data(), m.message_id_name.size()};
        if (hazard == nullptr && name == "SYNC-HAZARD-WRITE-AFTER-WRITE" &&
            m.identity.kind == gpu::ObjectKind::Resource && gpu::identity_registry().alive(m.identity))
        {
            hazard = &m;
        }
    }
    const gpu::ValidationReport report = capture.report();
    if (hazard == nullptr || report.dropped != 0U)
    {
        if (repaired && !report.clean())
        {
            std::printf("CRD_HOST_GPU_UNCLEAN errors=%llu warnings=%llu dropped=%llu\n",
                        static_cast<unsigned long long>(report.error), static_cast<unsigned long long>(report.warning),
                        static_cast<unsigned long long>(report.dropped));
            return 11;
        }
        return 0; // faulty: a missed or incomplete detection, which the parent reads from the missing clean line
    }
    char buffer[gpu::kObjectIdentityBufferSize];
    char site[gpu::kObjectIdentityBufferSize] = "no-label";
    (void)gpu::encode(hazard->identity, buffer, sizeof(buffer));
    (void)gpu::encode(hazard->label, site, sizeof(site));
    char message[160];
    (void)std::snprintf(message, sizeof(message), "%s on %s in %s", hazard->message_id_name.c_str(), buffer, site);
    cp::FlightTriggerInput trigger;
    trigger.kind    = cp::FlightTrigger::Manual;
    trigger.code    = kGpuHazardCode;
    trigger.message = cont::StringView{message};
    trigger.value_a = packed(hazard->identity);
    trigger.value_b = packed(hazard->label);
    (void)host.recorder().trigger(trigger);
    host.stop();
    std::printf("CRD_HOST_GPU_HAZARD bundles=%llu\n", static_cast<unsigned long long>(host.bundles_written()));
    return 9;
}

// A refund larger than the payment makes the balance negative.
bool balance_holds(bool repaired)
{
    crd::i64 paid = 0;
    for (crd::i64 i = 0; i < 40; ++i)
    {
        paid += i;
    }
    const crd::i64 refunded = repaired ? paid : paid + 1;
    return paid - refunded >= 0;
}
} // namespace

int main(int argc, char** argv)
{
    crd_diag_harden();
    const bool race = argc >= 3 && std::strcmp(argv[2], "race") == 0;
    // The race is a ThreadSanitizer route: under any other build, ASan included, it is absent.
    const char* sanitizer = CRD_DIAG_SPECIMEN_SANITIZER;
#if !CRD_DIAG_HAS_TSAN
    if (race)
    {
        sanitizer = "none";
    }
#endif
    std::printf("CRD_DIAG_IDENTITY=%s\nCRD_DIAG_SANITIZER=%s\n", CRD_DIAG_SPECIMEN_ID, sanitizer);
    std::fflush(stdout);
    const char* const modes[] = {"invariant", "assert", "use-after-free", "wait", "race", "crash", "program", "gpu"};
    bool              known   = false;
    for (const char* m : modes)
    {
        known = known || (argc >= 3 && std::strcmp(argv[2], m) == 0);
    }
    if (!known || (argc == 4 && std::strcmp(argv[3], "repaired") != 0) || argc > 4)
    {
        std::printf("usage: diag_host_specimen <profile> invariant|assert|use-after-free|wait|race|crash|program|gpu "
                    "[repaired]\n");
        return 2;
    }
    const bool        repaired = argc == 4;
    const char* const mode     = argv[2];

    cp::DiagnosticHostProfile profile;
    cont::String              reason;
    if (!cp::load_diagnostic_host_profile(argv[1], profile, reason))
    {
        std::printf("CRD_HOST_PROFILE_REFUSED %s\n", reason.c_str());
        return 3;
    }
    static cp::DiagnosticHost host; // static: the fatal path runs while main's frame is still live
    const cp::DiagHostStart   start = host.start(profile, reason);
    if (start != cp::DiagHostStart::Started)
    {
        std::printf("CRD_HOST_REFUSED %s %s\n", cp::diag_host_start_name(start).data(), reason.c_str());
        return 4;
    }
    std::printf("CRD_HOST_STARTED crash=%s flight=%s faults=%s\n", cp::diag_mode_state_name(host.modes().crash).data(),
                cp::diag_mode_state_name(host.modes().flight).data(),
                cp::diag_mode_state_name(host.modes().faults).data());
    std::fflush(stdout);
    host.recorder().set_replay_recipe("crd-diag-host-specimen <profile> <mode> [repaired]");

    char text[32];
    for (crd::u32 i = 0U; i < 39U; ++i)
    {
        (void)std::snprintf(text, sizeof(text), "order %u", static_cast<unsigned>(i));
        host.recorder().record(kOrderEvent, cp::Severity::Info, cont::StringView{text}, i, 0U);
    }
    // The last order names its customer: a restricted event, whose text no capture may carry.
    host.recorder().record(kOrderEvent, cp::Severity::Info, cont::StringView{"order 39 for customer-0042"}, 39U, 0U,
                           crd::RedactionClass::Restricted);

    static Shared shared; // static: a parked job still reads it after main's frame is gone
    shared.repaired = repaired;
    if (std::strcmp(mode, "invariant") == 0)
    {
        CRD_DIAG_INVARIANT(balance_holds(repaired), 0x0711U, "the order total went negative");
    }
    else if (std::strcmp(mode, "assert") == 0 && !balance_holds(repaired))
    {
        (void)crd::detail::report_assert_failure("total >= 0", __FILE__, __LINE__, "the order total went negative");
    }
    else if (std::strcmp(mode, "use-after-free") == 0)
    {
        use_after_free(host, repaired);
    }
    else if (std::strcmp(mode, "wait") == 0)
    {
        wait_cycle(host, shared);
    }
    else if (race)
    {
        job_race(host, shared);
    }
    else if (std::strcmp(mode, "crash") == 0)
    {
        write_order(repaired);
    }
    else if (std::strcmp(mode, "program") == 0 || std::strcmp(mode, "gpu") == 0)
    {
        const int ended = std::strcmp(mode, "gpu") == 0 ? gpu_hazard(host, repaired) : run_program(host, argv[1]);
        if (ended != 0)
        {
            std::fflush(stdout);
            return ended;
        }
    }
    if (!repaired)
    {
        // Nothing caught the defect (or the fatal path returned): exit 0 without the clean line, which the parent
        // reads as a missed detection, or as InstrumentAbsent for the race outside ThreadSanitizer.
        return 0;
    }
    (void)host.poll();
    std::printf("CRD_HOST_CLEAN bundles=%llu\n", static_cast<unsigned long long>(host.bundles_written()));
    std::fflush(stdout);
    host.stop();
    return 0;
}
