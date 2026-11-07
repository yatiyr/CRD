// crd-net -- reading frames off their carriers (NET.3; docs/design/network-foundation.md#frames).

#include <crd/net/reassembly.hpp>

#include <algorithm>
#include <cstring>

namespace crd::net
{

// ---- Streams ----

StreamAssembler::~StreamAssembler()
{
    if (m_state == State::Body)
    {
        m_admission.finish(m_frame);
    }
    if (m_opened)
    {
        m_admission.close_stream();
    }
}

FrameStep StreamAssembler::conclude(FrameStep step) noexcept
{
    step.action = admit_action(step.verdict, true);
    m_admission.count_verdict(step.verdict);
    if (step.action == AdmitAction::Close)
    {
        m_state = State::Closed;
        m_closed_by = step.verdict;
    }
    return step;
}

FrameStep StreamAssembler::feed(cont::ConstSpan<u8> bytes, crypto::Delivery delivery, u64 now_ns) noexcept
{
    switch (m_state)
    {
        case State::Preamble:
        case State::Header:
            return read_head(bytes, delivery, now_ns);
        case State::Body:
            return read_body(bytes);
        case State::StepOver:
        {
            FrameStep step;
            step.consumed = skip_bytes(bytes);
            return conclude(step);
        }
        case State::Closed:
            break;
    }
    // A stream that was refused or closed stays so, whatever else arrives on it.
    FrameStep step;
    step.verdict = m_closed_by;
    step.action = admit_action(m_closed_by, true);
    step.consumed = bytes.size();
    return step;
}

FrameStep StreamAssembler::read_head(cont::ConstSpan<u8> bytes, crypto::Delivery delivery, u64 now_ns) noexcept
{
    // The header in one piece: the input alone, or what an earlier feed left with as much of the input as it can hold.
    cont::ConstSpan<u8> view = bytes;
    usize from_input = bytes.size();
    if (m_carried != 0U)
    {
        from_input = std::min(bytes.size(), sizeof(m_carry) - m_carried);
        if (from_input != 0U)
        {
            std::memcpy(m_carry + m_carried, bytes.data(), from_input);
        }
        view = cont::ConstSpan<u8>(m_carry, m_carried + from_input);
    }
    return (m_state == State::Preamble) ? read_preamble(view, from_input) : read_frame(view, bytes, delivery, now_ns);
}

FrameStep StreamAssembler::read_preamble(cont::ConstSpan<u8> view, usize from_input) noexcept
{
    FrameStep step;
    Route route;
    usize used = 0U;
    const FrameRead read = read_route(view, route, used);
    if (read == FrameRead::Short)
    {
        // A short view fits: a route is shorter than the carry.
        std::memmove(m_carry, view.data(), view.size());
        m_carried = view.size();
        step.consumed = from_input;
        return conclude(step);
    }
    if (read == FrameRead::Malformed)
    {
        step.verdict = AdmitVerdict::Malformed;
        return conclude(step);
    }
    const AdmitVerdict verdict = m_admission.open_stream(route);
    if (verdict != AdmitVerdict::Admitted)
    {
        // Skip here is the stream's own end: the caller resets it.
        step.verdict = verdict;
        m_state = State::Closed;
        m_closed_by = verdict;
        return conclude(step);
    }
    m_opened = true;
    m_channel = route.channel;
    m_state = State::Header;
    step.consumed = used - m_carried;
    m_carried = 0U;
    return conclude(step);
}

FrameStep StreamAssembler::read_frame(cont::ConstSpan<u8> view, cont::ConstSpan<u8> bytes, crypto::Delivery delivery,
                                      u64 now_ns) noexcept
{
    FrameStep step;
    ParsedFrame parsed;
    step.verdict = m_admission.read_frame(m_channel, HeaderForm::Sized, view, parsed);
    if (step.verdict == AdmitVerdict::NeedMore)
    {
        // A header that is still short after kMaxFrameHeaderBytes does not exist: its numbers have bounded widths.
        std::memmove(m_carry, view.data(), view.size());
        step.consumed = view.size() - m_carried;
        m_carried = view.size();
        return conclude(step);
    }
    if (step.verdict == AdmitVerdict::NewerMessage)
    {
        if (!m_admission.charge_stepped_over(parsed, now_ns, step.retry_at_ns))
        {
            step.verdict = AdmitVerdict::PrincipalBytes;
            return conclude(step);
        }
        const usize header_from_input = parsed.header_bytes - m_carried;
        step.consumed = header_from_input;
        m_carried = 0U;
        m_skip = parsed.header.length;
        step.consumed += skip_bytes(bytes.subspan(header_from_input));
        return conclude(step);
    }
    if (step.verdict != AdmitVerdict::Admitted)
    {
        return conclude(step);
    }

    const usize header_from_input = parsed.header_bytes - m_carried;
    const cont::ConstSpan<u8> rest = bytes.subspan(header_from_input);
    const u32 length = parsed.header.length;
    const bool reassembled = rest.size() < length;

    step.verdict = m_admission.admit(m_channel, parsed, delivery, now_ns, reassembled, step);
    if (step.verdict == AdmitVerdict::Admitted && reassembled)
    {
        step.frame.buffer = m_admission.allocate_buffer(length);
        if (step.frame.buffer == nullptr)
        {
            m_admission.finish(step.frame);
            step.verdict = AdmitVerdict::NoMemory;
            step.retry_at_ns = kNeverNs;
        }
    }
    if (admit_action(step.verdict, true) == AdmitAction::Wait)
    {
        return conclude(step); // nothing consumed: the same bytes come again, with what is carried
    }

    m_carried = 0U;
    step.consumed = header_from_input;
    if (step.verdict != AdmitVerdict::Admitted)
    {
        // S7 refused: nothing reads the body.
        m_skip = length;
        step.consumed += skip_bytes(rest);
    }
    else if (!reassembled)
    {
        step.frame.body = rest.first(length);
        step.consumed += length;
    }
    else
    {
        if (!rest.empty())
        {
            std::memcpy(step.frame.buffer, rest.data(), rest.size());
        }
        m_filled = static_cast<u32>(rest.size());
        m_frame = step.frame;
        m_state = State::Body;
        step.consumed += rest.size();
        step.verdict = AdmitVerdict::NeedMore;
        step.frame = AdmittedFrame{};
    }
    return conclude(step);
}

FrameStep StreamAssembler::read_body(cont::ConstSpan<u8> bytes) noexcept
{
    FrameStep step;
    const usize take = std::min(bytes.size(), static_cast<usize>(m_frame.length - m_filled));
    if (take != 0U)
    {
        std::memcpy(m_frame.buffer + m_filled, bytes.data(), take);
    }
    m_filled += static_cast<u32>(take);
    step.consumed = take;
    if (m_filled == m_frame.length)
    {
        m_frame.body = cont::ConstSpan<u8>(m_frame.buffer, m_frame.length);
        step.frame = m_frame;
        step.verdict = AdmitVerdict::Admitted;
        m_state = State::Header;
    }
    return conclude(step);
}

usize StreamAssembler::skip_bytes(cont::ConstSpan<u8> bytes) noexcept
{
    const usize take = std::min(bytes.size(), static_cast<usize>(m_skip));
    m_skip -= static_cast<u32>(take);
    m_state = (m_skip == 0U) ? State::Header : State::StepOver;
    return take;
}

// ---- Datagrams ----

DatagramAssembler::DatagramAssembler(Admission& admission, u32 channel, cont::Span<FragmentSlot> slots,
                                     u64 timeout_ns) noexcept
    : m_admission(admission), m_channel(channel), m_slots(slots), m_timeout_ns(timeout_ns)
{
    m_latest_wins = admission.channel(channel).desc->channel_class == wire::ChannelClass::State;
}

DatagramAssembler::~DatagramAssembler()
{
    for (FragmentSlot& slot : m_slots)
    {
        if (slot.used)
        {
            abandon(slot);
        }
    }
}

FrameStep DatagramAssembler::conclude(FrameStep step) noexcept
{
    step.action = admit_action(step.verdict, false);
    m_admission.count_verdict(step.verdict);
    return step;
}

FrameStep DatagramAssembler::next(DatagramCursor& cursor, crypto::Delivery delivery, u64 now_ns) noexcept
{
    sweep(now_ns);
    switch (cursor.route.form)
    {
        case RouteForm::Whole:
            return read_whole(cursor, delivery, now_ns);
        case RouteForm::Listed:
            return read_listed(cursor, delivery, now_ns);
        case RouteForm::Fragment:
            break;
    }
    return read_fragment(cursor, delivery, now_ns);
}

FrameStep DatagramAssembler::read_whole(DatagramCursor& cursor, crypto::Delivery delivery, u64 now_ns) noexcept
{
    FrameStep step;
    ParsedFrame parsed;
    const cont::ConstSpan<u8> bytes = cursor.bytes.subspan(cursor.at);
    cursor.more = false;
    cursor.at = cursor.bytes.size();
    step.consumed = bytes.size();
    step.verdict = m_admission.read_frame(m_channel, HeaderForm::ToEnd, bytes, parsed);
    if (step.verdict != AdmitVerdict::Admitted)
    {
        return conclude(step);
    }
    step.verdict = m_admission.admit(m_channel, parsed, delivery, now_ns, false, step);
    if (step.verdict == AdmitVerdict::Admitted)
    {
        step.frame.body = bytes.subspan(parsed.header_bytes);
    }
    return conclude(step);
}

FrameStep DatagramAssembler::read_listed(DatagramCursor& cursor, crypto::Delivery delivery, u64 now_ns) noexcept
{
    FrameStep step;
    ParsedFrame parsed;
    const cont::ConstSpan<u8> bytes = cursor.bytes.subspan(cursor.at);
    step.verdict = m_admission.read_frame(m_channel, HeaderForm::Sized, bytes, parsed);
    if (step.verdict == AdmitVerdict::NeedMore)
    {
        step.verdict = AdmitVerdict::Malformed;
    }
    const usize left = bytes.size() - std::min(bytes.size(), parsed.header_bytes);
    const bool held = step.verdict == AdmitVerdict::Admitted || step.verdict == AdmitVerdict::NewerMessage;
    if (!held || parsed.header.length > left || (cursor.first && parsed.header.length == left))
    {
        // Nothing past this header can be trusted, and one message is the Whole form's: one encoding per message.
        step.verdict = (!held) ? step.verdict : AdmitVerdict::Malformed;
        step.consumed = bytes.size();
        cursor.at = cursor.bytes.size();
        cursor.more = false;
        return conclude(step);
    }
    cursor.first = false;
    step.consumed = parsed.header_bytes + parsed.header.length;
    cursor.at += step.consumed;
    cursor.more = cursor.at < cursor.bytes.size();
    if (step.verdict == AdmitVerdict::Admitted)
    {
        step.verdict = m_admission.admit(m_channel, parsed, delivery, now_ns, false, step);
        if (step.verdict == AdmitVerdict::Admitted)
        {
            step.frame.body = bytes.subspan(parsed.header_bytes, parsed.header.length);
        }
    }
    return conclude(step);
}

void DatagramAssembler::sweep(u64 now_ns) noexcept
{
    for (FragmentSlot& slot : m_slots)
    {
        if (slot.used && slot.expires_ns <= now_ns)
        {
            abandon(slot);
        }
    }
}

void DatagramAssembler::abandon(FragmentSlot& slot) noexcept
{
    mark_done(slot.sequence);
    m_admission.finish(slot.frame);
    slot = FragmentSlot{};
}

FragmentSlot* DatagramAssembler::find_slot(u64 sequence) noexcept
{
    for (FragmentSlot& slot : m_slots)
    {
        if (slot.used && slot.sequence == sequence)
        {
            return &slot;
        }
    }
    return nullptr;
}

void DatagramAssembler::mark_done(u64 sequence) noexcept
{
    if (!m_any_done)
    {
        m_any_done = true;
        m_done_high = sequence;
        return;
    }
    if (sequence > m_done_high)
    {
        const u64 ahead = sequence - m_done_high;
        m_done_window = (ahead >= 64U) ? 0U : (m_done_window << ahead);
        if (ahead <= 64U)
        {
            m_done_window |= u64{1} << (ahead - 1U); // the old highest
        }
        m_done_high = sequence;
    }
    else if (sequence < m_done_high && (m_done_high - sequence) <= 64U)
    {
        m_done_window |= u64{1} << (m_done_high - sequence - 1U);
    }
}

bool DatagramAssembler::is_done(u64 sequence) const noexcept
{
    if (!m_any_done)
    {
        return false;
    }
    if (sequence >= m_done_high)
    {
        return sequence == m_done_high;
    }
    // Below the highest: on a latest-wins channel it is superseded, and past the window nobody can tell, so it is
    // stale.
    const u64 behind = m_done_high - sequence;
    return m_latest_wins || behind > 64U || ((m_done_window >> (behind - 1U)) & 1U) != 0U;
}

AdmitVerdict DatagramAssembler::open_slot(const ParsedFrame& parsed, crypto::Delivery delivery, u64 now_ns,
                                          FrameStep& step, FragmentSlot*& slot) noexcept
{
    // A free slot, or the oldest message's when this one is newer than it. The room is made first, since what it gives
    // back may be what this message's reservation needs.
    FragmentSlot* free_slot = nullptr;
    FragmentSlot* oldest = nullptr;
    for (FragmentSlot& candidate : m_slots)
    {
        if (!candidate.used)
        {
            free_slot = (free_slot == nullptr) ? &candidate : free_slot;
        }
        else if (oldest == nullptr || candidate.sequence < oldest->sequence)
        {
            oldest = &candidate;
        }
    }
    if (free_slot == nullptr)
    {
        if (oldest == nullptr || oldest->sequence > parsed.header.sequence)
        {
            step.retry_at_ns = kNeverNs;
            return AdmitVerdict::ReassemblyFull;
        }
        abandon(*oldest);
        free_slot = oldest;
    }

    const AdmitVerdict verdict = m_admission.admit(m_channel, parsed, delivery, now_ns, true, step);
    if (verdict != AdmitVerdict::Admitted)
    {
        return verdict;
    }
    step.frame.buffer = m_admission.allocate_buffer(parsed.header.length);
    if (step.frame.buffer == nullptr)
    {
        m_admission.finish(step.frame);
        step.retry_at_ns = kNeverNs;
        return AdmitVerdict::NoMemory;
    }
    free_slot->used = true;
    free_slot->sequence = parsed.header.sequence;
    free_slot->expires_ns = (m_timeout_ns > kNeverNs - now_ns) ? kNeverNs : (now_ns + m_timeout_ns);
    free_slot->chunk = parsed.header.chunk;
    free_slot->pieces = fragment_count(parsed.header.length, parsed.header.chunk);
    free_slot->received = 0U;
    free_slot->delivery = delivery;
    free_slot->frame = step.frame;
    step.frame = AdmittedFrame{};
    slot = free_slot;
    return AdmitVerdict::Admitted;
}

FrameStep DatagramAssembler::read_fragment(DatagramCursor& cursor, crypto::Delivery delivery, u64 now_ns) noexcept
{
    FrameStep step;
    ParsedFrame parsed;
    const cont::ConstSpan<u8> bytes = cursor.bytes.subspan(cursor.at);
    cursor.more = false;
    cursor.at = cursor.bytes.size();
    step.consumed = bytes.size();
    step.verdict = m_admission.read_frame(m_channel, HeaderForm::Fragment, bytes, parsed);
    if (step.verdict != AdmitVerdict::Admitted)
    {
        return conclude(step);
    }

    const FrameHeader& header = parsed.header;
    const cont::ConstSpan<u8> piece = bytes.subspan(parsed.header_bytes);
    if (!fragment_shape_holds(header.length, header.chunk, header.index, piece.size()))
    {
        step.verdict = AdmitVerdict::BadFragment;
        return conclude(step);
    }
    if (is_done(header.sequence))
    {
        step.verdict = AdmitVerdict::StaleFragment;
        return conclude(step);
    }

    FragmentSlot* slot = find_slot(header.sequence);
    if (slot == nullptr)
    {
        step.verdict = open_slot(parsed, delivery, now_ns, step, slot);
        if (step.verdict != AdmitVerdict::Admitted)
        {
            return conclude(step);
        }
    }
    else if (slot->frame.message != parsed.message || slot->frame.object != header.object ||
             slot->frame.length != header.length || slot->chunk != header.chunk || slot->delivery != delivery)
    {
        step.verdict = AdmitVerdict::FragmentConflict;
        return conclude(step);
    }

    u64& word = slot->seen[header.index / 64U];
    const u64 bit = u64{1} << (header.index % 64U);
    if ((word & bit) != 0U)
    {
        step.verdict = AdmitVerdict::DuplicateFragment;
        return conclude(step);
    }
    word |= bit;
    std::memcpy(slot->frame.buffer + (u64{header.index} * slot->chunk), piece.data(), piece.size());
    ++slot->received;
    if (slot->received != slot->pieces)
    {
        step.verdict = AdmitVerdict::Buffered;
        return conclude(step);
    }

    step.frame = slot->frame;
    step.frame.body = cont::ConstSpan<u8>(step.frame.buffer, step.frame.length);
    step.verdict = AdmitVerdict::Admitted;
    const u64 done = slot->sequence;
    *slot = FragmentSlot{};
    mark_done(done);
    if (m_latest_wins)
    {
        // A newer message is whole: every older one is superseded.
        for (FragmentSlot& older : m_slots)
        {
            if (older.used && older.sequence < done)
            {
                abandon(older);
            }
        }
    }
    return conclude(step);
}

} // namespace crd::net
