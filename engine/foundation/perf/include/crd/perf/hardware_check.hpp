#pragma once

// crd-perf -- the hardware check (HW-RESILIENCE.3): the Cerid-free control stress check that `ceridc doctor
// --hardware` and the hardware.check diag command run, so a user, support or an agent can test a machine.
//
// One thread pinned to each logical CPU (crd::pin_current_thread) runs every arm for the same time on all CPUs at
// once, loading the package as a heavy application does. An arm's work is Cerid-free: plain arithmetic and memory
// traffic, each result checked against an answer evaluated at compile time or derived from the work's own position,
// never against Cerid code, so an event is the machine's (the 2026-10-26 host check's reasoning,
// docs/sessions/2026-10-26-catch2-fault-address.md). The arms, in report order:
//
//   reload            the 2026-10-26 control's shape: a walk of 4,096 nodes whose every link, owner and index is
//                     reloaded through volatile getters, each link compared with the node its index names (an integer
//                     division); a wrong link is reported and the walk continues from the right node.
//   compute-<level>   crd/perf/hardware_check_kernels.hpp's integer, logistic-map and divide recurrences on 16 lanes,
//                     once per dispatch level the process runs (portable, then sse2, avx2 and avx512, or neon), each
//                     block of 64 steps compared bit for bit with the answer evaluated at compile time.
//   memory            moving inversions over each CPU's own buffer: a pattern derived from each word's position and
//                     the pass written, read back, inverted and read back.
//   shared            a sequence-locked slot per CPU written by its CPU and read by the next one; a read whose sequence
//                     held but whose payload is not that sequence's is an event (coherence, ordering); each CPU runs on
//                     until it has checked one consistent read.
//
// Each (CPU, arm) reports its blocks, nanoseconds, events and the first event's block, place, value read and value
// expected. A CPU the OS refuses (offline, outside the process's set) is reported unpinned and runs nothing. No event
// over a run is no proof of a sound machine, only of none seen in that time on those CPUs: the report says how long
// each arm ran. Contract: docs/design/runtime-diagnostics.md (HW-RESILIENCE.3); ADR-0133.

#include <crd/containers/array.hpp>
#include <crd/containers/string.hpp>
#include <crd/core/cpu_features.hpp>
#include <crd/core/kernel_dispatch.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>
#include <crd/perf/hardware_check_kernels.hpp>

#include <atomic>
#include <span>

namespace crd::perf
{
namespace cont = crd::containers;

enum class HardwareArmKind : crd::u8
{
    Reload,
    Compute,
    Memory,
    Shared,
};

// One arm: Compute runs at `level`; the others ignore it.
struct HardwareArm
{
    HardwareArmKind kind = HardwareArmKind::Reload;
    crd::CpuLevel level = crd::CpuLevel::Portable;
};

// reload, compute at up to four levels, memory, shared.
inline constexpr crd::u32 kMaxHardwareArms = 7U;

// The arm's report name: "reload", "compute-avx2", "memory", "shared".
[[nodiscard]] const char* hardware_arm_name(HardwareArm arm) noexcept;

struct HardwareArmResult
{
    crd::u64 blocks = 0U;      // blocks checked
    crd::u64 nanoseconds = 0U; // time the arm ran on this CPU
    crd::u64 events = 0U;      // results that differed from their answer
    // The first event: its block, its place (reload: the step; compute: the lane plus 16 for y, 32 for z, 48 for c;
    // memory: the byte offset; shared: the payload word), the value read and the value expected (a double's bits).
    crd::u64 first_block = 0U;
    crd::u64 first_where = 0U;
    crd::u64 first_got = 0U;
    crd::u64 first_want = 0U;
    bool injected = false; // the hardware-check fault seam corrupted a value of this arm on this CPU
};

struct HardwareCpuResult
{
    crd::LogicalCpu cpu;
    bool pinned = false;    // the thread ran there; false: the OS refused the CPU and nothing ran
    crd::u8 core_type = 0U; // CPUID 0x1A read there: 0x20 efficiency, 0x40 performance, 0 when not hybrid
    HardwareArmResult arms[kMaxHardwareArms] = {}; // NOLINT(modernize-avoid-c-arrays): by HardwareCheckReport::arms
};

struct HardwareCheckConfig
{
    crd::u64 nanoseconds_per_arm = 2'000'000'000U; // each arm runs this long on every CPU at once
    crd::u32 memory_mib = 8U;                      // the memory arm's buffer per CPU
    const std::atomic<bool>* cancel = nullptr;     // set: every arm stops at its next block
};

inline constexpr crd::u64 kHardwareCheckMaxNanosecondsPerArm = 3'600'000'000'000U; // an hour
inline constexpr crd::u32 kHardwareCheckMaxMemoryMib = 1024U;

struct HardwareCheckReport
{
    explicit HardwareCheckReport(crd::memory::IAllocator* alloc) : cpus(alloc), reason(alloc) {}

    bool ran = false;       // false: refused before any arm ran, `reason` says why
    bool cancelled = false; // the cancel flag stopped it
    crd::u32 arm_count = 0U;
    HardwareArm arms[kMaxHardwareArms] = {}; // NOLINT(modernize-avoid-c-arrays): the arms in report order
    cont::Array<HardwareCpuResult> cpus;     // every logical CPU of crd::logical_cpu_count, in its order
    crd::u32 cpus_pinned = 0U;
    crd::u64 events = 0U; // over every CPU and arm
    cont::String reason;
};

// The compute arm's tables, one per level this build compiles, portable first.
[[nodiscard]] std::span<const crd::KernelLevel<HardwareCheckKernels>> hardware_check_levels() noexcept;

// Runs the check. Refused, running nothing, when the configuration is out of bounds, the OS offers no affinity, or
// the memory arm's buffers would pass half the machine's memory or cannot be committed.
[[nodiscard]] HardwareCheckReport run_hardware_check(const HardwareCheckConfig& config, crd::memory::IAllocator* alloc);

} // namespace crd::perf
