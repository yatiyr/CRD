// crd-fuzz-net-delta-stream: a DeltaReader fed any frame bodies, and a DeltaWriter's frames on a path the input
// controls.
//
// Input. [mode][writer minor][...]. Mode 0 cuts the rest into bodies, each [size, 16 bits][body]. Otherwise the rest is
// a script of one byte per step: which of the writer's frames to deliver now, repeat, lose or damage, and whether its
// acknowledgement arrives.
//
// Oracles:
//   * whatever is Delivered passes the generated decoder at the writer's revision;
//   * on the path, whatever is Delivered is byte for byte what the writer wrote at that sequence, unless a body was
//   damaged
//     past what its hash can see (the hash is 32 bits, and the script can hit it only by chance, which a replay would
//     name);
//   * the acknowledgement only rises;
//   * no read allocates.
#include <crd/fuzz/harness.hpp>
#include <crd/memory/allocator.hpp>
#include <crd/net/delta.hpp>

#include <cstdlib>
#include <cstring>
#include <generated/delta_test.hpp>

namespace
{
using crd::u16;
using crd::u32;
using crd::u64;
using crd::u8;
using crd::usize;
using crd::containers::ConstSpan;
using crd::containers::Span;
using crd::net::BaselineStore;
using crd::net::DeltaRead;
using crd::net::DeltaReader;
using crd::net::DeltaWriter;
using crd::net::wire::WireError;

namespace schema = crd::net::wire::delta_test;

class Heap final : public crd::memory::IAllocator
{
public:
    void* allocate(usize size, usize) override
    {
        ++calls;
        return std::malloc(size == 0U ? 1U : size);
    }

    void deallocate(void* pointer) noexcept override { std::free(pointer); }

    [[nodiscard]] bool owns(const void*) const noexcept override { return true; }

    usize calls = 0U;
};

constexpr usize kFrameBytes = 4096U;
constexpr u32 kSteps = 64U;

usize encode_world(u32 step, u8* out, usize room)
{
    schema::Entity entities[3] = {};
    static constexpr u8 kFixed[3] = {1U, 2U, 3U};
    for (u32 index = 0U; index < 3U; ++index)
    {
        entities[index].id = static_cast<u16>(index * 5U);
        entities[index].fixed = ConstSpan<u8>(kFixed);
        entities[index].pose.yaw = static_cast<u16>((step * (index + 1U)) % 1024U);
        entities[index].seen = ((step >> index) & 1U) != 0U;
    }
    schema::Snapshot snapshot{};
    snapshot.tick = step;
    snapshot.entities = crd::net::wire::WireList<schema::EntityCodec>(ConstSpan<schema::Entity>(entities));
    usize size = 0U;
    CRD_FUZZ_REQUIRE(crd::net::wire::encode(snapshot, schema::kMinor, Span<u8>(out, room), size) == WireError::Ok);
    return size;
}

void read_bodies(ConstSpan<u8> bytes, u16 writer_minor)
{
    Heap heap;
    BaselineStore store(heap, 8U, 8192U);
    DeltaReader reader(schema::kSnapshotLayout, store, heap);
    CRD_FUZZ_REQUIRE(reader.ready());
    const usize calls = heap.calls;
    usize at = 0U;
    u64 reads = 0U;
    u16 last_ack = 0U;
    bool had_ack = false;
    while (bytes.size() - at >= 2U)
    {
        const usize size = (static_cast<usize>(bytes[at]) << 8U) | bytes[at + 1U];
        at += 2U;
        if (size > bytes.size() - at)
        {
            break;
        }
        ConstSpan<u8> message;
        WireError error = WireError::Ok;
        const DeltaRead verdict = reader.read(bytes.subspan(at, size), writer_minor, message, error);
        at += size;
        ++reads;
        if (verdict == DeltaRead::Delivered)
        {
            CRD_FUZZ_REQUIRE(schema::kSnapshotLayout.validate(message, writer_minor) == WireError::Ok);
            CRD_FUZZ_REQUIRE(reader.has_ack());
            CRD_FUZZ_REQUIRE(!had_ack || static_cast<u16>(reader.ack() - last_ack) != 0U);
            last_ack = reader.ack();
            had_ack = true;
        }
        else
        {
            CRD_FUZZ_REQUIRE((verdict == DeltaRead::Malformed) == (error != WireError::Ok));
        }
    }
    u64 counted = 0U;
    for (usize index = 0U; index < crd::net::kDeltaReadCount; ++index)
    {
        counted += reader.count(static_cast<DeltaRead>(index));
    }
    CRD_FUZZ_REQUIRE(counted == reads);
    CRD_FUZZ_REQUIRE(heap.calls == calls);
}

void walk_path(ConstSpan<u8> script)
{
    Heap heap;
    BaselineStore sent(heap, 16U, 32768U);
    BaselineStore kept(heap, 16U, 32768U);
    DeltaWriter writer(schema::kSnapshotLayout, sent);
    DeltaReader reader(schema::kSnapshotLayout, kept, heap);
    static u8 frames[kSteps][kFrameBytes];
    static usize sizes[kSteps];
    static u8 worlds[kSteps][kFrameBytes];
    static usize world_sizes[kSteps];
    u32 written = 0U;
    const usize calls = heap.calls;
    u64 newest = 0U;
    for (const u8 step : script)
    {
        if (written < kSteps && (step & 1U) != 0U)
        {
            const u32 number = written;
            world_sizes[number] = encode_world(number, worlds[number], kFrameBytes);
            CRD_FUZZ_REQUIRE(sent.put(number + 1U, ConstSpan<u8>(worlds[number], world_sizes[number])));
            CRD_FUZZ_REQUIRE(writer.write(number + 1U, schema::kMinor, Span<u8>(frames[number], kFrameBytes),
                                          sizes[number]) == WireError::Ok);
            ++written;
        }
        if (written == 0U)
        {
            continue;
        }
        const u32 chosen = (step >> 1U) % written;
        ConstSpan<u8> message;
        WireError error = WireError::Ok;
        const DeltaRead verdict =
            reader.read(ConstSpan<u8>(frames[chosen], sizes[chosen]), schema::kMinor, message, error);
        if (verdict == DeltaRead::Delivered)
        {
            const u64 sequence = (static_cast<u64>(frames[chosen][0]) << 8U) | frames[chosen][1];
            CRD_FUZZ_REQUIRE(sequence > newest);
            newest = sequence;
            CRD_FUZZ_REQUIRE(message.size() == world_sizes[chosen]);
            CRD_FUZZ_REQUIRE(std::memcmp(message.data(), worlds[chosen], message.size()) == 0);
        }
        else
        {
            CRD_FUZZ_REQUIRE(verdict == DeltaRead::Stale || verdict == DeltaRead::MissingBaseline);
        }
        if ((step & 0x80U) != 0U && reader.has_ack())
        {
            writer.on_ack(reader.ack());
        }
    }
    CRD_FUZZ_REQUIRE(heap.calls == calls);
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    if (size < 2U || size - 2U > crd::fuzz::kMaxInputBytes)
    {
        return 0;
    }
    const ConstSpan<u8> rest(data + 2U, size - 2U);
    if (data[0] == 0U)
    {
        read_bodies(rest, static_cast<u16>(data[1] % (schema::kMinor + 3U)));
    }
    else
    {
        walk_path(rest);
    }
    return 0;
}

void crd_fuzz_seeds(crd::fuzz::SeedSink& sink)
{
    static constexpr u8 kPath[] = {1U, 0U, 1U, 3U, 0x81U, 5U, 0x85U, 7U, 0x87U, 3U, 0x89U, 0x8BU, 0x8DU, 9U, 11U};
    sink.add("path", kPath, sizeof(kPath));
    static constexpr u8 kBodies[] = {0U, 2U, 0U, 3U, 0U, 1U, 0U, 0U, 8U, 0U, 2U, 0U, 1U, 2U, 3U, 4U, 0xFFU, 0xFFU};
    sink.add("bodies", kBodies, sizeof(kBodies));
}
