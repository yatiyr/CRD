// crd-net-serialize-board: NET.3's serialization budget. Bytes, encode and decode nanoseconds per message and heap
// allocations of crd-net's generated codec against the best serializers of each kind, on the messages a game sends:
// a 64-entity snapshot stream, input commands and chat (board_model.hpp, serialize_board.json). Every arm starts from
// the application's plain message and ends in it, and every decode refuses malformed bytes as its library does. Each
// arm's decoded messages are held to the source (and crd-net's to serialize's, whose quantisation laws it shares), so a
// faster wrong answer fails. A second table gives crd-net's baseline deltas (S28) over the same snapshot stream, which
// no peer has.
//
// Usage: crd-net-serialize-board [--rounds N] [--quick]
//   rounds  interleaved rounds of every arm; the best of them is reported (default 25)
//   quick   one round over a short stream (the smoke CTest)

#include "generated/serialize_board.hpp"

#include "board_model.hpp"

#include <crd/containers/array.hpp>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <string_view>

namespace wire = crd::net::wire;
namespace sb = crd::net::wire::serialize_board;
using crd::u16;
using crd::u32;
using crd::u64;
using crd::u8;
using crd::usize;
using serialize_board::PeerArm;
using serialize_board::PlainChat;
using serialize_board::PlainCommand;
using serialize_board::PlainEntity;
using serialize_board::PlainSnapshot;

// Every C++ heap allocation of the process, so an arm that allocates per message shows it. crd-net's codec takes no
// allocator and calls none; this counts the peers' and proves crd-net's zero.
namespace
{
u64 g_allocations = 0U;
}

// NOLINTBEGIN(cppcoreguidelines-no-malloc,hicpp-no-malloc): the replaced global allocation functions.
void* operator new(std::size_t size)
{
    ++g_allocations;
    if (void* block = std::malloc(size == 0U ? 1U : size))
    {
        return block;
    }
    throw std::bad_alloc();
}

void operator delete(void* block) noexcept
{
    std::free(block);
}

void operator delete(void* block, std::size_t) noexcept
{
    std::free(block);
}
// NOLINTEND(cppcoreguidelines-no-malloc,hicpp-no-malloc)

namespace
{

template <typename T> using Array = crd::containers::Array<T>;
using Clock = std::chrono::steady_clock;

constexpr usize kSlotBytes = 8192U;
volatile u64 g_sink = 0U;

// ---- The world: a deterministic stream of what a server sends ----------------------------------------------------

struct Rng
{
    u64 state;

    u64 next()
    {
        state += 0x9E3779B97F4A7C15ULL;
        u64 z = state;
        z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31U);
    }

    float unit() { return static_cast<float>(next() >> 40U) * (1.0F / 16777216.0F); } // [0, 1)

    float between(float low, float high) { return low + ((high - low) * unit()); }

    u32 below(u32 bound) { return static_cast<u32>(next() % bound); }
};

void normalize(float* q)
{
    const float length = std::sqrt((q[0] * q[0]) + (q[1] * q[1]) + (q[2] * q[2]) + (q[3] * q[3]));
    for (int i = 0; i < 4; ++i)
    {
        q[i] /= length;
    }
}

float clamp(float value, float low, float high)
{
    if (value < low)
    {
        return low;
    }
    return (value > high) ? high : value;
}

void make_world(Rng& rng, PlainSnapshot& world)
{
    world.tick = 100000U;
    world.count = static_cast<u32>(serialize_board::kMaxEntities);
    for (u32 i = 0U; i < world.count; ++i)
    {
        PlainEntity& e = world.entities[i];
        e.id = static_cast<u16>((i * 16U) + rng.below(16U)); // ascending: S28's keyed list
        e.x = rng.between(-500.0F, 500.0F);
        e.y = rng.between(-20.0F, 60.0F);
        e.z = rng.between(-500.0F, 500.0F);
        for (float& component : e.turn)
        {
            component = rng.between(-1.0F, 1.0F);
        }
        normalize(e.turn);
        e.vx = rng.between(-8.0F, 8.0F);
        e.vy = 0.0F;
        e.vz = rng.between(-8.0F, 8.0F);
        e.health = static_cast<u8>(rng.below(101U));
        e.stance = static_cast<u8>(rng.below(4U));
        e.firing = rng.below(5U) == 0U;
        e.has_target = rng.below(3U) == 0U;
        e.target = e.has_target ? static_cast<u16>(rng.below(1024U)) : u16{0U};
    }
}

// One tick at 60 Hz: two in five entities move and turn, a few change speed, fire, take damage or pick a target.
void step_world(Rng& rng, PlainSnapshot& world)
{
    ++world.tick;
    for (u32 i = 0U; i < world.count; ++i)
    {
        PlainEntity& e = world.entities[i];
        if (rng.below(5U) < 2U)
        {
            e.x = clamp(e.x + (e.vx / 60.0F), -511.0F, 511.0F);
            e.z = clamp(e.z + (e.vz / 60.0F), -511.0F, 511.0F);
            for (float& component : e.turn)
            {
                component += rng.between(-0.02F, 0.02F);
            }
            normalize(e.turn);
        }
        if (rng.below(20U) == 0U)
        {
            e.vx = rng.between(-8.0F, 8.0F);
            e.vz = rng.between(-8.0F, 8.0F);
            e.stance = static_cast<u8>(rng.below(4U));
        }
        if (rng.below(10U) == 0U)
        {
            e.firing = !e.firing;
        }
        if (rng.below(50U) == 0U)
        {
            e.health = static_cast<u8>(rng.below(101U));
        }
        if (rng.below(100U) == 0U)
        {
            e.has_target = !e.has_target;
            e.target = e.has_target ? static_cast<u16>(rng.below(1024U)) : u16{0U};
        }
    }
}

void make_command(Rng& rng, u32 tick, PlainCommand& command)
{
    command.tick = tick;
    command.buttons = static_cast<u16>(rng.next() & 0x0F0FU);
    command.yaw = rng.between(-179.0F, 179.0F);
    command.pitch = rng.between(-89.0F, 89.0F);
    command.move_x = static_cast<std::int8_t>(static_cast<int>(rng.below(255U)) - 127);
    command.move_y = static_cast<std::int8_t>(static_cast<int>(rng.below(255U)) - 127);
}

void make_chat(Rng& rng, PlainChat& chat)
{
    static const char* const kWords[] = {"push", "mid",   "rotate", "nice", "shot", "need",   "heal", "on",
                                         "me",   "enemy", "flank",  "left", "the",  "bridge", "gg",   "wp"};
    chat.sender = static_cast<u16>(rng.below(1024U));
    chat.room = static_cast<u8>(rng.below(4U));
    const usize target = 8U + rng.below(150U);
    usize used = 0U;
    while (used < target)
    {
        const char* word = kWords[rng.below(16U)];
        const usize length = std::strlen(word);
        if (used + length + 1U > serialize_board::kMaxText)
        {
            break;
        }
        if (used != 0U)
        {
            chat.text[used++] = ' ';
        }
        std::memcpy(chat.text + used, word, length);
        used += length;
    }
    chat.text[used] = '\0';
    chat.text_bytes = static_cast<u8>(used);
    chat.mention_count = static_cast<u8>(rng.below(4U));
    for (u8 i = 0U; i < chat.mention_count; ++i)
    {
        chat.mentions[i] = static_cast<u16>(rng.below(1024U));
    }
}

// ---- crd-net's arm: from the plain message to the generated one, encoded; decoded and walked back ---------------

void to_wire(const PlainEntity& in, sb::Entity& out)
{
    out.id = in.id;
    out.x = in.x;
    out.y = in.y;
    out.z = in.z;
    std::memcpy(out.turn.xyzw, in.turn, sizeof(in.turn));
    out.vx = in.vx;
    out.vy = in.vy;
    out.vz = in.vz;
    out.health = in.health;
    out.stance = static_cast<sb::Stance>(in.stance);
    out.firing = in.firing;
    out.has_target = in.has_target;
    out.target = in.target;
}

void to_plain(const sb::Entity& in, PlainEntity& out)
{
    out.id = in.id;
    out.x = in.x;
    out.y = in.y;
    out.z = in.z;
    std::memcpy(out.turn, in.turn.xyzw, sizeof(out.turn));
    out.vx = in.vx;
    out.vy = in.vy;
    out.vz = in.vz;
    out.health = in.health;
    out.stance = static_cast<u8>(in.stance);
    out.firing = in.firing;
    out.has_target = in.has_target;
    out.target = in.target;
}

template <typename M> usize crd_write(const M& message, u8* out, usize capacity)
{
    usize written = 0U;
    return wire::encode(message, sb::kMinor, crd::containers::Span<u8>(out, capacity), written) == wire::WireError::Ok
               ? written
               : 0U;
}

usize crd_encode_snapshot(const PlainSnapshot& in, u8* out, usize capacity)
{
    sb::Entity entities[serialize_board::kMaxEntities];
    for (u32 i = 0U; i < in.count; ++i)
    {
        to_wire(in.entities[i], entities[i]);
    }
    sb::Snapshot message;
    message.tick = in.tick;
    message.entities = wire::WireList<sb::EntityCodec>(crd::containers::ConstSpan<sb::Entity>(entities, in.count));
    return crd_write(message, out, capacity);
}

bool crd_decode_snapshot(const u8* bytes, usize size, PlainSnapshot& out)
{
    sb::Snapshot message;
    if (wire::decode(crd::containers::ConstSpan<u8>(bytes, size), sb::kMinor, message) != wire::WireError::Ok)
    {
        return false;
    }
    out.tick = message.tick;
    out.count = 0U;
    sb::Entity entity;
    for (auto cursor = message.entities.cursor(); cursor.next(entity);)
    {
        to_plain(entity, out.entities[out.count++]);
    }
    return true;
}

usize crd_encode_command(const PlainCommand& in, u8* out, usize capacity)
{
    sb::Command message;
    message.tick = in.tick;
    message.buttons = in.buttons;
    message.yaw = in.yaw;
    message.pitch = in.pitch;
    message.move_x = in.move_x;
    message.move_y = in.move_y;
    return crd_write(message, out, capacity);
}

bool crd_decode_command(const u8* bytes, usize size, PlainCommand& out)
{
    sb::Command message;
    if (wire::decode(crd::containers::ConstSpan<u8>(bytes, size), sb::kMinor, message) != wire::WireError::Ok)
    {
        return false;
    }
    out.tick = message.tick;
    out.buttons = message.buttons;
    out.yaw = message.yaw;
    out.pitch = message.pitch;
    out.move_x = message.move_x;
    out.move_y = message.move_y;
    return true;
}

usize crd_encode_chat(const PlainChat& in, u8* out, usize capacity)
{
    sb::Chat message;
    message.sender = in.sender;
    message.room = static_cast<sb::Room>(in.room);
    message.text = std::string_view(in.text, in.text_bytes);
    message.mentions =
        wire::WireList<sb::ChatMentionsItemCodec>(crd::containers::ConstSpan<u16>(in.mentions, in.mention_count));
    return crd_write(message, out, capacity);
}

bool crd_decode_chat(const u8* bytes, usize size, PlainChat& out)
{
    sb::Chat message;
    if (wire::decode(crd::containers::ConstSpan<u8>(bytes, size), sb::kMinor, message) != wire::WireError::Ok)
    {
        return false;
    }
    out.sender = message.sender;
    out.room = static_cast<u8>(message.room);
    std::memcpy(out.text, message.text.data(), message.text.size());
    out.text[message.text.size()] = '\0';
    out.text_bytes = static_cast<u8>(message.text.size());
    out.mention_count = 0U;
    u16 mention = 0U;
    for (auto cursor = message.mentions.cursor(); cursor.next(mention);)
    {
        out.mentions[out.mention_count++] = mention;
    }
    return true;
}

const PeerArm kCrdArm{"crd-net (generated)", "bits; schema bounds, smallest three, varint tick",
                      crd_encode_snapshot,   crd_decode_snapshot,
                      crd_encode_command,    crd_decode_command,
                      crd_encode_chat,       crd_decode_chat};

// ---- Agreement: each arm's decoded message against the source (and crd-net's against serialize's) ------------------

enum class Law : u8
{
    Exact,     // natural types: every float comes back as it went
    Rounded,   // serialize's and crd-net's: a float within half a step, the smallest three within its error
    Truncated, // bitsery's value ranges: within a step
};

bool near(float got, float want, float step, float magnitude, Law law)
{
    if (law == Law::Exact)
    {
        return got == want;
    }
    // Half a step or a whole one, and the float rounding of the quantising arithmetic, which works across the range.
    const float rounding = 8.0F * std::numeric_limits<float>::epsilon() * magnitude;
    return std::fabs(got - want) <= ((law == Law::Rounded ? 0.5F : 1.0F) * step) + rounding;
}

bool same_rotation(const float* got, const float* want, Law law)
{
    if (law == Law::Exact)
    {
        return std::memcmp(got, want, 4U * sizeof(float)) == 0;
    }
    if (law == Law::Truncated)
    {
        const float step = 2.0F / 1023.0F;
        for (int i = 0; i < 4; ++i)
        {
            if (!near(got[i], want[i], step, 1.0F, law))
            {
                return false;
            }
        }
        return true;
    }
    // The smallest three may send -q for q: the same rotation.
    const float dot = (got[0] * want[0]) + (got[1] * want[1]) + (got[2] * want[2]) + (got[3] * want[3]);
    return std::fabs(dot) >= 0.999F;
}

bool same_entity(const PlainEntity& got, const PlainEntity& want, Law law)
{
    using namespace serialize_board;
    return got.id == want.id && near(got.x, want.x, kPositionResolution, kPositionMax, law) &&
           near(got.y, want.y, kPositionResolution, kPositionMax, law) &&
           near(got.z, want.z, kPositionResolution, kPositionMax, law) && same_rotation(got.turn, want.turn, law) &&
           near(got.vx, want.vx, kVelocityResolution, kVelocityMax, law) &&
           near(got.vy, want.vy, kVelocityResolution, kVelocityMax, law) &&
           near(got.vz, want.vz, kVelocityResolution, kVelocityMax, law) && got.health == want.health &&
           got.stance == want.stance && got.firing == want.firing && got.has_target == want.has_target &&
           got.target == want.target;
}

bool same(const PlainSnapshot& got, const PlainSnapshot& want, Law law)
{
    if (got.tick != want.tick || got.count != want.count)
    {
        return false;
    }
    for (u32 i = 0U; i < got.count; ++i)
    {
        const PlainEntity& g = got.entities[i];
        const PlainEntity& w = want.entities[i];
        if (!same_entity(g, w, law))
        {
            const auto d = [](float value)
            {
                return static_cast<double>(value);
            };
            std::fprintf(stderr,
                         "  entity %u: id %u/%u, position %.6f %.6f %.6f / %.6f %.6f %.6f, rotation %.5f %.5f %.5f "
                         "%.5f / %.5f %.5f %.5f %.5f, velocity %.4f %.4f %.4f / %.4f %.4f %.4f, health %u/%u, stance "
                         "%u/%u, firing %d/%d, target %d %u/%d %u\n",
                         i, g.id, w.id, d(g.x), d(g.y), d(g.z), d(w.x), d(w.y), d(w.z), d(g.turn[0]), d(g.turn[1]),
                         d(g.turn[2]), d(g.turn[3]), d(w.turn[0]), d(w.turn[1]), d(w.turn[2]), d(w.turn[3]), d(g.vx),
                         d(g.vy), d(g.vz), d(w.vx), d(w.vy), d(w.vz), g.health, w.health, g.stance, w.stance, g.firing,
                         w.firing, g.has_target, g.target, w.has_target, w.target);
            return false;
        }
    }
    return true;
}

bool same(const PlainCommand& got, const PlainCommand& want, Law law)
{
    using namespace serialize_board;
    return got.tick == want.tick && got.buttons == want.buttons &&
           near(got.yaw, want.yaw, kAngleResolution, kYawMax, law) &&
           near(got.pitch, want.pitch, kAngleResolution, kPitchMax, law) && got.move_x == want.move_x &&
           got.move_y == want.move_y;
}

bool same(const PlainChat& got, const PlainChat& want, Law)
{
    return got.sender == want.sender && got.room == want.room && got.text_bytes == want.text_bytes &&
           std::memcmp(got.text, want.text, want.text_bytes) == 0 && got.text[got.text_bytes] == '\0' &&
           got.mention_count == want.mention_count &&
           std::memcmp(got.mentions, want.mentions, want.mention_count * sizeof(u16)) == 0;
}

// Exactly the same values: crd-net against serialize, whose laws it shares bit for bit.
bool identical(const PlainSnapshot& a, const PlainSnapshot& b)
{
    if (a.tick != b.tick || a.count != b.count)
    {
        return false;
    }
    for (u32 i = 0U; i < a.count; ++i)
    {
        const PlainEntity& x = a.entities[i];
        const PlainEntity& y = b.entities[i];
        if (x.x != y.x || x.y != y.y || x.z != y.z || std::memcmp(x.turn, y.turn, sizeof(x.turn)) != 0 ||
            x.vx != y.vx || x.vy != y.vy || x.vz != y.vz || !same_entity(x, y, Law::Rounded))
        {
            return false;
        }
    }
    return true;
}

// ---- Timing ----------------------------------------------------------------------------------------------------

struct Cost
{
    double encode_ns = 1e300;
    double decode_ns = 1e300;
    u64 bytes = 0U;
    u64 allocations = 0U;
};

template <typename Work> double timed_ns(usize count, const Work& work)
{
    const Clock::time_point start = Clock::now();
    g_sink = g_sink + work();
    return std::chrono::duration<double, std::nano>(Clock::now() - start).count() / static_cast<double>(count);
}

// One round of one arm over one kind: encode every message into its slot, decode every slot.
template <typename T>
void measure(const T* messages, usize count, usize (*encode)(const T&, u8*, usize),
             bool (*decode)(const u8*, usize, T&), u8* slots, usize* sizes, T* decoded, Cost& cost, bool& failed)
{
    const u64 before = g_allocations;
    const double encode_ns = timed_ns(count,
                                      [&]
                                      {
                                          u64 total = 0U;
                                          for (usize i = 0U; i < count; ++i)
                                          {
                                              sizes[i] = encode(messages[i], slots + (i * kSlotBytes), kSlotBytes);
                                              total += sizes[i];
                                          }
                                          return total;
                                      });
    const double decode_ns = timed_ns(count,
                                      [&]
                                      {
                                          u64 good = 0U;
                                          for (usize i = 0U; i < count; ++i)
                                          {
                                              good += decode(slots + (i * kSlotBytes), sizes[i], decoded[i]) ? 1U : 0U;
                                          }
                                          return good;
                                      });
    cost.allocations = g_allocations - before;
    cost.encode_ns = encode_ns < cost.encode_ns ? encode_ns : cost.encode_ns;
    cost.decode_ns = decode_ns < cost.decode_ns ? decode_ns : cost.decode_ns;
    cost.bytes = 0U;
    for (usize i = 0U; i < count; ++i)
    {
        if (sizes[i] == 0U || !decode(slots + (i * kSlotBytes), sizes[i], decoded[i]))
        {
            failed = true;
        }
        cost.bytes += sizes[i];
    }
}

// Whether a message cut short by a byte, or followed by one, is refused (the board reports it per arm).
template <typename T>
bool refuses_damage(const u8* bytes, usize size, bool (*decode)(const u8*, usize, T&), u8* scratch, T& out)
{
    std::memcpy(scratch, bytes, size);
    scratch[size] = 0xA5U;
    return !decode(scratch, size - 1U, out) && !decode(scratch, size + 1U, out);
}

void print_costs(const char* kind, usize count, const PeerArm* const* arms, const Cost* costs, usize arm_count)
{
    const Cost& crd = costs[0];
    for (usize a = 0U; a < arm_count; ++a)
    {
        const Cost& c = costs[a];
        const double bytes = static_cast<double>(c.bytes) / static_cast<double>(count);
        std::printf("| %s | %s | %.1f | %.2fx | %.1f | %.1f | %.2fx | %.2fx | %llu |\n", kind, arms[a]->name, bytes,
                    bytes / (static_cast<double>(crd.bytes) / static_cast<double>(count)), c.encode_ns, c.decode_ns,
                    c.encode_ns / crd.encode_ns, c.decode_ns / crd.decode_ns,
                    static_cast<unsigned long long>(c.allocations));
    }
}

} // namespace

int main(int argc, char** argv)
{
    u32 rounds = 25U;
    usize ticks = 600U;
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--rounds") == 0 && i + 1 < argc)
        {
            rounds = static_cast<u32>(std::atoi(argv[++i]));
        }
        else if (std::strcmp(argv[i], "--quick") == 0)
        {
            rounds = 1U;
            ticks = 32U;
        }
        else
        {
            std::fprintf(stderr, "usage: crd-net-serialize-board [--rounds N] [--quick]\n");
            return 2;
        }
    }
    if (rounds == 0U)
    {
        rounds = 1U;
    }
    const usize commands = ticks * 2U;
    const usize chats = ticks / 4U;
    const usize most = commands;

    Array<PlainSnapshot> snapshots;
    Array<PlainCommand> command_set;
    Array<PlainChat> chat_set;
    snapshots.resize(ticks);
    command_set.resize(commands);
    chat_set.resize(chats);
    Rng rng{0x5E7A11B0A2DULL};
    make_world(rng, snapshots[0]);
    for (usize t = 1U; t < ticks; ++t)
    {
        snapshots[t] = snapshots[t - 1U];
        step_world(rng, snapshots[t]);
    }
    for (usize i = 0U; i < commands; ++i)
    {
        make_command(rng, static_cast<u32>(100000U + i), command_set[i]);
    }
    for (usize i = 0U; i < chats; ++i)
    {
        make_chat(rng, chat_set[i]);
    }

    const PeerArm* const arms[] = {&kCrdArm, &serialize_board::kSerializeArm, &serialize_board::kBitseryArm,
                                   &serialize_board::kZppBitsArm, &serialize_board::kProtozeroArm};
    const Law laws[] = {Law::Rounded, Law::Rounded, Law::Truncated, Law::Exact, Law::Exact};
    constexpr usize arm_count = sizeof(arms) / sizeof(arms[0]);

    Array<u8> slots;
    slots.resize(most * kSlotBytes);
    Array<usize> sizes;
    sizes.resize(most);
    Array<PlainSnapshot> snapshot_out[arm_count];
    Array<PlainCommand> command_out[arm_count];
    Array<PlainChat> chat_out[arm_count];
    for (usize a = 0U; a < arm_count; ++a)
    {
        snapshot_out[a].resize(ticks);
        command_out[a].resize(commands);
        chat_out[a].resize(chats);
    }
    Cost snapshot_cost[arm_count];
    Cost command_cost[arm_count];
    Cost chat_cost[arm_count];
    bool failed = false;
    bool strict[arm_count] = {};
    u8 scratch[kSlotBytes + 1U];

    // Untimed: each arm once, so a first call's one-time work (crd-net's kernel resolution and its self-check, a
    // library's tables) is no round's.
    for (usize a = 0U; a < arm_count; ++a)
    {
        const usize size = arms[a]->encode_snapshot(snapshots[0], scratch, kSlotBytes);
        g_sink = g_sink + size + (arms[a]->decode_snapshot(scratch, size, snapshot_out[a][0]) ? 1U : 0U);
    }

    for (u32 round = 0U; round < rounds; ++round)
    {
        for (usize a = 0U; a < arm_count; ++a)
        {
            const PeerArm& arm = *arms[a];
            bool arm_failed = false;
            measure(snapshots.data(), ticks, arm.encode_snapshot, arm.decode_snapshot, slots.data(), sizes.data(),
                    snapshot_out[a].data(), snapshot_cost[a], arm_failed);
            if (round == 0U)
            {
                PlainSnapshot damaged{};
                strict[a] = refuses_damage(slots.data(), sizes[0], arm.decode_snapshot, scratch, damaged);
            }
            measure(command_set.data(), commands, arm.encode_command, arm.decode_command, slots.data(), sizes.data(),
                    command_out[a].data(), command_cost[a], arm_failed);
            measure(chat_set.data(), chats, arm.encode_chat, arm.decode_chat, slots.data(), sizes.data(),
                    chat_out[a].data(), chat_cost[a], arm_failed);
            if (arm_failed)
            {
                std::fprintf(stderr, "%s: a message failed to encode or decode\n", arm.name);
                failed = true;
            }
        }
    }

    for (usize a = 0U; a < arm_count; ++a)
    {
        const char* kind = nullptr;
        usize at = 0U;
        for (usize t = 0U; kind == nullptr && t < ticks; ++t)
        {
            kind = same(snapshot_out[a][t], snapshots[t], laws[a]) ? nullptr : "snapshot";
            at = t;
        }
        for (usize i = 0U; kind == nullptr && i < commands; ++i)
        {
            kind = same(command_out[a][i], command_set[i], laws[a]) ? nullptr : "command";
            at = i;
        }
        for (usize i = 0U; kind == nullptr && i < chats; ++i)
        {
            kind = same(chat_out[a][i], chat_set[i], laws[a]) ? nullptr : "chat";
            at = i;
        }
        if (kind != nullptr)
        {
            std::fprintf(stderr, "%s: %s %zu differs from the source beyond its law\n", arms[a]->name, kind, at);
            failed = true;
        }
    }
    for (usize t = 0U; t < ticks; ++t)
    {
        if (!identical(snapshot_out[0][t], snapshot_out[1][t]))
        {
            std::fprintf(stderr, "crd-net and serialize decode tick %zu to different values\n", t);
            failed = true;
            break;
        }
    }
    if (!strict[0])
    {
        std::fprintf(stderr, "crd-net accepted a snapshot cut short or followed by a byte\n");
        failed = true;
    }
    if (snapshot_cost[0].allocations + command_cost[0].allocations + chat_cost[0].allocations != 0U)
    {
        std::fprintf(stderr, "crd-net allocated\n");
        failed = true;
    }

    std::printf("| Message | Arm | bytes | bytes / crd-net | encode ns | decode ns | encode / crd-net | "
                "decode / crd-net | allocations |\n|---|---|---|---|---|---|---|---|---|\n");
    print_costs("snapshot, 64 entities", ticks, arms, snapshot_cost, arm_count);
    print_costs("input command", commands, arms, command_cost, arm_count);
    print_costs("chat", chats, arms, chat_cost, arm_count);
    std::printf("\nArms: ");
    for (usize a = 0U; a < arm_count; ++a)
    {
        std::printf("%s%s (%s; a snapshot cut or extended by a byte: %s)", a == 0U ? "" : "; ", arms[a]->name,
                    arms[a]->format, strict[a] ? "refused" : "accepted");
    }
    std::printf("\n");

    // ---- Baseline deltas (S28) over the same stream: the newest tick acknowledged, and one six ticks old ----------
    Array<u8> whole;
    whole.resize(ticks * kSlotBytes);
    Array<usize> whole_bytes;
    whole_bytes.resize(ticks);
    for (usize t = 0U; t < ticks; ++t)
    {
        whole_bytes[t] = crd_encode_snapshot(snapshots[t], whole.data() + (t * kSlotBytes), kSlotBytes);
    }
    const usize lags[] = {1U, 6U};
    std::printf("\n| Baseline age | whole bytes | delta bytes | delta / whole | delta encode ns | delta apply ns | "
                "allocations |\n|---|---|---|---|---|---|---|\n");
    for (const usize lag : lags)
    {
        double best_encode = 1e300;
        double best_apply = 1e300;
        u64 delta_total = 0U;
        u64 whole_total = 0U;
        u64 allocations = 0U;
        for (u32 round = 0U; round < rounds; ++round)
        {
            const u64 before = g_allocations;
            const usize count = ticks - lag;
            delta_total = 0U;
            whole_total = 0U;
            const double encode_ns = timed_ns(
                count,
                [&]
                {
                    u64 total = 0U;
                    for (usize t = lag; t < ticks; ++t)
                    {
                        const crd::containers::ConstSpan<u8> baseline(whole.data() + ((t - lag) * kSlotBytes),
                                                                      whole_bytes[t - lag]);
                        const crd::containers::ConstSpan<u8> current(whole.data() + (t * kSlotBytes), whole_bytes[t]);
                        usize written = 0U;
                        if (wire::delta_encode(sb::kSnapshotLayout, baseline, current, sb::kMinor,
                                               crd::containers::Span<u8>(slots.data() + (t * kSlotBytes), kSlotBytes),
                                               written) != wire::WireError::Ok)
                        {
                            failed = true;
                        }
                        sizes[t] = written;
                        total += written;
                    }
                    return total;
                });
            const double apply_ns = timed_ns(
                count,
                [&]
                {
                    u64 good = 0U;
                    for (usize t = lag; t < ticks; ++t)
                    {
                        const crd::containers::ConstSpan<u8> baseline(whole.data() + ((t - lag) * kSlotBytes),
                                                                      whole_bytes[t - lag]);
                        usize written = 0U;
                        if (wire::delta_apply(sb::kSnapshotLayout, baseline,
                                              crd::containers::ConstSpan<u8>(slots.data() + (t * kSlotBytes), sizes[t]),
                                              sb::kMinor, crd::containers::Span<u8>(scratch, kSlotBytes),
                                              written) == wire::WireError::Ok &&
                            written == whole_bytes[t] &&
                            std::memcmp(scratch, whole.data() + (t * kSlotBytes), written) == 0)
                        {
                            ++good;
                        }
                    }
                    if (good != count)
                    {
                        failed = true;
                    }
                    return good;
                });
            allocations = g_allocations - before;
            best_encode = encode_ns < best_encode ? encode_ns : best_encode;
            best_apply = apply_ns < best_apply ? apply_ns : best_apply;
            for (usize t = lag; t < ticks; ++t)
            {
                delta_total += sizes[t];
                whole_total += whole_bytes[t];
            }
        }
        if (allocations != 0U)
        {
            std::fprintf(stderr, "crd-net's deltas allocated\n");
            failed = true;
        }
        const double count = static_cast<double>(ticks - lag);
        std::printf("| %zu tick%s | %.1f | %.1f | %.3f | %.1f | %.1f | %llu |\n", lag, lag == 1U ? "" : "s",
                    static_cast<double>(whole_total) / count, static_cast<double>(delta_total) / count,
                    static_cast<double>(delta_total) / static_cast<double>(whole_total), best_encode, best_apply,
                    static_cast<unsigned long long>(allocations));
    }
    if (failed)
    {
        std::fprintf(stderr, "FAILED\n");
        return 1;
    }
    std::printf(
        "\nAll arms agree with the source; crd-net and serialize decode identically; crd-net allocated nothing.\n");
    return 0;
}
