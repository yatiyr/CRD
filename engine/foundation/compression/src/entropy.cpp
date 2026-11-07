#include <crd/compression/entropy.hpp>

#include <cstring>

namespace crd::compression
{
namespace
{
// The state stays in [kLow, kLow * 256), a multiple of the scale, so coding and decoding are exact inverses. The range
// keeps the state to three bytes, which a message of tens of bytes notices, and sixteen times the scale keeps the coder
// close to the model's entropy.
constexpr crd::u32 kLow = 1U << 16U;
constexpr crd::u32 kRenormShift = 8U;
// A symbol of frequency f may be coded from a state below kCeiling * f.
constexpr crd::u32 kCeiling = (kLow >> kEntropyScaleBits) << kRenormShift;

[[nodiscard]] constexpr crd::u8 classify(crd::u8 byte) noexcept
{
    if (byte == 0U)
    {
        return 0U;
    }
    if (byte < 16U)
    {
        return 1U;
    }
    return ((byte & 15U) == 0U) ? 2U : 3U;
}

struct ContextTable
{
    crd::u8 of[kEntropySymbols] = {};
};

[[nodiscard]] constexpr ContextTable make_contexts() noexcept
{
    ContextTable table;
    for (crd::usize byte = 0U; byte < kEntropySymbols; ++byte)
    {
        table.of[byte] = classify(static_cast<crd::u8>(byte));
    }
    return table;
}

constexpr ContextTable kContexts = make_contexts();

// A frequency for each symbol of `counts`, at least 1, summing to the scale.
void normalize(const crd::u64 (&counts)[kEntropySymbols], crd::u16 (&frequencies)[kEntropySymbols]) noexcept
{
    crd::u64 total = 0U;
    for (const crd::u64 count : counts)
    {
        total += count;
    }
    if (total == 0U)
    {
        for (crd::u16& frequency : frequencies)
        {
            frequency = static_cast<crd::u16>(kEntropyScale / kEntropySymbols);
        }
        return;
    }
    crd::u32 assigned = 0U;
    crd::u64 remainders[kEntropySymbols] = {};
    for (crd::usize symbol = 0U; symbol < kEntropySymbols; ++symbol)
    {
        // A count times the scale cannot wrap: samples stay far below 2^52 bytes.
        const crd::u64 scaled = counts[symbol] * kEntropyScale;
        const crd::u64 share = scaled / total;
        remainders[symbol] = scaled % total;
        frequencies[symbol] = static_cast<crd::u16>((share == 0U) ? 1U : share);
        assigned += frequencies[symbol];
    }
    while (assigned < kEntropyScale)
    {
        crd::usize best = 0U;
        for (crd::usize symbol = 1U; symbol < kEntropySymbols; ++symbol)
        {
            if (remainders[symbol] > remainders[best])
            {
                best = symbol;
            }
        }
        remainders[best] = 0U;
        ++frequencies[best];
        ++assigned;
    }
    while (assigned > kEntropyScale)
    {
        // The largest frequency loses least per unit taken.
        crd::usize best = 0U;
        for (crd::usize symbol = 1U; symbol < kEntropySymbols; ++symbol)
        {
            if (frequencies[symbol] > frequencies[best])
            {
                best = symbol;
            }
        }
        --frequencies[best];
        --assigned;
    }
}
} // namespace

void EntropyTrainer::add(crd::containers::ConstSpan<crd::u8> sample) noexcept
{
    crd::u8 context = 0U;
    for (const crd::u8 byte : sample)
    {
        ++m_counts[context][byte];
        context = kContexts.of[byte];
    }
}

void EntropyTrainer::write_model(crd::containers::Span<crd::u8> out) const noexcept
{
    crd::usize at = 0U;
    for (const auto& counts : m_counts)
    {
        crd::u16 frequencies[kEntropySymbols] = {};
        normalize(counts, frequencies);
        for (const crd::u16 frequency : frequencies)
        {
            out[at] = static_cast<crd::u8>(frequency & 0xFFU);
            out[at + 1U] = static_cast<crd::u8>(frequency >> 8U);
            at += 2U;
        }
    }
}

bool EntropyModel::load(crd::containers::ConstSpan<crd::u8> source) noexcept
{
    m_loaded = false;
    if (source.size() != kEntropyModelBytes)
    {
        return false;
    }
    crd::usize at = 0U;
    for (crd::usize context = 0U; context < kEntropyContexts; ++context)
    {
        crd::u32 start = 0U;
        for (crd::usize symbol = 0U; symbol < kEntropySymbols; ++symbol)
        {
            const crd::u32 frequency = crd::u32{source[at]} | (crd::u32{source[at + 1U]} << 8U);
            at += 2U;
            if (frequency == 0U || frequency > kEntropyScale - start)
            {
                return false;
            }
            m_symbols[context][symbol] = Symbol{static_cast<crd::u16>(frequency), static_cast<crd::u16>(start)};
            std::memset(&m_slots[context][start], static_cast<int>(symbol), frequency);
            start += frequency;
        }
        if (start != kEntropyScale)
        {
            return false;
        }
    }
    m_loaded = true;
    return true;
}

crd::usize EntropyModel::encode(crd::containers::ConstSpan<crd::u8> in,
                                crd::containers::Span<crd::u8> out) const noexcept
{
    if (!m_loaded)
    {
        return 0U;
    }
    // Decoding reads in the opposite order, so coding runs from the last byte and fills `out` from its end.
    crd::u32 state = kLow;
    crd::usize at = out.size();
    for (crd::usize i = in.size(); i != 0U; --i)
    {
        const crd::u8 context = (i == 1U) ? crd::u8{0U} : kContexts.of[in[i - 2U]];
        const Symbol symbol = m_symbols[context][in[i - 1U]];
        const crd::u32 frequency = symbol.frequency;
        const crd::u32 ceiling = kCeiling * frequency;
        while (state >= ceiling)
        {
            if (at == 0U)
            {
                return 0U;
            }
            out[--at] = static_cast<crd::u8>(state & 0xFFU);
            state >>= kRenormShift;
        }
        state = ((state / frequency) << kEntropyScaleBits) + (state % frequency) + symbol.start;
    }
    if (at < kEntropyStateBytes)
    {
        return 0U;
    }
    at -= kEntropyStateBytes;
    out[at] = static_cast<crd::u8>(state >> 16U);
    out[at + 1U] = static_cast<crd::u8>((state >> 8U) & 0xFFU);
    out[at + 2U] = static_cast<crd::u8>(state & 0xFFU);
    const crd::usize size = out.size() - at;
    if (at != 0U)
    {
        std::memmove(out.data(), out.data() + at, size);
    }
    return size;
}

bool EntropyModel::decode(crd::containers::ConstSpan<crd::u8> in, crd::containers::Span<crd::u8> out) const noexcept
{
    if (!m_loaded || in.size() < kEntropyStateBytes)
    {
        return false;
    }
    crd::u32 state = (crd::u32{in[0]} << 16U) | (crd::u32{in[1]} << 8U) | crd::u32{in[2]};
    if (state < kLow)
    {
        return false;
    }
    crd::usize at = kEntropyStateBytes;
    crd::u8 context = 0U;
    for (crd::u8& byte : out)
    {
        const crd::u32 slot = state & (kEntropyScale - 1U);
        const crd::u8 value = m_slots[context][slot];
        const Symbol symbol = m_symbols[context][value];
        byte = value;
        state = (symbol.frequency * (state >> kEntropyScaleBits)) + slot - symbol.start;
        while (state < kLow)
        {
            if (at == in.size())
            {
                return false;
            }
            state = (state << kRenormShift) | in[at];
            ++at;
        }
        context = kContexts.of[value];
    }
    return state == kLow && at == in.size();
}

} // namespace crd::compression
