#pragma once

// crd-net -- the state-hash exchange (NET.3, threat T12; contract: docs/design/network-foundation.md#state-hashes): two
// peers of a lockstep or rollback session report the hash of each state they have confirmed (StateHash on session
// control) and find the first report where they differ. The hashes come from the replication policy (4.2c), which says
// which steps it records; what a divergence then does is HW-RESILIENCE.3's. Memory is fixed at construction, nothing
// here allocates, and whatever a peer reports costs at most one binary search.

#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>

namespace crd::net
{
namespace cont = crd::containers;

// What StateHash carries. `chain` confirms every step recorded up to this one, so a sparse report still covers the
// steps between two reports.
struct StateHashReport
{
    u64 step = 0U;
    u64 hash = 0U;
    u64 chain = 0U;
};

// The chain after `step`: the XXH3-64 of the step and hash, little-endian, seeded with the chain before it. The first
// step's previous chain is zero.
[[nodiscard]] u64 state_hash_chain(u64 previous, u64 step, u64 hash) noexcept;

enum class StateHashVerdict : u8
{
    Agreed,          // this report matches
    Waiting,         // nothing to compare yet: a report is ahead of this side, or this side's step has no report
    Stale,           // a report that does not rise, or a step that does not: it changed nothing
    NotHeld,         // this side has no record of the step: given up, or never recorded. A later chain covers it
    Diverged,        // the first difference: divergence() names it. Said exactly once
    AlreadyDiverged, // nothing is compared after the first difference until restart()
};

struct StateHashDivergence
{
    bool any_agreed = false;
    u64 agreed_through = 0U; // the newest step both sides reported alike, when any_agreed
    u64 step = 0U;           // the report that showed the difference: it lies in (agreed_through, step]
    u64 local_hash = 0U;
    u64 peer_hash = 0U;
    bool at_this_step = false; // the hashes themselves differ; otherwise only the chain does, so a step before this did
};

class StateHashExchange
{
public:
    // `local` keeps this side's newest records, `ahead` the peer's reports this side has not reached. Each holds at
    // least one entry and outlives the exchange. Reports that overflow `ahead` go oldest first: a later chain covers
    // them.
    StateHashExchange(cont::Span<StateHashReport> local, cont::Span<StateHashReport> ahead) noexcept;

    // This side's hash of `step`, which must rise. Fills what to send in StateHash, also after a divergence; on Stale,
    // fills nothing. A report waiting for this step is compared now.
    [[nodiscard]] StateHashVerdict record(u64 step, u64 hash, StateHashReport& report) noexcept;

    // The peer's StateHash.
    [[nodiscard]] StateHashVerdict on_report(const StateHashReport& report) noexcept;

    [[nodiscard]] bool diverged() const noexcept { return m_diverged; }
    [[nodiscard]] const StateHashDivergence& divergence() const noexcept { return m_divergence; }

    // Reports dropped because `ahead` was full.
    [[nodiscard]] u64 dropped() const noexcept { return m_dropped; }

    // After a resynchronisation both sides start over, chains included.
    void restart() noexcept;

private:
    [[nodiscard]] u64 newest_local() const noexcept;
    [[nodiscard]] const StateHashReport* find_local(u64 step) const noexcept;
    [[nodiscard]] StateHashVerdict compare(const StateHashReport& report) noexcept;

    cont::Span<StateHashReport> m_local;
    cont::Span<StateHashReport> m_ahead;
    usize m_local_count = 0U;
    usize m_local_next = 0U;
    usize m_ahead_count = 0U;
    usize m_ahead_first = 0U;
    u64 m_chain = 0U;
    bool m_any_report = false;
    u64 m_last_report = 0U;
    bool m_diverged = false;
    StateHashDivergence m_divergence;
    u64 m_dropped = 0U;
};

} // namespace crd::net
