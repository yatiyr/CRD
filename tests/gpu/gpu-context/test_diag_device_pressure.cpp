// DIAG.10c device fault attribution and the driver-call mode witness, device-free.
// Declared contract (docs/design/runtime-diagnostics.md, DIAG.10c):
// - attribute_device_fault labels a failure an engine defect only with a discriminating reproduction (the same inputs
//   failing on another configuration); a simulated failure is always Injected; otherwise a reading at the failure
//   that is Full or Over (physical memory, an enforced commit limit, the device's local or non-local budget) makes it
//   PressureCoincident, a reproduction that did not fail makes it NotReproduced, and anything else is Undiscriminated.
//   Every expected attribution below is derived by hand from that rule.
// - witness_driver_call puts back a mode the call changed, counts the call and the divergence per kind, keeps the
//   first divergence's fields and label, and returns what the call returned. The counts are process-wide, so each
//   case reads them before and after.

#include <crd/core/budget_pressure.hpp>
#include <crd/core/fp_environment.hpp>
#include <crd/core/host_pressure.hpp>
#include <crd/gpu/device_pressure.hpp>
#include <crd/gpu/driver_witness.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstring>

namespace
{
namespace g = crd::gpu;
using g::DeviceFailureKind;
using g::FaultAttribution;
using g::Reproduction;

crd::BudgetReading bounded(crd::u64 capacity, crd::u64 in_use)
{
    crd::BudgetReading r;
    r.read     = true;
    r.bounded  = true;
    r.capacity = capacity;
    r.in_use   = in_use;
    return r;
}

// Readings at the failure with every judged amount well within its bound.
g::GpuLossPressure calm()
{
    g::GpuLossPressure p;
    p.taken                = true;
    p.loss_sequence        = 7U;
    p.host.physical        = bounded(1000U, 100U);
    p.host.commit          = bounded(2000U, 100U);
    p.host.commit_enforced = true;
    p.device.local         = bounded(500U, 50U);
    p.device.non_local     = bounded(500U, 50U);
    p.device.source        = g::DeviceMemorySource::Dxgi;
    return p;
}

g::DeviceFaultEvidence evidence(DeviceFailureKind failure, Reproduction reproduction, const g::GpuLossPressure& p)
{
    g::DeviceFaultEvidence e;
    e.failure      = failure;
    e.reproduction = reproduction;
    e.pressure     = p;
    return e;
}

// A mode no thread starts in: flush-to-zero set (on x86-64; elsewhere nothing is read and nothing changes).
crd::FpEnvironment flushed()
{
    crd::FpEnvironment e = crd::read_fp_environment();
    e.mxcsr |= crd::kMxcsrFlushToZero;
    return e;
}

crd::u64 calls_of(const g::DriverWitnessReport& r, g::DriverCall c)
{
    return r.calls[static_cast<crd::usize>(c)];
}

crd::u64 divergences_of(const g::DriverWitnessReport& r, g::DriverCall c)
{
    return r.divergences[static_cast<crd::usize>(c)];
}
} // namespace

TEST_CASE("diag10c: a device fault is an engine defect only with a discriminating reproduction",
          "[gpu][pressure][attribution][diag10c]")
{
    const g::GpuLossPressure quiet = calm();
    const DeviceFailureKind  real[] = {DeviceFailureKind::Observed, DeviceFailureKind::EngineForced,
                                       DeviceFailureKind::TimedOut, DeviceFailureKind::Failed};
    for (const DeviceFailureKind kind : real)
    {
        CAPTURE(static_cast<int>(kind));
        CHECK(g::attribute_device_fault(evidence(kind, Reproduction::NotAttempted, quiet))
              == FaultAttribution::Undiscriminated);
        // Failing again on the same adapter does not exclude that adapter.
        CHECK(g::attribute_device_fault(evidence(kind, Reproduction::SameAdapterOnly, quiet))
              == FaultAttribution::Undiscriminated);
        CHECK(g::attribute_device_fault(evidence(kind, Reproduction::NotReproduced, quiet))
              == FaultAttribution::NotReproduced);
        CHECK(g::attribute_device_fault(evidence(kind, Reproduction::OtherConfiguration, quiet))
              == FaultAttribution::EngineDefect);
    }
    // No pressure evidence at all is no excuse either way.
    const g::GpuLossPressure none;
    CHECK(g::attribute_device_fault(evidence(DeviceFailureKind::Observed, Reproduction::NotAttempted, none))
          == FaultAttribution::Undiscriminated);
    CHECK(g::attribute_device_fault(evidence(DeviceFailureKind::Observed, Reproduction::OtherConfiguration, none))
          == FaultAttribution::EngineDefect);
}

TEST_CASE("diag10c: a simulated fault is always Injected and no failure is NoFailure",
          "[gpu][pressure][attribution][diag10c]")
{
    g::GpuLossPressure full = calm();
    full.device.local       = bounded(500U, 600U);
    const Reproduction all[] = {Reproduction::NotAttempted, Reproduction::NotReproduced,
                                Reproduction::SameAdapterOnly, Reproduction::OtherConfiguration};
    for (const Reproduction r : all)
    {
        CAPTURE(static_cast<int>(r));
        CHECK(g::attribute_device_fault(evidence(DeviceFailureKind::Simulated, r, calm()))
              == FaultAttribution::Injected);
        CHECK(g::attribute_device_fault(evidence(DeviceFailureKind::Simulated, r, full)) == FaultAttribution::Injected);
        CHECK(g::attribute_device_fault(evidence(DeviceFailureKind::None, r, full)) == FaultAttribution::NoFailure);
    }
}

TEST_CASE("diag10c: memory at or over a bound at the failure makes it pressure-coincident",
          "[gpu][pressure][attribution][diag10c]")
{
    // Each judged reading at its bound (Full) and past it (Over), one at a time.
    for (crd::u32 which = 0U; which < 4U; ++which)
    {
        for (const crd::u64 extra : {crd::u64{0}, crd::u64{1}})
        {
            CAPTURE(which, extra);
            g::GpuLossPressure p = calm();
            crd::BudgetReading* const target[] = {&p.host.physical, &p.host.commit, &p.device.local,
                                                  &p.device.non_local};
            target[which]->in_use = target[which]->capacity + extra;
            CHECK(g::worst_memory_level(p) == (extra == 0U ? crd::PressureLevel::Full : crd::PressureLevel::Over));
            CHECK(g::attribute_device_fault(evidence(DeviceFailureKind::Observed, Reproduction::NotAttempted, p))
                  == FaultAttribution::PressureCoincident);
            CHECK(g::attribute_device_fault(evidence(DeviceFailureKind::TimedOut, Reproduction::NotReproduced, p))
                  == FaultAttribution::PressureCoincident);
            CHECK(g::attribute_device_fault(evidence(DeviceFailureKind::Observed, Reproduction::SameAdapterOnly, p))
                  == FaultAttribution::PressureCoincident);
            // A reproduction elsewhere outranks the pressure: the inputs fail without it.
            CHECK(g::attribute_device_fault(evidence(DeviceFailureKind::Observed, Reproduction::OtherConfiguration, p))
                  == FaultAttribution::EngineDefect);
        }
    }

    // Near is not at the bound: 900 of 1000.
    g::GpuLossPressure near = calm();
    near.host.physical.in_use = 900U;
    CHECK(g::worst_memory_level(near) == crd::PressureLevel::Near);
    CHECK(g::attribute_device_fault(evidence(DeviceFailureKind::Observed, Reproduction::NotAttempted, near))
          == FaultAttribution::Undiscriminated);

    // An unenforced commit limit is not a bound (Linux heuristic overcommit): a charge past it is not pressure.
    g::GpuLossPressure loose     = calm();
    loose.host.commit_enforced   = false;
    loose.host.commit.in_use     = 5000U;
    CHECK(g::worst_memory_level(loose) == crd::PressureLevel::Within);
    CHECK(g::attribute_device_fault(evidence(DeviceFailureKind::Observed, Reproduction::NotAttempted, loose))
          == FaultAttribution::Undiscriminated);

    // Readings that were never taken, or an unread segment, judge nothing.
    g::GpuLossPressure untaken = calm();
    untaken.taken              = false;
    untaken.device.local       = bounded(500U, 600U);
    CHECK(g::worst_memory_level(untaken) == crd::PressureLevel::Unread);
    CHECK(g::attribute_device_fault(evidence(DeviceFailureKind::Observed, Reproduction::NotAttempted, untaken))
          == FaultAttribution::Undiscriminated);
    g::GpuLossPressure unread  = calm();
    unread.device.non_local    = crd::BudgetReading{};
    unread.device.non_local.in_use = 900000U; // not read: must not count
    CHECK(g::worst_memory_level(unread) == crd::PressureLevel::Within);
}

TEST_CASE("diag10c: loss pressure carries the host reading and the backend's device reading",
          "[gpu][pressure][diag10c]")
{
    g::DeviceMemoryReading device;
    device.source       = g::DeviceMemorySource::VulkanMemoryBudget;
    device.reason       = "";
    device.local        = bounded(4096U, 1024U);
    const g::GpuLossPressure p = g::take_loss_pressure(42U, device);
    CHECK(p.taken);
    CHECK(p.loss_sequence == 42U);
    CHECK(p.device.source == g::DeviceMemorySource::VulkanMemoryBudget);
    CHECK(p.device.local.capacity == 4096U);
    CHECK(p.device.local.in_use == 1024U);
    CHECK_FALSE(p.device.non_local.read);
    const crd::HostMemoryReading host = crd::read_host_memory();
    CHECK(p.host.physical.read == host.physical.read);
    CHECK(p.host.physical.capacity == host.physical.capacity);

    // The default reading says why it is empty instead of reporting zero use.
    const g::DeviceMemoryReading nothing;
    CHECK(nothing.source == g::DeviceMemorySource::Unavailable);
    CHECK_FALSE(nothing.local.read);
    CHECK(std::strlen(nothing.reason) > 0U);
}

TEST_CASE("diag10c: every attribution, source and driver call has a distinct name", "[gpu][pressure][diag10c]")
{
    const FaultAttribution attributions[] = {FaultAttribution::NoFailure,          FaultAttribution::Injected,
                                             FaultAttribution::EngineDefect,       FaultAttribution::PressureCoincident,
                                             FaultAttribution::NotReproduced,      FaultAttribution::Undiscriminated};
    STATIC_REQUIRE(sizeof(attributions) / sizeof(attributions[0]) == g::kFaultAttributionCount);
    for (crd::usize i = 0U; i < g::kFaultAttributionCount; ++i)
    {
        CHECK(std::strcmp(g::fault_attribution_name(attributions[i]), "unknown") != 0);
        for (crd::usize j = i + 1U; j < g::kFaultAttributionCount; ++j)
        {
            CHECK(std::strcmp(g::fault_attribution_name(attributions[i]), g::fault_attribution_name(attributions[j]))
                  != 0);
        }
    }
    CHECK(std::strcmp(g::device_memory_source_name(g::DeviceMemorySource::Unavailable), "unavailable") == 0);
    CHECK(std::strcmp(g::device_memory_source_name(g::DeviceMemorySource::Dxgi), "dxgi") == 0);
    CHECK(std::strcmp(g::device_memory_source_name(g::DeviceMemorySource::VulkanMemoryBudget),
                      "vulkan-memory-budget")
          == 0);
    for (crd::usize i = 0U; i < g::kDriverCallCount; ++i)
    {
        CHECK(std::strcmp(g::driver_call_name(static_cast<g::DriverCall>(i)), "unknown") != 0);
        for (crd::usize j = i + 1U; j < g::kDriverCallCount; ++j)
        {
            CHECK(std::strcmp(g::driver_call_name(static_cast<g::DriverCall>(i)),
                              g::driver_call_name(static_cast<g::DriverCall>(j)))
                  != 0);
        }
    }
}

TEST_CASE("diag10c: the driver witness puts back a mode a call changed and counts it", "[gpu][fp-mode][diag10c]")
{
    const crd::FpEnvironment start = crd::read_fp_environment();
    if (!start.sse_read)
    {
        SUCCEED("this architecture reads no floating-point mode yet");
        return;
    }
    const crd::FpEnvironment       dirty  = flushed();
    const g::DriverWitnessReport   before = g::driver_witness();

    // A clean call: counted, no divergence, its value returned.
    const int value = g::witness_driver_call(g::DriverCall::Submit, "test clean submit", []() { return 17; });
    CHECK(value == 17);
    const g::DriverWitnessReport clean = g::driver_witness();
    CHECK(calls_of(clean, g::DriverCall::Submit) == calls_of(before, g::DriverCall::Submit) + 1U);
    CHECK(divergences_of(clean, g::DriverCall::Submit) == divergences_of(before, g::DriverCall::Submit));

    // A call that leaves flush-to-zero set: the caller's mode comes back and the divergence is counted.
    bool ran = false;
    g::witness_driver_call(g::DriverCall::Present, "test present sets FTZ", [&]() {
        crd::write_fp_environment(dirty);
        ran = true;
    });
    CHECK(ran);
    CHECK(crd::fp_differences(crd::read_fp_environment(), start) == 0U);
    const g::DriverWitnessReport after = g::driver_witness();
    CHECK(calls_of(after, g::DriverCall::Present) == calls_of(before, g::DriverCall::Present) + 1U);
    CHECK(divergences_of(after, g::DriverCall::Present) == divergences_of(before, g::DriverCall::Present) + 1U);
    CHECK(after.total_divergences() == before.total_divergences() + 1U);
    REQUIRE(after.first_operation != nullptr);
    if (before.first_operation == nullptr)
    {
        // This process's first divergence: its fields and label are the ones just made.
        CHECK(std::strcmp(after.first_operation, "test present sets FTZ") == 0);
        CHECK(after.first_call == g::DriverCall::Present);
        CHECK(after.first_fields == static_cast<crd::u32>(crd::FpDifference::FlushToZero));
    }

    // A later divergence counts but does not replace the first one.
    const int code = g::witness_driver_call(g::DriverCall::CompileShader, "test compile sets FTZ", [&]() {
        crd::write_fp_environment(dirty);
        return -3;
    });
    CHECK(code == -3);
    CHECK(crd::fp_differences(crd::read_fp_environment(), start) == 0U);
    const g::DriverWitnessReport last = g::driver_witness();
    CHECK(divergences_of(last, g::DriverCall::CompileShader)
          == divergences_of(after, g::DriverCall::CompileShader) + 1U);
    CHECK(std::strcmp(last.first_operation, after.first_operation) == 0);
    CHECK(last.first_call == after.first_call);
    CHECK(last.first_fields == after.first_fields);

    // The witness never judges the caller's mode: a caller in another mode keeps it across a clean call.
    {
        const crd::ScopedFpEnvironment scoped(dirty);
        g::witness_driver_call(g::DriverCall::CreateDevice, "test clean create", []() {});
        CHECK(crd::fp_differences(crd::read_fp_environment(), dirty) == 0U);
    }
    const g::DriverWitnessReport end = g::driver_witness();
    CHECK(divergences_of(end, g::DriverCall::CreateDevice) == divergences_of(last, g::DriverCall::CreateDevice));
    CHECK(crd::fp_differences(crd::read_fp_environment(), start) == 0U);
}
