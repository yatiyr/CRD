// libwebrtc's FEC codec and RED splitter as HESAP-NET's oracle and board read them (crd-webrtc-fec-peer, the pinned
// branch's files with only their plumbing shimmed). ForwardErrorCorrection::CreateFlexfec still builds the draft-03
// reader and writer, so codec() builds it on the RFC 8627 pair through its protected constructor, and its generated
// packets and masks are read by compiling its headers with `private` open. The RFC 8627 reader takes its streams from
// the repair packet's CSRC list, which this branch's codec never parses, so CsrcReader names them as the RTP layer
// would.

#pragma once

#include <crd/core/types.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <limits>
#include <list>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#define private public   // NOLINT(readability-identifier-naming)
#define protected public // NOLINT(readability-identifier-naming)
#include "modules/audio_coding/neteq/red_payload_splitter.h"
#include "modules/rtp_rtcp/include/rtp_rtcp_defines.h"
#include "modules/rtp_rtcp/source/flexfec_header_reader_writer.h"
#include "modules/rtp_rtcp/source/forward_error_correction.h"
#undef protected
#undef private

namespace fec_peer
{

using Fec = webrtc::ForwardErrorCorrection;

class CsrcReader final : public webrtc::FlexfecHeaderReader
{
public:
    explicit CsrcReader(crd::u32 media_ssrc) : m_media_ssrc(media_ssrc) {}

    bool ReadFecHeader(Fec::ReceivedFecPacket* fec_packet) const override
    {
        fec_packet->protected_streams.assign(1U, Fec::ProtectedStream{m_media_ssrc});
        return webrtc::FlexfecHeaderReader::ReadFecHeader(fec_packet);
    }

private:
    crd::u32 m_media_ssrc;
};

[[nodiscard]] inline std::unique_ptr<Fec> codec(crd::u32 fec_ssrc, crd::u32 media_ssrc)
{
    return std::unique_ptr<Fec>(new Fec(std::make_unique<CsrcReader>(media_ssrc),
                                        std::make_unique<webrtc::FlexfecHeaderWriter>(), fec_ssrc, media_ssrc));
}

[[nodiscard]] inline webrtc::scoped_refptr<Fec::Packet> shared_packet(std::span<const crd::u8> bytes)
{
    webrtc::scoped_refptr<Fec::Packet> packet(new Fec::Packet());
    packet->data.SetData(bytes.data(), bytes.size());
    return packet;
}

[[nodiscard]] inline std::unique_ptr<Fec::Packet> owned_packet(std::span<const crd::u8> bytes)
{
    auto packet = std::make_unique<Fec::Packet>();
    packet->data.SetData(bytes.data(), bytes.size());
    return packet;
}

// A received packet as FlexfecReceiver hands it to DecodeFec().
inline void decode(Fec& fec, Fec::RecoveredPacketList& recovered, crd::u32 ssrc, crd::u16 sequence, bool is_fec,
                   std::span<const crd::u8> bytes)
{
    Fec::ReceivedPacket received;
    received.ssrc = ssrc;
    received.seq_num = sequence;
    received.is_fec = is_fec;
    received.is_recovered = false;
    received.pkt = shared_packet(bytes);
    fec.DecodeFec(received, &recovered);
}

} // namespace fec_peer
