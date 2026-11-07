// crd-net -- generated wire formats (NET.3; docs/design/network-foundation.md#wire-formats): the known answers of the
// generator's reference interpreter, then what a reference cannot say: which refusal a damaged message gets, zero-copy
// views, the revision rules across old and new writers, the budgets a schema publishes and the S18 artefact it fills.

#include <crd/crypto/capability.hpp>
#include <crd/hesap/net/quantize.hpp>
#include <crd/net/wire/session_control.hpp>

#include <generated/wire_test.hpp>
#include <wire_vector_checks.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstring>

using crd::f32;
using crd::f64;
using crd::i16;
using crd::i32;
using crd::i64;
using crd::i8;
using crd::u16;
using crd::u32;
using crd::u64;
using crd::u8;
using crd::usize;
using crd::containers::BitReader;
using crd::containers::BitWriter;
using crd::containers::ConstSpan;
using crd::containers::Span;
using crd::net::wire::decode;
using crd::net::wire::encode;
using crd::net::wire::encoded_bytes;
using crd::net::wire::kWireMinI64;
using crd::net::wire::same;
using crd::net::wire::WireError;
using crd::net::wire::WireList;
using crd::net::wire::WireQuaternion;
using crd::net::wire::testing::check_value;
using crd::net::wire::testing::check_verdict;

using namespace crd::net::wire::wire_test;
namespace control = crd::net::wire::session_control;
namespace cont    = crd::containers;

namespace
{
template <typename M>
Span<const u8> encoded(const M& message, u16 minor, u8* storage, usize capacity)
{
    usize size = 0U;
    REQUIRE(encode(message, minor, Span<u8>(storage, capacity), size) == WireError::Ok);
    return Span<const u8>(storage, size);
}
} // namespace

TEST_CASE("Generated code holds to the reference interpreter's bytes and verdicts", "[net][wire]")
{
#include <generated/wire_test_vectors.inc>
}

TEST_CASE("A message's bits and padding are the declared ones", "[net][wire]")
{
    Newer message{};
    message.only = 0xABCU;
    u8 out[8] = {};
    CHECK(encoded(message, 1U, out, sizeof(out)).size() == 2U);
    CHECK(out[0] == 0xABU);
    CHECK(out[1] == 0xC0U);

    Newer decoded{};
    const u8 good[2] = {0xABU, 0xC0U};
    CHECK(decode(ConstSpan<u8>(good), 2U, decoded) == WireError::Ok);
    CHECK(decoded.only == 0xABCU);

    SECTION("a set padding bit")
    {
        const u8 padded[2] = {0xABU, 0xC1U};
        CHECK(decode(ConstSpan<u8>(padded), kMinor, decoded) == WireError::BadPadding);
    }
    SECTION("a byte more")
    {
        const u8 longer[3] = {0xABU, 0xC0U, 0x00U};
        CHECK(decode(ConstSpan<u8>(longer), kMinor, decoded) == WireError::TrailingBits);
    }
    SECTION("cut short")
    {
        CHECK(decode(ConstSpan<u8>(good, 1U), kMinor, decoded) == WireError::Truncated);
        CHECK(decode(ConstSpan<u8>(), kMinor, decoded) == WireError::Truncated);
    }
    SECTION("a newer writer may hold anything more")
    {
        const u8 longer[4] = {0xABU, 0xC5U, 0xFFU, 0x01U};
        REQUIRE(decode(ConstSpan<u8>(longer), kMinor + 1U, decoded) == WireError::Ok);
        CHECK(decoded.only == 0xABCU);
    }
}

TEST_CASE("Every refusal names what is wrong", "[net][wire]")
{
    u8 out[256] = {};
    SECTION("a range's unused codes")
    {
        Vec vec{};
        // y is 1000 to 1099 in 7 bits, and codes 100 to 127 are no value.
        BitWriter writer{Span<u8>(out)};
        writer.put(0U, 10U);
        writer.put(100U, 7U);
        writer.put(0U, 6U);
        writer.finish();
        BitReader reader{ConstSpan<u8>(out, 3U)};
        CHECK(VecCodec::decode(reader, kMinor, vec) == WireError::OutOfRange);
    }
    SECTION("an encoder holds a value to its bound")
    {
        Lists lists{};
        const Node nodes[1] = {};
        lists.nodes = WireList<NodeCodec>(ConstSpan<Node>(nodes));
        const u8 small[1] = {8U};
        lists.small = WireList<ListsSmallItemCodec>(ConstSpan<u8>(small));
        const u8 exact[2] = {0U, 0U};
        lists.exact = WireList<ListsExactItemCodec>(ConstSpan<u8>(exact));
        usize written = 7U;
        CHECK(encode(lists, kMinor, Span<u8>(out), written) == WireError::OutOfRange);
        CHECK(written == 0U);

        Scalars scalars{};
        scalars.fourteen = 0x4000U;
        CHECK(encode(scalars, kMinor, Span<u8>(out), written) == WireError::OutOfRange);
        scalars.fourteen = 0x3FFFU;
        scalars.ranged   = 4;
        CHECK(encode(scalars, kMinor, Span<u8>(out), written) == WireError::OutOfRange);
        scalars.ranged = -4;
        CHECK(encode(scalars, kMinor, Span<u8>(out), written) == WireError::OutOfRange);
        scalars.ranged = 3;
        scalars.count  = 100001U;
        CHECK(encode(scalars, kMinor, Span<u8>(out), written) == WireError::OutOfRange);
        scalars.count = 100000U;
        CHECK(encode(scalars, kMinor, Span<u8>(out), written) == WireError::Ok);
    }
    SECTION("list and byte bounds on encode")
    {
        const u8 blob[41] = {};
        Blobs    blobs{};
        blobs.fixed = ConstSpan<u8>(blob, 4U);
        blobs.ranged = ConstSpan<u8>(blob, 2U);
        usize written = 0U;
        CHECK(encode(blobs, kMinor, Span<u8>(out), written) == WireError::OutOfRange);
        blobs.fixed  = ConstSpan<u8>(blob, 5U);
        blobs.ranged = ConstSpan<u8>(blob, 1U);
        CHECK(encode(blobs, kMinor, Span<u8>(out), written) == WireError::OutOfRange);
        blobs.ranged = ConstSpan<u8>(blob, 41U);
        CHECK(encode(blobs, kMinor, Span<u8>(out), written) == WireError::OutOfRange);
        blobs.ranged = ConstSpan<u8>(blob, 40U);
        CHECK(encode(blobs, kMinor, Span<u8>(out), written) == WireError::Ok);
        blobs.title = std::string_view("\xC0\x80", 2U);
        CHECK(encode(blobs, kMinor, Span<u8>(out), written) == WireError::BadText);
        const char long_title[62] = {};
        blobs.title = std::string_view(long_title, 61U);
        CHECK(encode(blobs, kMinor, Span<u8>(out), written) == WireError::OutOfRange);
    }
    SECTION("a list of a count below its minimum")
    {
        Lists lists{};
        usize written = 0U;
        CHECK(encode(lists, kMinor, Span<u8>(out), written) == WireError::OutOfRange);
    }
    SECTION("a non-canonical variable-length integer")
    {
        // group 3: 0b1'000 then 0b0'000 is zero spelled in two groups.
        BitWriter writer{Span<u8>(out)};
        writer.put(0b1000U, 4U);
        writer.put(0b0000U, 4U);
        writer.finish();
        BitReader reader{ConstSpan<u8>(out, 1U)};
        u64 value = 0U;
        CHECK(crd::net::wire::wire_take_varuint(reader, 3U, 100000U, value) == WireError::Overlong);
    }
    SECTION("a variable-length integer past its groups or its bound")
    {
        u8 ones[16];
        std::memset(ones, 0xFFU, sizeof(ones));
        BitReader reader{ConstSpan<u8>(ones)};
        u64 value = 0U;
        CHECK(crd::net::wire::wire_take_varuint(reader, 7U, 300U, value) == WireError::OutOfRange);
        const u8 above[2] = {0x80U | 0x2DU, 0x03U}; // 45 + 3 * 128 = 429
        BitReader second{ConstSpan<u8>(above)};
        CHECK(crd::net::wire::wire_take_varuint(second, 7U, 300U, value) == WireError::OutOfRange);
        BitReader third{ConstSpan<u8>(above)};
        CHECK(crd::net::wire::wire_take_varuint(third, 7U, 429U, value) == WireError::Ok);
        CHECK(value == 429U);
        // Zero groups never shift past the value's width, however many a message claims.
        u8 zeros[12];
        std::memset(zeros, 0x80U, sizeof(zeros));
        zeros[11] = 0x01U;
        BitReader endless{ConstSpan<u8>(zeros)};
        CHECK(crd::net::wire::wire_take_varuint(endless, 7U, ~u64{0}, value) == WireError::OutOfRange);
        // The tenth group of a 64-bit integer holds one bit.
        u8 wide[10];
        std::memset(wide, 0xFFU, sizeof(wide));
        wide[9] = 0x02U;
        BitReader fourth{ConstSpan<u8>(wide)};
        CHECK(crd::net::wire::wire_take_varuint(fourth, 7U, ~u64{0}, value) == WireError::OutOfRange);
        wide[9] = 0x01U;
        BitReader fifth{ConstSpan<u8>(wide)};
        CHECK(crd::net::wire::wire_take_varuint(fifth, 7U, ~u64{0}, value) == WireError::Ok);
        CHECK(value == ~u64{0});
    }
    SECTION("a signed one outside its bound")
    {
        BitWriter writer{Span<u8>(out)};
        crd::net::wire::wire_put_varuint(writer, crd::net::wire::wire_zigzag(100001), 5U);
        const usize size = writer.finish();
        BitReader reader{ConstSpan<u8>(out, size)};
        i64 value = 0;
        CHECK(crd::net::wire::wire_take_varsint(reader, 5U, -100000, 100000, value) == WireError::OutOfRange);
    }
    SECTION("a signed one inside its zigzag but outside its bound")
    {
        BitWriter writer{Span<u8>(out)};
        crd::net::wire::wire_put_varuint(writer, crd::net::wire::wire_zigzag(-100), 3U);
        const usize size = writer.finish();
        BitReader reader{ConstSpan<u8>(out, size)};
        i64 value = 0;
        CHECK(crd::net::wire::wire_take_varsint(reader, 3U, -5, 100, value) == WireError::OutOfRange);
        BitReader inside{ConstSpan<u8>(out, size)};
        CHECK(crd::net::wire::wire_take_varsint(inside, 3U, -100, 100, value) == WireError::Ok);
        CHECK(value == -100);
    }
    SECTION("an encoder holds a signed one to its bound")
    {
        Vec vec{};
        vec.z = 100001;
        BitWriter writer{Span<u8>(out)};
        CHECK(VecCodec::encode(vec, writer, kMinor) == WireError::OutOfRange);
        vec.z = -100001;
        CHECK(VecCodec::encode(vec, writer, kMinor) == WireError::OutOfRange);
        vec.z = 100000;
        CHECK(VecCodec::encode(vec, writer, kMinor) == WireError::Ok);
    }
    SECTION("a float that is not finite, unless it says so")
    {
        Scalars scalars{};
        scalars.ratio = std::bit_cast<f32>(u32{0x7FC00000U});
        usize written = 0U;
        CHECK(encode(scalars, kMinor, Span<u8>(out), written) == WireError::NonFinite);
        scalars.ratio   = 0.0F;
        scalars.precise = std::bit_cast<f64>(u64{0x7FF0000000000000ULL});
        CHECK(encode(scalars, kMinor, Span<u8>(out), written) == WireError::NonFinite);
        scalars.precise       = 0.0;
        scalars.anything      = std::bit_cast<f32>(u32{0xFF800000U});
        scalars.anything_wide = std::bit_cast<f64>(u64{0x7FF8000000000001ULL});
        REQUIRE(encode(scalars, kMinor, Span<u8>(out), written) == WireError::Ok);
        Scalars decoded{};
        REQUIRE(decode(ConstSpan<u8>(out, written), kMinor, decoded) == WireError::Ok);
        CHECK(same(decoded, scalars));
        const u8 nan[4] = {0x7FU, 0xC0U, 0U, 0U};
        BitReader near{ConstSpan<u8>(nan)};
        f32 value = 0.0F;
        CHECK(crd::net::wire::wire_take_float(near, true, value) == WireError::NonFinite);
    }
    SECTION("text that is not UTF-8")
    {
        const u8 bad[3] = {0x41U, 0xC3U, 0xA9U}; // a length of 2 in 3 bits, then a set padding bit
        BitReader reader{ConstSpan<u8>(bad)};
        std::string_view text;
        CHECK(crd::net::wire::wire_take_text(reader, 60U, 3U, text) == WireError::BadPadding);
        const u8 clean[3] = {0x40U, 0xC0U, 0x80U};
        BitReader second{ConstSpan<u8>(clean)};
        CHECK(crd::net::wire::wire_take_text(second, 60U, 3U, text) == WireError::BadText);
        const u8 fine[3] = {0x40U, 0xC3U, 0xA9U};
        BitReader third{ConstSpan<u8>(fine)};
        CHECK(crd::net::wire::wire_take_text(third, 60U, 3U, text) == WireError::Ok);
        CHECK(text == "\xC3\xA9");
    }
    SECTION("a length above its bound or past the input")
    {
        const u8 six[2] = {0xC0U, 0x01U};
        ConstSpan<u8> bytes;
        BitReader reader{ConstSpan<u8>(six)};
        CHECK(crd::net::wire::wire_take_bytes(reader, 0U, 5U, 3U, bytes) == WireError::OutOfRange);
        const u8 two[2] = {0x40U, 0x01U};
        BitReader second{ConstSpan<u8>(two)};
        CHECK(crd::net::wire::wire_take_bytes(second, 0U, 5U, 3U, bytes) == WireError::Truncated);
    }
}

TEST_CASE("A claimed count is bounded and past the bytes it is refused", "[net][wire]")
{
    // Lists::small holds up to 9 elements of 3 bits: its count takes 4 bits.
    const u8 bytes[1] = {0x90U};
    Lists    lists{};
    CHECK(decode(ConstSpan<u8>(bytes), kMinor, lists) == WireError::Truncated);
    u8 more[2] = {0xF0U, 0x00U};
    CHECK(decode(ConstSpan<u8>(more), kMinor, lists) == WireError::OutOfRange);
}

TEST_CASE("Decoded fields view the input", "[net][wire]")
{
    const u8 fixed[5]  = {1U, 2U, 3U, 4U, 5U};
    const u8 ranged[2] = {6U, 7U};
    Blobs    blobs{};
    blobs.before = 5U;
    blobs.fixed  = ConstSpan<u8>(fixed);
    blobs.ranged = ConstSpan<u8>(ranged);
    blobs.after  = 17U;
    blobs.title  = "ok";
    u8 out[128] = {};
    const auto wire = encoded(blobs, kMinor, out, sizeof(out));

    Blobs decoded{};
    REQUIRE(decode(wire, kMinor, decoded) == WireError::Ok);
    CHECK(decoded.fixed.data() >= out);
    CHECK(decoded.fixed.data() + decoded.fixed.size() <= out + wire.size());
    CHECK(decoded.ranged.data() >= out);
    CHECK(decoded.ranged.data() + decoded.ranged.size() <= out + wire.size());
    CHECK(reinterpret_cast<const u8*>(decoded.title.data()) >= out);
    CHECK(reinterpret_cast<const u8*>(decoded.title.data()) + 2U <= out + wire.size());
    CHECK(decoded.title == "ok");
    CHECK_FALSE(decoded.has_maybe);
    CHECK_FALSE(decoded.has_maybe_title);
    CHECK(decoded.before == 5U);
    CHECK(decoded.after == 17U);
}

TEST_CASE("A decoded list re-reads its elements from the message", "[net][wire]")
{
    const u8   small[3] = {1U, 2U, 7U};
    const u8   exact[2] = {4U, 0U};
    const u8   tag[3]   = {9U, 8U, 7U};
    Node       nodes[2] = {};
    nodes[0].at.x = -512;
    nodes[0].at.y = 1099;
    nodes[0].at.z = -100000;
    nodes[0].tag  = ConstSpan<u8>(tag);
    nodes[1].at.x = 511;
    nodes[1].tag  = ConstSpan<u8>(tag);
    nodes[1].has_extended  = true;
    nodes[1].extended.base = 300U;
    nodes[1].extended.label = "name";
    nodes[1].extended.added = 100U;

    Lists lists{};
    lists.small = WireList<ListsSmallItemCodec>(ConstSpan<u8>(small));
    lists.exact = WireList<ListsExactItemCodec>(ConstSpan<u8>(exact));
    lists.nodes = WireList<NodeCodec>(ConstSpan<Node>(nodes));

    u8 out[256] = {};
    const auto wire = encoded(lists, kMinor, out, sizeof(out));
    CHECK(encoded_bytes(lists, kMinor) == wire.size());

    Lists decoded{};
    REQUIRE(decode(wire, kMinor, decoded) == WireError::Ok);
    REQUIRE(decoded.small.size() == 3U);
    auto small_cursor = decoded.small.cursor();
    u8   value        = 0U;
    REQUIRE(small_cursor.next(value));
    CHECK(value == 1U);
    REQUIRE(small_cursor.next(value));
    CHECK(value == 2U);
    REQUIRE(small_cursor.next(value));
    CHECK(value == 7U);
    CHECK_FALSE(small_cursor.next(value));

    REQUIRE(decoded.nodes.size() == 2U);
    auto node_cursor = decoded.nodes.cursor();
    Node node{};
    REQUIRE(node_cursor.next(node));
    CHECK(node.at.x == -512);
    CHECK(node.at.y == 1099);
    CHECK(node.at.z == -100000);
    CHECK_FALSE(node.has_extended);
    REQUIRE(node_cursor.next(node));
    CHECK(node.at.x == 511);
    REQUIRE(node.has_extended);
    CHECK(node.extended.base == 300U);
    CHECK(node.extended.label == "name");
    CHECK(node.extended.added == 100U);
    CHECK_FALSE(node_cursor.next(node));

    // A second walk starts over.
    auto again = decoded.nodes.cursor();
    REQUIRE(again.next(node));
    CHECK(node.at.x == -512);

    SECTION("a changed element differs")
    {
        nodes[1].extended.base = 301U;
        CHECK_FALSE(same(lists, decoded));
    }
    SECTION("the same elements compare equal in either form")
    {
        CHECK(same(lists, decoded));
    }
}

TEST_CASE("A list given storage decodes each element once into it", "[net][wire]")
{
    const u8 small[3] = {1U, 2U, 7U};
    const u8 exact[2] = {4U, 0U};
    const u8 tag[3]   = {9U, 8U, 7U};
    Node     nodes[2] = {};
    nodes[0].at.x = -512;
    nodes[0].tag  = ConstSpan<u8>(tag);
    nodes[1].at.x = 511;
    nodes[1].tag  = ConstSpan<u8>(tag);
    nodes[1].has_extended  = true;
    nodes[1].extended.base = 300U;
    nodes[1].extended.label = "name";
    Lists lists{};
    lists.small = WireList<ListsSmallItemCodec>(ConstSpan<u8>(small));
    lists.exact = WireList<ListsExactItemCodec>(ConstSpan<u8>(exact));
    lists.nodes = WireList<NodeCodec>(ConstSpan<Node>(nodes));
    u8         out[256] = {};
    const auto wire     = encoded(lists, kMinor, out, sizeof(out));

    Node  node_storage[6] = {};
    u8    small_storage[9] = {};
    Lists decoded{};
    decoded.nodes.into(Span<Node>(node_storage));
    decoded.small.into(Span<u8>(small_storage));
    REQUIRE(decode(wire, kMinor, decoded) == WireError::Ok);
    CHECK_FALSE(decoded.nodes.viewed());
    REQUIRE(decoded.nodes.items().size() == 2U);
    CHECK(decoded.nodes.items().data() == node_storage);
    CHECK(node_storage[0].at.x == -512);
    CHECK(node_storage[1].extended.base == 300U);
    CHECK(node_storage[1].extended.label == "name");
    REQUIRE(decoded.small.items().size() == 3U);
    CHECK(small_storage[2] == 7U);
    CHECK(decoded.exact.viewed()); // no storage: the elements stay in the message
    CHECK(same(lists, decoded));

    SECTION("the storage stays with the list, and the next decode refills it")
    {
        lists.nodes = WireList<NodeCodec>(ConstSpan<Node>(nodes, 1U));
        const auto shorter = encoded(lists, kMinor, out, sizeof(out));
        REQUIRE(decode(shorter, kMinor, decoded) == WireError::Ok);
        CHECK(decoded.nodes.items().size() == 1U);
        CHECK(decoded.nodes.storage().data() == node_storage);
        CHECK(same(lists, decoded));
    }
    SECTION("more elements than the storage holds are viewed, and decode the same")
    {
        Node  one[1] = {};
        Lists viewed{};
        viewed.nodes.into(Span<Node>(one));
        REQUIRE(decode(wire, kMinor, viewed) == WireError::Ok);
        CHECK(viewed.nodes.viewed());
        CHECK(viewed.nodes.items().empty());
        CHECK(viewed.nodes.storage().data() == one);
        CHECK(same(lists, viewed));
    }
    SECTION("every cut and every flipped bit gets the verdict it gets without storage")
    {
        u8 damaged[256] = {};
        for (usize bit = 0U; bit <= wire.size() * 8U; ++bit)
        {
            std::memcpy(damaged, wire.data(), wire.size());
            const bool cut  = bit == wire.size() * 8U;
            const auto size = cut ? wire.size() - 1U : wire.size();
            if (!cut)
            {
                damaged[bit / 8U] = static_cast<u8>(damaged[bit / 8U] ^ (1U << (bit % 8U)));
            }
            Lists plain{};
            const WireError verdict = decode(ConstSpan<u8>(damaged, size), kMinor, plain);
            if (decode(ConstSpan<u8>(damaged, size), kMinor, decoded) != verdict ||
                (verdict == WireError::Ok && !same(plain, decoded)))
            {
                FAIL("bit " << bit << " decodes otherwise into storage");
            }
        }
    }
}

TEST_CASE("A struct field's list keeps the storage its message gave it", "[net][wire]")
{
    const u8 books[3] = {9U, 0U, 4U};
    Shelved  shelved{};
    shelved.shelf.label = 5U;
    shelved.shelf.books = WireList<ShelfBooksItemCodec>(ConstSpan<u8>(books));
    u8         out[64] = {};
    const auto wire    = encoded(shelved, kMinor, out, sizeof(out));

    u8      shelf_storage[4] = {};
    u8      spare_storage[4] = {};
    Shelved decoded{};
    decoded.shelf.books.into(Span<u8>(shelf_storage));
    decoded.spare.books.into(Span<u8>(spare_storage));
    REQUIRE(decode(wire, kMinor, decoded) == WireError::Ok);
    CHECK(decoded.shelf.label == 5U);
    REQUIRE(decoded.shelf.books.items().size() == 3U);
    CHECK(shelf_storage[0] == 9U);
    CHECK(shelf_storage[2] == 4U);
    CHECK_FALSE(decoded.has_spare);
    CHECK(decoded.spare.books.storage().data() == spare_storage);
    CHECK(same(shelved, decoded));

    // With the spare present, its list decodes into its own storage.
    const u8 more[1] = {6U};
    shelved.has_spare = true;
    shelved.spare.books = WireList<ShelfBooksItemCodec>(ConstSpan<u8>(more));
    const auto both = encoded(shelved, kMinor, out, sizeof(out));
    REQUIRE(decode(both, kMinor, decoded) == WireError::Ok);
    REQUIRE(decoded.has_spare);
    REQUIRE(decoded.spare.books.items().size() == 1U);
    CHECK(spare_storage[0] == 6U);
    CHECK(decoded.shelf.books.items().data() == shelf_storage);
    CHECK(same(shelved, decoded));
}

TEST_CASE("Older and newer writers read as the schema's revisions say", "[net][wire]")
{
    u8 out[256] = {};

    Revisions message{};
    message.first          = 0x123U;
    message.extended.base  = 7U;
    message.extended.label = "x";
    CHECK(message.note == 9U);
    CHECK(message.named == Mode::Auto);
    CHECK(message.extended.added == 3U);

    SECTION("a field the revision lacks takes its default on both sides")
    {
        const auto zero = encoded(message, 0U, out, sizeof(out));
        Revisions  decoded{};
        REQUIRE(decode(zero, 0U, decoded) == WireError::Ok);
        CHECK(same(decoded, message));
        CHECK(decoded.note == 9U);
        CHECK(decoded.named == Mode::Auto);
        CHECK(decoded.count == 0U);
        CHECK(decoded.extended.added == 3U);
        CHECK(decoded.items.empty());
        CHECK_FALSE(decoded.has_node);
    }
    SECTION("a set field the revision lacks is refused, never dropped")
    {
        usize written = 0U;
        message.note = 10U;
        CHECK(encode(message, 0U, Span<u8>(out), written) == WireError::NotInRevision);
        message.note  = 9U;
        message.count = 1U;
        CHECK(encode(message, 0U, Span<u8>(out), written) == WireError::NotInRevision);
        message.count = 0U;
        message.named = Mode::On;
        CHECK(encode(message, 0U, Span<u8>(out), written) == WireError::NotInRevision);
        message.named = Mode::Auto;
        const u8 tail[1] = {1U};
        message.tail = ConstSpan<u8>(tail);
        CHECK(encode(message, 1U, Span<u8>(out), written) == WireError::NotInRevision);
        message.tail = ConstSpan<u8>();
        message.has_node = true;
        CHECK(encode(message, 1U, Span<u8>(out), written) == WireError::NotInRevision);
        message.has_node = false;
        const bool items[1] = {true};
        message.items = WireList<RevisionsItemsItemCodec>(ConstSpan<bool>(items));
        CHECK(encode(message, 1U, Span<u8>(out), written) == WireError::NotInRevision);
        message.items = {};
        message.extended.added = 4U;
        CHECK(encode(message, 0U, Span<u8>(out), written) == WireError::NotInRevision);
        CHECK(encode(message, 1U, Span<u8>(out), written) == WireError::Ok);
    }
    SECTION("an enum value is its revision's")
    {
        usize written = 0U;
        message.named = Mode::Later;
        CHECK(encode(message, 1U, Span<u8>(out), written) == WireError::Ok);
        Revisions decoded{};
        REQUIRE(decode(ConstSpan<u8>(out, written), 1U, decoded) == WireError::Ok);
        CHECK(decoded.named == Mode::Later);

        Scalars scalars{};
        scalars.mode = Mode::Later;
        CHECK(encode(scalars, 0U, Span<u8>(out), written) == WireError::NotInRevision);
        REQUIRE(encode(scalars, 1U, Span<u8>(out), written) == WireError::Ok);
        Scalars read{};
        CHECK(decode(ConstSpan<u8>(out, written), 0U, read) == WireError::UnknownValue);
        CHECK(decode(ConstSpan<u8>(out, written), 1U, read) == WireError::Ok);
        scalars.mode = static_cast<Mode>(7U);
        CHECK(encode(scalars, kMinor, Span<u8>(out), written) == WireError::UnknownValue);
    }
    SECTION("an enum value a newer writer added reads through as a number")
    {
        BitWriter writer{Span<u8>(out)};
        writer.put(7U, 3U);
        writer.finish();
        BitReader reader{ConstSpan<u8>(out, 1U)};
        Mode      mode = Mode::Off;
        CHECK(crd::net::wire::wire_take_uint(reader, 3U, mode) == WireError::Ok);
        CHECK_FALSE(enum_known(mode, kMinor));
    }
    SECTION("a reader skips what a newer writer appended to an extensible struct")
    {
        // The struct at revision 2 and four more bytes inside it, as a revision 3 writer would send.
        Extended extended{};
        extended.base  = 511U;
        extended.label = "ab";
        extended.more  = {};
        u8        body[32] = {};
        BitWriter inner{Span<u8>(body)};
        REQUIRE(ExtendedCodec::encode_body(extended, inner, kMinor) == WireError::Ok);
        const usize body_size = inner.finish();
        BitWriter   writer{Span<u8>(out)};
        crd::net::wire::wire_put_varuint(writer, body_size + 4U, 7U);
        writer.put_bytes(ConstSpan<u8>(body, body_size));
        const u8 extra[4] = {0xDEU, 0xADU, 0xBEU, 0xEFU};
        writer.put_bytes(ConstSpan<u8>(extra));
        writer.put(0x5AU, 8U);
        const usize total = writer.finish();

        Extended newer{};
        BitReader reader{ConstSpan<u8>(out, total)};
        REQUIRE(ExtendedCodec::decode(reader, kMinor + 1U, newer) == WireError::Ok);
        CHECK(newer.base == 511U);
        CHECK(newer.label == "ab");
        u64 after = 0U;
        REQUIRE(reader.take(8U, after));
        CHECK(after == 0x5AU);

        // At a revision this reader knows, the same bytes are trailing bytes.
        BitReader strict{ConstSpan<u8>(out, total)};
        CHECK(ExtendedCodec::decode(strict, kMinor, newer) == WireError::TrailingBits);
    }
    SECTION("no writer writes a revision newer than its schema")
    {
        usize written = 0U;
        CHECK(encode(message, kMinor + 1U, Span<u8>(out), written) == WireError::BadRevision);
    }
    SECTION("a decode leaves nothing of what the message held before")
    {
        message.note     = 1U;
        message.count    = 7U;
        message.has_node = true;
        const u8 tag[3]  = {1U, 2U, 3U};
        message.node.tag = ConstSpan<u8>(tag);
        const auto full  = encoded(message, kMinor, out, sizeof(out));
        Revisions  decoded{};
        REQUIRE(decode(full, kMinor, decoded) == WireError::Ok);
        CHECK(decoded.note == 1U);
        CHECK(decoded.has_node);

        message.note     = 9U;
        message.count    = 0U;
        message.has_node = false;
        u8         other[256] = {};
        const auto zero       = encoded(message, 0U, other, sizeof(other));
        REQUIRE(decode(zero, 0U, decoded) == WireError::Ok);
        CHECK(decoded.note == 9U);
        CHECK(decoded.count == 0U);
        CHECK_FALSE(decoded.has_node);
        CHECK(same(decoded, message));
    }
    SECTION("an old writer's bytes stay as they were")
    {
        // Minor 0's layout: first in 11 bits; the extensible struct aligned, its length, base in 9 bits, the label's
        // length in 5 bits, padding, the label.
        const u8   frozen[] = {0x24U, 0x60U, 0x03U, 0x03U, 0x84U, 0x78U};
        const auto zero     = encoded(message, 0U, out, sizeof(out));
        REQUIRE(zero.size() == sizeof(frozen));
        CHECK(std::memcmp(out, frozen, sizeof(frozen)) == 0);
    }
}

TEST_CASE("An extensible struct's length takes the bytes its body needs", "[net][wire]")
{
    static const u8 kZeros[300] = {};
    for (const usize size : {usize{0}, usize{120}, usize{127}, usize{128}, usize{300}})
    {
        LongBody message{};
        message.before    = 5U;
        message.body.data = ConstSpan<u8>(kZeros, size);
        message.after     = 17U;
        u8         out[512] = {};
        const auto wire     = encoded(message, kMinor, out, sizeof(out));
        CHECK(wire.size() == encoded_bytes(message, kMinor));
        // Three bits and padding, one or two bytes of length, the flag and the data's length in 9 bits and padding,
        // the data, five bits and padding.
        const usize body = 2U + size;
        CHECK(wire.size() == 1U + ((body < 128U) ? 1U : 2U) + body + 1U);
        LongBody decoded{};
        REQUIRE(decode(wire, kMinor, decoded) == WireError::Ok);
        CHECK(same(decoded, message));
        CHECK(decoded.after == 17U);
    }
}

TEST_CASE("Encoding reports no room and writes past nothing", "[net][wire]")
{
    const u8 fixed[5]  = {1U, 2U, 3U, 4U, 5U};
    const u8 ranged[9] = {9U, 8U, 7U, 6U, 5U, 4U, 3U, 2U, 1U};
    Blobs    blobs{};
    blobs.fixed       = ConstSpan<u8>(fixed);
    blobs.ranged      = ConstSpan<u8>(ranged);
    blobs.title       = "a title long enough";
    blobs.has_maybe   = true;
    blobs.maybe       = ConstSpan<u8>(fixed);
    const usize total = encoded_bytes(blobs, kMinor);
    for (usize room = 0U; room < total; ++room)
    {
        u8 out[128];
        std::memset(out, 0xEEU, sizeof(out));
        usize written = 9U;
        CHECK(encode(blobs, kMinor, Span<u8>(out, room), written) == WireError::NoRoom);
        CHECK(written == 0U);
        for (usize i = room; i < sizeof(out); ++i)
        {
            CHECK(out[i] == 0xEEU);
        }
    }
    u8    out[128] = {};
    usize written  = 0U;
    CHECK(encode(blobs, kMinor, Span<u8>(out, total), written) == WireError::Ok);
    CHECK(written == total);
}

TEST_CASE("Quantised fields are hesap-net's formats", "[net][wire]")
{
    u8 out[128] = {};

    Quantized message{};
    message.height = 25.0F;
    message.fine   = 0.5F;
    const WireQuaternion path[2] = {WireQuaternion{}, WireQuaternion{{0.0F, 1.0F, 0.0F, 0.0F}}};
    message.path = WireList<QuantizedPathItemCodec>(ConstSpan<WireQuaternion>(path));
    const auto wire = encoded(message, kMinor, out, sizeof(out));

    STATIC_REQUIRE(QuantizedCodec::kLossy);
    STATIC_REQUIRE(kQuantizedHeightFormat.bits == 16U);
    STATIC_REQUIRE(kQuantizedFineFormat.bits == 10U);
    STATIC_REQUIRE(kQuantizedFineFormat.max_code == 1000U);

    BitReader reader{wire};
    u64       code = 0U;
    REQUIRE(reader.take(16U, code));
    CHECK(code == crd::hesap::net::bounded_encode(kQuantizedHeightFormat, 25.0F));
    REQUIRE(reader.take(10U, code));
    CHECK(code == crd::hesap::net::bounded_encode(kQuantizedFineFormat, 0.5F));
    u64 word = 0U;
    REQUIRE(reader.take(38U, word));
    u64       expected = 0U;
    const f32 identity[4] = {0.0F, 0.0F, 0.0F, 1.0F};
    crd::hesap::net::smallest_three_encode(kQuantizedTurnFormat, identity, 1U, &expected);
    CHECK(word == expected);

    Quantized decoded{};
    REQUIRE(decode(wire, kMinor, decoded) == WireError::Ok);
    f32 height = 0.0F;
    REQUIRE(crd::hesap::net::bounded_decode(kQuantizedHeightFormat,
                                            crd::hesap::net::bounded_encode(kQuantizedHeightFormat, 25.0F), height));
    CHECK(decoded.height == height);
    REQUIRE(decoded.path.size() == 2U);
    auto           cursor = decoded.path.cursor();
    WireQuaternion turn;
    REQUIRE(cursor.next(turn));
    REQUIRE(cursor.next(turn));
    CHECK(turn.xyzw[1] > 0.99999F);
    CHECK(turn.xyzw[1] <= 1.0F);

    SECTION("a word no encoder writes")
    {
        // Every stored component at its top leaves less than the smallest remainder.
        BitWriter writer{Span<u8>(out)};
        writer.put(0U, 16U);
        writer.put(0U, 10U);
        writer.put((u64{1} << 38U) - 1U, 38U);
        writer.put(0U, 3U);
        const usize size = writer.finish();
        Quantized refused{};
        CHECK(decode(ConstSpan<u8>(out, size), kMinor, refused) == WireError::BadQuaternion);
    }
    SECTION("a code past a resolution's top")
    {
        BitWriter writer{Span<u8>(out)};
        writer.put(0U, 16U);
        writer.put(1001U, 10U);
        writer.put(0U, 38U);
        writer.put(0U, 3U);
        const usize size = writer.finish();
        Quantized refused{};
        CHECK(decode(ConstSpan<u8>(out, size), kMinor, refused) == WireError::OutOfRange);
    }
    SECTION("what is not finite is not encoded")
    {
        usize written = 0U;
        message.height = std::bit_cast<f32>(u32{0x7F800000U});
        CHECK(encode(message, kMinor, Span<u8>(out), written) == WireError::NonFinite);
        message.height = 0.0F;
        message.turn.xyzw[0] = std::bit_cast<f32>(u32{0x7FC00000U});
        CHECK(encode(message, kMinor, Span<u8>(out), written) == WireError::NonFinite);
    }
}

TEST_CASE("A schema publishes what admission reads and fills its S18 artefact", "[net][wire]")
{
    STATIC_REQUIRE(ScalarsCodec::kMessageId == 1U);
    STATIC_REQUIRE(kSchema.major == 2U);
    STATIC_REQUIRE(kSchema.minor == 3U);
    REQUIRE(kSchema.messages.size() == 8U);
    REQUIRE(kSchema.channels.size() == 2U);
    u32 previous = 0U;
    for (const auto& message : kSchema.messages)
    {
        CHECK(message.id > previous);
        previous = message.id;
        REQUIRE(message.channel < kSchema.channels.size());
        CHECK(message.max_bytes >= message.min_bytes);
        CHECK(message.max_bytes <= kSchema.channels[message.channel].max_message_bytes);
        CHECK((message.operation & kSchema.operations) == message.operation);
        CHECK(message.operation != 0U);
    }
    CHECK(kSchema.operations == 0b100011U);
    CHECK(kSchema.idempotent == 0b100001U);
    CHECK(kSchema.channels[1].padding_block == 64U);
    CHECK(kSchema.channels[1].channel_class == crd::net::wire::ChannelClass::State);
    CHECK(kSchema.channels[1].idempotent);
    CHECK(kSchema.messages[0].operation == 0b100000U);

    crd::crypto::MessageSchema artefact{};
    artefact.kind = static_cast<crd::crypto::ScopeKind>(kSchema.kind);
    CHECK(artefact.kind == crd::crypto::ScopeKind::World);
    std::memcpy(artefact.schema.bytes, kSchema.id, sizeof(kSchema.id));
    artefact.major = kSchema.major;
    artefact.minor = kSchema.minor;
    artefact.audience.bytes[0] = 1U;
    artefact.operations = kSchema.operations;
    artefact.idempotent = kSchema.idempotent;
    CHECK(crd::crypto::validate_schema(artefact) == crd::crypto::CapabilityError::None);

    // A transport schema runs below S18.
    CHECK(control::kSchema.kind == 0U);
    CHECK(control::kSchema.operations == 0U);
    for (const auto& message : control::kSchema.messages)
    {
        CHECK(message.operation == 0U);
    }
}

TEST_CASE("The session control channel's bounds are crd-crypto's", "[net][wire]")
{
    static u8 big[crd::crypto::kMaxRevocationBytes + 1U];
    static u8 out[crd::crypto::kMaxRevocationBytes + 32U];

    SECTION("a capability at its bound and past it")
    {
        control::CapabilityPresent message{};
        message.token = ConstSpan<u8>(big, crd::crypto::kMaxCapabilityBytes);
        message.has_proof = true;
        message.proof = ConstSpan<u8>(big, crd::crypto::kMaxCapabilityProofBytes);
        usize written = 0U;
        REQUIRE(encode(message, control::kMinor, Span<u8>(out), written) == WireError::Ok);
        control::CapabilityPresent decoded{};
        REQUIRE(decode(ConstSpan<u8>(out, written), control::kMinor, decoded) == WireError::Ok);
        CHECK(decoded.token.size() == crd::crypto::kMaxCapabilityBytes);
        CHECK(decoded.proof.size() == crd::crypto::kMaxCapabilityProofBytes);
        message.token = ConstSpan<u8>(big, crd::crypto::kMaxCapabilityBytes + 1U);
        CHECK(encode(message, control::kMinor, Span<u8>(out), written) == WireError::OutOfRange);
    }
    SECTION("a schema and a revocation generation at their bounds and past them")
    {
        control::SchemaPresent schema{};
        schema.artefact = ConstSpan<u8>(big, crd::crypto::kMaxSchemaBytes);
        usize written = 0U;
        CHECK(encode(schema, control::kMinor, Span<u8>(out), written) == WireError::Ok);
        schema.artefact = ConstSpan<u8>(big, crd::crypto::kMaxSchemaBytes + 1U);
        CHECK(encode(schema, control::kMinor, Span<u8>(out), written) == WireError::OutOfRange);
        control::RevocationsPresent revocations{};
        revocations.generation = ConstSpan<u8>(big, crd::crypto::kMaxRevocationBytes);
        CHECK(encode(revocations, control::kMinor, Span<u8>(out), written) == WireError::Ok);
        revocations.generation = ConstSpan<u8>(big, crd::crypto::kMaxRevocationBytes + 1U);
        CHECK(encode(revocations, control::kMinor, Span<u8>(out), written) == WireError::OutOfRange);
    }
    SECTION("the small messages round-trip at their compact sizes")
    {
        control::StateHash hash{};
        hash.step  = 300U;
        hash.hash  = 0x0123456789ABCDEFULL;
        hash.chain = 0xFEDCBA9876543210ULL;
        usize written = 0U;
        REQUIRE(encode(hash, control::kMinor, Span<u8>(out), written) == WireError::Ok);
        CHECK(written == 18U);
        control::StateHash read{};
        REQUIRE(decode(ConstSpan<u8>(out, written), control::kMinor, read) == WireError::Ok);
        CHECK(same(hash, read));
        CHECK(read.step == 300U);

        control::GoAway away{};
        away.reason         = control::GoAwayReason::Overloaded;
        away.retry_after_ms = 1500U;
        away.detail         = "try later";
        REQUIRE(encode(away, control::kMinor, Span<u8>(out), written) == WireError::Ok);
        CHECK(written == 13U); // 4 + 18 + 9 bits, padded, and nine bytes of text
        control::GoAway got{};
        REQUIRE(decode(ConstSpan<u8>(out, written), control::kMinor, got) == WireError::Ok);
        CHECK(same(away, got));
        CHECK(got.detail == "try later");
        const u8 unknown[3] = {0xF0U, 0U, 0U};
        CHECK(decode(ConstSpan<u8>(unknown), control::kMinor, got) == WireError::UnknownValue);

        control::GenerationAnnounce generations{};
        generations.policy_generation = ~u64{0};
        REQUIRE(encode(generations, control::kMinor, Span<u8>(out), written) == WireError::Ok);
        CHECK(written == 12U);
        CHECK(control::GenerationAnnounceCodec::kMaxBytes == 30U);
    }
}
