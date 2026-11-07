// crd-net -- baseline deltas (NET.3; docs/design/network-foundation.md#baseline-deltas): the known answers of the
// generator's reference interpreter, then what a reference cannot say: which refusal a damaged delta gets, the exact
// bits of each form, the store's eviction, and writers and readers on a hostile path.

#include <crd/containers/array.hpp>
#include <crd/containers/checksum.hpp>
#include <crd/hesap/net/quantize.hpp>
#include <crd/memory/allocator.hpp>
#include <crd/net/admission.hpp>
#include <crd/net/delta.hpp>
#include <crd/net/wire/session_control.hpp>

#include <algorithm>
#include <bit>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <generated/delta_test.hpp>
#include <initializer_list>
#include <random>
#include <utility>
#include <wire_vector_checks.hpp>

using crd::f32;
using crd::i16;
using crd::i32;
using crd::i64;
using crd::i8;
using crd::u16;
using crd::u32;
using crd::u64;
using crd::u8;
using crd::usize;
using crd::containers::BitWriter;
using crd::containers::ConstSpan;
using crd::containers::Span;
using crd::net::BaselineStore;
using crd::net::delta_expand_sequence;
using crd::net::DeltaRead;
using crd::net::DeltaReader;
using crd::net::DeltaWriter;
using crd::net::kDeltaHeaderMaxBytes;
using crd::net::kDeltaHeaderMinBytes;
using crd::net::wire::decode;
using crd::net::wire::delta_apply;
using crd::net::wire::delta_encode;
using crd::net::wire::DeltaLayout;
using crd::net::wire::encode;
using crd::net::wire::kDeltaSlackBytes;
using crd::net::wire::kWireMinI64;
using crd::net::wire::wire_put_varuint;
using crd::net::wire::WireError;
using crd::net::wire::WireList;
using crd::net::wire::testing::check_value;
using crd::net::wire::testing::check_verdict;
using crd::net::wire::testing::from_hex;
using crd::net::wire::testing::kMaxVectorBytes;

using namespace crd::net::wire::delta_test;
namespace control = crd::net::wire::session_control;
namespace cont = crd::containers;
using Bytes = cont::Array<u8>;

namespace
{

// Counts every request, so a test can say nothing was asked for after construction.
class CountingAllocator final : public crd::memory::IAllocator
{
public:
    void* allocate(usize size, usize) override
    {
        ++m_calls;
        void* const memory = std::malloc(size == 0U ? 1U : size);
        if (memory != nullptr)
        {
            std::memset(memory, 0xA5, size);
        }
        return memory;
    }

    void deallocate(void* pointer) noexcept override { std::free(pointer); }

    [[nodiscard]] bool owns(const void*) const noexcept override { return true; }

    [[nodiscard]] usize calls() const noexcept { return m_calls; }

private:
    usize m_calls = 0U;
};

Bytes filled(usize count, u8 value)
{
    Bytes out;
    out.resize(count, value);
    return out;
}

Bytes sized(usize count)
{
    return filled(count, 0U);
}

template <typename M>
void check_delta(u16 minor, const char* baseline_hex, const char* current_hex, const char* delta_hex)
{
    static u8 baseline[kMaxVectorBytes];
    static u8 current[kMaxVectorBytes];
    static u8 expected[kMaxVectorBytes];
    static u8 out[kMaxVectorBytes + kDeltaSlackBytes];
    const usize baseline_size = from_hex(baseline_hex, baseline);
    const usize current_size = from_hex(current_hex, current);
    const usize expected_size = from_hex(delta_hex, expected);
    const DeltaLayout& layout = *crd::net::wire::find_message(kSchema, M::WireCodec::kMessageId)->delta;

    usize written = 0U;
    REQUIRE(delta_encode(layout, ConstSpan<u8>(baseline, baseline_size), ConstSpan<u8>(current, current_size), minor,
                         out, written) == WireError::Ok);
    REQUIRE(written == expected_size);
    CHECK((written == 0U || std::memcmp(out, expected, written) == 0));

    usize rebuilt = 0U;
    REQUIRE(delta_apply(layout, ConstSpan<u8>(baseline, baseline_size), ConstSpan<u8>(expected, expected_size), minor,
                        out, rebuilt) == WireError::Ok);
    REQUIRE(rebuilt == current_size);
    CHECK((rebuilt == 0U || std::memcmp(out, current, rebuilt) == 0));
}

template <typename M>
void check_delta_verdict(u16 writer_minor, const char* baseline_hex, const char* delta_hex, bool accepted,
                         const char* rebuilt_hex)
{
    static u8 baseline[kMaxVectorBytes];
    static u8 delta[kMaxVectorBytes];
    static u8 expected[kMaxVectorBytes];
    static u8 out[kMaxVectorBytes + kDeltaSlackBytes];
    const usize baseline_size = from_hex(baseline_hex, baseline);
    const usize delta_size = from_hex(delta_hex, delta);
    const usize expected_size = from_hex(rebuilt_hex, expected);
    const DeltaLayout& layout = *crd::net::wire::find_message(kSchema, M::WireCodec::kMessageId)->delta;

    usize rebuilt = 0U;
    WireError error = delta_apply(layout, ConstSpan<u8>(baseline, baseline_size), ConstSpan<u8>(delta, delta_size),
                                  writer_minor, out, rebuilt);
    if (error == WireError::Ok)
    {
        error = layout.validate(ConstSpan<u8>(out, rebuilt), writer_minor);
    }
    REQUIRE((error == WireError::Ok) == accepted);
    if (accepted)
    {
        REQUIRE(rebuilt == expected_size);
        CHECK((rebuilt == 0U || std::memcmp(out, expected, rebuilt) == 0));
    }
}

template <typename M> Bytes encoded(const M& message, u16 minor = kMinor)
{
    Bytes out = sized(kMaxVectorBytes);
    usize size = 0U;
    REQUIRE(encode(message, minor, Span<u8>(out), size) == WireError::Ok);
    out.resize(size);
    return out;
}

Bytes delta_of(const DeltaLayout& layout, const Bytes& baseline, const Bytes& current, u16 minor = kMinor)
{
    Bytes out = sized(kMaxVectorBytes);
    usize size = 0U;
    REQUIRE(delta_encode(layout, baseline, current, minor, Span<u8>(out), size) == WireError::Ok);
    out.resize(size);
    return out;
}

WireError apply_to(const DeltaLayout& layout, const Bytes& baseline, const Bytes& delta, Bytes& out, u16 minor = kMinor)
{
    out = sized(kMaxVectorBytes);
    usize size = 0U;
    const WireError error = delta_apply(layout, baseline, delta, minor, Span<u8>(out), size);
    out.resize(size);
    return error;
}

// Bits by hand, most significant first: each step is {value, width}.
Bytes bits_of(std::initializer_list<std::pair<u64, u32>> steps)
{
    Bytes out = sized(256U);
    BitWriter w{Span<u8>(out)};
    for (const auto& [value, width] : steps)
    {
        w.put(value, width);
    }
    out.resize(w.finish());
    return out;
}

Seat seat(i64 place, bool held)
{
    Seat out{};
    out.place = static_cast<decltype(out.place)>(place);
    out.held = held;
    return out;
}

Member member(u32 rank, i64 score)
{
    Member out{};
    out.rank = rank;
    out.score = static_cast<decltype(out.score)>(score);
    return out;
}

// A Roster of one member, the seats given and two fixed seats, for tests of one list at a time.
struct RosterOf
{
    Member members[6] = {member(10U, 1)};
    usize member_count = 1U;
    cont::Array<Seat> seats;
    Seat ordered[2] = {seat(0, false), seat(0, false)};
    cont::Array<Header> scores;

    [[nodiscard]] Roster get() const
    {
        Roster out{};
        out.members = WireList<MemberCodec>(ConstSpan<Member>(members, member_count));
        out.seats = WireList<SeatCodec>(ConstSpan<Seat>(seats));
        out.ordered = WireList<SeatCodec>(ConstSpan<Seat>(ordered, 2U));
        out.scores = WireList<HeaderCodec>(ConstSpan<Header>(scores));
        return out;
    }
};

Snapshot snapshot_of(ConstSpan<Entity> entities, u64 tick = 7U)
{
    Snapshot out{};
    out.tick = tick;
    out.entities = WireList<EntityCodec>(entities);
    out.header.level = 3U;
    return out;
}

Entity entity(u16 id)
{
    Entity out{};
    out.id = id;
    static constexpr u8 kFixed[3] = {1U, 2U, 3U};
    out.fixed = ConstSpan<u8>(kFixed);
    return out;
}

} // namespace

TEST_CASE("Deltas hold to the reference interpreter's bytes and verdicts", "[net][delta]")
{
#include <generated/delta_test_vectors.inc>
}

TEST_CASE("A delta names each changed field and sends only that", "[net][delta]")
{
    cont::Array<Entity> before{entity(5U), entity(9U)};
    cont::Array<Entity> after = before;
    after[1].pose.yaw = 100U;
    after[1].pose.yaw = 101U;
    const Bytes baseline_yaw = encoded(snapshot_of(before));
    const Bytes current_yaw = encoded(snapshot_of(after));
    const Bytes delta = delta_of(kSnapshotLayout, baseline_yaw, current_yaw);

    // tick unchanged 0; entities changed 1, the same count; entity 5 kept 0; entity 9 changed 10, id skipped: pose
    // changed 1; x, y, z unchanged 000; yaw changed 1 with the folded step +101 = 202 in 2-bit groups, then six
    // unchanged, then the ten other fields of the entity unchanged; header unchanged 0, shift 0, tail 0.
    const u64 folded = 202U; // 11 00 10 10 by groups, least significant first: 10, 10, 00, 11
    const Bytes expected = bits_of({{0U, 1U},
                                    {1U, 1U},
                                    {2U, 4U}, // the count of 2
                                    {0U, 1U},
                                    {0b10U, 2U},
                                    {1U, 1U},
                                    {0U, 3U},
                                    {1U, 1U},
                                    {0b110U, 3U},
                                    {0b110U, 3U},
                                    {0b100U, 3U},
                                    {0b011U, 3U},
                                    {0U, 5U}, // speed, wide, far, plain_count, plain_turn
                                    {0U, 9U}, // seen to back
                                    {0U, 3U}});
    (void)folded;
    CHECK(delta == expected);

    Bytes rebuilt;
    REQUIRE(apply_to(kSnapshotLayout, baseline_yaw, delta, rebuilt) == WireError::Ok);
    CHECK(rebuilt == current_yaw);
}

TEST_CASE("Differences wrap at their field's ends and stay small either way", "[net][delta]")
{
    // wide: 0 to the top of 64 bits is -1, and range x's two ends are one step apart too.
    Entity from = entity(1U);
    Entity to = from;
    from.pose.wide = 0U;
    to.pose.wide = ~u64{0U};
    from.pose.x = -32768;
    to.pose.x = 32767;
    from.pose.far = -9223372036854775807LL - 1LL;
    to.pose.far = 9223372036854775807LL;
    from.pose.speed = 0U;
    to.pose.speed = 1000000U;
    const Bytes old_bytes = encoded(snapshot_of(ConstSpan<Entity>(&from, 1U)));
    const Bytes new_bytes = encoded(snapshot_of(ConstSpan<Entity>(&to, 1U)));
    const Bytes delta = delta_of(kSnapshotLayout, old_bytes, new_bytes);
    Bytes rebuilt;
    REQUIRE(apply_to(kSnapshotLayout, old_bytes, delta, rebuilt) == WireError::Ok);
    CHECK(rebuilt == new_bytes);
    const Bytes back = delta_of(kSnapshotLayout, new_bytes, old_bytes);
    REQUIRE(apply_to(kSnapshotLayout, new_bytes, back, rebuilt) == WireError::Ok);
    CHECK(rebuilt == old_bytes);
    // x and far cost one group each, wide 9 bits: the whole forms would take 16 + 72 + 64 bits.
    CHECK(delta.size() < 16U);

    // A difference that wraps a range past its span is its decoder's refusal, not a value.
    RosterOf roster;
    roster.members[0] = member(10U, 90);
    const Bytes at_top = encoded(roster.get());
    // members changed, count 1, key kept: changed 10, score changed, y: the score's whole 7-bit code, one past the
    // span.
    const Bytes past =
        bits_of({{1U, 1U}, {0U, 3U}, {0b10U, 2U}, {1U, 1U}, {96U, 7U}, {0U, 1U}, {0U, 1U}, {0U, 1U}, {0U, 1U}});
    Bytes out;
    REQUIRE(apply_to(kRosterLayout, at_top, past, out) == WireError::Ok);
    CHECK(kRosterLayout.validate(out, kMinor) == WireError::OutOfRange);
}

TEST_CASE("A field said to have changed that did not has no encoding", "[net][delta]")
{
    RosterOf roster;
    roster.seats = {seat(-3, false), seat(4, true)};
    const Bytes baseline = encoded(roster.get());
    Bytes out;

    // members: changed, same count, then keep: nothing changed.
    CHECK(apply_to(kRosterLayout, baseline, bits_of({{1U, 1U}, {0U, 3U}, {0U, 1U}, {0U, 3U}}), out) ==
          WireError::Unchanged);
    // members: changed, same count, changed entry with no field changed.
    CHECK(apply_to(kRosterLayout, baseline, bits_of({{1U, 1U}, {0U, 3U}, {0b10U, 2U}, {0U, 2U}, {0U, 3U}}), out) ==
          WireError::Unchanged);
    // members: entry's score named with its own value.
    CHECK(apply_to(kRosterLayout, baseline,
                   bits_of({{1U, 1U}, {0U, 3U}, {0b10U, 2U}, {1U, 1U}, {6U, 7U}, {0U, 1U}, {0U, 3U}}),
                   out) == WireError::Unchanged);
    // title absent to absent.
    CHECK(apply_to(kRosterLayout, baseline,
                   bits_of({{1U, 1U}, {0U, 3U}, {0b10U, 2U}, {0U, 1U}, {1U, 1U}, {0U, 1U}, {0U, 3U}}),
                   out) == WireError::Unchanged);
    // ordered by position: changed, both kept.
    CHECK(apply_to(kRosterLayout, baseline, bits_of({{0U, 2U}, {1U, 1U}, {0U, 2U}, {0U, 1U}}), out) ==
          WireError::Unchanged);
    // ordered: an element changed with no field changed.
    CHECK(apply_to(kRosterLayout, baseline, bits_of({{0U, 2U}, {1U, 1U}, {1U, 1U}, {0U, 2U}, {0U, 1U}, {0U, 1U}}),
                   out) == WireError::Unchanged);

    // A varuint and a varsint named with their own value: pose.plain_count and pose.plain_turn.
    Entity one = entity(1U);
    one.pose.plain_count = 40U;
    one.pose.plain_turn = -40;
    one.pose.yaw = 9U;
    const Bytes with_entity = encoded(snapshot_of(ConstSpan<Entity>(&one, 1U)));
    const auto pose_delta = [&](std::initializer_list<std::pair<u64, u32>> fields)
    {
        Bytes out_bits = sized(256U);
        BitWriter w{Span<u8>(out_bits)};
        const auto put_all = [&w](std::initializer_list<std::pair<u64, u32>> steps)
        {
            for (const auto& [value, width] : steps)
            {
                w.put(value, width);
            }
        };
        put_all({{0U, 1U}, {1U, 1U}, {1U, 4U}, {0b10U, 2U}, {1U, 1U}});
        put_all(fields);
        put_all({{0U, 9U}, {0U, 3U}});
        out_bits.resize(w.finish());
        return out_bits;
    };
    // x..wide unchanged, plain_count changed to 40 in its 7-bit group, plain_turn unchanged.
    CHECK(apply_to(kSnapshotLayout, with_entity, pose_delta({{0U, 7U}, {1U, 1U}, {40U, 8U}, {0U, 1U}}), out) ==
          WireError::Unchanged);
    // plain_turn changed to its own zigzag code 79: 1001111 in one group.
    CHECK(apply_to(kSnapshotLayout, with_entity, pose_delta({{0U, 7U}, {0U, 1U}, {1U, 1U}, {79U, 8U}}), out) ==
          WireError::Unchanged);
    // yaw's difference of zero, and a uint's difference of zero through the varuint.
    CHECK(apply_to(kSnapshotLayout, with_entity, pose_delta({{0U, 3U}, {1U, 1U}, {0U, 3U}, {0U, 5U}}), out) ==
          WireError::Unchanged);
    // The bytes of blob named with their own value (empty).
    CHECK(apply_to(kSnapshotLayout, with_entity,
                   bits_of({{0U, 1U},
                            {1U, 1U},
                            {1U, 4U},
                            {0b10U, 2U},
                            {0U, 1U},
                            {0U, 1U},
                            {0U, 1U},
                            {0U, 1U},
                            {0U, 1U},
                            {0U, 1U},
                            {1U, 1U},
                            {0U, 4U}}),
                   out) == WireError::Unchanged);
    // A plain struct changed with no field changed.
    CHECK(apply_to(kSnapshotLayout, with_entity, pose_delta({{0U, 9U}}), out) == WireError::Unchanged);
    // An extensible struct changed with no field changed: header at the message's end.
    CHECK(apply_to(kSnapshotLayout, with_entity, bits_of({{0U, 2U}, {1U, 1U}, {0U, 5U}, {1U, 8U}, {0U, 8U}}), out) ==
          WireError::Unchanged);
}

TEST_CASE("A keyed list merges by its keys, and each edit has one encoding", "[net][delta]")
{
    RosterOf roster;
    roster.seats = {seat(-3, false), seat(4, true), seat(9, false)};
    const Bytes baseline = encoded(roster.get());

    SECTION("Implicit once either side runs out, and every operation in between")
    {
        RosterOf next = roster;
        next.seats = {seat(-5, false), seat(-3, true), seat(9, false), seat(20, true), seat(30, true)};
        const Bytes current = encoded(next.get());
        const Bytes delta = delta_of(kRosterLayout, baseline, current);
        // members 0; seats changed, count 5 in 3 bits; -5 inserted (place code 45 and held 0); -3 changed, its held
        // changed (a bit is its own news); 4 removed; 9 kept; 20 and 30 implicit inserts; ordered 0, scores 0.
        const Bytes expected = bits_of({{0U, 1U},
                                        {1U, 1U},
                                        {5U, 3U},
                                        {0b110U, 3U},
                                        {45U, 7U},
                                        {0U, 1U},
                                        {0b10U, 2U},
                                        {1U, 1U},
                                        {0b111U, 3U},
                                        {0U, 1U},
                                        {70U, 7U},
                                        {1U, 1U},
                                        {80U, 7U},
                                        {1U, 1U},
                                        {0U, 2U}});
        CHECK(delta == expected);
        Bytes rebuilt;
        REQUIRE(apply_to(kRosterLayout, baseline, delta, rebuilt) == WireError::Ok);
        CHECK(rebuilt == current);
    }

    SECTION("Everything removed takes no operation at all")
    {
        RosterOf next = roster;
        next.seats.clear();
        const Bytes delta = delta_of(kRosterLayout, baseline, encoded(next.get()));
        CHECK(delta == bits_of({{0U, 1U}, {1U, 1U}, {0U, 3U}, {0U, 2U}}));
    }

    SECTION("A hostile merge")
    {
        Bytes out;
        // An insertion at or above the next old key.
        CHECK(apply_to(kRosterLayout, baseline,
                       bits_of({{0U, 1U}, {1U, 1U}, {4U, 3U}, {0b110U, 3U}, {47U, 7U}, {0U, 1U}}),
                       out) == WireError::KeyOrder);
        // Removing a key, then inserting it again, says nothing new.
        CHECK(apply_to(kRosterLayout, baseline,
                       bits_of({{0U, 1U}, {1U, 1U}, {3U, 3U}, {0b111U, 3U}, {0b110U, 3U}, {47U, 7U}, {0U, 1U}}),
                       out) == WireError::KeyOrder);
        // An insertion below one already written.
        CHECK(apply_to(kRosterLayout, baseline,
                       bits_of({{0U, 1U},
                                {1U, 1U},
                                {5U, 3U},
                                {0b110U, 3U},
                                {46U, 7U},
                                {0U, 1U},
                                {0b110U, 3U},
                                {45U, 7U},
                                {0U, 1U}}),
                       out) == WireError::KeyOrder);
        // An implicit insertion past the end that does not rise above the last old key.
        CHECK(apply_to(kRosterLayout, baseline,
                       bits_of({{0U, 1U}, {1U, 1U}, {4U, 3U}, {0U, 1U}, {0U, 1U}, {0U, 1U}, {50U, 7U}, {0U, 1U}}),
                       out) == WireError::KeyOrder);
    }

    SECTION("Encoders and decoders hold the keys in order, so a baseline always ascends")
    {
        RosterOf disordered = roster;
        disordered.seats = {seat(4, true), seat(-3, false)};
        Bytes out = sized(kMaxVectorBytes);
        usize size = 0U;
        CHECK(encode(disordered.get(), kMinor, Span<u8>(out), size) == WireError::KeyOrder);
        RosterOf repeated = roster;
        repeated.seats = {seat(4, true), seat(4, false)};
        CHECK(encode(repeated.get(), kMinor, Span<u8>(out), size) == WireError::KeyOrder);

        // One seat flipped by hand into the wrong order: the same bytes as a disordered encoder would write.
        const Bytes bad = bits_of({{0U, 3U},
                                   {10U, 8U},
                                   {6U, 7U},
                                   {0U, 1U},
                                   {2U, 3U},
                                   {54U, 7U},
                                   {1U, 1U},
                                   {47U, 7U},
                                   {0U, 1U},
                                   {50U, 7U},
                                   {0U, 1U},
                                   {50U, 7U},
                                   {0U, 1U},
                                   {0U, 2U}});
        Roster message{};
        CHECK(decode(ConstSpan<u8>(bad), kMinor, message) == WireError::KeyOrder);
    }
}

TEST_CASE("Bytes and lists keep their alignment wherever they land", "[net][delta]")
{
    // A varuint before a byte field changes width, so its bytes land elsewhere in the delta and in the message.
    Entity one = entity(2U);
    static constexpr u8 kBlobOld[] = {0xAAU, 0xBBU, 0xCCU};
    static constexpr u8 kBlobNew[] = {0x11U, 0x22U};
    one.blob = ConstSpan<u8>(kBlobOld);
    one.tag = "before";
    one.has_tag = true;
    Entity two = one;
    two.blob = ConstSpan<u8>(kBlobNew);
    two.pose.speed = 999999U;
    two.has_tag = false;
    two.tag = {};
    static constexpr u8 kBag[] = {200U, 100U, 7U};
    two.bag = WireList<EntityBagItemCodec>(ConstSpan<u8>(kBag));
    two.has_back = true;
    two.back.yaw = 3U;
    const Bytes old_bytes = encoded(snapshot_of(ConstSpan<Entity>(&one, 1U), 1U));
    const Bytes new_bytes = encoded(snapshot_of(ConstSpan<Entity>(&two, 1U), 123456789U));
    Bytes rebuilt;
    REQUIRE(apply_to(kSnapshotLayout, old_bytes, delta_of(kSnapshotLayout, old_bytes, new_bytes), rebuilt) ==
            WireError::Ok);
    CHECK(rebuilt == new_bytes);
    REQUIRE(apply_to(kSnapshotLayout, new_bytes, delta_of(kSnapshotLayout, new_bytes, old_bytes), rebuilt) ==
            WireError::Ok);
    CHECK(rebuilt == old_bytes);

    // A padding bit before a delta's bytes is refused.
    const Bytes bad_padding = bits_of({{0U, 1U},
                                       {1U, 1U},
                                       {1U, 4U},
                                       {0b10U, 2U},
                                       {0U, 1U},
                                       {0U, 1U},
                                       {0U, 1U},
                                       {0U, 1U},
                                       {0U, 1U},
                                       {0U, 1U},
                                       {1U, 1U},
                                       {1U, 4U},
                                       {1U, 2U},
                                       {0xFFU, 8U}});
    CHECK(apply_to(kSnapshotLayout, old_bytes, bad_padding, rebuilt) == WireError::BadPadding);
}

TEST_CASE("Extensible structs: bodies inside bodies, and what a newer writer adds", "[net][delta]")
{
    Snapshot from = snapshot_of({});
    from.header.label = "first label";
    from.header.nested.depth = 2U;
    static constexpr u8 kNote[] = {1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U, 9U, 10U, 11U, 12U};
    from.header.nested.note = ConstSpan<u8>(kNote);
    Snapshot to = from;
    to.header.nested.depth = 5U;
    to.header.label = "x";
    const Bytes old_bytes = encoded(from);
    const Bytes new_bytes = encoded(to);
    const Bytes delta = delta_of(kSnapshotLayout, old_bytes, new_bytes);
    Bytes rebuilt;
    REQUIRE(apply_to(kSnapshotLayout, old_bytes, delta, rebuilt) == WireError::Ok);
    CHECK(rebuilt == new_bytes);

    // A newer writer appended bits to the message: this reader ignores them and its result lacks them.
    Bytes newer = delta;
    newer.push_back(0xFFU);
    newer.push_back(0xFFU);
    REQUIRE(apply_to(kSnapshotLayout, old_bytes, newer, rebuilt, kMinor + 1U) == WireError::Ok);
    CHECK(rebuilt == new_bytes);
    // The same bytes from a writer this reader knows have a trailing byte, or a padding bit.
    CHECK(apply_to(kSnapshotLayout, old_bytes, newer, rebuilt) != WireError::Ok);

    // Inside an extensible body too: a changed bit there with no field this reader knows is not "unchanged" for it, but
    // from a known writer it is.
    Snapshot base = snapshot_of({});
    const Bytes plain = encoded(base);
    // tick, entities unchanged; header changed; align; a body of one byte: level, label, nested, added, more unchanged.
    const Bytes nothing = bits_of({{0U, 2U}, {1U, 1U}, {0U, 5U}, {1U, 8U}, {0U, 8U}, {0U, 2U}});
    CHECK(apply_to(kSnapshotLayout, plain, nothing, rebuilt) == WireError::Unchanged);
    REQUIRE(apply_to(kSnapshotLayout, plain, nothing, rebuilt, kMinor + 1U) == WireError::Ok);

    // Room: a body larger than the writer's room says NoRoom, never a broken message.
    Bytes small = sized(4U);
    usize size = 123U;
    CHECK(delta_encode(kSnapshotLayout, old_bytes, new_bytes, kMinor, Span<u8>(small), size) == WireError::NoRoom);
    CHECK(size == 0U);
    CHECK(delta_apply(kSnapshotLayout, old_bytes, delta, kMinor, Span<u8>(small), size) == WireError::NoRoom);
    CHECK(size == 0U);
    CHECK(delta_encode(kSnapshotLayout, old_bytes, new_bytes, kMinor + 1U, Span<u8>(small), size) ==
          WireError::BadRevision);

    // The least room delta_apply needs is its message's size and the slack.
    Bytes exact = sized(new_bytes.size() + kDeltaSlackBytes);
    size = 0U;
    CHECK(delta_apply(kSnapshotLayout, old_bytes, delta, kMinor, Span<u8>(exact), size) == WireError::Ok);
    CHECK(size == new_bytes.size());
}

TEST_CASE("Every damaged delta is refused or rebuilds a valid message, and truncation never reads past", "[net][delta]")
{
    cont::Array<Entity> from{entity(1U), entity(3U), entity(8U)};
    cont::Array<Entity> to = from;
    REQUIRE(to.data() != nullptr); // what GCC's link-time -Wnull-dereference cannot see of a copy
    to[0].pose.x = 77;
    to[1].tag = "tag";
    to[1].has_tag = true;
    to.erase(2U);
    to.push_back(entity(12U));
    Snapshot one = snapshot_of(from, 5U);
    Snapshot two = snapshot_of(to, 9U);
    two.header.label = "abc";
    const Bytes old_bytes = encoded(one);
    const Bytes new_bytes = encoded(two);
    const Bytes delta = delta_of(kSnapshotLayout, old_bytes, new_bytes);
    Bytes out;
    for (usize cut = 0U; cut < delta.size(); ++cut)
    {
        const Bytes shorter(delta.begin(), delta.begin() + static_cast<std::ptrdiff_t>(cut));
        CHECK(apply_to(kSnapshotLayout, old_bytes, shorter, out) != WireError::Ok);
    }
    for (usize bit = 0U; bit < delta.size() * 8U; ++bit)
    {
        Bytes damaged = delta;
        damaged[bit / 8U] = static_cast<u8>(damaged[bit / 8U] ^ (0x80U >> (bit % 8U)));
        if (apply_to(kSnapshotLayout, old_bytes, damaged, out) == WireError::Ok)
        {
            CHECK(out != new_bytes);
        }
    }
}

TEST_CASE("A quantised field compares by its code", "[net][delta]")
{
    Motion from{};
    from.x = 10.0F;
    from.height = 5.0F;
    Motion near = from;
    near.height = 5.0001F; // the same code in 8 bits over 10 metres
    const Bytes old_bytes = encoded(from);
    const Bytes same_bytes = encoded(near);
    const Bytes nothing = delta_of(kMotionLayout, old_bytes, same_bytes);
    CHECK(nothing == bits_of({{0U, 3U}}));

    Motion moved = from;
    moved.x = 10.01F;
    const Bytes new_bytes = encoded(moved);
    const Bytes delta = delta_of(kMotionLayout, old_bytes, new_bytes);
    CHECK(delta.size() <= 2U); // the x code moves by a few steps: a changed bit, a few groups, two unchanged bits
    Bytes rebuilt;
    REQUIRE(apply_to(kMotionLayout, old_bytes, delta, rebuilt) == WireError::Ok);
    CHECK(rebuilt == new_bytes);
}

TEST_CASE("A delta message's description bounds its frame's body", "[net][delta]")
{
    const auto* snapshot = crd::net::wire::find_message(kSchema, 1U);
    REQUIRE(snapshot != nullptr);
    REQUIRE(snapshot->delta == &kSnapshotLayout);
    CHECK(snapshot->min_bytes == kDeltaHeaderMinBytes + 1U);
    CHECK(snapshot->max_bytes == kDeltaHeaderMaxBytes + SnapshotCodec::kMaxBytes);
    CHECK(snapshot->max_bytes <= kSchema.channels[0].max_message_bytes);
    const auto* plain = crd::net::wire::find_message(kSchema, 3U);
    REQUIRE(plain != nullptr);
    CHECK(plain->delta == nullptr);
    CHECK(plain->min_bytes == PlainCodec::kMinBytes);

    // Nothing writes or reads a delta message's body as if it were the encoding.
    Snapshot message = snapshot_of({});
    Bytes out = sized(kMaxVectorBytes);
    usize written = 7U;
    CHECK(crd::net::write_frame(kSchema, crd::net::HeaderForm::Sized, message, kMinor, 0U, Span<u8>(out), written) ==
          WireError::OutOfRange);
    CHECK(written == 0U);
    crd::net::AdmittedFrame frame;
    frame.message = snapshot;
    CHECK(crd::net::decode_frame(frame, message) == WireError::OutOfRange);
    Plain other{};
    written = 0U;
    CHECK(crd::net::write_frame(kSchema, crd::net::HeaderForm::Sized, other, kMinor, 1U, Span<u8>(out), written) ==
          WireError::Ok);
}

TEST_CASE("Session control's acknowledgement reads at its revision", "[net][delta]")
{
    control::BaselineAck ack{};
    ack.channel = 3U;
    ack.message = 1U;
    ack.object = 99U;
    ack.sequence = 65535U;
    Bytes out = sized(32U);
    usize size = 0U;
    REQUIRE(encode(ack, control::kMinor, Span<u8>(out), size) == WireError::Ok);
    control::BaselineAck read{};
    REQUIRE(decode(ConstSpan<u8>(out.data(), size), control::kMinor, read) == WireError::Ok);
    CHECK(read.sequence == 65535U);
    CHECK(read.object == 99U);
}

TEST_CASE("The store keeps what fits, gives up the oldest and never allocates again", "[net][delta]")
{
    CountingAllocator allocator;
    BaselineStore store(allocator, 4U, 100U);
    REQUIRE(store.ready());
    const usize calls = allocator.calls();
    Bytes bytes = filled(40U, 1U);
    ConstSpan<u8> view;

    SECTION("Slots")
    {
        const Bytes small = filled(5U, 2U);
        for (u64 sequence = 10U; sequence < 14U; ++sequence)
        {
            REQUIRE(store.put(sequence, small));
        }
        CHECK(store.find(10U, view));
        CHECK(view.size() == 5U);
        CHECK(view[0] == 2U);
        REQUIRE(store.put(14U, small)); // takes 10's slot
        CHECK_FALSE(store.find(10U, view));
        CHECK(store.find(11U, view));
        CHECK(store.find(14U, view));
        // A gap in the sequence costs only the slots it lands on.
        REQUIRE(store.put(30U, small));
        CHECK(store.find(11U, view));
        CHECK_FALSE(store.find(14U, view));
    }

    SECTION("Room: a message that wraps gives up only what it lands on")
    {
        REQUIRE(store.put(1U, bytes));
        bytes = filled(40U, 2U);
        REQUIRE(store.put(2U, bytes));
        bytes = filled(40U, 3U);
        REQUIRE(store.put(3U, bytes)); // 80 + 40 > 100: back to the start, over 1
        CHECK_FALSE(store.find(1U, view));
        REQUIRE(store.find(2U, view));
        CHECK(view[0] == 2U);
        REQUIRE(store.find(3U, view));
        CHECK(view[39] == 3U);
        bytes = filled(40U, 4U);
        REQUIRE(store.put(4U, bytes)); // 40 to 80, over 2
        CHECK_FALSE(store.find(2U, view));
        REQUIRE(store.find(3U, view));
        CHECK(view[0] == 3U);
    }

    SECTION("Exactly to the end, then a wrap over slot zero's message")
    {
        Bytes head = filled(40U, 1U);
        Bytes rest = filled(60U, 2U);
        REQUIRE(store.put(4U, head)); // slot 0
        REQUIRE(store.put(5U, rest)); // ends exactly at the room's end, so 4 stays
        REQUIRE(store.find(4U, view));
        CHECK(view[39] == 1U);
        const Bytes one = filled(1U, 7U);
        REQUIRE(store.put(6U, one)); // wraps to the start, over 4
        CHECK_FALSE(store.find(4U, view));
        REQUIRE(store.find(5U, view));
        CHECK(view[0] == 2U);
        REQUIRE(store.find(6U, view));
        REQUIRE(view.size() == 1U);
        CHECK(view[0] == 7U);
    }

    SECTION("What it refuses")
    {
        REQUIRE(store.put(5U, bytes));
        CHECK_FALSE(store.put(5U, bytes));
        CHECK_FALSE(store.put(4U, bytes));
        CHECK_FALSE(store.put(6U, filled(101U, 0U)));
        REQUIRE(store.put(6U, filled(100U, 9U))); // exactly its room
        CHECK_FALSE(store.find(5U, view));
        CHECK(store.find(6U, view));
        CHECK_FALSE(store.find(7U, view));
        REQUIRE(store.put(7U, ConstSpan<u8>{})); // an empty message is still a message
        CHECK(store.find(7U, view));
        CHECK(view.empty());
    }
    CHECK(allocator.calls() == calls);
}

TEST_CASE("A store refused by its allocator keeps nothing", "[net][delta]")
{
    class Refuses final : public crd::memory::IAllocator
    {
    public:
        explicit Refuses(u32 grants) : m_grants(grants) {} // one bit per request, lowest first
        void* allocate(usize size, usize) override
        {
            const bool granted = (m_grants & 1U) != 0U;
            m_grants >>= 1U;
            return granted ? std::malloc(size) : nullptr;
        }
        void deallocate(void* pointer) noexcept override { std::free(pointer); }
        [[nodiscard]] bool owns(const void*) const noexcept override { return true; }
        [[nodiscard]] void* try_allocate(usize size, usize alignment) noexcept override
        {
            return allocate(size, alignment);
        }

    private:
        u32 m_grants;
    };
    for (const u32 grants : {0b00U, 0b01U, 0b10U})
    {
        Refuses allocator(grants);
        BaselineStore store(allocator, 4U, 100U);
        CHECK_FALSE(store.ready());
        CHECK_FALSE(store.put(1U, ConstSpan<u8>{}));
        ConstSpan<u8> view;
        CHECK_FALSE(store.find(1U, view));
    }
    CountingAllocator counting;
    BaselineStore zero_slots(counting, 0U, 100U);
    BaselineStore zero_room(counting, 4U, 0U);
    CHECK_FALSE(zero_slots.ready());
    CHECK_FALSE(zero_room.ready());
    CHECK(counting.calls() == 0U);

    Refuses allocator(0U);
    BaselineStore reader_store(counting, 4U, 100U);
    DeltaReader reader(kSnapshotLayout, reader_store, allocator);
    CHECK_FALSE(reader.ready());
}

TEST_CASE("Sequences expand around the one expected", "[net][delta]")
{
    CHECK(delta_expand_sequence(0x10000U, 0x0000U) == 0x10000U);
    CHECK(delta_expand_sequence(0x10000U, 0xFFFFU) == 0xFFFFU);  // just behind
    CHECK(delta_expand_sequence(0x1FFFFU, 0x0000U) == 0x20000U); // just ahead, across the wrap
    CHECK(delta_expand_sequence(0x18000U, 0x0000U) == 0x20000U); // exactly half behind goes ahead
    CHECK(delta_expand_sequence(0x18000U, 0x8000U) == 0x18000U);
    CHECK(delta_expand_sequence(0x10001U, 0x8002U) == 0x8002U);               // more than half ahead goes behind
    CHECK(delta_expand_sequence(0x10001U, 0x8001U) == 0x18001U);              // exactly half ahead stays
    CHECK(delta_expand_sequence(5U, 0xFFFFU) == 0xFFFFU);                     // nothing lies behind zero
    CHECK(delta_expand_sequence(~u64{0U}, 0x0000U) == 0xFFFFFFFFFFFF0000ULL); // nothing lies past the top
}

namespace
{
struct Pair
{
    CountingAllocator allocator;
    BaselineStore sender_store{allocator, 32U, 64U * 1024U};
    BaselineStore reader_store{allocator, 32U, 64U * 1024U};
    DeltaWriter writer{kSnapshotLayout, sender_store};
    DeltaReader reader{kSnapshotLayout, reader_store, allocator};
    Bytes frame = sized(16U * 1024U);

    ConstSpan<u8> send(u64 sequence, const Bytes& message)
    {
        REQUIRE(sender_store.put(sequence, message));
        usize size = 0U;
        REQUIRE(writer.write(sequence, kMinor, Span<u8>(frame), size) == WireError::Ok);
        return ConstSpan<u8>(frame.data(), size);
    }
};

Bytes world(u64 tick, u32 count, u32 step)
{
    cont::Array<Entity> entities;
    for (u32 index = 0U; index < count; ++index)
    {
        Entity one = entity(static_cast<u16>(index * 3U));
        one.pose.x = static_cast<decltype(one.pose.x)>((index * 7U) + ((step * (index + 1U)) % 13U));
        one.pose.yaw = static_cast<u16>((step + index) % 1024U);
        one.seen = ((step / 8U) & 1U) != 0U;
        entities.push_back(one);
    }
    return encoded(snapshot_of(ConstSpan<Entity>(entities), tick));
}
} // namespace

TEST_CASE("A writer sends the whole until acknowledged, then only what changed", "[net][delta]")
{
    Pair pair;
    REQUIRE(pair.reader.ready());
    const usize calls = pair.allocator.calls();
    ConstSpan<u8> message;
    WireError error = WireError::Ok;

    const Bytes first = world(1U, 8U, 0U);
    ConstSpan<u8> body = pair.send(1000U, first);
    CHECK(body.size() == first.size() + kDeltaHeaderMinBytes);
    CHECK(body[0] == 0x03U); // 1000's low 16 bits, big-endian
    CHECK(body[1] == 0xE8U);
    CHECK(body[2] == 0U); // distance zero: the whole message
    const u32 hash = static_cast<u32>(crd::containers::xxh3_64(first));
    CHECK(body[3] == static_cast<u8>(hash >> 24U));
    CHECK(body[6] == static_cast<u8>(hash));
    REQUIRE(pair.reader.read(body, kMinor, message, error) == DeltaRead::Delivered);
    CHECK(Bytes(message.begin(), message.end()) == first);
    CHECK(pair.reader.has_ack());
    CHECK(pair.reader.ack() == 1000U);

    // Before any acknowledgement, whole again.
    const Bytes second = world(2U, 8U, 1U);
    body = pair.send(1001U, second);
    CHECK(pair.writer.whole_frames() == 2U);
    REQUIRE(pair.reader.read(body, kMinor, message, error) == DeltaRead::Delivered);

    pair.writer.on_ack(pair.reader.ack());
    const Bytes third = world(3U, 8U, 2U);
    body = pair.send(1002U, third);
    CHECK(pair.writer.delta_frames() == 1U);
    CHECK(body.size() < third.size() + kDeltaHeaderMinBytes);
    CHECK(body[2] == 1U);
    REQUIRE(pair.reader.read(body, kMinor, message, error) == DeltaRead::Delivered);
    CHECK(Bytes(message.begin(), message.end()) == third);
    Snapshot decoded{};
    CHECK(decode(message, kMinor, decoded) == WireError::Ok);
    CHECK(decoded.tick == 3U);
    CHECK(pair.reader.count(DeltaRead::Delivered) == 3U);

    // A repeat changes nothing.
    CHECK(pair.reader.read(body, kMinor, message, error) == DeltaRead::Stale);
    CHECK(pair.reader.count(DeltaRead::Stale) == 1U);
    CHECK(pair.allocator.calls() == calls);
}

TEST_CASE("A delta no smaller than its message goes whole", "[net][delta]")
{
    // Every field changes by much: the groups of x's difference and the changed bits lose to the fields' own widths.
    CountingAllocator allocator;
    BaselineStore sender_store(allocator, 8U, 1024U);
    BaselineStore reader_store(allocator, 8U, 1024U);
    DeltaWriter writer(kMotionLayout, sender_store);
    DeltaReader reader(kMotionLayout, reader_store, allocator);
    Motion from{};
    from.x = -90.0F;
    from.height = 1.0F;
    Motion to{};
    to.x = 70.0F;
    to.height = 9.0F;
    to.turn.xyzw[0] = 0.5F;
    to.turn.xyzw[1] = 0.5F;
    to.turn.xyzw[2] = 0.5F;
    to.turn.xyzw[3] = 0.5F;
    const Bytes first = encoded(from);
    const Bytes second = encoded(to);
    CHECK(delta_of(kMotionLayout, first, second).size() >= second.size());

    Bytes frame = sized(64U);
    ConstSpan<u8> message;
    WireError error = WireError::Ok;
    usize size = 0U;
    REQUIRE(sender_store.put(1U, first));
    REQUIRE(writer.write(1U, kMinor, Span<u8>(frame), size) == WireError::Ok);
    REQUIRE(reader.read(ConstSpan<u8>(frame.data(), size), kMinor, message, error) == DeltaRead::Delivered);
    writer.on_ack(reader.ack());
    REQUIRE(sender_store.put(2U, second));
    REQUIRE(writer.write(2U, kMinor, Span<u8>(frame), size) == WireError::Ok);
    CHECK(writer.whole_frames() == 2U);
    CHECK(frame[2] == 0U);
    REQUIRE(reader.read(ConstSpan<u8>(frame.data(), size), kMinor, message, error) == DeltaRead::Delivered);
    CHECK(Bytes(message.begin(), message.end()) == second);

    // Against a baseline near it, the same writer sends a delta.
    Motion near_it = to;
    near_it.x = 70.01F;
    REQUIRE(sender_store.put(3U, encoded(near_it)));
    writer.on_ack(reader.ack());
    REQUIRE(writer.write(3U, kMinor, Span<u8>(frame), size) == WireError::Ok);
    CHECK(writer.delta_frames() == 1U);
    REQUIRE(reader.read(ConstSpan<u8>(frame.data(), size), kMinor, message, error) == DeltaRead::Delivered);
    CHECK(Bytes(message.begin(), message.end()) == encoded(near_it));
}

TEST_CASE("A writer's refusals and what an acknowledgement cannot do", "[net][delta]")
{
    Pair pair;
    const Bytes first = world(1U, 4U, 0U);
    usize size = 0U;
    // Nothing kept under that sequence.
    CHECK(pair.writer.write(5U, kMinor, Span<u8>(pair.frame), size) == WireError::OutOfRange);
    REQUIRE(pair.sender_store.put(5U, first));
    u8 tiny[8] = {};
    CHECK(pair.writer.write(5U, kMinor, Span<u8>(tiny), size) == WireError::NoRoom);
    Bytes header_only = sized(kDeltaHeaderMaxBytes + 1U);
    CHECK(pair.writer.write(5U, kMinor, Span<u8>(header_only), size) == WireError::NoRoom);
    CHECK(size == 0U);
    REQUIRE(pair.writer.write(5U, kMinor, Span<u8>(pair.frame), size) == WireError::Ok);
    // A sequence must rise.
    CHECK(pair.writer.write(5U, kMinor, Span<u8>(pair.frame), size) == WireError::OutOfRange);

    // An acknowledgement of something never written is ignored, and so is an older one.
    pair.writer.on_ack(6U);
    REQUIRE(pair.sender_store.put(6U, first));
    REQUIRE(pair.writer.write(6U, kMinor, Span<u8>(pair.frame), size) == WireError::Ok);
    CHECK(pair.writer.whole_frames() == 2U);
    pair.writer.on_ack(6U);
    pair.writer.on_ack(5U);
    const Bytes next = world(2U, 4U, 1U);
    const ConstSpan<u8> body = pair.send(7U, next);
    CHECK(pair.writer.delta_frames() == 1U);
    CHECK(body[2] == 1U); // against 6, not 5

    // A baseline the store has given up: the whole message.
    Pair evicted;
    REQUIRE(evicted.sender_store.put(100U, first));
    REQUIRE(evicted.writer.write(100U, kMinor, Span<u8>(evicted.frame), size) == WireError::Ok);
    evicted.writer.on_ack(100U);
    for (u64 sequence = 101U; sequence < 140U; ++sequence)
    {
        static_cast<void>(evicted.send(sequence, first));
    }
    CHECK(evicted.writer.delta_frames() == 31U); // within the store's 32 slots
    CHECK(evicted.writer.whole_frames() == 9U);
    // A writer past the top of a key's order: a bad message surfaces, the writer's state stays.
    DeltaWriter fresh(kSnapshotLayout, evicted.sender_store);
    CHECK(fresh.write(139U, kMinor, Span<u8>(evicted.frame), size) == WireError::Ok);
    fresh.on_ack(139U);
}

TEST_CASE("A writer's choices at their boundaries", "[net][delta]")
{
    CountingAllocator allocator;
    BaselineStore store(allocator, 7U, 4096U);
    DeltaWriter writer(kMotionLayout, store);
    Motion base{};
    base.x = 0.0F;
    base.height = 1.0F;
    Motion near_it = base; // x's code moves within one 3-bit group, and turn and height both change
    near_it.x = 0.005F;
    near_it.height = 9.0F;
    near_it.turn.xyzw[0] = 0.5F;
    near_it.turn.xyzw[1] = 0.5F;
    near_it.turn.xyzw[2] = 0.5F;
    near_it.turn.xyzw[3] = 0.5F;
    Motion further = near_it; // within two groups: the delta then takes all the message does
    further.x = 0.05F;
    const Bytes baseline = encoded(base);
    REQUIRE(baseline.size() == 7U);
    REQUIRE(delta_of(kMotionLayout, baseline, encoded(near_it)).size() == 6U);
    REQUIRE(delta_of(kMotionLayout, baseline, encoded(further)).size() == 7U);

    Bytes frame = sized(64U);
    usize size = 0U;
    REQUIRE(store.put(1U, baseline));
    REQUIRE(writer.write(1U, kMinor, Span<u8>(frame), size) == WireError::Ok);
    writer.on_ack(1U);

    SECTION("One byte smaller goes as a delta, the same size goes whole")
    {
        REQUIRE(store.put(2U, encoded(near_it)));
        REQUIRE(writer.write(2U, kMinor, Span<u8>(frame), size) == WireError::Ok);
        CHECK(writer.delta_frames() == 1U);
        CHECK(size == kDeltaHeaderMinBytes + 6U);
        // A sequence must rise after a delta too.
        CHECK(writer.write(2U, kMinor, Span<u8>(frame), size) == WireError::OutOfRange);
        REQUIRE(store.put(3U, encoded(further)));
        REQUIRE(writer.write(3U, kMinor, Span<u8>(frame), size) == WireError::Ok);
        CHECK(writer.whole_frames() == 2U);
        CHECK(size == kDeltaHeaderMinBytes + 7U);
    }

    SECTION("A delta that does not fit where the whole does goes whole")
    {
        Motion all_of_it = near_it;
        all_of_it.x = 90.0F;
        REQUIRE(delta_of(kMotionLayout, baseline, encoded(all_of_it)).size() > 7U);
        REQUIRE(store.put(2U, encoded(all_of_it)));
        Bytes exact = sized(kDeltaHeaderMinBytes + 7U + 2U); // the header at its most, and the whole
        REQUIRE(writer.write(2U, kMinor, Span<u8>(exact), size) == WireError::Ok);
        CHECK(writer.whole_frames() == 2U);
    }

    SECTION("A distance of a whole span cannot be said: the whole goes, one less is a delta")
    {
        REQUIRE(store.put(65536U, encoded(near_it)));
        REQUIRE(writer.write(65536U, kMinor, Span<u8>(frame), size) == WireError::Ok);
        CHECK(writer.delta_frames() == 1U);
        CHECK(frame[2] == 0xFFU); // 65535 in three groups
        CHECK(frame[3] == 0xFFU);
        CHECK(frame[4] == 0x03U);
        REQUIRE(store.put(65537U, encoded(near_it)));
        REQUIRE(writer.write(65537U, kMinor, Span<u8>(frame), size) == WireError::Ok);
        CHECK(writer.whole_frames() == 2U);
        CHECK(frame[2] == 0U);
    }

    SECTION("A baseline that is not a message surfaces its error, and no frame")
    {
        BaselineStore wrong(allocator, 5U, 4096U);
        DeltaWriter other(kSnapshotLayout, wrong);
        const Bytes nothing;
        REQUIRE(wrong.put(1U, nothing));
        REQUIRE(other.write(1U, kMinor, Span<u8>(frame), size) == WireError::Ok);
        other.on_ack(1U);
        REQUIRE(wrong.put(2U, world(1U, 2U, 0U)));
        size = 99U;
        CHECK(other.write(2U, kMinor, Span<u8>(frame), size) == WireError::Truncated);
        CHECK(size == 0U);
    }
}

TEST_CASE("Room: a too small output is refused, never overrun, at every size", "[net][delta]")
{
    // Fresh vectors of exactly each size, so that the address sanitizer lanes see a write one past.
    const Bytes old_bytes = world(1U, 6U, 0U);
    const Bytes new_bytes = [&]
    {
        Snapshot next = snapshot_of({}, 2U);
        next.header.label = "a longer label";
        next.header.nested.depth = 4U;
        return encoded(next);
    }();
    const Bytes delta = delta_of(kSnapshotLayout, old_bytes, new_bytes);
    for (usize room = 0U; room <= delta.size() + kDeltaSlackBytes; ++room)
    {
        Bytes out = sized(room);
        usize size = 123U;
        const WireError error = delta_encode(kSnapshotLayout, old_bytes, new_bytes, kMinor, Span<u8>(out), size);
        if (error == WireError::Ok)
        {
            CHECK(Bytes(out.begin(), out.begin() + static_cast<std::ptrdiff_t>(size)) == delta);
        }
        else
        {
            CHECK(error == WireError::NoRoom);
            CHECK(size == 0U);
        }
    }
    for (usize room = 0U; room <= new_bytes.size() + kDeltaSlackBytes; ++room)
    {
        Bytes out = sized(room);
        usize size = 123U;
        const WireError error = delta_apply(kSnapshotLayout, old_bytes, delta, kMinor, Span<u8>(out), size);
        if (error == WireError::Ok)
        {
            CHECK(Bytes(out.begin(), out.begin() + static_cast<std::ptrdiff_t>(size)) == new_bytes);
        }
        else
        {
            CHECK(error == WireError::NoRoom);
            CHECK(size == 0U);
        }
    }

    CountingAllocator allocator;
    BaselineStore store(allocator, 8U, 8192U);
    DeltaWriter writer(kSnapshotLayout, store);
    REQUIRE(store.put(1U, old_bytes));
    Bytes large = sized(4096U);
    usize written = 0U;
    REQUIRE(writer.write(1U, kMinor, Span<u8>(large), written) == WireError::Ok);
    writer.on_ack(1U);
    REQUIRE(store.put(2U, new_bytes));
    u64 sequence = 2U;
    for (usize room = 0U; room <= new_bytes.size() + kDeltaHeaderMaxBytes; ++room)
    {
        Bytes out = sized(room);
        usize size = 123U;
        const WireError error = writer.write(sequence, kMinor, Span<u8>(out), size);
        if (error == WireError::Ok)
        {
            CHECK(size <= room);
            CHECK(room >= kDeltaHeaderMaxBytes);
            ++sequence;
            REQUIRE(store.put(sequence, new_bytes));
        }
        else
        {
            CHECK(error == WireError::NoRoom);
            CHECK(size == 0U);
        }
    }
    // A whole message that exactly fits; one byte less does not.
    DeltaWriter fresh(kSnapshotLayout, store);
    Bytes tight = sized(kDeltaHeaderMinBytes + new_bytes.size());
    CHECK(fresh.write(sequence, kMinor, Span<u8>(tight), written) ==
          ((tight.size() >= kDeltaHeaderMaxBytes) ? WireError::Ok : WireError::NoRoom));
    CHECK(written == tight.size());
    DeltaWriter short_of_one(kSnapshotLayout, store);
    Bytes less = sized(kDeltaHeaderMinBytes + new_bytes.size() - 1U);
    CHECK(short_of_one.write(sequence, kMinor, Span<u8>(less), written) == WireError::NoRoom);
    CHECK(written == 0U);
}

TEST_CASE("The fold's far ends travel, and a reader rebuilds the largest message", "[net][delta]")
{
    // tick from 0 to half the range folds to all ones; a varint's most groups.
    Snapshot one = snapshot_of({}, 0U);
    Snapshot other = snapshot_of({}, u64{1U} << 63U);
    const Bytes old_bytes = encoded(one);
    const Bytes new_bytes = encoded(other);
    Bytes rebuilt;
    REQUIRE(apply_to(kSnapshotLayout, old_bytes, delta_of(kSnapshotLayout, old_bytes, new_bytes), rebuilt) ==
            WireError::Ok);
    CHECK(rebuilt == new_bytes);
    REQUIRE(apply_to(kSnapshotLayout, new_bytes, delta_of(kSnapshotLayout, new_bytes, old_bytes), rebuilt) ==
            WireError::Ok);
    CHECK(rebuilt == old_bytes);

    static constexpr u8 kLong[12] = {9U, 9U, 9U, 9U, 9U, 9U, 9U, 9U, 9U, 9U, 9U, 9U};
    static constexpr u8 kBag[5] = {1U, 2U, 3U, 4U, 5U};
    cont::Array<Entity> entities;
    for (u16 index = 0U; index < 8U; ++index)
    {
        Entity big = entity(index);
        big.blob = ConstSpan<u8>(kLong, 8U);
        big.tag = "tag of twelve";
        big.tag = big.tag.substr(0U, 12U);
        big.has_tag = true;
        big.bag = WireList<EntityBagItemCodec>(ConstSpan<u8>(kBag));
        big.has_back = true;
        const auto widest = [](Pose& pose)
        {
            pose.z = -100000;
            pose.speed = 1000000U;
            pose.wide = ~u64{0U};
            pose.far = -9223372036854775807LL - 1LL;
            pose.plain_count = 5000U;
            pose.plain_turn = -5000;
        };
        widest(big.pose);
        widest(big.back);
        entities.push_back(big);
    }
    Snapshot largest = snapshot_of(entities, ~u64{0U});
    largest.header.label = "twenty characters...";
    largest.header.nested.note = ConstSpan<u8>(kLong);
    largest.header.more = ConstSpan<u8>(kLong, 9U);
    largest.has_shift = true;
    largest.shift.z = -100000;
    largest.shift.speed = 1000000U;
    largest.shift.wide = ~u64{0U};
    largest.shift.far = -9223372036854775807LL - 1LL;
    largest.shift.plain_count = 5000U;
    largest.shift.plain_turn = -5000;
    largest.tail = ConstSpan<u8>(kLong, 4U);
    const Bytes at_most = [&]
    {
        Bytes out = sized(SnapshotCodec::kMaxBytes + 16U);
        usize size = 0U;
        REQUIRE(encode(largest, kMinor, Span<u8>(out), size) == WireError::Ok);
        out.resize(size);
        return out;
    }();
    REQUIRE(at_most.size() + kDeltaSlackBytes > SnapshotCodec::kMaxBytes);

    CountingAllocator allocator;
    BaselineStore sender_store(allocator, 8U, 64U * 1024U);
    BaselineStore reader_store(allocator, 8U, 64U * 1024U);
    DeltaWriter writer(kSnapshotLayout, sender_store);
    DeltaReader reader(kSnapshotLayout, reader_store, allocator);
    Bytes frame = sized(SnapshotCodec::kMaxBytes + kDeltaHeaderMaxBytes);
    usize size = 0U;
    ConstSpan<u8> message;
    WireError error = WireError::Ok;
    REQUIRE(sender_store.put(1U, at_most));
    REQUIRE(writer.write(1U, kMinor, Span<u8>(frame), size) == WireError::Ok);
    REQUIRE(reader.read(ConstSpan<u8>(frame.data(), size), kMinor, message, error) == DeltaRead::Delivered);
    writer.on_ack(reader.ack());
    largest.tick = ~u64{0U} - 1U;
    usize next_size = 0U;
    Bytes next = sized(SnapshotCodec::kMaxBytes + 16U);
    REQUIRE(encode(largest, kMinor, Span<u8>(next), next_size) == WireError::Ok);
    next.resize(next_size);
    REQUIRE(sender_store.put(2U, next));
    REQUIRE(writer.write(2U, kMinor, Span<u8>(frame), size) == WireError::Ok);
    CHECK(writer.delta_frames() == 1U);
    REQUIRE(reader.read(ConstSpan<u8>(frame.data(), size), kMinor, message, error) == DeltaRead::Delivered);
    CHECK(Bytes(message.begin(), message.end()) == next);
}

TEST_CASE("A writer does not give up on a delta a little smaller than its message", "[net][delta]")
{
    // More fields changed brings the delta closer to the message; one count lands within the writer's slack of it.
    static constexpr u8 kFixedOther[3] = {7U, 8U, 9U};
    static constexpr u8 kBlobFull[8] = {1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U};
    bool landed = false;
    for (u32 changed = 1U; changed <= 8U * 14U && !landed; ++changed)
    {
        cont::Array<Entity> before;
        for (u16 id = 0U; id < 8U; ++id)
        {
            before.push_back(entity(id));
        }
        cont::Array<Entity> after = before;
        for (u32 at = 0U; at < changed; ++at)
        {
            Entity& one = after[at / 14U];
            switch (at % 14U)
            {
                case 0U:
                    one.pose.x = 30000;
                    break;
                case 1U:
                    one.pose.y = 900;
                    break;
                case 2U:
                    one.pose.z = 99999;
                    break;
                case 3U:
                    one.pose.yaw = 1000U;
                    break;
                case 4U:
                    one.pose.speed = 900000U;
                    break;
                case 5U:
                    one.pose.wide = 0x7000000000000000ULL;
                    break;
                case 6U:
                    one.pose.far = 0x7000000000000000LL;
                    break;
                case 7U:
                    one.pose.plain_count = 5000U;
                    break;
                case 8U:
                    one.pose.plain_turn = 5000;
                    break;
                case 9U:
                    one.weight = 1.5F;
                    break;
                case 10U:
                    one.fixed = ConstSpan<u8>(kFixedOther);
                    break;
                case 11U:
                    one.blob = ConstSpan<u8>(kBlobFull);
                    break;
                case 12U:
                    one.seen = true;
                    break;
                default:
                    one.stance = Stance::Run;
                    break;
            }
        }
        const Bytes first = encoded(snapshot_of(before));
        const Bytes second = encoded(snapshot_of(after));
        const usize delta = delta_of(kSnapshotLayout, first, second).size();
        if (delta >= second.size() || second.size() - delta >= kDeltaSlackBytes)
        {
            continue;
        }
        landed = true;
        CountingAllocator allocator;
        BaselineStore store(allocator, 8U, 8192U);
        DeltaWriter writer(kSnapshotLayout, store);
        Bytes frame = sized(4096U);
        usize size = 0U;
        REQUIRE(store.put(1U, first));
        REQUIRE(writer.write(1U, kMinor, Span<u8>(frame), size) == WireError::Ok);
        writer.on_ack(1U);
        REQUIRE(store.put(2U, second));
        REQUIRE(writer.write(2U, kMinor, Span<u8>(frame), size) == WireError::Ok);
        CHECK(writer.delta_frames() == 1U);
        CHECK(size == kDeltaHeaderMinBytes + delta);
    }
    CHECK(landed);
}

TEST_CASE("A hostile merge after each way of passing a key", "[net][delta]")
{
    RosterOf roster;
    roster.seats = {seat(-3, false), seat(4, true), seat(9, false)};
    const Bytes baseline = encoded(roster.get());
    Bytes out;
    // Keep -3 (code 47), then an insertion of the same key, or one below it.
    for (const u64 code : {u64{47U}, u64{46U}})
    {
        CHECK(apply_to(kRosterLayout, baseline,
                       bits_of({{0U, 1U}, {1U, 1U}, {4U, 3U}, {0U, 1U}, {0b110U, 3U}, {code, 7U}, {0U, 1U}}),
                       out) == WireError::KeyOrder);
    }
    // Change -3, then the same.
    for (const u64 code : {u64{47U}, u64{46U}})
    {
        CHECK(
            apply_to(kRosterLayout, baseline,
                     bits_of({{0U, 1U}, {1U, 1U}, {4U, 3U}, {0b10U, 2U}, {1U, 1U}, {0b110U, 3U}, {code, 7U}, {0U, 1U}}),
                     out) == WireError::KeyOrder);
    }
    // A legal one with each of the same shapes, for contrast: an insertion between -3 and 4.
    CHECK(apply_to(kRosterLayout, baseline,
                   bits_of({{0U, 1U},
                            {1U, 1U},
                            {4U, 3U},
                            {0U, 1U},
                            {0b110U, 3U},
                            {48U, 7U},
                            {0U, 1U},
                            {0U, 1U},
                            {0U, 1U},
                            {0U, 2U}}),
                   out) == WireError::Ok);
    CHECK(kRosterLayout.validate(out, kMinor) == WireError::Ok);
}

TEST_CASE("A reader's sequences at half a span, and its distance's bound", "[net][delta]")
{
    CountingAllocator allocator;
    BaselineStore store(allocator, 8U, 4096U);
    const auto fresh_body = [&](u64 sequence, const Bytes& whole, u32 distance)
    {
        Bytes body = sized(whole.size() + kDeltaHeaderMaxBytes);
        BitWriter w{Span<u8>(body)};
        w.put(sequence & 0xFFFFU, 16U);
        wire_put_varuint(w, distance, 7U);
        w.put(static_cast<u32>(crd::containers::xxh3_64(whole)), 32U);
        w.put_bytes(whole);
        body.resize(w.finish());
        return body;
    };
    DeltaReader reader(kMotionLayout, store, allocator);
    Motion motion{};
    const Bytes message_bytes = encoded(motion);
    ConstSpan<u8> message;
    WireError error = WireError::Ok;
    REQUIRE(reader.read(fresh_body(5U, message_bytes, 0U), kMinor, message, error) == DeltaRead::Delivered);
    // The next expected is 6. Half a span past it still lies ahead; one more lies behind.
    CHECK(reader.read(fresh_body(6U + 0x8001U, message_bytes, 0U), kMinor, message, error) == DeltaRead::Stale);
    REQUIRE(reader.read(fresh_body(6U + 0x8000U, message_bytes, 0U), kMinor, message, error) == DeltaRead::Delivered);
    CHECK(reader.ack() == static_cast<u16>(6U + 0x8000U));
    // The largest distance can be said, and has no baseline here; one more is refused.
    CHECK(reader.read(fresh_body(0x8007U, message_bytes, 65535U), kMinor, message, error) ==
          DeltaRead::MissingBaseline);
    CHECK(reader.read(fresh_body(0x8007U, message_bytes, 65536U), kMinor, message, error) == DeltaRead::Malformed);
    CHECK(error == WireError::OutOfRange);
}

TEST_CASE("One store serves many writers, each from what its peer acknowledged", "[net][delta]")
{
    CountingAllocator allocator;
    BaselineStore shared(allocator, 32U, 64U * 1024U);
    DeltaWriter near(kSnapshotLayout, shared);
    DeltaWriter far(kSnapshotLayout, shared);
    BaselineStore near_store(allocator, 32U, 64U * 1024U);
    BaselineStore far_store(allocator, 32U, 64U * 1024U);
    DeltaReader near_reader(kSnapshotLayout, near_store, allocator);
    DeltaReader far_reader(kSnapshotLayout, far_store, allocator);
    Bytes frame = sized(16U * 1024U);
    ConstSpan<u8> message;
    WireError error = WireError::Ok;
    for (u64 tick = 1U; tick <= 20U; ++tick)
    {
        const Bytes current = world(tick, 6U, static_cast<u32>(tick));
        REQUIRE(shared.put(tick, current));
        usize size = 0U;
        REQUIRE(near.write(tick, kMinor, Span<u8>(frame), size) == WireError::Ok);
        REQUIRE(near_reader.read(ConstSpan<u8>(frame.data(), size), kMinor, message, error) == DeltaRead::Delivered);
        CHECK(Bytes(message.begin(), message.end()) == current);
        near.on_ack(near_reader.ack());
        if (tick % 5U == 0U || tick == 1U) // the far peer hears one tick in five
        {
            REQUIRE(far.write(tick, kMinor, Span<u8>(frame), size) == WireError::Ok);
            REQUIRE(far_reader.read(ConstSpan<u8>(frame.data(), size), kMinor, message, error) == DeltaRead::Delivered);
            CHECK(Bytes(message.begin(), message.end()) == current);
            far.on_ack(far_reader.ack());
        }
    }
    CHECK(near.delta_frames() == 19U);
    CHECK(far.delta_frames() == 4U);
}

TEST_CASE("A reader refuses what it cannot rebuild and keeps its baselines", "[net][delta]")
{
    Pair pair;
    ConstSpan<u8> message;
    WireError error = WireError::Ok;
    const Bytes first = world(1U, 4U, 0U);
    const Bytes first_body = [&]
    {
        const ConstSpan<u8> body = pair.send(70000U, first);
        return Bytes(body.begin(), body.end());
    }();

    // Nothing to rebuild against yet.
    pair.writer.on_ack(static_cast<u16>(70000U));
    const Bytes second = world(2U, 4U, 1U);
    const Bytes second_body = [&]
    {
        const ConstSpan<u8> body = pair.send(70001U, second);
        return Bytes(body.begin(), body.end());
    }();
    CHECK(second_body[2] == 1U);
    CHECK(pair.reader.read(second_body, kMinor, message, error) == DeltaRead::MissingBaseline);
    CHECK_FALSE(pair.reader.has_ack());

    REQUIRE(pair.reader.read(first_body, kMinor, message, error) == DeltaRead::Delivered);

    SECTION("Each field of the header")
    {
        for (usize size = 0U; size < 7U; ++size)
        {
            CHECK(pair.reader.read(ConstSpan<u8>(second_body.data(), size), kMinor, message, error) ==
                  DeltaRead::Malformed);
            CHECK(error == WireError::Truncated);
        }
        Bytes overlong = second_body;
        overlong[2] = 0x81U; // a continuation, then a zero group: an overlong 1
        overlong.insert(3U, 0x00U);
        CHECK(pair.reader.read(overlong, kMinor, message, error) == DeltaRead::Malformed);
        CHECK(error == WireError::Overlong);
    }

    SECTION("A hash that does not match keeps nothing, and the good one still reads")
    {
        Bytes wrong_hash = second_body;
        wrong_hash[3] ^= 1U;
        CHECK(pair.reader.read(wrong_hash, kMinor, message, error) == DeltaRead::Mismatch);
        CHECK(pair.reader.count(DeltaRead::Mismatch) == 1U);
        CHECK(pair.reader.ack() == static_cast<u16>(70000U));
        REQUIRE(pair.reader.read(second_body, kMinor, message, error) == DeltaRead::Delivered);
        CHECK(Bytes(message.begin(), message.end()) == second);
    }

    SECTION("A whole message that its hash does not cover")
    {
        Bytes whole = first_body;
        whole[0] = second_body[0];
        whole[1] = second_body[1];
        whole.back() ^= 1U; // a trailing padding or field bit
        const DeltaRead verdict = pair.reader.read(whole, kMinor, message, error);
        CHECK((verdict == DeltaRead::Mismatch || verdict == DeltaRead::Malformed));
    }

    SECTION("A delta rebuilding what its decoder refuses")
    {
        Bytes damaged = second_body;
        damaged.resize(7U);
        damaged.push_back(0xFFU);
        CHECK(pair.reader.read(damaged, kMinor, message, error) == DeltaRead::Malformed);
        CHECK(error != WireError::Ok);
    }

    SECTION("An old sequence, overtaken")
    {
        const Bytes third = world(3U, 4U, 2U);
        REQUIRE(pair.sender_store.put(70002U, third));
        usize size = 0U;
        Bytes third_body = sized(16U * 1024U);
        REQUIRE(pair.writer.write(70002U, kMinor, Span<u8>(third_body), size) == WireError::Ok);
        third_body.resize(size);
        REQUIRE(pair.reader.read(third_body, kMinor, message, error) == DeltaRead::Delivered);
        CHECK(pair.reader.read(second_body, kMinor, message, error) == DeltaRead::Stale);
    }
}

TEST_CASE("A reader's store that cannot keep a message says so", "[net][delta]")
{
    CountingAllocator allocator;
    BaselineStore store(allocator, 2U, 8U);
    DeltaReader reader(kSnapshotLayout, store, allocator);
    Pair sender;
    const Bytes first = world(1U, 4U, 0U);
    const ConstSpan<u8> body = sender.send(1U, first);
    ConstSpan<u8> message;
    WireError error = WireError::Ok;
    CHECK(reader.read(body, kMinor, message, error) == DeltaRead::Full);
    CHECK_FALSE(reader.has_ack());
    CHECK(reader.count(DeltaRead::Full) == 1U);
}

TEST_CASE("A reader older than its writer rebuilds what it knows", "[net][delta]")
{
    Pair pair;
    ConstSpan<u8> message;
    WireError error = WireError::Ok;
    const u16 newer = kMinor + 1U;
    const Bytes first = world(1U, 3U, 0U);
    REQUIRE(pair.sender_store.put(1U, first));
    usize size = 0U;
    REQUIRE(pair.writer.write(1U, kMinor, Span<u8>(pair.frame), size) == WireError::Ok);
    REQUIRE(pair.reader.read(ConstSpan<u8>(pair.frame.data(), size), newer, message, error) == DeltaRead::Delivered);
    pair.writer.on_ack(pair.reader.ack());
    const Bytes second = world(2U, 3U, 1U);
    REQUIRE(pair.sender_store.put(2U, second));
    REQUIRE(pair.writer.write(2U, kMinor, Span<u8>(pair.frame), size) == WireError::Ok);
    // What a newer writer appended, and a hash taken over its own whole message, which this reader cannot rebuild.
    Bytes tailed(pair.frame.begin(), pair.frame.begin() + static_cast<std::ptrdiff_t>(size));
    tailed.push_back(0xA5U);
    tailed[3] ^= 1U;
    REQUIRE(pair.reader.read(tailed, newer, message, error) == DeltaRead::Delivered);
    CHECK(Bytes(message.begin(), message.end()) == second);
}

TEST_CASE("Sequences cross the 16-bit wrap on both sides", "[net][delta]")
{
    Pair pair;
    ConstSpan<u8> message;
    WireError error = WireError::Ok;
    for (u64 sequence = 65530U; sequence < 65545U; ++sequence)
    {
        const Bytes current = world(sequence, 4U, static_cast<u32>(sequence));
        REQUIRE(pair.reader.read(pair.send(sequence, current), kMinor, message, error) == DeltaRead::Delivered);
        CHECK(Bytes(message.begin(), message.end()) == current);
        pair.writer.on_ack(pair.reader.ack());
    }
    CHECK(pair.writer.delta_frames() == 14U);
}

TEST_CASE("On a hostile path a reader delivers only what was written, and recovers", "[net][delta]")
{
    // Acknowledgements travel the same path. Whatever is lost, repeated or reordered: everything delivered is byte for
    // byte what the writer wrote at that sequence, newer than anything before it, and once the path clears deltas flow
    // again.
    for (const u32 loss_percent : {0U, 5U, 30U, 70U})
    {
        for (const u32 seed : {1U, 2U, 3U, 4U, 5U})
        {
            std::mt19937 random(seed * 1000U + loss_percent);
            Pair pair;
            const usize calls = pair.allocator.calls();
            cont::Array<Bytes> sent; // by sequence
            sent.push_back(Bytes{});
            cont::Array<Bytes> flight;
            cont::Array<u16> acks;
            ConstSpan<u8> message;
            WireError error = WireError::Ok;
            u64 newest = 0U;
            usize delivered = 0U;
            const auto chance = [&](u32 percent)
            {
                return (random() % 100U) < percent;
            };
            const auto deliver = [&](const Bytes& body)
            {
                u64 low = (static_cast<u64>(body[0]) << 8U) | body[1];
                const DeltaRead verdict = pair.reader.read(body, kMinor, message, error);
                REQUIRE((verdict == DeltaRead::Delivered || verdict == DeltaRead::Stale ||
                         verdict == DeltaRead::MissingBaseline));
                if (verdict == DeltaRead::Delivered)
                {
                    // The test's sequences stay below the wrap, so the low bits name it.
                    REQUIRE(low > newest);
                    newest = low;
                    REQUIRE(Bytes(message.begin(), message.end()) == sent[low]);
                    ++delivered;
                    acks.push_back(pair.reader.ack());
                }
            };
            for (u64 sequence = 1U; sequence <= 400U; ++sequence)
            {
                const Bytes current = world(sequence, 5U, static_cast<u32>(sequence));
                sent.push_back(current);
                const ConstSpan<u8> body = pair.send(sequence, current);
                if (!chance(loss_percent))
                {
                    flight.push_back(Bytes(body.begin(), body.end()));
                    if (chance(10U))
                    {
                        flight.push_back(Bytes(body.begin(), body.end()));
                    }
                }
                std::shuffle(flight.begin(), flight.end(), random);
                while (flight.size() > 3U)
                {
                    deliver(flight.back());
                    flight.pop_back();
                }
                if (!acks.empty() && !chance(loss_percent))
                {
                    pair.writer.on_ack(acks[random() % acks.size()]);
                    acks.clear();
                }
            }
            for (const Bytes& body : flight)
            {
                deliver(body);
            }
            flight.clear();
            CHECK(pair.allocator.calls() == calls);
            CHECK(delivered > 0U);

            // The path clears.
            const u64 deltas_before = pair.writer.delta_frames();
            for (u64 sequence = 401U; sequence <= 420U; ++sequence)
            {
                const Bytes current = world(sequence, 5U, static_cast<u32>(sequence));
                sent.push_back(current);
                const ConstSpan<u8> body = pair.send(sequence, current);
                const Bytes copy(body.begin(), body.end());
                const DeltaRead verdict = pair.reader.read(copy, kMinor, message, error);
                if (verdict == DeltaRead::Delivered)
                {
                    REQUIRE(Bytes(message.begin(), message.end()) == current);
                    pair.writer.on_ack(pair.reader.ack());
                }
            }
            CHECK(pair.reader.ack() == 420U);
            CHECK(pair.writer.delta_frames() > deltas_before);
        }
    }
}

TEST_CASE("A delta pays for a large world with one thing moving", "[net][delta]")
{
    cont::Array<Entity> entities;
    for (u16 index = 0U; index < 8U; ++index)
    {
        entities.push_back(entity(static_cast<u16>(index * 2U)));
    }
    cont::Array<Entity> moved = entities;
    moved[4].pose.x = 3;
    const Bytes old_bytes = encoded(snapshot_of(entities));
    const Bytes new_bytes = encoded(snapshot_of(moved));
    const Bytes delta = delta_of(kSnapshotLayout, old_bytes, new_bytes);
    CHECK(old_bytes.size() > 200U);
    CHECK(delta.size() <= 6U);
}
