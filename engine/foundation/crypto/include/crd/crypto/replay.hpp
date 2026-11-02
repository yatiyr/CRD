#pragma once

// crd-crypto -- anti-replay (NET.2, threat T07; contract: docs/design/network-foundation.md#anti-replay).
//
// The two records a receiver keeps so that an authenticated message an attacker captured and sends again is refused:
//
//   ReplayWindow  sequence numbers, each used once and mostly in order (QUIC packet numbers per packet number space,
//                 RFC 9000 section 12.3; the counters of later media and datagram channels): exact over the kSpan
//                 numbers that end at the largest accepted, every older one refused as Stale.
//   ReplayFilter  random identifiers, each used once in any order over a lifetime (QUIC NEW_TOKEN tokens, RFC 9000
//                 section 8.1.4; the single-use tickets 0-RTT accepts, RFC 8446 section 8.1): a Bloom filter by issue
//                 period, whose rare false positive refuses a first use and never accepts a replay.
//
// Rules:
//
//   order     a receiver checks a number before it opens the message's AEAD (a replay costs no decryption) and
//             records it only after the AEAD opened, so a forged message never moves a window or fills a filter.
//   scope     each record covers one receiver: the servers of a fleet do not share one, so an identifier accepted by
//             one server is still accepted once by each other (RFC 8446 section 8's zones), which is why early data
//             carries only idempotent operations.
//   threads   a window has one receiver (its packet number space's); a filter's check runs on several threads at
//             once, and init and clear need the caller's exclusive access.
//   allocation a window holds its bits inline and never allocates; a filter allocates its bits once, at init.

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/crypto/provider.hpp>

namespace crd::crypto
{
// What a window says of a sequence number.
enum class ReplayVerdict : u8
{
    Fresh,     // never accepted: process it
    Duplicate, // accepted before: drop it
    Stale,     // older than the window remembers: drop it, since it cannot be told from a replay
};

// The sequence numbers a receiver accepted on one channel: the largest, and a bitmap of the `Bits` numbers ending in
// its 64-bit word (RFC 6479's ring of words: moving the largest up clears only the words it passes, never shifts).
// The kSpan numbers that end at the largest are remembered exactly; anything older is Stale. 8 bytes a 64 numbers
// plus 16: a 256-bit window, QUIC's, is 48 bytes.
template <usize Bits>
class ReplayWindow
{
    static_assert(Bits >= 128U && (Bits & (Bits - 1U)) == 0U, "a power of two of at least two 64-bit words");

public:
    static constexpr u64 kSpan = Bits - 64U; // the numbers, ending at the largest, it remembers

    [[nodiscard]] bool any() const noexcept { return m_any; }
    // The largest number accepted; 0 before any (any() tells the two apart).
    [[nodiscard]] u64 largest() const noexcept { return m_largest; }

    [[nodiscard]] ReplayVerdict check(u64 number) const noexcept
    {
        if (!m_any || number > m_largest)
        {
            return ReplayVerdict::Fresh;
        }
        if (m_largest - number >= kSpan)
        {
            return ReplayVerdict::Stale;
        }
        return (m_words[word_of(number)] & bit_of(number)) != 0U ? ReplayVerdict::Duplicate : ReplayVerdict::Fresh;
    }

    // Records `number`, whose message authenticated: Fresh when it was new (and is now remembered), else what check()
    // says, and nothing changes.
    ReplayVerdict accept(u64 number) noexcept
    {
        const ReplayVerdict verdict = check(number);
        if (verdict != ReplayVerdict::Fresh)
        {
            return verdict;
        }
        if (!m_any)
        {
            m_largest = number;
            m_any     = true;
        }
        else if (number > m_largest)
        {
            // The words above the largest's, up to the new one's, held only numbers now older than the span.
            const u64 passed = number / 64U - m_largest / 64U;
            const u64 count  = passed < kWords ? passed : kWords;
            for (u64 i = 1U; i <= count; ++i)
            {
                m_words[word_of(m_largest + i * 64U)] = 0U;
            }
            m_largest = number;
        }
        m_words[word_of(number)] |= bit_of(number);
        return ReplayVerdict::Fresh;
    }

    void clear() noexcept { *this = ReplayWindow(); }

private:
    static constexpr usize kWords = Bits / 64U;

    [[nodiscard]] static usize word_of(u64 number) noexcept { return static_cast<usize>((number / 64U) % kWords); }
    [[nodiscard]] static u64   bit_of(u64 number) noexcept { return u64{1} << (number % 64U); }

    u64  m_words[kWords] = {};
    u64  m_largest       = 0U;
    bool m_any           = false;
};

inline constexpr usize kReplayIdBytes = 16U; // 128 random bits: what a filter reads of an identifier

class ReplayFilter
{
public:
    ReplayFilter() noexcept = default;
    ReplayFilter(const ReplayFilter&)            = delete;
    ReplayFilter& operator=(const ReplayFilter&) = delete;
    ReplayFilter(ReplayFilter&&)                 = delete;
    ReplayFilter& operator=(ReplayFilter&&)      = delete;

    // Room for `items` first uses each `period` seconds, from `now`. An identifier is filed by the period it was
    // issued in, in one of four slots: the periods an accepted identifier can come from (the last, this and the next,
    // for an issuer whose clock runs ahead) and the one after, which is cleared before it begins. So the period is at
    // least an identifier's lifetime and its issuers' clock skew, which its user checks (QuicTokenKeys::validate).
    // Two bytes an item in each slot keep false positives near 0.1% at capacity. BadLength when `items` is 0 or more
    // than 2^37 (2^32 cache-line blocks), or `period` is 0.
    [[nodiscard]] CryptoError init(usize items, u64 period, u64 now);
    void                      clear() noexcept;

    [[nodiscard]] bool  ready() const noexcept { return m_blocks != 0U; }
    [[nodiscard]] u64   period() const noexcept { return m_period; }
    [[nodiscard]] usize bytes() const noexcept { return m_bits.size() * sizeof(Block); }

    // True the first time the identifier `id` (kReplayIdBytes of random, authenticated bytes: a token's or a ticket's
    // salt, never anything a peer chooses), issued at `issued`, is seen at `now`; false when it was seen before, for a
    // false positive, or for an identifier too old for any slot. One identifier checked on two threads at the same
    // instant may be accepted by both, and one checked while its slot is being cleared, after a whole period without
    // checks, is accepted unrecorded; a check after either returned refuses it.
    [[nodiscard]] bool first_use(cont::ConstSpan<u8> id, u64 issued, u64 now) noexcept;

private:
    static constexpr usize kSlots = 4U;
    struct alignas(64) Block
    {
        u64 words[8];
    };

    void prepare(u64 epoch) noexcept;

    cont::Array<Block> m_bits;
    usize              m_blocks       = 0U;
    u64                m_period       = 0U;
    u64                m_tags[kSlots] = {}; // each slot's period + 1 (0: none), read and written atomically
};
} // namespace crd::crypto
