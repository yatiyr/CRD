// crd-net -- QUIC's ECN validation (NET-PATH; docs/design/network-foundation.md#ecn).

#include <crd/net/quic_ecn.hpp>

namespace crd::net
{

void QuicEcnValidator::start(Ecn codepoint) noexcept
{
    *this = QuicEcnValidator{};
    if (codepoint == Ecn::Ect0 || codepoint == Ecn::Ect1)
    {
        m_codepoint = codepoint;
        m_state = QuicEcnState::Testing;
    }
}

Ecn QuicEcnValidator::marking(u64 now_ns, u64 testing_ns) noexcept
{
    if (m_state == QuicEcnState::Testing && m_testing_from_ns != kNeverNs && now_ns >= m_testing_from_ns &&
        now_ns - m_testing_from_ns >= testing_ns)
    {
        end_testing();
    }
    return (m_state == QuicEcnState::Testing || m_state == QuicEcnState::Capable) ? m_codepoint : Ecn::NotEct;
}

void QuicEcnValidator::on_sent(QuicSpace space, Ecn ecn, u64 now_ns) noexcept
{
    if (ecn != Ecn::Ect0 && ecn != Ecn::Ect1)
    {
        return;
    }
    EcnCounts& sent = m_spaces[quic_space_index(space)].sent;
    if (ecn == Ecn::Ect0)
    {
        ++sent.ect0;
    }
    else
    {
        ++sent.ect1;
    }
    if (m_state != QuicEcnState::Testing)
    {
        return;
    }
    ++m_unvalidated;
    m_testing_from_ns = (m_testing_from_ns == kNeverNs) ? now_ns : m_testing_from_ns;
    if (m_unvalidated >= kQuicEcnTestingPackets)
    {
        end_testing();
    }
}

void QuicEcnValidator::on_gone(Ecn ecn) noexcept
{
    if (ecn == Ecn::NotEct || (m_state != QuicEcnState::Testing && m_state != QuicEcnState::Unknown))
    {
        return;
    }
    ++m_unvalidated_gone;
    if (m_state == QuicEcnState::Unknown && m_unvalidated_gone >= m_unvalidated)
    {
        fail(QuicEcnFailure::Lost);
    }
}

bool QuicEcnValidator::on_ack(QuicSpace id, const QuicAck& ack, const QuicAckOutcome& outcome) noexcept
{
    if (m_state == QuicEcnState::Off || m_state == QuicEcnState::Failed || !outcome.advanced)
    {
        return false;
    }
    Space& space = m_spaces[quic_space_index(id)];
    const u64 marked = outcome.ect0_packets + outcome.ect1_packets;
    if (!ack.ecn)
    {
        if (marked != 0U)
        {
            fail(QuicEcnFailure::Missing);
        }
        return false;
    }
    const EcnCounts& was = space.reported;
    if (ack.ect0 < was.ect0 || ack.ect1 < was.ect1 || ack.ce < was.ce)
    {
        fail(QuicEcnFailure::Decreased);
        return false;
    }
    // Counts are varints below 2^62, so their sums do not wrap.
    if (ack.ect0 > space.sent.ect0 || ack.ect1 > space.sent.ect1 ||
        ack.ect0 + ack.ect1 + ack.ce > space.sent.ect0 + space.sent.ect1)
    {
        fail(QuicEcnFailure::Exceeded);
        return false;
    }
    const u64 ce_rise = ack.ce - was.ce;
    if (ack.ect0 - was.ect0 + ce_rise < outcome.ect0_packets || ack.ect1 - was.ect1 + ce_rise < outcome.ect1_packets)
    {
        fail(QuicEcnFailure::Short);
        return false;
    }
    space.reported = EcnCounts{ack.ect0, ack.ect1, ack.ce};
    if (marked != 0U && m_state != QuicEcnState::Capable)
    {
        m_state = QuicEcnState::Capable;
    }
    return ce_rise != 0U && m_state == QuicEcnState::Capable;
}

const EcnCounts& QuicEcnValidator::reported(QuicSpace space) const noexcept
{
    return m_spaces[quic_space_index(space)].reported;
}

void QuicEcnValidator::fail(QuicEcnFailure failure) noexcept
{
    m_state = QuicEcnState::Failed;
    m_failure = failure;
}

void QuicEcnValidator::end_testing() noexcept
{
    m_state = QuicEcnState::Unknown;
    if (m_unvalidated_gone >= m_unvalidated)
    {
        fail(QuicEcnFailure::Lost);
    }
}

} // namespace crd::net
