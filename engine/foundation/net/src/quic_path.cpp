// crd-net -- a QUIC connection's paths and their validation (NET-PATH; docs/design/network-foundation.md#migration).
#include <crd/crypto/provider.hpp>
#include <crd/net/quic_path.hpp>

#include <cstring>

namespace crd::net
{
namespace
{
[[nodiscard]] constexpr u64 add_saturating(u64 a, u64 b) noexcept
{
    return (a > kNeverNs - b) ? kNeverNs : a + b;
}
} // namespace

bool quic_port_only_change(const QuicPath& from, const QuicPath& to) noexcept
{
    return from.socket == to.socket && from.peer.address == to.peer.address && from.peer.port != to.peer.port;
}

void QuicPaths::open(const QuicPath& path, bool validated, u64 now_ns) noexcept
{
    *this = QuicPaths{};
    Slot& slot = m_slots[0];
    slot.used = true;
    slot.path = path;
    slot.validated = validated;
    slot.proven = validated;
    slot.last_ns = now_ns;
}

usize QuicPaths::find(const QuicPath& path) const noexcept
{
    for (usize index = 0U; index < kQuicMaxPaths; ++index)
    {
        if (m_slots[index].used && m_slots[index].path == path)
        {
            return index;
        }
    }
    return kQuicNoPath;
}

usize QuicPaths::admit(const QuicPath& path, u64 now_ns) noexcept
{
    const usize held = find(path);
    if (held != kQuicNoPath)
    {
        return held;
    }
    for (usize index = 0U; index < kQuicMaxPaths; ++index)
    {
        Slot& slot = m_slots[index];
        if (!slot.used)
        {
            slot.used = true;
            slot.path = path;
            slot.last_ns = now_ns;
            return index;
        }
    }
    return kQuicNoPath;
}

usize QuicPaths::least_recent() const noexcept
{
    usize chosen = kQuicNoPath;
    for (usize index = 0U; index < kQuicMaxPaths; ++index)
    {
        const bool spare = m_slots[index].used && index != m_active && index != m_fallback;
        if (spare && (chosen == kQuicNoPath || m_slots[index].last_ns < m_slots[chosen].last_ns))
        {
            chosen = index;
        }
    }
    return chosen;
}

void QuicPaths::drop(usize index) noexcept
{
    if (index >= kQuicMaxPaths || index == m_active)
    {
        return;
    }
    m_slots[index] = Slot{};
    m_fallback = (m_fallback == index) ? kQuicNoPath : m_fallback;
}

void QuicPaths::observation_sent(usize index, u64 sequence) noexcept
{
    m_slots[index].observation_owed = false;
    m_slots[index].observation_sequence = sequence;
}

void QuicPaths::observation_lost(usize index, u64 sequence) noexcept
{
    Slot& slot = m_slots[index];
    slot.observation_owed = slot.observation_owed || (slot.used && slot.observation_sequence == sequence);
}

bool QuicPaths::on_observed(usize index, u64 sequence, const Endpoint& observed) noexcept
{
    Slot& slot = m_slots[index];
    if (slot.observed_sequence != kQuicNoObservation && sequence <= slot.observed_sequence)
    {
        return false;
    }
    slot.observed_sequence = sequence;
    slot.observed = observed;
    return true;
}

void QuicPaths::bind(const QuicPath& path) noexcept
{
    m_slots[m_active].path = path;
}

void QuicPaths::activate(usize index) noexcept
{
    if (!used(index) || index == m_active)
    {
        return;
    }
    m_fallback = m_slots[m_active].validated ? m_active : m_fallback;
    m_fallback = (m_fallback == index) ? kQuicNoPath : m_fallback;
    m_active = index;
}

void QuicPaths::on_received(usize index, usize bytes, u64 now_ns) noexcept
{
    Slot& slot = m_slots[index];
    slot.received = add_saturating(slot.received, bytes);
    slot.last_ns = (now_ns > slot.last_ns) ? now_ns : slot.last_ns;
}

void QuicPaths::on_sent(usize index, usize bytes) noexcept
{
    m_slots[index].sent = add_saturating(m_slots[index].sent, bytes);
}

usize QuicPaths::credit(usize index) const noexcept
{
    const Slot& slot = m_slots[index];
    if (slot.validated)
    {
        return ~usize{0};
    }
    const u64 allowed = (slot.received > kNeverNs / 3U) ? kNeverNs : 3U * slot.received;
    return (allowed > slot.sent) ? static_cast<usize>(allowed - slot.sent) : 0U;
}

void QuicPaths::set_validated(usize index) noexcept
{
    m_slots[index].validated = true;
    m_slots[index].proven = true;
}

void QuicPaths::validate(usize index, u64 now_ns, u64 timeout_ns) noexcept
{
    Slot& slot = m_slots[index];
    if (slot.deadline_ns != kNeverNs)
    {
        return;
    }
    slot.deadline_ns = quic_validation_deadline(now_ns, timeout_ns);
    slot.due_ns = now_ns;
    slot.challenge_count = 0U;
}

bool QuicPaths::challenge_due(usize index, u64 now_ns) const noexcept
{
    const Slot& slot = m_slots[index];
    return slot.deadline_ns != kNeverNs && now_ns >= slot.due_ns;
}

void QuicPaths::challenge_sent(usize index, cont::ConstSpan<u8> data, bool expanded, u64 now_ns, u64 resend_ns) noexcept
{
    Slot& slot = m_slots[index];
    Challenge& challenge = slot.challenges[slot.challenge_count % kQuicPathChallengesKept];
    std::memcpy(challenge.data, data.data(), kQuicPathDataBytes);
    challenge.expanded = expanded;
    // The first resend waits one probe period, each later one twice the one before (section 8.2.1).
    slot.resend_ns = (slot.challenge_count == 0U) ? resend_ns : add_saturating(slot.resend_ns, slot.resend_ns);
    ++slot.challenge_count;
    slot.due_ns = add_saturating(now_ns, slot.resend_ns);
}

QuicPathResponse QuicPaths::on_response(cont::ConstSpan<u8> data, usize& index) noexcept
{
    index = kQuicNoPath;
    if (data.size() != kQuicPathDataBytes)
    {
        return QuicPathResponse::Ignored;
    }
    // Every live challenge is compared in constant time, none skipped once one matches.
    bool expanded = false;
    for (usize at = 0U; at < kQuicMaxPaths; ++at)
    {
        const Slot& slot = m_slots[at];
        const usize kept =
            (slot.challenge_count < kQuicPathChallengesKept) ? slot.challenge_count : kQuicPathChallengesKept;
        for (usize which = 0U; which < kept; ++which)
        {
            const Challenge& challenge = slot.challenges[which];
            const cont::ConstSpan<u8> sent(challenge.data, kQuicPathDataBytes);
            const bool equal = slot.deadline_ns != kNeverNs && crypto::constant_time_equal(data, sent);
            index = equal ? at : index;
            expanded = equal ? (expanded || challenge.expanded) : expanded;
        }
    }
    if (index == kQuicNoPath)
    {
        return QuicPathResponse::Ignored;
    }
    Slot& slot = m_slots[index];
    slot.validated = true;
    if (expanded)
    {
        slot.proven = true;
        slot.deadline_ns = kNeverNs;
        slot.due_ns = kNeverNs;
        slot.challenge_count = 0U;
    }
    else
    {
        // The address is proven; the path's 1200 bytes are not: a challenge that is expanded goes now (section 8.2.3).
        slot.due_ns = 0U;
    }
    return QuicPathResponse::Validated;
}

void QuicPaths::abandon(usize index) noexcept
{
    Slot& slot = m_slots[index];
    slot.deadline_ns = kNeverNs;
    slot.due_ns = kNeverNs;
    slot.challenge_count = 0U;
}

usize QuicPaths::expired(u64 now_ns) noexcept
{
    for (usize index = 0U; index < kQuicMaxPaths; ++index)
    {
        Slot& slot = m_slots[index];
        if (slot.used && slot.deadline_ns != kNeverNs && now_ns >= slot.deadline_ns)
        {
            abandon(index);
            return index;
        }
    }
    return kQuicNoPath;
}

void QuicPaths::on_challenge(usize index, cont::ConstSpan<u8> data) noexcept
{
    Slot& slot = m_slots[index];
    std::memcpy(slot.response, data.data(), kQuicPathDataBytes);
    slot.response_owed = true;
}

} // namespace crd::net
