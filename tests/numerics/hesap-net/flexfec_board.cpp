// crd-hesap-flexfec-board: nanoseconds and heap of hesap-net's FlexFEC and RED against libwebrtc's own files
// (crd-webrtc-fec-peer), on 24 RTP packets of 1200 bytes and libwebrtc's packet masks. Each arm's output is compared
// with the other side's, so a faster wrong answer fails.
//   read header   one RFC 8627 header of a mix of 15, 46 and 110 mask bits, copied in first on both sides, since
//                 libwebrtc's reader rewrites it in place
//   six repairs   the six repair packets of a 25% rate: hesap the masks libwebrtc's table gave, libwebrtc EncodeFec()
//   one repair    one repair packet over all 24 packets
//   recover       three packets lost: a window of 24 and six repairs against DecodeFec() of each packet received
//   RED split     a payload with two redundant blocks of 160 bytes and a primary of 160
//
// Usage: crd-hesap-flexfec-board [--rounds N] [--quick]
//   rounds  interleaved rounds of every arm; the best of them is reported (default 25)
//   quick   one round with few repeats (the smoke CTest)

#include "fec_packets.hpp"
#include "fec_peer.hpp"
#include "seeded_words.hpp"

#include <crd/containers/array.hpp>
#include <crd/hesap/net/flexfec.hpp>
#include <crd/hesap/net/red.hpp>

#include <bit>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <new>

namespace
{
crd::u64 g_allocations = 0U;
crd::u64 g_allocated_bytes = 0U;
} // namespace

// Counts what each arm allocates.
void* operator new(std::size_t size)
{
    ++g_allocations;
    g_allocated_bytes += size;
    void* memory = std::malloc(size != 0U ? size : 1U);
    if (memory == nullptr)
    {
        std::abort();
    }
    return memory;
}

void operator delete(void* memory) noexcept
{
    std::free(memory);
}

void operator delete(void* memory, std::size_t) noexcept
{
    std::free(memory);
}

namespace hn = crd::hesap::net;
using crd::u16;
using crd::u32;
using crd::u64;
using crd::u8;
using crd::usize;
using fec_peer::Fec;
using hn::FlexFecBlock;
using hn::FlexFecHeader;
using hn::FlexFecStatus;
using hn::FlexFecVariant;
using Bytes = crd::containers::Array<u8>;
using Span = std::span<const u8>;

namespace
{

constexpr u32 kMediaSsrc = 0x11223344U;
constexpr u32 kFecSsrc = 0x55667788U;
constexpr u32 kMedia = 24U;
constexpr u16 kBase = 1000U;
constexpr usize kPayload = 1188U;
constexpr u8 kProtection = 64U; // six repair packets of 24
constexpr usize kHeaderBytes = 32U;
constexpr u32 kHeaders = 3000U;

struct Run
{
    double seconds = 0.0;
    u64 allocations = 0U;
    u64 allocated_bytes = 0U;
    u64 answer = 0U;
};

template <class Body>
[[nodiscard]] Run timed(const Body& body)
{
    const u64 allocations = g_allocations;
    const u64 allocated_bytes = g_allocated_bytes;
    const auto begin = std::chrono::steady_clock::now();
    const u64 answer = body();
    const auto end = std::chrono::steady_clock::now();
    return {std::chrono::duration<double>(end - begin).count(), g_allocations - allocations,
            g_allocated_bytes - allocated_bytes, answer};
}

[[nodiscard]] u64 fnv(Span bytes, u64 hash = 14695981039346656037ULL)
{
    for (const u8 b : bytes)
    {
        hash = (hash ^ b) * 1099511628211ULL;
    }
    return hash;
}

[[noreturn]] void refuse(const char* what)
{
    std::fprintf(stderr, "crd-hesap-flexfec-board: %s\n", what);
    std::exit(1);
}

[[nodiscard]] u64 repair_hash(Span repair, u64 hash)
{
    FlexFecHeader header;
    if (hn::flexfec_parse(repair, 1U, header) != FlexFecStatus::Ok)
    {
        refuse("a repair packet does not parse");
    }
    return fnv(repair.first(8U), fnv(repair.subspan(header.header_bytes), hash));
}

struct Fixture
{
    crd::containers::Array<Bytes> media;
    crd::containers::Array<Span> views;
    Fec::PacketList list;
    std::unique_ptr<Fec> fec = fec_peer::codec(kFecSsrc, kMediaSsrc);
    crd::containers::Array<FlexFecBlock> blocks;
    crd::containers::Array<Bytes> repairs;
    crd::containers::Array<Bytes> outputs;
    Bytes headers;
    Bytes red;
    u32 lost[3] = {};
};

// Three lost packets that libwebrtc's six masks repair, as far apart as the masks allow.
[[nodiscard]] bool pick_lost(Fixture& f)
{
    crd::containers::Array<Bytes> window;
    crd::containers::Array<hn::FlexFecSlot> slots;
    window.resize(kMedia);
    slots.resize(kMedia);
    const u32 ssrcs[1] = {kMediaSsrc};
    crd::containers::Array<hn::FlexFecRepair> views;
    for (const Bytes& repair : f.repairs)
    {
        views.push_back({Span(repair.data(), repair.size()), ssrcs});
    }
    for (u32 gap = kMedia / 3U; gap > 0U; --gap)
    {
        for (u32 first = 0U; first + (2U * gap) < kMedia; ++first)
        {
            for (u32 i = 0U; i < kMedia; ++i)
            {
                window[i].resize(1500U);
                slots[i].data = window[i].data();
                slots[i].capacity = 1500U;
                std::memcpy(window[i].data(), f.media[i].data(), f.media[i].size());
                slots[i].length = f.media[i].size();
                slots[i].present = i != first && i != first + gap && i != first + (2U * gap);
            }
            const hn::FlexFecStream stream{kMediaSsrc, kBase, std::span<hn::FlexFecSlot>(slots.data(), kMedia)};
            if (hn::flexfec_recover_window(std::span<const hn::FlexFecStream>(&stream, 1U),
                                           std::span<const hn::FlexFecRepair>(views.data(), views.size())) == 3U)
            {
                f.lost[0] = first;
                f.lost[1] = first + gap;
                f.lost[2] = first + (2U * gap);
                return true;
            }
        }
    }
    return false;
}

[[nodiscard]] Fixture fixture()
{
    Fixture f;
    SeededWords rng(8627U, 9U);
    for (u32 i = 0U; i < kMedia; ++i)
    {
        f.media.push_back(rtp_packet(rng, kMediaSsrc, static_cast<u16>(kBase + i), kPayload));
    }
    for (const Bytes& packet : f.media)
    {
        f.views.push_back(Span(packet.data(), packet.size()));
        f.list.push_back(fec_peer::owned_packet(f.views.back()));
    }
    std::list<Fec::Packet*> generated; // crd-lint-allow-std-container: its interface
    if (f.fec->EncodeFec(f.list, kProtection, 0, false, webrtc::kFecMaskRandom, &generated) != 0)
    {
        refuse("libwebrtc refuses the window");
    }
    for (const Fec::Packet* repair : generated)
    {
        Bytes copy;
        copy.resize(repair->data.size());
        std::memcpy(copy.data(), repair->data.cdata(), copy.size());
        FlexFecHeader header;
        if (hn::flexfec_parse(Span(copy.data(), copy.size()), 1U, header) != FlexFecStatus::Ok)
        {
            refuse("libwebrtc's repair packet does not parse");
        }
        f.blocks.push_back(header.blocks[0]);
        f.repairs.push_back(std::move(copy));
        Bytes output;
        output.resize(1500U + FlexFecHeader::kMaxBytes);
        f.outputs.push_back(std::move(output));
    }

    f.headers.resize(kHeaders * kHeaderBytes);
    for (u8& b : f.headers)
    {
        b = static_cast<u8>(rng.next_u32());
    }
    for (u32 i = 0U; i < kHeaders; ++i)
    {
        u8* header = &f.headers[i * kHeaderBytes];
        const u32 width = i % 3U;
        header[0] &= 0x3FU;
        header[10] = static_cast<u8>(width == 0U ? (header[10] & 0x7FU) : (header[10] | 0x80U));
        header[12] = static_cast<u8>(width == 1U ? (header[12] & 0x7FU) : (header[12] | 0x80U));
        header[11] |= 1U; // never an empty mask
    }

    hn::RedBlock redundant[2];
    u8 frames[3][160];
    for (auto& frame : frames)
    {
        for (u8& b : frame)
        {
            b = static_cast<u8>(rng.next_u32());
        }
    }
    redundant[0] = {111U, 1920U, 0U, Span(frames[0], 160U)};
    redundant[1] = {111U, 960U, 0U, Span(frames[1], 160U)};
    f.red.resize(1024U);
    usize written = 0U;
    if (hn::red_encode(redundant, 111U, Span(frames[2], 160U), std::span<u8>(f.red.data(), f.red.size()), written) !=
        hn::RedStatus::Ok)
    {
        refuse("red_encode refuses");
    }
    f.red.resize(written);
    if (!pick_lost(f))
    {
        refuse("no three lost packets repair");
    }
    return f;
}

[[nodiscard]] FlexFecStatus encode_mask(const Fixture& f, const FlexFecBlock& block, std::span<u8> out, usize& written)
{
    Span sources[kMedia];
    usize count = 0U;
    for (u32 i = 0U; i < kMedia; ++i)
    {
        if (((block.mask[0] >> i) & 1U) != 0U)
        {
            sources[count++] = f.views[i];
        }
    }
    const u32 ssrcs[1] = {kMediaSsrc};
    return hn::flexfec_encode(std::span<const Span>(sources, count), ssrcs, FlexFecVariant::Mask,
                              std::span<const FlexFecBlock>(&block, 1U), out, written);
}

struct Row
{
    const char* name;
    u32 operations;
};

void report(const Row& row, double ours_seconds, const Run& ours, double theirs_seconds, const Run& theirs)
{
    const double per = 1e9 / static_cast<double>(row.operations);
    const double blocks = static_cast<double>(theirs.allocations) / static_cast<double>(row.operations);
    const double bytes = static_cast<double>(theirs.allocated_bytes) / static_cast<double>(row.operations);
    std::printf("| %s | %.1f | %.1f | %.2f | %.0f blocks, %.0f bytes | %.1f blocks, %.0f bytes |\n", row.name,
                ours_seconds * per, theirs_seconds * per, theirs_seconds / ours_seconds,
                static_cast<double>(ours.allocations) / static_cast<double>(row.operations),
                static_cast<double>(ours.allocated_bytes) / static_cast<double>(row.operations), blocks, bytes);
}

template <class Ours, class Theirs>
void arm(const Row& row, u32 rounds, const Ours& ours, const Theirs& theirs)
{
    double best_ours = 1e300;
    double best_theirs = 1e300;
    Run last_ours;
    Run last_theirs;
    for (u32 round = 0U; round < rounds; ++round)
    {
        last_ours = timed(ours);
        last_theirs = timed(theirs);
        best_ours = std::min(best_ours, last_ours.seconds);
        best_theirs = std::min(best_theirs, last_theirs.seconds);
        if (last_ours.answer != last_theirs.answer)
        {
            refuse(row.name);
        }
    }
    report(row, best_ours, last_ours, best_theirs, last_theirs);
}

} // namespace

int main(int argc, char** argv)
{
    u32 rounds = 25U;
    u32 repeat = 200U;
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--quick") == 0)
        {
            rounds = 1U;
            repeat = 2U;
        }
        else if (std::strcmp(argv[i], "--rounds") == 0 && i + 1 < argc)
        {
            rounds = static_cast<u32>(std::atoi(argv[++i]));
        }
    }

    Fixture f = fixture();
    std::printf("| Operation | hesap ns | libwebrtc ns | libwebrtc / hesap | hesap heap per operation | "
                "libwebrtc heap per operation |\n|---|---|---|---|---|---|\n");

    {
        Fec::ReceivedFecPacket packet;
        packet.pkt = fec_peer::shared_packet(Span(f.headers.data(), kHeaderBytes));
        packet.protected_streams.resize(1U);
        const webrtc::FlexfecHeaderReader reader;
        arm(
            {"read header", kHeaders * repeat}, rounds,
            [&]
            {
                u64 sum = 0U;
                u8 scratch[kHeaderBytes];
                FlexFecHeader header;
                for (u32 r = 0U; r < repeat; ++r)
                {
                    for (u32 i = 0U; i < kHeaders; ++i)
                    {
                        std::memcpy(scratch, &f.headers[i * kHeaderBytes], kHeaderBytes);
                        if (hn::flexfec_parse(Span(scratch, kHeaderBytes), 1U, header) != FlexFecStatus::Ok)
                        {
                            refuse("hesap refuses a header");
                        }
                        sum += header.blocks[0].sn_base + header.header_bytes +
                               static_cast<u64>(std::popcount(header.blocks[0].mask[0])) +
                               static_cast<u64>(std::popcount(header.blocks[0].mask[1]));
                    }
                }
                return sum;
            },
            [&]
            {
                u64 sum = 0U;
                u8* data = packet.pkt->data.MutableData();
                for (u32 r = 0U; r < repeat; ++r)
                {
                    for (u32 i = 0U; i < kHeaders; ++i)
                    {
                        std::memcpy(data, &f.headers[i * kHeaderBytes], kHeaderBytes);
                        if (!reader.webrtc::FlexfecHeaderReader::ReadFecHeader(&packet))
                        {
                            refuse("libwebrtc refuses a header");
                        }
                        // The packed mask is what its reader leaves; what follows it is the payload's.
                        const auto& stream = packet.protected_streams[0];
                        u64 words[2] = {};
                        std::memcpy(words, data + stream.packet_mask_offset, stream.packet_mask_size);
                        sum += stream.seq_num_base + packet.fec_header_size +
                               static_cast<u64>(std::popcount(words[0])) + static_cast<u64>(std::popcount(words[1]));
                    }
                }
                return sum;
            });
    }

    const u32 sets = repeat / 2U + 1U;
    arm(
        {"six repairs", sets}, rounds,
        [&]
        {
            for (u32 r = 0U; r < sets; ++r)
            {
                for (usize m = 0U; m < f.blocks.size(); ++m)
                {
                    usize written = 0U;
                    if (encode_mask(f, f.blocks[m], std::span<u8>(f.outputs[m].data(), f.outputs[m].size()), written) !=
                        FlexFecStatus::Ok)
                    {
                        refuse("hesap refuses a mask");
                    }
                }
            }
            return u64{0U};
        },
        [&]
        {
            for (u32 r = 0U; r < sets; ++r)
            {
                std::list<Fec::Packet*> generated; // crd-lint-allow-std-container: its interface
                if (f.fec->EncodeFec(f.list, kProtection, 0, false, webrtc::kFecMaskRandom, &generated) != 0)
                {
                    refuse("libwebrtc refuses the window");
                }
            }
            return u64{0U};
        });
    for (usize m = 0U; m < f.blocks.size(); ++m)
    {
        usize written = 0U;
        if (encode_mask(f, f.blocks[m], std::span<u8>(f.outputs[m].data(), f.outputs[m].size()), written) !=
                FlexFecStatus::Ok ||
            repair_hash(Span(f.outputs[m].data(), written), 0U) !=
                repair_hash(Span(f.fec->generated_fec_packets_[m].data.cdata(),
                                 f.fec->generated_fec_packets_[m].data.size()),
                            0U))
        {
            refuse("six repairs");
        }
    }

    FlexFecBlock all;
    all.sn_base = kBase;
    all.mask[0] = (u64{1U} << kMedia) - 1U;
    const u8 all_masks[6] = {0xFFU, 0xFFU, 0xFFU, 0U, 0U, 0U};
    arm(
        {"one repair over 24", sets}, rounds,
        [&]
        {
            usize written = 0U;
            for (u32 r = 0U; r < sets; ++r)
            {
                if (encode_mask(f, all, std::span<u8>(f.outputs[0].data(), f.outputs[0].size()), written) !=
                    FlexFecStatus::Ok)
                {
                    refuse("hesap refuses the mask");
                }
            }
            return repair_hash(Span(f.outputs[0].data(), written), 0U);
        },
        [&]
        {
            Fec::Packet& generated = f.fec->generated_fec_packets_[0];
            for (u32 r = 0U; r < sets; ++r)
            {
                generated.data.SetSize(0);
                std::memcpy(f.fec->packet_masks_.data(), all_masks, 6U);
                f.fec->packet_mask_size_ = 6U;
                f.fec->GenerateFecPayloads(f.list, 1U);
                f.fec->FinalizeFecHeaders(1U, kMediaSsrc, kBase);
            }
            return repair_hash(Span(generated.data.cdata(), generated.data.size()), 0U);
        });

    {
        crd::containers::Array<Bytes> window;
        crd::containers::Array<hn::FlexFecSlot> slots;
        window.resize(kMedia);
        slots.resize(kMedia);
        for (u32 i = 0U; i < kMedia; ++i)
        {
            window[i].resize(1500U);
            slots[i].data = window[i].data();
            slots[i].capacity = 1500U;
            std::memcpy(window[i].data(), f.media[i].data(), f.media[i].size());
            slots[i].length = f.media[i].size();
            slots[i].present = true;
        }
        const u32 ssrcs[1] = {kMediaSsrc};
        const hn::FlexFecStream stream{kMediaSsrc, kBase, std::span<hn::FlexFecSlot>(slots.data(), kMedia)};
        crd::containers::Array<hn::FlexFecRepair> views;
        for (const Bytes& repair : f.repairs)
        {
            views.push_back({Span(repair.data(), repair.size()), ssrcs});
        }
        const std::unique_ptr<Fec> decoder = fec_peer::codec(kFecSsrc, kMediaSsrc);
        Fec::RecoveredPacketList recovered;
        arm(
            {"recover three", sets}, rounds,
            [&]
            {
                u32 total = 0U;
                for (u32 r = 0U; r < sets; ++r)
                {
                    for (const u32 lost : f.lost)
                    {
                        slots[lost].present = false;
                    }
                    total += hn::flexfec_recover_window(std::span<const hn::FlexFecStream>(&stream, 1U),
                                                        std::span<const hn::FlexFecRepair>(views.data(), views.size()));
                }
                if (total != sets * std::size(f.lost))
                {
                    refuse("hesap does not recover the three");
                }
                u64 hash = 0U;
                for (const u32 lost : f.lost)
                {
                    hash = fnv(Span(slots[lost].data, slots[lost].length), hash);
                }
                return hash;
            },
            [&]
            {
                u32 total = 0U;
                for (u32 r = 0U; r < sets; ++r)
                {
                    decoder->ResetState(&recovered);
                    for (u32 i = 0U; i < kMedia; ++i)
                    {
                        if (i != f.lost[0] && i != f.lost[1] && i != f.lost[2])
                        {
                            fec_peer::decode(*decoder, recovered, kMediaSsrc, static_cast<u16>(kBase + i), false,
                                             f.views[i]);
                        }
                    }
                    for (usize m = 0U; m < f.repairs.size(); ++m)
                    {
                        fec_peer::decode(*decoder, recovered, kFecSsrc, static_cast<u16>(m), true,
                                         Span(f.repairs[m].data(), f.repairs[m].size()));
                    }
                    for (const auto& packet : recovered)
                    {
                        total += packet->was_recovered ? 1U : 0U;
                    }
                }
                if (total != sets * std::size(f.lost))
                {
                    refuse("libwebrtc does not recover the three");
                }
                u64 hash = 0U;
                for (const u32 lost : f.lost)
                {
                    for (const auto& packet : recovered)
                    {
                        if (packet->seq_num == static_cast<u16>(kBase + lost))
                        {
                            hash = fnv(Span(packet->pkt->data.cdata(), packet->pkt->data.size()), hash);
                        }
                    }
                }
                return hash;
            });
    }

    const u32 splits = repeat * 500U;
    webrtc::RedPayloadSplitter splitter;
    arm(
        {"RED split", splits}, rounds,
        [&]
        {
            u64 sum = 0U;
            hn::RedBlock blocks[3];
            for (u32 r = 0U; r < splits; ++r)
            {
                u32 count = 0U;
                if (hn::red_parse(Span(f.red.data(), f.red.size()), 100000U + r, blocks, count) != hn::RedStatus::Ok)
                {
                    refuse("hesap refuses the payload");
                }
                sum += blocks[0].timestamp + blocks[count - 1U].data.size();
            }
            return sum;
        },
        [&]
        {
            u64 sum = 0U;
            for (u32 r = 0U; r < splits; ++r)
            {
                webrtc::PacketList list;
                webrtc::Packet whole;
                whole.timestamp = 100000U + r;
                whole.payload.SetData(f.red.data(), f.red.size());
                list.push_back(std::move(whole));
                if (!splitter.SplitRed(&list))
                {
                    refuse("NetEq refuses the payload");
                }
                sum += list.back().timestamp + list.front().payload.size();
            }
            return sum;
        });
    return 0;
}
