// RFC 6330's RaptorQ fountain code (HESAP-NET; capability numerics.erasure-codes, owner crd-hesap-net).
//
// A block of K source symbols, 1 <= K <= 56403, is extended by zero padding symbols to the next K' of the RFC's
// Table 2. L > K' intermediate symbols C are the solution of A·C = D, where A holds the pre-coding relations (S binary
// LDPC rows and H octet HDPC rows, whose right side is zero) and one binary row per encoding symbol. Any encoding
// symbol is the XOR of a few of C (raptorq_symbol), so an encoder solves for C from the source symbols and a decoder
// solves for C from any K' or a few more encoding symbols with distinct ESIs, then reads its lost source symbols the
// same way.
//
// RaptorqSolver is §5.4.2's inactivation decoding in block form, the same schedule without a matrix to rewrite (the
// raptorq crate's errata 9 to 11 observe the core of this). The first phase chooses rows and columns so that
// A = [V1 U1; V2 U2], V1 lower triangular with a unit diagonal. It only removes columns from V, so it never creates a
// nonzero there, and all of V1, V2, U1 and U2 stay A's own sparse entries. Then
//     S = U2 - V2·V1^-1·U1     S·C_u = D2 - V2·V1^-1·D1     C_v = V1^-1·(D1 - U1·C_u)
// S is the dense Schur complement of §5.4.2.3, u columns by every row not chosen. Its bit rows come from a forward
// substitution, the H HDPC rows go through MT·GAMMA's recurrence without a table of G_HDPC, and it is factored once. So
// a matrix costs O(nonzeros·u/64 + (K'+S)·u + rows·u^2) one time, and each set of T-byte symbols only 2·nonzeros XORs,
// (K'+S) runs of the recurrence and u^2 products, all through the dispatched GF(2^8) kernels. A plan solves any number
// of blocks of one K and one set of ESIs: an encoder's, for a K, solves every block of that K.
//
// Symbols may sit at any alignment, but a vector crossing a cache line costs two: a caller who wants the speed of
// docs/bench/2026-11-07-hesap-net-raptorq-stalls.md puts the sources, `intermediate`, `scratch` and the outputs on
// 64-byte lines (1.36x on AVX-512 at K = 10, against an 8-byte skew).
//
// Nothing allocates. analyze takes memory for the sparse matrix, sized exactly by structure_bytes. Only then is u
// known, so factor takes memory sized by factor_bytes, and solve its scratch sized by solve_scratch_bytes. With the K'
// + o rows an o of a few gives, u is a few more than the P of Table 2 (a few hundred at most), so the dense part is
// small. A decoder is given every row it receives and uses them all: feeding it many more than K' + 2 costs rows·u^2.
//
// §5.8: analyze and factor refuse with RaptorqStatus::Singular only when the rows are mathematically insufficient,
// because S has full column rank exactly when A does. An attacker who chooses the ESIs can only cost a sender's
// choice of K' + o rows a few inactivations or a refusal, never a wrong answer.
//
// The rest is §3 and §4: the FEC payload ID and object transmission information, §4.3's parameter derivation, and
// §4.4.1.2's partition of an object into source blocks and sub-blocks, with the sub-symbol interleaving of N > 1.

#pragma once

#include <crd/core/types.hpp>

#include <span>

namespace crd::hesap::net
{

inline constexpr crd::u32 kRaptorqMaxSourceSymbols = 56403U;           // K'_max
inline constexpr crd::u32 kRaptorqEsiLimit = 1U << 24U;                // the FEC payload ID's 24 bits
inline constexpr crd::u32 kRaptorqEncodingId = 6U;                     // §3.3.1, assigned by IANA
inline constexpr crd::u64 kRaptorqMaxTransferLength = 942574504275ULL; // §3.3.2 as verified erratum 5548 corrects it
inline constexpr crd::usize kRaptorqPayloadIdBytes = 4U;               // §3.2
inline constexpr crd::usize kRaptorqOtiBytes = 12U;                    // §3.3.3
inline constexpr crd::u32 kRaptorqMaxSourceBlocks = 255U;              // Z's 8 bits, a positive integer (§3.3.3)
inline constexpr crd::u32 kRaptorqMaxSubBlocks = 65535U;               // N's 16 bits

// One row of Table 2.
struct RaptorqSystematicRow
{
    crd::u16 k_prime;
    crd::u16 j;
    crd::u16 s;
    crd::u16 h;
    crd::u16 w;
};

// The derived parameters of §5.3.3.3 for one K'.
struct RaptorqParameters
{
    crd::u32 k_prime = 0U;
    crd::u32 j = 0U;
    crd::u32 s = 0U;
    crd::u32 h = 0U;
    crd::u32 w = 0U;
    crd::u32 l = 0U;  // K' + S + H
    crd::u32 p = 0U;  // L - W
    crd::u32 p1 = 0U; // the smallest prime >= P
    crd::u32 u = 0U;  // P - H
    crd::u32 b = 0U;  // W - S
};

// The tuple of §5.3.5.4.
struct RaptorqTuple
{
    crd::u32 d = 0U;
    crd::u32 a = 0U;
    crd::u32 b = 0U;
    crd::u32 d1 = 0U;
    crd::u32 a1 = 0U;
    crd::u32 b1 = 0U;
};

// Table 2, ascending in K'.
[[nodiscard]] std::span<const RaptorqSystematicRow> raptorq_systematic_rows() noexcept;

// The parameters of the smallest K' >= k: false, and `out` as it was, unless 1 <= k <= K'_max.
[[nodiscard]] bool raptorq_parameters(crd::u32 k, RaptorqParameters& out) noexcept;

// Rand[y, i, m] of §5.3.5.1 for i < 256 and m > 0.
[[nodiscard]] crd::u32 raptorq_rand(crd::u32 y, crd::u32 i, crd::u32 m) noexcept;

// Deg[v] of §5.3.5.2 for v < 2^20.
[[nodiscard]] crd::u32 raptorq_degree(crd::u32 v, crd::u32 w) noexcept;

// Tuple[K', X] of §5.3.5.4.
[[nodiscard]] RaptorqTuple raptorq_tuple(const RaptorqParameters& parameters, crd::u32 isi) noexcept;

// The ISI of an encoding symbol of a block of k source symbols (§5.3.2).
[[nodiscard]] constexpr crd::u32 raptorq_isi(const RaptorqParameters& parameters, crd::u32 k, crd::u32 esi) noexcept
{
    return esi < k ? esi : esi + (parameters.k_prime - k);
}

// Enc[K', C, Tuple[K', isi]] of §5.3.5.3: output = the XOR of its intermediate symbols. `intermediate` holds C[0] ..
// C[L-1], `bytes` each; `output` overlaps none of them.
void raptorq_symbol(const RaptorqParameters& parameters, const crd::u8* intermediate, crd::usize bytes, crd::u32 isi,
                    crd::u8* output) noexcept;

// A refused request reports its first fault in this order.
enum class RaptorqStatus : crd::u8
{
    Ok,
    BadBlockSize,      // k is not in 1 .. K'_max
    EsiOutOfRange,     // an ESI is not below 2^24
    TooFewSymbols,     // fewer than K' rows with the padding symbols
    WorkspaceTooSmall, // a memory span is smaller than its sizing call says
    DuplicateEsi,      // two encoding symbols carry one ESI
    BadSequence,       // factor before analyze, or solve before factor
    WrongCount,        // solve's symbols do not match what analyze was given
    Singular,          // the rows do not determine C: more symbols are needed
};

// The solver of one K and one set of encoding symbols. It holds pointers into the memory it is given, which outlives
// it, and solve only reads them, so one solver serves several threads each with its own scratch. A failed call leaves
// it unprepared.
class RaptorqSolver
{
public:
    // The bytes analyze needs, at any alignment: for the encoder of a block of k source symbols, or for a decoder of
    // the encoding symbols `esis` of such a block. 0 when analyze would refuse the request before looking at its
    // memory.
    [[nodiscard]] static crd::usize structure_bytes(crd::u32 k) noexcept;
    [[nodiscard]] static crd::usize structure_bytes(crd::u32 k, std::span<const crd::u32> esis) noexcept;

    // The first phase on the encoder's rows, ESIs 0 .. k-1 and the padding.
    [[nodiscard]] RaptorqStatus analyze(crd::u32 k, std::span<crd::u8> structure) noexcept;

    // The first phase on a decoder's rows: the encoding symbols `esis`, in solve's order, and the padding.
    [[nodiscard]] RaptorqStatus analyze(crd::u32 k, std::span<const crd::u32> esis,
                                        std::span<crd::u8> structure) noexcept;

    [[nodiscard]] const RaptorqParameters& parameters() const noexcept { return m_parameters; }

    // After analyze: the number of inactivated columns, u of §5.4.2.2, and the rows left for the Schur complement.
    [[nodiscard]] crd::u32 inactivated() const noexcept { return m_u; }
    [[nodiscard]] crd::u32 schur_rows() const noexcept { return m_schur_rows; }

    // After analyze: the bytes factor needs, at any alignment.
    [[nodiscard]] crd::usize factor_bytes() const noexcept;

    // The second phase: forms and factors S. Singular when its rank is below u.
    [[nodiscard]] RaptorqStatus factor(std::span<crd::u8> memory) noexcept;

    // After factor: bytes solve needs as scratch for symbols of `bytes` bytes.
    [[nodiscard]] crd::usize solve_scratch_bytes(crd::usize bytes) const noexcept;

    // intermediate = C[0] .. C[L-1], `bytes` each, from one symbol per row given to analyze (`symbols[i]` for ESI i at
    // the encoder, or for `esis[i]` at the decoder; a null symbol is all zeros); nothing overlaps. `bytes` may be 0.
    [[nodiscard]] RaptorqStatus solve(std::span<const crd::u8* const> symbols, crd::usize bytes,
                                      std::span<crd::u8> intermediate, std::span<crd::u8> scratch) const noexcept;

private:
    [[nodiscard]] RaptorqStatus analyze_rows(crd::u32 k, const crd::u32* esis, crd::u32 received,
                                             std::span<crd::u8> structure) noexcept;
    void select_rows() noexcept;
    void retire_column(crd::u32 column) noexcept;
    void choose_row(crd::u32 row, crd::u32 diagonal) noexcept;
    [[nodiscard]] crd::u32 pick_degree_two_row() noexcept;
    [[nodiscard]] crd::u32 pick_sparsest_row(crd::u32 bucket) const noexcept;
    void unlink_row(crd::u32 row) noexcept;
    void link_row(crd::u32 row) noexcept;
    void partition_entries() noexcept;

    RaptorqParameters m_parameters;
    crd::u32 m_k = 0U;
    crd::u32 m_received = 0U;    // encoding symbols, before the padding rows
    crd::u32 m_binary_rows = 0U; // S + received + padding
    crd::u32 m_chosen = 0U;      // i of §5.4.2.2
    crd::u32 m_u = 0U;
    crd::u32 m_schur_rows = 0U; // the binary rows not chosen, then the H HDPC rows
    crd::u32 m_active_columns = 0U;
    bool m_analyzed = false;
    bool m_factored = false;

    // In analyze's memory. Row r's entries are row_columns[row_start[r] .. row_end[r]), first those of columns solved
    // before it, up to row_split[r], then those of inactivated columns; its diagonal, if chosen, follows row_end[r].
    crd::u32* m_row_start = nullptr;
    crd::u32* m_row_split = nullptr;
    crd::u32* m_row_end = nullptr;
    crd::u32* m_row_columns = nullptr;
    crd::u32* m_chosen_row = nullptr;    // the row of each step of the first phase
    crd::u32* m_chosen_column = nullptr; // and its diagonal column
    crd::u32* m_column_state = nullptr;  // for each of L columns: active, the step it was solved at, or its u index
    crd::u32* m_u_column = nullptr;      // the column of each u index
    crd::u32* m_schur_row = nullptr;     // the binary rows left, ascending
    crd::u8* m_hdpc_first = nullptr;     // for j < K'+S-1: the two rows of column j of MT
    crd::u8* m_hdpc_second = nullptr;

    // In analyze's memory, for the first phase only.
    crd::u32* m_column_start = nullptr;
    crd::u32* m_column_rows = nullptr;
    crd::u32* m_row_weight = nullptr;
    crd::u32* m_row_degree = nullptr;
    crd::u32* m_row_xor = nullptr;
    crd::u32* m_row_next = nullptr;
    crd::u32* m_row_prev = nullptr;
    crd::u32* m_bucket_head = nullptr;
    crd::u32* m_set_parent = nullptr;
    crd::u32* m_set_size = nullptr;

    // In factor's memory: S by rows, each of m_u bytes, and the rows in pivot order. For pivot j at row x = pivot[j],
    // S[x][j] is the inverse of the pivot, S[x][k < j] the multipliers of earlier pivots and S[x][k > j] the unit upper
    // triangle.
    crd::u8* m_schur = nullptr;
    crd::u32* m_pivot_row = nullptr;
};

// The FEC payload ID of §3.2. Both false when the span or a field does not fit: a span of other than 4 bytes, an ESI of
// 2^24 or more.
[[nodiscard]] bool raptorq_write_payload_id(crd::u8 sbn, crd::u32 esi, std::span<crd::u8> out) noexcept;
[[nodiscard]] bool raptorq_read_payload_id(std::span<const crd::u8> in, crd::u8& sbn, crd::u32& esi) noexcept;

// The object transmission information of §3.3.
struct RaptorqOti
{
    crd::u64 transfer_length = 0U; // F
    crd::u32 symbol_size = 0U;     // T
    crd::u32 source_blocks = 0U;   // Z
    crd::u32 sub_blocks = 0U;      // N
    crd::u32 alignment = 0U;       // Al
};

// Whether the fields fit their widths and §4.4.1.2's constraints: T a positive multiple of Al, each sub-symbol at
// least Al bytes, every source block at most K'_max symbols.
[[nodiscard]] bool raptorq_oti_valid(const RaptorqOti& oti) noexcept;

// Both false when the span is not 12 bytes or the information is not valid (a reader ignores the reserved octet).
[[nodiscard]] bool raptorq_write_oti(const RaptorqOti& oti, std::span<crd::u8> out) noexcept;
[[nodiscard]] bool raptorq_read_oti(std::span<const crd::u8> in, RaptorqOti& oti) noexcept;

// §4.3: T, Z and N for an object of `transfer_length` octets, from the working memory WS, the alignment Al, the lower
// bound SS on a sub-symbol in units of Al and the payload size P'. False when the inputs are out of range, P' is not a
// multiple of Al, no K' fits WS, or the object would need more than 255 source blocks.
[[nodiscard]] bool raptorq_derive_oti(crd::u64 transfer_length, crd::u64 working_memory, crd::u32 alignment,
                                      crd::u32 sub_symbol_units, crd::u32 payload_size, RaptorqOti& oti) noexcept;

// Partition[I, J] of §4.4.1.2 for J > 0.
struct RaptorqPartition
{
    crd::u64 large = 0U;       // IL
    crd::u64 small = 0U;       // IS
    crd::u64 large_count = 0U; // JL
    crd::u64 small_count = 0U; // JS
};
[[nodiscard]] constexpr RaptorqPartition raptorq_partition(crd::u64 size, crd::u64 parts) noexcept
{
    const crd::u64 small = size / parts;
    const crd::u64 large_count = size - (small * parts);
    return {small + (large_count != 0U ? 1U : 0U), small, large_count, parts - large_count};
}

// One source block of an object: its symbols, its first octet in the object and its octets there (the last block's are
// fewer than k·T when the object ends inside its last symbol).
struct RaptorqBlock
{
    crd::u32 k = 0U;
    crd::u64 offset = 0U;
    crd::u64 bytes = 0U;
};

// Block `sbn` of a valid `oti`: false when sbn >= Z. A block past the object's last symbol has k = 0 and starts at its
// end.
[[nodiscard]] bool raptorq_block(const RaptorqOti& oti, crd::u32 sbn, RaptorqBlock& block) noexcept;

// Source symbol m of a block of k symbols: the concatenation of sub-symbol m of each of the N sub-blocks of its bytes.
// `block` is the block's octets in the object, read as zero past their end when `read`, the padding of §4.4.1.2, and
// dropped when written. symbol.size() must be T and m below k: otherwise false, and nothing is copied.
[[nodiscard]] bool raptorq_read_symbol(const RaptorqOti& oti, crd::u32 k, std::span<const crd::u8> block, crd::u32 m,
                                       std::span<crd::u8> symbol) noexcept;
[[nodiscard]] bool raptorq_write_symbol(const RaptorqOti& oti, crd::u32 k, std::span<crd::u8> block, crd::u32 m,
                                        std::span<const crd::u8> symbol) noexcept;

} // namespace crd::hesap::net
