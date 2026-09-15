#pragma once

// crd-memory -- allocation/free provenance, redzones, bounded quarantine, guarded sampling and
// leak/retention summaries, as a decorator over any IAllocator.
//
// DiagnosticAllocator wraps a backing IAllocator and, per configuration, adds:
//   - Redzones: guard bytes on both sides of every user allocation, pattern-filled and verified on
//     free (and on demand) so an under/overrun is an ISOLATED, ATTRIBUTABLE named violation.
//   - Provenance: each live allocation carries size/alignment/owner/task/generation plus interned
//     allocation and free call-stack ids (raw PCs captured cheaply, symbolized OFFLINE). The record
//     store is a BOUNDED, INDEPENDENT arena taken from a separate metadata allocator -- never the
//     backing allocator being diagnosed, so diagnosis cannot recurse into or perturb it.
//   - Quarantine: freed blocks are held (poisoned, redzones retained) up to a byte cap before the
//     backing free actually runs, so a use-after-free lands on a still-owned block whose allocation
//     AND free sites are still reportable. The cap is explicit; eviction is FIFO.
//   - Guarded sampling: only a configured fraction / size-eligible subset gets full provenance. The
//     sampling report states the probability, eligible size window and retained history -- it never
//     implies exhaustive detection.
//   - Leak/retention summary: the still-live records with their allocation sites.
//
// Metadata saturation (the bounded record/stack tables filling) never corrupts the allocator and
// never silently disables a mandatory mode: the allocation still succeeds, a MetadataSaturation
// violation is raised, and a saturation counter advances.
//
// Contract: docs/design/runtime-diagnostics.md; ADR-0133.

#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::memory
{
// An interned call stack: a compact id standing for a captured sequence of program counters.
// 0 is reserved for "no stack" (capture disabled or table saturated).
using StackId = crd::u32;
inline constexpr StackId kNullStackId = 0U;

// Maximum PCs retained per captured stack (frames beyond this are dropped -- cheap capture).
inline constexpr crd::u32 kMaxStackFrames = 16U;

enum class ViolationKind : crd::u32
{
    RedzoneUnderrun,      // bytes written before the user block
    RedzoneOverrun,       // bytes written past the user block
    DoubleFree,           // freeing a pointer already freed (found in quarantine or unknown)
    UnknownPointer,       // freeing a pointer this allocator never handed out
    MetadataSaturation,   // the bounded record/stack arena is full (allocation still succeeded)
    QuarantineUseAfterFree // a redzone check found a quarantined (freed) block was written after free
};

struct Violation
{
    ViolationKind kind = ViolationKind::UnknownPointer;
    const void*   ptr = nullptr;   // user pointer involved
    crd::usize    size = 0U;       // user size, if known
    StackId       alloc_stack = kNullStackId;
    StackId       free_stack = kNullStackId; // the free site, for double-free / use-after-free
    crd::usize    byte_offset = 0U; // first corrupted redzone byte, relative to the user block
};

// Called on each detected violation. Default handler logs and increments the counter; it does NOT
// abort, so tests can install a recording handler and assert the exact verdict.
using ViolationHandler = void (*)(const Violation& v, void* user);

// Optional per-allocation identity a caller can stamp (owner subsystem / task / generation). Borrowed
// values only -- the diagnostic allocator copies the scalars, never the pointed-to owner.
struct AllocationTag
{
    crd::u32 owner = 0U;      // owning-module id (0 = unset)
    crd::u32 task = 0U;       // task/job id (0 = unset)
    crd::u32 generation = 0U; // caller generation key (0 = unset)
};

struct DiagnosticConfig
{
    crd::u32 redzone_bytes = 16U;    // guard bytes each side; 0 disables redzones
    crd::u8  redzone_pattern = 0xFDU;
    crd::u8  quarantine_pattern = 0xDEU; // fill freed payloads with this while quarantined

    // Sampling: 1 = every eligible allocation captures a full stack; N = 1-in-N (deterministic
    // counter, not RNG, so runs are reproducible). Only sizes in [min,max] are eligible.
    crd::u32   sample_period = 1U;
    crd::usize min_sampled_size = 0U;
    crd::usize max_sampled_size = ~crd::usize{0};

    // Quarantine: total bytes of freed user payload held before the real free runs. 0 = free
    // immediately (no use-after-free window).
    crd::usize quarantine_capacity_bytes = 0U;

    // Bounded metadata arena sizing.
    crd::u32 max_live_records = 4096U; // simultaneous live+quarantined allocations tracked
    crd::u32 max_stacks = 1024U;       // distinct interned call stacks

    bool capture_stacks = true; // capture PCs on alloc/free (off => provenance without sites)
    bool mandatory = true;      // mandatory mode: saturation raises a violation, never silent
};

struct SamplingReport
{
    crd::u32   sample_period = 1U;
    crd::usize min_sampled_size = 0U;
    crd::usize max_sampled_size = 0U;
    crd::u64   eligible_count = 0U; // allocations that fell in the size window
    crd::u64   sampled_count = 0U;  // of those, the ones that captured a full stack
    crd::u64   total_count = 0U;    // all allocations
    crd::usize retained_live = 0U;  // records currently live (history retained)
};

// Provenance of one pointer, as reported for a stale access. `freed` distinguishes a live block from
// one still sitting in quarantine (a use-after-free target) -- both keep their alloc/free sites.
struct Provenance
{
    bool       found = false;
    bool       freed = false;
    const void* ptr = nullptr;
    crd::usize size = 0U;
    crd::usize alignment = 0U;
    AllocationTag tag{};
    StackId    alloc_stack = kNullStackId;
    StackId    free_stack = kNullStackId;
};

class DiagnosticAllocator final : public IAllocator
{
public:
    // `backing` is the allocator under diagnosis (never used for metadata). `metadata` is an
    // INDEPENDENT allocator the bounded record/stack/quarantine arenas are carved from once; if null,
    // the process default allocator is used. Both must outlive this object.
    DiagnosticAllocator(IAllocator* backing, const DiagnosticConfig& cfg, IAllocator* metadata = nullptr,
                        const char* name = "DiagnosticAllocator") noexcept;
    ~DiagnosticAllocator() override;

    DiagnosticAllocator(const DiagnosticAllocator&) = delete;
    DiagnosticAllocator& operator=(const DiagnosticAllocator&) = delete;

    // ---- IAllocator -----------------------------------------------------
    void* allocate(usize size, usize alignment = kDefaultAlignment) override;
    void  deallocate(void* p) noexcept override;
    [[nodiscard]] bool owns(const void* p) const noexcept override;
    [[nodiscard]] usize allocation_size(const void* p) const noexcept override;

    // ---- Tagged allocation ---------------------------------------------
    // Same as allocate() but stamps owner/task/generation into the provenance record.
    void* allocate_tagged(usize size, usize alignment, const AllocationTag& tag);

    // ---- Provenance / diagnostics --------------------------------------
    // Report the record for a user pointer (live or quarantined). Used to attribute a stale access.
    [[nodiscard]] Provenance provenance_of(const void* p) const noexcept;

    // Copy up to `max` raw PCs of an interned stack into `out`; returns the frame count. Symbolize
    // these offline (module base + PC). Returns 0 for kNullStackId or an unknown id.
    crd::u32 stack_frames(StackId id, void** out, crd::u32 max) const noexcept;

    // Verify every live and quarantined block's redzones now. Returns the number of violations found
    // (each also routed to the handler). A clean instrumented heap returns 0.
    crd::usize check_all_redzones() const noexcept;

    // Invoke `fn` for each still-live (not freed) record: its pointer, size and allocation stack.
    // Returns the live count. This is the leak/retention summary.
    using LeakVisitor = void (*)(const Provenance& p, void* user);
    crd::usize report_leaks(LeakVisitor fn, void* user) const noexcept;

    [[nodiscard]] SamplingReport sampling_report() const noexcept;
    [[nodiscard]] crd::u64 metadata_saturation_count() const noexcept { return m_saturation_count; }
    [[nodiscard]] crd::u64 violation_count() const noexcept { return m_violation_count; }
    [[nodiscard]] crd::usize live_count() const noexcept { return m_live_count; }
    [[nodiscard]] crd::usize quarantined_bytes() const noexcept { return m_quarantine_bytes; }

    // Per-user-allocation memory overhead in bytes (both redzones + header), for cost measurement.
    [[nodiscard]] crd::usize per_allocation_overhead() const noexcept;

    void set_violation_handler(ViolationHandler fn, void* user) noexcept
    {
        m_handler = fn;
        m_handler_user = user;
    }

private:
    struct Record;    // live/quarantined allocation record (pointer-keyed, open addressed)
    struct StackEntry; // interned call stack
    struct QuarantineEntry;

    void*  raw_from_user(void* user) const noexcept;   // user ptr -> backing block start
    void   raise(const Violation& v) const noexcept;
    StackId intern_current_stack(crd::u32 skip) noexcept;
    Record* find_record(const void* user) const noexcept;
    Record* insert_record(void* user) noexcept;        // null on saturation
    bool    verify_redzones(const Record& r) const noexcept; // false + raises on corruption
    void    flush_quarantine_until(crd::usize incoming_bytes) noexcept;
    void    real_free(Record& r) noexcept;

    IAllocator* m_backing;
    IAllocator* m_metadata;
    DiagnosticConfig m_cfg;

    Record*          m_records = nullptr;   // [m_cfg.max_live_records], open-addressed by user ptr
    StackEntry*      m_stacks = nullptr;    // [m_cfg.max_stacks], interned; index 0 reserved (null)
    QuarantineEntry* m_quarantine = nullptr; // [m_cfg.max_live_records] FIFO ring
    crd::u32 m_stack_count = 1U;            // 0 reserved for kNullStackId

    crd::usize m_live_count = 0U;           // records occupied (live + quarantined)
    crd::u32   m_q_head = 0U;               // FIFO ring indices
    crd::u32   m_q_tail = 0U;
    crd::u32   m_q_size = 0U;
    crd::usize m_quarantine_bytes = 0U;

    crd::u32 m_sample_counter = 0U;

    // Counters (mutable so const query paths that detect corruption can still tally).
    mutable crd::u64 m_violation_count = 0U;
    crd::u64 m_saturation_count = 0U;
    crd::u64 m_total_count = 0U;
    crd::u64 m_eligible_count = 0U;
    crd::u64 m_sampled_count = 0U;

    ViolationHandler m_handler = nullptr;
    void*            m_handler_user = nullptr;
    bool             m_ok = false; // false if the metadata arena could not be reserved
};

} // namespace crd::memory
