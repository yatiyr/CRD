#pragma once

// entropy.hpp — static entropy coding of one small message (capability compression.codecs; contract:
// docs/design/network-foundation.md#body-coding). A range variant of asymmetric numeral systems (Duda, "Asymmetric
// numeral systems: entropy coding combining speed of Huffman coding with compression rate of arithmetic coding",
// arXiv:1311.2540) with one state and byte renormalisation, so it costs three bytes of state per message, under a model
// trained once and held by both ends: a datagram decodes alone, whatever was lost before it. Nothing here allocates.
//
// Each byte is coded in one of four contexts, chosen by the byte before it. Bit-packed deltas leave long runs of zero
// bits and small differences, and those classes capture most of what a whole order-1 table would, in a sixty-fourth of
// the memory and without its dilution on short messages.
//
// A coded message has one encoding: decoding reverses every step of coding exactly, and a decode that does not end on
// the starting state with every byte used is refused.

#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>

namespace crd::compression
{

inline constexpr crd::usize kEntropyContexts = 4U;
inline constexpr crd::usize kEntropySymbols = 256U;
// A context's frequencies sum to 2^12.
inline constexpr crd::u32 kEntropyScaleBits = 12U;
inline constexpr crd::u32 kEntropyScale = 1U << kEntropyScaleBits;
// A model's source: each context's frequencies as 16-bit little-endian numbers, every one at least 1.
inline constexpr crd::usize kEntropyModelBytes = kEntropyContexts * kEntropySymbols * 2U;
// What a coded message spends on its state.
inline constexpr crd::usize kEntropyStateBytes = 3U;

// Counts what samples hold and writes a model's source from them.
class EntropyTrainer
{
public:
    void add(crd::containers::ConstSpan<crd::u8> sample) noexcept;

    // Writes kEntropyModelBytes: frequencies by largest remainder, every symbol staying codable.
    void write_model(crd::containers::Span<crd::u8> out) const noexcept;

private:
    crd::u64 m_counts[kEntropyContexts][kEntropySymbols] = {};
};

// A model's tables. Read-only after load(), so any number of threads may code with it at once.
class EntropyModel
{
public:
    // False, leaving the model unusable, when `source` is not a model: the wrong size, a frequency of zero, or a
    // context whose frequencies do not sum to the scale.
    [[nodiscard]] bool load(crd::containers::ConstSpan<crd::u8> source) noexcept;

    // Codes `in` at the start of `out` and returns the size; zero when `out` is too small, which a caller uses to give
    // up once coding has not paid.
    [[nodiscard]] crd::usize encode(crd::containers::ConstSpan<crd::u8> in,
                                    crd::containers::Span<crd::u8> out) const noexcept;

    // True only when `in` is the coding of exactly out.size() bytes. Hostile input: the work is linear in out.size().
    [[nodiscard]] bool decode(crd::containers::ConstSpan<crd::u8> in,
                              crd::containers::Span<crd::u8> out) const noexcept;

private:
    struct Symbol
    {
        crd::u16 frequency = 0U;
        crd::u16 start = 0U;
    };

    Symbol m_symbols[kEntropyContexts][kEntropySymbols] = {};
    crd::u8 m_slots[kEntropyContexts][kEntropyScale] = {};
    bool m_loaded = false;
};

} // namespace crd::compression
