// crd-perf -- state-hash ledgers and the divergence check (HW-RESILIENCE.3). See state_hash.hpp.

#include "json_writer.hpp"
#include "payload_codec.hpp"

#include <crd/containers/checksum.hpp>
#include <crd/perf/bundle.hpp>
#include <crd/perf/state_hash.hpp>

#include <thread>

namespace crd::perf
{
namespace
{
namespace cont = crd::containers;
using detail::append_hex;
using detail::append_u64;

// The payload's fixed fields: magic, version, flags, the name's length and its longest name, capacity, first step,
// steps recorded, base chain and kept steps. A full ledger's payload fits one bundle section.
constexpr crd::u64 kLedgerPayloadFixedBytes = 3U * 4U + 2U + kStateLedgerNameBytes + 4U + 3U * 8U + 4U;
static_assert(kLedgerPayloadFixedBytes + crd::u64{kMaxStateLedgerSteps} * 16U <= kDefaultMaxSectionBytes,
              "a full state-hash ledger must fit one bundle section");

// Tries at a ledger's lock from a fatal path, as the flight command host's module snapshot takes its own.
constexpr crd::u32 kLedgerLockAttempts = 2000U;

[[nodiscard]] crd::u32 clamp_capacity(crd::u32 capacity) noexcept
{
    if (capacity == 0U)
    {
        return 1U;
    }
    return capacity < kMaxStateLedgerSteps ? capacity : kMaxStateLedgerSteps;
}

// Two participants hold the same value at a step: both recorded it with one hash, or neither did; and, when chains
// are compared, the same history.
[[nodiscard]] bool same_value(const StateParticipantValue& a, const StateParticipantValue& b, bool chains) noexcept
{
    if (a.present != b.present || (a.present && a.hash != b.hash))
    {
        return false;
    }
    return !chains || a.chain == b.chain;
}

// One processor: the vendor, the signature and the model string (a signature names a family and stepping, which a
// desktop and a mobile part can share).
[[nodiscard]] bool same_processor(const HardwareIdentity& a, const HardwareIdentity& b) noexcept
{
    return (a.flags & kHardwareCpuKnown) != 0U && (b.flags & kHardwareCpuKnown) != 0U &&
           a.cpu_signature == b.cpu_signature && cont::to_view(a.cpu_vendor) == cont::to_view(b.cpu_vendor) &&
           cont::to_view(a.cpu_model) == cont::to_view(b.cpu_model);
}

void append_machine(cont::String& out, const HardwareIdentity* m)
{
    if (m == nullptr)
    {
        out.append("an unrecorded machine");
        return;
    }
    out.append(m->cpu_model.empty() ? cont::StringView{"an unnamed processor"} : cont::to_view(m->cpu_model));
    if ((m->flags & kHardwareMicrocodeKnown) != 0U)
    {
        out.append(", microcode ");
        append_hex(out, m->microcode);
    }
    if (!m->board_name.empty())
    {
        out.append(", board ");
        out.append(cont::to_view(m->board_vendor));
        out.append(m->board_vendor.empty() ? cont::StringView{} : cont::StringView{" "});
        out.append(cont::to_view(m->board_name));
    }
}

void append_value(cont::String& out, const StateParticipantValue& v, bool before)
{
    if (!v.present)
    {
        out.append("no state");
        return;
    }
    if (before)
    {
        out.append("history ");
        append_hex(out, v.chain);
        return;
    }
    append_hex(out, v.hash);
}

} // namespace

crd::u64 state_hash(cont::ConstSpan<crd::u8> state) noexcept
{
    return cont::xxh3_64(state);
}

crd::u64 chain_state_hash(crd::u64 chain, crd::u64 step, crd::u64 hash) noexcept
{
    crd::u8 bytes[24];
    const crd::u64 words[3] = {chain, step, hash};
    for (crd::u32 w = 0U; w < 3U; ++w)
    {
        for (crd::u32 i = 0U; i < 8U; ++i)
        {
            bytes[w * 8U + i] = static_cast<crd::u8>(words[w] >> (8U * i));
        }
    }
    return cont::xxh3_64(cont::ConstSpan<crd::u8>{bytes, sizeof(bytes)});
}

// ---- the ledger ---------------------------------------------------------------------------------------------------

// Holds a ledger's lock: waiting for it, or within a bounded number of tries.
class StateLedgerLock
{
public:
    explicit StateLedgerLock(const StateHashLedger& ledger) noexcept : m_flag(ledger.m_locked)
    {
        while (m_flag.exchange(true, std::memory_order_acquire))
        {
            std::this_thread::yield();
        }
        m_held = true;
    }

    StateLedgerLock(const StateHashLedger& ledger, crd::u32 attempts) noexcept : m_flag(ledger.m_locked)
    {
        for (crd::u32 attempt = 0U; attempt < attempts && !m_held; ++attempt)
        {
            m_held = !m_flag.exchange(true, std::memory_order_acquire);
            if (!m_held)
            {
                std::this_thread::yield();
            }
        }
    }

    ~StateLedgerLock()
    {
        if (m_held)
        {
            m_flag.store(false, std::memory_order_release);
        }
    }

    StateLedgerLock(const StateLedgerLock&) = delete;
    StateLedgerLock& operator=(const StateLedgerLock&) = delete;
    StateLedgerLock(StateLedgerLock&&) = delete;
    StateLedgerLock& operator=(StateLedgerLock&&) = delete;

    [[nodiscard]] bool held() const noexcept { return m_held; }

private:
    std::atomic<bool>& m_flag;
    bool m_held = false;
};

StateHashLedger::StateHashLedger(cont::StringView name, crd::u32 capacity, crd::memory::IAllocator* alloc)
    : m_name(alloc), m_ring(alloc), m_capacity(clamp_capacity(capacity))
{
    m_name.append(name.size() <= kStateLedgerNameBytes ? name : name.substr(0U, kStateLedgerNameBytes));
}

StateHashLedger::StateHashLedger(StateHashLedger&& other) noexcept
    : m_name(static_cast<cont::String&&>(other.m_name)),
      m_ring(static_cast<cont::Array<StateHashEntry>&&>(other.m_ring)), m_capacity(other.m_capacity),
      m_start(other.m_start), m_size(other.m_size), m_recorded(other.m_recorded), m_first_step(other.m_first_step),
      m_base_chain(other.m_base_chain), m_chain(other.m_chain)
{
    other.m_start = 0U;
    other.m_size = 0U;
    other.m_recorded = 0U;
    other.m_first_step = 0U;
    other.m_base_chain = 0U;
    other.m_chain = 0U;
}

StateHashLedger& StateHashLedger::operator=(StateHashLedger&& other) noexcept
{
    if (this != &other)
    {
        m_name = static_cast<cont::String&&>(other.m_name);
        m_ring = static_cast<cont::Array<StateHashEntry>&&>(other.m_ring);
        m_capacity = other.m_capacity;
        m_start = other.m_start;
        m_size = other.m_size;
        m_recorded = other.m_recorded;
        m_first_step = other.m_first_step;
        m_base_chain = other.m_base_chain;
        m_chain = other.m_chain;
        other.m_start = 0U;
        other.m_size = 0U;
        other.m_recorded = 0U;
        other.m_first_step = 0U;
        other.m_base_chain = 0U;
        other.m_chain = 0U;
    }
    return *this;
}

bool StateHashLedger::record(crd::u64 step, cont::ConstSpan<crd::u8> state)
{
    return record_hash(step, state_hash(state));
}

bool StateHashLedger::record_hash(crd::u64 step, crd::u64 hash)
{
    const StateLedgerLock lock(*this);
    if (m_recorded > 0U && step <= entry(m_size - 1U).step)
    {
        return false;
    }
    if (m_recorded == 0U)
    {
        m_first_step = step;
    }
    if (m_size < m_capacity)
    {
        m_ring.push_back(StateHashEntry{step, hash});
        ++m_size;
    }
    else
    {
        // The oldest kept step leaves the window; its chain becomes the base.
        StateHashEntry& oldest = m_ring[m_start];
        m_base_chain = chain_state_hash(m_base_chain, oldest.step, oldest.hash);
        oldest = StateHashEntry{step, hash};
        m_start = m_start + 1U == m_capacity ? 0U : m_start + 1U;
    }
    m_chain = chain_state_hash(m_chain, step, hash);
    ++m_recorded;
    return true;
}

cont::StringView StateHashLedger::name() const noexcept
{
    return cont::to_view(m_name);
}

const StateHashEntry& StateHashLedger::entry(crd::u32 i) const noexcept
{
    const crd::u32 at = m_start + i;
    return m_ring[at >= m_capacity ? at - m_capacity : at];
}

const StateHashEntry* StateHashLedger::find(crd::u64 step) const noexcept
{
    crd::u32 lo = 0U;
    crd::u32 hi = m_size;
    while (lo < hi)
    {
        const crd::u32 mid = lo + (hi - lo) / 2U;
        if (entry(mid).step < step)
        {
            lo = mid + 1U;
        }
        else
        {
            hi = mid;
        }
    }
    return lo < m_size && entry(lo).step == step ? &entry(lo) : nullptr;
}

// ---- the payload --------------------------------------------------------------------------------------------------

namespace
{
void put_ledger(cont::Array<crd::u8>& out, const StateHashLedger& ledger)
{
    out.reserve(static_cast<crd::usize>(kLedgerPayloadFixedBytes) + static_cast<crd::usize>(ledger.size()) * 16U);
    detail::put_u32(out, kStateLedgerMagic);
    detail::put_u32(out, kStateLedgerVersion);
    detail::put_u32(out, 0U);
    detail::put_field(out, ledger.name(), kStateLedgerNameBytes);
    detail::put_u32(out, ledger.capacity());
    detail::put_u64(out, ledger.first_step());
    detail::put_u64(out, ledger.recorded());
    detail::put_u64(out, ledger.base_chain());
    detail::put_u32(out, ledger.size());
    for (crd::u32 i = 0U; i < ledger.size(); ++i)
    {
        detail::put_u64(out, ledger.entry(i).step);
        detail::put_u64(out, ledger.entry(i).hash);
    }
}
} // namespace

cont::Array<crd::u8> encode_state_hash_ledger(const StateHashLedger& ledger, crd::memory::IAllocator* alloc)
{
    cont::Array<crd::u8> out(alloc);
    const StateLedgerLock lock(ledger);
    put_ledger(out, ledger);
    return out;
}

bool try_encode_state_hash_ledger(const StateHashLedger& ledger, cont::Array<crd::u8>& out)
{
    out.clear();
    const StateLedgerLock lock(ledger, kLedgerLockAttempts);
    if (!lock.held())
    {
        return false;
    }
    put_ledger(out, ledger);
    return true;
}

StateHashLedgerRead decode_state_hash_ledger(cont::ConstSpan<crd::u8> payload, crd::memory::IAllocator* alloc)
{
    StateHashLedgerRead r(alloc);
    detail::Cursor c{payload};
    const crd::u32 magic = c.u32();
    const crd::u32 version = c.u32();
    const crd::u32 flags = c.u32();
    if (!c.ok || magic != kStateLedgerMagic)
    {
        r.error = "not a state-hash ledger";
        return r;
    }
    if (version != kStateLedgerVersion)
    {
        r.error = "unknown state-hash ledger version";
        return r;
    }
    if (flags != 0U)
    {
        r.error = "unknown state-hash ledger flags";
        return r;
    }
    StateHashLedger& l = r.ledger;
    l.m_name.clear();
    if (!c.field(l.m_name, kStateLedgerNameBytes))
    {
        r.error = "ledger name over its bound or past the payload";
        return r;
    }
    const crd::u32 capacity = c.u32();
    const crd::u64 first_step = c.u64();
    const crd::u64 recorded = c.u64();
    const crd::u64 base_chain = c.u64();
    const crd::u32 count = c.u32();
    if (!c.ok)
    {
        r.error = "ledger header past the payload";
        return r;
    }
    if (capacity == 0U || capacity > kMaxStateLedgerSteps || count > capacity)
    {
        r.error = "ledger capacity or step count over its bound";
        return r;
    }
    // A ledger keeps its last steps: none only when it recorded none, all of them only from a zero base chain.
    if (count > recorded || (recorded > 0U && count == 0U) || (count == recorded && base_chain != 0U) ||
        (count < recorded && count < capacity))
    {
        r.error = "ledger counts disagree";
        return r;
    }
    if (!c.need(static_cast<crd::usize>(count) * 16U))
    {
        r.error = "ledger steps past the payload";
        return r;
    }
    l.m_capacity = capacity;
    l.m_ring.reserve(count);
    crd::u64 chain = base_chain;
    for (crd::u32 i = 0U; i < count; ++i)
    {
        const StateHashEntry e{c.u64(), c.u64()};
        const bool first = i == 0U;
        if ((first && (count == recorded ? e.step != first_step : e.step <= first_step)) ||
            (!first && e.step <= l.m_ring[i - 1U].step))
        {
            r.error = "ledger steps out of order";
            return r;
        }
        l.m_ring.push_back(e);
        chain = chain_state_hash(chain, e.step, e.hash);
    }
    if (!c.done())
    {
        r.error = "trailing bytes after the ledger";
        return r;
    }
    l.m_size = count;
    l.m_start = 0U;
    l.m_recorded = recorded;
    l.m_first_step = first_step;
    l.m_base_chain = base_chain;
    l.m_chain = chain;
    r.ok = true;
    return r;
}

// ---- the monitor --------------------------------------------------------------------------------------------------

cont::StringView state_divergence_verdict_name(StateDivergenceVerdict v) noexcept
{
    switch (v)
    {
        case StateDivergenceVerdict::Waiting:
            return "waiting";
        case StateDivergenceVerdict::Agree:
            return "agree";
        case StateDivergenceVerdict::Diverged:
            return "diverged";
        case StateDivergenceVerdict::NoMajority:
            return "no-majority";
        case StateDivergenceVerdict::Undecided:
            return "undecided";
    }
    return "unknown";
}

cont::StringView state_divergence_cause_name(StateDivergenceCause c) noexcept
{
    switch (c)
    {
        case StateDivergenceCause::Unknown:
            return "unknown";
        case StateDivergenceCause::Machine:
            return "machine";
        case StateDivergenceCause::ProcessorDifference:
            return "processor-difference";
    }
    return "unknown";
}

StateDivergenceMonitor::StateDivergenceMonitor(crd::memory::IAllocator* alloc)
    : m_alloc(alloc), m_parts(alloc), m_step(alloc), m_result(alloc)
{
}

crd::u32 StateDivergenceMonitor::add(const StateHashLedger& ledger, const HardwareIdentity* machine)
{
    if (m_result.compared > 0U || m_result.skipped > 0U)
    {
        return kNoStateParticipant;
    }
    Participant p;
    p.ledger = &ledger;
    p.machine = machine;
    m_parts.push_back(p);
    m_step.push_back(StateParticipantValue{});
    return static_cast<crd::u32>(m_parts.size() - 1U);
}

bool StateDivergenceMonitor::set_reference(crd::u32 index) noexcept
{
    if (index >= m_parts.size() || m_result.compared > 0U || m_result.skipped > 0U)
    {
        return false;
    }
    m_result.reference = index;
    m_result.has_reference = true;
    return true;
}

const StateDivergence& StateDivergenceMonitor::update()
{
    const crd::usize n = m_parts.size();
    if (n < 2U)
    {
        return m_result;
    }
    // A ledger that dropped steps this walk never saw restarts from its base chain; the steps before its oldest kept
    // one cannot be judged for anyone.
    for (Participant& p : m_parts)
    {
        const crd::u64 dropped = p.ledger->recorded() - p.ledger->size();
        if (p.consumed < dropped)
        {
            p.consumed = dropped;
            p.chain = p.ledger->base_chain();
            const crd::u64 oldest = p.ledger->entry(0U).step;
            m_floor = oldest > m_floor ? oldest : m_floor;
        }
    }
    for (;;)
    {
        crd::u64 step = ~crd::u64{0};
        for (const Participant& p : m_parts)
        {
            if (p.consumed == p.ledger->recorded())
            {
                return m_result; // a participant has not reached the next step yet
            }
            const crd::u64 dropped = p.ledger->recorded() - p.ledger->size();
            const crd::u64 next = p.ledger->entry(static_cast<crd::u32>(p.consumed - dropped)).step;
            step = next < step ? next : step;
        }
        for (crd::usize i = 0U; i < n; ++i)
        {
            Participant& p = m_parts[i];
            const crd::u64 dropped = p.ledger->recorded() - p.ledger->size();
            const StateHashEntry& e = p.ledger->entry(static_cast<crd::u32>(p.consumed - dropped));
            StateParticipantValue& v = m_step[i];
            v = StateParticipantValue{};
            v.present = e.step == step;
            if (v.present)
            {
                v.hash = e.hash;
                p.chain = chain_state_hash(p.chain, e.step, e.hash);
                ++p.consumed;
            }
            v.chain = p.chain;
        }
        if (step < m_floor)
        {
            ++m_result.skipped;
            continue;
        }
        judge(step);
    }
}

void StateDivergenceMonitor::judge(crd::u64 step)
{
    StateDivergence& r = m_result;
    ++r.compared;
    r.last_step = step;
    if (r.verdict != StateDivergenceVerdict::Waiting && r.verdict != StateDivergenceVerdict::Agree)
    {
        return; // the first divergence stays latched
    }
    const crd::usize n = m_parts.size();
    bool chains = true;
    for (crd::usize i = 1U; i < n; ++i)
    {
        chains = chains && m_parts[i].ledger->first_step() == m_parts[0].ledger->first_step();
    }
    bool all_same = true;
    for (crd::usize i = 1U; i < n; ++i)
    {
        all_same = all_same && same_value(m_step[i], m_step[0], chains);
    }
    if (all_same)
    {
        r.verdict = StateDivergenceVerdict::Agree;
        return;
    }

    // The expected value: the reference's, else one held by more than half of the participants.
    crd::usize expected = n;
    if (r.has_reference)
    {
        expected = r.reference;
    }
    else if (n >= 3U)
    {
        for (crd::usize i = 0U; i < n && expected == n; ++i)
        {
            crd::usize votes = 0U;
            for (crd::usize j = 0U; j < n; ++j)
            {
                votes += same_value(m_step[i], m_step[j], chains) ? 1U : 0U;
            }
            expected = votes * 2U > n ? i : n;
        }
    }
    r.step = step;
    r.chains_compared = chains;
    r.values.clear();
    for (crd::usize i = 0U; i < n; ++i)
    {
        r.values.push_back(m_step[i]);
    }
    r.expected_known = expected != n;
    if (!r.expected_known)
    {
        r.verdict = n >= 3U ? StateDivergenceVerdict::NoMajority : StateDivergenceVerdict::Undecided;
        r.before = true;
        for (crd::usize i = 1U; i < n; ++i)
        {
            r.before = r.before && same_value(m_step[i], m_step[0], false);
        }
        return;
    }
    r.verdict = StateDivergenceVerdict::Diverged;
    r.expected = m_step[expected];
    r.before = true;
    for (crd::usize i = 0U; i < n; ++i)
    {
        StateParticipantValue& v = r.values[i];
        v.agrees = same_value(v, r.expected, chains);
        r.before = r.before && same_value(v, r.expected, false);
        if (!v.agrees && m_parts[i].machine != nullptr)
        {
            // The machine's processor advisory, judged as classify_fault judges it.
            const FaultClassification c =
                classify_fault(nullptr, m_parts[i].machine, ReplayOutcome::NotRun, nullptr, m_alloc);
            v.advisory = c.advisory;
            v.microcode = c.microcode;
        }
    }
    r.expected.agrees = true;

    // The cause: a diverging machine whose processor an agreeing machine shares points at that machine; diverging
    // processors no agreeing machine has point at the code. Both, or an unrecorded processor, leave it unknown.
    bool shared = false;
    bool distinct = true;
    for (crd::usize i = 0U; i < n; ++i)
    {
        if (r.values[i].agrees)
        {
            continue;
        }
        const HardwareIdentity* bad = m_parts[i].machine;
        for (crd::usize j = 0U; j < n; ++j)
        {
            const HardwareIdentity* good = m_parts[j].machine;
            if (!r.values[j].agrees)
            {
                continue;
            }
            if (bad == nullptr || good == nullptr || (bad->flags & kHardwareCpuKnown) == 0U ||
                (good->flags & kHardwareCpuKnown) == 0U)
            {
                distinct = false;
                continue;
            }
            if (same_processor(*bad, *good))
            {
                shared = true;
                distinct = false;
            }
        }
    }
    if (shared)
    {
        r.cause = StateDivergenceCause::Machine;
    }
    else if (distinct)
    {
        r.cause = StateDivergenceCause::ProcessorDifference;
    }
}

cont::String state_divergence_message(const StateDivergenceMonitor& monitor, crd::memory::IAllocator* alloc)
{
    const StateDivergence& r = monitor.result();
    cont::String out(alloc);
    if (r.verdict == StateDivergenceVerdict::Waiting)
    {
        out.append("No step has been compared yet.");
        return out;
    }
    if (r.verdict == StateDivergenceVerdict::Agree)
    {
        out.append("Every participant computed the same state at all ");
        append_u64(out, r.compared);
        out.append(" compared steps, the last at step ");
        append_u64(out, r.last_step);
        out.append(".");
        return out;
    }
    out.append(r.before ? "The states diverged at or before step " : "The states diverged at step ");
    append_u64(out, r.step);
    if (r.verdict != StateDivergenceVerdict::Diverged)
    {
        out.append(r.verdict == StateDivergenceVerdict::Undecided
                       ? ", and with two participants and no reference neither can be named: "
                       : ", and no state is held by more than half of the participants: ");
        for (crd::u32 i = 0U; i < monitor.participants(); ++i)
        {
            out.append(i == 0U ? cont::StringView{} : cont::StringView{"; "});
            out.append(monitor.ledger(i).name());
            out.append(" (");
            append_machine(out, monitor.machine(i));
            out.append(") computed ");
            append_value(out, r.values[i], r.before);
        }
        out.append(".");
        return out;
    }
    out.append(": ");
    bool first = true;
    for (crd::u32 i = 0U; i < monitor.participants(); ++i)
    {
        const StateParticipantValue& v = r.values[i];
        if (v.agrees)
        {
            continue;
        }
        out.append(first ? cont::StringView{} : cont::StringView{"; "});
        first = false;
        out.append(monitor.ledger(i).name());
        out.append(" (");
        append_machine(out, monitor.machine(i));
        out.append(") computed ");
        append_value(out, v, r.before);
    }
    out.append(r.has_reference ? cont::StringView{" where the reference "} : cont::StringView{" where the majority "});
    if (r.has_reference)
    {
        out.append(monitor.ledger(r.reference).name());
        out.append(" ");
    }
    out.append("computed ");
    append_value(out, r.expected, r.before);
    out.append(". ");
    switch (r.cause)
    {
        case StateDivergenceCause::Machine:
            out.append("A machine with the same processor agrees, so the diverging machine is the likely cause: run "
                       "'ceridc doctor --hardware' on it.");
            break;
        case StateDivergenceCause::ProcessorDifference:
            out.append("Every diverging machine has a processor no agreeing machine has, which points at code whose "
                       "results depend on the processor rather than at one machine.");
            break;
        case StateDivergenceCause::Unknown:
            out.append("Run 'ceridc doctor --hardware' on the diverging machine to test it.");
            break;
    }
    for (crd::u32 i = 0U; i < monitor.participants(); ++i)
    {
        const StateParticipantValue& v = r.values[i];
        if (v.agrees || v.advisory == nullptr)
        {
            continue;
        }
        out.append(" The processor of ");
        out.append(monitor.ledger(i).name());
        out.append(" has a known instability, ");
        out.append(v.advisory->name);
        if (v.microcode == MicrocodeState::BeforeFix)
        {
            out.append(", and its microcode is older than the fix ");
            append_hex(out, v.advisory->fixed_microcode);
            out.append(": update the BIOS from the board's maker.");
        }
        else
        {
            out.append(".");
        }
    }
    return out;
}

} // namespace crd::perf
