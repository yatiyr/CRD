// crd-net -- a QUIC connection's connection IDs (NET-PATH; contract: docs/design/network-foundation.md#multipath).
#include <crd/crypto/provider.hpp>
#include <crd/net/quic_connection_ids.hpp>
#include <crd/net/wire.hpp>

#include <cstring>

namespace crd::net
{
namespace
{

constexpr usize kIssueAttempts = 4U; // random IDs a host draws before it gives up on a full table
constexpr u64 kMaxPathId = 0xFFFFFFFFU;
constexpr usize kLocalSlots = 2U * kQuicMaxLocalConnectionIds;

[[nodiscard]] u64 path_of(const QuicFrame& frame) noexcept
{
    // NEW_CONNECTION_ID and RETIRE_CONNECTION_ID are path ID 0's (RFC 10069 sections 4.4 and 4.5).
    return (frame.kind == QuicFrameKind::PathNewConnectionId) ? frame.path_id : 0U;
}

[[nodiscard]] bool same_token(const QuicPeerCid& held, cont::ConstSpan<u8> token) noexcept
{
    return held.has_token && token.size() == crypto::kQuicResetTokenBytes &&
           std::memcmp(held.reset_token, token.data(), crypto::kQuicResetTokenBytes) == 0;
}

} // namespace

// ---- The host side ----

bool QuicRoutedIds::issue(cont::Span<u8> id, cont::Span<u8> token) noexcept
{
    if (id.size() < 8U || id.size() > crypto::kQuicMaxConnectionIdBytes)
    {
        return false;
    }
    for (usize attempt = 0U; attempt < kIssueAttempts; ++attempt)
    {
        if (crypto::random_bytes(id) != crypto::CryptoError::None)
        {
            return false;
        }
        const RouteError routed = m_routes.insert(id, m_connection);
        if (routed == RouteError::Exists)
        {
            continue;
        }
        if (routed != RouteError::None)
        {
            return false;
        }
        if (!this->token(id, token))
        {
            static_cast<void>(m_routes.erase(id));
            return false;
        }
        return true;
    }
    return false;
}

bool QuicRoutedIds::token(cont::ConstSpan<u8> id, cont::Span<u8> token) noexcept
{
    return m_reset.token(id, token) == crypto::CryptoError::None;
}

void QuicRoutedIds::retire(cont::ConstSpan<u8> id) noexcept
{
    static_cast<void>(m_routes.erase(id));
}

void QuicConnectionIds::watch_reset(cont::ConstSpan<u8>) noexcept {}

void QuicConnectionIds::unwatch_reset(cont::ConstSpan<u8>) noexcept {}

void QuicRoutedIds::watch_reset(cont::ConstSpan<u8> token) noexcept
{
    if (m_peer_tokens != nullptr)
    {
        static_cast<void>(m_peer_tokens->insert(token, m_connection)); // a token held already stays its first holder's
    }
}

void QuicRoutedIds::unwatch_reset(cont::ConstSpan<u8> token) noexcept
{
    if (m_peer_tokens != nullptr && m_peer_tokens->find(token) == m_connection)
    {
        static_cast<void>(m_peer_tokens->erase(token));
    }
}

// ---- Spaces ----

QuicCids::QuicCids(QuicConnectionIds* host) noexcept : m_host(host)
{
    m_first.m_open = true;
}

QuicCidSpace* QuicCids::space(u64 path_id) noexcept
{
    for (QuicCidSpace* held : m_spaces)
    {
        if (held != nullptr && held->m_path_id == path_id)
        {
            return held;
        }
    }
    return nullptr;
}

const QuicCidSpace* QuicCids::space(u64 path_id) const noexcept
{
    for (const QuicCidSpace* held : m_spaces)
    {
        if (held != nullptr && held->m_path_id == path_id)
        {
            return held;
        }
    }
    return nullptr;
}

bool QuicCids::open(u64 path_id, QuicCidSpace& storage) noexcept
{
    if (path_id > kMaxPathId || is_open(path_id))
    {
        return false;
    }
    for (QuicCidSpace*& slot : m_spaces)
    {
        if (slot == nullptr)
        {
            storage = QuicCidSpace{};
            storage.m_path_id = path_id;
            storage.m_open = true;
            slot = &storage;
            return true;
        }
    }
    return false;
}

QuicCidSpace* QuicCids::close(u64 path_id) noexcept
{
    for (QuicCidSpace*& slot : m_spaces)
    {
        if (slot != nullptr && slot->m_path_id == path_id)
        {
            QuicCidSpace* closed = slot;
            unroute(*closed);
            while (closed->m_peer_count > 0U)
            {
                forget_peer(*closed, closed->m_peer_count - 1U);
            }
            closed->m_open = false;
            slot = nullptr;
            return closed;
        }
    }
    return nullptr;
}

QuicPeerCid* QuicCids::held(QuicCidSpace& space, u64 sequence) noexcept
{
    for (usize index = 0U; index < space.m_peer_count; ++index)
    {
        if (space.m_peer[index].sequence == sequence)
        {
            return &space.m_peer[index];
        }
    }
    return nullptr;
}

QuicLocalCid* QuicCids::own(QuicCidSpace& space, u64 sequence) noexcept
{
    for (usize index = 0U; index < space.m_local_count; ++index)
    {
        if (space.m_local[index].sequence == sequence)
        {
            return &space.m_local[index];
        }
    }
    return nullptr;
}

QuicCidSpace::Retired* QuicCids::retired(QuicCidSpace& space, u64 sequence) noexcept
{
    for (QuicCidSpace::Retired& entry : space.m_retired)
    {
        if (entry.sequence == sequence)
        {
            return &entry;
        }
    }
    return nullptr;
}

// ---- The peer's IDs ----

void QuicCids::watch(const QuicPeerCid& id) noexcept
{
    if (m_host != nullptr && id.has_token && id.used)
    {
        m_host->watch_reset(cont::ConstSpan<u8>(id.reset_token, crypto::kQuicResetTokenBytes));
    }
}

void QuicCids::unwatch(const QuicPeerCid& id) noexcept
{
    if (m_host != nullptr && id.has_token && id.used)
    {
        m_host->unwatch_reset(cont::ConstSpan<u8>(id.reset_token, crypto::kQuicResetTokenBytes));
    }
}

void QuicCids::forget_peer(QuicCidSpace& space, usize index) noexcept
{
    unwatch(space.m_peer[index]);
    space.m_current = (space.m_current == space.m_peer[index].sequence) ? kQuicNoSequence : space.m_current;
    for (usize at = index + 1U; at < space.m_peer_count; ++at)
    {
        space.m_peer[at - 1U] = space.m_peer[at];
    }
    --space.m_peer_count;
}

bool QuicCids::owe(QuicCidSpace& space, u64 sequence, u64 limit) noexcept
{
    QuicCidSpace::Retired* entry = retired(space, sequence);
    if (entry != nullptr && entry->state != QuicCidSpace::Retirement::Done)
    {
        return true; // owed or in flight already
    }
    usize outstanding = 0U;
    QuicCidSpace::Retired* free = nullptr;
    for (QuicCidSpace::Retired& other : space.m_retired)
    {
        if (other.sequence != kQuicNoSequence && other.state != QuicCidSpace::Retirement::Done)
        {
            ++outstanding;
            continue;
        }
        // An empty entry first, else the oldest settled one, which a repeated frame is least likely to name.
        if (free == nullptr || (free->sequence != kQuicNoSequence &&
                                (other.sequence == kQuicNoSequence || other.sequence < free->sequence)))
        {
            free = &other;
        }
    }
    // Retirements owed and in flight stay within twice the limit (RFC 9000 section 5.1.2), which is at most the
    // entries a space holds, so one is free below it.
    if (outstanding >= 2U * limit || free == nullptr)
    {
        return false;
    }
    free = (entry != nullptr) ? entry : free; // a settled retirement of the same ID is owed again in its own entry
    free->sequence = sequence;
    free->state = QuicCidSpace::Retirement::Owed;
    return true;
}

void QuicCids::first_peer(cont::ConstSpan<u8> id) noexcept
{
    m_first.m_peer[0] = QuicPeerCid{};
    static_cast<void>(m_first.m_peer[0].id.assign(id)); // a header's, bounded to 20 bytes
    m_first.m_peer_count = 1U;
    m_first.m_current = 0U;
}

QuicTransportError QuicCids::add_peer(const QuicFrame& frame, u64 limit, bool& added) noexcept
{
    added = false;
    QuicCidSpace* target = space(path_of(frame));
    if (target == nullptr)
    {
        return QuicTransportError::NoError; // a space closed: the frame is ignored (RFC 10069 section 4)
    }
    for (QuicCidSpace* other : m_spaces)
    {
        for (usize index = 0U; other != nullptr && index < other->m_peer_count; ++index)
        {
            const QuicPeerCid& id = other->m_peer[index];
            const bool same_id = wire::wire_same_bytes(id.id.view(), frame.data);
            if (other == target && id.sequence == frame.sequence)
            {
                // A repeat changes nothing; another ID or token under a sequence held is PROTOCOL_VIOLATION.
                const bool same = same_id && (!id.has_token || same_token(id, frame.reset_token));
                return same ? QuicTransportError::NoError : QuicTransportError::ProtocolViolation;
            }
            // One ID under two sequences or path IDs, or one token under two IDs (RFC 9000 section 10.3.2).
            if (same_id || same_token(id, frame.reset_token))
            {
                return QuicTransportError::ProtocolViolation;
            }
        }
    }
    if (frame.sequence < target->m_retire_prior_to)
    {
        // Retired already by an earlier Retire Prior To, so it is retired again (section 5.1.2).
        return owe(*target, frame.sequence, limit) ? QuicTransportError::NoError
                                                   : QuicTransportError::ConnectionIdLimitError;
    }
    if (retired(*target, frame.sequence) != nullptr)
    {
        return QuicTransportError::NoError; // retired here alone: its repeated frame is not held again
    }
    if (frame.retire_prior_to > target->m_retire_prior_to)
    {
        target->m_retire_prior_to = frame.retire_prior_to;
        usize index = 0U;
        while (index < target->m_peer_count)
        {
            const u64 sequence = target->m_peer[index].sequence;
            if (sequence >= target->m_retire_prior_to)
            {
                ++index;
                continue;
            }
            forget_peer(*target, index); // the one sent to is replaced by the frame's
            if (!owe(*target, sequence, limit))
            {
                return QuicTransportError::ConnectionIdLimitError;
            }
        }
    }
    if (target->m_peer_count >= limit || target->m_peer_count >= kQuicMaxPeerConnectionIds)
    {
        return QuicTransportError::ConnectionIdLimitError;
    }
    QuicPeerCid& id = target->m_peer[target->m_peer_count++];
    id = QuicPeerCid{};
    id.sequence = frame.sequence;
    static_cast<void>(id.id.assign(frame.data)); // read_quic_frame bounds it to 20 bytes
    std::memcpy(id.reset_token, frame.reset_token.data(), crypto::kQuicResetTokenBytes);
    id.has_token = true;
    target->m_current = (target->m_current == kQuicNoSequence) ? frame.sequence : target->m_current;
    added = true;
    return QuicTransportError::NoError;
}

void QuicCids::peer_reset_token(cont::ConstSpan<u8> token) noexcept
{
    QuicCidSpace* first = space(0U);
    QuicPeerCid* id = (first == nullptr) ? nullptr : held(*first, 0U);
    if (id != nullptr && token.size() == crypto::kQuicResetTokenBytes)
    {
        unwatch(*id); // a token learnt again replaces the one watched
        std::memcpy(id->reset_token, token.data(), crypto::kQuicResetTokenBytes);
        id->has_token = true;
        watch(*id);
    }
}

usize QuicCids::peer_count(u64 path_id) const noexcept
{
    const QuicCidSpace* held = space(path_id);
    return (held == nullptr) ? 0U : held->m_peer_count;
}

const QuicPeerCid* QuicCids::peer_at(u64 path_id, usize index) const noexcept
{
    const QuicCidSpace* held = space(path_id);
    return (held == nullptr || index >= held->m_peer_count) ? nullptr : &held->m_peer[index];
}

const QuicPeerCid* QuicCids::peer(u64 path_id, u64 sequence) const noexcept
{
    const QuicCidSpace* held = space(path_id);
    for (usize index = 0U; held != nullptr && index < held->m_peer_count; ++index)
    {
        if (held->m_peer[index].sequence == sequence)
        {
            return &held->m_peer[index];
        }
    }
    return nullptr;
}

u64 QuicCids::current(u64 path_id) const noexcept
{
    const QuicCidSpace* held = space(path_id);
    return (held == nullptr) ? kQuicNoSequence : held->m_current;
}

void QuicCids::set_current(u64 path_id, u64 sequence) noexcept
{
    QuicCidSpace* target = space(path_id);
    if (target != nullptr && held(*target, sequence) != nullptr)
    {
        target->m_current = sequence;
    }
}

void QuicCids::use(u64 path_id, u64 sequence) noexcept
{
    QuicCidSpace* target = space(path_id);
    QuicPeerCid* id = (target == nullptr) ? nullptr : held(*target, sequence);
    if (id != nullptr && !id->used)
    {
        id->used = true;
        watch(*id);
    }
}

bool QuicCids::retire_peer(u64 path_id, u64 sequence, u64 limit) noexcept
{
    QuicCidSpace* target = space(path_id);
    for (usize index = 0U; target != nullptr && index < target->m_peer_count; ++index)
    {
        if (target->m_peer[index].sequence == sequence)
        {
            forget_peer(*target, index);
            return owe(*target, sequence, limit);
        }
    }
    return true;
}

bool QuicCids::reset_by(cont::ConstSpan<u8> tail) const noexcept
{
    // Every token of a used, unretired ID is compared in constant time, none skipped once one matches.
    bool matched = false;
    for (const QuicCidSpace* held : m_spaces)
    {
        for (usize index = 0U; held != nullptr && index < held->m_peer_count; ++index)
        {
            const QuicPeerCid& id = held->m_peer[index];
            if (id.has_token && id.used)
            {
                const bool equal = crypto::constant_time_equal(
                    tail, cont::ConstSpan<u8>(id.reset_token, crypto::kQuicResetTokenBytes));
                matched = matched || equal;
            }
        }
    }
    return matched;
}

// ---- Retirements ----

bool QuicCids::retirement_owed() const noexcept
{
    u64 path_id = 0U;
    u64 sequence = 0U;
    return next_retirement(path_id, sequence);
}

bool QuicCids::next_retirement(u64& path_id, u64& sequence) const noexcept
{
    for (const QuicCidSpace* held : m_spaces)
    {
        if (held == nullptr)
        {
            continue;
        }
        for (const QuicCidSpace::Retired& entry : held->m_retired)
        {
            if (entry.sequence != kQuicNoSequence && entry.state == QuicCidSpace::Retirement::Owed)
            {
                path_id = held->m_path_id;
                sequence = entry.sequence;
                return true;
            }
        }
    }
    return false;
}

void QuicCids::retirement_sent(u64 path_id, u64 sequence) noexcept
{
    QuicCidSpace* target = space(path_id);
    QuicCidSpace::Retired* entry = (target == nullptr) ? nullptr : retired(*target, sequence);
    if (entry != nullptr && entry->state == QuicCidSpace::Retirement::Owed)
    {
        entry->state = QuicCidSpace::Retirement::InFlight;
    }
}

void QuicCids::retirement_settled(u64 path_id, u64 sequence, bool acked) noexcept
{
    QuicCidSpace* target = space(path_id);
    QuicCidSpace::Retired* entry = (target == nullptr) ? nullptr : retired(*target, sequence);
    if (entry != nullptr && entry->state == QuicCidSpace::Retirement::InFlight)
    {
        entry->state = acked ? QuicCidSpace::Retirement::Done : QuicCidSpace::Retirement::Owed;
    }
}

// ---- This side's IDs ----

void QuicCids::first_local(cont::ConstSpan<u8> id, cont::ConstSpan<u8> token) noexcept
{
    QuicLocalCid& first = m_first.m_local[0];
    first = QuicLocalCid{};
    static_cast<void>(first.id.assign(id)); // bounded to 20 bytes by every caller
    if (token.size() == crypto::kQuicResetTokenBytes)
    {
        std::memcpy(first.reset_token, token.data(), crypto::kQuicResetTokenBytes);
    }
    first.acked = true;
    m_first.m_local_count = 1U;
    m_first.m_local_next = 1U;
    m_local_bytes = first.id.bytes;
}

usize QuicCids::active(const QuicCidSpace& space) const noexcept
{
    usize count = 0U;
    for (usize index = 0U; index < space.m_local_count; ++index)
    {
        count += (space.m_local[index].sequence >= space.m_local_retire_prior_to) ? 1U : 0U;
    }
    return count;
}

bool QuicCids::issue(u64 path_id, bool announced) noexcept
{
    QuicCidSpace* target = space(path_id);
    if (m_host == nullptr || m_local_bytes == 0U || target == nullptr || target->m_local_count >= kLocalSlots ||
        target->m_local_next >= kQuicMaxVarint)
    {
        return false;
    }
    QuicLocalCid& id = target->m_local[target->m_local_count];
    id = QuicLocalCid{};
    id.id.bytes = m_local_bytes;
    if (!m_host->issue(cont::Span<u8>(id.id.id, id.id.bytes),
                       cont::Span<u8>(id.reset_token, crypto::kQuicResetTokenBytes)))
    {
        return false;
    }
    id.sequence = target->m_local_next++;
    id.owed = !announced;
    id.acked = announced;
    ++target->m_local_count;
    return true;
}

void QuicCids::top_up(u64 limit) noexcept
{
    const usize target = static_cast<usize>((limit < kQuicMaxLocalConnectionIds) ? limit : kQuicMaxLocalConnectionIds);
    for (QuicCidSpace* held : m_spaces)
    {
        if (held == nullptr)
        {
            continue;
        }
        usize count = active(*held);
        while (count < target && issue(held->m_path_id))
        {
            ++count;
        }
    }
}

bool QuicCids::rotate(u64 path_id, u64 limit) noexcept
{
    QuicCidSpace* target = space(path_id);
    if (target == nullptr || m_host == nullptr || m_local_bytes == 0U)
    {
        return false;
    }
    // An earlier Retire Prior To is retired in full first (section 5.1.2's SHOULD NOT).
    for (usize index = 0U; index < target->m_local_count; ++index)
    {
        if (target->m_local[index].sequence < target->m_local_retire_prior_to)
        {
            return false;
        }
    }
    const u64 earlier = target->m_local_retire_prior_to;
    const usize before = target->m_local_count;
    target->m_local_retire_prior_to = target->m_local_next;
    const usize wanted = static_cast<usize>((limit < kQuicMaxLocalConnectionIds) ? limit : kQuicMaxLocalConnectionIds);
    usize count = 0U;
    while (count < wanted && issue(path_id))
    {
        ++count;
    }
    if (target->m_local_count == before)
    {
        target->m_local_retire_prior_to = earlier;
        return false;
    }
    return true;
}

QuicTransportError QuicCids::retire_local(u64 path_id, u64 sequence, QuicCidRef packet) noexcept
{
    QuicCidSpace* target = space(path_id);
    if (target == nullptr)
    {
        return QuicTransportError::NoError; // a space closed: the frame is ignored (RFC 10069 section 4)
    }
    // Never toward zero-length IDs, never past the IDs issued, never the one the packet was sent to (section 19.16).
    if (m_local_bytes == 0U || sequence >= target->m_local_next ||
        (packet.found() && packet.path_id == path_id && packet.sequence == sequence))
    {
        return QuicTransportError::ProtocolViolation;
    }
    for (usize index = 0U; index < target->m_local_count; ++index)
    {
        if (target->m_local[index].sequence != sequence)
        {
            continue;
        }
        if (m_host != nullptr)
        {
            m_host->retire(target->m_local[index].id.view());
        }
        for (usize at = index + 1U; at < target->m_local_count; ++at)
        {
            target->m_local[at - 1U] = target->m_local[at];
        }
        --target->m_local_count;
        break; // the replacement is issued by the next top_up() (section 5.1.1)
    }
    return QuicTransportError::NoError;
}

QuicCidRef QuicCids::find(cont::ConstSpan<u8> id) const noexcept
{
    for (const QuicCidSpace* held : m_spaces)
    {
        for (usize index = 0U; held != nullptr && index < held->m_local_count; ++index)
        {
            if (wire::wire_same_bytes(held->m_local[index].id.view(), id))
            {
                return QuicCidRef{held->m_path_id, held->m_local[index].sequence};
            }
        }
    }
    return QuicCidRef{};
}

const QuicLocalCid* QuicCids::local(u64 path_id, u64 sequence) const noexcept
{
    const QuicCidSpace* held = space(path_id);
    for (usize index = 0U; held != nullptr && index < held->m_local_count; ++index)
    {
        if (held->m_local[index].sequence == sequence)
        {
            return &held->m_local[index];
        }
    }
    return nullptr;
}

usize QuicCids::local_count(u64 path_id) const noexcept
{
    const QuicCidSpace* held = space(path_id);
    return (held == nullptr) ? 0U : held->m_local_count;
}

u64 QuicCids::local_retire_prior_to(u64 path_id) const noexcept
{
    const QuicCidSpace* held = space(path_id);
    return (held == nullptr) ? 0U : held->m_local_retire_prior_to;
}

bool QuicCids::local_owed() const noexcept
{
    u64 path_id = 0U;
    return next_owed(path_id) != nullptr;
}

const QuicLocalCid* QuicCids::next_owed(u64& path_id) const noexcept
{
    for (const QuicCidSpace* held : m_spaces)
    {
        for (usize index = 0U; held != nullptr && index < held->m_local_count; ++index)
        {
            if (held->m_local[index].owed)
            {
                path_id = held->m_path_id;
                return &held->m_local[index];
            }
        }
    }
    return nullptr;
}

void QuicCids::local_sent(u64 path_id, u64 sequence) noexcept
{
    QuicCidSpace* target = space(path_id);
    QuicLocalCid* id = (target == nullptr) ? nullptr : own(*target, sequence);
    if (id != nullptr)
    {
        id->owed = false;
    }
}

void QuicCids::local_settled(u64 path_id, u64 sequence, bool acked) noexcept
{
    // Resent until acknowledged while held (section 13.3), a rotated one too: it asks the peer to retire.
    QuicCidSpace* target = space(path_id);
    QuicLocalCid* id = (target == nullptr) ? nullptr : own(*target, sequence);
    if (id != nullptr)
    {
        id->acked = id->acked || acked;
        id->owed = !id->acked;
    }
}

void QuicCids::reannounce() noexcept
{
    for (QuicCidSpace* held : m_spaces)
    {
        for (usize index = 0U; held != nullptr && index < held->m_local_count; ++index)
        {
            QuicLocalCid& id = held->m_local[index];
            id.owed = id.owed || !id.acked;
        }
    }
}

void QuicCids::unroute(QuicCidSpace& space) noexcept
{
    for (usize index = 0U; index < space.m_local_count; ++index)
    {
        if (m_host != nullptr && space.m_local[index].id.bytes != 0U)
        {
            m_host->retire(space.m_local[index].id.view());
        }
    }
    space.m_local_count = 0U;
}

void QuicCids::finish() noexcept
{
    for (QuicCidSpace* held : m_spaces)
    {
        if (held == nullptr)
        {
            continue;
        }
        unroute(*held);
        for (usize index = 0U; index < held->m_peer_count; ++index)
        {
            unwatch(held->m_peer[index]);
            held->m_peer[index].used = false;
        }
    }
}

} // namespace crd::net
