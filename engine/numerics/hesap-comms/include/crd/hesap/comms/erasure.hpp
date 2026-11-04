// Packet erasure codes (HESAP-NET; capability numerics.erasure-codes, owner crd-hesap-comms).
//
// ReedSolomonCode is RFC 5510 §8's systematic Reed-Solomon code over GF(2^8) (crd/hesap/comms/gf256.hpp): a block of
// k source symbols has n encoding symbols, k <= n <= 255, of which the first k are the sources and the rest repair
// symbols, and any k of the n recover the block. Every symbol of a block is `bytes` long; the RFC's virtual zero
// padding of a short final source symbol is the caller's. The generator matrix is GM = V(k,k)^-1 V(k,n) with
// V's entries alpha^(i·j), so column j of GM is the Lagrange basis on the points alpha^0 .. alpha^(k-1) evaluated at
// alpha^j: repair symbol j is the value at alpha^j of the polynomial of degree below k through the sources, and
// decoding interpolates through the k received points. Both use barycentric weights, O(k^2) once per block shape or
// erasure pattern and O(k) per coefficient column, instead of the RFC's Gauss-Jordan inversion; the bytes are the
// RFC's, which the tests hold to the literal matrix product. The regions go through the dispatched GF(2^8) dot product
// (crd/hesap/comms/erasure_dispatch.hpp). Nothing allocates.

#pragma once

#include <crd/core/types.hpp>

#include <span>

namespace crd::hesap::comms
{

// A refused request reports its first fault in this order: the counts; then each received symbol (or repair index)
// in order, its range before a repeat of an earlier one; then each lost index in order, its range, a repeat, then
// whether it was received.
enum class ErasureStatus : crd::u8
{
    Ok,
    WrongCount,      // the received symbols are not exactly k, or the outputs do not match the indices
    IndexOutOfRange, // an encoding symbol index is not below n, a repair index not in [k, n), a lost one not below k
    DuplicateIndex,  // two received symbols, or two requested outputs, carry the same index
    NotLost,         // a source index asked for is among the received symbols
};

// One received encoding symbol: its index in [0, n) and its `bytes` of data.
struct ErasureSymbol
{
    crd::u32 index = 0U;
    const crd::u8* data = nullptr;
};

class ReedSolomonCode
{
public:
    static constexpr crd::u32 kMaxSymbols = 255U;

    [[nodiscard]] static constexpr bool valid(crd::u32 k, crd::u32 n) noexcept
    {
        return k >= 1U && k <= n && n <= kMaxSymbols;
    }

    // A block of k source symbols and n encoding symbols; valid(k, n) must hold.
    ReedSolomonCode(crd::u32 k, crd::u32 n) noexcept;

    [[nodiscard]] crd::u32 source_count() const noexcept { return k_; }
    [[nodiscard]] crd::u32 symbol_count() const noexcept { return n_; }

    // GM's entry in row i < k and column j < n.
    [[nodiscard]] crd::u8 coefficient(crd::u32 i, crd::u32 j) const noexcept;

    // outputs[q] = encoding symbol repair[q], each index in [k, n) and none twice, from the k sources in order.
    [[nodiscard]] ErasureStatus encode(std::span<const crd::u8* const> sources, std::span<const crd::u32> repair,
                                       std::span<crd::u8* const> outputs, crd::usize bytes) const noexcept;

    // From exactly k received encoding symbols with distinct indices, outputs[q] = source symbol lost[q], each index
    // below k, not received and not asked for twice. Nothing is written unless the result is Ok.
    [[nodiscard]] ErasureStatus decode(std::span<const ErasureSymbol> received, std::span<const crd::u32> lost,
                                       std::span<crd::u8* const> outputs, crd::usize bytes) const noexcept;

private:
    crd::u32 k_;
    crd::u32 n_;
    crd::u8 weight_logs_[kMaxSymbols]; // log of each source point's weight 1 / prod_{m != l} (alpha^l - alpha^m)
};

} // namespace crd::hesap::comms
