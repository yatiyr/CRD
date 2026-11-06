// Interleavers against burst loss (HESAP-NET, capability numerics.erasure-codes, owner crd-hesap-net).
//
// A burst of consecutive losses on the wire takes consecutive symbols of one codeword unless the sender reorders
// them; an interleaver spreads a burst over many codewords, each of which an erasure code (erasure.hpp, flexfec.hpp)
// then repairs. A symbol is any fixed number of bytes, or a packet the consumer schedules by position.
//
// Block interleaver: R rows of C symbols are written row by row (symbol r·C + c, so a row is one codeword) and sent
// column by column (position c·R + r). Every R consecutive positions hold one symbol of each row, so a burst of at
// most n·R positions costs any row at most n symbols. The delay is the whole block.
//
// Convolutional (Forney) interleaver of B branches and depth M: symbol k goes to branch b = k mod B, which holds it
// for b·M turns of the commutator, so it leaves at time k + b·M·B; the deinterleaver holds branch b for (B − 1 − b)·M
// turns, so every symbol comes back in order, delay() = (B − 1)·M·B symbols late. The B symbols of one turn
// (k = g·B .. g·B + B − 1) leave M·B + 1 apart, so a burst of at most M·B + 1 positions takes at most one of them. It
// delays and stores half of what a block interleaver of the same spread does: M·B·(B − 1)/2 symbols per side. The
// symbols out before the first delay() went in are zeros. Nothing allocates.

#pragma once

#include <crd/core/types.hpp>

#include <span>

namespace crd::hesap::net
{

// The sending position of symbol `index` of a rows x columns block, and the symbol at a sending position.
[[nodiscard]] constexpr crd::u64 block_interleave_position(crd::u32 rows, crd::u32 columns, crd::u64 index) noexcept
{
    return ((index % columns) * rows) + (index / columns);
}

[[nodiscard]] constexpr crd::u64 block_interleave_index(crd::u32 rows, crd::u32 columns, crd::u64 position) noexcept
{
    return ((position % rows) * columns) + (position / rows);
}

// Moves the rows·columns symbols of `in` (symbol_bytes each, row by row) into `out` in sending order; the
// deinterleaver moves them back. False, writing nothing, when rows, columns or symbol_bytes is 0, `in` is not
// rows·columns symbols, `out` is shorter or the two overlap.
[[nodiscard]] bool block_interleave(std::span<const crd::u8> in, std::span<crd::u8> out, crd::u32 rows,
                                    crd::u32 columns, crd::usize symbol_bytes) noexcept;
[[nodiscard]] bool block_deinterleave(std::span<const crd::u8> in, std::span<crd::u8> out, crd::u32 rows,
                                      crd::u32 columns, crd::usize symbol_bytes) noexcept;

class ConvolutionalInterleaver
{
public:
    enum class Role : crd::u8
    {
        Interleave,
        Deinterleave,
    };

    static constexpr crd::u32 kMaxBranches = 0xFFFFU;
    static constexpr crd::u32 kMaxDepth = 0xFFFFU;

    // The storage one side binds, M·B·(B − 1)/2 symbols; 0 when the parameters are outside init()'s range.
    [[nodiscard]] static crd::usize storage_bytes(crd::u32 branches, crd::u32 depth, crd::usize symbol_bytes) noexcept;

    [[nodiscard]] static constexpr crd::u64 delay(crd::u32 branches, crd::u32 depth) noexcept
    {
        return crd::u64{branches - 1U} * depth * branches;
    }

    // Binds `storage`, exactly storage_bytes() long, and clears it. False, binding nothing, when branches is not
    // 1..kMaxBranches, depth not 1..kMaxDepth, symbol_bytes is 0 or the storage is another size.
    [[nodiscard]] bool init(Role role, crd::u32 branches, crd::u32 depth, crd::usize symbol_bytes,
                            std::span<crd::u8> storage) noexcept;

    // One symbol in, one out, symbol_bytes each; `in` and `out` are the same bytes or do not overlap.
    void push(std::span<const crd::u8> in, std::span<crd::u8> out) noexcept;

private:
    crd::u8* m_storage = nullptr;
    crd::usize m_symbol = 0U;
    crd::u64 m_turn = 0U;
    crd::u32 m_branches = 0U;
    crd::u32 m_depth = 0U;
    crd::u32 m_branch = 0U;
    Role m_role = Role::Interleave;
};

} // namespace crd::hesap::net
