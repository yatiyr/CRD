// crd-net -- QUIC byte streams and flow control (4.2a; contract: docs/design/network-foundation.md#quic-streams).
#include <crd/net/quic_stream.hpp>

#include <crd/net/quic_header.hpp>

#include <bit>
#include <cstring>

namespace crd::net
{
namespace
{
constexpr usize kWordBits = 64U;
constexpr u64   kAllOnes  = ~u64{0};
// An RTT past 2^60 ns counts as that, so twice it cannot wrap.
constexpr u64 kMaxRttNs = u64{1} << 60U;

[[nodiscard]] constexpr u64 min_u64(u64 a, u64 b) noexcept
{
    return (a < b) ? a : b;
}

[[nodiscard]] constexpr u64 max_u64(u64 a, u64 b) noexcept
{
    return (a > b) ? a : b;
}

// The mask of `count` bits (1 to 64) from bit `from` of a word.
[[nodiscard]] constexpr u64 bit_mask(usize from, usize count) noexcept
{
    return ((count == kWordBits) ? kAllOnes : ((u64{1} << count) - 1U)) << from;
}

// The bitmaps and bytes of a buffer are rings of its capacity: offset o lives at o % capacity. Every range below is
// at most the capacity long, so it wraps at most once.
struct Ring
{
    usize capacity = 0U;

    [[nodiscard]] usize at(u64 offset) const noexcept { return static_cast<usize>(offset % capacity); }
};

// Sets bits [pos, pos + count) of `bits` (no wrap) to `value` where `keep` (when given) is clear; returns how many
// changed when `Count` (a caller that only sets or clears skips the count).
template <bool Count>
u64 mark_linear(cont::Span<u64> bits, const u64* keep, usize pos, usize count, bool value) noexcept
{
    u64 changed = 0U;
    while (count > 0U)
    {
        const usize word = pos / kWordBits;
        const usize bit  = pos % kWordBits;
        const usize take = (count < kWordBits - bit) ? count : kWordBits - bit;
        u64         mask = bit_mask(bit, take);
        if (keep != nullptr)
        {
            mask &= ~keep[word];
        }
        const u64 before = bits[word];
        const u64 after  = value ? (before | mask) : (before & ~mask);
        if constexpr (Count)
        {
            changed += static_cast<u64>(std::popcount(before ^ after));
        }
        bits[word] = after;
        pos += take;
        count -= take;
    }
    return changed;
}

template <bool Count>
u64 mark_ring(const Ring& ring, cont::Span<u64> bits, const u64* keep, u64 offset, u64 count, bool value) noexcept
{
    const usize pos   = ring.at(offset);
    const usize total = static_cast<usize>(count);
    const usize first = (total < ring.capacity - pos) ? total : ring.capacity - pos;
    return mark_linear<Count>(bits, keep, pos, first, value) + mark_linear<Count>(bits, keep, 0U, total - first, value);
}

// The bits of [offset, offset + count) set to `value` where `keep` (when given) is clear; returns how many changed.
u64 mark(const Ring& ring, cont::Span<u64> bits, const u64* keep, u64 offset, u64 count, bool value) noexcept
{
    return mark_ring<true>(ring, bits, keep, offset, count, value);
}

// The bits of [offset, offset + count) set to `value`, uncounted.
void fill(const Ring& ring, cont::Span<u64> bits, u64 offset, u64 count, bool value) noexcept
{
    static_cast<void>(mark_ring<false>(ring, bits, nullptr, offset, count, value));
}

// The length of the run of bits equal to `value` from `pos` (no wrap), at most `count`.
[[nodiscard]] usize run_linear(cont::ConstSpan<u64> bits, usize pos, usize count, bool value) noexcept
{
    usize counted = 0U;
    while (counted < count)
    {
        const usize at   = pos + counted;
        const usize bit  = at % kWordBits;
        const usize room = kWordBits - bit;
        const u64   word = value ? (bits[at / kWordBits] >> bit) : ~(bits[at / kWordBits] >> bit);
        usize       same = static_cast<usize>(std::countr_one(word));
        if (same > room)
        {
            same = room; // the bits shifted in past the word
        }
        const usize left = count - counted;
        if (same >= left)
        {
            return count;
        }
        counted += same;
        if (same < room)
        {
            break;
        }
    }
    return counted;
}

[[nodiscard]] u64 run(const Ring& ring, cont::ConstSpan<u64> bits, u64 offset, u64 count, bool value) noexcept
{
    const usize pos   = ring.at(offset);
    const usize total = static_cast<usize>(count);
    const usize first = (total < ring.capacity - pos) ? total : ring.capacity - pos;
    const usize head  = run_linear(bits, pos, first, value);
    if (head < first || first == total)
    {
        return head;
    }
    return head + run_linear(bits, 0U, total - first, value);
}

void copy_in(const Ring& ring, cont::Span<u8> bytes, u64 offset, const u8* from, usize count) noexcept
{
    const usize pos   = ring.at(offset);
    const usize first = (count < ring.capacity - pos) ? count : ring.capacity - pos;
    std::memcpy(bytes.data() + pos, from, first);
    std::memcpy(bytes.data(), from + first, count - first);
}

[[nodiscard]] bool equal(const Ring& ring, cont::ConstSpan<u8> bytes, u64 offset, const u8* to, usize count) noexcept
{
    const usize pos   = ring.at(offset);
    const usize first = (count < ring.capacity - pos) ? count : ring.capacity - pos;
    return std::memcmp(bytes.data() + pos, to, first) == 0 &&
           std::memcmp(bytes.data(), to + first, count - first) == 0;
}

[[nodiscard]] bool storage_valid(usize capacity, usize words) noexcept
{
    return capacity != 0U && capacity % kQuicStreamChunk == 0U && words == quic_stream_bitmap_words(capacity);
}

void zero(cont::Span<u64> bits) noexcept
{
    std::memset(bits.data(), 0, bits.size_bytes());
}

// Calls `move(from_pos, to_pos, count)` for each run of [base, base + count) that wraps neither ring.
template <typename Move> void each_run(const Ring& from, const Ring& to, u64 base, u64 count, Move move) noexcept
{
    for (u64 at = base; at < base + count;)
    {
        const usize source = from.at(at);
        const usize target = to.at(at);
        const u64   run    = min_u64(min_u64(base + count - at, from.capacity - source), to.capacity - target);
        move(source, target, static_cast<usize>(run));
        at += run;
    }
}

// Puts the bits of every offset in [base, base + from.capacity) of `old` at its place in `fresh` (zero before). Both
// rings' capacities are multiples of the word, so an offset keeps its bit within a word.
void move_bits(const Ring& from, const Ring& to, cont::ConstSpan<u64> old, cont::Span<u64> fresh, u64 base) noexcept
{
    each_run(from, to, base, from.capacity,
             [&](usize source, usize target, usize count)
             {
                 while (count > 0U)
                 {
                     const usize bit  = source % kWordBits;
                     const usize take = (count < kWordBits - bit) ? count : kWordBits - bit;
                     fresh[target / kWordBits] |= old[source / kWordBits] & bit_mask(bit, take);
                     source += take;
                     target += take;
                     count -= take;
                 }
             });
}

void move_bytes(const Ring& from, const Ring& to, cont::ConstSpan<u8> old, cont::Span<u8> fresh, u64 base,
                u64 count) noexcept
{
    each_run(from, to, base, count, [&](usize source, usize target, usize run)
             { std::memcpy(fresh.data() + target, old.data() + source, run); });
}
} // namespace

// ---- QuicReceiveBuffer ----

bool QuicReceiveBuffer::init(cont::Span<u8> bytes, cont::Span<u64> bits) noexcept
{
    *this = QuicReceiveBuffer{};
    if (!storage_valid(bytes.size(), bits.size()))
    {
        return false;
    }
    m_bytes = bytes;
    m_bits  = bits;
    zero(m_bits);
    return true;
}

QuicReceiveResult QuicReceiveBuffer::insert(u64 offset, cont::ConstSpan<u8> data, bool fin) noexcept
{
    if (m_bytes.empty())
    {
        return QuicReceiveResult::Unusable;
    }
    const u64 length = data.size();
    if (offset > kQuicMaxVarint || length > kQuicMaxVarint - offset)
    {
        return QuicReceiveResult::BeyondWindow;
    }
    const u64 end = offset + length;
    if (m_final_known ? (end > m_final || (fin && end != m_final)) : (fin && end < m_received))
    {
        return QuicReceiveResult::FinalSize;
    }
    if (length != 0U && end > m_read && end - m_read > capacity())
    {
        return QuicReceiveResult::BeyondWindow;
    }

    // Data below the read offset was read already and is not compared (nothing holds it). Below the contiguous end
    // every byte is held; past it a byte is held exactly when its bit is set, and no bit is set at m_marked or above.
    const u64 from = max_u64(offset, m_read);
    if (from < end)
    {
        const Ring ring{capacity()};
        const u64  whole = min_u64(end, m_contiguous);
        if (from < whole &&
            !equal(ring, m_bytes, from, data.data() + (from - offset), static_cast<usize>(whole - from)))
        {
            return QuicReceiveResult::Changed;
        }
        const u64 marked_end = min_u64(end, m_marked);
        for (u64 at = max_u64(from, m_contiguous); at < marked_end;)
        {
            at += run(ring, m_bits, at, marked_end - at, false);
            if (at >= marked_end)
            {
                break;
            }
            const u64 held = run(ring, m_bits, at, marked_end - at, true);
            if (!equal(ring, m_bytes, at, data.data() + (at - offset), static_cast<usize>(held)))
            {
                return QuicReceiveResult::Changed;
            }
            at += held;
        }
        copy_in(ring, m_bytes, from, data.data() + (from - offset), static_cast<usize>(end - from));
        if (from > m_contiguous)
        {
            fill(ring, m_bits, from, end - from, true); // past a gap: held by its bits
            m_marked = max_u64(m_marked, end);
        }
        else if (end > m_contiguous)
        {
            // In order: everything to `end` is held without its bits, and a run that arrived early continues it.
            m_contiguous = end;
            if (m_marked > end)
            {
                m_contiguous += run(ring, m_bits, end, m_read + capacity() - end, true);
            }
        }
    }
    m_received = max_u64(m_received, end);
    if (fin)
    {
        m_final_known = true;
        m_final       = end;
    }
    return QuicReceiveResult::Accepted;
}

bool QuicReceiveBuffer::grow(cont::Span<u8> bytes, cont::Span<u64> bits) noexcept
{
    if (m_bytes.empty() || !storage_valid(bytes.size(), bits.size()) || bytes.size() < capacity())
    {
        return false;
    }
    const Ring from{capacity()};
    const Ring to{bytes.size()};
    zero(bits);
    move_bits(from, to, m_bits, bits, m_read);
    // Data lies below read_offset() + capacity(); a final size alone may lie past it.
    move_bytes(from, to, m_bytes, bytes, m_read, min_u64(m_received - min_u64(m_received, m_read), capacity()));
    m_bytes = bytes;
    m_bits  = bits;
    return true;
}

cont::ConstSpan<u8> QuicReceiveBuffer::readable() const noexcept
{
    if (m_bytes.empty())
    {
        return {};
    }
    const Ring  ring{capacity()};
    const usize pos = ring.at(m_read);
    return cont::ConstSpan<u8>(m_bytes.data() + pos, static_cast<usize>(min_u64(m_contiguous - m_read,
                                                                                capacity() - pos)));
}

usize QuicReceiveBuffer::copy_contiguous(usize skip, cont::Span<u8> out) const noexcept
{
    const u64 available = m_contiguous - m_read;
    if (m_bytes.empty() || skip >= available)
    {
        return 0U;
    }
    const usize count = static_cast<usize>(min_u64(available - skip, out.size()));
    const usize pos = Ring{capacity()}.at(m_read + skip);
    const usize first = static_cast<usize>(min_u64(count, capacity() - pos));
    std::memcpy(out.data(), m_bytes.data() + pos, first);
    if (count != first)
    {
        std::memcpy(out.data() + first, m_bytes.data(), count - first);
    }
    return count;
}

void QuicReceiveBuffer::consume(u64 count) noexcept
{
    const u64 taken = min_u64(count, m_contiguous - m_read);
    if (taken == 0U)
    {
        return;
    }
    if (m_marked > m_read)
    {
        fill(Ring{capacity()}, m_bits, m_read, taken, false); // a wrapped offset finds its bit clear
    }
    m_read += taken;
}

// ---- QuicSendBuffer ----

bool QuicSendBuffer::init(cont::Span<u8> bytes, cont::Span<u64> acked, cont::Span<u64> lost) noexcept
{
    *this = QuicSendBuffer{};
    if (!storage_valid(bytes.size(), acked.size()) || lost.size() != acked.size())
    {
        return false;
    }
    m_bytes      = bytes;
    m_acked_bits = acked;
    m_lost_bits  = lost;
    zero(m_acked_bits);
    zero(m_lost_bits);
    return true;
}

usize QuicSendBuffer::room() const noexcept
{
    return capacity() - static_cast<usize>(m_written - m_acked);
}

usize QuicSendBuffer::write(cont::ConstSpan<u8> data) noexcept
{
    if (m_bytes.empty() || m_finished)
    {
        return 0U;
    }
    const usize space = room();
    const usize taken = (data.size() < space) ? data.size() : space;
    if (taken == 0U)
    {
        return 0U;
    }
    copy_in(Ring{capacity()}, m_bytes, m_written, data.data(), taken);
    m_written += taken;
    return taken;
}

bool QuicSendBuffer::grow(cont::Span<u8> bytes, cont::Span<u64> acked, cont::Span<u64> lost) noexcept
{
    if (m_bytes.empty() || !storage_valid(bytes.size(), acked.size()) || lost.size() != acked.size() ||
        bytes.size() < capacity())
    {
        return false;
    }
    const Ring from{capacity()};
    const Ring to{bytes.size()};
    zero(acked);
    zero(lost);
    move_bits(from, to, m_acked_bits, acked, m_acked);
    move_bits(from, to, m_lost_bits, lost, m_acked);
    move_bytes(from, to, m_bytes, bytes, m_acked, m_written - m_acked);
    m_bytes      = bytes;
    m_acked_bits = acked;
    m_lost_bits  = lost;
    return true;
}

void QuicSendBuffer::finish() noexcept
{
    if (!m_bytes.empty())
    {
        m_finished = true;
    }
}

void QuicSendBuffer::truncate(u64 reliable) noexcept
{
    if (m_bytes.empty())
    {
        return;
    }
    // Bytes acknowledged beyond the size stay acknowledged, so the invariant m_acked <= m_sent <= m_written holds.
    const u64 end = max_u64(min_u64(reliable, m_written), m_acked);
    if (m_sent > end)
    {
        const Ring ring{capacity()};
        if (m_lost_bytes > 0U)
        {
            m_lost_bytes -= mark(ring, m_lost_bits, nullptr, end, m_sent - end, false);
        }
        fill(ring, m_acked_bits, end, m_sent - end, false);
        m_sent = end;
    }
    m_written   = end;
    m_finished  = true;
    m_fin_sent  = true;
    m_fin_lost  = false;
    m_fin_acked = true;
}

bool QuicSendBuffer::pending() const noexcept
{
    return m_lost_bytes > 0U || m_sent < m_written || fin_owed();
}

QuicSendChunk QuicSendBuffer::next(usize max_bytes, u64 fresh_limit) const noexcept
{
    QuicSendChunk chunk;
    if (m_bytes.empty())
    {
        return chunk;
    }
    const Ring ring{capacity()};
    if (m_lost_bytes > 0U)
    {
        const u64 low   = max_u64(m_lost_low, m_acked);
        const u64 start = low + run(ring, m_lost_bits, low, m_sent - low, false);
        const u64 most  = min_u64(min_u64(max_bytes, m_sent - start), capacity() - ring.at(start));
        chunk.offset    = start;
        chunk.length    = static_cast<usize>(run(ring, m_lost_bits, start, most, true));
        // Lost data lies below sent_end(), so it ends at the final size only once everything was sent.
        chunk.fin = chunk.length != 0U && fin_owed() && start + chunk.length == m_written;
        return chunk;
    }
    const u64 limit = min_u64(fresh_limit, m_written);
    if (m_sent < limit)
    {
        chunk.offset = m_sent;
        chunk.length = static_cast<usize>(min_u64(min_u64(max_bytes, limit - m_sent), capacity() - ring.at(m_sent)));
        chunk.fin    = chunk.length != 0U && fin_owed() && m_sent + chunk.length == m_written;
        if (chunk.length != 0U)
        {
            return chunk;
        }
    }
    if (fin_owed() && m_sent == m_written)
    {
        chunk.offset = m_written;
        chunk.length = 0U;
        chunk.fin    = true;
    }
    return chunk;
}

cont::ConstSpan<u8> QuicSendBuffer::bytes_of(const QuicSendChunk& chunk) const noexcept
{
    if (m_bytes.empty() || chunk.length == 0U)
    {
        return {};
    }
    return cont::ConstSpan<u8>(m_bytes.data() + Ring{capacity()}.at(chunk.offset), chunk.length);
}

void QuicSendBuffer::on_sent(const QuicSendChunk& chunk) noexcept
{
    if (m_bytes.empty())
    {
        return;
    }
    const u64 end = chunk.offset + chunk.length;
    const u64 low = max_u64(chunk.offset, m_acked);
    const u64 high = min_u64(end, m_sent);
    if (low < high && m_lost_bytes > 0U)
    {
        m_lost_bytes -= mark(Ring{capacity()}, m_lost_bits, nullptr, low, high - low, false);
    }
    if (chunk.offset <= m_sent && end > m_sent)
    {
        m_sent = min_u64(end, m_written);
    }
    if (chunk.fin && m_finished && end == m_written)
    {
        m_fin_sent = true;
        m_fin_lost = false;
    }
}

void QuicSendBuffer::on_acked(u64 offset, usize length, bool fin) noexcept
{
    if (m_bytes.empty() || offset > kQuicMaxVarint)
    {
        return;
    }
    const Ring ring{capacity()};
    const u64  end  = offset + length;
    const u64  low  = max_u64(offset, m_acked);
    const u64  high = min_u64(end, m_sent);
    if (low < high)
    {
        if (m_lost_bytes > 0U)
        {
            m_lost_bytes -= mark(ring, m_lost_bits, nullptr, low, high - low, false);
        }
        if (low != m_acked)
        {
            fill(ring, m_acked_bits, low, high - low, true); // past a gap: acknowledged by its bits
            m_acked_marked = max_u64(m_acked_marked, high);
        }
        else
        {
            // In order: everything to `high` is acknowledged without its bits, and a run acknowledged early continues
            // it. Bits below the acknowledged end are cleared, as a wrapped offset needs them.
            u64 to = high;
            if (m_acked_marked > high)
            {
                to += run(ring, m_acked_bits, high, m_sent - high, true);
            }
            if (m_acked_marked > low)
            {
                fill(ring, m_acked_bits, low, to - low, false);
            }
            m_acked    = to;
            m_lost_low = max_u64(m_lost_low, m_acked);
        }
    }
    if (fin && m_fin_sent && end == m_written)
    {
        m_fin_acked = true;
        m_fin_lost  = false;
    }
}

void QuicSendBuffer::on_lost(u64 offset, usize length, bool fin) noexcept
{
    if (m_bytes.empty() || offset > kQuicMaxVarint)
    {
        return;
    }
    const u64 end  = offset + length;
    const u64 low  = max_u64(offset, m_acked);
    const u64 high = min_u64(end, m_sent);
    if (low < high)
    {
        // Bytes another copy already had acknowledged stay acknowledged.
        const u64 marked = mark(Ring{capacity()}, m_lost_bits, m_acked_bits.data(), low, high - low, true);
        if (marked > 0U)
        {
            m_lost_low = (m_lost_bytes == 0U) ? low : min_u64(m_lost_low, low);
            m_lost_bytes += marked;
        }
    }
    if (fin && m_fin_sent && end == m_written)
    {
        m_fin_lost = true;
    }
}

// ---- QuicSendCredit ----

void QuicSendCredit::reset(u64 limit) noexcept
{
    m_limit        = limit;
    m_blocked_at   = 0U;
    m_blocked_sent = false;
}

void QuicSendCredit::raise(u64 limit) noexcept
{
    m_limit = max_u64(m_limit, limit);
}

bool QuicSendCredit::blocked_owed(u64 used, bool waiting) const noexcept
{
    return waiting && used >= m_limit && !(m_blocked_sent && m_blocked_at == m_limit);
}

void QuicSendCredit::on_blocked_sent() noexcept
{
    m_blocked_at   = m_limit;
    m_blocked_sent = true;
}

void QuicSendCredit::on_blocked_lost(u64 limit) noexcept
{
    if (m_blocked_sent && m_blocked_at == limit)
    {
        m_blocked_sent = false;
    }
}

// ---- QuicReceiveWindow ----

void QuicReceiveWindow::reset(u64 window, u64 max_window) noexcept
{
    *this        = QuicReceiveWindow{};
    m_window     = min_u64(window, kQuicMaxVarint);
    m_max_window = max_u64(m_window, min_u64(max_window, kQuicMaxVarint));
    m_limit      = m_window;
}

bool QuicReceiveWindow::on_received(u64 end) noexcept
{
    if (end > m_limit)
    {
        return false;
    }
    m_received = max_u64(m_received, end);
    return true;
}

void QuicReceiveWindow::on_consumed(u64 consumed) noexcept
{
    m_consumed = max_u64(m_consumed, min_u64(consumed, m_limit));
}

void QuicReceiveWindow::on_peer_blocked() noexcept
{
    m_blocked = true;
}

bool QuicReceiveWindow::update_owed() const noexcept
{
    return m_lost || raise_owed();
}

bool QuicReceiveWindow::raise_owed() const noexcept
{
    const u64 next = min_u64(m_consumed + m_window, kQuicMaxVarint);
    if (next <= m_limit)
    {
        return false;
    }
    // m_consumed never passes m_limit, so the credit left is m_limit - m_consumed.
    return m_blocked || (m_limit - m_consumed) * 2U <= m_window;
}

u64 QuicReceiveWindow::take_update(u64 now_ns, u64 rtt_ns) noexcept
{
    if (!raise_owed())
    {
        // Only a lost update is owed: it goes again as it was (section 13.3), which says nothing about the reader's
        // pace, so the window is not tuned by it.
        m_lost = false;
        return m_limit;
    }
    const u64 rtt = min_u64(rtt_ns, kMaxRttNs);
    if (m_updated && now_ns >= m_updated_ns && now_ns - m_updated_ns < 2U * rtt)
    {
        m_window = min_u64(m_window * 2U, m_max_window);
    }
    m_limit      = max_u64(m_limit, min_u64(m_consumed + m_window, kQuicMaxVarint));
    m_updated    = true;
    m_updated_ns = now_ns;
    m_blocked    = false;
    m_lost       = false;
    return m_limit;
}

void QuicReceiveWindow::on_update_lost(u64 limit) noexcept
{
    if (m_updated && limit == m_limit)
    {
        m_lost = true;
    }
}

} // namespace crd::net
