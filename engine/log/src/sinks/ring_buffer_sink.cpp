#include "../log_formatter.hpp"

#include <crd/log/log_channel.hpp>
#include <crd/log/sinks/ring_buffer_sink.hpp>

namespace crd::log
{
RingBufferSink::RingBufferSink(usize capacity) noexcept : m_capacity(capacity == 0 ? 1 : capacity), m_buffer(m_capacity)
{
}

void RingBufferSink::write(const LogRecord& rec)
{
    StoredLogRecord stored;
    stored.level = rec.level;
    stored.channel_name = (rec.channel && rec.channel->name) ? rec.channel->name : "";
    stored.message.assign(rec.message.data(), rec.message.size());
    const char* file = rec.loc.file_name() ? rec.loc.file_name() : "";
    stored.file = file;
    stored.line = static_cast<u32>(rec.loc.line());
    stored.thread_id = rec.thread_id;
    stored.time = rec.time;

    std::lock_guard<std::mutex> lock(m_mutex);
    m_buffer[m_head] = std::move(stored);
    m_head = (m_head + 1) % m_capacity;
    if (m_count < m_capacity)
    {
        ++m_count;
    }
}

std::vector<StoredLogRecord> RingBufferSink::snapshot() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<StoredLogRecord> out;
    out.reserve(m_count);
    // Oldest entry is m_count items behind m_head (mod capacity).
    const usize start = (m_head + m_capacity - m_count) % m_capacity;
    for (usize i = 0; i < m_count; ++i)
    {
        out.push_back(m_buffer[(start + i) % m_capacity]);
    }
    return out;
}

usize RingBufferSink::size() const noexcept
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_count;
}

void RingBufferSink::clear() noexcept
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_head = 0;
    m_count = 0;
}
} // namespace crd::log
