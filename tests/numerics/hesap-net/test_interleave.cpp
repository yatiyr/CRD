// HESAP-NET's interleavers: the block interleaver's permutation and its inverse for every shape up to 12 x 12, the
// burst bound per row, the convolutional interleaver against its closed form (symbol k leaves at k + (k mod B)·M·B
// and returns delay() later) for every B up to 8 and M up to 4, in place and not, a worked 3-branch example, the
// spread of one commutator turn, and every refusal with nothing written or bound.

#include <crd/containers/array.hpp>
#include <crd/hesap/net/interleave.hpp>

#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <limits>
#include <span>

namespace hn = crd::hesap::net;
using crd::u32;
using crd::u64;
using crd::u8;
using crd::usize;
using hn::ConvolutionalInterleaver;
using Bytes = crd::containers::Array<u8>;
using Role = ConvolutionalInterleaver::Role;

namespace
{

constexpr u8 kSentinel = 0xA5U;
constexpr usize kMaxSymbol = 8U; // the widest symbol a case uses, which bounds the tag loops for GCC's -O3

// Byte i of the symbol naming `index`: the index + 1 little-endian, then the index's low byte repeated.
u8 tag_byte(usize i, u64 index)
{
    return i < 4U ? static_cast<u8>((index + 1U) >> (8U * i)) : static_cast<u8>(index);
}

void tag(u8* symbol, usize bytes, u64 index)
{
    REQUIRE(bytes <= kMaxSymbol);
    for (usize i = 0U; i < bytes && i < kMaxSymbol; ++i)
    {
        symbol[i] = tag_byte(i, index);
    }
}

bool is_tag(const u8* symbol, usize bytes, u64 index)
{
    REQUIRE(bytes <= kMaxSymbol);
    for (usize i = 0U; i < bytes && i < kMaxSymbol; ++i)
    {
        if (symbol[i] != tag_byte(i, index))
        {
            return false;
        }
    }
    return true;
}

bool is_zero(const u8* symbol, usize bytes)
{
    for (usize i = 0U; i < bytes; ++i)
    {
        if (symbol[i] != 0U)
        {
            return false;
        }
    }
    return true;
}

} // namespace

TEST_CASE("Interleave: a 2 x 3 block is sent column by column", "[hesap][interleave]")
{
    const u8 in[] = {0U, 1U, 2U, 3U, 4U, 5U};
    u8 out[6] = {};
    REQUIRE(hn::block_interleave(in, out, 2U, 3U, 1U));
    const u8 sent[] = {0U, 3U, 1U, 4U, 2U, 5U};
    CHECK(std::memcmp(out, sent, sizeof(sent)) == 0);
    u8 back[6] = {};
    REQUIRE(hn::block_deinterleave(out, back, 2U, 3U, 1U));
    CHECK(std::memcmp(back, in, sizeof(in)) == 0);
}

TEST_CASE("Interleave: every block shape up to 12 x 12 is a permutation the deinterleaver inverts",
          "[hesap][interleave]")
{
    for (u32 rows = 1U; rows <= 12U; ++rows)
    {
        for (u32 columns = 1U; columns <= 12U; ++columns)
        {
            const u64 n = u64{rows} * columns;
            bool hit[144] = {};
            for (u64 i = 0U; i < n; ++i)
            {
                const u64 p = hn::block_interleave_position(rows, columns, i);
                REQUIRE(p < n);
                CHECK_FALSE(hit[p]);
                hit[p] = true;
                CHECK(p == ((i % columns) * rows) + (i / columns)); // row i / C, column i % C
                CHECK(hn::block_interleave_index(rows, columns, p) == i);
            }
            for (const usize symbol : {usize{1U}, usize{3U}})
            {
                // Buffers of the largest block, and a byte past it.
                u8 in[(12U * 12U * 3U) + 1U];
                u8 sent[sizeof(in)];
                u8 back[sizeof(in)];
                const usize bytes = n * symbol;
                std::memset(sent, kSentinel, sizeof(sent));
                std::memset(back, kSentinel, sizeof(back));
                for (u64 i = 0U; i < n; ++i)
                {
                    tag(in + (i * symbol), symbol, i);
                }
                REQUIRE(hn::block_interleave(std::span<const u8>(in, bytes), std::span<u8>(sent, bytes), rows, columns,
                                             symbol));
                for (u64 i = 0U; i < n; ++i)
                {
                    CHECK(is_tag(sent + (hn::block_interleave_position(rows, columns, i) * symbol), symbol, i));
                }
                CHECK(sent[bytes] == kSentinel);
                REQUIRE(hn::block_deinterleave(std::span<const u8>(sent, bytes), std::span<u8>(back, bytes), rows,
                                               columns, symbol));
                CHECK(std::memcmp(back, in, bytes) == 0);
            }
        }
    }
}

TEST_CASE("Interleave: a burst of n*R positions costs each row of a block at most n symbols", "[hesap][interleave]")
{
    for (u32 rows = 1U; rows <= 8U; ++rows)
    {
        for (u32 columns = 1U; columns <= 8U; ++columns)
        {
            const u64 n = u64{rows} * columns;
            for (u32 bursts = 1U; bursts <= columns; ++bursts)
            {
                const u64 length = u64{bursts} * rows;
                for (u64 start = 0U; start + length <= n; ++start)
                {
                    u32 lost[8] = {};
                    for (u64 p = start; p < start + length; ++p)
                    {
                        ++lost[hn::block_interleave_index(rows, columns, p) / columns];
                    }
                    for (u32 r = 0U; r < rows; ++r)
                    {
                        CHECK(lost[r] == bursts); // exactly n inside the block
                    }
                }
            }
        }
    }
}

TEST_CASE("Interleave: a malformed or overlapping block is refused and nothing is written", "[hesap][interleave]")
{
    u8 in[12] = {};
    u8 out[12];
    std::memset(out, kSentinel, sizeof(out));
    CHECK_FALSE(hn::block_interleave(in, out, 0U, 3U, 1U));
    CHECK_FALSE(hn::block_interleave(in, out, 3U, 0U, 1U));
    CHECK_FALSE(hn::block_interleave(in, out, 3U, 4U, 0U));
    CHECK_FALSE(hn::block_interleave(in, out, 3U, 3U, 1U));                           // 12 bytes are not 9 symbols
    CHECK_FALSE(hn::block_interleave(std::span<const u8>(in, 11U), out, 2U, 2U, 3U)); // nor 11 bytes 4 of 3
    CHECK_FALSE(hn::block_deinterleave(in, std::span<u8>(out, 11U), 3U, 4U, 1U));     // short output
    CHECK_FALSE(hn::block_deinterleave(in, out, 1U, 1U, 1U));
    for (const u8 b : out)
    {
        CHECK(b == kSentinel);
    }
    // Overlapping buffers: the same bytes, and a shift by one symbol either way.
    u8 shared[16];
    std::memset(shared, kSentinel, sizeof(shared));
    CHECK_FALSE(hn::block_interleave(std::span<const u8>(shared, 12U), std::span<u8>(shared, 12U), 3U, 4U, 1U));
    CHECK_FALSE(hn::block_interleave(std::span<const u8>(shared, 12U), std::span<u8>(shared + 1, 12U), 3U, 4U, 1U));
    CHECK_FALSE(hn::block_deinterleave(std::span<const u8>(shared + 1, 12U), std::span<u8>(shared, 12U), 3U, 4U, 1U));
    for (const u8 b : shared)
    {
        CHECK(b == kSentinel);
    }
}

TEST_CASE("Interleave: three branches of depth 1 delay their symbols 0, 3 and 6 places", "[hesap][interleave]")
{
    ConvolutionalInterleaver tx;
    ConvolutionalInterleaver rx;
    REQUIRE(ConvolutionalInterleaver::storage_bytes(3U, 1U, 1U) == 3U);
    u8 tx_store[3];
    u8 rx_store[3];
    std::memset(tx_store, kSentinel, sizeof(tx_store));
    REQUIRE(tx.init(Role::Interleave, 3U, 1U, 1U, tx_store));
    REQUIRE(rx.init(Role::Deinterleave, 3U, 1U, 1U, rx_store));
    CHECK(ConvolutionalInterleaver::delay(3U, 1U) == 6U);
    const u8 sent[] = {1U, 0U, 0U, 4U, 2U, 0U, 7U, 5U, 3U, 10U, 8U, 6U, 13U, 11U, 9U};
    const u8 received[] = {0U, 0U, 0U, 0U, 0U, 0U, 1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U, 9U};
    for (u32 t = 0U; t < 15U; ++t)
    {
        const u8 in = static_cast<u8>(t + 1U);
        u8 wire = 0xEEU;
        u8 out = 0xEEU;
        tx.push(std::span<const u8>(&in, 1U), std::span<u8>(&wire, 1U));
        rx.push(std::span<const u8>(&wire, 1U), std::span<u8>(&out, 1U));
        CHECK(wire == sent[t]);
        CHECK(out == received[t]);
    }
}

TEST_CASE("Interleave: every convolutional interleaver up to 8 branches and depth 4 meets its closed form",
          "[hesap][interleave]")
{
    for (u32 branches = 1U; branches <= 8U; ++branches)
    {
        for (u32 depth = 1U; depth <= 4U; ++depth)
        {
            for (const usize symbol : {usize{4U}, usize{7U}})
            {
                for (const bool in_place : {false, true})
                {
                    INFO("B " << branches << " M " << depth << " symbol " << symbol << " in place " << in_place);
                    const usize bytes = ConvolutionalInterleaver::storage_bytes(branches, depth, symbol);
                    REQUIRE(bytes == (u64{branches} * (branches - 1U) / 2U) * depth * symbol);
                    Bytes tx_store;
                    Bytes rx_store;
                    tx_store.resize(bytes, kSentinel);
                    rx_store.resize(bytes, kSentinel);
                    ConvolutionalInterleaver tx;
                    ConvolutionalInterleaver rx;
                    REQUIRE(tx.init(Role::Interleave, branches, depth, symbol,
                                    std::span<u8>(tx_store.data(), tx_store.size())));
                    REQUIRE(rx.init(Role::Deinterleave, branches, depth, symbol,
                                    std::span<u8>(rx_store.data(), rx_store.size())));
                    const u64 delay = ConvolutionalInterleaver::delay(branches, depth);
                    const u64 total = delay + (3U * branches) + 5U;
                    Bytes wire;
                    wire.resize(total * symbol);
                    for (u64 t = 0U; t < total; ++t)
                    {
                        u8 in[kMaxSymbol];
                        u8 sent[kMaxSymbol];
                        u8 out[kMaxSymbol];
                        tag(in, symbol, t);
                        u8* const sent_at = in_place ? in : sent;
                        tx.push(std::span<const u8>(in, symbol), std::span<u8>(sent_at, symbol));
                        std::memcpy(wire.data() + (t * symbol), sent_at, symbol);
                        u8* const out_at = in_place ? sent_at : out;
                        rx.push(std::span<const u8>(sent_at, symbol), std::span<u8>(out_at, symbol));
                        if (t < delay)
                        {
                            CHECK(is_zero(out_at, symbol));
                        }
                        else
                        {
                            CHECK(is_tag(out_at, symbol, t - delay));
                        }
                    }
                    // Symbol k left at k + (k mod B)·M·B; a place no symbol reached by then holds zeros.
                    bool filled[256] = {};
                    for (u64 k = 0U; k < total; ++k)
                    {
                        const u64 leaves = k + ((k % branches) * depth * branches);
                        if (leaves < total)
                        {
                            CHECK(is_tag(wire.data() + (leaves * symbol), symbol, k));
                            filled[leaves] = true;
                        }
                    }
                    for (u64 t = 0U; t < total; ++t)
                    {
                        if (!filled[t])
                        {
                            CHECK(is_zero(wire.data() + (t * symbol), symbol));
                        }
                    }
                    // The B symbols of one turn leave M·B + 1 apart.
                    for (u64 b = 1U; b < branches; ++b)
                    {
                        const u64 gap = (b + (b * depth * branches)) - ((b - 1U) + ((b - 1U) * depth * branches));
                        CHECK(gap == (u64{depth} * branches) + 1U);
                    }
                }
            }
        }
    }
}

TEST_CASE("Interleave: a convolutional interleaver outside its range binds nothing", "[hesap][interleave]")
{
    u8 store[64];
    ConvolutionalInterleaver x;
    CHECK_FALSE(x.init(Role::Interleave, 0U, 1U, 1U, {}));
    CHECK_FALSE(x.init(Role::Interleave, 2U, 0U, 1U, {}));
    CHECK_FALSE(x.init(Role::Interleave, 1U, 0U, 1U, {})); // one branch still needs a depth
    CHECK_FALSE(x.init(Role::Interleave, 2U, 1U, 0U, {}));
    CHECK_FALSE(x.init(Role::Interleave, ConvolutionalInterleaver::kMaxBranches + 1U, 1U, 1U, {}));
    CHECK_FALSE(x.init(Role::Deinterleave, 2U, ConvolutionalInterleaver::kMaxDepth + 1U, 1U, {}));
    std::memset(store, kSentinel, sizeof(store));
    CHECK_FALSE(x.init(Role::Interleave, 3U, 2U, 2U, std::span<u8>(store, 11U))); // 12 bytes needed
    CHECK_FALSE(x.init(Role::Interleave, 3U, 2U, 2U, std::span<u8>(store, 13U)));
    for (const u8 b : store)
    {
        CHECK(b == kSentinel); // a refused init clears nothing
    }
    CHECK(ConvolutionalInterleaver::storage_bytes(3U, 2U, 2U) == 12U);
    CHECK(ConvolutionalInterleaver::storage_bytes(0U, 1U, 1U) == 0U);
    CHECK(ConvolutionalInterleaver::storage_bytes(ConvolutionalInterleaver::kMaxBranches,
                                                  ConvolutionalInterleaver::kMaxDepth,
                                                  std::numeric_limits<usize>::max()) == 0U); // past usize

    // One branch is the identity and stores nothing.
    REQUIRE(x.init(Role::Interleave, 1U, 5U, 2U, {}));
    const u8 in[2] = {7U, 8U};
    u8 out[2] = {};
    x.push(in, out);
    CHECK((out[0] == 7U && out[1] == 8U));
}
