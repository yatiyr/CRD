#pragma once

// What the coded-body tests share: the models tests/foundation/net/schemas/coding_test.json pins, trained from fixed
// sources, and a simulated world whose deltas are what a snapshot channel really carries.

#include <crd/compression/entropy.hpp>
#include <crd/containers/array.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>
#include <crd/net/coding.hpp>
#include <crd/net/delta.hpp>

#include <cstring>
#include <generated/coding_test.hpp>
#include <random>

namespace crd::net::test
{
namespace coding = wire::coding_test;

inline constexpr u32 kCodedEvent = 0U;
inline constexpr u32 kCodedState = 1U;
inline constexpr u32 kCoderMessageBytes = 60000U;

using Bytes = crd::containers::Array<u8>;

[[nodiscard]] inline Bytes bytes_of(cont::ConstSpan<u8> span)
{
    Bytes out;
    out.resize(span.size());
    if (!span.empty())
    {
        std::memcpy(out.data(), span.data(), span.size());
    }
    return out;
}

// N bodies, most at rest, some moving, their state advanced a tick at a time and encoded.
class SimulatedWorld
{
public:
    SimulatedWorld(u32 count, u32 seed) : m_rng(seed)
    {
        m_bodies.resize(count);
        m_moving.resize(count);
        for (u32 index = 0U; index < count; ++index)
        {
            coding::Body& body = m_bodies[index];
            body.id = static_cast<u16>(index * 2U);
            body.x = m_rng() & 0x3FFFFU;
            body.y = m_rng() & 0x3FFFFU;
            body.z = m_rng() & 0x3FFFFU;
            body.spin = m_rng() & 0x1FFFFFFFU;
            m_moving[index] = (m_rng() % 100U) < 15U;
        }
    }

    [[nodiscard]] Bytes tick()
    {
        ++m_tick;
        for (usize index = 0U; index < m_bodies.size(); ++index)
        {
            if (m_moving[index] == 0U)
            {
                continue;
            }
            coding::Body& body = m_bodies[index];
            body.x = (body.x + (m_rng() % 16U)) & 0x3FFFFU;
            body.z = (body.z + (m_rng() % 8U)) & 0x3FFFFU;
            // A rotation's code moves a little each tick too.
            if ((m_rng() % 4U) != 0U)
            {
                body.spin = (body.spin + (m_rng() % 64U)) & 0x1FFFFFFFU;
            }
        }
        coding::World world;
        world.tick = m_tick;
        world.bodies =
            wire::WireList<coding::BodyCodec>(cont::ConstSpan<coding::Body>(m_bodies.data(), m_bodies.size()));
        Bytes out;
        out.resize(wire::encoded_bytes(world, coding::kMinor));
        usize written = 0U;
        const wire::WireError error =
            wire::encode(world, coding::kMinor, cont::Span<u8>(out.data(), out.size()), written);
        static_cast<void>(error);
        return out;
    }

private:
    std::mt19937 m_rng;
    u32 m_tick = 0U;
    crd::containers::Array<coding::Body> m_bodies;
    crd::containers::Array<u8> m_moving;
};

// The dictionary of the event channel's first revision: what its notes tend to say.
[[nodiscard]] inline Bytes note_dictionary()
{
    static constexpr char kCommon[] =
        "the player joined the room and left the match; the server restarted the round. "
        "score updated for team blue and team red; connection lost; connection restored. ";
    return bytes_of(cont::ConstSpan<u8>(reinterpret_cast<const u8*>(kCommon), sizeof(kCommon) - 1U));
}

// A text a note of that dictionary might carry.
[[nodiscard]] inline std::string_view common_note()
{
    return "the player joined the room; score updated for team blue; the server restarted the round; "
           "connection lost; connection restored; the player left the match; score updated for team red";
}

// The entropy model of the event channel's next revision, from English-like bytes.
[[nodiscard]] inline Bytes note_entropy_source()
{
    crd::compression::EntropyTrainer trainer;
    const std::string_view text = common_note();
    for (u32 round = 0U; round < 16U; ++round)
    {
        trainer.add(cont::ConstSpan<u8>(reinterpret_cast<const u8*>(text.data()), text.size()));
    }
    Bytes source;
    source.resize(crd::compression::kEntropyModelBytes);
    trainer.write_model(cont::Span<u8>(source.data(), source.size()));
    return source;
}

// The state channel's entropy model, from the deltas of a simulated world.
[[nodiscard]] inline Bytes state_entropy_source()
{
    crd::compression::EntropyTrainer trainer;
    SimulatedWorld world(200U, 1234U);
    Bytes previous = world.tick();
    Bytes room;
    room.resize(64000U);
    for (u32 round = 0U; round < 200U; ++round)
    {
        Bytes current = world.tick();
        usize written = 0U;
        if (wire::delta_encode(coding::kWorldLayout, cont::ConstSpan<u8>(previous.data(), previous.size()),
                               cont::ConstSpan<u8>(current.data(), current.size()), coding::kMinor,
                               cont::Span<u8>(room.data(), room.size()), written) == wire::WireError::Ok)
        {
            trainer.add(cont::ConstSpan<u8>(room.data(), written));
        }
        previous = current;
    }
    Bytes source;
    source.resize(crd::compression::kEntropyModelBytes);
    trainer.write_model(cont::Span<u8>(source.data(), source.size()));
    return source;
}

// One loop's models and coder, shared by a test binary: they take far more memory and time than a frame.
struct CodedModels
{
    CodedModels()
        : heap(96U * 1024U * 1024U), dictionary_source(note_dictionary()), note_source(note_entropy_source()),
          state_source(state_entropy_source()),
          dictionary(heap, wire::CodecKind::Dictionary,
                     cont::ConstSpan<u8>(dictionary_source.data(), dictionary_source.size()), kCoderMessageBytes),
          note(heap, wire::CodecKind::Entropy, cont::ConstSpan<u8>(note_source.data(), note_source.size()),
               kCoderMessageBytes),
          state(heap, wire::CodecKind::Entropy, cont::ConstSpan<u8>(state_source.data(), state_source.size()),
                kCoderMessageBytes),
          coder(heap, kCoderMessageBytes)
    {
        list[0] = &dictionary;
        list[1] = &note;
        list[2] = &state;
    }

    memory::TlsfAllocator heap;
    Bytes dictionary_source;
    Bytes note_source;
    Bytes state_source;
    BodyModel dictionary;
    BodyModel note;
    BodyModel state;
    BodyCoder coder;
    const BodyModel* list[3] = {};
};

[[nodiscard]] inline CodedModels& coded_models()
{
    static CodedModels models;
    return models;
}

} // namespace crd::net::test
