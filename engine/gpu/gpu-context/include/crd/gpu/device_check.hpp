#pragma once

// crd-gpu-context -- the GPU device checks (HW-RESILIENCE.3).
//
// The CPU's kernel families check their tables before first use (crd/core/kernel_dispatch.hpp); a GPU device path
// gets the same treatment. Before a backend hands out a device on an adapter, it runs a known-answer compute kernel
// (crd/kir/ckir_device_check.hpp, lowered by the backend's own emitter and compiler) on a separate probe device of that
// adapter, so the device the caller receives carries none of the check's work. An adapter whose results differ bit for
// bit is passed over for the backend's next adapter in its order of preference; when none passes the factory returns
// no device, which every consumer already handles as "no GPU: use the CPU path". The process keeps running on the
// path that passed, and the log below says which adapter failed, in which part, and what the backend runs instead.
//
// An adapter is checked at most once per process (two identical GPUs are two adapters: the ordinal tells them apart),
// so only the first device on each adapter pays for it. The device-check fault seam (crd/core/fault_seam.hpp) fails a
// named adapter's check, so tests drive the fallback on a sound GPU. A host prints the demotions where it starts its
// diagnostics (report_device_demotions) and `gpu.resources` (crd-perf-gpu-bridge) reports every check and selection.
//
// Backends with the check: Vulkan (create_vulkan_gpu_context). Contract: docs/design/runtime-diagnostics.md.

#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/gpu/adapter.hpp>
#include <crd/gpu/compute.hpp>

#include <cstdio>
#include <mutex>

namespace crd::gpu
{

enum class DeviceCheckStatus : crd::u8
{
    NotRun,         // the backend has chosen no adapter
    Passed,         // the preferred adapter passed and runs
    Unchecked,      // the preferred adapter runs without a check: the check could not run here (no shader compiler)
    Demoted,        // the preferred adapter failed; a later one passed (or runs unchecked) and runs
    NoDevicePassed, // the preferred adapter failed and no other gave a device that passed: the backend gives no device
};

[[nodiscard]] const char* device_check_status_name(DeviceCheckStatus status) noexcept;

// What a backend's check found: the first failing part, or why the check could not run on this machine (the host
// side cannot build the kernel: no shader compiler). Not running is no evidence against the device, which is then
// used unchecked and reported so; both null when it passed.
struct DeviceCheckOutcome
{
    const char* failed = nullptr;
    const char* unchecked = nullptr;
};

// One adapter's check.
struct DeviceCheck
{
    GpuAdapter adapter;
    crd::u32 ordinal = 0U; // the adapter's place in its backend's enumeration
    // nullptr when it passed; else the static name of the first failing part: "device" (the probe device or its compute
    // queue did not open), "injected" (the fault seam), or what run_device_check names
    const char* failed = nullptr;
    bool injected = false;           // that failure was the device-check fault seam's
    const char* unchecked = nullptr; // why the check could not run (the adapter is used unchecked), static
    crd::u64 nanoseconds = 0U;       // how long the check took
};

// A backend's choice of adapter.
struct DeviceSelection
{
    DeviceCheckStatus status = DeviceCheckStatus::NotRun;
    GpuAdapter selected;             // the adapter the backend prefers
    GpuAdapter runs;                 // the adapter its devices run on (unknown for NoDevicePassed)
    const char* failed = nullptr;    // the selected adapter's first failing part (Demoted, NoDevicePassed)
    bool injected = false;           // that failure was the device-check fault seam's
    const char* unchecked = nullptr; // why the adapter it runs was not checked (Unchecked, or Demoted onto it)
    crd::u32 adapters_failed = 0U;   // adapters the backend passed over, the selected one included
};

// The process's checks and each backend's latest selection. Thread-safe; the listener runs under the log's lock, so
// it must not note anything itself.
class DeviceChecks
{
public:
    static constexpr crd::u32 kMaxChecks = 64U;    // every adapter of every backend
    static constexpr crd::u32 kMaxSelections = 8U; // one per backend
    using Listener = void (*)(const DeviceSelection& selection);

    DeviceChecks() = default;

    DeviceChecks(const DeviceChecks&) = delete;
    DeviceChecks& operator=(const DeviceChecks&) = delete;
    DeviceChecks(DeviceChecks&&) = delete;
    DeviceChecks& operator=(DeviceChecks&&) = delete;

    // The check of the adapter at `ordinal` that is `adapter`, when the process ran it.
    [[nodiscard]] bool find(const GpuAdapter& adapter, crd::u32 ordinal, DeviceCheck& out) const noexcept;
    // Note `check` (kept once per adapter and ordinal). False when it is new and the log is full.
    bool note_check(const DeviceCheck& check) noexcept;

    // Note the selection of the backend `selection.selected.backend` names. The listener hears it when it differs from
    // that backend's previous selection.
    void note_selection(const DeviceSelection& selection) noexcept;
    // Send every noted selection, then each new one, to `listener` (nullptr stops sending).
    void listen(Listener listener) noexcept;

    [[nodiscard]] crd::u32 check_count() const noexcept;
    [[nodiscard]] DeviceCheck check_at(crd::u32 index) const noexcept;
    [[nodiscard]] crd::u32 selection_count() const noexcept;
    [[nodiscard]] DeviceSelection selection_at(crd::u32 index) const noexcept;

    // Drop every check and selection, keeping the listener: a test starts from an unchecked process.
    void forget() noexcept;

private:
    mutable std::mutex m_mutex;
    DeviceCheck m_checks[kMaxChecks]{};
    crd::u32 m_check_count = 0U;
    DeviceSelection m_selections[kMaxSelections]{};
    crd::u32 m_selection_count = 0U;
    Listener m_listener = nullptr;
};

// The process's log; every backend notes into it.
[[nodiscard]] DeviceChecks& device_checks() noexcept;

// The check of the adapter at `ordinal` that is `adapter`: the logged one when the process ran it, else the
// device-check fault seam's failure ("injected") or what `run(user)` found, timed and logged. Checks run one at a time,
// so two threads opening the same adapter check it once.
[[nodiscard]] DeviceCheck check_device_once(const GpuAdapter& adapter, crd::u32 ordinal,
                                            DeviceCheckOutcome (*run)(void* user), void* user);

// "[crd] GPU device path <backend> on <selected> failed its known answers in <part>[ (fault seam)]; it runs <runs |
// no device>[ unchecked (<why>)] (<status>, <n> adapters failed)", or for Unchecked "[crd] GPU device path <backend>
// on <selected> ran no known-answer check (<why>); it runs unchecked (unchecked, 0 adapters failed)", written into
// `out` (at most `bytes`, terminated). Returns the length the full line needs, as snprintf does.
crd::usize format_device_demotion(const DeviceSelection& selection, char* out, crd::usize bytes) noexcept;

// From now on write format_device_demotion's line to `out` (nullptr: nowhere) for every selection that does not run its
// selected adapter checked, those noted before included. A host calls it once, where it starts its diagnostics, as it
// calls crd::perf::report_kernel_self_checks for the CPU's kernel families.
void report_device_demotions(std::FILE* out) noexcept;

// The program a check runs: one f32 input buffer at binding 0, `floats` f32 outputs at binding 1 and `words` u32
// outputs at binding 2 (crd/kir/ckir_device_check.hpp's layout), over `workgroups` workgroups; `verify` names the first
// failing part of the outputs, or returns nullptr.
struct DeviceCheckProgram
{
    crd::containers::ConstSpan<float> input;
    crd::u32 floats = 0U;
    crd::u32 words = 0U;
    crd::u32 workgroups = 1U;
    const char* (*verify)(const float* floats, const crd::u32* words) noexcept = nullptr;
};

// Run `program` through `pipeline` on `compute` and return nullptr when it reproduced its known answers, else the
// first failing part: "build" (`pipeline` is null: the kernel did not lower or compile), "buffers" (a buffer could not
// be created or mapped), or what `verify` names.
[[nodiscard]] const char* run_device_check(IComputeContext& compute, ComputePipeline* pipeline,
                                           const DeviceCheckProgram& program);

} // namespace crd::gpu
