// crd-perf -- the hardware check (HW-RESILIENCE.3). See crd/perf/hardware_check.hpp.

#include <crd/core/fault_seam.hpp>
#include <crd/core/kernel_dispatch.hpp>
#include <crd/core/kernel_level.hpp>
#include <crd/core/platform.hpp>
#include <crd/core/seed_generators.hpp>
#include <crd/perf/hardware_check.hpp>
#include <crd/perf/hardware_check_kernels.hpp>
#include <crd/perf/hardware_identity.hpp>
#include <crd/time/clocks.hpp>
#include <crd/vm/virtual_memory.hpp>

#include <atomic>
#include <bit>
#include <new>
#include <thread>

namespace crd::perf
{
namespace
{

// CRD_KERNELS_HWCHECK: the levels crd_add_cpu_kernels built (../CMakeLists.txt); each needs its entry below.
constexpr int kLevelsWithBody = (1 << CRD_KERNEL_LEVEL_PORTABLE) | (1 << CRD_KERNEL_LEVEL_SSE2) |
                                (1 << CRD_KERNEL_LEVEL_AVX2) | (1 << CRD_KERNEL_LEVEL_AVX512) |
                                (1 << CRD_KERNEL_LEVEL_NEON);
static_assert((CRD_KERNELS_HWCHECK & ~kLevelsWithBody) == 0, "a built level has no table entry here");
static_assert(CRD_KERNEL_BUILT(CRD_KERNELS_HWCHECK, PORTABLE), "the portable level is always built");

constexpr crd::KernelLevel<HardwareCheckKernels> kComputeLevels[] = {
    {crd::CpuLevel::Portable, &kHardwareCheckKernelsPortable, {}},
#if CRD_KERNEL_BUILT(CRD_KERNELS_HWCHECK, SSE2)
    {crd::CpuLevel::Sse2, &kHardwareCheckKernelsSse2, {}},
#endif
#if CRD_KERNEL_BUILT(CRD_KERNELS_HWCHECK, AVX2)
    {crd::CpuLevel::Avx2, &kHardwareCheckKernelsAvx2, {}},
#endif
#if CRD_KERNEL_BUILT(CRD_KERNELS_HWCHECK, AVX512)
    {crd::CpuLevel::Avx512, &kHardwareCheckKernelsAvx512, {}},
#endif
#if CRD_KERNEL_BUILT(CRD_KERNELS_HWCHECK, NEON)
    {crd::CpuLevel::Neon, &kHardwareCheckKernelsNeon, {}},
#endif
};
static_assert(std::size(kComputeLevels) + 3U <= kMaxHardwareArms, "every arm has a report slot");

// ---- compute: the recurrence of hardware_check_kernels.hpp evaluated at compile time --------------------------------

constexpr crd::u32 kComputeSteps = 64U;
constexpr crd::u32 kLanes = kHardwareComputeLanes;

constexpr HardwareComputeState compute_seed() noexcept
{
    HardwareComputeState s{};
    for (crd::u32 l = 0U; l < kLanes; ++l)
    {
        s.x[l] = 0x0123456789ABCDEFULL * (l + 1U);
        s.y[l] = static_cast<double>(l + 1U) / 17.0;
        s.z[l] = 1.0 + static_cast<double>(l) / 8.0;
        s.c[l] = l;
    }
    return s;
}

// Each step exactly as the kernels take it: one IEEE operation at a time, no fused multiply-add.
constexpr HardwareComputeState compute_answer() noexcept
{
    HardwareComputeState s = compute_seed();
    for (crd::u32 l = 0U; l < kLanes; ++l)
    {
        for (crd::u32 step = 0U; step < kComputeSteps; ++step)
        {
            s.x[l] = (s.x[l] & 0xFFFFFFFFULL) * 0x9E3779B1ULL + ((s.x[l] >> 29U) ^ (s.x[l] << 7U));
            s.y[l] = 3.99 * (s.y[l] * (1.0 - s.y[l]));
            s.z[l] = (s.y[l] + 3.0) / (s.z[l] + 1.0);
            s.c[l] = ((s.c[l] << 1U) | (s.c[l] >> 63U)) ^
                     (std::bit_cast<crd::u64>(s.y[l]) + std::bit_cast<crd::u64>(s.z[l]));
        }
    }
    return s;
}

// A state's words in report order: x, then y, z (their bits) and c, each by lane.
struct ComputeWords
{
    crd::u64 w[4U * kLanes]; // NOLINT(modernize-avoid-c-arrays)
};

constexpr ComputeWords words_of(const HardwareComputeState& s) noexcept
{
    ComputeWords out{};
    for (crd::u32 l = 0U; l < kLanes; ++l)
    {
        out.w[l] = s.x[l];
        out.w[kLanes + l] = std::bit_cast<crd::u64>(s.y[l]);
        out.w[2U * kLanes + l] = std::bit_cast<crd::u64>(s.z[l]);
        out.w[3U * kLanes + l] = s.c[l];
    }
    return out;
}

constexpr HardwareComputeState kComputeSeed = compute_seed();
constexpr ComputeWords kComputeAnswer = words_of(compute_answer());

// ---- reload: a cycle of nodes walked through volatile owners --------------------------------------------------------

constexpr crd::u64 kNodes = 4096U;
// Hull-Dobell: an odd increment and a multiplier of 1 modulo 4 make i -> (i * a + c) mod 2^12 one cycle of every node.
constexpr crd::u64 kLinkMultiplier = 1029U;
constexpr crd::u64 kLinkIncrement = 17U;

struct Node
{
    Node* volatile next;
    volatile crd::u64 index;
};

struct Owner
{
    Node* volatile nodes;
    volatile crd::u64 count;
};

// The getter chain of the 2026-10-26 control, out of line so every step calls it and reloads what it returns.
CRD_NOINLINE const Owner* owner_of(const Owner* const volatile* slot) noexcept
{
    return *slot;
}

CRD_NOINLINE Node* link_of(const Node* node) noexcept
{
    return node->next;
}

// ---- memory and shared ----------------------------------------------------------------------------------------------

constexpr crd::u32 kPayloadWords = 7U;
constexpr crd::u32 kSharedRounds = 256U; // write and read rounds in one block

// One CPU's slot: its sequence and the payload that sequence names, one cache line.
struct alignas(64) Slot
{
    std::atomic<crd::u64> sequence{0U};
    std::atomic<crd::u64> payload[kPayloadWords] = {}; // NOLINT(modernize-avoid-c-arrays)
};

constexpr crd::u64 payload_word(crd::u64 owner, crd::u64 sequence, crd::u32 word) noexcept
{
    return crd::splitmix64_mix((owner << 40U) ^ (sequence * kPayloadWords + word));
}

// ---- the run --------------------------------------------------------------------------------------------------------

class Barrier
{
public:
    explicit Barrier(crd::u32 count) noexcept : m_count(count) {}

    void wait() noexcept
    {
        const crd::u32 generation = m_generation.load(std::memory_order_acquire);
        if (m_arrived.fetch_add(1U, std::memory_order_acq_rel) + 1U == m_count)
        {
            m_arrived.store(0U, std::memory_order_relaxed);
            m_generation.fetch_add(1U, std::memory_order_release);
            m_generation.notify_all();
            return;
        }
        while (m_generation.load(std::memory_order_acquire) == generation)
        {
            m_generation.wait(generation, std::memory_order_acquire);
        }
    }

private:
    std::atomic<crd::u32> m_arrived{0U};
    std::atomic<crd::u32> m_generation{0U};
    crd::u32 m_count;
};

struct Run
{
    const HardwareCheckConfig* config = nullptr;
    const HardwareCheckReport* report = nullptr;
    HardwareCpuResult* results = nullptr;
    crd::u32 cpus = 0U;
    crd::u64* memory = nullptr; // memory_words per CPU
    crd::usize memory_words = 0U;
    Node* nodes = nullptr; // kNodes per CPU
    Owner* owners = nullptr;
    Slot* slots = nullptr;
    Barrier* barrier = nullptr;
};

[[nodiscard]] crd::u64 now_ns() noexcept
{
    return static_cast<crd::u64>(crd::time::MonotonicClock::now().ns_since_epoch());
}

[[nodiscard]] bool cancelled(const Run& run) noexcept
{
    return run.config->cancel != nullptr && run.config->cancel->load(std::memory_order_acquire);
}

void note(HardwareArmResult& r, crd::u64 block, crd::u64 where, crd::u64 got, crd::u64 want) noexcept
{
    if (r.events++ == 0U)
    {
        r.first_block = block;
        r.first_where = where;
        r.first_got = got;
        r.first_want = want;
    }
}

// The pending corruption of one value an arm reads (the fault seam's answer), spent at its first use.
struct Injection
{
    bool pending = false;

    [[nodiscard]] crd::u64 apply(crd::u64 value) noexcept
    {
        if (!pending)
        {
            return value;
        }
        pending = false;
        return value ^ 1U;
    }
};

void reload_block(const Run& run, crd::u32 cpu, crd::u64 block, Injection& injection, HardwareArmResult& r) noexcept
{
    const Owner* const volatile slot = &run.owners[cpu];
    const Owner* owner = owner_of(&slot);
    Node* cursor = owner->nodes;
    for (crd::u64 step = 0U; step < kNodes; ++step)
    {
        owner = owner_of(&slot);
        Node* const base = owner->nodes;
        const crd::u64 want = (cursor->index * kLinkMultiplier + kLinkIncrement) % owner->count;
        const crd::u64 got = injection.apply(static_cast<crd::u64>(link_of(cursor) - base));
        if (got != want)
        {
            note(r, block, step, got, want);
        }
        cursor = base + want; // never follow a wrong link
    }
}

void compute_block(const HardwareCheckKernels& table, crd::u64 block, Injection& injection,
                   HardwareArmResult& r) noexcept
{
    HardwareComputeState state = kComputeSeed;
    table.compute(state, kComputeSteps);
    const ComputeWords words = words_of(state);
    for (crd::u32 i = 0U; i < 4U * kLanes; ++i)
    {
        const crd::u64 got = injection.apply(words.w[i]);
        if (got != kComputeAnswer.w[i])
        {
            note(r, block, i, got, kComputeAnswer.w[i]);
        }
    }
}

void memory_block(const Run& run, crd::u32 cpu, crd::u64 block, Injection& injection, HardwareArmResult& r) noexcept
{
    crd::u64* const words = run.memory + static_cast<crd::usize>(cpu) * run.memory_words;
    const crd::usize n = run.memory_words;
    const crd::u64 salt = crd::splitmix64_at(cpu, block); // each word's pattern: its position and the pass
    for (crd::u32 inverted = 0U; inverted < 2U; ++inverted)
    {
        const crd::u64 flip = inverted != 0U ? ~crd::u64{0U} : crd::u64{0U};
        for (crd::usize i = 0U; i < n; ++i)
        {
            words[i] = crd::splitmix64_mix(salt + i) ^ flip;
        }
        for (crd::usize i = 0U; i < n; ++i)
        {
            const crd::u64 want = crd::splitmix64_mix(salt + i) ^ flip;
            const crd::u64 got = injection.apply(words[i]);
            if (got != want)
            {
                note(r, block, i * sizeof(crd::u64), got, want);
            }
        }
    }
}

// Returns how many of the block's reads were consistent, and so checked.
crd::u32 shared_block(const Run& run, crd::u32 cpu, crd::u32 neighbour, crd::u64 block, Injection& injection,
                      HardwareArmResult& r) noexcept
{
    crd::u32 checked = 0U;
    Slot& mine = run.slots[cpu];
    const Slot& theirs = run.slots[neighbour];
    for (crd::u32 round = 0U; round < kSharedRounds; ++round)
    {
        const crd::u64 sequence = mine.sequence.load(std::memory_order_relaxed) + 2U;
        mine.sequence.store(sequence - 1U, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        for (crd::u32 w = 0U; w < kPayloadWords; ++w)
        {
            mine.payload[w].store(payload_word(cpu, sequence, w), std::memory_order_relaxed);
        }
        mine.sequence.store(sequence, std::memory_order_release);

        const crd::u64 before = theirs.sequence.load(std::memory_order_acquire);
        crd::u64 read[kPayloadWords] = {}; // NOLINT(modernize-avoid-c-arrays)
        for (crd::u32 w = 0U; w < kPayloadWords; ++w)
        {
            read[w] = theirs.payload[w].load(std::memory_order_relaxed);
        }
        std::atomic_thread_fence(std::memory_order_acquire);
        if ((before & 1U) != 0U || theirs.sequence.load(std::memory_order_relaxed) != before)
        {
            continue; // a write was in progress: no consistent read this round
        }
        ++checked;
        for (crd::u32 w = 0U; w < kPayloadWords; ++w)
        {
            const crd::u64 want = payload_word(neighbour, before, w);
            const crd::u64 got = injection.apply(read[w]);
            if (got != want)
            {
                note(r, block, w, got, want);
            }
        }
    }
    return checked;
}

void worker(const Run& run, crd::u32 cpu) noexcept
{
    HardwareCpuResult& result = run.results[cpu];
    result.pinned = crd::pin_current_thread(result.cpu);
    if (result.pinned)
    {
        result.core_type = crd::decode_cpuid(crd::read_cpuid_here()).core_type;
    }
    run.barrier->wait(); // every CPU's pin is known: the shared arm's ring follows the pinned CPUs
    crd::u32 neighbour = cpu;
    for (crd::u32 step = 1U; step <= run.cpus; ++step)
    {
        const crd::u32 next = (cpu + step) % run.cpus;
        if (run.results[next].pinned)
        {
            neighbour = next;
            break;
        }
    }
    const HardwareCheckReport& report = *run.report;
    for (crd::u32 a = 0U; a < report.arm_count; ++a)
    {
        run.barrier->wait();
        if (!result.pinned)
        {
            continue;
        }
        const HardwareArm arm = report.arms[a];
        HardwareArmResult& r = result.arms[a];
        Injection injection;
        injection.pending = crd::consult_hardware_check_fault(cpu, hardware_arm_name(arm));
        r.injected = injection.pending;
        const HardwareCheckKernels* table = nullptr;
        for (const crd::KernelLevel<HardwareCheckKernels>& level : kComputeLevels)
        {
            if (level.level == arm.level)
            {
                table = level.table;
            }
        }
        const crd::u64 start = now_ns();
        crd::u64 elapsed = 0U;
        bool checked = arm.kind != HardwareArmKind::Shared; // the other arms check every block
        do
        {
            switch (arm.kind)
            {
                case HardwareArmKind::Reload:
                    reload_block(run, cpu, r.blocks, injection, r);
                    break;
                case HardwareArmKind::Compute:
                    compute_block(*table, r.blocks, injection, r);
                    break;
                case HardwareArmKind::Memory:
                    memory_block(run, cpu, r.blocks, injection, r);
                    break;
                case HardwareArmKind::Shared:
                    checked = shared_block(run, cpu, neighbour, r.blocks, injection, r) != 0U || checked;
                    break;
            }
            ++r.blocks;
            elapsed = now_ns() - start;
            // A neighbour preempted inside its writes leaves no consistent read: the arm runs on until one is
            // checked, which the neighbour's next write (or its end) allows.
        } while ((elapsed < run.config->nanoseconds_per_arm || !checked) && !cancelled(run));
        r.nanoseconds = elapsed;
    }
}

// The arms' memory: committed pages of one reserved range (zero when committed), released at scope exit.
class Pages
{
public:
    explicit Pages(crd::usize bytes) noexcept : m_region(crd::vm::reserve(bytes))
    {
        if (m_region && !crd::vm::commit(m_region.base, m_region.size))
        {
            crd::vm::release(m_region);
        }
    }
    ~Pages()
    {
        if (m_region)
        {
            crd::vm::release(m_region);
        }
    }
    Pages(const Pages&) = delete;
    Pages& operator=(const Pages&) = delete;
    Pages(Pages&&) = delete;
    Pages& operator=(Pages&&) = delete;

    [[nodiscard]] crd::u8* data() const noexcept { return static_cast<crd::u8*>(m_region.base); }

private:
    crd::vm::VmRegion m_region;
};

} // namespace

std::span<const crd::KernelLevel<HardwareCheckKernels>> hardware_check_levels() noexcept
{
    return kComputeLevels;
}

const char* hardware_arm_name(HardwareArm arm) noexcept
{
    switch (arm.kind)
    {
        case HardwareArmKind::Reload:
            return "reload";
        case HardwareArmKind::Memory:
            return "memory";
        case HardwareArmKind::Shared:
            return "shared";
        case HardwareArmKind::Compute:
            break;
    }
    switch (arm.level)
    {
        case crd::CpuLevel::Portable:
            return "compute-portable";
        case crd::CpuLevel::Sse2:
            return "compute-sse2";
        case crd::CpuLevel::Sse42:
            return "compute-sse4.2";
        case crd::CpuLevel::Avx2:
            return "compute-avx2";
        case crd::CpuLevel::Avx512:
            return "compute-avx512";
        case crd::CpuLevel::Neon:
            return "compute-neon";
    }
    return "compute";
}

HardwareCheckReport run_hardware_check(const HardwareCheckConfig& config, crd::memory::IAllocator* alloc)
{
    HardwareCheckReport report(alloc);
    if (config.nanoseconds_per_arm == 0U || config.nanoseconds_per_arm > kHardwareCheckMaxNanosecondsPerArm ||
        config.memory_mib == 0U || config.memory_mib > kHardwareCheckMaxMemoryMib)
    {
        report.reason.append("each arm runs for more than 0 and at most 3,600 s, with 1 to 1,024 MiB per CPU");
        return report;
    }
    const crd::u32 cpus = crd::logical_cpu_count();
    if (cpus == 0U)
    {
        report.reason.append("this OS offers no thread affinity, so no arm can run on a named CPU");
        return report;
    }
    // The memory arm loads memory, it must not page: its buffers stay within half the machine's memory.
    const HardwareIdentity& machine = host_hardware_identity();
    if ((machine.flags & kHardwareMemoryKnown) != 0U &&
        static_cast<crd::u64>(config.memory_mib) * cpus * (crd::u64{1} << 20U) > machine.memory_bytes / 2U)
    {
        report.reason.append(
            "the memory arm's buffers (memory_mib on every CPU) would pass half this machine's memory");
        return report;
    }

    report.arms[report.arm_count++] = HardwareArm{HardwareArmKind::Reload, crd::CpuLevel::Portable};
    const crd::CpuLevel selected = crd::cpu_selection().selected;
    for (const crd::KernelLevel<HardwareCheckKernels>& level : kComputeLevels)
    {
        if (crd::cpu_level_includes(selected, level.level))
        {
            report.arms[report.arm_count++] = HardwareArm{HardwareArmKind::Compute, level.level};
        }
    }
    report.arms[report.arm_count++] = HardwareArm{HardwareArmKind::Memory, crd::CpuLevel::Portable};
    report.arms[report.arm_count++] = HardwareArm{HardwareArmKind::Shared, crd::CpuLevel::Portable};

    // One range: each CPU's memory buffer, then the nodes, owners and slots of every CPU, each part 64-byte aligned.
    const crd::usize memory_words = static_cast<crd::usize>(config.memory_mib) * (crd::usize{1} << 20U) / 8U;
    const crd::usize nodes_at = memory_words * sizeof(crd::u64) * cpus;
    const crd::usize owners_at = nodes_at + sizeof(Node) * kNodes * cpus;
    const crd::usize slots_at = (owners_at + sizeof(Owner) * cpus + 63U) & ~crd::usize{63U};
    const Pages pages(slots_at + sizeof(Slot) * cpus);
    if (pages.data() == nullptr)
    {
        report.reason.append("the arms' memory could not be committed (lower memory_mib)");
        return report;
    }

    Run run;
    run.config = &config;
    run.report = &report;
    run.cpus = cpus;
    run.memory = reinterpret_cast<crd::u64*>(pages.data());
    run.memory_words = memory_words;
    run.nodes = reinterpret_cast<Node*>(pages.data() + nodes_at);
    run.owners = reinterpret_cast<Owner*>(pages.data() + owners_at);
    run.slots = reinterpret_cast<Slot*>(pages.data() + slots_at);
    for (crd::u32 c = 0U; c < cpus; ++c)
    {
        Node* const base = run.nodes + static_cast<crd::usize>(c) * kNodes;
        for (crd::u64 i = 0U; i < kNodes; ++i)
        {
            ::new (static_cast<void*>(base + i)) Node{base + (i * kLinkMultiplier + kLinkIncrement) % kNodes, i};
        }
        ::new (static_cast<void*>(run.owners + c)) Owner{base, kNodes};
        Slot* const slot = ::new (static_cast<void*>(run.slots + c)) Slot{};
        for (crd::u32 w = 0U; w < kPayloadWords; ++w)
        {
            slot->payload[w].store(payload_word(c, 0U, w), std::memory_order_relaxed); // sequence 0's payload
        }
    }
    report.cpus.resize(cpus);
    for (crd::u32 c = 0U; c < cpus; ++c)
    {
        report.cpus[c].cpu = crd::logical_cpu_at(c);
    }
    run.results = report.cpus.data();
    Barrier barrier(cpus);
    run.barrier = &barrier;

    cont::Array<std::thread> threads(alloc);
    threads.reserve(cpus);
    for (crd::u32 c = 0U; c < cpus; ++c)
    {
        threads.push_back(std::thread(&worker, std::cref(run), c));
    }
    for (std::thread& t : threads)
    {
        t.join();
    }

    report.ran = true;
    report.cancelled = cancelled(run);
    for (const HardwareCpuResult& c : report.cpus)
    {
        report.cpus_pinned += c.pinned ? 1U : 0U;
        for (crd::u32 a = 0U; a < report.arm_count; ++a)
        {
            report.events += c.arms[a].events;
        }
    }
    return report;
}

} // namespace crd::perf
