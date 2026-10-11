// DIAG.10c -- repeated-reload, warm/cold and shutdown workloads over the resource manager, with seeded read faults.
// Declared contract (docs/design/runtime-diagnostics.md, DIAG.10c): crd::run_workload drives cycles of a fresh
// ResourceManager that mounts a one-mesh pack (setup), a cold first load from disk, then reloads of new content written
// to the same pack, and the manager's destruction (teardown). Every iteration's mesh must carry the content it should:
// the new content after a reload that succeeded, the last good content after one that failed. A variant arms one
// seeded fault plan on the real I/O fault seam (crd-perf's FaultInjector): every injected reload must fail, keep the
// last good mesh, and the next reload must swap the new content in; nothing may be left behind by any cycle. A
// variant with a memory budget below the mesh stays clean and shows the soft budget's overruns in its pressure
// reading, which is where that stressor's effect belongs. Each variant changes one stressor. A shipping build cannot
// arm a plan; there the fault variants assert the refusal. Wall-clock timing is recorded as evidence, never asserted.

#include "fault_pack_support.hpp"
#include "scoped_test_file.hpp"

#include <catch2/catch_test_macros.hpp>

#include <crd/core/budget_pressure.hpp>
#include <crd/core/fault_seam.hpp>
#include <crd/core/lifetime_ledger.hpp>
#include <crd/core/workload_probe.hpp>
#include <crd/memory/allocators/growable_tlsf_allocator.hpp>
#include <crd/perf/fault_injection.hpp>
#include <crd/resources/mesh_resource.hpp>
#include <crd/resources/resource_handle.hpp>
#include <crd/resources/resource_id.hpp>
#include <crd/resources/resource_manager.hpp>

#include <cstring>
#include <memory>
#include <new>

namespace
{
namespace fp = crd_test::fault_pack;
namespace cp = crd::perf;
using crd::FindingAttribution;
using crd::LifetimeVerdict;
using crd::ProgressVerdict;
using crd::Stressor;
using crd::WorkloadVerdict;
using crd::resources::MeshResource;
using crd::resources::MountId;
using crd::resources::ResourceHandle;
using crd::resources::ResourceId;
using crd::resources::ResourceManager;

alignas(crd::memory::GrowableTlsfAllocator) unsigned char s_wl_alloc_buf[sizeof(crd::memory::GrowableTlsfAllocator)];
// Never destroyed: static-destruction-order safe, like the other resource tests' allocators.
crd::memory::GrowableTlsfAllocator& s_wl_alloc = // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)
    *::new (s_wl_alloc_buf) crd::memory::GrowableTlsfAllocator();

constexpr crd::u32 kCycles     = 3U;
constexpr crd::u32 kIterations = 24U;

crd::u64 bits_of(crd::f32 x)
{
    crd::u32 b = 0U;
    std::memcpy(&b, &x, sizeof(b));
    return b;
}

// The mesh content iteration `i` of cycle `c` writes.
crd::f32 content_of(crd::u32 cycle, crd::u32 iteration)
{
    return static_cast<crd::f32>((100U * cycle) + iteration + 1U);
}

struct ReloadWorkload
{
    explicit ReloadWorkload(const crd::platform::fs::Path* pack_path) : path(pack_path) {}

    const crd::platform::fs::Path* path;
    ResourceId                     id       = ResourceId::mint_random();
    bool                           faulty   = false; // a fault variant: `plan` is armed before each injected reload
    cp::FaultPlan                  plan;
    crd::u64                       budget   = 0U; // a memory budget, 0: none
    cp::FaultInjector              injector;

    std::unique_ptr<ResourceManager> rm;
    ResourceHandle<MeshResource>     handle;
    MountId                          mount;
    crd::f32                         current = 0.0F; // the content the mesh should hold
    crd::u64                         overruns = 0U;  // the soft budget's overruns, summed over cycles
    crd::u32                         arm_refusals = 0U;
};

bool reload_setup(void* user, crd::u32 cycle) noexcept
{
    auto* w = static_cast<ReloadWorkload*>(user);
    if (!fp::try_write_pack(&s_wl_alloc, *w->path, w->id, crd::resources::kFourCC_MESH,
                            fp::mesh_artifact(&s_wl_alloc, w->id, content_of(cycle, 0U))))
    {
        return false;
    }
    w->rm = std::make_unique<ResourceManager>(&s_wl_alloc);
    crd::resources::register_mesh_loader(w->rm.get());
    if (w->budget != 0U)
    {
        w->rm->set_memory_budget(w->budget);
    }
    w->mount   = w->rm->mount_manifest(w->path->generic());
    w->current = content_of(cycle, 0U);
    return w->mount.is_valid();
}

void reload_iterate(void* user, crd::u32 cycle, crd::u32 iteration, bool /*injected*/,
                    crd::WorkloadIteration& out) noexcept
{
    auto* w = static_cast<ReloadWorkload*>(user);
    if (iteration == 0U)
    {
        // Cold: the first load reads the pack from disk.
        w->handle = w->rm->load_sync<MeshResource>(w->id);
        out.ok    = w->handle.is_ready();
    }
    else
    {
        const crd::f32 next    = content_of(cycle, iteration);
        const bool     written = fp::try_write_pack(&s_wl_alloc, *w->path, w->id, crd::resources::kFourCC_MESH,
                                                    fp::mesh_artifact(&s_wl_alloc, w->id, next));
        out.ok = written && w->rm->reload_mount_now(w->mount) == 1U;
        if (out.ok)
        {
            w->current = next;
        }
        // Warm: a load of the resident mesh is served without a read and gives the same payload.
        const ResourceHandle<MeshResource> again = w->rm->load_sync<MeshResource>(w->id);
        out.ok = out.ok && again.get() == w->handle.get();
    }
    const MeshResource* const mesh = w->handle.get();
    out.signature                  = mesh != nullptr ? bits_of(fp::first_x(*mesh)) : 0U;
    out.expected                   = bits_of(w->current); // a failed reload keeps the last good content
    out.expected_known             = true;
}

void reload_teardown(void* user, crd::u32 /*cycle*/) noexcept
{
    auto* w = static_cast<ReloadWorkload*>(user);
    w->overruns += w->rm->memory_pressure().overruns;
    w->handle = ResourceHandle<MeshResource>{};
    w->rm->unmount(w->mount);
    w->rm.reset();
}

void reload_arm(void* user, crd::u32 /*cycle*/, crd::u32 /*iteration*/) noexcept
{
    auto* w = static_cast<ReloadWorkload*>(user);
    if (w->injector.arm(w->plan, fp::kInject) != cp::FaultArmStatus::Armed)
    {
        ++w->arm_refusals;
    }
}

crd::u32 reload_disarm(void* user) noexcept
{
    auto*          w     = static_cast<ReloadWorkload*>(user);
    const crd::u64 fired = w->injector.fired(cp::FaultSite::FailedRead) + w->injector.fired(cp::FaultSite::PartialRead);
    w->injector.disarm();
    return static_cast<crd::u32>(fired);
}

crd::WorkloadSpec reload_spec(ReloadWorkload& w, crd::u64 fault_plan_id)
{
    crd::WorkloadSpec spec;
    spec.name       = "resource-reload";
    spec.cycles     = kCycles;
    spec.iterations = kIterations;
    spec.user       = &w;
    spec.setup      = &reload_setup;
    spec.iterate    = &reload_iterate;
    spec.teardown   = &reload_teardown;
    spec.stall_ns   = 5000ULL * 1000ULL * 1000ULL; // five seconds: far above a cycle's setup or a reload anywhere
    if (w.faulty)
    {
        spec.arm            = &reload_arm;
        spec.disarm         = &reload_disarm;
        spec.fault_seed     = fault_plan_id;
        spec.fault_permille = 1000U; // every odd iteration but the last
    }
    spec.stressors.set(Stressor::Cycles, kCycles);
    spec.stressors.set(Stressor::Iterations, kIterations);
    spec.stressors.set(Stressor::ReloadCadence, 1U);
    spec.stressors.set(Stressor::FaultPlan, fault_plan_id);
    spec.stressors.set(Stressor::MemoryBudget, w.budget);
    return spec;
}

crd::WorkloadResult run(const crd::WorkloadSpec& spec)
{
    crd::WorkloadResult r;
    crd::run_workload(spec, r);
    char line[512];
    (void)crd::format_workload_result(spec.name, r, line, sizeof(line));
    WARN(line); // evidence: the timing verdict is printed on every run and judged by nothing
    return r;
}

void check_clean(const crd::WorkloadResult& r)
{
    CHECK(r.verdict == WorkloadVerdict::Clean);
    CHECK(r.cycles_run == kCycles);
    CHECK(r.iterations_run == static_cast<crd::u64>(kCycles) * kIterations);
    CHECK(r.diverged == 0U);
    CHECK(r.worst_cycle.verdict == LifetimeVerdict::Balanced);
    CHECK(r.progress.verdict == ProgressVerdict::Progressing);
}
} // namespace

TEST_CASE("diag10c: repeated reloads keep each cycle's content and lifetimes, and recover from every injected fault",
          "[resources][workload][fault][diag10c]")
{
    const ResourceId     pack_id = ResourceId::mint_random();
    const ScopedTestFile pack(fp::temp_pack_path(&s_wl_alloc, "workload_reload_", pack_id));

    ReloadWorkload            base(&pack.path());
    const crd::WorkloadResult baseline = run(reload_spec(base, 0U));
    check_clean(baseline);
    CHECK(baseline.injected_iterations == 0U);
    CHECK(baseline.warm_count == kCycles * (kIterations - 1U));
    CHECK(base.overruns == 0U);

    // One changed stressor: a memory budget below one mesh. The soft budget never refuses, so the run stays clean;
    // its effect is the overruns its pressure reading counts.
    ReloadWorkload tight(&pack.path());
    tight.budget                    = 1U;
    const crd::WorkloadResult small = run(reload_spec(tight, 0U));
    check_clean(small);
    CHECK(tight.overruns > 0U);
    crd::AttributionReport a = crd::attribute_finding(baseline, small);
    CHECK(a.comparison.changed == Stressor::MemoryBudget);
    CHECK(a.attribution == FindingAttribution::NoNewFinding);

    // One changed stressor at a time: a failed read, then a short read, of every injected reload's pack.
    const char* const plans[2] = {"seed=11;failed-read@0+1%1000000", "seed=12;partial-read@0+1%1000000=16"};
    for (crd::u32 p = 0U; p < 2U; ++p)
    {
        INFO("plan " << plans[p]);
        ReloadWorkload faulty(&pack.path());
        faulty.faulty = true;
        faulty.plan   = fp::plan_of(plans[p]);
        const fp::ScopedIoHook hook(faulty.injector);
        const crd::WorkloadResult r = run(reload_spec(faulty, p + 1U));
        a                           = crd::attribute_finding(baseline, r);
        CHECK(a.comparison.changed == Stressor::FaultPlan);
        // Eleven injected reloads a cycle: iterations 1, 3, ..., 21.
        CHECK(r.injected_iterations == static_cast<crd::u64>(kCycles) * 11U);
        if (!crd::fault_seams_compiled())
        {
            // A shipping build refuses every arm and every reload proceeds: the injection proves nothing there.
            CHECK_FALSE(hook.installed);
            CHECK(faulty.arm_refusals == kCycles * 11U);
            CHECK(r.verdict == WorkloadVerdict::FaultNotFired);
            CHECK(a.attribution == FindingAttribution::Attributed);
            continue;
        }
        REQUIRE(hook.installed);
        check_clean(r);
        CHECK(faulty.arm_refusals == 0U);
        CHECK(r.faults_fired == r.injected_iterations);
        CHECK(r.detected == r.injected_iterations);
        CHECK(r.recovered == r.injected_iterations);
        CHECK(a.attribution == FindingAttribution::NoNewFinding);
    }
}
