#include <crd/net/delivery_rate.hpp>

#include <crd/hesap/net/rate.hpp>

namespace crd::net
{

DeliveryStamp DeliveryRateSampler::on_sent(u64 bytes, u64 in_flight, u64 now_ns) noexcept
{
    if (in_flight <= bytes)
    {
        // Nothing else was in flight: the intervals start at this packet, not across the idle time before it.
        m_first_sent_ns = now_ns;
        m_delivered_ns = now_ns;
    }
    DeliveryStamp stamp;
    stamp.delivered = m_delivered;
    stamp.delivered_ns = m_delivered_ns;
    stamp.first_sent_ns = m_first_sent_ns;
    stamp.lost = m_lost;
    stamp.in_flight = in_flight;
    stamp.app_limited = m_app_limited != 0U;
    return stamp;
}

void DeliveryRateSampler::on_ack_begin() noexcept
{
    m_sample = DeliverySample{};
}

void DeliveryRateSampler::on_acked(const DeliveryStamp& stamp, u64 bytes, u64 sent_ns, u64 order, u64 now_ns) noexcept
{
    m_delivered += bytes;
    m_delivered_ns = now_ns;
    m_sample.newly_acked += bytes;
    const bool newest = !m_sample.acked || sent_ns > m_first_sent_ns ||
                        (sent_ns == m_first_sent_ns && order > m_newest_order);
    if (!newest)
    {
        return;
    }
    m_sample.acked = true;
    m_sample.prior_delivered = stamp.delivered;
    m_sample.app_limited = stamp.app_limited;
    m_sample.tx_in_flight = stamp.in_flight;
    m_sample.lost = m_lost - ((stamp.lost < m_lost) ? stamp.lost : m_lost); // a stamp from before a reset reads 0
    m_send_elapsed_ns = (sent_ns > stamp.first_sent_ns) ? sent_ns - stamp.first_sent_ns : 0U;
    m_ack_elapsed_ns = (now_ns > stamp.delivered_ns) ? now_ns - stamp.delivered_ns : 0U;
    m_first_sent_ns = sent_ns;
    m_newest_order = order;
}

void DeliveryRateSampler::on_lost(u64 bytes) noexcept
{
    m_lost += bytes;
}

void DeliveryRateSampler::on_ack_end(u64 min_rtt_ns) noexcept
{
    if (m_app_limited != 0U && m_delivered > m_app_limited)
    {
        m_app_limited = 0U; // the bubble was delivered
    }
    if (!m_sample.acked)
    {
        return;
    }
    m_sample.interval_ns = (m_send_elapsed_ns > m_ack_elapsed_ns) ? m_send_elapsed_ns : m_ack_elapsed_ns;
    const u64 prior = m_sample.prior_delivered;
    m_sample.delivered = m_delivered - ((prior < m_delivered) ? prior : m_delivered);
    if (m_sample.interval_ns < min_rtt_ns)
    {
        return; // section 4.1.3.5: an interval under the min RTT gives no reliable rate
    }
    m_sample.rate = hesap::net::delivery_rate(m_sample.delivered, m_send_elapsed_ns, m_ack_elapsed_ns);
}

void DeliveryRateSampler::on_app_limited(u64 in_flight) noexcept
{
    const u64 end = m_delivered + in_flight;
    m_app_limited = (end != 0U) ? end : 1U;
}

void DeliveryRateSampler::reset() noexcept
{
    *this = DeliveryRateSampler{};
}

} // namespace crd::net
