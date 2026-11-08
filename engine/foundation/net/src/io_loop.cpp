// crd-net -- the transport's I/O loop (4.2a; docs/design/network-foundation.md#ingress).

#include <crd/memory/allocator.hpp>
#include <crd/memory/construct.hpp>
#include <crd/net/io_loop.hpp>
#include <crd/time/clocks.hpp>

namespace crd::net
{
namespace
{
constexpr u32 kMaxBatch = 1024U;
constexpr u64 kAnswerRetryNs = 1000000U; // an answer the host was too busy to take is offered again within 1 ms
} // namespace

u64 HostClock::monotonic_ns() const noexcept
{
    const i64 now = time::TickClock::now_ns();
    return (now > 0) ? static_cast<u64>(now) : 0U;
}

u64 HostClock::unix_seconds() const noexcept
{
    const i64 now = time::WallClock::now().ns_since_epoch();
    return (now > 0) ? static_cast<u64>(now) / kNanosPerSecond : 0U;
}

IoLoop::IoLoop(memory::IAllocator& allocator, DatagramSocket& socket, const TransportClock& clock, Ingress& ingress,
               IngressHandler& handler, const IoLoopConfig& config) noexcept
    : m_allocator(allocator), m_socket(socket), m_clock(clock), m_ingress(ingress), m_handler(handler),
      m_config(config)
{
    if (!ingress.ready() || config.batch == 0U || config.batch > kMaxBatch ||
        config.slot_bytes < kQuicMinInitialDatagram || config.slot_bytes > kMaxUdpPayloadV6 ||
        config.batches_per_poll == 0U || config.queued_answers == 0U || config.max_wait_ns == 0U)
    {
        return;
    }
    const usize buffer_bytes = usize{config.batch} * config.slot_bytes;
    const usize answer_count = usize{config.queued_answers} * kQuicMaxStatelessAnswerBytes;
    ReceivedDatagram* const slots = memory::try_construct_array<ReceivedDatagram>(allocator, config.batch);
    u8* const buffers = memory::try_construct_array<u8>(allocator, buffer_bytes);
    OutgoingDatagram* const answers = memory::try_construct_array<OutgoingDatagram>(allocator, config.queued_answers);
    u8* const answer_bytes = memory::try_construct_array<u8>(allocator, answer_count);
    if (slots == nullptr || buffers == nullptr || answers == nullptr || answer_bytes == nullptr)
    {
        memory::destroy_array(allocator, slots, config.batch);
        memory::destroy_array(allocator, buffers, buffer_bytes);
        memory::destroy_array(allocator, answers, config.queued_answers);
        memory::destroy_array(allocator, answer_bytes, answer_count);
        return;
    }
    for (u32 index = 0U; index < config.batch; ++index)
    {
        slots[index].storage = cont::Span<u8>(buffers + (usize{index} * config.slot_bytes), config.slot_bytes);
    }
    m_slots = slots;
    m_buffers = buffers;
    m_answers = answers;
    m_answer_bytes = answer_bytes;
}

IoLoop::~IoLoop()
{
    memory::destroy_array(m_allocator, m_slots, m_config.batch);
    memory::destroy_array(m_allocator, m_buffers, usize{m_config.batch} * m_config.slot_bytes);
    memory::destroy_array(m_allocator, m_answers, m_config.queued_answers);
    memory::destroy_array(m_allocator, m_answer_bytes, usize{m_config.queued_answers} * kQuicMaxStatelessAnswerBytes);
}

u64 IoLoop::poll() noexcept
{
    if (m_slots == nullptr)
    {
        return kNeverNs;
    }
    ++m_counters.polls;
    const u64 now = m_clock.monotonic_ns();
    const u64 unix_seconds = m_clock.unix_seconds();
    flush_answers();
    for (u32 batch = 0U; batch < m_config.batches_per_poll; ++batch)
    {
        BatchResult result;
        receive_batch(now, unix_seconds, result);
        if (result.error != SocketError::None)
        {
            ++m_counters.receive_errors;
            break;
        }
        if (result.count < m_config.batch)
        {
            break;
        }
    }
    flush_answers();
    return m_handler.on_timers(m_clock.monotonic_ns());
}

void IoLoop::run(const std::atomic<bool>& stop) noexcept
{
    while (!stop.load(std::memory_order_acquire))
    {
        const u64 next = poll();
        const u64 now = m_clock.monotonic_ns();
        if (stop.load(std::memory_order_acquire))
        {
            return;
        }
        if (next <= now)
        {
            continue;
        }
        u64 wait = next - now;
        wait = (wait < m_config.max_wait_ns) ? wait : m_config.max_wait_ns;
        if (m_queued != 0U && wait > kAnswerRetryNs)
        {
            wait = kAnswerRetryNs;
        }
        (void)m_socket.wait_readable(wait);
    }
}

void IoLoop::receive_batch(u64 now_ns, u64 unix_seconds, BatchResult& result) noexcept
{
    result = m_socket.receive(cont::Span<ReceivedDatagram>(m_slots, m_config.batch));
    m_counters.received += result.count;
    for (usize index = 0U; index < result.count; ++index)
    {
        const ReceivedDatagram& datagram = m_slots[index];
        const IngressDecision decision =
            m_ingress.classify(datagram.storage.first(datagram.size), datagram.truncated, datagram.peer, now_ns,
                               unix_seconds);
        switch (decision.action)
        {
        case IngressAction::Route:
            m_handler.on_route(decision, datagram, now_ns);
            break;
        case IngressAction::Admit:
            m_handler.on_admit(decision, datagram, now_ns);
            break;
        case IngressAction::Retry:
        case IngressAction::NegotiateVersion:
        case IngressAction::StatelessReset:
        case IngressAction::InvalidToken:
            queue_answer(decision, datagram.peer, unix_seconds);
            break;
        case IngressAction::Drop:
            break;
        }
    }
}

void IoLoop::queue_answer(const IngressDecision& decision, const Endpoint& peer, u64 unix_seconds) noexcept
{
    if (m_queued == m_config.queued_answers)
    {
        ++m_counters.answers_dropped;
        return;
    }
    const u32 index = (m_head + m_queued) % m_config.queued_answers;
    const cont::Span<u8> out(m_answer_bytes + (usize{index} * kQuicMaxStatelessAnswerBytes),
                             kQuicMaxStatelessAnswerBytes);
    const usize written = m_ingress.answer(decision, peer, unix_seconds, out);
    if (written == 0U)
    {
        return;
    }
    m_answers[index] = OutgoingDatagram{out.first(written), peer};
    ++m_queued;
}

void IoLoop::flush_answers() noexcept
{
    while (m_queued != 0U)
    {
        const u32 run = (m_queued < m_config.queued_answers - m_head) ? m_queued : m_config.queued_answers - m_head;
        const BatchResult result = m_socket.send(cont::ConstSpan<OutgoingDatagram>(m_answers + m_head, run));
        const auto sent = static_cast<u32>(result.count);
        m_head = (m_head + sent) % m_config.queued_answers;
        m_queued -= sent;
        m_counters.answers_sent += sent;
        if (result.error == SocketError::Busy)
        {
            return;
        }
        if (result.error != SocketError::None && m_queued != 0U)
        {
            // That answer alone cannot leave (an unreachable peer, a refused address): it is given up.
            m_head = (m_head + 1U) % m_config.queued_answers;
            --m_queued;
            ++m_counters.answers_dropped;
        }
        else if (sent == 0U)
        {
            return;
        }
    }
}

} // namespace crd::net
