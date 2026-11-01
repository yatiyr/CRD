#pragma once

// crd-perf -- state-hash ledgers and the divergence check (HW-RESILIENCE.3).
//
// A deterministic simulation run on several machines, or replayed against a recorded run, must reach the same state at
// every step. Each participant records the hash of its state at each step in a StateHashLedger; a
// StateDivergenceMonitor judges every step all its participants have reached, as soon as they have reached it, and
// latches the first step at which they disagree. The machine is named by its HardwareIdentity (HW-RESILIENCE.2): the
// participants that disagree with the reference (a recorded run) or with the majority of three or more, and whether
// the evidence points at that machine (a machine with the same processor agrees) or at code that computes differently
// on different processors (every diverging machine's processor differs from every agreeing one's).
//
// Hashes (fixed bits, so a lockstep peer, a recorded run and an offline reader agree on them):
//   state hash  xxh3_64(state bytes, seed 0) (crd/containers/checksum.hpp), or a caller's own u64 per step;
//   chain       chain_0 = 0, chain_n = xxh3_64(LE64(chain_{n-1}) ++ LE64(step_n) ++ LE64(hash_n)), over every step
//               the ledger recorded since its first, so one u64 covers its whole history.
// A ledger keeps its last `capacity` steps and the chain before the oldest of them, so a session of any length stays
// bounded; a divergence older than every ledger's window is still seen, as "at or before" the first step judged.
//
// The participants of one monitor record the same steps from the same first step (a lockstep session hashes the same
// ticks); a step one participant recorded and another did not is itself a divergence. The exchange of these hashes on
// the wire belongs to NET.3 and 4.2c.
//
// Ledger payload (little-endian, versioned; the bundle section CRC covers it):
//   u32 magic 'CSL1', u32 version, u32 flags (0), the name (u16 length, at most kStateLedgerNameBytes bytes),
//   u32 capacity, u64 first step, u64 steps recorded, u64 chain before the oldest kept step, u32 kept steps, then each
//   kept step's u64 step and u64 hash, oldest first. The decoder refuses a wrong magic or version, unknown flags, a
//   name or count over its bound, steps out of order, counts that disagree, and trailing bytes.
//
// Threads: one simulation loop records a ledger and updates its monitors. Another thread may encode the ledger while
// it records (a flight bundle written by a host's poll, a request or a fatal path): record() and the encoders hold the
// ledger's lock, which the bundle writer of a fatal path only tries (try_encode_state_hash_ledger).
// Contract: docs/design/runtime-diagnostics.md (HW-RESILIENCE.3); ADR-0133.

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/containers/string.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>
#include <crd/perf/fault_classification.hpp>
#include <crd/perf/hardware_identity.hpp>

#include <atomic>

namespace crd::perf
{

inline constexpr crd::u32 kStateLedgerMagic = 0x314C5343U; // bytes 'C','S','L','1'
inline constexpr crd::u32 kStateLedgerVersion = 1U;
inline constexpr crd::u32 kStateLedgerNameBytes = 64U;
inline constexpr crd::u32 kDefaultStateLedgerSteps = 1U << 16U; // about 18 minutes at 60 steps a second
inline constexpr crd::u32 kMaxStateLedgerSteps = 1U << 21U;     // a ledger's and a payload's bound (32 MiB of steps),
                                                                // so a full ledger fits one bundle section

struct StateHashEntry
{
    crd::u64 step = 0U;
    crd::u64 hash = 0U;
};

// The state hash of `state`: xxh3_64 with seed 0.
[[nodiscard]] crd::u64 state_hash(crd::containers::ConstSpan<crd::u8> state) noexcept;

// The chain after `step` with `hash`, from the chain before it (see the header comment).
[[nodiscard]] crd::u64 chain_state_hash(crd::u64 chain, crd::u64 step, crd::u64 hash) noexcept;

struct StateHashLedgerRead;

// Decode an untrusted ledger payload (encode_state_hash_ledger); nothing is read outside `payload`.
[[nodiscard]] StateHashLedgerRead
decode_state_hash_ledger(crd::containers::ConstSpan<crd::u8> payload,
                         crd::memory::IAllocator* alloc = crd::memory::default_allocator());

class StateHashLedger
{
public:
    // `name` names the participant (clipped to kStateLedgerNameBytes); `capacity` is clamped to
    // [1, kMaxStateLedgerSteps].
    explicit StateHashLedger(crd::containers::StringView name, crd::u32 capacity = kDefaultStateLedgerSteps,
                             crd::memory::IAllocator* alloc = crd::memory::default_allocator());

    // A move takes the steps, never the lock: move a ledger only while no other thread reads it.
    StateHashLedger(StateHashLedger&& other) noexcept;
    StateHashLedger& operator=(StateHashLedger&& other) noexcept;
    StateHashLedger(const StateHashLedger&) = delete;
    StateHashLedger& operator=(const StateHashLedger&) = delete;
    ~StateHashLedger() = default;

    // Record the state of `step`. False, and nothing recorded, when `step` is not after the last recorded step.
    bool record(crd::u64 step, crd::containers::ConstSpan<crd::u8> state);
    bool record_hash(crd::u64 step, crd::u64 hash);

    [[nodiscard]] crd::containers::StringView name() const noexcept;
    [[nodiscard]] crd::u32 capacity() const noexcept { return m_capacity; }
    [[nodiscard]] crd::u64 recorded() const noexcept { return m_recorded; }     // every step since the first
    [[nodiscard]] crd::u32 size() const noexcept { return m_size; }             // the steps kept
    [[nodiscard]] crd::u64 first_step() const noexcept { return m_first_step; } // valid when recorded() > 0
    [[nodiscard]] crd::u64 base_chain() const noexcept { return m_base_chain; } // the chain before entry(0)
    [[nodiscard]] crd::u64 chain() const noexcept { return m_chain; }           // the chain after the last step

    // The i-th kept step, oldest first (i < size()).
    [[nodiscard]] const StateHashEntry& entry(crd::u32 i) const noexcept;

    // The kept step `step`, or null.
    [[nodiscard]] const StateHashEntry* find(crd::u64 step) const noexcept;

private:
    friend StateHashLedgerRead decode_state_hash_ledger(crd::containers::ConstSpan<crd::u8> payload,
                                                        crd::memory::IAllocator* alloc);
    friend class StateLedgerLock;

    mutable std::atomic<bool> m_locked{false}; // record() and the encoders (StateLedgerLock)

    crd::containers::String m_name;
    crd::containers::Array<StateHashEntry> m_ring;
    crd::u32 m_capacity = 1U;
    crd::u32 m_start = 0U; // the ring index of entry(0)
    crd::u32 m_size = 0U;
    crd::u64 m_recorded = 0U;
    crd::u64 m_first_step = 0U;
    crd::u64 m_base_chain = 0U;
    crd::u64 m_chain = 0U;
};

// The ledger's payload, waiting for its lock (a request or the recording thread, never a fatal path).
[[nodiscard]] crd::containers::Array<crd::u8>
encode_state_hash_ledger(const StateHashLedger& ledger,
                         crd::memory::IAllocator* alloc = crd::memory::default_allocator());

// The ledger's payload for a fatal path: its lock is tried a bounded number of times and never waited on, so a thread
// that holds it (an assertion inside record()) gets false, and `out` is empty.
[[nodiscard]] bool try_encode_state_hash_ledger(const StateHashLedger& ledger, crd::containers::Array<crd::u8>& out);

struct StateHashLedgerRead
{
    explicit StateHashLedgerRead(crd::memory::IAllocator* alloc) : ledger({}, 1U, alloc) {}

    bool ok = false;
    crd::containers::StringView error; // why the payload was refused (static text)
    StateHashLedger ledger;
};

enum class StateDivergenceVerdict : crd::u8
{
    Waiting = 0U,    // no step judged yet (fewer than two participants, or one has not reached a common step)
    Agree = 1U,      // every judged step agreed
    Diverged = 2U,   // the first disagreeing step, and who disagrees with the reference or the majority
    NoMajority = 3U, // three or more disagreed with no hash held by more than half and no reference
    Undecided = 4U,  // two disagreed with no reference: the step is known, the culprit cannot be
};

// "waiting", "agree", "diverged", "no-majority", "undecided".
[[nodiscard]] crd::containers::StringView state_divergence_verdict_name(StateDivergenceVerdict v) noexcept;

// What the machines of a Diverged verdict point at.
enum class StateDivergenceCause : crd::u8
{
    Unknown = 0U,             // a processor not recorded on one side, or both readings fit
    Machine = 1U,             // a machine with the same processor (vendor, signature and model) computed the expected
                              // state: the diverging machine is the likely cause (run `ceridc doctor --hardware` on it)
    ProcessorDifference = 2U, // every diverging machine's processor differs from every agreeing one's: code whose
                              // results depend on the processor, not one machine's fault
};

// "unknown", "machine", "processor-difference".
[[nodiscard]] crd::containers::StringView state_divergence_cause_name(StateDivergenceCause c) noexcept;

// One participant at the first divergent step.
struct StateParticipantValue
{
    bool present = false; // it recorded the step
    crd::u64 hash = 0U;
    crd::u64 chain = 0U;                   // its chain after the step (before it, when absent)
    bool agrees = false;                   // equal to the expected value (false for every participant without one)
    const CpuAdvisory* advisory = nullptr; // its processor's known instability, when it disagrees
    MicrocodeState microcode = MicrocodeState::NotListed;
};

struct StateDivergence
{
    explicit StateDivergence(crd::memory::IAllocator* alloc) : values(alloc) {}

    StateDivergenceVerdict verdict = StateDivergenceVerdict::Waiting;
    crd::u64 compared = 0U;  // steps judged
    crd::u64 skipped = 0U;   // steps passed over because a ledger no longer kept them
    crd::u64 last_step = 0U; // the last step judged (compared > 0)
    // The first divergence (Diverged, NoMajority, Undecided):
    crd::u64 step = 0U;
    bool before = false;         // the hashes of `step` agree but the chains do not: diverged at or before `step`
    bool chains_compared = true; // false when the participants' first steps differ, so only step hashes are judged
    crd::u32 reference = 0U;     // the reference participant's index (has_reference)
    bool has_reference = false;
    bool expected_known = false; // the reference's or the majority's value is `expected`
    StateParticipantValue expected;
    StateDivergenceCause cause = StateDivergenceCause::Unknown;
    crd::containers::Array<StateParticipantValue> values; // one per participant, in add() order
};

inline constexpr crd::u32 kNoStateParticipant = ~crd::u32{0};

class StateDivergenceMonitor
{
public:
    explicit StateDivergenceMonitor(crd::memory::IAllocator* alloc = crd::memory::default_allocator());

    // Add a participant: its ledger and, when known, its machine (both must outlive the monitor). Returns its index,
    // or kNoStateParticipant once a step has been judged.
    crd::u32 add(const StateHashLedger& ledger, const HardwareIdentity* machine);

    // Judge against participant `index` (a recorded run) instead of the majority. False once a step has been judged
    // or for an unknown index.
    bool set_reference(crd::u32 index) noexcept;

    // Judge every step all participants have reached since the last call (a participant whose next kept step is
    // later than a step did not record it). Returns the result; the first divergence stays latched.
    const StateDivergence& update();

    [[nodiscard]] const StateDivergence& result() const noexcept { return m_result; }
    [[nodiscard]] crd::u32 participants() const noexcept { return static_cast<crd::u32>(m_parts.size()); }
    [[nodiscard]] const StateHashLedger& ledger(crd::u32 index) const noexcept { return *m_parts[index].ledger; }
    [[nodiscard]] const HardwareIdentity* machine(crd::u32 index) const noexcept { return m_parts[index].machine; }

private:
    struct Participant
    {
        const StateHashLedger* ledger = nullptr;
        const HardwareIdentity* machine = nullptr;
        crd::u64 consumed = 0U; // steps of the ledger's whole sequence already walked
        crd::u64 chain = 0U;    // its chain after them
    };

    void judge(crd::u64 step);

    crd::memory::IAllocator* m_alloc;
    crd::containers::Array<Participant> m_parts;
    crd::containers::Array<StateParticipantValue> m_step; // scratch: the values of the step being judged
    crd::u64 m_floor = 0U; // steps below it are walked without a judgement (a ledger no longer kept them)
    StateDivergence m_result;
};

// One plain sentence: the step, who diverged on which machine, and what that points at.
[[nodiscard]] crd::containers::String
state_divergence_message(const StateDivergenceMonitor& monitor,
                         crd::memory::IAllocator* alloc = crd::memory::default_allocator());

} // namespace crd::perf
