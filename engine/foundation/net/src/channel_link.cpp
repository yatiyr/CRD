// crd-net -- a session's channels over whichever path carries it (4.2a; docs/design/network-foundation.md#channels).

#include <crd/memory/construct.hpp>
#include <crd/net/channel_link.hpp>
#include <crd/net/frame.hpp>
#include <crd/net/webtransport.hpp>

#include <memory>
#include <new>

namespace crd::net
{

namespace
{

[[nodiscard]] ChannelError from_stream(QuicStreamError error) noexcept
{
    switch (error)
    {
        case QuicStreamError::None:
            return ChannelError::None;
        case QuicStreamError::Blocked:
        case QuicStreamError::NoSlot:
            return ChannelError::Blocked;
        case QuicStreamError::Invalid:
            return ChannelError::Invalid;
        case QuicStreamError::Unknown:
        case QuicStreamError::Ended:
            break;
    }
    return ChannelError::Closed;
}

[[nodiscard]] ChannelError from_http3(Http3Error error) noexcept
{
    switch (error)
    {
        case Http3Error::None:
            return ChannelError::None;
        case Http3Error::NoStream:
        case Http3Error::Blocked:
        case Http3Error::NotReady:
            return ChannelError::Blocked;
        case Http3Error::TooLarge:
            return ChannelError::TooLarge;
        case Http3Error::Invalid:
            return ChannelError::Invalid;
        case Http3Error::Closed:
        case Http3Error::GoingAway:
        case Http3Error::Unknown:
            break;
    }
    return ChannelError::Closed;
}

[[nodiscard]] ChannelError from_datagram(QuicDatagramError error) noexcept
{
    switch (error)
    {
        case QuicDatagramError::None:
            return ChannelError::None;
        case QuicDatagramError::TooLarge:
            return ChannelError::TooLarge;
        case QuicDatagramError::Full:
        case QuicDatagramError::NotReady:
            return ChannelError::Blocked;
        case QuicDatagramError::Unsupported:
            break;
    }
    return ChannelError::Closed;
}

[[nodiscard]] constexpr bool bidirectional_id(u64 id) noexcept
{
    return (id & 2U) == 0U;
}

// RFC 9218's urgencies stop at 7: a channel's lower priorities share the last.
[[nodiscard]] constexpr u8 urgency_of(const wire::ChannelDesc& desc) noexcept
{
    return (desc.priority < kQuicStreamUrgencies) ? desc.priority : static_cast<u8>(kQuicStreamUrgencies - 1U);
}

} // namespace

// ---- The carrier ----

ChannelError ChannelCarrier::open(bool bidirectional, u64& stream) noexcept
{
    return (m_streams != nullptr) ? from_stream(m_streams->open(bidirectional, stream))
                                  : from_http3(m_transport->open_stream(m_session, bidirectional, stream));
}

bool ChannelCarrier::can_open() const noexcept
{
    return (m_streams != nullptr) ? m_streams->can_open(false) : m_transport->can_open(m_session);
}

ChannelError ChannelCarrier::write(u64 stream, cont::ConstSpan<u8> data, bool fin, usize& taken) noexcept
{
    return (m_streams != nullptr) ? from_stream(m_streams->write(stream, data, fin, taken))
                                  : from_http3(m_transport->write(stream, data, fin, taken));
}

usize ChannelCarrier::send_room(u64 stream, usize wanted) noexcept
{
    return (m_streams != nullptr) ? m_streams->send_room(stream, wanted) : m_transport->send_room(stream, wanted);
}

ChannelError ChannelCarrier::set_urgency(u64 stream, u8 urgency) noexcept
{
    return (m_streams != nullptr) ? from_stream(m_streams->set_urgency(stream, urgency))
                                  : from_http3(m_transport->set_urgency(stream, urgency));
}

QuicStreamRead ChannelCarrier::peek(u64 stream) const noexcept
{
    return (m_streams != nullptr) ? m_streams->peek(stream) : m_transport->peek(stream);
}

ChannelError ChannelCarrier::consume(u64 stream, usize count) noexcept
{
    return (m_streams != nullptr) ? from_stream(m_streams->consume(stream, count))
                                  : from_http3(m_transport->consume(stream, count));
}

ChannelError ChannelCarrier::reset(u64 stream, u32 code) noexcept
{
    return (m_streams != nullptr) ? from_stream(m_streams->reset(stream, code))
                                  : from_http3(m_transport->reset(stream, code));
}

ChannelError ChannelCarrier::stop(u64 stream, u32 code) noexcept
{
    return (m_streams != nullptr) ? from_stream(m_streams->stop_sending(stream, code))
                                  : from_http3(m_transport->stop(stream, code));
}

ChannelError ChannelCarrier::send_datagram(cont::ConstSpan<u8> payload, u64 now_ns) noexcept
{
    if (m_streams == nullptr)
    {
        return from_http3(m_transport->send_datagram(m_session, payload, now_ns));
    }
    u64 id = 0U;
    return from_datagram(m_datagrams->queue(payload, now_ns, id));
}

usize ChannelCarrier::max_datagram() const noexcept
{
    return (m_streams != nullptr) ? m_datagrams->max_payload() : m_transport->max_datagram(m_session);
}

bool ChannelCarrier::poll(QuicStreamEvent& event) noexcept
{
    return m_streams->poll(event);
}

bool ChannelCarrier::peek_datagram(cont::ConstSpan<u8>& payload, bool& early) const noexcept
{
    early = m_datagrams->oldest_early();
    return m_datagrams->peek(payload);
}

void ChannelCarrier::pop_datagram() noexcept
{
    m_datagrams->pop();
}

// ---- The link ----

ChannelLink::~ChannelLink() noexcept
{
    if (m_allocator == nullptr)
    {
        return;
    }
    give_up_frame();
    for (u32 at = 0U; at < m_config.streams; ++at)
    {
        if (m_streams[at].used)
        {
            release(m_streams[at]);
        }
    }
    for (u32 at = 0U; at < m_channel_count; ++at)
    {
        if (m_datagrams[at].assembler != nullptr)
        {
            std::destroy_at(m_datagrams[at].assembler);
        }
    }
    memory::destroy_array(*m_allocator, m_streams, usize{m_config.streams});
    memory::destroy_array(*m_allocator, m_send, usize{m_channel_count});
    memory::destroy_array(*m_allocator, m_datagrams, usize{m_channel_count});
    memory::destroy_array(*m_allocator, m_slots, usize{m_slot_count});
    memory::destroy_array(*m_allocator, m_padding, m_padding_bytes);
}

bool ChannelLink::init(memory::IAllocator& allocator, Admission& admission, const ChannelCarrier& carrier,
                       const ChannelLinkConfig& config) noexcept
{
    const u32 channels = admission.channel_count();
    if (m_allocator != nullptr || channels == 0U || !carrier.valid() || config.streams == 0U ||
        config.streams > kChannelLinkMaxStreams || config.fragment_slots > kChannelLinkMaxStreams ||
        config.fragment_timeout_ns == 0U)
    {
        return false;
    }
    u32 unreliable = 0U;
    u32 largest_block = 0U;
    for (u32 channel = 0U; channel < channels; ++channel)
    {
        const wire::ChannelDesc& desc = *admission.channel(channel).desc;
        unreliable += desc.reliable ? 0U : 1U;
        largest_block = (desc.padding_block > largest_block) ? desc.padding_block : largest_block;
    }
    const usize padding_bytes =
        (carrier.pads_in_band() && largest_block != 0U) ? (usize{2} * largest_block) + kMaxFrameHeaderBytes : 0U;
    const usize slot_count = usize{unreliable} * config.fragment_slots;
    ReadStream* const streams = memory::try_construct_array<ReadStream>(allocator, usize{config.streams});
    SendChannel* const send = memory::try_construct_array<SendChannel>(allocator, usize{channels});
    DatagramChannel* const datagrams = memory::try_construct_array<DatagramChannel>(allocator, usize{channels});
    FragmentSlot* const slots = memory::try_construct_array<FragmentSlot>(allocator, slot_count);
    u8* const padding = memory::try_construct_array<u8>(allocator, padding_bytes);
    if (streams == nullptr || send == nullptr || datagrams == nullptr || (slot_count != 0U && slots == nullptr) ||
        (padding_bytes != 0U && padding == nullptr))
    {
        memory::destroy_array(allocator, streams, usize{config.streams});
        memory::destroy_array(allocator, send, usize{channels});
        memory::destroy_array(allocator, datagrams, usize{channels});
        memory::destroy_array(allocator, slots, slot_count);
        memory::destroy_array(allocator, padding, padding_bytes);
        return false;
    }
    m_allocator = &allocator;
    m_admission = &admission;
    m_carrier = carrier;
    m_config = config;
    m_streams = streams;
    m_send = send;
    m_datagrams = datagrams;
    m_slots = slots;
    m_padding = padding;
    m_padding_bytes = padding_bytes;
    m_channel_count = channels;
    m_slot_count = static_cast<u32>(slot_count);
    u32 first_slot = 0U;
    for (u32 channel = 0U; channel < channels; ++channel)
    {
        if (!admission.channel(channel).desc->reliable)
        {
            const cont::Span<FragmentSlot> pieces =
                (config.fragment_slots != 0U) ? cont::Span<FragmentSlot>(slots + first_slot, config.fragment_slots)
                                              : cont::Span<FragmentSlot>{};
            m_datagrams[channel].assembler = ::new (static_cast<void*>(m_datagrams[channel].storage))
                DatagramAssembler(admission, channel, pieces, config.fragment_timeout_ns);
            first_slot += config.fragment_slots;
        }
    }
    return true;
}

u32 ChannelLink::streams_held() const noexcept
{
    u32 held = 0U;
    for (u32 at = 0U; at < m_config.streams; ++at)
    {
        held += m_streams[at].used ? 1U : 0U;
    }
    return held;
}

ChannelLink::ReadStream* ChannelLink::find_stream(u64 id) noexcept
{
    for (u32 at = 0U; at < m_config.streams; ++at)
    {
        if (m_streams[at].used && m_streams[at].id == id)
        {
            return &m_streams[at];
        }
    }
    return nullptr;
}

ChannelLink::SendChannel* ChannelLink::find_sender(u64 stream) noexcept
{
    for (u32 at = 0U; at < m_channel_count; ++at)
    {
        if (m_send[at].open && m_send[at].stream == stream)
        {
            return &m_send[at];
        }
    }
    return nullptr;
}

void ChannelLink::push_ready(u32 index) noexcept
{
    ReadStream& stream = m_streams[index];
    if (stream.queued || stream.waiting)
    {
        return;
    }
    stream.queued = true;
    stream.next = kNone;
    if (m_ready_tail == kNone)
    {
        m_ready_head = index;
    }
    else
    {
        m_streams[m_ready_tail].next = index;
    }
    m_ready_tail = index;
}

u32 ChannelLink::pop_ready() noexcept
{
    const u32 index = m_ready_head;
    m_ready_head = m_streams[index].next;
    if (m_ready_head == kNone)
    {
        m_ready_tail = kNone;
    }
    m_streams[index].queued = false;
    return index;
}

void ChannelLink::refuse(u64 id) noexcept
{
    ++m_counters.streams_refused;
    static_cast<void>(m_carrier.stop(id, kChannelStreamRefused));
    if (bidirectional_id(id))
    {
        static_cast<void>(m_carrier.reset(id, kChannelStreamRefused));
    }
}

void ChannelLink::stream_opened(u64 id) noexcept
{
    if (m_failed)
    {
        return;
    }
    if (bidirectional_id(id))
    {
        refuse(id); // a channel runs one way: each side opens its own
        return;
    }
    u32 free_slot = kNone;
    for (u32 at = 0U; at < m_config.streams; ++at)
    {
        if (!m_streams[at].used)
        {
            free_slot = at;
            break;
        }
    }
    if (free_slot == kNone)
    {
        refuse(id);
        return;
    }
    ReadStream& stream = m_streams[free_slot];
    stream.id = id;
    stream.used = true;
    stream.early = false;
    stream.assembler = ::new (static_cast<void*>(stream.storage)) StreamAssembler(*m_admission);
    push_ready(free_slot);
}

void ChannelLink::stream_readable(u64 id) noexcept
{
    ReadStream* const stream = find_stream(id);
    if (stream != nullptr)
    {
        push_ready(static_cast<u32>(stream - m_streams));
    }
}

void ChannelLink::stream_aborted(u64 id) noexcept
{
    ReadStream* const stream = find_stream(id);
    if (stream != nullptr)
    {
        ++m_counters.streams_aborted;
        release(*stream);
    }
}

void ChannelLink::stream_stopped(u64 id, u64 code) noexcept
{
    SendChannel* const send = find_sender(id);
    if (send != nullptr)
    {
        send->open = false;
        send->pending = 0U;
        send->stop_code = static_cast<u32>(code);
        m_reports += send->stopped ? 0U : 1U;
        send->stopped = true;
    }
}

void ChannelLink::stream_writable(u64 id) noexcept
{
    SendChannel* const send = find_sender(id);
    if (send != nullptr && send->blocked)
    {
        send->blocked = false;
        send->writable_owed = true;
        ++m_reports;
    }
}

void ChannelLink::wake_blocked() noexcept
{
    for (u32 at = 0U; at < m_channel_count; ++at)
    {
        if (m_send[at].blocked)
        {
            m_send[at].blocked = false;
            m_send[at].writable_owed = true;
            ++m_reports;
        }
    }
}

void ChannelLink::release(ReadStream& stream) noexcept
{
    if (stream.waiting)
    {
        stream.waiting = false;
        --m_waiting;
    }
    std::destroy_at(stream.assembler); // gives the stream's quota and any partial frame back
    stream.assembler = nullptr;
    stream.used = false; // a place it may still hold in the ready list finds it unused, or its successor: both are fine
}

void ChannelLink::finish_stream(ReadStream& stream) noexcept
{
    if (!stream.assembler->between_frames())
    {
        fail(AdmitVerdict::Malformed);
        return;
    }
    release(stream);
}

void ChannelLink::fail(AdmitVerdict verdict) noexcept
{
    if (!m_failed)
    {
        m_failed = true;
        m_verdict = verdict;
    }
}

void ChannelLink::give_up_frame() noexcept
{
    if (!m_frame_out)
    {
        return;
    }
    m_frame_out = false;
    m_admission->finish(m_frame);
    m_room_freed = true;
    if (m_frame_stream == kNone)
    {
        return;
    }
    ReadStream& stream = m_streams[m_frame_stream];
    if (m_frame_bytes != 0U || m_frame_ends)
    {
        static_cast<void>(m_carrier.consume(stream.id, m_frame_bytes));
    }
    if (m_frame_ends)
    {
        finish_stream(stream);
    }
    else
    {
        push_ready(m_frame_stream);
    }
}

void ChannelLink::wake_waiting(u64 now_ns) noexcept
{
    if (m_waiting == 0U || (!m_room_freed && now_ns < m_wake_at))
    {
        m_room_freed = false;
        return;
    }
    const bool freed = m_room_freed;
    m_room_freed = false;
    u64 earliest = kNeverNs;
    for (u32 at = 0U; at < m_config.streams; ++at)
    {
        ReadStream& stream = m_streams[at];
        if (!stream.used || !stream.waiting)
        {
            continue;
        }
        if (freed || stream.wait_until <= now_ns)
        {
            stream.waiting = false;
            --m_waiting;
            push_ready(at);
        }
        else if (stream.wait_until < earliest)
        {
            earliest = stream.wait_until;
        }
    }
    m_wake_at = earliest;
}

void ChannelLink::hold_back(ReadStream& stream, u64 retry_at_ns, u64 now_ns) noexcept
{
    // A limit shared with the principal's other sessions may come free with no call of this link to say so.
    stream.wait_until = (retry_at_ns == kNeverNs) ? now_ns + kChannelRoomRetryNs : retry_at_ns;
    stream.waiting = true;
    ++m_waiting;
    if (stream.wait_until < m_wake_at)
    {
        m_wake_at = stream.wait_until;
    }
}

bool ChannelLink::take_report(ChannelEvent& event) noexcept
{
    if (m_reports == 0U)
    {
        return false;
    }
    for (u32 at = 0U; at < m_channel_count; ++at)
    {
        SendChannel& send = m_send[at];
        if (send.stopped)
        {
            send.stopped = false;
            --m_reports;
            event.kind = ChannelEventKind::Stopped;
            event.channel = at;
            event.code = send.stop_code;
            return true;
        }
        if (send.writable_owed)
        {
            send.writable_owed = false;
            --m_reports;
            event.kind = ChannelEventKind::Writable;
            event.channel = at;
            return true;
        }
    }
    return false;
}

bool ChannelLink::read_stream(u32 index, ChannelEvent& event, u64 now_ns) noexcept
{
    ReadStream& stream = m_streams[index];
    if (stream.waiting)
    {
        return false;
    }
    for (;;)
    {
        const QuicStreamRead in = m_carrier.peek(stream.id);
        if (in.reset)
        {
            ++m_counters.streams_aborted;
            release(stream);
            return false;
        }
        if (in.data.empty())
        {
            if (in.fin)
            {
                static_cast<void>(m_carrier.consume(stream.id, 0U));
                finish_stream(stream);
            }
            return false;
        }
        // Bytes that may be replayed are fed by themselves, and a frame whose head began among them stays early.
        const bool early = in.early_bytes != 0U || (stream.early && !stream.assembler->between_frames());
        stream.early = early;
        const cont::ConstSpan<u8> view = (in.early_bytes != 0U) ? in.data.subspan(0U, in.early_bytes) : in.data;
        const FrameStep step =
            stream.assembler->feed(view, early ? crypto::Delivery::Early : crypto::Delivery::Established, now_ns);
        const bool ends = in.fin && step.consumed == in.data.size();
        if (step.action == AdmitAction::Deliver)
        {
            m_frame_out = true;
            m_frame = step.frame;
            m_frame_stream = index;
            m_frame_bytes = step.consumed;
            m_frame_ends = ends;
            event.kind = ChannelEventKind::Frame;
            event.channel = step.frame.channel;
            event.frame = step.frame;
            return true;
        }
        if (step.action == AdmitAction::Close)
        {
            fail(step.verdict);
            return false;
        }
        if (step.consumed != 0U || ends)
        {
            static_cast<void>(m_carrier.consume(stream.id, step.consumed));
        }
        if (step.action == AdmitAction::Wait)
        {
            hold_back(stream, step.retry_at_ns, now_ns);
            return false;
        }
        if (step.action == AdmitAction::Skip && stream.assembler->refused())
        {
            refuse(stream.id);
            release(stream);
            return false;
        }
        if (ends)
        {
            finish_stream(stream);
            return false;
        }
        if (step.consumed == 0U)
        {
            return false;
        }
    }
}

void ChannelLink::open_datagram(cont::ConstSpan<u8> payload, bool early, bool held, u64 now_ns) noexcept
{
    if (m_admission->open_datagram(payload, now_ns, m_cursor) != AdmitVerdict::Admitted)
    {
        ++m_counters.datagrams_refused;
        if (held)
        {
            m_carrier.pop_datagram();
        }
        return;
    }
    m_datagram_open = true;
    m_datagram_held = held;
    m_datagram_delivery = early ? crypto::Delivery::Early : crypto::Delivery::Established;
    if (m_cursor.route.form == RouteForm::Fragment)
    {
        m_sweep_at = now_ns + m_config.fragment_timeout_ns; // every piece so far has given up by then
    }
}

void ChannelLink::end_datagram() noexcept
{
    m_datagram_open = false;
    if (m_datagram_held)
    {
        m_carrier.pop_datagram();
        m_datagram_held = false;
    }
}

bool ChannelLink::read_datagram(ChannelEvent& event, u64 now_ns) noexcept
{
    DatagramAssembler& assembler = *m_datagrams[m_cursor.route.channel].assembler;
    while (m_cursor.more)
    {
        const FrameStep step = assembler.next(m_cursor, m_datagram_delivery, now_ns);
        if (step.action == AdmitAction::Deliver)
        {
            m_frame_out = true;
            m_frame = step.frame;
            m_frame_stream = kNone;
            event.kind = ChannelEventKind::Frame;
            event.channel = step.frame.channel;
            event.frame = step.frame;
            return true;
        }
    }
    end_datagram();
    return false;
}

void ChannelLink::on_quic_event(const QuicStreamEvent& event) noexcept
{
    if ((event.events & kQuicStreamOpened) != 0U)
    {
        stream_opened(event.id);
    }
    if ((event.events & kQuicStreamReadable) != 0U)
    {
        stream_readable(event.id);
    }
    if ((event.events & kQuicStreamReset) != 0U)
    {
        stream_aborted(event.id);
    }
    if ((event.events & kQuicStreamStopSending) != 0U)
    {
        stream_stopped(event.id, event.stop_error);
    }
    if ((event.events & kQuicStreamWritable) != 0U)
    {
        stream_writable(event.id);
    }
}

bool ChannelLink::poll_path(u64 now_ns) noexcept
{
    if (!m_carrier.polled())
    {
        return false;
    }
    QuicStreamEvent stream_event;
    if (m_carrier.poll(stream_event))
    {
        on_quic_event(stream_event);
        return true;
    }
    cont::ConstSpan<u8> payload;
    bool early = false;
    if (m_carrier.peek_datagram(payload, early))
    {
        open_datagram(payload, early, true, now_ns);
        return true;
    }
    return false;
}

void ChannelLink::on_webtransport(const WtEvent& event, u64 now_ns) noexcept
{
    give_up_frame();
    if (m_datagram_open)
    {
        end_datagram(); // the caller did not drain next(): what the last event viewed is gone
    }
    switch (event.kind)
    {
        case WtEventKind::StreamOpened:
            stream_opened(event.stream);
            break;
        case WtEventKind::StreamReadable:
            stream_readable(event.stream);
            break;
        case WtEventKind::StreamWritable:
            stream_writable(event.stream);
            break;
        case WtEventKind::SessionWritable:
            wake_blocked();
            break;
        case WtEventKind::StreamAborted:
            stream_aborted(event.stream);
            break;
        case WtEventKind::StreamStopped:
            stream_stopped(event.stream, event.code);
            break;
        case WtEventKind::Datagram:
            if (!m_failed)
            {
                open_datagram(event.bytes, false, false, now_ns);
            }
            break;
        default:
            break; // the session's own events are its owner's
    }
}

bool ChannelLink::next(ChannelEvent& event, u64 now_ns) noexcept
{
    event = ChannelEvent{};
    give_up_frame();
    for (;;)
    {
        if (m_failed)
        {
            if (m_failure_told)
            {
                return false;
            }
            m_failure_told = true;
            event.kind = ChannelEventKind::Failed;
            event.verdict = m_verdict;
            return true;
        }
        wake_waiting(now_ns);
        if (now_ns >= m_sweep_at)
        {
            m_sweep_at = kNeverNs;
            for (u32 channel = 0U; channel < m_channel_count; ++channel)
            {
                if (m_datagrams[channel].assembler != nullptr)
                {
                    m_datagrams[channel].assembler->sweep(now_ns);
                }
            }
        }
        if (m_open_waiting && m_carrier.can_open())
        {
            m_open_waiting = false;
            wake_blocked();
        }
        if (take_report(event))
        {
            return true;
        }
        if (m_datagram_open)
        {
            if (read_datagram(event, now_ns))
            {
                return true;
            }
            continue;
        }
        if (m_ready_head != kNone)
        {
            const u32 index = pop_ready();
            if (m_streams[index].used && read_stream(index, event, now_ns))
            {
                return true;
            }
            continue;
        }
        if (!poll_path(now_ns))
        {
            return false;
        }
    }
}

u64 ChannelLink::next_timer_ns() const noexcept
{
    const u64 wake = (m_waiting != 0U) ? m_wake_at : kNeverNs;
    return (wake < m_sweep_at) ? wake : m_sweep_at;
}

ChannelError ChannelLink::open_channel(u32 channel, const wire::ChannelDesc& desc, SendChannel& send) noexcept
{
    const ChannelError error = m_carrier.open(false, send.stream);
    if (error != ChannelError::None)
    {
        if (error == ChannelError::Blocked)
        {
            send.blocked = true;
            m_open_waiting = true;
        }
        return error;
    }
    static_cast<void>(m_carrier.set_urgency(send.stream, urgency_of(desc)));
    send.open = true;
    send.pending = 0U;
    send.route_sent = 0U;
    send.route_bytes = static_cast<u8>(write_route(Route{channel}, cont::Span<u8>(send.route, sizeof(send.route))));
    return ChannelError::None;
}

ChannelError ChannelLink::send_route(SendChannel& send) noexcept
{
    usize taken = 0U;
    const ChannelError error = m_carrier.write(
        send.stream, cont::ConstSpan<u8>(send.route + send.route_sent, send.route_bytes - send.route_sent), false,
        taken);
    send.route_sent = static_cast<u8>(send.route_sent + taken);
    send.pending += taken;
    if (error == ChannelError::None && send.route_sent == send.route_bytes)
    {
        return ChannelError::None;
    }
    if (error == ChannelError::None || error == ChannelError::Blocked)
    {
        send.blocked = true;
        return ChannelError::Blocked;
    }
    return error;
}

void ChannelLink::pad_stream(const wire::ChannelDesc& desc, SendChannel& send) noexcept
{
    if (!m_carrier.pads_in_band() || desc.padding_block == 0U)
    {
        return;
    }
    const usize bytes =
        write_stream_padding(send.pending, desc.padding_block, cont::Span<u8>(m_padding, m_padding_bytes));
    if (bytes == 0U || m_carrier.send_room(send.stream, bytes) < bytes)
    {
        return; // the stream stays correct and is padded at its next write: a frame is never cut by padding
    }
    usize taken = 0U;
    if (m_carrier.write(send.stream, cont::ConstSpan<u8>(m_padding, bytes), false, taken) == ChannelError::None)
    {
        send.pending += taken;
        m_counters.padding_bytes += taken;
    }
}

ChannelError ChannelLink::write(u32 channel, cont::ConstSpan<u8> frames, usize& taken) noexcept
{
    taken = 0U;
    if (m_failed)
    {
        return ChannelError::Closed;
    }
    if (channel >= m_channel_count || !m_admission->channel(channel).desc->reliable)
    {
        return ChannelError::Invalid;
    }
    if (frames.empty())
    {
        return ChannelError::None;
    }
    const wire::ChannelDesc& desc = *m_admission->channel(channel).desc;
    SendChannel& send = m_send[channel];
    if (!send.open)
    {
        const ChannelError opened = open_channel(channel, desc, send);
        if (opened != ChannelError::None)
        {
            return opened;
        }
    }
    if (send.route_sent != send.route_bytes)
    {
        const ChannelError routed = send_route(send);
        if (routed != ChannelError::None)
        {
            return routed;
        }
    }
    const ChannelError error = m_carrier.write(send.stream, frames, false, taken);
    send.pending += taken;
    if (error != ChannelError::None)
    {
        send.blocked = send.blocked || error == ChannelError::Blocked;
        return error;
    }
    if (taken != frames.size())
    {
        send.blocked = true;
        return ChannelError::None;
    }
    pad_stream(desc, send);
    if (!desc.ordered)
    {
        usize none = 0U;
        const ChannelError ended = m_carrier.write(send.stream, cont::ConstSpan<u8>{}, true, none);
        send.open = false;
        return ended;
    }
    return ChannelError::None;
}

ChannelError ChannelLink::send_datagram(u32 channel, cont::Span<u8> buffer, usize used, u64 now_ns) noexcept
{
    if (m_failed)
    {
        return ChannelError::Closed;
    }
    if (channel >= m_channel_count || m_admission->channel(channel).desc->reliable || used > buffer.size())
    {
        return ChannelError::Invalid;
    }
    const wire::ChannelDesc& desc = *m_admission->channel(channel).desc;
    usize size = used;
    if (m_carrier.pads_in_band() && desc.padding_block != 0U)
    {
        const usize limit = m_carrier.max_datagram();
        const cont::Span<u8> room = buffer.subspan(0U, (limit < buffer.size()) ? limit : buffer.size());
        if (used <= room.size())
        {
            size = pad_datagram(channel, desc.padding_block, used, room);
            m_counters.padding_bytes += size - used;
        }
    }
    return m_carrier.send_datagram(cont::ConstSpan<u8>(buffer.data(), size), now_ns);
}

} // namespace crd::net
