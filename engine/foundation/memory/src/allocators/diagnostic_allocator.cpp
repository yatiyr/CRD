#include <crd/core/assert.hpp>
#include <crd/log/log.hpp>
#include <crd/memory/alignment.hpp>
#include <crd/memory/diagnostic_allocator.hpp>
#include <crd/memory/log_channel.hpp>

#include <cstdint>
#include <cstring>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h> // RtlCaptureStackBackTrace
#elif defined(__has_include)
#if __has_include(<execinfo.h>)
#include <execinfo.h> // backtrace
#define CRD_DIAG_HAS_EXECINFO 1
#endif
#endif
#ifndef CRD_DIAG_HAS_EXECINFO
#define CRD_DIAG_HAS_EXECINFO 0
#endif

namespace crd::memory
{
namespace
{
constexpr crd::u8 kStateEmpty = 0U;
constexpr crd::u8 kStateOccupied = 1U;
constexpr crd::u8 kStateTombstone = 2U;

crd::u32 capture_pcs(void** out, crd::u32 max, crd::u32 skip) noexcept
{
#if defined(_WIN32)
    // RtlCaptureStackBackTrace can capture at most a bounded number of frames; skip our own frames.
    return static_cast<crd::u32>(
        RtlCaptureStackBackTrace(static_cast<ULONG>(skip + 1U), static_cast<ULONG>(max), out, nullptr));
#elif CRD_DIAG_HAS_EXECINFO
    void*     tmp[kMaxStackFrames + 8U];
    const int want = static_cast<int>(max + skip + 1U);
    const int got = backtrace(tmp, want < static_cast<int>(kMaxStackFrames + 8U) ? want
                                                                                 : static_cast<int>(kMaxStackFrames + 8U));
    crd::u32  n = 0U;
    for (int i = static_cast<int>(skip + 1U); i < got && n < max; ++i)
    {
        out[n++] = tmp[i];
    }
    return n;
#else
    (void)out;
    (void)max;
    (void)skip;
    return 0U;
#endif
}
} // namespace

struct DiagnosticAllocator::Record
{
    void*         user;      // key; also state-tagged via `state`
    void*         raw;       // backing block start
    crd::usize    size;      // user payload size
    crd::usize    alignment; // user alignment
    crd::u32      front;     // bytes before user (front redzone + alignment pad)
    crd::u32      rear;      // rear redzone bytes
    AllocationTag tag;
    StackId       alloc_stack;
    StackId       free_stack;
    bool          freed;     // true while sitting in quarantine
    crd::u8       state;     // kStateEmpty / kStateOccupied / kStateTombstone
};

struct DiagnosticAllocator::StackEntry
{
    crd::u64 hash;
    crd::u32 count;
    void*    pcs[kMaxStackFrames];
};

struct DiagnosticAllocator::QuarantineEntry
{
    crd::u32 record_index;
};

DiagnosticAllocator::DiagnosticAllocator(IAllocator* backing, const DiagnosticConfig& cfg, IAllocator* metadata,
                                         const char* name) noexcept
    : m_backing(backing), m_metadata(metadata != nullptr ? metadata : default_allocator()), m_cfg(cfg)
{
    m_name = name;
    CRD_ASSERT(backing != nullptr);
    CRD_ASSERT(m_backing != m_metadata); // independence: metadata must not recurse into the diagnosed allocator
    if (m_cfg.max_live_records == 0U)
    {
        m_cfg.max_live_records = 1U;
    }
    if (m_cfg.max_stacks == 0U)
    {
        m_cfg.max_stacks = 1U;
    }
    if (m_cfg.sample_period == 0U)
    {
        m_cfg.sample_period = 1U;
    }

    const usize rec_bytes = sizeof(Record) * m_cfg.max_live_records;
    const usize stk_bytes = sizeof(StackEntry) * m_cfg.max_stacks;
    const usize q_bytes = sizeof(QuarantineEntry) * m_cfg.max_live_records;

    m_records = static_cast<Record*>(m_metadata->allocate(rec_bytes, alignof(Record)));
    m_stacks = static_cast<StackEntry*>(m_metadata->allocate(stk_bytes, alignof(StackEntry)));
    m_quarantine = static_cast<QuarantineEntry*>(m_metadata->allocate(q_bytes, alignof(QuarantineEntry)));

    if (m_records != nullptr && m_stacks != nullptr && m_quarantine != nullptr)
    {
        std::memset(m_records, 0, rec_bytes); // state = kStateEmpty, user = nullptr
        std::memset(m_stacks, 0, stk_bytes);
        std::memset(m_quarantine, 0, q_bytes);
        m_ok = true;
    }
}

DiagnosticAllocator::~DiagnosticAllocator()
{
    if (m_ok)
    {
        // Real-free everything still in quarantine (the user already logically freed these); leave
        // still-live blocks alone -- they are user leaks and report_leaks() names them.
        for (crd::u32 i = 0U; i < m_cfg.max_live_records; ++i)
        {
            Record& r = m_records[i];
            if (r.state == kStateOccupied && r.freed)
            {
                m_backing->deallocate(r.raw);
            }
        }
    }
    if (m_records != nullptr)
    {
        m_metadata->deallocate(m_records);
    }
    if (m_stacks != nullptr)
    {
        m_metadata->deallocate(m_stacks);
    }
    if (m_quarantine != nullptr)
    {
        m_metadata->deallocate(m_quarantine);
    }
}

void DiagnosticAllocator::raise(const Violation& v) const noexcept
{
    ++m_violation_count;
    if (m_handler != nullptr)
    {
        m_handler(v, m_handler_user);
    }
    else
    {
        CRD_LOG_ERROR(g_log_memory, "{} diagnostic violation kind={} ptr={} offset={}", m_name,
                      static_cast<crd::u32>(v.kind), v.ptr, v.byte_offset);
    }
}

StackId DiagnosticAllocator::intern_current_stack(crd::u32 skip) noexcept
{
    if (!m_cfg.capture_stacks)
    {
        return kNullStackId;
    }
    void*          pcs[kMaxStackFrames];
    const crd::u32 n = capture_pcs(pcs, kMaxStackFrames, skip + 1U);
    if (n == 0U)
    {
        return kNullStackId;
    }
    crd::u64 hash = 1469598103934665603ULL; // FNV-1a over the PCs
    for (crd::u32 i = 0U; i < n; ++i)
    {
        hash ^= reinterpret_cast<std::uintptr_t>(pcs[i]);
        hash *= 1099511628211ULL;
    }
    for (crd::u32 i = 1U; i < m_stack_count; ++i) // dedup against interned stacks (0 reserved)
    {
        if (m_stacks[i].hash == hash && m_stacks[i].count == n &&
            std::memcmp(m_stacks[i].pcs, pcs, n * sizeof(void*)) == 0)
        {
            return static_cast<StackId>(i);
        }
    }
    if (m_stack_count >= m_cfg.max_stacks) // stack table saturated
    {
        return kNullStackId;
    }
    StackEntry& e = m_stacks[m_stack_count];
    e.hash = hash;
    e.count = n;
    std::memcpy(e.pcs, pcs, n * sizeof(void*));
    return static_cast<StackId>(m_stack_count++);
}

DiagnosticAllocator::Record* DiagnosticAllocator::find_record(const void* user) const noexcept
{
    if (!m_ok || user == nullptr)
    {
        return nullptr;
    }
    const crd::u32 cap = m_cfg.max_live_records;
    const crd::u64 h = (reinterpret_cast<std::uintptr_t>(user) >> 4) * 0x9E3779B97F4A7C15ULL;
    crd::u32       idx = static_cast<crd::u32>(h % cap);
    for (crd::u32 step = 0U; step < cap; ++step)
    {
        Record& r = m_records[idx];
        if (r.state == kStateEmpty)
        {
            return nullptr; // probe chain ends
        }
        if (r.state == kStateOccupied && r.user == user)
        {
            return &r;
        }
        idx = (idx + 1U) % cap;
    }
    return nullptr;
}

DiagnosticAllocator::Record* DiagnosticAllocator::insert_record(void* user) noexcept
{
    const crd::u32 cap = m_cfg.max_live_records;
    const crd::u64 h = (reinterpret_cast<std::uintptr_t>(user) >> 4) * 0x9E3779B97F4A7C15ULL;
    crd::u32       idx = static_cast<crd::u32>(h % cap);
    crd::u32       tomb = cap; // first tombstone seen
    for (crd::u32 step = 0U; step < cap; ++step)
    {
        Record& r = m_records[idx];
        if (r.state == kStateEmpty)
        {
            Record& slot = (tomb != cap) ? m_records[tomb] : r;
            slot.state = kStateOccupied;
            slot.user = user;
            ++m_live_count;
            return &slot;
        }
        if (r.state == kStateTombstone && tomb == cap)
        {
            tomb = idx;
        }
        idx = (idx + 1U) % cap;
    }
    if (tomb != cap) // no empty found, but a tombstone can be reused
    {
        Record& slot = m_records[tomb];
        slot.state = kStateOccupied;
        slot.user = user;
        ++m_live_count;
        return &slot;
    }
    return nullptr; // table saturated
}

bool DiagnosticAllocator::verify_redzones(const Record& r) const noexcept
{
    bool                 ok = true;
    const crd::u8* const raw = static_cast<const crd::u8*>(r.raw);
    const crd::u8* const user = static_cast<const crd::u8*>(r.user);
    for (crd::u32 i = 0U; i < r.front; ++i) // front redzone + alignment pad: [raw, user)
    {
        if (raw[i] != m_cfg.redzone_pattern)
        {
            Violation v;
            v.kind = ViolationKind::RedzoneUnderrun;
            v.ptr = r.user;
            v.size = r.size;
            v.alloc_stack = r.alloc_stack;
            v.free_stack = r.free_stack;
            v.byte_offset = i;
            raise(v);
            ok = false;
            break;
        }
    }
    const crd::u8* const rear = user + r.size; // rear redzone: [user+size, user+size+rear)
    for (crd::u32 i = 0U; i < r.rear; ++i)
    {
        if (rear[i] != m_cfg.redzone_pattern)
        {
            Violation v;
            v.kind = ViolationKind::RedzoneOverrun;
            v.ptr = r.user;
            v.size = r.size;
            v.alloc_stack = r.alloc_stack;
            v.free_stack = r.free_stack;
            v.byte_offset = r.size + i;
            raise(v);
            ok = false;
            break;
        }
    }
    if (r.freed) // quarantined: the payload must still read as the quarantine pattern
    {
        for (crd::usize i = 0U; i < r.size; ++i)
        {
            if (user[i] != m_cfg.quarantine_pattern)
            {
                Violation v;
                v.kind = ViolationKind::QuarantineUseAfterFree;
                v.ptr = r.user;
                v.size = r.size;
                v.alloc_stack = r.alloc_stack;
                v.free_stack = r.free_stack;
                v.byte_offset = i;
                raise(v);
                ok = false;
                break;
            }
        }
    }
    return ok;
}

void DiagnosticAllocator::real_free(Record& r) noexcept
{
    if (r.freed) // evicting from quarantine: last chance to catch a use-after-free
    {
        (void)verify_redzones(r);
    }
    m_backing->deallocate(r.raw);
    r.state = kStateTombstone;
    r.user = nullptr;
    r.raw = nullptr;
    r.freed = false;
    --m_live_count;
}

void DiagnosticAllocator::flush_quarantine_until(crd::usize incoming_bytes) noexcept
{
    const crd::u32 cap = m_cfg.max_live_records;
    while (m_q_size > 0U &&
           (m_quarantine_bytes + incoming_bytes > m_cfg.quarantine_capacity_bytes || m_q_size >= cap))
    {
        const crd::u32 slot = m_quarantine[m_q_head].record_index;
        m_q_head = (m_q_head + 1U) % cap;
        --m_q_size;
        Record& r = m_records[slot];
        m_quarantine_bytes -= r.size;
        real_free(r);
    }
}

void* DiagnosticAllocator::allocate(usize size, usize alignment)
{
    return allocate_tagged(size, alignment, AllocationTag{});
}

void* DiagnosticAllocator::allocate_tagged(usize size, usize alignment, const AllocationTag& tag)
{
    if (size == 0U)
    {
        return nullptr;
    }
    CRD_ASSERT(is_pow2(alignment));
    ++m_total_count;

    if (!m_ok) // metadata arena unavailable: degrade to a clean passthrough, never corrupt anything
    {
        return m_backing->allocate(size, alignment);
    }

    // Sampling decision (guards the expensive stack capture only; redzones/records are always on).
    const bool eligible = size >= m_cfg.min_sampled_size && size <= m_cfg.max_sampled_size;
    bool       sampled = false;
    if (eligible)
    {
        ++m_eligible_count;
        sampled = (m_sample_counter % m_cfg.sample_period) == 0U;
        ++m_sample_counter;
    }

    const crd::u32 rz = m_cfg.redzone_bytes;
    const crd::u32 front = rz == 0U ? 0U : static_cast<crd::u32>(align_up(rz, alignment));
    const usize    total = static_cast<usize>(front) + size + rz;

    void* const raw = m_backing->allocate(total, alignment);
    if (raw == nullptr)
    {
        return nullptr; // backing OOM -- surfaced to the caller unchanged
    }
    void* const user = static_cast<crd::u8*>(raw) + front;

    if (rz != 0U) // pattern-fill both guard regions
    {
        std::memset(raw, m_cfg.redzone_pattern, front);
        std::memset(static_cast<crd::u8*>(user) + size, m_cfg.redzone_pattern, rz);
    }

    Record* const rec = insert_record(user);
    if (rec == nullptr) // record table saturated: the allocation still succeeds, but is untracked
    {
        ++m_saturation_count;
        if (m_cfg.mandatory)
        {
            Violation v;
            v.kind = ViolationKind::MetadataSaturation;
            v.ptr = user;
            v.size = size;
            raise(v); // never silent in mandatory mode
        }
        // Undo the redzoned block (we cannot track its raw start) and hand back a plain passthrough
        // whose user pointer == the backing block, so deallocate() frees it correctly via owns().
        m_backing->deallocate(raw);
        return m_backing->allocate(size, alignment);
    }

    rec->raw = raw;
    rec->size = size;
    rec->alignment = alignment;
    rec->front = front;
    rec->rear = rz;
    rec->tag = tag;
    rec->freed = false;
    rec->free_stack = kNullStackId;
    rec->alloc_stack = sampled ? intern_current_stack(1U) : kNullStackId;
    if (sampled)
    {
        ++m_sampled_count;
    }
    return user;
}

void DiagnosticAllocator::deallocate(void* p) noexcept
{
    if (p == nullptr)
    {
        return;
    }
    if (!m_ok)
    {
        m_backing->deallocate(p);
        return;
    }

    Record* const rec = find_record(p);
    if (rec == nullptr)
    {
        if (m_backing->owns(p)) // an untracked (saturated) passthrough block
        {
            m_backing->deallocate(p);
        }
        else
        {
            Violation v;
            v.kind = ViolationKind::UnknownPointer;
            v.ptr = p;
            raise(v);
        }
        return;
    }

    if (rec->freed) // already freed and sitting in quarantine -> double free
    {
        Violation v;
        v.kind = ViolationKind::DoubleFree;
        v.ptr = p;
        v.size = rec->size;
        v.alloc_stack = rec->alloc_stack;
        v.free_stack = rec->free_stack;
        raise(v);
        return; // do not free again
    }

    (void)verify_redzones(*rec); // catch an under/overrun that happened before the free
    if (rec->alloc_stack != kNullStackId) // sampled allocations also record their free site
    {
        rec->free_stack = intern_current_stack(1U);
    }

    if (m_cfg.quarantine_capacity_bytes > 0U && rec->size <= m_cfg.quarantine_capacity_bytes)
    {
        flush_quarantine_until(rec->size);
        std::memset(rec->user, m_cfg.quarantine_pattern, rec->size); // poison; use-after-free shows up
        rec->freed = true;
        const crd::u32 cap = m_cfg.max_live_records;
        m_quarantine[m_q_tail].record_index = static_cast<crd::u32>(rec - m_records);
        m_q_tail = (m_q_tail + 1U) % cap;
        ++m_q_size;
        m_quarantine_bytes += rec->size;
    }
    else
    {
        real_free(*rec);
    }
}

bool DiagnosticAllocator::owns(const void* p) const noexcept
{
    return find_record(p) != nullptr;
}

usize DiagnosticAllocator::allocation_size(const void* p) const noexcept
{
    const Record* const rec = find_record(p);
    return rec != nullptr ? rec->size : 0U;
}

void* DiagnosticAllocator::raw_from_user(void* user) const noexcept
{
    const Record* const rec = find_record(user);
    return rec != nullptr ? rec->raw : nullptr;
}

Provenance DiagnosticAllocator::provenance_of(const void* p) const noexcept
{
    Provenance out;
    const Record* const rec = find_record(p);
    if (rec == nullptr)
    {
        return out;
    }
    out.found = true;
    out.freed = rec->freed;
    out.ptr = rec->user;
    out.size = rec->size;
    out.alignment = rec->alignment;
    out.tag = rec->tag;
    out.alloc_stack = rec->alloc_stack;
    out.free_stack = rec->free_stack;
    return out;
}

crd::u32 DiagnosticAllocator::stack_frames(StackId id, void** out, crd::u32 max) const noexcept
{
    if (id == kNullStackId || id >= m_stack_count || out == nullptr)
    {
        return 0U;
    }
    const StackEntry& e = m_stacks[id];
    const crd::u32    n = e.count < max ? e.count : max;
    std::memcpy(out, e.pcs, n * sizeof(void*));
    return n;
}

crd::usize DiagnosticAllocator::check_all_redzones() const noexcept
{
    if (!m_ok)
    {
        return 0U;
    }
    const crd::u64 before = m_violation_count;
    for (crd::u32 i = 0U; i < m_cfg.max_live_records; ++i)
    {
        const Record& r = m_records[i];
        if (r.state == kStateOccupied)
        {
            (void)verify_redzones(r);
        }
    }
    return static_cast<crd::usize>(m_violation_count - before);
}

crd::usize DiagnosticAllocator::report_leaks(LeakVisitor fn, void* user) const noexcept
{
    if (!m_ok)
    {
        return 0U;
    }
    crd::usize live = 0U;
    for (crd::u32 i = 0U; i < m_cfg.max_live_records; ++i)
    {
        const Record& r = m_records[i];
        if (r.state == kStateOccupied && !r.freed)
        {
            ++live;
            if (fn != nullptr)
            {
                Provenance p;
                p.found = true;
                p.freed = false;
                p.ptr = r.user;
                p.size = r.size;
                p.alignment = r.alignment;
                p.tag = r.tag;
                p.alloc_stack = r.alloc_stack;
                p.free_stack = r.free_stack;
                fn(p, user);
            }
        }
    }
    return live;
}

SamplingReport DiagnosticAllocator::sampling_report() const noexcept
{
    SamplingReport rep;
    rep.sample_period = m_cfg.sample_period;
    rep.min_sampled_size = m_cfg.min_sampled_size;
    rep.max_sampled_size = m_cfg.max_sampled_size;
    rep.eligible_count = m_eligible_count;
    rep.sampled_count = m_sampled_count;
    rep.total_count = m_total_count;
    rep.retained_live = m_live_count;
    return rep;
}

crd::usize DiagnosticAllocator::per_allocation_overhead() const noexcept
{
    if (m_cfg.redzone_bytes == 0U)
    {
        return 0U;
    }
    return align_up(m_cfg.redzone_bytes, kDefaultAlignment) + m_cfg.redzone_bytes;
}

} // namespace crd::memory
