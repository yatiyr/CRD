// The plumbing crd-webrtc-fec-peer shims: NetEq's Packet holds a parsed frame, which a split RED block never has.
#pragma once

namespace webrtc
{
class AudioDecoder
{
public:
    class EncodedAudioFrame
    {
    public:
        virtual ~EncodedAudioFrame() = default;
    };
};
} // namespace webrtc
